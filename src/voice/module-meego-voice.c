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
#include "module-meego-voice-symdef.h"

#include <pulsecore/modargs.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sample-util.h>

#include "voice-convert.h"
#include "voice-aep-sink-input.h"
#include "voice-hw-sink-input.h"
#include "voice-hw-source-output.h"
#include "voice-raw-sink.h"
#include "voice-raw-source.h"
#include "voice-voip-sink.h"
#include "voice-voip-source.h"
#include "voice-util.h"
#include "voice-aep-ear-ref.h"
#include "voice-mainloop-handler.h"
#include "module-voice-api.h"

#include "shared-data.h"
#include "proplist-meego.h"
#include "proplist-nemo.h"

PA_MODULE_AUTHOR("Jyri Sarha");
PA_MODULE_DESCRIPTION("Nokia voice module");
PA_MODULE_USAGE("voice_sink_name=<name for the voice sink> "
                "voice_source_name=<name for the voice source> "
                "master_sink=<sink to connect to> "
                "master_source=<source to connect to> "
                "raw_sink=<name for raw sink> "
                "raw_source=<name for raw source> "
                "max_hw_frag_size=<maximum fragment size of master sink and source in usecs>");
PA_MODULE_VERSION(PACKAGE_VERSION) ;


static const char* const valid_modargs[] = {
    "voice_sink_name",
    "voice_source_name",
    "master_sink",
    "master_source",
    "raw_sink_name",
    "raw_source_name",
    "max_hw_frag_size",
    NULL,
};

/* update the volume by firing the appropriate volume hook */
static void voice_update_volumes(struct userdata *u) {
    const pa_cvolume *cvol;

    pa_assert(u);
    pa_assert(u->master_sink);

    cvol = &u->master_sink->real_volume;

    /* check the volume , if its same as previous one then return */
    if (pa_cvolume_equal(cvol, &u->previous_volume))
        return;

    /* assign the current volume, will be used for the next time*/
    u->previous_volume = *cvol;

    if (voice_voip_source_active(u))
        meego_algorithm_hook_fire(u->hooks[HOOK_CALL_VOLUME], (void*)cvol);
    else
        meego_algorithm_hook_fire(u->hooks[HOOK_VOLUME], (void*)cvol);

    pa_log_debug("volume is updated");
}

static void master_sink_volume_subscribe_cb(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(c);
    pa_assert(u);

    voice_update_volumes(u);
}

static void master_source_state_subscribe_cb(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(c);
    pa_assert(u);

    if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) != PA_SUBSCRIPTION_EVENT_CHANGE)
        return;

    if (!u->master_source)
        return;

    if (u->master_source != pa_idxset_get_by_index(c->sources, idx))
        return;

#if (PA_CHECK_VERSION(13,0,0))
    if (u->master_source->state == u->previous_master_source_state)
#else
    if (pa_source_get_state(u->master_source) == u->previous_master_source_state)
#endif
        return;

#if (PA_CHECK_VERSION(13,0,0))
    u->previous_master_source_state = u->master_source->state;
#else
    u->previous_master_source_state = pa_source_get_state(u->master_source);
#endif

    if (u->previous_master_source_state == PA_SOURCE_SUSPENDED) {
        meego_algorithm_hook_fire(u->hooks[HOOK_SOURCE_RESET], NULL);
        pa_log_debug("VOICE_HOOK_SOURCE_RESET fired");
        voice_aep_ear_ref_loop_reset(u);
    }
}

