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

#define _GNU_SOURCE

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <pulsecore/llist.h>
#include <pulsecore/hook-list.h>
#include <pulsecore/core-util.h>

#include "module-meego-parameters-userdata.h"
#include "parameters.h"

#include <meego/parameter-hook-implementor.h>
#include <meego/parameter-modifier.h>

#include <meego/proplist-meego.h>

struct set {
    char *name;
    unsigned hash;
    void *data;
    unsigned length;

    PA_LLIST_FIELDS(struct set);
};

struct algorithm {
    char *name;
    unsigned hash;
    bool enabled:1;
    bool full_updates:1;
    bool fired:1;
    pa_hook hook;
    struct set *active_set;
    PA_LLIST_HEAD(struct set, sets);

    PA_LLIST_FIELDS(struct algorithm);
};

struct algorithm_enabler {
    struct algorithm *a;
    struct set *set;
    meego_parameter_modifier *modifier;

    PA_LLIST_FIELDS(struct algorithm_enabler);
};

struct mode {
    char *name;
    unsigned hash;
    PA_LLIST_HEAD(struct algorithm_enabler, algorithm_enablers);

    PA_LLIST_FIELDS(struct mode);
};

static char *readlink_malloc(const char *filename) {
    int size = 100;
    char *buffer = NULL;

    while (1) {
        buffer = (char *)realloc(buffer, size);
        int nchars = readlink(filename, buffer, size);
        if (nchars < 0) {
            free(buffer);
            return NULL;
        }
        if (nchars < (size - 1)) {
            buffer[nchars] = '\0';
            return buffer;
        }
        size *= 2;
    }
}

static char *set_readlink_abs(const char *path, const char *sym) {
    char *name;
    char *sym_value;
    char *abs_name;

    name = pa_sprintf_malloc("%s/%s", path, sym);
    sym_value = readlink_malloc(name);
    pa_xfree(name);

    name = pa_sprintf_malloc("%s/%s", path, sym_value);
    pa_xfree(sym_value);

    abs_name = canonicalize_file_name(name);
    pa_xfree(name);

    return abs_name;
}

static struct mode *find_mode_by_name(struct mode **m_head, const char *name) {
    struct mode *m;
    unsigned hash = pa_idxset_string_hash_func(name);

    PA_LLIST_FOREACH(m, *m_head) {
        if (hash == m->hash) {
            pa_assert(pa_streq(m->name, name));
            return m;
        }
    }

    return NULL;
}

static struct algorithm *find_algorithm_by_name(struct algorithm **a_head, const char *name) {
    struct algorithm *a;
    unsigned hash = pa_idxset_string_hash_func(name);

    PA_LLIST_FOREACH(a, *a_head) {
        if (hash == a->hash) {
            pa_assert(pa_streq(a->name, name));
            return a;
        }
    }

    return NULL;
}

static struct set *find_set_by_name(struct set **s_head, const char *name) {
    struct set *s;
    unsigned hash = pa_idxset_string_hash_func(name);

    PA_LLIST_FOREACH(s, *s_head) {
        if (hash == s->hash) {
            pa_assert(pa_streq(s->name, name));
            return s;
        }
    }

    return NULL;
}

static struct algorithm_enabler *find_enabler_by_name(struct algorithm_enabler **e_head, const char *name) {
    struct algorithm_enabler *e;
    unsigned hash = pa_idxset_string_hash_func(name);

    PA_LLIST_FOREACH(e, *e_head) {
        if (hash == e->a->hash) {
            pa_assert(pa_streq(e->a->name, name));
            return e;
        }
    }

    return NULL;
}

static int file_select(const struct dirent *entry) {
    return entry->d_name[0] != '.';
}

static void *read_parameters_from_file(const char *file, unsigned *length) {
    FILE *fp;
    char *s = NULL;
    struct stat buf;
    size_t c = 0;

    if (!stat(file, &buf)) {
        if ((fp = fopen(file, "r")) != NULL) {
            s = pa_xmalloc((size_t)(buf.st_size + sizeof(char)));
            c = fread(s, 1, (size_t)buf.st_size, fp);
            fclose(fp);

            pa_assert((size_t)buf.st_size ==  c);

            if (c == (size_t)buf.st_size)
                s[c] = '\0';
        }
    }

    *length = c;
    return s;
}

