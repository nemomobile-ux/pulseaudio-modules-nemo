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

#include <pulsecore/core-util.h>
#include <pulsecore/namereg.h>

#include "common/proplist-meego.h"
#include "common/parameter-hook.h"
#include "common/parameter-modifier.h"
#include "voice/module-voice-api.h"

PA_MODULE_AUTHOR("Antti-Ville Jansson");
PA_MODULE_DESCRIPTION("Test module for parameters module");
PA_MODULE_USAGE("");
PA_MODULE_VERSION(PACKAGE_VERSION);

struct algorithm {
    char *mode;
    char *parameters;
    char *modified_parameters;
    meego_parameter_status_t status;
};

struct userdata {
    pa_sink *mode_sink;
    struct algorithm alg_a;
    struct algorithm alg_b;
    struct algorithm alg_c;
};

static void algorithm_reset(struct algorithm *alg) {
    if (alg->mode) {
        pa_xfree(alg->mode);
        alg->mode = NULL;
    }
    if (alg->parameters) {
        pa_xfree(alg->parameters);
        alg->parameters = NULL;
    }
}

static inline const char *status_to_string(meego_parameter_status_t status) {
    switch (status) {
        case MEEGO_PARAM_ENABLE:
            return "MEEGO_PARAM_ENABLE";
        case MEEGO_PARAM_DISABLE:
            return "MEEGO_PARAM_DISABLE";
        case MEEGO_PARAM_UPDATE:
            return "MEEGO_PARAM_UPDATE";
        case MEEGO_PARAM_MODE_CHANGE:
            return "MEEGO_PARAM_MODE_CHANGE";
        default:
            pa_assert_not_reached();
    }
}

static pa_hook_result_t parameters_changed_cb(pa_core *c, meego_parameter_update_args *ua, struct algorithm *alg) {
    pa_assert(c);
    pa_assert(ua);
    pa_assert(alg);

    alg->status = ua->status;

    algorithm_reset(alg);

    if (ua->mode)
        alg->mode = pa_xstrdup(ua->mode);

    if (ua->parameters) {
        pa_assert(ua->length > 0);
        alg->parameters = pa_xstrndup(ua->parameters, ua->length);
    }

    pa_log_debug("parameters_changed_cb, status=%s, mode=%s, params=%s", status_to_string(alg->status),
                                                                         pa_strnull(alg->mode),
                                                                         pa_strnull(alg->parameters));

    return PA_HOOK_OK;
}

static void switch_mode(struct userdata *u, const char *mode) {
    pa_proplist *proplist = pa_proplist_new();
    pa_log_debug("Switching to mode %s", mode);

    pa_proplist_sets(proplist, PA_NOKIA_PROP_AUDIO_MODE, mode);
    pa_proplist_sets(proplist, PA_NOKIA_PROP_AUDIO_ACCESSORY_HWID, "");
    pa_sink_update_proplist(u->mode_sink, PA_UPDATE_REPLACE, proplist);

    pa_proplist_free(proplist);
}

static void verify(struct algorithm *params, const char *mode, const char *params_str, meego_parameter_status_t expected_status) {

    pa_log_debug("Expected: status=%s, mode=%s, params=%s", status_to_string(expected_status),
                                                            pa_strnull(mode),
                                                            pa_strnull(params_str));

    pa_log_debug("Got: status=%s, mode=%s, params=%s", status_to_string(params->status),
                                                       pa_strnull(params->mode),
                                                       pa_strnull(params->parameters));

    if (mode) {
        pa_assert(params->mode);
        pa_assert(pa_streq(params->mode, mode));
    }
    if (params_str) {
        pa_assert(params->parameters);
        pa_assert(pa_streq(params->parameters, params_str));
    }
    pa_assert(params->status == expected_status);
}

static void disable_algs(struct userdata *u) {
    /* A hackish way of disabling all algs and setting all statuses to MEEGO_PARAM_DISABLE.
       mode_reset1 contains unique params for all algorithms while mode_reset2 contains nothing. */
    switch_mode(u, "mode_reset1");
    switch_mode(u, "mode_reset2");
}

