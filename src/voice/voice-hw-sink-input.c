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

#include <stdio.h>
#include <assert.h>
#include <math.h>

#include <pulse/xmalloc.h>

#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/namereg.h>
#include <pulsecore/log.h>
#include <pulsecore/mutex.h>
#include <pulsecore/atomic.h>
#include <pulsecore/semaphore.h>
#include <pulsecore/thread.h>
#include <pulsecore/sample-util.h>

#include "module-voice-userdata.h"
#include "voice-hw-sink-input.h"
#include "voice-util.h"
#include "voice-voip-sink.h"
#include "voice-raw-sink.h"
#include "voice-aep-ear-ref.h"
#include "voice-convert.h"
#include "pa-optimized.h"
#include "optimized.h"
#include "memory.h"
#include "voice-voip-source.h"

#include "module-voice-api.h"
#include "voice-hooks.h"

static unsigned int voice_dl_sideinfo_pop(struct userdata *u, int length) {
    unsigned int spc_flags = 0;

    pa_assert(u);
    pa_assert(length % u->aep_fragment_size == 0);

    while (length) {
        spc_flags = PA_PTR_TO_UINT(pa_queue_pop(u->dl_sideinfo_queue));
        length -= u->aep_fragment_size;
    }

    return spc_flags & ~VOICE_SIDEINFO_FLAG_BOGUS;
}

/* Called from IO thread context. */
static void voice_aep_sink_process(struct userdata *u, pa_memchunk *chunk) {
    unsigned int spc_flags = 0;

    pa_assert(u);

    /*
     * note: HOOK_AEP_DOWNLINK must be called even if chunk is silent
     *       to ensure proper handling of SPEECH->DTX transitions
     */

    if (voice_voip_sink_active_iothread(u)) {
        aep_downlink params;

        pa_sink_render_full(u->voip_sink, u->aep_fragment_size, chunk);
        spc_flags = voice_dl_sideinfo_pop(u, u->aep_fragment_size);

        params.chunk = chunk;
        params.spc_flags = spc_flags;
        /* TODO: get rid of cmt boolean */
        params.cmt = true;

        /* TODO: I think this should be called from behind the hook */
        pa_memchunk_make_writable(chunk, u->aep_fragment_size);

        meego_algorithm_hook_fire(u->hooks[HOOK_AEP_DOWNLINK], &params);
    }
    else {
        pa_silence_memchunk_get(&u->core->silence_cache,
                                u->core->mempool,
                                chunk,
                                &u->aep_sample_spec,
                                u->aep_fragment_size);
    }
}

/*** sink_input callbacks ***/
static int hw_sink_input_pop_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk) {
    struct userdata *u;
    meego_algorithm_hook_data hook_data;
    pa_memchunk aepchunk = { 0, 0, 0 };
    pa_memchunk rawchunk = { 0, 0, 0 };
    pa_volume_t aep_volume = PA_VOLUME_NORM;

    pa_assert(i);
    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);
    pa_assert(chunk);

#ifdef SINK_TIMING_DEBUG_ON
    static struct timeval tv_last = { 0, 0 };
    struct timeval tv_new;
    pa_rtclock_get(&tv_new);
    pa_usec_t ched_period = pa_timeval_diff(&tv_last, &tv_new);
    tv_last = tv_new;
