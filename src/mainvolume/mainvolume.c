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

pa_volume_t mv_step_value(struct mv_volume_steps *s, unsigned step) {
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
 * returns found step or -1 if not found
 */
int mv_search_step(int *steps, int n_steps, int vol) {
    int sel = 0;

    int low = 0;
    int high = n_steps;
    int mid;

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

void mv_normalize_steps(struct mv_volume_steps *steps) {
    unsigned i = 0;

    pa_assert(steps);
    pa_assert(steps->n_steps > 0);

    /* if first step is less than equal to -20000 mB (PA_DECIBEL_MININFTY if
     * INFINITY is not defined), set it directly to PA_VOLUME_MUTED */
    if (steps->step[0] <= -20000) {
        steps->step[0] = PA_VOLUME_MUTED;
        i = 1;
    }

    /* convert mB step values to software volume values.
     * divide mB values by 100.0 to get dB */
    for (; i < steps->n_steps; i++) {
        double value = (double)steps->step[i];
        steps->step[i] = pa_sw_volume_from_dB(value / 100.0);
    }
}

int mv_parse_single_steps(struct mv_volume_steps *steps, const char *step_string) {
    int len;
    int count = 0;
    int i = 0;

    pa_assert(steps);
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
            return -1;

        /* increment i by one to get to the start of value */
        i++;

        /* search for next step:value pair separator to determine value string length */
        start = i;
        for (; i < len && step_string[i] != ','; i++);
        value_len = i - start;

        if (value_len < 1 || value_len > sizeof(step)-1)
            return -1;

        /* copy value string part to step string and convert to integer */
        memcpy(step, &step_string[start], value_len);
        step[value_len] = '\0';

        if (pa_atoi(step, &value)) {
            return -1;
        }
        steps->step[count] = value;

        count++;
    }

    steps->n_steps = count;
    steps->current_step = 0;

    return count;
}

static int parse_high_volume_step(struct mv_volume_steps_set *set, const char *high_volume) {
    int step;

    pa_assert(set);

    if (!high_volume)
        return -1;

    if (pa_atoi(high_volume, &step)) {
        pa_log_warn("Failed to parse high volume step \"%s\"", high_volume);
        return -1;
    }

    if (step > set->media.n_steps - 1) {
        pa_log_warn("High volume step %d over bounds (max value %u", step, set->media.n_steps - 1);
        return -1;
    }

    return step;
}

int mv_parse_steps(struct mv_userdata *u,
                   const char *route,
                   const char *step_string_call,
                   const char *step_string_media,
                   const char *high_volume) {
    int count1 = 0;
    int count2 = 0;
    struct mv_volume_steps_set *set;
    struct mv_volume_steps call_steps;
    struct mv_volume_steps media_steps;

    pa_assert(u);
    pa_assert(u->steps);
    pa_assert(route);

    if (!step_string_call || !step_string_media) {
        return 0;
    }

    count1 = mv_parse_single_steps(&call_steps, step_string_call);
    if (count1 < 1) {
        pa_log_warn("failed to parse call steps; %s", step_string_call);
        return -1;
    }
    mv_normalize_steps(&call_steps);

    count2 = mv_parse_single_steps(&media_steps, step_string_media);
    if (count2 < 1) {
        pa_log_warn("failed to parse media steps; %s", step_string_media);
        return -1;
    }
    mv_normalize_steps(&media_steps);

    set = pa_xnew0(struct mv_volume_steps_set, 1);
    set->route = pa_xstrdup(route);
    set->call = call_steps;
    set->media = media_steps;
    set->high_volume_step = parse_high_volume_step(set, high_volume);
    set->first = true;

    pa_log_debug("adding %d call and %d media steps with route %s",
                 set->call.n_steps,
                 set->media.n_steps,
                 set->route);
    if (set->high_volume_step > -1)
        pa_log_debug("setting media high volume step %d", set->high_volume_step);

    pa_hashmap_put(u->steps, set->route, set);

    return count1 + count2;
}

int mv_safe_step(struct mv_userdata *u) {
    pa_assert(u);

    if (u->call_active)
        return -1;

    if (u->current_steps)
        return u->current_steps->high_volume_step - 1;

    return -1;
}

bool mv_high_volume(struct mv_userdata *u) {
    pa_assert(u);

    if (u->call_active)
        return false;

    if (u->current_steps
        && u->current_steps->high_volume_step > -1
        && u->current_steps->media.current_step >= u->current_steps->high_volume_step)
        return true;
    else
        return false;
}

bool mv_has_high_volume(struct mv_userdata *u) {
    pa_assert(u);

    if (u->call_active || !u->notifier.mode_active)
        return false;

    if (u->current_steps && u->current_steps->high_volume_step > -1)
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
