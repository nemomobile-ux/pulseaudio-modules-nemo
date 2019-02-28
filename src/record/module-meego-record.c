/*
 * Copyright (C) 2010 Nokia Corporation.
 *
 * Contact: Maemo MMF Audio <mmf-audio@projects.maemo.org>
 *          or Jyri Sarha <jyri.sarha@nokia.com>
 *
 * These PulseAudio Modules are free software; you can redistribute
 * it and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/xmalloc.h>
#include <pulse/proplist.h>

#include <pulsecore/source-output.h>
#include <pulsecore/source.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/namereg.h>
#include <pulsecore/log.h>
#include <pulsecore/mutex.h>
#include <pulsecore/atomic.h>
#include <pulsecore/thread.h>

#include "module-meego-record-symdef.h"

#include "proplist-meego.h"
#include "pa-optimized.h"
#include "memory.h"
#include "algorithm-hook.h"

#include "module-record-api.h"

PA_MODULE_AUTHOR("Juho Hamalainen");
PA_MODULE_DESCRIPTION("Nokia record module");
PA_MODULE_USAGE(
    "master_source=<source to connect to> "
    "source_name=<name of created source> "
    "stereo=<use 2 channels instead of mono, default true>"
    "rate=<sample rate, default 48000> "
    "samplelength=<sample length in ms, default 20> "
);
PA_MODULE_VERSION(PACKAGE_VERSION);

static const char* const valid_modargs[] = {
    "master_source",
    "source_name",
    "stereo",
    "rate",
    "samplelength",
    NULL,
};

#define DEFAULT_CHANNELS     (1)
#define DEFAULT_SAMPLELENGTH (20) //ms
#define DEFAULT_SAMPLERATE   (48000)

#define PROPLIST_SINK "sink.hw0"

struct userdata {
    pa_core *core;
    pa_module *module;

    pa_source *master_source;
    pa_source *source;
    pa_source_output *source_output;

    int maxblocksize;

    /** Algorithm variables */
    meego_algorithm_hook_api *algorithm;
    meego_algorithm_hook *hook_algorithm;
    pa_memblockq *memblockq;
};

/*************************
  SOURCE CALLBACKS
 *************************/

/* Called from I/O thread context */
static int source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SOURCE(o)->userdata;

    switch (code) {

        case PA_SOURCE_MESSAGE_GET_LATENCY: {
            pa_usec_t usec = 0;

            if (PA_MSGOBJECT(u->master_source)->process_msg(
                PA_MSGOBJECT(u->master_source), PA_SOURCE_MESSAGE_GET_LATENCY, &usec, 0, NULL) < 0)
                usec = 0;

            *((pa_usec_t*) data) = usec;
            return 0;
        }
        case PA_SOURCE_MESSAGE_ADD_OUTPUT: {
            pa_source_output *so = PA_SOURCE_OUTPUT(data);
            pa_assert(so != u->source_output);
            /* Pass through to pa_source_process_msg */
            break;
        }

    }

    return pa_source_process_msg(o, code, data, offset, chunk);
}


/* Called from I/O thread context */
static void source_update_requested_latency(pa_source *s) {
    struct userdata *u;

    pa_source_assert_ref(s);
    pa_assert_se(u = s->userdata);

    /* Just hand this one over to the master source */
    pa_source_output_set_requested_latency_within_thread(
            u->source_output,
            pa_source_get_requested_latency_within_thread(s));
}

/* Called from main context */
static int source_set_state(pa_source *s, pa_source_state_t state) {
    struct userdata *u;

    pa_source_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (PA_SOURCE_IS_LINKED(state) && u->source_output && PA_SOURCE_OUTPUT_IS_LINKED(pa_source_output_get_state(u->source_output))) {
        pa_source_output_cork(u->source_output, state == PA_SOURCE_SUSPENDED);
    }

    pa_log_debug("source_set_state() called with %d", state);
    return 0;
}

#if PULSEAUDIO_VERSION >= 12
static int source_set_state_in_main_thread(pa_source *s, pa_source_state_t state, pa_suspend_cause_t suspend_cause) {
    if (s->state == state)
        return 0;

    return source_set_state(s, state);
}
#endif

/*************************
  SOURCE OUTPUT CALLBACKS
 *************************/