#endif

    /* We only operate with N * u->hw_fragment_size chunks. */
    if (length > u->hw_fragment_size_max)
        length = u->hw_fragment_size_max;
    else if (length % u->hw_fragment_size)
        length += u->hw_fragment_size - (length % u->hw_fragment_size);

    if (u->aep_sink_input && PA_SINK_INPUT_IS_LINKED(
            u->aep_sink_input->thread_info.state)) {
        aep_volume = u->aep_sink_input->thread_info.muted ?
            PA_VOLUME_MUTED : u->aep_sink_input->thread_info.soft_volume.values[0];
    }

    if (voice_voip_sink_active_iothread(u)) {
        if (u->voip_sink->thread_info.rewind_requested)
            pa_sink_process_rewind(u->voip_sink, 0);
        voice_aep_sink_process(u, &aepchunk);
        if (aep_volume != PA_VOLUME_MUTED && !pa_memblock_is_silence(aepchunk.memblock)) {
            if (aep_volume != PA_VOLUME_NORM) {
                pa_memchunk_make_writable(&aepchunk, 0);
                pa_optimized_apply_volume(&aepchunk, aep_volume);
            }
        }
        else if (!pa_memblock_is_silence(aepchunk.memblock)) {
            pa_memblock_unref(aepchunk.memblock);
            pa_silence_memchunk_get(&u->core->silence_cache,
                                    u->core->mempool,
                                    &aepchunk,
                                    &u->aep_sample_spec,
                                    aepchunk.length);
        }
        length = 2*(48/8)*aepchunk.length;
    }

    if (voice_raw_sink_active_iothread(u)) {
        if (u->raw_sink->thread_info.rewind_requested)
            pa_sink_process_rewind(u->raw_sink, 0);

        pa_sink_render_full(u->raw_sink, length, &rawchunk);

        if (pa_atomic_load(&u->mixer_state) == PROP_MIXER_TUNING_ALT &&
            u->alt_mixer_compensation != PA_VOLUME_NORM &&
            !pa_memblock_is_silence(rawchunk.memblock)) {
            pa_memchunk_make_writable(&rawchunk, 0);
            pa_optimized_apply_volume(&rawchunk, u->alt_mixer_compensation);
        }
    }

    if (aepchunk.length > 0 && !pa_memblock_is_silence(aepchunk.memblock)) {
        if (rawchunk.length > 0 && !pa_memblock_is_silence(rawchunk.memblock)) {
#if 1 /* Use only NB IIR EQ and down mix raw sink to mono when in a call */
            pa_memchunk monochunk, stereochunk;
            hook_data.channels = 1;
            hook_data.channel[0] = aepchunk;
            meego_algorithm_hook_fire(u->hooks[HOOK_NARROWBAND_EAR_EQU_MONO], &hook_data);
            aepchunk = hook_data.channel[0];
            voice_convert_run_8_to_48(u, u->aep_to_hw_sink_resampler, &aepchunk, chunk);
            pa_optimized_downmix_to_mono(&rawchunk, &monochunk);
            pa_memblock_unref(rawchunk.memblock);
            pa_memchunk_reset(&rawchunk);
            pa_assert(monochunk.length == chunk->length);
            pa_optimized_equal_mix_in(chunk, &monochunk);
            pa_memblock_unref(monochunk.memblock);
            hook_data.channel[0] = *chunk;
            meego_algorithm_hook_fire(u->hooks[HOOK_XPROT_MONO], &hook_data);
            *chunk = hook_data.channel[0];
            pa_optimized_mono_to_stereo(chunk, &stereochunk);
            pa_memblock_unref(chunk->memblock);
            *chunk = stereochunk;
#else /* Do full stereo processing if the raw and aep inputs are both available */

            voice_convert_run_8_to_48_stereo(u, u->aep_to_hw_sink_resampler, &aepchunk, chunk);
            pa_assert(chunk->length == rawchunk.length);
            pa_optimized_equal_mix_in(chunk, &rawchunk);

            if (meego_algorithm_hook_enabled(u->hooks[HOOK_HW_SINK_PROCESS])) {
                const short *src_bufs[2];
                short *dst;

                pa_memchunk_reset(&hook_data.channel[0]);
                pa_memchunk_reset(&hook_data.channel[1]);
                hook_data.channels = 2;

                pa_optimized_deinterleave_stereo_to_mono(chunk, &hook_data.channel[0], &hook_data.channel[1]);

                meego_algorithm_hook_fire(u->hooks[HOOK_HW_SINK_PROCESS], &hook_data);

                /* interleave */
                dst = pa_memblock_acquire(chunk->memblock);
                dst += chunk->index / sizeof(short);

                src_bufs[0] = pa_memblock_acquire(hook_data.channel[0].memblock);
                src_bufs[1] = pa_memblock_acquire(hook_data.channel[1].memblock);
                interleave_mono_to_stereo(src_bufs, dst, hook_data.channel[0].length / sizeof(short));
                pa_memblock_release(chunk->memblock);
                pa_memblock_release(hook_data.channel[0].memblock);
                pa_memblock_release(hook_data.channel[1].memblock);
                pa_memblock_unref(hook_data.channel[0].memblock);
                pa_memblock_unref(hook_data.channel[1].memblock);
            }
#endif
        } else {
            pa_memchunk stereochunk;
            hook_data.channels = 1;
            hook_data.channel[0] = aepchunk;
            meego_algorithm_hook_fire(u->hooks[HOOK_NARROWBAND_EAR_EQU_MONO], &hook_data);
            aepchunk = hook_data.channel[0];
            voice_convert_run_8_to_48(u, u->aep_to_hw_sink_resampler, &aepchunk, chunk);
            hook_data.channel[0] = *chunk;
            meego_algorithm_hook_fire(u->hooks[HOOK_XPROT_MONO], &hook_data);
            *chunk = hook_data.channel[0];
            pa_optimized_mono_to_stereo(chunk, &stereochunk);
            pa_memblock_unref(chunk->memblock);
            *chunk = stereochunk;
        }
    } else if (rawchunk.length > 0 && !pa_memblock_is_silence(rawchunk.memblock)) {
        *chunk = rawchunk;
        pa_memchunk_reset(&rawchunk);

        if (meego_algorithm_hook_enabled(u->hooks[HOOK_HW_SINK_PROCESS])) {
            const short *src_bufs[2];
            short *dst;

            pa_memchunk_reset(&hook_data.channel[0]);
            pa_memchunk_reset(&hook_data.channel[1]);
            hook_data.channels = 2;

            pa_memchunk_make_writable(chunk, 0);

            pa_optimized_deinterleave_stereo_to_mono(chunk, &hook_data.channel[0], &hook_data.channel[1]);

            meego_algorithm_hook_fire(u->hooks[HOOK_HW_SINK_PROCESS], &hook_data);

            /* interleave */
            dst = pa_memblock_acquire(chunk->memblock);
            dst += chunk->index / sizeof(short);

            src_bufs[0] = pa_memblock_acquire(hook_data.channel[0].memblock);
            src_bufs[1] = pa_memblock_acquire(hook_data.channel[1].memblock);
            interleave_mono_to_stereo(src_bufs, dst, hook_data.channel[0].length / sizeof(short));
            pa_memblock_release(chunk->memblock);
            pa_memblock_release(hook_data.channel[0].memblock);
            pa_memblock_release(hook_data.channel[1].memblock);
            pa_memblock_unref(hook_data.channel[0].memblock);
            pa_memblock_unref(hook_data.channel[1].memblock);
        }

    } else {
        pa_silence_memchunk_get(&u->core->silence_cache,
                                u->core->mempool,
                                chunk,
                                &i->sample_spec,
                                length);
    }
    if (rawchunk.memblock) {
        pa_memblock_unref(rawchunk.memblock);
        pa_memchunk_reset(&rawchunk);
    }
    if (aepchunk.memblock) {
        pa_memblock_unref(aepchunk.memblock);
        pa_memchunk_reset(&aepchunk);
    }

    /* FIXME: voice_voip_source_active_iothread() should only be called from
     * the source's main thread. The solution will possibly be copying the
     * source state in some appropriate place to a variable that can be safely
     * accessed from the sink's IO thread. The fact that we run the IO threads
     * with FIFO scheduling should remove any synchronization issues between
     * two IO threads, though, but this is still wrong in principle. */

    /* FIXME: We should have a local atomic indicator to follow source side activity */
    if (voice_voip_source_active(u)) {
        pa_memchunk earref;
        if (pa_memblock_is_silence(chunk->memblock))
            pa_silence_memchunk_get(&u->core->silence_cache,
                                    u->core->mempool,
                                    &earref,
                                    &u->aep_sample_spec,
                                    chunk->length/(2*(48/8)));
        else
            voice_convert_run_48_stereo_to_8(u, u->ear_to_aep_resampler, chunk, &earref);
        voice_aep_ear_ref_dl(u, &earref);
        pa_memblock_unref(earref.memblock);
    }