static void set_load(struct set *s) {
    pa_log_debug("Loading set %s ", s->name);
    pa_assert(!s->data);
    s->data = read_parameters_from_file(s->name, &s->length);
}

static void set_unload(struct set *s) {
    pa_log_debug("Unloading set %s", s->name);

    pa_xfree(s->data);
    s->data = NULL;
    s->length = 0;
}

static struct set *set_new(struct userdata *u, struct algorithm *a, const char *name) {
    struct set *s;

    s = pa_xnew(struct set, 1);
    s->name = pa_xstrdup(name);
    s->hash = pa_idxset_string_hash_func(name);
    s->data = NULL;
    s->length = 0;

    if (u->parameters.cache)
        set_load(s);

    pa_log_debug("Adding set: %s to algorithm: %s", s->name, a->name);
    PA_LLIST_PREPEND(struct set, a->sets, s);

    return s;
}

static void set_free(struct algorithm *a, struct set *s) {
    pa_log_debug("Removing set: %s from algorithm: %s", s->name, a->name);
    PA_LLIST_REMOVE(struct set, a->sets, s);

    if (s == a->active_set)
        a->active_set = NULL;

    pa_xfree(s->name);
    pa_xfree(s->data);
    pa_xfree(s);
}

static struct algorithm *algorithm_new(pa_core *c, struct algorithm **a_head, const char *name) {
    struct algorithm *a;

    pa_assert(name);

    a = pa_xnew(struct algorithm, 1);
    a->name = pa_xstrdup(name);
    a->hash = pa_idxset_string_hash_func(name);
    a->enabled = true;
    a->full_updates = false;
    pa_hook_init(&a->hook, c);
    a->active_set = NULL;
    PA_LLIST_HEAD_INIT(struct set, a->sets);

    pa_log_debug("Adding new algorithm: %s", a->name);
    PA_LLIST_PREPEND(struct algorithm, *a_head, a);

    return a;
}

static pa_hook_result_t algorithm_enable(struct userdata *u, struct algorithm *a) {
    meego_parameter_update_args ua;

    pa_assert(u);
    pa_assert(a);
    pa_assert(a->active_set);

    ua.mode = u->mode;
    ua.status = MEEGO_PARAM_ENABLE;
    ua.parameters = NULL;
    ua.length = 0;
    a->enabled = true;

    pa_log_debug("Enabling %s (%s)", a->name, a->active_set->name);

    return pa_hook_fire(&a->hook, &ua);
}

static pa_hook_result_t algorithm_disable(struct userdata *u, struct algorithm *a) {
    meego_parameter_update_args ua;

    pa_assert(u);
    pa_assert(a);

    ua.mode = u->mode;
    ua.status = MEEGO_PARAM_DISABLE;
    ua.parameters = NULL;
    ua.length = 0;
    a->enabled = false;

    pa_log_debug("Disabling %s (%s)", a->name, (a->active_set ? a->active_set->name : "not initialized"));

    return pa_hook_fire(&a->hook, &ua);
}

static void algorithm_free(struct userdata *u, struct algorithm **a_head, struct algorithm *a) {
    struct set *s;

    pa_assert(a);

    pa_log_debug("Removing algorithm: %s", a->name);
    PA_LLIST_REMOVE(struct algorithm, *a_head, a);

    algorithm_disable(u, a);

    while ((s = a->sets))
        set_free(a, s);

    pa_xfree(a->name);
    pa_hook_done(&a->hook);
    pa_xfree(a);
}

static void mode_update(struct userdata *u) {
    meego_parameter_update_args ua;

    pa_assert(u);

    if (u->mode) {
        ua.mode = u->mode;
        ua.status = MEEGO_PARAM_MODE_CHANGE;
        ua.parameters = NULL;
        ua.length = 0;

        pa_hook_fire(&u->mode_hook, &ua);
    }
}

static void algorithm_mode_update(struct userdata *u, struct algorithm *a) {
    meego_parameter_update_args ua;

    pa_assert(u);
    pa_assert(u->mode);
    pa_assert(a);

    ua.mode = u->mode;
    ua.status = MEEGO_PARAM_MODE_CHANGE;

    if (a->enabled && a->active_set) {
        ua.parameters = a->active_set->data;
        ua.length = a->active_set->length;
    } else {
        ua.parameters = NULL;
        ua.length = 0;
    }

    pa_log_debug("Mode update for %s (%s)",
                 a->name,
                 ((a->enabled && a->active_set) ? a->active_set->name : "disabled"));

    pa_hook_fire(&a->hook, &ua);
}

