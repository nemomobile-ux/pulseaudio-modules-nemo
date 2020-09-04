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

#ifndef fooctrlelementhfoo
#define fooctrlelementhfoo

#include <alsa/asoundlib.h>

typedef struct ctrl_element ctrl_element;

ctrl_element *ctrl_element_new(snd_mixer_t *mixer, const char* name);

void ctrl_element_free(ctrl_element *ctrl);

int ctrl_element_mute(ctrl_element *ctrl);

int set_ctrl_element_volume(ctrl_element *ctrl, int step);

#endif