#ifdef SINK_TIMING_DEBUG_ON
    pa_rtclock_get(&tv_new);
    pa_usec_t process_delay = pa_timeval_diff(&tv_last, &tv_new);
    printf("%d,%d ", (int)ched_period, (int)process_delay);
    /*
      pa_usec_t latency;
      PA_MSGOBJECT(u->master_sink)->process_msg(PA_MSGOBJECT(u->master_sink), PA_SINK_MESSAGE_GET_LATENCY, &latency, 0, NULL);
      printf("%lld ", latency);
    */
#endif

    return 0;
}

/*** sink_input callbacks ***/
static int hw_sink_input_pop_8k_mono_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk) {
    struct userdata *u;
    bool have_aep_frame = 0;
    bool have_raw_frame = 0;

    pa_assert(i);
    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);
    pa_assert(chunk);

#ifdef SINK_TIMING_DEBUG_ON
    static struct timeval tv_last = { 0, 0 };
    struct timeval tv_new;
    pa_rtclock_get(&tv_new);
    pa_usec_t ched_period = pa_timeval_diff(&tv_last, &tv_new);
    tv_last = tv_new;
#endif

    pa_volume_t aep_volume = PA_VOLUME_NORM;
    if (u->aep_sink_input && PA_SINK_INPUT_IS_LINKED(
            u->aep_sink_input->thread_info.state)) {
        aep_volume = u->aep_sink_input->thread_info.muted ?
            PA_VOLUME_MUTED : u->aep_sink_input->thread_info.soft_volume.values[0];
    }

    if (voice_voip_sink_active_iothread(u)) {
        pa_memchunk ichunk;
        if (u->voip_sink->thread_info.rewind_requested)
            pa_sink_process_rewind(u->voip_sink, 0);
        voice_aep_sink_process(u, &ichunk);
        if (aep_volume != PA_VOLUME_MUTED) {
            *chunk = ichunk;
        }
        else {
            pa_memblock_unref(ichunk.memblock);
            pa_silence_memchunk_get(&u->core->silence_cache,
                                    u->core->mempool,
                                    chunk,
                                    &u->aep_sample_spec,
                                    ichunk.length);
        }
        have_aep_frame = 1;
    }

    if (voice_raw_sink_active_iothread(u)) {
        if (u->raw_sink->thread_info.rewind_requested)
            pa_sink_process_rewind(u->raw_sink, 0);
        if (have_aep_frame) {
            pa_memchunk tchunk, ichunk;
            pa_sink_render_full(u->raw_sink, 2*(48/8)*chunk->length, &tchunk);
            voice_convert_run_48_stereo_to_8(u, u->raw_sink_to_hw8khz_sink_resampler, &tchunk, &ichunk);
            pa_assert(ichunk.length == chunk->length);
            pa_memblock_unref(tchunk.memblock);
            if (!pa_memblock_is_silence(chunk->memblock)) {
                if (aep_volume == PA_VOLUME_NORM)
                    pa_optimized_equal_mix_in(&ichunk, chunk);
                else
                    pa_optimized_mix_in_with_volume(&ichunk, chunk, aep_volume);
            }
            pa_memblock_unref(chunk->memblock);
            *chunk = ichunk;
        }
        else {
            pa_memchunk ichunk;
            pa_sink_render_full(u->raw_sink, u->hw_fragment_size, &ichunk);
            voice_convert_run_48_stereo_to_8(u, u->raw_sink_to_hw8khz_sink_resampler, &ichunk, chunk);
            pa_memblock_unref(ichunk.memblock);
        }
        have_raw_frame = 1;
    } else if (have_aep_frame && aep_volume != PA_VOLUME_NORM &&
               !pa_memblock_is_silence(chunk->memblock)) {
        pa_optimized_apply_volume(chunk, aep_volume);
    }

    if (!have_raw_frame && !have_aep_frame) {
        pa_silence_memchunk_get(&u->core->silence_cache,
                                u->core->mempool,
                                chunk,
                                &i->sample_spec,
                                length);
    }

    /* FIXME: voice_voip_source_active_iothread() should only be called from
     * the source's main thread. The solution will possibly be copying the
     * source state in some appropriate place to a variable that can be safely
     * accessed from the sink's IO thread. The fact that we run the IO threads
     * with FIFO scheduling should remove any synchronization issues between
     * two IO threads, though, but this is still wrong in principle. */
    /* FIXME: We should have a local atomic indicator to follow source side activity */
    if (voice_voip_source_active_iothread(u))
        voice_aep_ear_ref_dl(u, chunk);

