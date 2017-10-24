#ifndef fooshareddatafoo
#define fooshareddatafoo

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

#include <pulsecore/core.h>
#include <pulsecore/hook-list.h>

typedef struct pa_shared_data pa_shared_data;

pa_shared_data *pa_shared_data_get(pa_core *core);
pa_shared_data *pa_shared_data_ref(pa_shared_data *t);
void pa_shared_data_unref(pa_shared_data *t);

/* Set boolean value to shared item. Fire value changed hook if value has changed from previous value.
 * Return 0 on success, -1 on failure */
int pa_shared_data_set_boolean(pa_shared_data *t, const char *key, bool value);
/* Get boolean value from shared item. If shared item exists but is of different type than BOOLEAN, return true.
 * If shared item doesn't exist, returns false. */
bool pa_shared_data_get_boolean(pa_shared_data *t, const char *key);

/* Set integer value to shared item. If item with identical key already exists and has different
 * type, return -1. If key is invalid, return -1. Returns 0 on success. */
int pa_shared_data_set_integer(pa_shared_data *t, const char *key, int32_t value);
/* Get integer value from shared item. If item doesn't exist or the key is incorrect,
 * return -1. Returns 0 on success. */
int pa_shared_data_get_integer(pa_shared_data *t, const char *key, int32_t *return_value);
/* Changes the shared data integer value. Value defaults to 0 if key doesn't already exist.
 * If item already exist with different type than integer or the key is incorrect return -1.
 * Returns 0 on success. */
int pa_shared_data_inc_integer(pa_shared_data *t, const char *key, int32_t change);

/* Set char value to shared item. Fire value changed hook if value has changed from previous value.
 * Return 0 on success, -1 on failure */
int pa_shared_data_sets(pa_shared_data *t, const char *key, const char *value);
/* Set char value to shared item. Fire value changed hook always, even if value hasn't changed.
 * Return 0 on success, -1 on failure */
int pa_shared_data_sets_always(pa_shared_data *t, const char *key, const char *value);
/* Get value from shared item. If shared item doesn't exist, returns NULL. */
const char *pa_shared_data_gets(pa_shared_data *t, const char *key);

/* Set variable data to shared item. Fire value changed hook always, even if data is identical to previous data.
 * Return 0 on success, -1 on failure */
int pa_shared_data_setd(pa_shared_data *t, const char *key, const void *data, size_t nbytes);
/* Get data from shared item.
 * Return 0 on success, -1 on failure or if item with key doesn't exist. */
int pa_shared_data_getd(pa_shared_data *t, const char *key, const void **data, size_t *nbytes);

/**
 * hook_data    - pa_shared_data *
 * call_data    - const char *
 * slot_data    - void *userdata
 */
pa_hook_slot *pa_shared_data_connect(pa_shared_data *t, const char *key, pa_hook_cb_t callback, void *userdata);
void pa_shared_data_hook_slot_free(pa_hook_slot *slot);

#endif