static int set_hooks(struct userdata *u) {
    pa_assert(u);

    u->algorithm = meego_algorithm_hook_api_get(u->core);

    u->hooks[HOOK_HW_SINK_PROCESS]              = meego_algorithm_hook_init(u->algorithm, VOICE_HOOK_HW_SINK_PROCESS);
    u->hooks[HOOK_NARROWBAND_EAR_EQU_MONO]      = meego_algorithm_hook_init(u->algorithm, VOICE_HOOK_NARROWBAND_EAR_EQU_MONO);
    u->hooks[HOOK_NARROWBAND_MIC_EQ_MONO]       = meego_algorithm_hook_init(u->algorithm, VOICE_HOOK_NARROWBAND_MIC_EQ_MONO);
    u->hooks[HOOK_WIDEBAND_MIC_EQ_MONO]         = meego_algorithm_hook_init(u->algorithm, VOICE_HOOK_WIDEBAND_MIC_EQ_MONO);
    u->hooks[HOOK_WIDEBAND_MIC_EQ_STEREO]       = meego_algorithm_hook_init(u->algorithm, VOICE_HOOK_WIDEBAND_MIC_EQ_STEREO);
    u->hooks[HOOK_XPROT_MONO]                   = meego_algorithm_hook_init(u->algorithm, VOICE_HOOK_XPROT_MONO);
    u->hooks[HOOK_VOLUME]                       = meego_algorithm_hook_init(u->algorithm, VOICE_HOOK_VOLUME);
    u->hooks[HOOK_CALL_VOLUME]                  = meego_algorithm_hook_init(u->algorithm, VOICE_HOOK_CALL_VOLUME);
    u->hooks[HOOK_CALL_BEGIN]                   = meego_algorithm_hook_init(u->algorithm, VOICE_HOOK_CALL_BEGIN);
    u->hooks[HOOK_CALL_END]                     = meego_algorithm_hook_init(u->algorithm, VOICE_HOOK_CALL_END);
    u->hooks[HOOK_AEP_DOWNLINK]                 = meego_algorithm_hook_init(u->algorithm, VOICE_HOOK_AEP_DOWNLINK);
    u->hooks[HOOK_AEP_UPLINK]                   = meego_algorithm_hook_init(u->algorithm, VOICE_HOOK_AEP_UPLINK);
    u->hooks[HOOK_RMC_MONO]                     = meego_algorithm_hook_init(u->algorithm, VOICE_HOOK_RMC_MONO);
    u->hooks[HOOK_SOURCE_RESET]                 = meego_algorithm_hook_init(u->algorithm, VOICE_HOOK_SOURCE_RESET);
    return 0;
}