#ifdef SINK_TIMING_DEBUG_ON
    pa_rtclock_get(&tv_new);
    pa_usec_t process_delay = pa_timeval_diff(&tv_last, &tv_new);
    printf("%d,%d ", (int)ched_period, (int)process_delay);
    /*
      pa_usec_t latency;
      PA_MSGOBJECT(u->master_sink)->process_msg(PA_MSGOBJECT(u->master_sink), PA_SINK_MESSAGE_GET_LATENCY, &latency, 0, NULL);
      printf("%lld ", latency);
    */
#endif
    return 0;
}

/* Called from I/O thread context */
static void hw_sink_input_process_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;
    
    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!PA_SINK_INPUT_IS_LINKED(i->thread_info.state))
        return;

    if (u->raw_sink && PA_SINK_IS_OPENED(u->raw_sink->thread_info.state)) {
        size_t amount = voice_convert_nbytes(nbytes, &i->sample_spec, &u->raw_sink->sample_spec);

        if (u->raw_sink->thread_info.rewind_nbytes > 0) {
            amount = PA_MIN(u->raw_sink->thread_info.rewind_nbytes, amount);
            u->raw_sink->thread_info.rewind_nbytes = 0;
        }
        pa_sink_process_rewind(u->raw_sink, amount);
    }

    if (u->voip_sink && PA_SINK_IS_OPENED(u->voip_sink->thread_info.state)) {
        size_t amount = voice_convert_nbytes(nbytes, &i->sample_spec, &u->voip_sink->sample_spec);

        if (u->voip_sink->thread_info.rewind_nbytes > 0) {
            amount = PA_MIN(u->voip_sink->thread_info.rewind_nbytes, amount);
            u->voip_sink->thread_info.rewind_nbytes = 0;
        }
        pa_sink_process_rewind(u->voip_sink, amount);

        if (amount > 0)
            voice_aep_ear_ref_loop_reset(u);
    }
}