/* Update an algorithm using a modifier, if possible.
 *
 * Currently, the algorithm is always updated when using a modifier (as opposed
 * to the active_set checks done with regular file system parameters). Thus,
 * it's possible that two different modifiers are used for consecutive modes
 * and they return the exact same data and the algorithm will still be updated.
 * Such optimizations are however not currently needed.
 **/
static bool algorithm_modified_update(struct userdata *u, struct algorithm *a, struct algorithm_enabler *e) {
    meego_parameter_update_args ua;
    void *parameters = NULL;
    void *base_parameters = NULL;
    unsigned len_base_parameters = 0;
    bool updated = false;
    meego_parameter_modifier *modifier = NULL;

    pa_assert(u);
    pa_assert(a);
    pa_assert(e);

    modifier = e->modifier;

    if (!modifier)
        return false;

    if (e->set) {
        if (!u->parameters.cache)
            set_load(e->set);
        base_parameters = e->set->data;
        len_base_parameters = e->set->length;
    }

    pa_log_debug("Updating algorithm %s in mode %s with modifier %p", modifier->algorithm_name,
                                                                      modifier->mode_name,
                                                                      (void*)modifier);

    /* Modifiers do not necessarily need base parameters. Also, we need to
     * enable situations where an algorithm is only enabled if it has a
     * modifier (i.e. no parameters from the file system). Thus, modifiers must
     * also be able to handle NULL base parameters (at least by failing gracefully). */
    updated = modifier->get_parameters(base_parameters, len_base_parameters,
                                       &parameters, &ua.length,
                                       modifier->userdata);

    if (updated) {
        ua.mode = u->mode;
        ua.status = MEEGO_PARAM_UPDATE;
        ua.parameters = parameters;
        pa_assert(ua.parameters && ua.length > 0);
        a->enabled = true;
        a->active_set = NULL;
        pa_hook_fire(&a->hook, &ua);
        pa_log_debug("Update from modifier successful");
    } else
        pa_log_warn("Update from modifier failed");

    if (e->set && !u->parameters.cache)
        set_unload(e->set);

    return updated;
}

static pa_hook_result_t algorithm_update(struct userdata *u, struct algorithm *a, struct set *s) {
    meego_parameter_update_args ua;
    pa_hook_result_t r;

    a->active_set = s;

    if (!a->active_set) {
        pa_log_warn("No active set for %s, can't update", a->name);
        return PA_HOOK_OK;
    }

    if (!u->parameters.cache)
       set_load(s);

    ua.mode = u->mode;
    ua.status = MEEGO_PARAM_UPDATE;
    ua.parameters = s->data;
    ua.length = s->length;
    a->enabled = true;

    pa_log_debug("Updating %s with %s", a->name, s->name);

    r = pa_hook_fire(&a->hook, &ua);

    if (!u->parameters.cache)
        set_unload(s);

    return r;
}

int algorithm_reload(struct userdata *u, const char *alg) {
    struct mode *m;
    struct algorithm *a;
    struct set *s;
    struct algorithm_enabler *e;
    char *path;
    const char *setname;

    pa_assert(u);
    pa_assert(alg);

    pa_log_debug("Reloading %s", alg);

    if ((a = find_algorithm_by_name(&u->parameters.algorithms, alg)) == NULL) {
        pa_log_warn("Can not reload %s, not found", alg);
        return -1;
    }

    while ((s = a->sets))
        set_free(a, s);

    PA_LLIST_FOREACH(m, u->parameters.modes) {
        if ((e = find_enabler_by_name(&m->algorithm_enablers, alg)) != NULL) {
            PA_LLIST_REMOVE(struct algorithm_enabler, m->algorithm_enablers, e);

            path = pa_sprintf_malloc("%s/modes/%s", u->parameters.directory, m->name);
            if ((setname = set_readlink_abs(path, alg)) != NULL) {
                e = pa_xnew(struct algorithm_enabler, 1);
                e->a = a;

                if ((e->set = find_set_by_name(&a->sets, setname)) == NULL)
                    e->set = set_new(u, a, setname);
                else
                    pa_log_debug("%s set: %s already loaded", a->name, e->set->name);

                if (m->hash == u->hash)
                    algorithm_update(u, a, e->set);

                PA_LLIST_PREPEND(struct algorithm_enabler, m->algorithm_enablers, e);
            } else {
                pa_log_warn("%s reload failed in mode %s", alg, m->name);
            }
            pa_xfree(path);
        }
    }

    return 0;
}

