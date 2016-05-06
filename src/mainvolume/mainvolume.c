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

#include <pulsecore/core.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/hashmap.h>

#include "call-state-tracker.h"
#include "volume-proxy.h"
#include "proplist-nemo.h"

#include "mainvolume.h"

void mv_volume_steps_set_free(struct mv_volume_steps_set *set) {
    pa_assert(set);

    pa_xfree(set->route);
    pa_xfree(set->call.step);
    pa_xfree(set->media.step);
    pa_xfree(set);
}

struct mv_volume_steps* mv_active_steps(struct mv_userdata *u) {
    pa_assert(u);
    pa_assert(u->current_steps);

    if (u->call_active)
        return &u->current_steps->call;
    else
        return &u->current_steps->media;
}

bool mv_set_step(struct mv_userdata *u, unsigned step) {
    struct mv_volume_steps *s;
    bool changed = false;
    pa_assert(u);

    s = mv_active_steps(u);

    pa_assert(s);
    pa_assert(step < s->n_steps);

    if (s->current_step != step) {
        pa_log_debug("set current step to %d", step);
        s->current_step = step;
        changed = true;
    }

    return changed;
}

pa_volume_t mv_step_value(struct mv_volume_steps *s, uint32_t step) {
    pa_assert(s);

    return s->step[step];
}

pa_volume_t mv_current_step_value(struct mv_userdata *u) {
    struct mv_volume_steps *s;

    pa_assert(u);

    s = mv_active_steps(u);
    return mv_step_value(s, s->current_step);
}

/* otherwise basic binary search except that exact value is not checked,
 * so that we can search by volume range.
 * returns found step
 */
uint32_t mv_search_step(pa_volume_t *steps, uint32_t n_steps, pa_volume_t vol) {
    uint32_t sel = 0;

    uint32_t low = 0;
    uint32_t high = n_steps;
    uint32_t mid;

    while (low < high) {
        mid = low + ((high-low)/2);
        if (steps[mid] < vol)
            low = mid + 1;
        else
            high = mid;
    }

    /* check only that our search is valid, don't check
     * for exact value, so that we get step by range */
    if (low < n_steps)
        sel = low;
    else
        /* special case when volume is more than volume in last
         * step, we select the last ("loudest") step */
        sel = n_steps - 1;

    return sel;
}

static void normalize_steps(struct mv_volume_steps *steps, int32_t *steps_mB, uint32_t count) {
    uint32_t i = 0;

    pa_assert(steps);
    pa_assert(count > 0);

    steps->n_steps = count;
    steps->current_step = 0;

    /* if first step is less than equal to -20000 mB (PA_DECIBEL_MININFTY if
     * INFINITY is not defined), set it directly to PA_VOLUME_MUTED */
    if (steps_mB[0] <= -20000) {
        steps->step[0] = PA_VOLUME_MUTED;
        i = 1;
    }

    /* convert mB step values to software volume values.
     * divide mB values by 100.0 to get dB */
    for (; i < steps->n_steps; i++) {
        double value = (double) steps_mB[i];
        steps->step[i] = pa_sw_volume_from_dB(value / 100.0);
    }
}

static uint32_t parse_single_steps(int32_t *steps_mB, const char *step_string) {
    uint32_t len;
    uint32_t count = 0;
    uint32_t i = 0;

    pa_assert(steps_mB);
    if (!step_string)
        return 0;

    len = strlen(step_string);

    while (i < len && count < MAX_STEPS) {
        char step[16];
        int value;
        size_t start, value_len;

        /* search for next step:value separator */
        for (; i < len && step_string[i] != ':'; i++);

        /* invalid syntax in step string, bail out */
        if (i == len)
            return 0;

        /* increment i by one to get to the start of value */
        i++;

        /* search for next step:value pair separator to determine value string length */
        start = i;
        for (; i < len && step_string[i] != ','; i++);
        value_len = i - start;

        if (value_len < 1 || value_len > sizeof(step)-1)
            return 0;

        /* copy value string part to step string and convert to integer */
        memcpy(step, &step_string[start], value_len);
        step[value_len] = '\0';

        if (pa_atoi(step, &value)) {
            return 0;
        }
        steps_mB[count] = value;

        count++;
    }

    return count;
}

