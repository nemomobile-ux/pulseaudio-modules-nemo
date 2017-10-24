/***
  This file is part of PulseAudio.

  Copyright (C) 2013 Jolla Ltd.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/core.h>
#include <pulsecore/hook-list.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/refcnt.h>
#include <pulsecore/shared.h>
#include <pulsecore/core-util.h>
#include <pulse/utf8.h>
#include <pulse/proplist.h>

#include "shared-data.h"

enum shared_item_type {
    SHARED_ITEM_NONE,
    SHARED_ITEM_BOOL,
    SHARED_ITEM_STR,
    SHARED_ITEM_DATA,
    SHARED_ITEM_MAX
};

typedef struct shared_item {
    char *key;
    enum shared_item_type type;
    void *value;
    size_t nbytes;
    pa_hook changed_hook;
} shared_item;

struct pa_shared_data {
    PA_REFCNT_DECLARE;

    pa_core *core;
    pa_hashmap *items;
};

static void shared_item_free(shared_item *i);

static pa_shared_data* shared_data_new(pa_core *c) {
    pa_shared_data *t;

    pa_assert(c);

    t = pa_xnew0(pa_shared_data, 1);
    PA_REFCNT_INIT(t);
    t->core = c;
    t->items = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL, (pa_free_cb_t) shared_item_free);

    pa_assert_se(pa_shared_set(c, "shared-data-0", t) >= 0);

    return t;
}

pa_shared_data *pa_shared_data_get(pa_core *core) {
    pa_shared_data *t;

    if ((t = pa_shared_get(core, "shared-data-0")))
        return pa_shared_data_ref(t);

    return shared_data_new(core);
}

pa_shared_data *pa_shared_data_ref(pa_shared_data *t) {
    pa_assert(t);
    pa_assert(PA_REFCNT_VALUE(t) >= 1);

    PA_REFCNT_INC(t);

    return t;
}

static void shared_item_free(shared_item *i) {
    pa_hook_done(&i->changed_hook);
    pa_xfree(i->key);
    if (i->type == SHARED_ITEM_STR || i->type == SHARED_ITEM_DATA)
        pa_xfree(i->value);
    pa_xfree(i);
}

void pa_shared_data_unref(pa_shared_data *t) {
    pa_assert(t);
    pa_assert(PA_REFCNT_VALUE(t) >= 1);

    if (PA_REFCNT_DEC(t) > 0)
        return;

    pa_hashmap_free(t->items);

    pa_assert_se(pa_shared_remove(t->core, "shared-data-0") >= 0);

    pa_xfree(t);
}

static shared_item *item_get(pa_shared_data *t, pa_hashmap *items, const char *key) {
    shared_item *item;

    pa_assert(t);
    pa_assert(items);
    pa_assert(key);

    if (!(item = pa_hashmap_get(items, key))) {
        pa_log_debug("New shared item with key '%s'", key);

        item = pa_xnew0(shared_item, 1);
        item->key = pa_xstrdup(key);
        pa_hashmap_put(items, item->key, item);
        pa_hook_init(&item->changed_hook, t);
    }

    return item;
}

#define GETI(t, key)    \
    pa_assert(t);       \
    pa_assert(key);     \
    pa_assert_se((item = item_get(t, t->items, key)));


pa_hook_slot *pa_shared_data_connect(pa_shared_data *t, const char *key, pa_hook_cb_t callback, void *userdata) {
    shared_item *item;
    GETI(t, key);

    return pa_hook_connect(&item->changed_hook, PA_HOOK_NORMAL, callback, userdata);
}

void pa_shared_data_hook_slot_free(pa_hook_slot *slot) {
    pa_assert(slot);
    pa_hook_slot_free(slot);
}

int pa_shared_data_set_boolean(pa_shared_data *t, const char *key, bool value) {
    shared_item *item;
    bool changed = false;
    GETI(t, key);

    if (item->type != SHARED_ITEM_NONE && item->type != SHARED_ITEM_BOOL)
        return -1;

    if (item->type == SHARED_ITEM_NONE)
        changed = true;

    if (item->type == SHARED_ITEM_BOOL && value != !!PA_PTR_TO_UINT(item->value))
        changed = true;

    item->type = SHARED_ITEM_BOOL;
    item->value = PA_UINT_TO_PTR(value);
    item->nbytes = sizeof(void*);

    if (changed) {
        pa_log_debug("Shared item '%s' changes to bool value %s", item->key, value ? "true" : "false");
        pa_hook_fire(&item->changed_hook, item->key);
    }

    return 0;
}

bool pa_shared_data_get_boolean(pa_shared_data *t, const char *key) {
    shared_item *item;
    GETI(t, key);

    if (item->type == SHARED_ITEM_BOOL)
        return !!PA_PTR_TO_UINT(item->value);
    else if (item->type == SHARED_ITEM_NONE)
        return false;
    else if (item->value)
        return true;
    else
        return false;
}

static int shared_data_sets(pa_shared_data *t, const char *key, const char *value, bool fire_always) {
    shared_item *item;
    bool changed = true;

    pa_assert(key);
    pa_assert(value);

    if (!pa_proplist_key_valid(key) || !pa_utf8_valid(value))
        return -1;

    GETI(t, key);

    if (item->type != SHARED_ITEM_NONE && item->type != SHARED_ITEM_STR)
        return -1;

    if (item->value) {
        if (pa_streq(item->value, value))
            changed = false;
        else
            pa_xfree(item->value);
    }

    if (changed) {
        item->type = SHARED_ITEM_STR;
        item->value = pa_xstrdup(value);
        item->nbytes = strlen(value) + 1;
    }

    if (fire_always || changed) {
        pa_log_debug("Shared item '%s' changes to str value '%s'", item->key, (const char *) item->value);
        pa_hook_fire(&item->changed_hook, item->key);
    }

    return 0;
}

int pa_shared_data_sets_always(pa_shared_data *t, const char *key, const char *value) {
    return shared_data_sets(t, key, value, true);
}

int pa_shared_data_sets(pa_shared_data *t, const char *key, const char *value) {
    return shared_data_sets(t, key, value, false);
}

const char *pa_shared_data_gets(pa_shared_data *t, const char *key) {
    shared_item *item;

    GETI(t, key);

    if (item->type == SHARED_ITEM_STR)
        return (char *) item->value;
    else
        return NULL;
}

int pa_shared_data_setd(pa_shared_data *t, const char *key, const void *data, size_t nbytes) {
    shared_item *item;

    pa_assert(data || nbytes == 0);

    if (!pa_proplist_key_valid(key))
        return -1;

    GETI(t, key);

    if (item->value)
        pa_xfree(item->value);

    item->value = pa_xmalloc(nbytes + 1);
    if (nbytes > 0)
        memcpy(item->value, data, nbytes);

    ((char *) item->value)[nbytes] = 0;

    pa_log_debug("Shared item '%s' changes to data ptr from %p", item->key, (void *) data);
    pa_hook_fire(&item->changed_hook, item->key);

    return 0;
}

int pa_shared_data_getd(pa_shared_data *t, const char *key, const void **data, size_t *nbytes) {
    shared_item *item;

    pa_assert(key);
    pa_assert(data);
    pa_assert(nbytes);

    if (!pa_proplist_key_valid(key))
        return -1;

    GETI(t, key);

    *data = item->value;
    *nbytes = item->nbytes;

    return 0;
}

#undef GETI