static struct mode *mode_new(struct mode **m_head, const char *name) {
    struct mode *m;

    m = pa_xnew0(struct mode, 1);
    PA_LLIST_INIT(struct mode, m);
    m->name = pa_xstrdup(name);
    m->hash = pa_idxset_string_hash_func(name);
    PA_LLIST_HEAD_INIT(struct algorithm_enabler, m->algorithm_enablers);

    pa_log_debug("Adding new mode: %s", m->name);
    PA_LLIST_PREPEND(struct mode, *m_head, m);

    return m;
}

static void mode_free(struct userdata *u, struct mode **m_head, struct mode *m) {
    struct algorithm_enabler *e;

    pa_log_debug("Removing mode: %s", m->name);
    PA_LLIST_REMOVE(struct mode, *m_head, m);

    while ((e = m->algorithm_enablers)) {
        if (pa_streq(m->name, u->mode))
            algorithm_disable(u, e->a);

        pa_log_debug("Removing enabler: %s from mode: %s", e->a->name, m->name);
        PA_LLIST_REMOVE(struct algorithm_enabler, m->algorithm_enablers, e);
        pa_xfree(e);
    }

    pa_xfree(m->name);
    pa_xfree(m);
}

static struct mode *add_mode(struct userdata *u, const char *mode) {
    struct dirent **namelist;
    int n;
    struct mode *m;
    struct algorithm *a;
    struct algorithm_enabler *e;
    char *path;
    char *sym;
    char *setname;

    m = mode_new(&u->parameters.modes, mode);

    path = pa_sprintf_malloc("%s/modes/%s", u->parameters.directory, mode);

    pa_log_debug("Scanning mode from %s", path);

    if ((n = scandir(path, &namelist, file_select, alphasort)) >= 0) {

        while (n--) {
            sym = namelist[n]->d_name;
            pa_log_debug("Checking symlink value %s", sym);

            if ((setname = set_readlink_abs(path, sym)) != NULL) {
                if ((a = find_algorithm_by_name(&u->parameters.algorithms, sym)) == NULL)
                    a = algorithm_new(u->core, &u->parameters.algorithms, sym);

                e = pa_xnew0(struct algorithm_enabler, 1);
                e->a = a;

                if ((e->set = find_set_by_name(&a->sets, setname)) == NULL)
                    e->set = set_new(u, a, setname);
                else
                    pa_log_debug("%s set: %s already loaded", a->name, e->set->name);

                PA_LLIST_PREPEND(struct algorithm_enabler, m->algorithm_enablers, e);
                pa_log_debug("Enabling %s in %s mode", a->name, mode);

                pa_xfree(setname);
            } else {
                pa_log_debug("%s is not a symlink", sym);
            }
            pa_xfree(namelist[n]);
        }
        pa_xfree(namelist);
    } else {
        mode_free(u, &u->parameters.modes, m);
        m = NULL;
    }

    pa_xfree(path);

    return m;
}

int update_mode(struct userdata *u, const char *mode) {
    struct mode *m = find_mode_by_name(&u->parameters.modes, mode);

    if (!m)
        return -1;

    mode_free(u, &u->parameters.modes, m);

    if ((m = add_mode(u, mode)) == NULL)
        return -1;

    u->hash = 0;

    return switch_mode(u, mode);
}

static pa_hook_result_t update_requests(pa_core *c, meego_parameter_connection_args *args, struct userdata *u) {
    struct algorithm *a = NULL;
    struct mode *m = NULL;
    struct algorithm_enabler *e = NULL;

    pa_assert(c);
    pa_assert(args);
    pa_assert(u);

    if (args->name == NULL) {
        pa_hook_connect(&u->mode_hook, args->prio, args->cb, args->userdata);

        mode_update(u);

        return PA_HOOK_OK;
    }

    if ((a = find_algorithm_by_name(&u->parameters.algorithms, args->name)) == NULL)
        a = algorithm_new(c, &u->parameters.algorithms, args->name);

    a->full_updates = args->full_updates;

    pa_hook_connect(&a->hook, args->prio, args->cb, args->userdata);

    pa_log_debug("Update hook connected for %s", args->name);

    if (u->mode && (m = find_mode_by_name(&u->parameters.modes, u->mode)))
        e = find_enabler_by_name(&m->algorithm_enablers, args->name);

    if (e) {
        if (!algorithm_modified_update(u, a, e))
            algorithm_update(u, a, a->active_set);
    } else
        algorithm_disable(u, a);

    return PA_HOOK_OK;
}