static int unset_hooks(struct userdata *u) {
    int i;

    pa_assert(u);

    for (i = 0; i < HOOK_MAX; i++)
        meego_algorithm_hook_done(u->hooks[i]);

    meego_algorithm_hook_api_unref(u->algorithm);
    u->algorithm = NULL;

    return 0;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    const char *master_sink_name;
    const char *master_source_name;
    const char *raw_sink_name;
    const char *raw_source_name;
    const char *voice_sink_name;
    const char *voice_source_name;
    const char *max_hw_frag_size_str;
    int max_hw_frag_size = 3840;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    master_sink_name = pa_modargs_get_value(ma, "master_sink", NULL);
    master_source_name = pa_modargs_get_value(ma, "master_source", NULL);

    raw_sink_name = pa_modargs_get_value(ma, "raw_sink_name", "sink.voice.raw");
    raw_source_name = pa_modargs_get_value(ma, "raw_source_name", "source.voice.raw");
    voice_sink_name = pa_modargs_get_value(ma, "voice_sink_name", "sink.voice");
    voice_source_name = pa_modargs_get_value(ma, "voice_source_name", "source.voice");
    max_hw_frag_size_str = pa_modargs_get_value(ma, "max_hw_frag_size", "3840");

    pa_log_debug("Got arguments: master_sink=\"%s\" master_source=\"%s\" "
                 "raw_sink_name=\"%s\" raw_source_name=\"%s\" max_hw_frag_size=\"%s\".",
                 master_sink_name, master_source_name,
                 raw_sink_name, raw_source_name,
                 max_hw_frag_size_str);

    m->userdata = u = pa_xnew0(struct userdata, 1);

    if (!(u->master_sink = pa_namereg_get(m->core, master_sink_name, PA_NAMEREG_SINK))) {
        pa_log("Master sink \"%s\" not found", master_sink_name);
        goto fail;
    }

    if (!(u->master_source = pa_namereg_get(m->core, master_source_name, PA_NAMEREG_SOURCE))) {
        pa_log("Master source \"%s\" not found", master_source_name);
        goto fail;
    }

    if (u->master_sink->sample_spec.format != u->master_source->sample_spec.format &&
        u->master_sink->sample_spec.rate != u->master_source->sample_spec.rate &&
        u->master_sink->sample_spec.channels != u->master_source->sample_spec.channels) {
        pa_log("Master source and sink must have same sample spec");
        goto fail;
    }

    if (pa_atoi(max_hw_frag_size_str, &max_hw_frag_size) < 0 ||
        max_hw_frag_size < 960 ||
        max_hw_frag_size > 128*960) {
        pa_log("Bad value for max_hw_frag_size: %s", max_hw_frag_size_str);
        goto fail;
    }

    u->modargs = ma;
    u->core = m->core;
    u->module = m;

    set_hooks(u);

    u->mainloop_handler = voice_mainloop_handler_new(u);

    u->ul_timing_advance = 500; // = 500 micro seconds, seems to be a good default value

    pa_channel_map_init_mono(&u->mono_map);
    pa_channel_map_init_stereo(&u->stereo_map);

    u->hw_sample_spec.format = PA_SAMPLE_S16NE;
    u->hw_sample_spec.rate = VOICE_SAMPLE_RATE_HW_HZ;
    u->hw_sample_spec.channels = 2;

    u->hw_mono_sample_spec.format = PA_SAMPLE_S16NE;
    u->hw_mono_sample_spec.rate = VOICE_SAMPLE_RATE_HW_HZ;
    u->hw_mono_sample_spec.channels = 1;

    u->aep_sample_spec.format = PA_SAMPLE_S16NE;
    u->aep_sample_spec.rate = VOICE_SAMPLE_RATE_AEP_HZ;
    u->aep_sample_spec.channels = 1;
    pa_channel_map_init_mono(&u->aep_channel_map);
    // The result is rounded down incorrectly thus +1
    u->aep_fragment_size = pa_usec_to_bytes(VOICE_PERIOD_AEP_USECS+1, &u->aep_sample_spec);
    u->aep_hw_fragment_size = pa_usec_to_bytes(VOICE_PERIOD_AEP_USECS+1, &u->hw_sample_spec);
    u->hw_fragment_size = pa_usec_to_bytes(VOICE_PERIOD_MASTER_USECS+1, &u->hw_sample_spec);
    u->hw_fragment_size_max = max_hw_frag_size;
    if (0 != (u->hw_fragment_size_max % u->hw_fragment_size))
        u->hw_fragment_size_max += u->hw_fragment_size - (u->hw_fragment_size_max % u->hw_fragment_size);
    u->aep_hw_mono_fragment_size = pa_usec_to_bytes(VOICE_PERIOD_AEP_USECS+1, &u->hw_mono_sample_spec);
    u->hw_mono_fragment_size = pa_usec_to_bytes(VOICE_PERIOD_MASTER_USECS+1, &u->hw_mono_sample_spec);

    u->voice_ul_fragment_size = pa_usec_to_bytes(VOICE_PERIOD_CMT_USECS+1, &u->aep_sample_spec);
    pa_silence_memchunk_get(&u->core->silence_cache,
                            u->core->mempool,
                            &u->aep_silence_memchunk,
                            & u->aep_sample_spec,
                            u->aep_fragment_size);

    voice_memchunk_pool_load(u);

    if (voice_init_raw_sink(u, raw_sink_name))
        goto fail;

    u->shared = pa_shared_data_get(m->core);

    pa_atomic_store(&u->mixer_state, PROP_MIXER_TUNING_PRI);
    pa_shared_data_sets(u->shared, PA_NEMO_PROP_CALL_STATE, PA_NEMO_PROP_CALL_STATE_INACTIVE);
    u->alt_mixer_compensation = PA_VOLUME_NORM;

    if (voice_init_hw_sink_input(u))
        goto fail;

    /* This must be set before calling pa_sink_put(), because pa_sink_put() has
     * assertion
     * "!(s->flags & PA_SINK_SHARE_VOLUME_WITH_MASTER) || s->input_to_master".
     */
    u->raw_sink->input_to_master = u->hw_sink_input;

    /* This must be called before calling voice_init_voip_sink(), because
     * pa_sink_input_new() has assertion
     * "PA_SINK_IS_LINKED(pa_sink_get_state(data->sink))". */
    pa_sink_put(u->raw_sink);

    /* This must be called before calling voice_init_aep_sink_input(), because
     * the flat volume logic will otherwise mess up the aep sink input's volume
     * when pa_sink_input_put(u->hw_sink_input) is called. */
    pa_sink_input_put(u->hw_sink_input);

    if (voice_init_voip_sink(u, voice_sink_name))
        goto fail;

    if (voice_init_aep_sink_input(u))
        goto fail;

    u->sink_temp_buff = pa_xmalloc(2*u->hw_fragment_size_max);
    u->sink_temp_buff_len = 2*u->hw_fragment_size_max;

    if (voice_init_raw_source(u, raw_source_name))
        goto fail;
    pa_source_put(u->raw_source);

    if (voice_init_voip_source(u, voice_source_name))
        goto fail;
    pa_source_put(u->voip_source);

    if (voice_init_hw_source_output(u))
        goto fail;

    /* TODO: Guess we should use max_hw_frag_size here */
    u->hw_source_memblockq = // 8 * 5ms = 40ms
        pa_memblockq_new("voice hw_source_memblockq", 0, 2*u->hw_fragment_size_max, 0, &u->hw_sample_spec, 0, 0, 0, NULL);

    u->ul_memblockq =
        pa_memblockq_new("voice ul_memblockq", 0, 2*u->voice_ul_fragment_size, 0, &u->aep_sample_spec, 0, 0, 0, NULL);

    u->dl_sideinfo_queue = pa_queue_new();

    u->ul_deadline = 0;

    u->linear_q15_master_volume_L = INT16_MAX;
    u->linear_q15_master_volume_R = INT16_MAX;

    voice_aep_ear_ref_init(u);

    if (voice_convert_init(u))
        goto fail;

    /* IHF mode is the default and this initialization is consistent with it. */
    u->active_mic_channel = MIC_CH0;

    meego_parameter_request_updates("voice", (pa_hook_cb_t)voice_parameter_cb, PA_HOOK_NORMAL, false, u);
    meego_parameter_request_updates("alsa", (pa_hook_cb_t)alsa_parameter_cb, PA_HOOK_NORMAL, false, u);
    meego_parameter_request_updates("aep", (pa_hook_cb_t)aep_parameter_cb, PA_HOOK_LATE, false, u);

    /*         aep-s-i                                            */
    /* voip-sink ---\                 hw-sink-input               */
    /*                > optimized mix -------------> master-sink  */
    /*                |                                           */
    /*             raw-sink                                       */

    /*                                                  */
    /* voip-src  <---       hw-source-output            */
    /*                < mux <------------- master-src   */
    /*  raw-src  <---                                   */

    u->voip_sink->input_to_master = u->aep_sink_input;

    pa_sink_put(u->voip_sink);

    pa_source_output_put(u->hw_source_output);
    pa_sink_input_put(u->aep_sink_input);

    u->sink_subscription = pa_subscription_new(m->core, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SINK_INPUT, master_sink_volume_subscribe_cb, u);

#if (PA_CHECK_VERSION(13,0,0))
    u->previous_master_source_state = u->master_source->state;
#else
    u->previous_master_source_state = pa_source_get_state(u->master_source);
#endif
    u->source_change_subscription = pa_subscription_new(m->core, PA_SUBSCRIPTION_MASK_SOURCE, master_source_state_subscribe_cb, u);
    return 0;

fail:
    pa__done(m);
    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u = m->userdata;

    assert(m);

    if (!u)
        return;

    if (u->shared)
        pa_shared_data_unref(u->shared);

    voice_clear_up(u);

    if (u->modargs)
        pa_modargs_free(u->modargs);

    unset_hooks(u);

    pa_xfree(u);
}
