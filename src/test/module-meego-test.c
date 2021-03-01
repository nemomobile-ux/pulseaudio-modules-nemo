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
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pulse/xmalloc.h>
#include <pulse/proplist.h>

#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/namereg.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>

#include "proplist-meego.h"
#include "shared-data.h"
#include "proplist-nemo.h"

PA_MODULE_AUTHOR("Pekka Ervasti");
PA_MODULE_DESCRIPTION("test module");
PA_MODULE_USAGE(
        "op=<test operation, mode/si/proplist/call> "
        "sink_name=<name of hw sink> "
        "audio_mode=<ihf,hs,etc> "
        "active=call active <true/false> "
        "hwid=<accessory hwid> "
        "property=<property key to change> "
        "value=<property value to change>");
PA_MODULE_VERSION(PACKAGE_VERSION);

static const char* const valid_modargs[] = {
    "op",
    "sink_name",
    "audio_mode",
    "call",
    "active",
    "hwid",
    "property",
    "value",
    "cork",
    "uncork",
    "sink-input",
    NULL,
};

#define OP_AUDIO_MODE "mode"
#define OP_SINK_INPUT "si"
#define OP_PROPLIST "proplist"
#define OP_CALL "call"
#define OP_CORK "cork"
#define OP_UNCORK "uncork"

struct userdata {
    pa_core *core;
    pa_module *module;

    /* for sink-input test */
    pa_subscription *subscription;
};

/* copy/pasted from module-stream-restore.c */
#define IDENTIFICATION_PROPERTY "module-stream-restore.id"
static char *get_name(pa_proplist *p, const char *prefix) {
    const char *r;
    char *t;

    if (!p)
        return NULL;

    if ((r = pa_proplist_gets(p, IDENTIFICATION_PROPERTY)))
        return pa_xstrdup(r);

    if ((r = pa_proplist_gets(p, PA_PROP_MEDIA_ROLE)))
        t = pa_sprintf_malloc("%s-by-media-role:%s", prefix, r);
    else if ((r = pa_proplist_gets(p, PA_PROP_APPLICATION_ID)))
        t = pa_sprintf_malloc("%s-by-application-id:%s", prefix, r);
    else if ((r = pa_proplist_gets(p, PA_PROP_APPLICATION_NAME)))
        t = pa_sprintf_malloc("%s-by-application-name:%s", prefix, r);
    else if ((r = pa_proplist_gets(p, PA_PROP_MEDIA_NAME)))
        t = pa_sprintf_malloc("%s-by-media-name:%s", prefix, r);
    else
        t = pa_sprintf_malloc("%s-fallback:%s", prefix, r);

    pa_proplist_sets(p, IDENTIFICATION_PROPERTY, t);
    return t;
}

static void test_sink_input_subscribe_cb(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    pa_sink_input *si = NULL;
    char *name;
    char tmp[256];
    pa_cvolume absolute_volume;
    pa_cvolume reference_ratio;

    if (t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE))
        return;

    if (!(si = pa_idxset_get_by_index(c->sink_inputs, idx)))
        return;

    if (!(name = get_name(si->proplist, "sink-input")))
        return;

    pa_sink_input_get_volume(si, &reference_ratio, false);
    pa_sink_input_get_volume(si, &absolute_volume, true);

    pa_log_debug("sink-input: %s reference ratio: %s absolute volume: %s",
            name,
            pa_cvolume_snprint(tmp, sizeof(tmp), &reference_ratio),
            pa_cvolume_snprint(tmp, sizeof(tmp), &absolute_volume));

    pa_xfree(name);
}

static void test_sink_input(struct userdata *u) {
    u->subscription = pa_subscription_new(u->core, PA_SUBSCRIPTION_MASK_SINK_INPUT, test_sink_input_subscribe_cb, u);
    pa_log_debug("Setting up subscription for sink-input");
}

static void test_call(struct userdata *u, pa_modargs *ma) {
    pa_shared_data *shared;
    bool active;

    shared = pa_shared_data_get(u->core);

    pa_assert(shared);

    if (pa_modargs_get_value_boolean(ma, "active", &active) < 0)
        pa_log_error("call op (active) expects boolean argument");
    else
        pa_shared_data_sets(shared, PA_NEMO_PROP_CALL_STATE, active ? PA_NEMO_PROP_CALL_STATE_ACTIVE : PA_NEMO_PROP_CALL_STATE_INACTIVE);

    pa_shared_data_unref(shared);
}

