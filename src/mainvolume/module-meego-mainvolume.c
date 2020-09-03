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
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/core-util.h>
#include <pulsecore/protocol-dbus.h>
#include <pulsecore/dbus-util.h>
#include <pulsecore/hook-list.h>
#include <pulsecore/idxset.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/conf-parser.h>
#include <pulse/timeval.h>
#include <pulse/rtclock.h>

#include "module-meego-mainvolume-symdef.h"
#include "mainvolume.h"
#include "listening-watchdog.h"

#include "meego/parameter-hook.h"
#include "meego/proplist-meego.h"
#include "meego/proplist-nemo.h"
#include "meego/shared-data.h"
#include "meego/volume-proxy.h"
#include "sailfishos/defines.h"

PA_MODULE_AUTHOR("Juho Hämäläinen");
PA_MODULE_DESCRIPTION("Nokia mainvolume module");
PA_MODULE_USAGE("tuning_mode=<true/false> defaults to false "
                "virtual_stream=<true/false> create virtual stream for voice call volume control (default false) "
                "listening_time_notifier_conf=<file location for listening time notifier configuration> "
                "mute_routing=<true/false> apply muting to media streams when volumes are out of sync (default true) "
                "unmute_delay=<time in ms> time to keep media streams muted after volumes are in sync (default 50)");
PA_MODULE_VERSION(PACKAGE_VERSION);

static const char* const valid_modargs[] = {
    "tuning_mode",
    "virtual_stream",
    "listening_time_notifier_conf",
    "mute_routing",
    "unmute_delay",
    NULL,
};

static void dbus_init(struct mv_userdata *u);
static void dbus_done(struct mv_userdata *u);
static void dbus_signal_steps(struct mv_userdata *u);
static void dbus_signal_listening_notifier(struct mv_userdata *u, uint32_t listening_time);
static void dbus_signal_high_volume(struct mv_userdata *u, uint32_t safe_step);
static void dbus_signal_call_status(struct mv_userdata *u);
static void dbus_signal_media_state(struct mv_userdata *u);

static void check_notifier(struct mv_userdata *u);

#define DEFAULT_LISTENING_NOTIFIER_CONF_FILE PA_DEFAULT_CONFIG_DIR PA_PATH_SEP "mainvolume-listening-time-notifier.conf"

#define PROP_CALL_STEPS "x-nemo.mainvolume.call"
#define PROP_VOIP_STEPS "x-nemo.mainvolume.voip"
#define PROP_MEDIA_STEPS "x-nemo.mainvolume.media"
#define PROP_HIGH_VOLUME "x-nemo.mainvolume.high-volume-step"

/* If multiple step change calls are coming in succession, wait SIGNAL_WAIT_TIME before
 * sending change signal. */
#define SIGNAL_WAIT_TIME ((pa_usec_t)(0.5 * PA_USEC_PER_SEC))

#define DEFAULT_MUTE_ROUTING (true)
#define DEFAULT_VOLUME_SYNC_DELAY_MS (50)

static void signal_steps(struct mv_userdata *u);

static void signal_timer_stop(struct mv_userdata *u) {
    if (u->signal_time_event) {
        u->core->mainloop->time_free(u->signal_time_event);
        u->signal_time_event = NULL;
    }
}

static void signal_time_callback(pa_mainloop_api *a, pa_time_event *e, const struct timeval *t, void *userdata) {
    struct mv_userdata *u = (struct mv_userdata*)userdata;

    pa_assert(a);
    pa_assert(e);
    pa_assert(u);
    pa_assert(e == u->signal_time_event);

    signal_timer_stop(u);

    /* try signalling current steps again */
    signal_steps(u);
}

static void signal_timer_set(struct mv_userdata *u, const pa_usec_t time) {
    pa_assert(u);

    /* only set time event if none currently pending */
    if (!u->signal_time_event)
        u->signal_time_event = pa_core_rttime_new(u->core, time, signal_time_callback, u);
}

static void check_and_signal_high_volume(struct mv_userdata *u) {
    pa_assert(u);

     /* We need to signal 0 when
     *   - call active
     *   - route is not in mode-list
     * otherwise signal safe step
     */
    if (mv_has_high_volume(u))
        dbus_signal_high_volume(u, mv_safe_step(u));
    else
        dbus_signal_high_volume(u, 0);
}

static void signal_steps(struct mv_userdata *u) {
    pa_usec_t now;

    now = pa_rtclock_now();

    /* if we haven't sent ack signal for a long time, send initial reply
     * immediately */
    if (now - u->last_signal_timestamp >= SIGNAL_WAIT_TIME) {
        signal_timer_stop(u);
        dbus_signal_steps(u);
        return;
    }

    /* Then if new set step events come really frequently, wait until step events stop before
     * signaling */
    if (now - u->last_step_set_timestamp >= SIGNAL_WAIT_TIME) {
        signal_timer_stop(u);
        dbus_signal_steps(u);
        return;
    } else
        /* keep last signal timestamp reseted so signals aren't sent every SIGNAL_WAIT_TIME */
        u->last_signal_timestamp = now;

    signal_timer_set(u, now + SIGNAL_WAIT_TIME);
}

static void sink_input_kill_cb(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);

    pa_sink_input_unlink(i);
    pa_sink_input_unref(i);
}

/* no-op */
static int sink_input_pop_cb(pa_sink_input *i, size_t nbytes, pa_memchunk *chunk) {
    return 0;
}

/* no-op */
static void sink_input_process_rewind_cb(pa_sink_input *i, size_t nbytes) {
}

static void create_virtual_stream(struct mv_userdata *u) {
    pa_sink_input_new_data data;

    pa_assert(u);

    if (!u->virtual_stream || u->virtual_sink_input)
        return;

    pa_sink_input_new_data_init(&data);

    data.driver = __FILE__;
    data.module = u->module;
    pa_proplist_sets(data.proplist, PA_PROP_MEDIA_NAME, "Virtual Stream for MainVolume Volume Control");
    pa_proplist_sets(data.proplist, PA_PROP_MEDIA_ROLE, "phone");
    pa_sink_input_new_data_set_sample_spec(&data, &u->core->default_sample_spec);
    pa_sink_input_new_data_set_channel_map(&data, &u->core->default_channel_map);
    data.flags = PA_SINK_INPUT_START_CORKED | PA_SINK_INPUT_NO_REMAP | PA_SINK_INPUT_NO_REMIX;

    pa_sink_input_new(&u->virtual_sink_input, u->module->core, &data);
    pa_sink_input_new_data_done(&data);

    if (!u->virtual_sink_input) {
        pa_log("failed to create virtual sink input.");
        return;
    }

    u->virtual_sink_input->userdata = u;
    u->virtual_sink_input->kill = sink_input_kill_cb;
    u->virtual_sink_input->pop = sink_input_pop_cb;
    u->virtual_sink_input->process_rewind = sink_input_process_rewind_cb;

    pa_sink_input_put(u->virtual_sink_input);

    pa_log_debug("created virtual sink input for voice call volume control.");
}