/* Called from I/O thread context */
static void hw_sink_input_update_max_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!PA_SINK_INPUT_IS_LINKED(i->thread_info.state))
        return;

    if (u->raw_sink && PA_SINK_IS_LINKED(u->raw_sink->thread_info.state))
        pa_sink_set_max_rewind_within_thread(u->raw_sink, voice_convert_nbytes(nbytes, &i->sample_spec, &u->raw_sink->sample_spec));

    if (u->voip_sink && PA_SINK_IS_LINKED(u->voip_sink->thread_info.state))
        pa_sink_set_max_rewind_within_thread(u->voip_sink, voice_convert_nbytes(nbytes, &i->sample_spec, &u->voip_sink->sample_spec));
}

/* Called from I/O thread context */
static void hw_sink_input_update_max_request_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!PA_SINK_INPUT_IS_LINKED(i->thread_info.state))
        return;

    if (u->raw_sink && PA_SINK_IS_LINKED(u->raw_sink->thread_info.state))
        pa_sink_set_max_request_within_thread(u->raw_sink, voice_convert_nbytes(nbytes, &i->sample_spec, &u->raw_sink->sample_spec));

    if (u->voip_sink && PA_SINK_IS_LINKED(u->voip_sink->thread_info.state))
        pa_sink_set_max_request_within_thread(u->voip_sink, voice_convert_nbytes(nbytes, &i->sample_spec, &u->voip_sink->sample_spec));
}

/* Called from I/O thread context */
static void hw_sink_input_update_sink_latency_range_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (u->raw_sink && PA_SINK_IS_LINKED(u->raw_sink->thread_info.state))
        pa_sink_set_latency_range_within_thread(u->raw_sink, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);

    if (u->voip_sink && PA_SINK_IS_LINKED(u->voip_sink->thread_info.state))
        pa_sink_set_latency_range_within_thread(u->voip_sink, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);
}

/* Called from I/O thread context */
static void hw_sink_input_update_sink_fixed_latency_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (u->raw_sink && PA_SINK_IS_LINKED(u->raw_sink->thread_info.state))
        pa_sink_set_fixed_latency_within_thread(u->raw_sink, i->sink->thread_info.fixed_latency);

    if (u->voip_sink && PA_SINK_IS_LINKED(u->voip_sink->thread_info.state))
        pa_sink_set_fixed_latency_within_thread(u->voip_sink, i->sink->thread_info.fixed_latency);
}