/* Called from I/O thread context */
static void source_output_push_cb(pa_source_output *o, const pa_memchunk *new_chunk) {
    struct userdata *u;
    pa_memchunk chunk;
    meego_algorithm_hook_data data;
    pa_memchunk out_chunk;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);
    pa_assert(new_chunk);

    if (pa_memblockq_push(u->memblockq, new_chunk) < 0) {
        pa_log_error("Failed to push %zu byte chunk into memblockq (len %zu).",
                new_chunk->length, pa_memblockq_get_length(u->memblockq));
        return;
    }

    while (util_memblockq_to_chunk(u->core->mempool, u->memblockq, &chunk, u->maxblocksize)) {

        if (PA_SOURCE_IS_OPENED(u->source->thread_info.state)) {
            if (meego_algorithm_hook_enabled(u->hook_algorithm)) {

                data.channels = 2;
                pa_optimized_deinterleave_stereo_to_mono(&chunk, &data.channel[0], &data.channel[1]);

                meego_algorithm_hook_fire(u->hook_algorithm, &data);

                pa_optimized_interleave_stereo(&data.channel[0], &data.channel[1], &out_chunk);

                pa_memblock_unref(chunk.memblock);
                pa_memblock_unref(data.channel[0].memblock);
                pa_memblock_unref(data.channel[1].memblock);

                chunk = out_chunk;
            }

            pa_source_post(u->source, &chunk);
        }

        pa_memblock_unref(chunk.memblock);

    }
}

/* Called from I/O thread context */
static void source_output_process_rewind_cb(pa_source_output *o, size_t nbytes) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    if (!u->source || !PA_SOURCE_IS_LINKED(u->source->thread_info.state))
        return;

    pa_source_process_rewind(u->source, nbytes);
}

/* Called from I/O thread context */
static void source_output_update_max_rewind_cb(pa_source_output *o, size_t nbytes) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    if (!u->source || !PA_SOURCE_IS_LINKED(u->source->thread_info.state))
        return;

    pa_source_set_max_rewind_within_thread(u->source, nbytes);
}

/* Called from I/O thread context */
static void source_output_update_source_latency_range_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    if (!u->source || !PA_SOURCE_IS_LINKED(u->source->thread_info.state))
        return;

    pa_source_set_latency_range_within_thread(u->source, o->source->thread_info.min_latency,
                                              o->source->thread_info.max_latency);
}

/* Called from I/O thread context */
static void source_output_update_source_fixed_latency_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    if (!u->source || !PA_SOURCE_IS_LINKED(u->source->thread_info.state))
        return;

    pa_source_set_fixed_latency_within_thread(u->source, o->source->thread_info.fixed_latency);
}

static void source_outputs_may_move(pa_source *s, bool move) {
    pa_source_output *o;
    uint32_t idx;

    for (o = PA_SOURCE_OUTPUT(pa_idxset_first(s->outputs, &idx)); o; o = PA_SOURCE_OUTPUT(pa_idxset_next(s->outputs, &idx))) {
        if (move)
            o->flags &= ~PA_SOURCE_OUTPUT_DONT_MOVE;
        else
            o->flags |= PA_SOURCE_OUTPUT_DONT_MOVE;
    }
}

/* Called from I/O thread context */
static void source_output_detach_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    if (PA_SOURCE_IS_LINKED(u->source->thread_info.state))
        pa_source_detach_within_thread(u->source);
    else
        pa_log_debug("fixme: !PA_SOURCE_IS_LINKED ?");

    pa_source_set_rtpoll(u->source, NULL);
    source_outputs_may_move(u->source, false);
}

/* Called from I/O thread context */
static void source_output_attach_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    if (!u->source || !PA_SOURCE_IS_LINKED(u->source->thread_info.state))
        return;

    source_outputs_may_move(u->source, true);
    pa_source_set_rtpoll(u->source, o->source->thread_info.rtpoll);

    if (o->source->flags & PA_SOURCE_DYNAMIC_LATENCY)
        pa_source_set_latency_range_within_thread(u->source, o->source->thread_info.min_latency,
                                                  o->source->thread_info.max_latency);
    else
        pa_source_set_fixed_latency_within_thread(u->source, o->source->thread_info.fixed_latency);

    pa_source_set_max_rewind_within_thread(u->source, pa_source_output_get_max_rewind(o));
    /* The order is important here. This should be called last: */
    pa_source_attach_within_thread(u->source);
}