static void destroy_virtual_stream(struct mv_userdata *u) {
    pa_sink_input *i;

    pa_assert(u);

    if (!u->virtual_sink_input)
        return;

    i = u->virtual_sink_input;
    u->virtual_sink_input = NULL;
    pa_sink_input_kill(i);

    pa_log_debug("removed virtual stream.");
}

static void update_virtual_stream(struct mv_userdata *u) {
    pa_assert(u);

    if (!u->voip_active && (u->call_active || u->emergency_call_active))
        create_virtual_stream(u);
    else
        destroy_virtual_stream(u);
}

static pa_hook_result_t call_state_cb(void *hook_data, void *call_data, void *slot_data) {
    const char *key       = call_data;
    struct mv_userdata *u = slot_data;

    const char *str;

    pa_assert(key);
    pa_assert(u);
    pa_assert(u->current_steps);

    if ((str = pa_shared_data_gets(u->shared, key))) {
        if (pa_streq(str, PA_NEMO_PROP_CALL_STATE_ACTIVE)) {
            u->call_active = true;
            u->voip_active = false;
        } else if (pa_streq(str, PA_NEMO_PROP_CALL_STATE_VOIP_ACTIVE)) {
            u->call_active = true;
            u->voip_active = true;
        } else {
            u->call_active = false;
            u->voip_active = false;
        }
    } else {
        u->call_active = false;
        u->voip_active = false;
    }

    pa_log_debug("%scall is %s (media step %u call step %u)",
                 u->voip_active ? "voip " : "",
                 u->call_active ? PA_NEMO_PROP_CALL_STATE_ACTIVE : PA_NEMO_PROP_CALL_STATE_INACTIVE,
                 u->current_steps->media.current_step, u->current_steps->call.current_step);

    update_virtual_stream(u);

    signal_steps(u);

    if (u->notifier.watchdog)
        check_notifier(u);

    check_and_signal_high_volume(u);

    /* Notify users of new call status. */
    dbus_signal_call_status(u);

    return PA_HOOK_OK;
}

static void update_media_state(struct mv_userdata *u) {
    media_state_t state = MEDIA_INACTIVE;

    pa_assert(u);

    if (!u->call_active) {
        if (u->notifier.streams_active)
            state = MEDIA_BACKGROUND;

        if (u->notifier.policy_media_state != MEDIA_INACTIVE)
            state = u->notifier.policy_media_state;
    }

    if (state != u->notifier.media_state) {
        u->notifier.media_state = state;
        dbus_signal_media_state(u);
    }
}

static pa_hook_result_t media_state_cb(void *hook_data, void *call_data, void *slot_data) {
    const char *key       = call_data;
    struct mv_userdata *u = slot_data;

    const char *str;
    media_state_t state;

    pa_assert(key);
    pa_assert(u);

    if (!(str = pa_shared_data_gets(u->shared, key)))
        return PA_HOOK_OK;

    if (!mv_media_state_from_string(str, &state)) {
        pa_log_warn("Unknown media state %s", str);
        return PA_HOOK_OK;
    }

    u->notifier.policy_media_state = state;

    update_media_state(u);

    return PA_HOOK_OK;
}

static void update_emergency_call_state(struct mv_userdata *u, bool new_state) {
    struct mv_volume_steps *steps;
    pa_cvolume vol;

    pa_assert(u);

    if (new_state != u->emergency_call_active) {
        u->emergency_call_active = new_state;
        pa_log_info("Emergency call state changes to %s", u->emergency_call_active ? "active" : "inactive");

        update_virtual_stream(u);
        steps = mv_active_steps(u);

        pa_volume_proxy_get_volume(u->volume_proxy, CALL_STREAM, &vol);

        if (u->emergency_call_active)
            pa_cvolume_set(&vol, vol.channels, mv_step_value(steps, steps->n_steps - 1));
        else
            pa_cvolume_set(&vol, vol.channels, mv_step_value(steps, steps->current_step));

        pa_volume_proxy_set_volume(u->volume_proxy, CALL_STREAM, &vol, false);
    }
}

static pa_hook_result_t emergency_call_state_cb(void *hook_data, void *call_data, void *slot_data) {
    const char *key       = call_data;
    struct mv_userdata *u = slot_data;

    const char *str;

    pa_assert(key);
    pa_assert(u);

    if (!(str = pa_shared_data_gets(u->shared, key)))
        return PA_HOOK_OK;

    update_emergency_call_state(u, pa_streq(str, PA_NEMO_PROP_EMERGENCY_CALL_STATE_ACTIVE));

    return PA_HOOK_OK;
}

static void volume_sync_add_mute(struct mv_userdata *u, pa_sink_input *si) {
    const char *role;
    pa_cvolume mute;

    pa_assert(u);
    pa_assert(si);

    if (!(role = pa_proplist_gets(si->proplist, PA_PROP_MEDIA_ROLE)))
        return;

    if (pa_streq(role, "x-maemo") || pa_streq(role, "media")) {
        pa_cvolume_set(&mute, si->soft_volume.channels, 0);
        pa_log_debug("add mute to sink-input %s", pa_proplist_gets(si->proplist, PA_PROP_MEDIA_NAME));
        pa_sink_input_add_volume_factor(si, "mw-mute-when-moving", &mute);
    }
}

static void volume_sync_remove_mute(struct mv_userdata *u, pa_sink_input *si) {
    const char *role;

    pa_assert(u);
    pa_assert(si);

    if (!(role = pa_proplist_gets(si->proplist, PA_PROP_MEDIA_ROLE)))
        return;

    if (pa_streq(role, "x-maemo") || pa_streq(role, "media")) {
        pa_log_debug("remove mute from sink-input %s", pa_proplist_gets(si->proplist, PA_PROP_MEDIA_NAME));
        pa_sink_input_remove_volume_factor(si, "mw-mute-when-moving");
    }
}

static void volume_sync_remove_mute_all(struct mv_userdata *u) {
    pa_sink_input *si;
    uint32_t idx;

    pa_assert(u);

    PA_IDXSET_FOREACH(si, u->core->sink_inputs, idx)
        volume_sync_remove_mute(u, si);
    pa_log_debug("volumes in sync");
}

static void volume_sync_delayed_unmute_stop(struct mv_userdata *u) {
    pa_assert(u);

    if (u->volume_unmute_time_event) {
        u->core->mainloop->time_free(u->volume_unmute_time_event);
        u->volume_unmute_time_event = NULL;
    }
}