/* Called from I/O thread context */
static void hw_sink_input_detach_slave_sink(pa_sink *sink) {
    if (sink && PA_SINK_IS_LINKED(sink->thread_info.state)) {
        pa_sink_detach_within_thread(sink);
        pa_sink_set_rtpoll(sink, NULL);
        voice_sink_inputs_may_move(sink, false);
    }
}

/* Called from I/O thread context */
static void hw_sink_input_detach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    u->master_sink = NULL;

    hw_sink_input_detach_slave_sink(u->raw_sink);
    hw_sink_input_detach_slave_sink(u->voip_sink);

    pa_log_debug("Detach called");
}

/* Called from I/O thread context */
static void hw_sink_input_attach_slave_sink(struct userdata *u, pa_sink *sink, pa_sink *to_sink) {
    pa_assert(u);
    pa_assert(to_sink);

    if (sink && PA_SINK_IS_LINKED(sink->thread_info.state)) {

        /* FIXME: This should be done by the core. */
        pa_sink_set_rtpoll(sink, to_sink->thread_info.rtpoll);

        voice_sink_inputs_may_move(sink, true);
        if (to_sink->flags & PA_SINK_DYNAMIC_LATENCY)
            pa_sink_set_latency_range_within_thread(sink, to_sink->thread_info.min_latency,
                                                    to_sink->thread_info.max_latency);
        else
            pa_sink_set_fixed_latency_within_thread(sink, to_sink->thread_info.fixed_latency);
        pa_sink_set_max_request_within_thread(sink, to_sink->thread_info.max_request);
        pa_sink_set_max_rewind_within_thread(sink, to_sink->thread_info.max_rewind);
        pa_log_debug("%s (flags=0x%04x) updated min_l=%" PRIu64 " max_l=%" PRIu64 " fixed_l=%" PRIu64 " max_req=%zu max_rew=%zu",
                     sink->name, sink->flags,
                     sink->thread_info.min_latency, sink->thread_info.max_latency,
                     sink->thread_info.fixed_latency, sink->thread_info.max_request,
                     sink->thread_info.max_rewind);
        /* The order is important here. This should be called last: */
        pa_sink_attach_within_thread(sink);
    }
}

/* Called from I/O thread context */
static void hw_sink_input_attach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    hw_sink_input_attach_slave_sink(u, u->raw_sink, i->sink);
    hw_sink_input_attach_slave_sink(u, u->voip_sink, i->sink);

    pa_log_debug("Attach called, new master %p %s", (void*)u->master_sink, u->master_sink->name);

    voice_aep_ear_ref_loop_reset(u);
}

/* Called from main context */
static void hw_sink_input_kill_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_log_debug("Kill called");

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_unlink(u->voip_sink);
    pa_sink_unlink(u->raw_sink);

    pa_sink_input_unlink(u->hw_sink_input);

    pa_sink_unref(u->voip_sink);
    u->voip_sink = NULL;
    pa_sink_unref(u->raw_sink);
    u->raw_sink = NULL;

    /* FIXME: this is sort-of understandable with the may_move hack... we avoid abort in free() here */
    u->hw_sink_input->thread_info.attached = false;
    pa_sink_input_unref(u->hw_sink_input);
    u->hw_sink_input = NULL;
}

/* Called from main thread context */
static void hw_sink_input_update_slave_sink(struct userdata *u, pa_sink *sink, pa_sink *to_sink) {
    pa_proplist *p;
    pa_sink_input *i;
    uint32_t idx;
    pa_assert(sink);

    if (!to_sink) {
        pa_sink_set_asyncmsgq(sink, NULL);

        return;
    }

    pa_sink_set_asyncmsgq(sink, to_sink->asyncmsgq);
    pa_sink_update_flags(sink, PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY, to_sink->flags);

    p = pa_proplist_new();
    pa_proplist_setf(p, PA_PROP_DEVICE_DESCRIPTION, "%s connected to %s", sink->name, u->master_sink->name);

    /* FIXME: This should be done by the core. */
    pa_proplist_sets(p, PA_PROP_DEVICE_MASTER_DEVICE, u->master_sink->name);

    pa_sink_update_proplist(sink, PA_UPDATE_REPLACE, p);
    pa_proplist_free(p);

    /* Call moving callbacks of slave sink's sink-inputs. FIXME: This shouldn't
     * be needed after asyncmsgq updating is moved to the core. */
    PA_IDXSET_FOREACH(i, sink->inputs, idx)
        if (i->moving)
            i->moving(i, sink);
}

