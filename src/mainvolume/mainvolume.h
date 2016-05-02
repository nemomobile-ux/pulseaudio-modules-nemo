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

#ifndef foomainvolumehfoo
#define foomainvolumehfoo

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/core.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/protocol-dbus.h>
#include <pulsecore/hook-list.h>
#include <pulsecore/strlist.h>

#include "shared-data.h"
#include "volume-proxy.h"
#include "listening-watchdog.h"

#define MEDIA_STREAM "sink-input-by-media-role:x-maemo"
#define CALL_STREAM "sink-input-by-media-role:phone"
#define MAX_STEPS (64)

typedef enum media_state {
    MEDIA_INACTIVE,
    MEDIA_FOREGROUND,
    MEDIA_BACKGROUND,
    MEDIA_ACTIVE,
    MEDIA_MAX
} media_state_t;

struct mv_volume_steps {
    pa_volume_t step[MAX_STEPS];
    uint32_t n_steps;
    uint32_t current_step;
};

struct mv_volume_steps_set {
    char *route;

    struct mv_volume_steps call;
    struct mv_volume_steps media;
    bool has_high_volume_step;
    uint32_t high_volume_step;
    /* when parsing volume steps first is set true,
     * and if entering a route with volume higher than high_volume_step,
     * volume is reset to safe volume.
     * This is done once per parsed steps, first is set
     * to false after first check. */
    bool first;
};

struct mv_userdata {
    pa_core *core;
    pa_module *module;
    bool tuning_mode;
    bool virtual_stream;

    pa_sink_input *virtual_sink_input;

    pa_hashmap *steps;
    struct mv_volume_steps_set *current_steps;
    char *route;

    pa_shared_data *shared;
    pa_hook_slot *call_state_hook_slot;
    pa_hook_slot *media_state_hook_slot;
    bool call_active;

    pa_volume_proxy *volume_proxy;
    pa_hook_slot *volume_proxy_slot;

    pa_hook_slot *sink_proplist_changed_slot;

    pa_time_event *signal_time_event;
    pa_usec_t last_signal_timestamp;
    pa_usec_t last_step_set_timestamp;

    pa_dbus_protocol *dbus_protocol;
    char *dbus_path;

    struct notifier_data {
        mv_listening_watchdog *watchdog;
        pa_hook_slot *sink_input_put_slot;
        pa_hook_slot *sink_input_changed_slot;
        pa_hook_slot *sink_input_unlink_slot;
        uint32_t timeout;
        /* Modes that are watched. */
        pa_hashmap *modes;
        bool mode_active;

        /* Roles for sink-inputs that are watched. */
        pa_hashmap *roles;

        /* Currently existing sink-inputs matching with roles.
         * key: sink-input-object data: uint32_t bit flag for sink-input
         * When sink-input is in playing state, si's flag is applied to enabled_slots
         * and vice-versa. */
        pa_hashmap *sink_inputs;
        uint32_t enabled_slots; /* bit slots that are enabled */
        uint32_t free_slots;    /* 1 means free, 0 means taken. */

        bool streams_active;                /* Active media streams. */
        media_state_t policy_media_state;   /* Media state from policy enforcement point of view. */
        media_state_t media_state;          /* Media state combined from active streams and policy state. */
    } notifier;
};

/* return either "media" or "call" volume steps struct based on whether
 * call is currently active */
struct mv_volume_steps* mv_active_steps(struct mv_userdata *u);

/* set new step as current step.
 * returns true if new step differs from current step.
 */
bool mv_set_step(struct mv_userdata *u, uint32_t step);

/* get value of step from steps. */
pa_volume_t mv_step_value(struct mv_volume_steps *s, uint32_t step);

/* get value of step from currently active steps. */
pa_volume_t mv_current_step_value(struct mv_userdata *u);

/* search for step with volume vol.
 * returns found step
 */
uint32_t mv_search_step(pa_volume_t *steps, uint32_t n_steps, pa_volume_t vol);

/* normalize mdB values to linear values */
void mv_normalize_steps(struct mv_volume_steps *steps);

/* parse step values for route from step_string_call and step_string_media.
 * after successfull parsing of both strings, new filled struct mv_volume_steps_set is
 * added to hashmap with route as key.
 * return true if parsing of both media and call steps was successful.
 */
bool mv_parse_steps(struct mv_userdata *u,
                    const char *route,
                    const char *step_string_call,
                    const char *step_string_media,
                    const char *high_volume); /* high_volume can be NULL */

/* Return highest step safe for listening with headphones. */
uint32_t mv_safe_step(struct mv_userdata *u);

/* Return true if current media step is same or over high volume step. */
bool mv_high_volume(struct mv_userdata *u);

/* Return true if currently active media steps have high volume step defined. */
bool mv_has_high_volume(struct mv_userdata *u);

/* Update notifier state based on route. */
void mv_notifier_update_route(struct mv_userdata *u, const char *route);
bool mv_notifier_active(struct mv_userdata *u);


bool mv_media_state_from_string(const char *str, media_state_t *state);
const char *mv_media_state_from_enum(media_state_t state);

#endif