/* Called from main context */
static void source_output_moving_cb(pa_source_output *o, pa_source *dest){
    struct userdata *u;
    pa_proplist *p;
    pa_source_output *so;
    uint32_t idx;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    if (!dest) {
        pa_source_set_asyncmsgq(u->source, NULL);

        return;
    }

    u->master_source = dest;
    pa_source_set_asyncmsgq(u->source, dest->asyncmsgq);
    pa_source_update_flags(u->source, PA_SOURCE_LATENCY|PA_SOURCE_DYNAMIC_LATENCY, dest->flags);

    p = pa_proplist_new();
    pa_proplist_setf(p, PA_PROP_DEVICE_DESCRIPTION, "%s connected to %s", u->source->name, u->master_source->name);
    pa_proplist_sets(p, PA_PROP_DEVICE_MASTER_DEVICE, u->master_source->name);
    pa_source_update_proplist(u->source, PA_UPDATE_REPLACE, p);
    pa_proplist_free(p);

    /* Call moving callbacks of slave sources's source-outputs. */
    PA_IDXSET_FOREACH(so, u->source->outputs, idx)
        if (so->moving)
            so->moving(so, u->source);
}

/* Called from main context */
static void source_output_kill_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    pa_source_unlink(u->source);

    pa_source_output_unlink(u->source_output);

    pa_source_unref(u->source);
    u->source = NULL;
    pa_source_output_unref(u->source_output);
    u->source_output = NULL;

    pa_module_unload_request(u->module, true);
}

static void set_hooks(struct userdata *u) {
    u->algorithm = meego_algorithm_hook_api_get(u->core);
    u->hook_algorithm = meego_algorithm_hook_init(u->algorithm, RECORD_HOOK_DYNAMIC_ENHANCE);
}

static void unset_hooks(struct userdata *u) {
    meego_algorithm_hook_done(u->hook_algorithm);

    meego_algorithm_hook_api_unref(u->algorithm);
    u->algorithm = NULL;
    u->hook_algorithm = NULL;
}


int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    const char *source_name, *master_source_name;
    pa_source *master_source;
    pa_sample_spec ss;
    pa_channel_map map;
    char t[256];
    pa_source_output_new_data source_output_data;
    pa_source_new_data source_data;
    bool stereo = true;
    unsigned samplerate;
    unsigned samplelength;
    int maxblocksize;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log_error("Failed to parse module arguments");
        goto fail;
    }

    source_name = pa_modargs_get_value(ma, "source_name", NULL);
    master_source_name = pa_modargs_get_value(ma, "master_source", NULL);

    if (pa_modargs_get_value(ma, "stereo", NULL)) {
        stereo = pa_parse_boolean(pa_modargs_get_value(ma, "stereo", NULL));
    }

    samplerate = DEFAULT_SAMPLERATE;
    samplelength = DEFAULT_SAMPLELENGTH;
    pa_modargs_get_value_u32(ma, "rate", &samplerate);
    pa_modargs_get_value_u32(ma, "samplelength", &samplelength);

    if (!(master_source = pa_namereg_get(m->core, master_source_name, PA_NAMEREG_SOURCE))) {
        pa_log_error("Master source \"%s\" not found", master_source_name);
        goto fail;
    }

    /* samplerate (Hz) * x channels * 2 (two bytes = 16 bit) * samplelength (s)  */
    maxblocksize = samplerate * (stereo ? 2:1) * 2 * samplelength / 1000 ;

    /* ss.format = PA_SAMPLE_S16LE; */
    ss.format = master_source->sample_spec.format;
    ss.rate = samplerate;
    if (stereo) {
        ss.channels = 2;
        pa_channel_map_init_stereo(&map);
    } else {
        ss.channels = 1;
        pa_channel_map_init_mono(&map);
    }

    u = pa_xnew0(struct userdata, 1);
    m->userdata = u;
    u->core = m->core;
    u->module = m;
    u->master_source = master_source;
    u->source = NULL;
    u->source_output = NULL;
    u->maxblocksize = maxblocksize;

    u->memblockq = pa_memblockq_new("record memblockq", 0, maxblocksize*8, 0, &ss, 0, 0, 0, NULL);
    if (!u->memblockq) {
        pa_log_error("couldn't alloc memblockq");
        goto fail;
    }

    /* SOURCE */

    pa_source_new_data_init(&source_data);
    source_data.module = m;
    source_data.driver = __FILE__;
    pa_source_new_data_set_name(&source_data, source_name);
    pa_source_new_data_set_sample_spec(&source_data, &ss);
    pa_source_new_data_set_channel_map(&source_data, &map);
    pa_proplist_setf(source_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "%s connected to %s", source_name, master_source->name);
    pa_proplist_sets(source_data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, master_source->name);
    pa_proplist_sets(source_data.proplist,
                     PA_PROP_SOURCE_RECORD_API_EXTENSION_PROPERTY_NAME,
                     PA_PROP_SOURCE_RECORD_API_EXTENSION_PROPERTY_VALUE);

    u->source = pa_source_new(m->core, &source_data, u->master_source->flags &
                              (PA_SOURCE_LATENCY|PA_SOURCE_DYNAMIC_LATENCY));
    pa_source_new_data_done(&source_data);
    if (!u->source) {
        pa_log_error("Failed to create source.");
        goto fail;
    }

    u->source->parent.process_msg = source_process_msg;