/* Called from main context */
static void hw_sink_input_moving_cb(pa_sink_input *i, pa_sink *dest){
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_log_debug("Sink input moving to %s", dest ? dest->name : "(null)");

    u->master_sink = dest;

    hw_sink_input_update_slave_sink(u, u->voip_sink, dest);
    hw_sink_input_update_slave_sink(u, u->raw_sink, dest);

    if (!dest)
        return;

    /* reinit tuning paramter moved to move_finish_cb:
       voice_sink_proplist_update(u, dest);

       you cannot change route here, or it may cause crash when
       restoring route volume, because i->sink is NULL or one of the
       sink->asyncmsq is NULL at that time */

    if ((i->sample_spec.rate == VOICE_SAMPLE_RATE_AEP_HZ &&
         dest->sample_spec.rate != VOICE_SAMPLE_RATE_AEP_HZ) ||
        (i->sample_spec.rate != VOICE_SAMPLE_RATE_AEP_HZ &&
         dest->sample_spec.rate == VOICE_SAMPLE_RATE_AEP_HZ)) {
        pa_log_info("Reinitialize due to samplerate change %d->%d.",
                    i->sample_spec.rate, dest->sample_spec.rate);
        pa_log_debug("New sink format %s", pa_sample_format_to_string(dest->sample_spec.format)) ;
        pa_log_debug("New sink rate %d", dest->sample_spec.rate);
        pa_log_debug("New sink channels %d", dest->sample_spec.channels);

        voice_reinit_hw_sink_input(u);
    }
}

static bool hw_sink_input_may_move_to_cb(pa_sink_input *i, pa_sink *dest) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (u->master_sink == NULL)
        return true;

    return ((u->master_sink != dest) && (u->master_sink->asyncmsgq != dest->asyncmsgq));
}

static pa_hook_result_t hw_sink_input_move_fail_cb(pa_core *c, pa_sink_input *i, struct userdata *u) {
    const char *master_sink;
    pa_sink *s = NULL;

    pa_assert(u);
    pa_sink_input_assert_ref(i);

    if (i != u->hw_sink_input)
        return PA_HOOK_OK;

    master_sink = pa_modargs_get_value(u->modargs, "master_sink", NULL);

    if (!master_sink
        || !(s = pa_namereg_get(u->core, master_sink, PA_NAMEREG_SINK))) {

        pa_log_debug("Master sink \"%s\" not found", master_sink);
        return PA_HOOK_OK;
    }

    if (pa_sink_input_finish_move(i, s, true) >= 0)
        return PA_HOOK_STOP;

    pa_log("Failed to fallback on \"%s\".", master_sink);

    /* ok fallback on destroying the hw_sink_input (voice module will be unloaded) */
    return PA_HOOK_OK;
}

