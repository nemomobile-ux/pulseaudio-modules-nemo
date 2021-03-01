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

#include <errno.h>
#include <pulsecore/namereg.h>

#include "module-voice-userdata.h"
#include "voice-util.h"
#include "voice-aep-ear-ref.h"
#include "voice-convert.h"
#include "proplist-meego.h"
#include "proplist-nemo.h"
#include "voice-mainloop-handler.h"

#include "voice-voip-source.h"
#include "voice-voip-sink.h"

#include "shared-data.h"

/*** Deallocate stuff ***/
void voice_clear_up(struct userdata *u) {
    pa_assert(u);

    if (u->mainloop_handler) {
        u->mainloop_handler->parent.free((pa_object *)u->mainloop_handler);
        u->mainloop_handler = NULL;
    }

    if (u->hw_sink_input) {
        pa_sink_input_unlink(u->hw_sink_input);
        pa_sink_input_unref(u->hw_sink_input);
        u->hw_sink_input = NULL;
    }

    if (u->raw_sink) {
        pa_sink_unlink(u->raw_sink);
        pa_sink_unref(u->raw_sink);
        u->raw_sink = NULL;
    }

    if (u->voip_sink) {
        pa_sink_unlink(u->voip_sink);
        pa_sink_unref(u->voip_sink);
        u->voip_sink = NULL;
    }

    if (u->hw_source_output) {
        if (u->hw_source_output_move_fail_slot) {
            pa_hook_slot_free(u->hw_source_output_move_fail_slot);
            u->hw_source_output_move_fail_slot = NULL;
        }
        if (u->hw_source_output_flags_changed_slot) {
            pa_hook_slot_free(u->hw_source_output_flags_changed_slot);
            u->hw_source_output_flags_changed_slot = NULL;
        }
        pa_source_output_unlink(u->hw_source_output);
        pa_source_output_unref(u->hw_source_output);
        u->hw_source_output = NULL;
    }

    if (u->voip_source) {
        pa_source_unlink(u->voip_source);
        pa_source_unref(u->voip_source);
        u->voip_source = NULL;
    }

    if (u->raw_source) {
        pa_source_unlink(u->raw_source);
        pa_source_unref(u->raw_source);
        u->raw_source = NULL;
    }

    if (u->hw_source_memblockq) {
        pa_memblockq_free(u->hw_source_memblockq);
        u->hw_source_memblockq = NULL;
    }

    if (u->ul_memblockq) {
        pa_memblockq_free(u->ul_memblockq);
        u->ul_memblockq = NULL;
    }

    if (u->dl_sideinfo_queue) {
        pa_queue_free(u->dl_sideinfo_queue, NULL);
        u->dl_sideinfo_queue = NULL;
    }

    voice_aep_ear_ref_unload(u);

    if (u->aep_silence_memchunk.memblock) {
        pa_memblock_unref(u->aep_silence_memchunk.memblock);
        pa_memchunk_reset(&u->aep_silence_memchunk);
    }

    if (u->sink_temp_buff) {
        pa_xfree(u->sink_temp_buff);
        u->sink_temp_buff = NULL;
    }

    if (u->sink_subscription) {
        pa_subscription_free(u->sink_subscription);
        u->sink_subscription = NULL;
    }

    if (u->sink_proplist_changed_slot) {
        pa_hook_slot_free(u->sink_proplist_changed_slot);
        u->sink_proplist_changed_slot = NULL;
    }

    if (u->source_proplist_changed_slot) {
        pa_hook_slot_free(u->source_proplist_changed_slot);
        u->source_proplist_changed_slot = NULL;
    }

    if (u->source_change_subscription) {
        pa_subscription_free(u->source_change_subscription);
        u->source_change_subscription = NULL;
    }

    voice_convert_free(u);
    voice_memchunk_pool_unload(u);
}

static voice_memchunk_pool *voice_memchunk_pool_table = NULL;
void voice_memchunk_pool_load(struct userdata *u) {
    int i;

    pa_assert(0 == offsetof(voice_memchunk_pool, chunk));
    pa_atomic_ptr_store(&u->memchunk_pool, NULL);

    voice_memchunk_pool_table = pa_xmalloc0(sizeof(voice_memchunk_pool)*VOICE_MEMCHUNK_POOL_SIZE);
    pa_assert(voice_memchunk_pool_table);

    for (i = 0; i<VOICE_MEMCHUNK_POOL_SIZE; i++)
        voice_memchunk_pool_free(u, (pa_memchunk *)&voice_memchunk_pool_table[i]);
}