#if PULSEAUDIO_VERSION >= 12
    u->source->set_state_in_main_thread = source_set_state_in_main_thread;
#else
    u->source->set_state = source_set_state;
#endif
    u->source->update_requested_latency = source_update_requested_latency;

    u->source->userdata = u;

    pa_source_set_asyncmsgq(u->source, u->master_source->asyncmsgq);
    pa_source_set_rtpoll(u->source, u->master_source->thread_info.rtpoll);

    /* SOURCE OUTPUT */

    pa_source_output_new_data_init(&source_output_data);
    source_output_data.flags = 0; // PA_SOURCE_OUTPUT_DONT_MOVE
    snprintf(t, sizeof(t), "output of %s", source_name);
    pa_proplist_sets(source_output_data.proplist, PA_PROP_MEDIA_NAME, t);
    pa_proplist_sets(source_output_data.proplist, PA_PROP_APPLICATION_NAME, t); /* this is the default value used by PA modules */
    source_output_data.source = master_source;
    source_output_data.destination_source = u->source;
    source_output_data.driver = __FILE__;
    source_output_data.module = m;
    pa_source_output_new_data_set_sample_spec(&source_output_data, &ss);
    pa_source_output_new_data_set_channel_map(&source_output_data, &map);

    pa_source_output_new(&u->source_output, m->core, &source_output_data);
    pa_source_output_new_data_done(&source_output_data);
    if (!u->source_output) {
        pa_log_error("Failed to create source output.");
        goto fail;
    }

    u->source_output->push = source_output_push_cb;
    u->source_output->process_rewind = source_output_process_rewind_cb;
    u->source_output->update_max_rewind = source_output_update_max_rewind_cb;
    u->source_output->update_source_latency_range = source_output_update_source_latency_range_cb;
    u->source_output->update_source_fixed_latency = source_output_update_source_fixed_latency_cb;
    u->source_output->kill = source_output_kill_cb;
    u->source_output->attach = source_output_attach_cb;
    u->source_output->detach = source_output_detach_cb;
    u->source_output->moving = source_output_moving_cb;
    u->source_output->userdata = u;

    set_hooks(u);

    u->source->output_from_master = u->source_output;

    /* SOURCE & SOURCE OUTPUT READY */

    pa_source_put(u->source);
    pa_source_output_put(u->source_output);

    pa_modargs_free(ma);

    return 0;

 fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);
    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    unset_hooks(u);

    if (u->source_output) {
        pa_source_output_unlink(u->source_output);
        pa_source_output_unref(u->source_output);
    }

    if (u->source) {
        pa_source_unlink(u->source);
        pa_source_unref(u->source);
    }

    if (u->memblockq) {
        pa_memblockq_free(u->memblockq);
        u->memblockq = NULL;
    }

    pa_xfree(u);
}
