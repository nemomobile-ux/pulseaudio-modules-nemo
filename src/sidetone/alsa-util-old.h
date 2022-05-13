#ifndef fooalsautiloldhfoo
#define fooalsautiloldhfoo

/***
  This file is copied from PulseAudio (the version used in MeeGo 1.2
  Harmattan).

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <alsa/asoundlib.h>

/* Currently the upstream offers only pa_alsa_open_mixer_for_pcm() for opening
 * an alsa mixer handle, which isn't usable in the "alsa-old" modules, because
 * they must not open the pcm handle before the first stream connects, and the
 * aforementioned _open_mixer_for_pcm() requires an opened pcm handle. This
 * function only requires the name of the device that needs to be opened.
 *
 * The code behind this function is mostly copied from 0.9.15, which is why
 * this file is called alsa-utils-old.h.
 *
 * Returns NULL on failure. */
snd_mixer_t *pa_alsa_old_open_mixer(const char *dev);

#endif
