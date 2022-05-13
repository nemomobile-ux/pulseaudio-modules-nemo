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

#ifndef foosidetonealsautilshfoo
#define foosidetonealsautilshfoo

#include <alsa/asoundlib.h>
#include <pulsecore/macro.h>

snd_mixer_elem_t *mixer_get_element(snd_mixer_t *mixer, const char* element_name);

int mixer_get_element_volume(snd_mixer_t *mixer, const char *element_name, snd_mixer_selem_channel_id_t channel, bool playback, long *volume);

#endif