static bool parse_high_volume_step(struct mv_volume_steps_set *set, const char *high_volume,
                                   bool *has_high_volume, uint32_t *high_volume_step) {
    int step;

    pa_assert(set);

    *has_high_volume = false;
    *high_volume_step = 0;

    if (!high_volume)
        return false;

    if (pa_atoi(high_volume, &step)) {
        pa_log_warn("Failed to parse high volume step \"%s\"", high_volume);
        return false;
    }

    if (step < 1) {
        pa_log_warn("Minimum high volume step is 1.");
        return false;
    }

    if ((uint32_t) step > set->media.n_steps - 1) {
        pa_log_warn("High volume step %d over bounds (max value %u", step, set->media.n_steps - 1);
        return false;
    }

    *has_high_volume = true;
    *high_volume_step = (uint32_t) step;

    return true;
}

bool mv_parse_steps(struct mv_userdata *u,
                    const char *route,
                    const char *step_string_call,
                    const char *step_string_media,
                    const char *high_volume) {
    struct mv_volume_steps_set *set = NULL;
    int32_t call_steps_mB[MAX_STEPS];
    int32_t media_steps_mB[MAX_STEPS];
    uint32_t call_steps_count;
    uint32_t media_steps_count;

    pa_assert(u);
    pa_assert(u->steps);
    pa_assert(route);

    if (!step_string_call || !step_string_media) {
        goto fail;
    }

    set = pa_xnew0(struct mv_volume_steps_set, 1);

    call_steps_count = parse_single_steps(call_steps_mB, step_string_call);
    if (call_steps_count < 1) {
        pa_log_warn("failed to parse call steps; %s", step_string_call);
        goto fail;
    }
    set->call.step = pa_xmalloc(sizeof(pa_volume_t) * call_steps_count);
    normalize_steps(&set->call, call_steps_mB, call_steps_count);

    media_steps_count = parse_single_steps(media_steps_mB, step_string_media);
    if (media_steps_count < 1) {
        pa_log_warn("failed to parse media steps; %s", step_string_media);
        goto fail;
    }
    set->media.step = pa_xmalloc(sizeof(pa_volume_t) * media_steps_count);
    normalize_steps(&set->media, media_steps_mB, media_steps_count);

    set->route = pa_xstrdup(route);
    set->first = true;

    pa_log_debug("adding %d call and %d media steps with route %s",
                 set->call.n_steps,
                 set->media.n_steps,
                 set->route);
    if (parse_high_volume_step(set, high_volume, &set->has_high_volume_step, &set->high_volume_step))
        pa_log_debug("setting media high volume step %d", set->high_volume_step);

    pa_hashmap_put(u->steps, set->route, set);

    return true;

fail:
    if (set)
        mv_volume_steps_set_free(set);

    return false;
}

uint32_t mv_safe_step(struct mv_userdata *u) {
    pa_assert(u);
    pa_assert(!u->call_active);
    pa_assert(u->current_steps);
    pa_assert(u->current_steps->has_high_volume_step);

    return u->current_steps->high_volume_step - 1;
}

bool mv_has_high_volume(struct mv_userdata *u) {
    pa_assert(u);

    if (u->call_active || !u->notifier.mode_active)
        return false;

    if (u->current_steps && u->current_steps->has_high_volume_step)
        return true;
    else
        return false;
}

void mv_notifier_update_route(struct mv_userdata *u, const char *route)
{
    pa_assert(u);
    pa_assert(route);
    pa_assert(u->notifier.modes);

    if (pa_hashmap_get(u->notifier.modes, u->route))
        u->notifier.mode_active = true;
    else
        u->notifier.mode_active = false;
}

bool mv_notifier_active(struct mv_userdata *u)
{
    pa_assert(u);

    if (u->notifier.mode_active && u->notifier.enabled_slots && !u->call_active)
        return true;
    else
        return false;
}

struct media_state_map {
    media_state_t state;
    const char *str;
};

static struct media_state_map media_states[MEDIA_MAX] = {
    { MEDIA_INACTIVE,   PA_NEMO_PROP_MEDIA_STATE_INACTIVE   },
    { MEDIA_FOREGROUND, PA_NEMO_PROP_MEDIA_STATE_FOREGROUND },
    { MEDIA_BACKGROUND, PA_NEMO_PROP_MEDIA_STATE_BACKGROUND },
    { MEDIA_ACTIVE,     PA_NEMO_PROP_MEDIA_STATE_ACTIVE     }
};

bool mv_media_state_from_string(const char *str, media_state_t *state) {
    uint32_t i;
    for (i = 0; i < MEDIA_MAX; i++) {
        if (pa_streq(media_states[i].str, str)) {
            *state = media_states[i].state;
            return true;
        }
    }

    return false;
}

const char *mv_media_state_from_enum(media_state_t state) {
    pa_assert(state < MEDIA_MAX);

    return media_states[state].str;
}