static void run_basic_tests(struct userdata *u) {

    /* Subscribe to updates. Note that "alg_b" wants full updates. */
    meego_parameter_request_updates("alg_a", (pa_hook_cb_t)parameters_changed_cb, PA_HOOK_NORMAL, false, &u->alg_a);
    meego_parameter_request_updates("alg_b", (pa_hook_cb_t)parameters_changed_cb, PA_HOOK_NORMAL, true,  &u->alg_b);
    meego_parameter_request_updates("alg_c", (pa_hook_cb_t)parameters_changed_cb, PA_HOOK_NORMAL, false, &u->alg_c);

    disable_algs(u);

    /* Shuffle the modes around a bit to verify correct parameters and status enums */
    switch_mode(u, "mode_a");
    verify(&u->alg_a, "mode_a", "set_a1_parameters", MEEGO_PARAM_UPDATE);
    verify(&u->alg_b, "mode_a", "set_b1_parameters", MEEGO_PARAM_UPDATE);

    switch_mode(u, "mode_b");
    verify(&u->alg_a, "mode_b", "set_a2_parameters", MEEGO_PARAM_UPDATE);
    verify(&u->alg_b, "mode_b", NULL, MEEGO_PARAM_MODE_CHANGE);

    switch_mode(u, "mode_c");
    verify(&u->alg_a, "mode_c", "set_a3_parameters", MEEGO_PARAM_UPDATE);
    verify(&u->alg_b, "mode_c", "set_b2_parameters", MEEGO_PARAM_UPDATE);

    switch_mode(u, "mode_d");
    verify(&u->alg_a, "mode_d", NULL, MEEGO_PARAM_DISABLE);
    verify(&u->alg_b, "mode_d", NULL, MEEGO_PARAM_DISABLE);
    verify(&u->alg_c, "mode_d", "set_c1_parameters", MEEGO_PARAM_UPDATE);

    switch_mode(u, "mode_c");
    verify(&u->alg_a, "mode_c", NULL, MEEGO_PARAM_ENABLE);
    verify(&u->alg_b, "mode_c", NULL, MEEGO_PARAM_ENABLE);
    verify(&u->alg_c, "mode_c", NULL, MEEGO_PARAM_DISABLE);

    disable_algs(u); /* -> mode_reset2 */

    /* Stop updates to verify that it's really working */
    meego_parameter_stop_updates("alg_a", (pa_hook_cb_t)parameters_changed_cb, &u->alg_a);
    meego_parameter_stop_updates("alg_b", (pa_hook_cb_t)parameters_changed_cb, &u->alg_b);
    meego_parameter_stop_updates("alg_c", (pa_hook_cb_t)parameters_changed_cb, &u->alg_c);

    /* This mode switch should now have no effect */
    switch_mode(u, "mode_a");
    verify(&u->alg_a, "mode_reset2", NULL, MEEGO_PARAM_DISABLE);
    verify(&u->alg_b, "mode_reset2", NULL, MEEGO_PARAM_DISABLE);
    verify(&u->alg_c, "mode_reset2", NULL, MEEGO_PARAM_DISABLE);

    /* This should cause updates to mode_a */
    meego_parameter_request_updates("alg_a", (pa_hook_cb_t)parameters_changed_cb, PA_HOOK_NORMAL, false, &u->alg_a);
    meego_parameter_request_updates("alg_b", (pa_hook_cb_t)parameters_changed_cb, PA_HOOK_NORMAL, true,  &u->alg_b);
    meego_parameter_request_updates("alg_c", (pa_hook_cb_t)parameters_changed_cb, PA_HOOK_NORMAL, false, &u->alg_c);

    verify(&u->alg_a, "mode_a", "set_a1_parameters", MEEGO_PARAM_UPDATE);
    verify(&u->alg_b, "mode_a", "set_b1_parameters", MEEGO_PARAM_UPDATE);
    verify(&u->alg_c, "mode_a", NULL, MEEGO_PARAM_DISABLE);

    meego_parameter_stop_updates("alg_a", (pa_hook_cb_t)parameters_changed_cb, &u->alg_a);
    meego_parameter_stop_updates("alg_b", (pa_hook_cb_t)parameters_changed_cb, &u->alg_b);
    meego_parameter_stop_updates("alg_c", (pa_hook_cb_t)parameters_changed_cb, &u->alg_c);

    disable_algs(u);
}

static bool get_parameters_cb(const void *base_parameters, unsigned len_base_parameters,
                            void **parameters, unsigned *len_parameters, void *userdata) {

    struct algorithm *alg = (struct algorithm*)userdata;

    if (alg->modified_parameters) {
        pa_xfree(alg->modified_parameters);
        alg->modified_parameters = NULL;
    }

    /* As the modifier implementor, we own the modified data */
    if (base_parameters) {
        char *params = pa_xstrndup(base_parameters, len_base_parameters);
        alg->modified_parameters = pa_sprintf_malloc("%s-modified", params);
        pa_xfree(params);
    } else
        alg->modified_parameters = pa_xstrdup("modified");

    *parameters = alg->modified_parameters;
    *len_parameters = strlen(*parameters);

    return true;
}

static bool failing_get_parameters_cb(const void *base_parameters, unsigned len_base_parameters,
                            void **parameters, unsigned *len_parameters, void *userdata) {
    return false;
}