static void volume_sync_delayed_unmute_cb(pa_mainloop_api *a, pa_time_event *e, const struct timeval *t, void *userdata) {
    struct mv_userdata *u = (struct mv_userdata*)userdata;

    pa_assert(a);
    pa_assert(e);
    pa_assert(u);
    pa_assert(e == u->volume_unmute_time_event);

    volume_sync_delayed_unmute_stop(u);
    volume_sync_remove_mute_all(u);
    u->mute_routing_active = false;
}

static void volume_sync_delayed_unmute_set(struct mv_userdata *u) {
    pa_usec_t time;

    pa_assert(u);

    time = pa_rtclock_now() + u->volume_sync_delay_ms * PA_USEC_PER_MSEC;

    pa_log_debug("volume sync unmute streams in %u ms", u->volume_sync_delay_ms);
    if (u->volume_unmute_time_event)
        pa_core_rttime_restart(u->core, u->volume_unmute_time_event, time);
    else
        u->volume_unmute_time_event = pa_core_rttime_new(u->core, time, volume_sync_delayed_unmute_cb, u);
}

static pa_hook_result_t volume_sync_cb(void *hook_data, void *call_data, void *slot_data) {
    const char *key       = call_data;
    struct mv_userdata *u = slot_data;

    pa_sink_input *si;
    uint32_t idx;
    int32_t state;

    if (pa_shared_data_get_integer(u->shared, key, &state) == 0) {
        if (u->prev_state != PA_SAILFISHOS_MEDIA_VOLUME_IN_SYNC &&
            state         == PA_SAILFISHOS_MEDIA_VOLUME_IN_SYNC) {

            if (u->volume_sync_delay_ms)
                volume_sync_delayed_unmute_set(u);
            else {
                volume_sync_remove_mute_all(u);
                u->mute_routing_active = false;
            }
        }
        else if (u->prev_state == PA_SAILFISHOS_MEDIA_VOLUME_IN_SYNC &&
                 state         != PA_SAILFISHOS_MEDIA_VOLUME_IN_SYNC) {

            pa_log_debug("volumes out of sync");
            volume_sync_delayed_unmute_stop(u);
            if (!u->mute_routing_active) {
                PA_IDXSET_FOREACH(si, u->core->sink_inputs, idx)
                    volume_sync_add_mute(u, si);
            }
            u->mute_routing_active = true;
        }

        u->prev_state = state;
    }

    return PA_HOOK_OK;
}

/* create new volume steps set for route with linear steps.
 * route        - name of step route
 * call_steps   - number of steps for call case
 * media_steps  - number of steps for media case
 */
static struct mv_volume_steps_set* fallback_new(const char *route, const int call_steps, const int media_steps) {
    struct mv_volume_steps_set *fallback;
    int i;

    pa_assert(route);
    pa_assert(call_steps > 1);
    pa_assert(media_steps > 1);

    fallback = pa_xnew0(struct mv_volume_steps_set, 1);
    fallback->call.n_steps = call_steps;
    fallback->call.step = pa_xmalloc(sizeof(pa_volume_t) * call_steps);
    fallback->media.n_steps = media_steps;
    fallback->media.step = pa_xmalloc(sizeof(pa_volume_t) * media_steps);
    fallback->has_high_volume_step = false;

    /* calculate call/media_steps linearly using PA_VOLUME_NORM
     * as max value, starting from 0 volume. */

    for (i = 0; i < call_steps; i++)
        fallback->call.step[i] = ((double) PA_VOLUME_NORM / (double) (call_steps - 1)) * (double) i;

    for (i = 0; i < media_steps; i++)
        fallback->media.step[i] = ((double) PA_VOLUME_NORM / (double) (media_steps - 1)) * (double) i;

    fallback->route = pa_xstrdup(route);

    return fallback;
}

static pa_hook_result_t parameters_changed_cb(void *hook_data, void *call_data, void *slot_data) {
    meego_parameter_update_args *ua = call_data;
    struct mv_userdata *u           = slot_data;

    struct mv_volume_steps_set *set;
    pa_proplist *p = NULL;
    bool ret = false;

    pa_assert(ua);
    pa_assert(u);

    pa_shared_data_inc_integer(u->shared, PA_SAILFISHOS_MEDIA_VOLUME_SYNC,
                                          PA_SAILFISHOS_MEDIA_VOLUME_CHANGING);

    if (u->route)
        pa_xfree(u->route);

    u->route = pa_xstrdup(ua->mode);

    /* in tuning mode we always update steps when changing
     * x-maemo.mode.
     * First remove tunings in current route, then try to parse
     * normally */
    if (u->tuning_mode && ua->parameters) {
        if ((set = pa_hashmap_remove(u->steps, u->route))) {
            mv_volume_steps_set_free(set);
            set = NULL;
        }
    }

    /* try to get step configuration from cache (hashmap) and
     * if steps aren't found try to parse them from property
     * list.
     * If no tunings can be found from property list or the tunings
     * are incorrect, we use "fallback" route, which is created
     * in module init.
     */
    set = pa_hashmap_get(u->steps, u->route);
    if (set) {
        u->current_steps = set;
    } else {
        if (ua && ua->parameters && (p = pa_proplist_from_string(ua->parameters)))
            ret = mv_parse_steps(u,
                                 u->route,
                                 pa_proplist_gets(p, PROP_CALL_STEPS),
                                 pa_proplist_gets(p, PROP_VOIP_STEPS),
                                 pa_proplist_gets(p, PROP_MEDIA_STEPS),
                                 pa_proplist_gets(p, PROP_HIGH_VOLUME));

        if (ret) {
            u->current_steps = pa_hashmap_get(u->steps, u->route);
        } else {
            pa_log_info("failed to update steps for %s, using fallback.", u->route);
            u->current_steps = pa_hashmap_get(u->steps, "fallback");
        }
    }

    if (p)
        pa_proplist_free(p);

    pa_log_debug("mode changes to %s (%u media steps, %u call steps)",
                 u->route, u->current_steps->media.n_steps, u->current_steps->call.n_steps);

    /* Check if new route is in notifier watch list */
    if (u->notifier.watchdog) {
        mv_notifier_update_route(u, u->route);
        check_notifier(u);
    }

    /* When mode changes immediately send HighVolume signal
     * containing the safe step if one is defined */
    check_and_signal_high_volume(u);

    pa_shared_data_inc_integer(u->shared, PA_SAILFISHOS_MEDIA_VOLUME_SYNC,
                                          PA_SAILFISHOS_MEDIA_VOLUME_CHANGE_DONE);

    return PA_HOOK_OK;
}

