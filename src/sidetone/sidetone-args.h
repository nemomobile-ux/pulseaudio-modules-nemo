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

#ifndef foosidetoneargshfoo
#define foosidetoneargshfoo

#include <alsa/asoundlib.h>

#include <pulsecore/modargs.h>
#include "sidetone.h"
typedef struct {
    const char *mixer;

    const char *control_element;

    const char *mainvolume;

    struct mv_volume_steps *steps;

    const char *master_sink;

    pa_modargs *modargs;

} sidetone_args;

sidetone_args* sidetone_args_new(const char* args);

void sidetone_args_free(sidetone_args *args);

int parse_volume_steps(struct mv_volume_steps *steps, const char *step_string);

#endif