static pa_hook_result_t stop_requests(pa_core *c, meego_parameter_connection_args *args, struct userdata *u) {
    struct algorithm *a = NULL;
    pa_hook_slot *slot = NULL;

    pa_assert(c);
    pa_assert(args);
    pa_assert(u);

    if (args->name == NULL)
        slot = u->mode_hook.slots;
    else if ((a = find_algorithm_by_name(&u->parameters.algorithms, args->name)) != NULL)
        slot = a->hook.slots;

    while (slot) {
        if (slot->callback == args->cb && slot->data == args->userdata) {
            pa_hook_slot_free(slot);
            pa_log_debug("Stopped requests for %s.", args->name ? args->name : "mode hook");
            return PA_HOOK_OK;
        }
        slot = slot->next;
    }

    pa_log_error("Unable to stop requests for %s. No hook registered.",
                 args->name ? args->name : "mode hook");

    return PA_HOOK_OK;
}

static pa_hook_result_t register_modifier(pa_core *c, meego_parameter_modifier *modifier, struct userdata *u) {
    struct algorithm *a = NULL;
    struct mode *m = NULL;
    struct algorithm_enabler *e = NULL;

    pa_assert(c);
    pa_assert(u);
    pa_assert(modifier);
    pa_assert(modifier->get_parameters);
    pa_assert(modifier->mode_name);
    pa_assert(modifier->algorithm_name);

    if ((m = find_mode_by_name(&u->parameters.modes, modifier->mode_name)) == NULL) {
        if ((m = add_mode(u, modifier->mode_name)) == NULL) {
            pa_log_error("Could not add mode %s", modifier->mode_name);
            return PA_HOOK_OK;
        }
    }

    if ((a = find_algorithm_by_name(&u->parameters.algorithms, modifier->algorithm_name)) == NULL)
        a = algorithm_new(c, &u->parameters.algorithms, modifier->algorithm_name);

    if ((e = find_enabler_by_name(&m->algorithm_enablers, modifier->algorithm_name)) == NULL) {
        e = pa_xnew0(struct algorithm_enabler, 1);
        e->a = a;
        e->set = NULL;
        PA_LLIST_PREPEND(struct algorithm_enabler, m->algorithm_enablers, e);
    }

    /* Only one modifier allowed for each (mode, algorithm) pair (more wouldn't make sense) */
    if (e->modifier) {
        pa_log_error("Cannot register modifier. Modifier %p already registered for mode %s, algorithm %s",
                     (void*)e->modifier, modifier->mode_name, modifier->algorithm_name);
        return PA_HOOK_OK;
    }

    e->modifier = modifier;

    pa_log_debug("Registered modifier for algorithm %s in mode %s", modifier->algorithm_name, modifier->mode_name);

    /* Update the relevant algorithm immediately if we happen to be in the
     * relevant mode already and the algorithm exists */
    if (pa_streq(u->mode, modifier->mode_name) && a->hook.slots)
        algorithm_modified_update(u, a, e);

    return PA_HOOK_OK;
}

static pa_hook_result_t unregister_modifier(pa_core *c, meego_parameter_modifier *modifier, struct userdata *u) {
    struct mode *m = NULL;
    struct algorithm_enabler *e = NULL;

    pa_assert(c);
    pa_assert(modifier);
    pa_assert(u);

    if ((m = find_mode_by_name(&u->parameters.modes, modifier->mode_name)))
        e = find_enabler_by_name(&m->algorithm_enablers, modifier->algorithm_name);

    if (!e || !e->modifier) {
        pa_log_warn("No modifier exists for algorithm %s, mode %s", modifier->algorithm_name, modifier->mode_name);
        return PA_HOOK_OK;
    }

    if (e->modifier != modifier) {
        pa_log_warn("Different modifier %p registered for algorithm %s, mode %s", (void*)e->modifier,
                                                                                  modifier->algorithm_name,
                                                                                  modifier->mode_name);
        return PA_HOOK_OK;
    }

    e->modifier = NULL; /* The modifier provider is responsible for deletion */

    /* Remove the enabler if it was solely using the modifier (i.e. no params from file) */
    if (!e->set) {
        PA_LLIST_REMOVE(struct algorithm_enabler, m->algorithm_enablers, e);
        pa_xfree(e);
    }

    pa_log_debug("Unregistered modifier for algorithm %s in mode %s", modifier->algorithm_name, modifier->mode_name);

    return PA_HOOK_OK;
}