static bool step_and_call_values(struct mv_userdata *u,
                                 const char *name,
                                 struct mv_volume_steps **steps,
                                 bool *call_steps) {
    if (pa_streq(name, CALL_STREAM)) {
        *steps = &u->current_steps->call;
        *call_steps = true;
        return true;
    } else if (pa_streq(name, VOIP_STREAM)) {
        *steps = &u->current_steps->voip;
        *call_steps = true;
        return true;
    } else if (pa_streq(name, MEDIA_STREAM)) {
        *steps = &u->current_steps->media;
        *call_steps = false;
        return true;
    } else
        return false;
}

static pa_hook_result_t volume_changing_cb(void *hook_data, void *call_data, void *slot_data) {
    pa_volume_proxy_entry *e = call_data;
    struct mv_userdata *u    = slot_data;

    struct mv_volume_steps *steps;
    uint32_t new_step;
    bool call_steps;

    pa_assert(u);

    if (!step_and_call_values(u, e->name, &steps, &call_steps))
        return PA_HOOK_OK;

    if (u->emergency_call_active && pa_streq(e->name, CALL_STREAM)) {
        pa_log_info("Reset call volume to maximum with emergency call.");
        pa_cvolume_set(&e->volume, e->volume.channels, mv_step_value(steps, steps->n_steps - 1));
        return PA_HOOK_OK;
    }

    /* Check only once per module load / parsed step set
     * if volume is higher than safe step. If so, reset to
     * safe step. */
    if (!call_steps && u->current_steps->first && mv_has_high_volume(u)) {
        new_step = mv_search_step(steps->step, steps->n_steps, pa_cvolume_avg(&e->volume));

        if (new_step > mv_safe_step(u)) {
            pa_log_info("high volume after module load, requested %u, we will reset to safe step %u", new_step, mv_safe_step(u));
            pa_cvolume_set(&e->volume, e->volume.channels, mv_step_value(steps, mv_safe_step(u)));
        }
        u->current_steps->first = false;
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t volume_changed_cb(void *hook_data, void *call_data, void *slot_data) {
    pa_volume_proxy_entry *e = call_data;
    struct mv_userdata *u    = slot_data;

    struct mv_volume_steps *steps;
    uint32_t new_step;
    bool call_steps;

    pa_assert(u);

    if (!step_and_call_values(u, e->name, &steps, &call_steps))
        return PA_HOOK_OK;

    new_step = mv_search_step(steps->step, steps->n_steps, pa_cvolume_avg(&e->volume));

    if (new_step != steps->current_step) {
        pa_log_debug("volume changed for stream %s, vol %u (step %u)", e->name,
                                                                       pa_cvolume_avg(&e->volume),
                                                                       new_step);

        steps->current_step = new_step;
    }

    /* if the changed route volume was for currently active steps (phone / x-maemo)
     * then signal steps forward. */
    if (call_steps == u->call_active)
        signal_steps(u);

    return PA_HOOK_OK;
}

static void check_notifier(struct mv_userdata *u) {
    pa_assert(u);

    if (mv_notifier_active(u))
        mv_listening_watchdog_start(u->notifier.watchdog);
    else
        mv_listening_watchdog_pause(u->notifier.watchdog);

    u->notifier.streams_active = u->notifier.enabled_slots ? true : false;
    update_media_state(u);
}

static void notify_event_cb(struct mv_listening_watchdog *wd, bool initial_notify, void *userdata) {
    struct mv_userdata *u = userdata;

    pa_assert(wd);
    pa_assert(u);

    pa_log_debug("Listening timer expired, send %snotify signal.", initial_notify ? "initial " : "");
    if (initial_notify)
        dbus_signal_listening_notifier(u, 0);
    else {
        dbus_signal_listening_notifier(u, u->notifier.timeout);
        check_notifier(u);
    }
}

static uint32_t acquire_slot(struct mv_userdata *u) {
    uint32_t i;
    uint32_t slot;

    pa_assert(u);

    if (!u->notifier.free_slots) {
        pa_log_warn("All sink-input watcher slots taken.");
        return 0;
    }

    for (i = 0; i < 32; i++) {
        slot = (1 << i);
        if (u->notifier.free_slots & slot) {
            u->notifier.free_slots &= ~slot;
            return slot;
        }
    }
    return 0;
}

static void release_slot(struct mv_userdata *u, uint32_t slot) {
    pa_assert(u);

    u->notifier.free_slots |= slot;
}

static pa_hook_result_t sink_input_put_cb(pa_core *c, pa_object *o, struct mv_userdata *u) {
    pa_sink_input *si;
    pa_sink_input_state_t state;
    const char *role;
    uint32_t slot;

    pa_assert(o);
    pa_assert(u);
    si = PA_SINK_INPUT(o);

    if (!(role = pa_proplist_gets(si->proplist, PA_PROP_MEDIA_ROLE)))
        /* No media role, skip */
        goto end;

    if (u->mute_routing_active)
        volume_sync_add_mute(u, si);

    if (!pa_hashmap_get(u->notifier.roles, role))
        /* Not our stream, skip */
        goto end;

    if (!(slot = acquire_slot(u)))
        /* All slots taken, skip */
        goto end;

    pa_sink_input_ref(si);
    if (pa_hashmap_put(u->notifier.sink_inputs, si, PA_UINT32_TO_PTR(slot))) {
        /* Already in our hashmap? Shouldn't happen... */
        pa_sink_input_unref(si);
        release_slot(u, slot);
        goto end;
    }

#if (PA_CHECK_VERSION(13,0,0))
    state = si->state;
#else
    state = pa_sink_input_get_state(si);
#endif
    if (state == PA_SINK_INPUT_DRAINED ||
        state == PA_SINK_INPUT_RUNNING)
        u->notifier.enabled_slots |= slot;

    check_notifier(u);

end:
    return PA_HOOK_OK;
}
static pa_hook_result_t sink_input_state_changed_cb(pa_core *c, pa_object *o, struct mv_userdata *u) {
    pa_sink_input *si;
    pa_sink_input_state_t state;
    uintptr_t *slot_ptr;
    uint32_t slot;

    pa_assert(o);
    pa_assert(u);
    pa_assert(pa_sink_input_isinstance(o));

    si = PA_SINK_INPUT(o);

    if (!(slot_ptr = pa_hashmap_get(u->notifier.sink_inputs, si)))
        /* Not our stream, skip */
        goto end;

    slot = PA_PTR_TO_UINT32(slot_ptr);

#if (PA_CHECK_VERSION(13,0,0))
    state = si->state;
#else
    state = pa_sink_input_get_state(si);
#endif
    if (state == PA_SINK_INPUT_DRAINED ||
        state == PA_SINK_INPUT_RUNNING)
        u->notifier.enabled_slots |= slot;
    else
        u->notifier.enabled_slots &= ~slot;

    check_notifier(u);

end:
    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_unlink_cb(pa_core *c, pa_object *o, struct mv_userdata *u) {
    pa_sink_input *si;
    uintptr_t *slot_ptr;
    uint32_t slot;

    pa_assert(o);
    pa_assert(u);
    pa_assert(pa_sink_input_isinstance(o));

    si = PA_SINK_INPUT(o);

    if (!(slot_ptr = pa_hashmap_remove(u->notifier.sink_inputs, si)))
        /* Not our stream, skip */
        goto end;

    slot = PA_PTR_TO_UINT32(slot_ptr);
    u->notifier.enabled_slots &= ~slot;
    release_slot(u, slot);

    pa_sink_input_unref(si);

    check_notifier(u);

end:
    return PA_HOOK_OK;
}

#define NOTIFIER_LIST_DELIMITER ","

static int parse_list(pa_config_parser_state *state) {
    const char *split_state = NULL;
    char *c;

    pa_assert(state);

    while ((c = pa_split(state->rvalue, NOTIFIER_LIST_DELIMITER, &split_state))) {
        pa_hashmap *m = state->data;
        if (pa_hashmap_put(m, c, c) != 0) {
            pa_log_warn("Duplicate %s entry: \"%s\"", state->lvalue, c);
            pa_xfree(c);
        } else
            pa_log_debug("Notifier conf %s add: \"%s\"", state->lvalue, c);
    }

    return 0;
}

static void setup_notifier(struct mv_userdata *u, const char *conf_file) {
    uint32_t timeout = 0;
    const char *conf;
    pa_hashmap *mode_list;
    pa_hashmap *role_list;

    mode_list = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL, pa_xfree);
    role_list = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL, pa_xfree);

    pa_config_item items[] = {
        { "timeout",    pa_config_parse_unsigned,   &timeout,   NULL },
        { "role-list",  parse_list,                 role_list,  NULL },
        { "mode-list",  parse_list,                 mode_list,  NULL },
        { NULL, NULL, NULL, NULL }
    };

    conf = conf_file ? conf_file : DEFAULT_LISTENING_NOTIFIER_CONF_FILE;
    pa_log_debug("Read long listening time notifier config from %s", conf);
#if (PULSEAUDIO_VERSION >= 8)
    pa_config_parse(conf, NULL, items, NULL, false, NULL);
#else
    pa_config_parse(conf, NULL, items, NULL, NULL);
#endif

    if (pa_hashmap_isempty(role_list) || pa_hashmap_isempty(mode_list) || timeout == 0) {
        /* No valid configuration parsed, free and return */
        pa_hashmap_free(mode_list);
        pa_hashmap_free(role_list);
        pa_log_debug("Long listening time notifier disabled.");
        return;
    }

    u->notifier.watchdog = mv_listening_watchdog_new(u->core, notify_event_cb, timeout, u);
    u->notifier.timeout = timeout;
    u->notifier.roles = role_list;
    u->notifier.modes = mode_list;
    u->notifier.free_slots = UINT32_MAX;
    u->notifier.sink_inputs = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    u->notifier.sink_input_put_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_put_cb, u);
    u->notifier.sink_input_changed_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_INPUT_STATE_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_state_changed_cb, u);
    u->notifier.sink_input_unlink_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_unlink_cb, u);

    pa_log_debug("Long listening time notifier setup done.");
}