void voice_memchunk_pool_unload(struct userdata *u) {
    int i = 0;

    if (voice_memchunk_pool_table == NULL)
        return;

    while (voice_memchunk_pool_get(u)) i++;

    if (i < VOICE_MEMCHUNK_POOL_SIZE)
        pa_log("voice_memchunk_pool only %d element of %d allocated was retured to pool",
               i, VOICE_MEMCHUNK_POOL_SIZE);

    pa_xfree(voice_memchunk_pool_table);
    voice_memchunk_pool_table = NULL;
}

/* Generic source state change logic. Used by raw_source and voice_source. */
int voice_source_set_state(pa_source *s, pa_source *other, pa_source_state_t state) {
    struct userdata *u;

    pa_source_assert_ref(s);
    pa_assert_se(u = s->userdata);
    if (!other) {
        pa_log_debug("other source not initialized or already freed");
        return 0;
    }
    pa_source_assert_ref(other);

    if (u->hw_source_output) {
        if (u->hw_source_output->state == PA_SOURCE_OUTPUT_RUNNING) {
            if (state == PA_SOURCE_SUSPENDED &&
                other->state == PA_SOURCE_SUSPENDED) {
                pa_source_output_cork(u->hw_source_output, true);
                pa_log_debug("hw_source_output corked");
            }
        }
        else if (u->hw_source_output->state == PA_SOURCE_OUTPUT_CORKED) {
            if (PA_SOURCE_IS_OPENED(state) ||
                PA_SOURCE_IS_OPENED(other->state)) {
                pa_source_output_cork(u->hw_source_output, false);
                pa_log_debug("hw_source_output uncorked");
            }
        }
    }

    return 0;
}

/* Generic sink state change logic. Used by raw_sink and voip_sink. */
int voice_sink_set_state(pa_sink *s, pa_sink *other, pa_sink_state_t state) {
    struct userdata *u;
    pa_sink *om_sink;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);
    if (!other) {
        pa_log_debug("other sink not initialized or already freed");
        return 0;
    }
    pa_sink_assert_ref(other);
    om_sink = voice_get_original_master_sink(u);

    if (u->hw_sink_input && PA_SINK_INPUT_IS_LINKED(u->hw_sink_input->state)) {
        if (u->hw_sink_input->state == PA_SINK_INPUT_CORKED) {
            if (PA_SINK_IS_OPENED(state) ||
                PA_SINK_IS_OPENED(other->state)) {
                pa_sink_input_cork(u->hw_sink_input, false);
                pa_log_debug("hw_sink_input uncorked");
            }
        }
        else {
            if (state == PA_SINK_SUSPENDED &&
                other->state == PA_SINK_SUSPENDED) {
                pa_sink_input_cork(u->hw_sink_input, true);
                pa_log_debug("hw_sink_input corked");
            }
        }
    }

    if (om_sink == NULL) {
        pa_log_info("No master sink, assuming primary mixer tuning.\n");
        pa_atomic_store(&u->mixer_state, PROP_MIXER_TUNING_PRI);
        pa_shared_data_sets(u->shared, PA_NEMO_PROP_CALL_STATE, PA_NEMO_PROP_CALL_STATE_INACTIVE);
    }
    else if (voice_voip_sink_active(u)) {
        if (pa_atomic_load(&u->mixer_state) == PROP_MIXER_TUNING_PRI) {
             pa_proplist *p = pa_proplist_new();
             pa_assert(p);
             pa_proplist_sets(p, PROP_MIXER_TUNING_MODE, PROP_MIXER_TUNING_ALT_S);
             pa_sink_update_proplist(om_sink, PA_UPDATE_REPLACE, p);
             pa_atomic_store(&u->mixer_state, PROP_MIXER_TUNING_ALT);
             pa_shared_data_sets(u->shared, PA_NEMO_PROP_CALL_STATE, PA_NEMO_PROP_CALL_STATE_ACTIVE);
             pa_proplist_free(p);

             meego_algorithm_hook_fire(u->hooks[HOOK_CALL_BEGIN], s);
        }
    }
    else {
        if (pa_atomic_load(&u->mixer_state) == PROP_MIXER_TUNING_ALT) {
            pa_proplist *p = pa_proplist_new();
            pa_assert(p);
            pa_proplist_sets(p, PROP_MIXER_TUNING_MODE, PROP_MIXER_TUNING_PRI_S);
            pa_sink_update_proplist(om_sink, PA_UPDATE_REPLACE, p);
            pa_atomic_store(&u->mixer_state, PROP_MIXER_TUNING_PRI);
            pa_shared_data_sets(u->shared, PA_NEMO_PROP_CALL_STATE, PA_NEMO_PROP_CALL_STATE_INACTIVE);
            pa_proplist_free(p);

            meego_algorithm_hook_fire(u->hooks[HOOK_CALL_END], s);
        }
    }

    return 0;
}