int initme(struct userdata *u, const char *initial_mode) {
    PA_LLIST_HEAD_INIT(struct mode, u->parameters.modes);
    PA_LLIST_HEAD_INIT(struct algorithm, u->parameters.algorithms);

    u->implementor_args.update_request_cb = (pa_hook_cb_t)update_requests;
    u->implementor_args.stop_request_cb = (pa_hook_cb_t)stop_requests;
    u->implementor_args.modifier_registration_cb = (pa_hook_cb_t)register_modifier;
    u->implementor_args.modifier_unregistration_cb = (pa_hook_cb_t)unregister_modifier;

    meego_parameter_receive_requests(u->core, &u->implementor_args, u);
    pa_log_debug("Connected to update requests %p", (void*)u->implementor_args.update_request_slot);
    pa_log_debug("Connected to modifier registrations %p", (void*)u->implementor_args.modifier_registration_slot);
    pa_log_debug("Connected to modifier unregistrations %p", (void*)u->implementor_args.modifier_unregistration_slot);

    u->hash = 0;
    u->mode = NULL;

    /* Let's start in the initial mode */
    return switch_mode(u, initial_mode);
}

void unloadme(struct userdata *u) {
    struct mode *m;
    struct algorithm *a;

    pa_assert(u);

    meego_parameter_discontinue_requests(&u->implementor_args);

    if (u->parameters.directory)
        pa_xfree((void*)u->parameters.directory);

    while ((m = u->parameters.modes))
        mode_free(u, &u->parameters.modes, m);

    while ((a = u->parameters.algorithms))
        algorithm_free(u, &u->parameters.algorithms, a);
}

int switch_mode(struct userdata *u, const char *mode) {
    struct mode *m;
    struct algorithm *a;
    struct algorithm_enabler *e;
    unsigned hash = pa_idxset_string_hash_func(mode);

    if (hash == u->hash)
        return 0;

    if ((m = find_mode_by_name(&u->parameters.modes, mode)) == NULL)
        m = add_mode(u, mode);

    if (!m) {
        pa_log_error("No such mode: %s", mode);
        return -1;
    }

    u->hash = hash;

    if (u->mode)
        pa_xfree((void*)u->mode);

    u->mode = pa_xstrdup(mode);

    pa_log_debug("Checking mode: %s", mode);

    PA_LLIST_FOREACH(e, m->algorithm_enablers) {
        a = e->a;

        pa_assert(e->set || e->modifier);

        if (!a->hook.slots) {
            /* If no one is listening to parameter updates, we need
             * still update algorithm active_set so that when parameter updates
             * are requested later in update_requests() active set arguments
             * are passed to caller. */
            a->active_set = e->set;
            pa_log_debug("No one listening %s updates", a->name);
            continue;
        }

        if (algorithm_modified_update(u, a, e)) /* Try to use a modifier first. */
            pa_log_debug("Updated from modifier");
        else if (!e->set) {
            pa_log_error("Modifier failed and no parameters available. Disabling %s", a->name);
            continue;
        } else if (e->set != a->active_set)
            algorithm_update(u, a, e->set);
        else if (!a->enabled)
            algorithm_enable(u, a);
        else if (a->full_updates)
            algorithm_mode_update(u, a);
        else
            pa_log_debug("Not updating %s (%s)", a->name, a->active_set->name);

        pa_assert((!a->active_set && e->modifier) || (a->active_set && e->set == a->active_set));

        a->fired = true;
    }

    PA_LLIST_FOREACH(a, u->parameters.algorithms) {
        if (a->fired == false && a->enabled == true)
            algorithm_disable(u, a);
        else if (a->fired == false && a->full_updates)
            algorithm_mode_update(u, a);

        a->fired = false;
    }

    mode_update(u);

    return 0;
}