static void free_si_hashmap(pa_hashmap *h) {
    pa_sink_input *si;
    void *state = NULL;
    const void *key;

    pa_assert(h);

    while (pa_hashmap_iterate(h, &state, &key)) {
        /* Yes we strip const from key pointer. We shouldn't get
         * here other than when mainvolume-module is unloaded
         * while daemon is running. And when we do, we have our a bit
         * cleverly used hashmap with data as keys, which we need
         * to unref. */
        si = (pa_sink_input *) key;
        pa_sink_input_unref(si);
    }

    pa_hashmap_free(h);
}

static void notifier_done(struct mv_userdata *u) {
    pa_assert(u);

    if (!u->notifier.watchdog)
        return;

    if (u->notifier.sink_input_put_slot)
        pa_hook_slot_free(u->notifier.sink_input_put_slot);
    if (u->notifier.sink_input_changed_slot)
        pa_hook_slot_free(u->notifier.sink_input_changed_slot);
    if (u->notifier.sink_input_unlink_slot)
        pa_hook_slot_free(u->notifier.sink_input_unlink_slot);

    mv_listening_watchdog_free(u->notifier.watchdog);

    if (u->notifier.roles)
        pa_hashmap_free(u->notifier.roles);
    if (u->notifier.modes)
        pa_hashmap_free(u->notifier.modes);
    if (u->notifier.sink_inputs)
        free_si_hashmap(u->notifier.sink_inputs);
}

int pa__init(pa_module *m) {
    pa_modargs *ma = NULL;
    const char *notifier_conf;
    struct mv_userdata *u;
    struct mv_volume_steps_set *fallback;

    u = pa_xnew0(struct mv_userdata, 1);

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u;
    u->core = m->core;
    u->module = m;

    u->steps = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func,
                                   NULL, (pa_free_cb_t) mv_volume_steps_set_free);

    fallback = fallback_new("fallback", 10, 20);
    pa_hashmap_put(u->steps, fallback->route, fallback);
    u->current_steps = fallback;

    u->tuning_mode = false;
    u->virtual_stream = false;
    u->mute_routing = DEFAULT_MUTE_ROUTING;
    u->volume_sync_delay_ms = DEFAULT_VOLUME_SYNC_DELAY_MS;

    if (pa_modargs_get_value_boolean(ma, "tuning_mode", &u->tuning_mode) < 0) {
        pa_log_error("tuning_mode expects boolean argument");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "virtual_stream", &u->virtual_stream) < 0) {
        pa_log_error("virtual_stream expects boolean argument");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "mute_routing", &u->mute_routing) < 0) {
        pa_log_error("mute_routing expects boolean argument");
        goto fail;
    }

    if (pa_modargs_get_value_u32(ma, "unmute_delay", &u->volume_sync_delay_ms) < 0) {
        pa_log_error("unmute_delay expects unsigned integer argument");
        goto fail;
    }

    notifier_conf = pa_modargs_get_value(ma, "listening_time_notifier_conf", NULL);
    setup_notifier(u, notifier_conf);

    u->shared = pa_shared_data_get(u->core);
    u->call_state_hook_slot = pa_shared_data_connect(u->shared, PA_NEMO_PROP_CALL_STATE, call_state_cb, u);
    u->media_state_hook_slot = pa_shared_data_connect(u->shared, PA_NEMO_PROP_MEDIA_STATE, media_state_cb, u);
    u->emergency_call_state_hook_slot = pa_shared_data_connect(u->shared, PA_NEMO_PROP_EMERGENCY_CALL_STATE, emergency_call_state_cb, u);
    if (u->mute_routing)
        u->volume_sync_hook_slot = pa_shared_data_connect(u->shared,
                                                          PA_SAILFISHOS_MEDIA_VOLUME_SYNC,
                                                          volume_sync_cb, u);
    u->prev_state = PA_SAILFISHOS_MEDIA_VOLUME_IN_SYNC;

    u->volume_proxy = pa_volume_proxy_get(u->core);
    u->volume_proxy_slot = pa_hook_connect(&pa_volume_proxy_hooks(u->volume_proxy)[PA_VOLUME_PROXY_HOOK_CHANGING],
                                           PA_HOOK_NORMAL,
                                           volume_changing_cb,
                                           u);
    u->volume_proxy_slot = pa_hook_connect(&pa_volume_proxy_hooks(u->volume_proxy)[PA_VOLUME_PROXY_HOOK_CHANGED],
                                           PA_HOOK_NORMAL,
                                           volume_changed_cb,
                                           u);

    dbus_init(u);

    meego_parameter_request_updates("mainvolume", parameters_changed_cb, PA_HOOK_EARLY, true, u);

    pa_modargs_free(ma);

    return 0;

 fail:
    if (ma)
        pa_modargs_free(ma);

    pa_xfree(u);
    m->userdata = NULL;

    return -1;
}