static pa_sink_input *voice_hw_sink_input_new(struct userdata *u, pa_sink_input_flags_t flags) {
    pa_sink_input_new_data sink_input_data;
    pa_sink_input *new_sink_input;
    char t[256];

    pa_assert(u);
    pa_assert(u->master_sink);
    ENTER();

    snprintf(t, sizeof(t), VOICE_MASTER_SINK_INPUT_NAME);

    pa_sink_input_new_data_init(&sink_input_data);
    sink_input_data.flags = flags;
    sink_input_data.driver = __FILE__;
    sink_input_data.module = u->module;
    sink_input_data.sink = u->master_sink;
    sink_input_data.origin_sink = u->raw_sink;

    pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_NAME, t);
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_APPLICATION_NAME, t); /* this is the default value used by PA modules */
    if (u->master_sink->sample_spec.rate == VOICE_SAMPLE_RATE_AEP_HZ) {
        pa_sink_input_new_data_set_sample_spec(&sink_input_data, &u->aep_sample_spec);
        pa_sink_input_new_data_set_channel_map(&sink_input_data, &u->aep_channel_map);
    }
    else {
        pa_sink_input_new_data_set_sample_spec(&sink_input_data, &u->hw_sample_spec);
        pa_sink_input_new_data_set_channel_map(&sink_input_data, &u->stereo_map);
    }

    pa_sink_input_new(&new_sink_input, u->core, &sink_input_data);
    pa_sink_input_new_data_done(&sink_input_data);

    if (!new_sink_input) {
        pa_log_warn("Creating sink input failed");
        return NULL;
    }

    u->master_sink = new_sink_input->sink;

    if (u->master_sink->sample_spec.rate == VOICE_SAMPLE_RATE_AEP_HZ)
        new_sink_input->pop = hw_sink_input_pop_8k_mono_cb;
    else
        new_sink_input->pop = hw_sink_input_pop_cb;
    new_sink_input->process_rewind = hw_sink_input_process_rewind_cb;
    new_sink_input->update_max_rewind = hw_sink_input_update_max_rewind_cb;
    new_sink_input->update_max_request = hw_sink_input_update_max_request_cb;
    new_sink_input->update_sink_latency_range = hw_sink_input_update_sink_latency_range_cb;
    new_sink_input->update_sink_fixed_latency = hw_sink_input_update_sink_fixed_latency_cb;
    new_sink_input->kill = hw_sink_input_kill_cb;
    new_sink_input->attach = hw_sink_input_attach_cb;
    new_sink_input->detach = hw_sink_input_detach_cb;
    new_sink_input->moving = hw_sink_input_moving_cb;
    new_sink_input->may_move_to = hw_sink_input_may_move_to_cb;
    new_sink_input->userdata = u;

    return new_sink_input;
}

int voice_init_hw_sink_input(struct userdata *u) {
    pa_assert(u);

    u->hw_sink_input = voice_hw_sink_input_new(u, 0);
    pa_return_val_if_fail (u->hw_sink_input, -1);

    u->hw_sink_input_move_fail_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FAIL], PA_HOOK_EARLY, (pa_hook_cb_t) hw_sink_input_move_fail_cb, u);

    return 0;
}

struct voice_hw_sink_input_reinit_defered {
    struct userdata *u;
    pa_defer_event *defer;
};

static void voice_hw_sink_input_reinit_defer_cb(pa_mainloop_api *m, pa_defer_event *de, void *userdata) {
    struct voice_hw_sink_input_reinit_defered *d;
    pa_sink_input *new_si, *old_si;
    struct userdata *u;
    bool start_uncorked;

    pa_assert_se(d = userdata);
    pa_assert_se(u = d->u);
    pa_assert_se(old_si = u->hw_sink_input);

    m->defer_free(d->defer);
    pa_xfree(d);
    d = NULL;

    start_uncorked = PA_SINK_IS_OPENED(u->raw_sink->state) ||
        PA_SINK_IS_OPENED(u->voip_sink->state) ||
        old_si->state != PA_SINK_INPUT_CORKED;
    pa_log("HWSI START UNCORKED: %d", start_uncorked);

    new_si = voice_hw_sink_input_new(u, start_uncorked ? 0 : PA_SINK_INPUT_START_CORKED);
    pa_return_if_fail(new_si);

    pa_sink_update_flags(u->raw_sink, PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY, new_si->sink->flags);

    pa_sink_input_cork(old_si, true);

    pa_log_debug("reinitialize hw sink-input %s %p", u->master_sink->name, (void*)new_si);

    u->hw_sink_input = new_si;
    u->raw_sink->input_to_master = new_si;
    pa_sink_input_put(u->hw_sink_input);

    pa_log_debug("Detaching the old sink input %p", (void*)old_si);

    old_si->detach = NULL;
    pa_sink_input_unlink(old_si);
    pa_sink_input_unref(old_si);

    voice_aep_ear_ref_loop_reset(u);
}

void voice_reinit_hw_sink_input(struct userdata *u) {
    struct voice_hw_sink_input_reinit_defered *d;
    pa_assert(u);

    d = pa_xnew0(struct voice_hw_sink_input_reinit_defered, 1); /* in theory should be tracked if pulseaudio exit before defer called (could be added to userdata, but would need to be queued */
    d->u = u;
    d->defer = u->core->mainloop->defer_new(u->core->mainloop, voice_hw_sink_input_reinit_defer_cb, d);
}
