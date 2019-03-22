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
#include "module-voice-userdata.h"
#include "module-voice-api.h"
#include "voice-hooks.h"
#include "voice-voip-sink.h"
#include "voice-aep-ear-ref.h"
#include "voice-util.h"
#include <pulse/volume.h>

/*** voip sink callbacks ***/

/* Called from I/O thread context */
static int voip_sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {

        case VOICE_SINK_GET_SIDE_INFO_QUEUE_PTR: {
            /* TODO: Make sure there is only one client (or multiple queues) */
            if (!u->dl_sideinfo_queue) {
                pa_log_warn("Side info queue not set");
            }
            *((pa_queue **) data) = u->dl_sideinfo_queue;
            pa_log_debug("Side info queue (%p) passed to client", (void *) u->dl_sideinfo_queue);
            return 0;
        }

        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t usec = 0;

            if (PA_MSGOBJECT(u->raw_sink)->process_msg(PA_MSGOBJECT(u->raw_sink), PA_SINK_MESSAGE_GET_LATENCY, &usec, (int64_t)0, NULL) < 0)
                usec = 0;

            *((pa_usec_t*) data) = usec;
            return 0;
        }

        case PA_SINK_MESSAGE_ADD_INPUT: {
            pa_sink_input *i = PA_SINK_INPUT(data);
            if (i == u->hw_sink_input) {
                pa_log_error("Denied loop connection");
                // TODO: How to deny connection...
                return -1;
            }
            // Pass trough to pa_sink_process_msg
            break;
        }

    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from main context */
static int voip_sink_set_state(pa_sink *s, pa_sink_state_t state) {
    struct userdata *u;
    int ret = 0;
    ENTER();

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    ret = voice_sink_set_state(s, u->raw_sink, state);

    /* TODO: Check if we still need to fiddle with PROP_MIXER_TUNING_MODE */
    if (s->state != PA_SINK_RUNNING && state == PA_SINK_RUNNING) {
        voice_aep_ear_ref_loop_reset(u);
        meego_algorithm_hook_fire(u->hooks[HOOK_CALL_BEGIN], s);
    }
    else if (s->state == PA_SINK_RUNNING && state != PA_SINK_RUNNING)
        meego_algorithm_hook_fire(u->hooks[HOOK_CALL_END], s);

    pa_log_debug("(%p): called with %d", (void *)s, state);
    return ret;
}

#if PULSEAUDIO_VERSION >= 12
static int sink_set_state_in_main_thread(pa_sink *s, pa_sink_state_t state, pa_suspend_cause_t suspend_cause) {
    if (s->state == state)
        return 0;

    return voip_sink_set_state(s, state);
}
#endif

/* Called from I/O thread context */
static void voip_sink_request_rewind(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    /* Just hand this one over to the master sink */
    if (u->hw_sink_input && s->thread_info.rewind_nbytes > 0) {
        size_t nbytes = voice_convert_nbytes(s->thread_info.rewind_nbytes, &s->sample_spec, &u->hw_sink_input->sample_spec);

        pa_sink_input_request_rewind(u->hw_sink_input, nbytes, true, false, false);
    }
}

/* Called from I/O thread context */
static void voip_sink_update_requested_latency(pa_sink *s) {
    struct userdata *u;
    ENTER();

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    /* For example a2dp playback if sink is destroyed this function
     * is called with hw_sink_input->sink = NULL, while moving sink_input
     * elsewhere, which causes
     * pa_sink_input_set_requested_latency_within_thread() segfault. */
    if (!u->hw_sink_input->sink) {
        pa_log_debug("%s() hw_sink_input->sink = NULL, won't propagate to master sink", __FUNCTION__);
        return;
    }

    /* Just hand this one over to the master sink */
    pa_sink_input_set_requested_latency_within_thread(
        u->hw_sink_input,
        voice_sink_get_requested_latency(s, u->raw_sink));
}

int voice_init_voip_sink(struct userdata *u, const char *name) {
    pa_sink_new_data sink_data;
    pa_assert(u);
    pa_assert(u->core);
    pa_assert(u->master_sink);
    ENTER();

    pa_sink_new_data_init(&sink_data);
    sink_data.module = u->module;
    sink_data.driver = __FILE__;
    pa_sink_new_data_set_name(&sink_data, name);
    pa_sink_new_data_set_sample_spec(&sink_data, &u->aep_sample_spec);
    pa_sink_new_data_set_channel_map(&sink_data, &u->aep_channel_map);
    pa_proplist_setf(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "%s connected conceptually to %s", name, u->raw_sink->name);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, u->raw_sink->name);
    pa_proplist_sets(sink_data.proplist, "module-suspend-on-idle.timeout", "1");

    pa_proplist_sets(sink_data.proplist, PA_PROP_SINK_API_EXTENSION_PROPERTY_NAME,
                     PA_PROP_SINK_API_EXTENSION_PROPERTY_VALUE);

    u->voip_sink = pa_sink_new(u->core, &sink_data,
                               (u->master_sink->flags & (PA_SINK_LATENCY | PA_SINK_DYNAMIC_LATENCY)) | PA_SINK_SHARE_VOLUME_WITH_MASTER);

    pa_sink_new_data_done(&sink_data);

    /* Create sink */
    if (!u->voip_sink) {
        pa_log_error("Failed to create sink.");
        return -1;
    }

    u->voip_sink->parent.process_msg = voip_sink_process_msg;
#if PULSEAUDIO_VERSION >= 12
    u->voip_sink->set_state_in_main_thread = sink_set_state_in_main_thread;
#else
    u->voip_sink->set_state = voip_sink_set_state;
#endif
    u->voip_sink->update_requested_latency = voip_sink_update_requested_latency;
    u->voip_sink->request_rewind = voip_sink_request_rewind;
    u->voip_sink->userdata = u;
    pa_memblock_ref(u->aep_silence_memchunk.memblock);
    u->voip_sink->silence = u->aep_silence_memchunk;

    pa_sink_set_asyncmsgq(u->voip_sink, u->master_sink->asyncmsgq);
    pa_sink_set_rtpoll(u->voip_sink, u->master_sink->thread_info.rtpoll);

    return 0;
}