void pa__done(pa_module *m) {
    struct mv_userdata *u = m->userdata;

    notifier_done(u);

    meego_parameter_stop_updates("mainvolume", parameters_changed_cb, u);

    volume_sync_delayed_unmute_stop(u);
    signal_timer_stop(u);

    dbus_done(u);

    destroy_virtual_stream(u);

    if (u->sink_proplist_changed_slot)
        pa_hook_slot_free(u->sink_proplist_changed_slot);

    if (u->call_state_hook_slot)
        pa_hook_slot_free(u->call_state_hook_slot);

    if (u->media_state_hook_slot)
        pa_hook_slot_free(u->media_state_hook_slot);

    if (u->emergency_call_state_hook_slot)
        pa_hook_slot_free(u->emergency_call_state_hook_slot);

    if (u->volume_sync_hook_slot)
        pa_hook_slot_free(u->volume_sync_hook_slot);

    if (u->shared)
        pa_shared_data_unref(u->shared);

    if (u->volume_proxy_slot)
        pa_hook_slot_free(u->volume_proxy_slot);

    if (u->volume_proxy)
        pa_volume_proxy_unref(u->volume_proxy);

    pa_hashmap_free(u->steps);

    pa_assert(m);

    if (u)
        pa_xfree(u);
}

/*
 * DBus
 */

#define MAINVOLUME_API_MAJOR (2)
#define MAINVOLUME_API_MINOR (3)
#define MAINVOLUME_PATH "/com/meego/mainvolume2"
#define MAINVOLUME_IFACE "com.Meego.MainVolume2"

static void mainvolume_get_revision(DBusConnection *conn, DBusMessage *msg, void *_u);
static void mainvolume_get_step_count(DBusConnection *conn, DBusMessage *msg, void *_u);
static void mainvolume_get_current_step(DBusConnection *conn, DBusMessage *msg, void *_u);
static void mainvolume_set_current_step(DBusConnection *conn, DBusMessage *msg, DBusMessageIter *iter, void *_u);
static void mainvolume_get_high_volume_step(DBusConnection *conn, DBusMessage *msg, void *_u);
static void mainvolume_get_call_state(DBusConnection *conn, DBusMessage *msg, void *_u);
static void mainvolume_get_media_state(DBusConnection *conn, DBusMessage *msg, void *_u);
static void mainvolume_get_all(DBusConnection *conn, DBusMessage *msg, void *_u);

enum mainvolume_handler_index {
    MAINVOLUME_HANDLER_REVISION,
    MAINVOLUME_HANDLER_STEP_COUNT,
    MAINVOLUME_HANDLER_CURRENT_STEP,
    MAINVOLUME_HANDLER_HIGH_VOLUME,
    MAINVOLUME_HANDLER_CALL_STATE,
    MAINVOLUME_HANDLER_MEDIA_STATE,
    MAINVOLUME_HANDLER_MAX
};

static pa_dbus_property_handler mainvolume_handlers[MAINVOLUME_HANDLER_MAX] = {
    [MAINVOLUME_HANDLER_REVISION] = {
        .property_name = "InterfaceRevision",
        .type = "u",
        .get_cb = mainvolume_get_revision,
        .set_cb = NULL
    },
    [MAINVOLUME_HANDLER_STEP_COUNT] = {
        .property_name = "StepCount",
        .type = "u",
        .get_cb = mainvolume_get_step_count,
        .set_cb = NULL
    },
    [MAINVOLUME_HANDLER_CURRENT_STEP] = {
        .property_name = "CurrentStep",
        .type = "u",
        .get_cb = mainvolume_get_current_step,
        .set_cb = mainvolume_set_current_step
    },
    [MAINVOLUME_HANDLER_HIGH_VOLUME] = {
        .property_name = "HighVolumeStep",
        .type = "u",
        .get_cb = mainvolume_get_high_volume_step,
        .set_cb = NULL
    },
    [MAINVOLUME_HANDLER_CALL_STATE] = {
        .property_name = "CallState",
        .type = "s",
        .get_cb = mainvolume_get_call_state,
        .set_cb = NULL
    },
    [MAINVOLUME_HANDLER_MEDIA_STATE] = {
        .property_name = "MediaState",
        .type = "s",
        .get_cb = mainvolume_get_media_state,
        .set_cb = NULL
    }
};

enum mainvolume_signal_index {
    MAINVOLUME_SIGNAL_STEPS_UPDATED,
    MAINVOLUME_SIGNAL_NOTIFY_LISTENER,  /* Notify user that one has listened to audio for a long time. */
    MAINVOLUME_SIGNAL_HIGH_VOLUME,      /* Notify user that current volume is harmful for hearing. */
    MAINVOLUME_SIGNAL_CALL_STATE,       /* Notify user of current call state. */
    MAINVOLUME_SIGNAL_MEDIA_STATE,      /* Notify user about media state. */
    MAINVOLUME_SIGNAL_MAX
};

static pa_dbus_arg_info steps_updated_args[] = {
    {"StepCount", "u", NULL},
    {"CurrentStep", "u", NULL}
};

static pa_dbus_arg_info high_volume_args[] = {
    {"SafeStep", "u", NULL}
};

static pa_dbus_arg_info listening_time_args[] = {
    {"ListeningTime", "u", NULL}
};

static pa_dbus_arg_info media_or_call_state_args[] = {
    {"State", "s", NULL}
};

