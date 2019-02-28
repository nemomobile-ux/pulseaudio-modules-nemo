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
#include <pulse/rtclock.h>

#include "module-voice-userdata.h"
#include "module-voice-api.h"
#include "voice-hooks.h"
#include "voice-voip-source.h"
#include "voice-aep-ear-ref.h"
#include "voice-util.h"
/*** voice source callbacks ***/

/* Called from I/O thread context */
static int voip_source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SOURCE(o)->userdata;

    switch (code) {

        case VOICE_SOURCE_SET_UL_DEADLINE: {
            u->ul_deadline = offset;
            pa_log_debug("Uplink deadline set to %" PRIi64 " (%" PRIu64 " usec from now)",
                         u->ul_deadline, u->ul_deadline - pa_rtclock_now());
            return 0;
        }

        case PA_SOURCE_MESSAGE_GET_LATENCY: {
            pa_usec_t usec = 0;

            if (PA_MSGOBJECT(u->master_source)->process_msg(
                    PA_MSGOBJECT(u->master_source), PA_SOURCE_MESSAGE_GET_LATENCY, &usec, 0, NULL) < 0)
                usec = 0;

            usec += pa_bytes_to_usec(pa_memblockq_get_length(u->ul_memblockq),
                                     &u->aep_sample_spec);
            *((pa_usec_t*) data) = usec;
            return 0;
        }
    }

    return pa_source_process_msg(o, code, data, offset, chunk);
}

/* Called from main context */
static int voip_source_set_state(pa_source *s, pa_source_state_t state) {
    struct userdata *u;
    int ret = 0;

    pa_source_assert_ref(s);
    pa_assert_se(u = s->userdata);

    ret = voice_source_set_state(s, u->raw_source, state);

    /* TODO: Check if we still need to fiddle with PROP_MIXER_TUNING_MODE */
    if (s->state != PA_SOURCE_RUNNING && state == PA_SOURCE_RUNNING) {
        meego_algorithm_hook_fire(u->hooks[HOOK_CALL_BEGIN], s);
    }

    pa_log_debug("(%p) called with %d", (void *)s, state);
    return ret;
}

#if PULSEAUDIO_VERSION >= 12
static int source_set_state_in_main_thread(pa_source *s, pa_source_state_t state, pa_suspend_cause_t suspend_cause) {
    if (s->state == state)
        return 0;

    return voip_source_set_state(s, state);
}
#endif

/* Called from I/O thread context */
static void voip_source_update_requested_latency(pa_source *s) {
    struct userdata *u;

    pa_source_assert_ref(s);
    pa_assert_se(u = s->userdata);

    /* Just hand this one over to the master source */
    pa_source_output_set_requested_latency_within_thread(
            u->hw_source_output,
            voice_source_get_requested_latency(s, u->raw_source));
}

int voice_init_voip_source(struct userdata *u, const char *name) {
    pa_source_new_data data;

    pa_assert(u);
    pa_assert(u->master_source);

    pa_source_new_data_init(&data);
    data.driver = __FILE__;
    data.module = u->module;
    pa_source_new_data_set_name(&data, name);
    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "%s source connected to %s", name, u->raw_source->name);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, u->raw_source->name);
    pa_proplist_sets(data.proplist, "module-suspend-on-idle.timeout", "0");
    pa_proplist_sets(data.proplist, PA_PROP_SOURCE_API_EXTENSION_PROPERTY_NAME,
                     PA_PROP_SOURCE_API_EXTENSION_PROPERTY_VALUE);

    pa_source_new_data_set_sample_spec(&data, &u->aep_sample_spec);
    pa_source_new_data_set_channel_map(&data, &u->aep_channel_map);

    u->voip_source = pa_source_new(u->core, &data, u->master_source->flags &
                                   (PA_SOURCE_LATENCY|PA_SOURCE_DYNAMIC_LATENCY));
    pa_source_new_data_done(&data);

    if (!u->voip_source) {
        pa_log("Failed to create source");
        return -1;
    }

    u->voip_source->parent.process_msg = voip_source_process_msg;
#if PULSEAUDIO_VERSION >= 12
    u->voip_source->set_state_in_main_thread = source_set_state_in_main_thread;
#else
    u->voip_source->set_state = voip_source_set_state;
#endif
    u->raw_source->update_requested_latency = voip_source_update_requested_latency;
    u->voip_source->userdata = u;

    pa_source_set_asyncmsgq(u->voip_source, u->master_source->asyncmsgq);
    pa_source_set_rtpoll(u->voip_source, u->master_source->thread_info.rtpoll);

    return 0;
}
