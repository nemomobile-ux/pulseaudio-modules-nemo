/*
 * Copyright (C) 2013 Jolla Ltd.
 *
 * Contact: Juho Hämäläinen <juho.hamalainen@tieto.com>
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

#include <pulse/timeval.h>
#include <pulse/rtclock.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/database.h>
#include <pulsecore/log.h>

#include "listening-watchdog.h"

#define LISTENING_WATCHDOG_DB_NAME "mainvolume-listening-watchdog-0"
#define WD_DB_ENTRY "c"

struct mv_listening_watchdog {
    pa_core *core;

    pa_database *db;
    bool initial_notify;
    pa_usec_t timeout;
    pa_usec_t start_time;
    pa_usec_t counter;

    pa_time_event *timer_event;

    mv_listening_watchdog_notify_cb_t notify_cb;
    void *userdata;
};

static pa_usec_t read_counter_value(pa_database *db) {
    pa_datum key;
    pa_datum data;
    pa_usec_t counter = 0;

    pa_assert(db);

    key.data = (void*) WD_DB_ENTRY;
    key.size = strlen(WD_DB_ENTRY);

    if (pa_database_get(db, &key, &data)) {
        counter = *(pa_usec_t *) data.data;
        pa_datum_free(&data);
    }

    pa_log_debug("Restore counter value %llu minutes (%llu seconds)", (long long unsigned) (counter / PA_USEC_PER_SEC / 60),
                                                                      (long long unsigned) (counter / PA_USEC_PER_SEC));

    return counter;
}

static void write_counter_value(pa_database *db, pa_usec_t counter) {
    pa_datum key;
    pa_datum data;

    pa_assert(db);

    key.data = (void*) WD_DB_ENTRY;
    key.size = strlen(WD_DB_ENTRY);

    data.data = &counter;
    data.size = sizeof(pa_usec_t);

    pa_assert_se((pa_database_set(db, &key, &data, true) == 0));
    pa_log_debug("Store counter value %llu minutes (%llu seconds)", (long long unsigned) (counter / PA_USEC_PER_SEC / 60),
                                                                    (long long unsigned) (counter / PA_USEC_PER_SEC));
}

mv_listening_watchdog* mv_listening_watchdog_new(pa_core *core,
                                                 mv_listening_watchdog_notify_cb_t cb,
                                                 pa_usec_t timeout,
                                                 void *userdata) {
    pa_database *db;
    char *fname = NULL;
    mv_listening_watchdog *wd = NULL;

    pa_assert(core);
    pa_assert(cb);
    pa_assert(timeout > 0);

    if (!(fname = pa_state_path(LISTENING_WATCHDOG_DB_NAME, true))) {
        pa_log("Failed to open watchdog database: couldn't get state path");
        goto end;
    }
    if (!(db = pa_database_open_internal(fname, true))) {
        pa_log("Failed to open watchdog database: %s", pa_cstrerror(errno));
        goto end;
    }

    wd = pa_xnew0(mv_listening_watchdog, 1);
    wd->db = db;
    wd->counter = read_counter_value(wd->db);
    wd->core = core;
    wd->notify_cb = cb;
    wd->userdata = userdata;
    wd->timeout = timeout * PA_USEC_PER_SEC * 60;
    wd->initial_notify = true;

    if (wd->counter > wd->timeout)
        wd->counter = 0;

end:
    pa_xfree(fname);
    return wd;
}

void mv_listening_watchdog_free(mv_listening_watchdog *wd) {
    pa_assert(wd);

    mv_listening_watchdog_pause(wd);
    if (wd->db) {
        write_counter_value(wd->db, wd->counter);
        pa_database_close(wd->db);
    }
    pa_xfree(wd);
}

static void timer_event_cb(pa_mainloop_api *a, pa_time_event *e, const struct timeval *t, void *userdata) {
    mv_listening_watchdog *wd = userdata;

    pa_assert(a);
    pa_assert(e);
    pa_assert(wd);

    pa_assert(e == wd->timer_event);
    mv_listening_watchdog_pause(wd);
    mv_listening_watchdog_reset(wd);

    pa_log_debug("Listening watchdog notify event.");
    wd->notify_cb(wd, false, wd->userdata);
}

void mv_listening_watchdog_start(mv_listening_watchdog *wd) {
    pa_usec_t now, timer;

    pa_assert(wd);

    if (mv_listening_watchdog_running(wd))
        return;

    now = pa_rtclock_now();
    wd->start_time = now;
    timer = now + (wd->timeout - wd->counter);

    if (wd->initial_notify) {
        wd->notify_cb(wd, true, wd->userdata);
        wd->initial_notify = false;
    }

    wd->timer_event = pa_core_rttime_new(wd->core, timer, timer_event_cb, wd);
}

void mv_listening_watchdog_reset(mv_listening_watchdog *wd) {
    bool running;

    pa_assert(wd);

    running = mv_listening_watchdog_running(wd);
    mv_listening_watchdog_pause(wd);
    wd->counter = 0;
    if (running)
        mv_listening_watchdog_start(wd);
}

void mv_listening_watchdog_pause(mv_listening_watchdog *wd) {
    pa_assert(wd);

    if (!mv_listening_watchdog_running(wd))
        return;

    wd->core->mainloop->time_free(wd->timer_event);
    wd->timer_event = NULL;
    wd->counter += pa_rtclock_now() - wd->start_time;
}

bool mv_listening_watchdog_running(mv_listening_watchdog *wd) {
    pa_assert(wd);

    if (wd->timer_event)
        return true;
    else
        return false;
}