static pa_dbus_signal_info mainvolume_signals[MAINVOLUME_SIGNAL_MAX] = {
    [MAINVOLUME_SIGNAL_STEPS_UPDATED] = {
        .name = "StepsUpdated",
        .arguments = steps_updated_args,
        .n_arguments = 2
    },
    [MAINVOLUME_SIGNAL_NOTIFY_LISTENER] = {
        .name = "NotifyListeningTime",
        .arguments = listening_time_args,
        .n_arguments = 1
    },
    [MAINVOLUME_SIGNAL_HIGH_VOLUME] = {
        .name = "NotifyHighVolume",
        .arguments = high_volume_args,
        .n_arguments = 1
    },
    [MAINVOLUME_SIGNAL_CALL_STATE] = {
        .name = "CallStateChanged",
        .arguments = media_or_call_state_args,
        .n_arguments = 1
    },
    [MAINVOLUME_SIGNAL_MEDIA_STATE] = {
        .name = "MediaStateChanged",
        .arguments = media_or_call_state_args,
        .n_arguments = 1
    }
};

static pa_dbus_interface_info mainvolume_info = {
    .name = MAINVOLUME_IFACE,
    .method_handlers = NULL,
    .n_method_handlers = 0,
    .property_handlers = mainvolume_handlers,
    .n_property_handlers = MAINVOLUME_HANDLER_MAX,
    .get_all_properties_cb = mainvolume_get_all,
    .signals = mainvolume_signals,
    .n_signals = MAINVOLUME_SIGNAL_MAX
};

void dbus_init(struct mv_userdata *u) {
    pa_assert(u);
    pa_assert(u->core);

    u->dbus_protocol = pa_dbus_protocol_get(u->core);
    u->dbus_path = pa_sprintf_malloc("/com/meego/mainvolume%d", MAINVOLUME_API_MAJOR);

    pa_dbus_protocol_add_interface(u->dbus_protocol, MAINVOLUME_PATH, &mainvolume_info, u);
    pa_dbus_protocol_register_extension(u->dbus_protocol, MAINVOLUME_IFACE);
}

void dbus_done(struct mv_userdata *u) {
    pa_assert(u);

    pa_dbus_protocol_unregister_extension(u->dbus_protocol, MAINVOLUME_IFACE);
    pa_dbus_protocol_remove_interface(u->dbus_protocol, u->dbus_path, mainvolume_info.name);
    pa_xfree(u->dbus_path);
    pa_dbus_protocol_unref(u->dbus_protocol);
}

static void dbus_signal_call_status(struct mv_userdata *u) {
    DBusMessage *signal;
    const char *state_str;

    pa_assert(u);

    state_str = u->call_active ? PA_NEMO_PROP_CALL_STATE_ACTIVE : PA_NEMO_PROP_CALL_STATE_INACTIVE;

    pa_assert_se((signal = dbus_message_new_signal(MAINVOLUME_PATH,
                                                   MAINVOLUME_IFACE,
                                                   mainvolume_signals[MAINVOLUME_SIGNAL_CALL_STATE].name)));
    pa_assert_se(dbus_message_append_args(signal,
                                          DBUS_TYPE_STRING, &state_str,
                                          DBUS_TYPE_INVALID));
    pa_dbus_protocol_send_signal(u->dbus_protocol, signal);
    dbus_message_unref(signal);

    pa_log_debug("Signal %s. State: %s", mainvolume_signals[MAINVOLUME_SIGNAL_CALL_STATE].name, state_str);
}

static void dbus_signal_high_volume(struct mv_userdata *u, uint32_t safe_step) {
    DBusMessage *signal;

    pa_assert(u);

    pa_assert_se((signal = dbus_message_new_signal(MAINVOLUME_PATH,
                                                   MAINVOLUME_IFACE,
                                                   mainvolume_signals[MAINVOLUME_SIGNAL_HIGH_VOLUME].name)));
    pa_assert_se(dbus_message_append_args(signal,
                                          DBUS_TYPE_UINT32, &safe_step,
                                          DBUS_TYPE_INVALID));
    pa_dbus_protocol_send_signal(u->dbus_protocol, signal);
    dbus_message_unref(signal);

    pa_log_debug("Signal %s. Safe step: %u", mainvolume_signals[MAINVOLUME_SIGNAL_HIGH_VOLUME].name, safe_step);
}

void dbus_signal_steps(struct mv_userdata *u) {
    DBusMessage *signal;
    struct mv_volume_steps *steps;
    uint32_t current_step;
    uint32_t step_count;

    pa_assert(u);
    pa_assert(u->current_steps);

    steps = mv_active_steps(u);
    step_count = steps->n_steps;
    if (u->emergency_call_active)
        current_step = steps->n_steps - 1;
    else
        current_step = steps->current_step;

    pa_log_debug("signal active step %u", current_step);

    pa_assert_se((signal = dbus_message_new_signal(MAINVOLUME_PATH,
                                                   MAINVOLUME_IFACE,
                                                   mainvolume_signals[MAINVOLUME_SIGNAL_STEPS_UPDATED].name)));
    pa_assert_se(dbus_message_append_args(signal,
                                          DBUS_TYPE_UINT32, &step_count,
                                          DBUS_TYPE_UINT32, &current_step,
                                          DBUS_TYPE_INVALID));
    pa_dbus_protocol_send_signal(u->dbus_protocol, signal);
    dbus_message_unref(signal);

    u->last_signal_timestamp = pa_rtclock_now();
}

static void dbus_signal_listening_notifier(struct mv_userdata *u, uint32_t timeout) {
    DBusMessage *signal;

    pa_assert(u);

    pa_assert_se((signal = dbus_message_new_signal(MAINVOLUME_PATH,
                                                   MAINVOLUME_IFACE,
                                                   mainvolume_signals[MAINVOLUME_SIGNAL_NOTIFY_LISTENER].name)));
    pa_assert_se(dbus_message_append_args(signal,
                                          DBUS_TYPE_UINT32, &timeout,
                                          DBUS_TYPE_INVALID));

    pa_dbus_protocol_send_signal(u->dbus_protocol, signal);
    dbus_message_unref(signal);
}

static void dbus_signal_media_state(struct mv_userdata *u) {
    DBusMessage *signal;
    const char *state;

    pa_assert(u);

    state = mv_media_state_from_enum(u->notifier.media_state);

    pa_assert_se((signal = dbus_message_new_signal(MAINVOLUME_PATH,
                                                   MAINVOLUME_IFACE,
                                                   mainvolume_signals[MAINVOLUME_SIGNAL_MEDIA_STATE].name)));
    pa_assert_se(dbus_message_append_args(signal,
                                          DBUS_TYPE_STRING, &state,
                                          DBUS_TYPE_INVALID));
    pa_dbus_protocol_send_signal(u->dbus_protocol, signal);
    dbus_message_unref(signal);

    pa_log_debug("Signal %s. State: %s (%u)", mainvolume_signals[MAINVOLUME_SIGNAL_MEDIA_STATE].name,
                                           state, u->notifier.media_state);
}