static void run_modifier_tests(struct userdata *u) {
    meego_parameter_modifier modifier_a;
    meego_parameter_modifier modifier_b;
    meego_parameter_modifier modifier_c;

    modifier_a.mode_name = "mode_a";
    modifier_a.algorithm_name = "alg_a";
    modifier_a.get_parameters = get_parameters_cb;
    modifier_a.userdata = &u->alg_a;

    modifier_b.mode_name = "mode_b";
    modifier_b.algorithm_name = "alg_b";
    modifier_b.get_parameters = failing_get_parameters_cb;
    modifier_b.userdata = &u->alg_b;

    /* This modifier doesn't have base parameters in the file system */
    modifier_c.mode_name = "mode_c";
    modifier_c.algorithm_name = "alg_c";
    modifier_c.get_parameters = get_parameters_cb;
    modifier_c.userdata = &u->alg_c;

    disable_algs(u);

    meego_parameter_request_updates("alg_a", (pa_hook_cb_t)parameters_changed_cb, PA_HOOK_NORMAL, false, &u->alg_a);
    meego_parameter_request_updates("alg_b", (pa_hook_cb_t)parameters_changed_cb, PA_HOOK_NORMAL, true,  &u->alg_b);
    meego_parameter_request_updates("alg_c", (pa_hook_cb_t)parameters_changed_cb, PA_HOOK_NORMAL, false, &u->alg_c);

    switch_mode(u, "mode_a");
    verify(&u->alg_a, "mode_a", "set_a1_parameters", MEEGO_PARAM_UPDATE);
    verify(&u->alg_b, "mode_a", "set_b1_parameters", MEEGO_PARAM_UPDATE);

    /* Register the modifiers. This should instantly trigger the modified update for "alg_a" since we're in "mode_a" */
    meego_parameter_register_modifier(&modifier_a);
    meego_parameter_register_modifier(&modifier_b);

    verify(&u->alg_a, "mode_a", "set_a1_parameters-modified", MEEGO_PARAM_UPDATE);
    verify(&u->alg_b, "mode_a", "set_b1_parameters", MEEGO_PARAM_UPDATE); /* Unchanged */

    switch_mode(u, "mode_b");
    verify(&u->alg_a, "mode_b", "set_a2_parameters", MEEGO_PARAM_UPDATE);
    verify(&u->alg_b, "mode_b", NULL, MEEGO_PARAM_MODE_CHANGE); /* The modifier fails, so this is the expected result */

    switch_mode(u, "mode_c");
    verify(&u->alg_a, "mode_c", "set_a3_parameters", MEEGO_PARAM_UPDATE);
    verify(&u->alg_b, "mode_c", "set_b2_parameters", MEEGO_PARAM_UPDATE);

    /* Let's unregister the modifiers and see that things work as before */
    meego_parameter_unregister_modifier(&modifier_a);
    meego_parameter_unregister_modifier(&modifier_b);

    switch_mode(u, "mode_a");
    verify(&u->alg_a, "mode_a", "set_a1_parameters", MEEGO_PARAM_UPDATE);
    verify(&u->alg_b, "mode_a", "set_b1_parameters", MEEGO_PARAM_UPDATE);

    /* Try the modifier that has no base parameters */
    meego_parameter_register_modifier(&modifier_c);

    switch_mode(u, "mode_c");
    verify(&u->alg_a, "mode_c", "set_a3_parameters", MEEGO_PARAM_UPDATE);
    verify(&u->alg_b, "mode_c", "set_b2_parameters", MEEGO_PARAM_UPDATE);
    verify(&u->alg_c, "mode_c", "modified", MEEGO_PARAM_UPDATE); /* Withóut the modifier this would be disabled */

    /* Now register a modifier when we're not in the affected mode */
    meego_parameter_register_modifier(&modifier_a);

    /* Switch to the affected mode and verify successful modification */
    switch_mode(u, "mode_a");
    verify(&u->alg_a, "mode_a", "set_a1_parameters-modified", MEEGO_PARAM_UPDATE);
    verify(&u->alg_b, "mode_a", "set_b1_parameters", MEEGO_PARAM_UPDATE);

    meego_parameter_unregister_modifier(&modifier_a);

    meego_parameter_stop_updates("alg_a", (pa_hook_cb_t)parameters_changed_cb, &u->alg_a);
    meego_parameter_stop_updates("alg_b", (pa_hook_cb_t)parameters_changed_cb, &u->alg_b);
    meego_parameter_stop_updates("alg_c", (pa_hook_cb_t)parameters_changed_cb, &u->alg_c);

    meego_parameter_unregister_modifier(&modifier_c);

    disable_algs(u);
}

int pa__init(pa_module *m) {
    struct userdata *u = NULL;

    u = pa_xnew0(struct userdata, 1);
    m->userdata = u;

    u->mode_sink = pa_namereg_get(m->core, "sink.hw0", PA_NAMEREG_SINK);
    if (!u->mode_sink) {
        pa_log_error("sink.hw0 not found");
        pa_xfree(u);
        m->userdata = NULL;
        return -1;
    }

    run_basic_tests(u);
    run_modifier_tests(u);

    pa_module_unload_request(m, true);
    return 0;
}

void pa__done(pa_module *m) {
    struct userdata *u = m->userdata;

    algorithm_reset(&u->alg_a);
    algorithm_reset(&u->alg_b);
    algorithm_reset(&u->alg_c);

    if (u->alg_a.modified_parameters)
        pa_xfree(u->alg_a.modified_parameters);
    if (u->alg_b.modified_parameters)
        pa_xfree(u->alg_b.modified_parameters);
    if (u->alg_c.modified_parameters)
        pa_xfree(u->alg_c.modified_parameters);


    if (u)
        pa_xfree(u);
}