/* Used by raw_source and voip_source. Called from I/O thread. */
pa_usec_t voice_source_get_requested_latency(pa_source *s, pa_source *other) {
    struct userdata *u;
    pa_usec_t latency;
    pa_source_assert_ref(s);

    latency = pa_source_get_requested_latency_within_thread(s);

    pa_assert_se(u = s->userdata);
    if (!other) {
        pa_log_debug("other source not initialized or already freed");
        return latency;
    }
    pa_source_assert_ref(other);

    if (latency == (pa_usec_t) -1 || latency > pa_source_get_requested_latency_within_thread(other))
        latency = pa_source_get_requested_latency_within_thread(other);

    return latency;
}

/* Used by raw_sink and voip_sink. Called from I/O thread. */
pa_usec_t voice_sink_get_requested_latency(pa_sink *s, pa_sink *other) {
    struct userdata *u;
    pa_usec_t latency;
    pa_sink_assert_ref(s);

    latency = pa_sink_get_requested_latency_within_thread(s);

    pa_assert_se(u = s->userdata);
    if (!other) {
        pa_log_debug("other sink not initialized or already freed");
        return latency;
    }
    pa_sink_assert_ref(other);

    if (latency == (pa_usec_t) -1 || latency > pa_sink_get_requested_latency_within_thread(other))
        latency = pa_sink_get_requested_latency_within_thread(other);

    return latency;
}

void voice_sink_inputs_may_move(pa_sink *s, bool move) {
    pa_sink_input *i;
    uint32_t idx;

    for (i = PA_SINK_INPUT(pa_idxset_first(s->inputs, &idx)); i; i = PA_SINK_INPUT(pa_idxset_next(s->inputs, &idx))) {
        if (move)
            i->flags &= ~PA_SINK_INPUT_DONT_MOVE;
        else
            i->flags |= PA_SINK_INPUT_DONT_MOVE;
    }
}

void voice_source_outputs_may_move(pa_source *s, bool move) {
    pa_source_output *i;
    uint32_t idx;

    for (i = PA_SOURCE_OUTPUT(pa_idxset_first(s->outputs, &idx)); i; i = PA_SOURCE_OUTPUT(pa_idxset_next(s->outputs, &idx))) {
        if (move)
            i->flags &= ~PA_SOURCE_OUTPUT_DONT_MOVE;
        else
            i->flags |= PA_SOURCE_OUTPUT_DONT_MOVE;
    }
}

pa_sink *voice_get_original_master_sink(struct userdata *u) {
    const char *om_name;
    pa_sink *om_sink;
    pa_assert(u);
    pa_assert(u->modargs);
    pa_assert(u->core);
    om_name = pa_modargs_get_value(u->modargs, "master_sink", NULL);
    if (!om_name) {
        pa_log_error("Master sink name not found from modargs!");
        return NULL;
    }
    if (!(om_sink = pa_namereg_get(u->core, om_name, PA_NAMEREG_SINK))) {
        pa_log_debug("Original master sink \"%s\" not found", om_name);
        return NULL;
    }
    return om_sink;
}

pa_source *voice_get_original_master_source(struct userdata *u) {
    const char *om_name;
    pa_source *om_source;
    pa_assert(u);
    pa_assert(u->modargs);
    pa_assert(u->core);
    om_name = pa_modargs_get_value(u->modargs, "master_source", NULL);
    if (!om_name) {
        pa_log_error("Master source name not found from modargs!");
        return NULL;
    }
    if (!(om_source = pa_namereg_get(u->core, om_name, PA_NAMEREG_SOURCE))) {
        pa_log_error("Original master source \"%s\" not found", om_name);
        return NULL;
    }
    return om_source;
}