void mainvolume_get_revision(DBusConnection *conn, DBusMessage *msg, void *_u) {
    uint32_t rev = MAINVOLUME_API_MINOR;
    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_UINT32, &rev);
}

void mainvolume_get_step_count(DBusConnection *conn, DBusMessage *msg, void *_u) {
    struct mv_userdata *u = (struct mv_userdata*)_u;
    struct mv_volume_steps *steps;
    uint32_t step_count;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u);

    steps = mv_active_steps(u);
    step_count = steps->n_steps;
    pa_log_debug("D-Bus: Get step count (%u)", step_count);

    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_UINT32, &step_count);
}

void mainvolume_get_current_step(DBusConnection *conn, DBusMessage *msg, void *_u) {
    struct mv_userdata *u = (struct mv_userdata*)_u;
    struct mv_volume_steps *steps;
    uint32_t step;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u);

    steps = mv_active_steps(u);
    step = steps->current_step;
    pa_log_debug("D-Bus: Get current step (%u)", step);

    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_UINT32, &step);
}

static const char *active_stream_type(struct mv_userdata *u) {
    if (u->voip_active)
        return VOIP_STREAM;
    else if (u->call_active)
        return CALL_STREAM;

    return MEDIA_STREAM;
}

void mainvolume_set_current_step(DBusConnection *conn, DBusMessage *msg, DBusMessageIter *iter, void *_u) {
    struct mv_userdata *u = (struct mv_userdata*)_u;
    struct mv_volume_steps *steps;
    const char *stream;
    pa_cvolume vol;
    uint32_t set_step;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u);

    if (u->emergency_call_active) {
        pa_log_info("D-Bus: Emergency call is active, don't allow changing volume.");
        goto done;
    }

    steps = mv_active_steps(u);

    dbus_message_iter_get_basic(iter,  &set_step);

    pa_log_debug("D-Bus: Set step (%u)", set_step);

    if (set_step >= steps->n_steps) {
        pa_log_debug("D-Bus: Step %u out of bounds.", set_step);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "Step %u out of bounds.", set_step);
        return;
    }

    if (mv_set_step(u, set_step)) {
        stream = active_stream_type(u);
        pa_volume_proxy_get_volume(u->volume_proxy, stream, &vol);
        pa_cvolume_set(&vol, vol.channels, mv_current_step_value(u));
        pa_volume_proxy_set_volume(u->volume_proxy, stream, &vol, false);
    }

done:
    pa_dbus_send_empty_reply(conn, msg);

    u->last_step_set_timestamp = pa_rtclock_now();
    signal_steps(u);
}

void mainvolume_get_high_volume_step(DBusConnection *conn, DBusMessage *msg, void *_u) {
    struct mv_userdata *u = (struct mv_userdata*)_u;
    uint32_t high_volume_step = 0;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u);

    if (mv_has_high_volume(u))
        high_volume_step = mv_safe_step(u) + 1;

    pa_log_debug("D-Bus: Get high volume step (%u)", high_volume_step);

    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_UINT32, &high_volume_step);
}

void mainvolume_get_call_state(DBusConnection *conn, DBusMessage *msg, void *_u) {
    struct mv_userdata *u = (struct mv_userdata*)_u;
    const char *state;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u);

    state = u->call_active ? PA_NEMO_PROP_CALL_STATE_ACTIVE : PA_NEMO_PROP_CALL_STATE_INACTIVE;

    pa_log_debug("D-Bus: Get CallState %s", state);

    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_STRING, &state);
}

void mainvolume_get_media_state(DBusConnection *conn, DBusMessage *msg, void *_u) {
    struct mv_userdata *u = (struct mv_userdata*)_u;
    const char *state;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u);

    state = mv_media_state_from_enum(u->notifier.media_state);

    pa_log_debug("D-Bus: Get MediaState %s (%u)", state, u->notifier.media_state);

    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_STRING, &state);
}

void mainvolume_get_all(DBusConnection *conn, DBusMessage *msg, void *_u) {
    struct mv_userdata *u = (struct mv_userdata*)_u;
    DBusMessage *reply = NULL;
    DBusMessageIter msg_iter;
    DBusMessageIter dict_iter;
    uint32_t rev;
    struct mv_volume_steps *steps;
    uint32_t step_count;
    uint32_t current_step;
    uint32_t high_volume_step = 0;
    const char *call_state;
    const char *media_state;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u);

    steps = mv_active_steps(u);

    rev = MAINVOLUME_API_MINOR;
    step_count = steps->n_steps;
    current_step = steps->current_step;

    if (mv_has_high_volume(u))
        high_volume_step = mv_safe_step(u) + 1;

    call_state = u->call_active ? PA_NEMO_PROP_CALL_STATE_ACTIVE : PA_NEMO_PROP_CALL_STATE_INACTIVE;

    media_state = mv_media_state_from_enum(u->notifier.media_state);

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &msg_iter);
    pa_assert_se(dbus_message_iter_open_container(&msg_iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter));

    pa_dbus_append_basic_variant_dict_entry(&dict_iter,
                                            mainvolume_handlers[MAINVOLUME_HANDLER_REVISION].property_name,
                                            DBUS_TYPE_UINT32, &rev);
    pa_dbus_append_basic_variant_dict_entry(&dict_iter,
                                            mainvolume_handlers[MAINVOLUME_HANDLER_STEP_COUNT].property_name,
                                            DBUS_TYPE_UINT32, &step_count);
    pa_dbus_append_basic_variant_dict_entry(&dict_iter,
                                            mainvolume_handlers[MAINVOLUME_HANDLER_CURRENT_STEP].property_name,
                                            DBUS_TYPE_UINT32, &current_step);
    pa_dbus_append_basic_variant_dict_entry(&dict_iter,
                                            mainvolume_handlers[MAINVOLUME_HANDLER_HIGH_VOLUME].property_name,
                                            DBUS_TYPE_UINT32, &high_volume_step);
    pa_dbus_append_basic_variant_dict_entry(&dict_iter,
                                            mainvolume_handlers[MAINVOLUME_HANDLER_CALL_STATE].property_name,
                                            DBUS_TYPE_STRING, &media_state);
    pa_dbus_append_basic_variant_dict_entry(&dict_iter,
                                            mainvolume_handlers[MAINVOLUME_HANDLER_MEDIA_STATE].property_name,
                                            DBUS_TYPE_STRING, &media_state);

    pa_log_debug("D-Bus: GetAll: revision %u, step count %u, current step %u, high volume step %u call state %s media state %s",
                 rev, step_count, current_step, high_volume_step, call_state, media_state);
    pa_assert_se(dbus_message_iter_close_container(&msg_iter, &dict_iter));
    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_message_unref(reply);
}