static void test_cork(struct userdata *u, pa_modargs *ma, bool corked) {
    uint32_t idx;
    pa_sink_input *si;

    if (pa_modargs_get_value_u32(ma, "sink-input", &idx) < 0)
        pa_log_error("%scork op (sink-input) expects unsigned argument", corked ? "" : "un");
    else {
        if (!(si = pa_idxset_get_by_index(u->core->sink_inputs, idx)))
            pa_log_error("no sink-input found with idx %u", idx);
        else
            pa_sink_input_send_event(si, corked ? PA_STREAM_EVENT_REQUEST_CORK : PA_STREAM_EVENT_REQUEST_UNCORK, NULL);
    }

}

static void test_proplist(struct userdata *u, pa_modargs *ma) {
    const char *sink_name;
    const char *property;
    const char *value;
    pa_sink *s;
    pa_proplist *p;

    sink_name = pa_modargs_get_value(ma, "sink_name", NULL);
    s = pa_namereg_get(u->core, sink_name, PA_NAMEREG_SINK);

    if (!s) {
        pa_log("No such sink: %s", sink_name);
        return;
    }

    pa_sink_ref(s);

    property = pa_modargs_get_value(ma, "property", NULL);
    value = pa_modargs_get_value(ma, "value", NULL);

    if (!property) {
        pa_log("No property defined");
        return;
    }
    if (!value) {
        pa_log("No value defined!");
        return;
    }

    p = pa_proplist_new();

    pa_proplist_sets(p, property, value);

    pa_proplist_update(s->proplist, PA_UPDATE_REPLACE, p);
    pa_log_debug("for sink %s set %s = %s", s->name, property, value);

    if (PA_SINK_IS_LINKED(s->state)) {
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SINK_PROPLIST_CHANGED], s);
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
    }

    pa_proplist_free(p);
    pa_sink_unref(s);
}

static void test_audio_mode(struct userdata *u, pa_modargs *ma) {
    const char *sink_name;
    const char *audio_mode;
    const char *hwid;
    pa_sink *hw_sink;
    pa_proplist *p;

    sink_name = pa_modargs_get_value(ma, "sink_name", NULL);
    hw_sink = pa_namereg_get(u->core, sink_name, PA_NAMEREG_SINK);

    if (!hw_sink) {
        pa_log("No such sink: %s", sink_name);
        return;
    }

    audio_mode = pa_modargs_get_value(ma, "audio_mode", "ihf");
    hwid = pa_modargs_get_value(ma, "hwid", "");

    p = pa_proplist_new();

    pa_proplist_sets(p, PA_NOKIA_PROP_AUDIO_MODE, audio_mode);
    pa_proplist_sets(p, PA_NOKIA_PROP_AUDIO_ACCESSORY_HWID, hwid);

    pa_sink_update_proplist(hw_sink, PA_UPDATE_REPLACE, p);
    pa_proplist_free(p);
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    const char *op;

    u = pa_xmalloc(sizeof(struct userdata));

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u;
    u->core = m->core;
    u->module = m;

    op = pa_modargs_get_value(ma, "op", NULL);
    if (!op) {
        pa_log("Operation not defined!");
        goto fail;
    }

    if (pa_streq(op, OP_AUDIO_MODE))
        test_audio_mode(u, ma);
    else if (pa_streq(op, OP_SINK_INPUT))
        test_sink_input(u);
    else if (pa_streq(op, OP_PROPLIST))
        test_proplist(u, ma);
    else if (pa_streq(op, OP_CALL))
        test_call(u, ma);
    else if (pa_streq(op, OP_CORK))
        test_cork(u, ma, true);
    else if (pa_streq(op, OP_UNCORK))
        test_cork(u, ma, false);

    /* unload test module immediately, as the work is now done. */
    pa_module_unload_request(u->module, true);
    pa_modargs_free(ma);

    return 0;

 fail:
    if (ma)
        pa_modargs_free(ma);

    pa_xfree(u);
    m->userdata = NULL;

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u = m->userdata;
    assert(m);

    if (!u)
        return;

    pa_xfree(u);
}