pa_hook_result_t alsa_parameter_cb(pa_core *c, meego_parameter_update_args *ua, struct userdata *u) {
    pa_proplist *p;

    pa_assert(ua);
    pa_assert(u);

    if (ua->parameters) {
        p = pa_proplist_from_string(ua->parameters);

        pa_sink_update_proplist(u->master_sink, PA_UPDATE_REPLACE, p);

        pa_proplist_free(p);
    }

    return PA_HOOK_OK;
}

pa_hook_result_t aep_parameter_cb(pa_core *c, meego_parameter_update_args *ua, struct userdata *u) {
    pa_assert(ua);
    pa_assert(u);

    if (ua->parameters)
        voice_aep_ear_ref_loop_reset(u);

    return PA_HOOK_OK;
}

pa_hook_result_t voice_parameter_cb(pa_core *c, meego_parameter_update_args *ua, struct userdata *u) {
    pa_proplist *p;
    const char *v;
    int temp = 0;
    double tempf = 0;

    pa_assert(ua);
    pa_assert(u);

    if (ua->parameters) {
        p = pa_proplist_from_string(ua->parameters);

        v = pa_strnull(pa_proplist_gets(p, PA_NOKIA_PROP_AUDIO_CMT_UL_TIMING_ADVANCE));
        if (!pa_atoi(v, &temp) && temp > -5000 && temp < 5000)
            u->ul_timing_advance = temp;
        pa_log_debug("ul_timing_advance \"%s\" %d %d", v, u->ul_timing_advance, temp);

        v = pa_strnull(pa_proplist_gets(p, PA_NOKIA_PROP_AUDIO_ALT_MIXER_COMPENSATION));
        if (!pa_atod(v, &tempf) && tempf > -60 && tempf < 0)
            u->alt_mixer_compensation = pa_sw_volume_from_dB(tempf);
        pa_log_debug("alt_mixer_compensation \"%s\" %d %f", v, u->alt_mixer_compensation, tempf);

        v = pa_strnull(pa_proplist_gets(p, PA_NOKIA_PROP_AUDIO_EAR_REF_PADDING));
        if (!pa_atoi(v, &temp) && temp > -10000 && temp < 200000)
            u->ear_ref.loop_padding_usec = temp;
        pa_log_debug("ear_ref_padding \"%s\" %d %d", v, (int)u->ear_ref.loop_padding_usec, temp);

        v = pa_strnull(pa_proplist_gets(p, PA_NOKIA_PROP_AUDIO_ACTIVE_MIC_CHANNEL));
        temp = -1;
        if (!pa_atoi(v, &temp) && temp > 0 && temp <= 5) {
            u->active_mic_channel = temp;
        }
        pa_log_debug("active mic channel is now %d (\"%s\"=%d received)", (int)u->active_mic_channel, v, temp);

        pa_proplist_free(p);
    }

    return PA_HOOK_OK;
}

size_t voice_convert_nbytes(size_t nbytes, const pa_sample_spec *from_ss, const pa_sample_spec *to_ss) {
    size_t frames;

    pa_assert(from_ss);
    pa_assert(to_ss);

    frames = nbytes / pa_frame_size(from_ss);

    /* Convert to the destination sample rate. */
    frames = frames * to_ss->rate / from_ss->rate;

    return frames * pa_frame_size(to_ss);
}

#ifdef EXTRA_DEBUG
/* This code is not finished and probably dont work. */

struct file_table_entry {
    const char *name;
    FILE *file;
};

#define FILE_TABLE_SIZE 16
static const int file_table_size = FILE_TABLE_SIZE;
static struct file_table_entry file_table[FILE_TABLE_SIZE] = { { NULL, NULL } };

void voice_append_chunk_to_file(struct userdata *u, const char *file_name, pa_memchunk *chunk) {
    int i;
    FILE *file = NULL;
    for (i = 0; i < file_table_size && file_table[i].name != NULL; i++) {
        if (0 == strcmp(file_name, file_table[i].name)) {
            file = file_table[i].file;
        }
    }

    if (i >= file_table_size) {
        pa_log("Can't open new files, not writing to file \"%s\"", file_name);
        return;
    }

    if (file == NULL) {
        file = file_table[i].file = fopen(file_name, "w");
        if (file == NULL) {
            pa_log("Can't open file \"%s\": %s", file_name, strerror(errno));
            return;
        }
    }

    char *p = pa_memblock_acquire(chunk->memblock);
    fwrite(p + chunk->index, 1, chunk->length, file);
    pa_memblock_release(chunk->memblock);
}

#endif
