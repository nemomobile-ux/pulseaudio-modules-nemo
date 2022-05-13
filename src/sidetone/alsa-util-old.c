/***
  This file is copied from PulseAudio (the version used in MeeGo 1.2
  Harmattan).

  Copyright 2004-2009 Lennart Poettering
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "alsa-util-old.h"

snd_mixer_t *pa_alsa_old_open_mixer(const char *dev) {
    snd_mixer_t *mixer;
    int err;

    pa_assert(dev);

    if ((err = snd_mixer_open(&mixer, 0)) < 0) {
        pa_log_error("Error opening mixer: %s", snd_strerror(err));
        return NULL;
    }

    /* The rest is copied from the old pa_alsa_prepare_mixer(). */

    if ((err = snd_mixer_attach(mixer, dev)) < 0) {
        pa_log_info("Unable to attach to mixer %s: %s", dev, snd_strerror(err));
        goto fail;
    }

    if ((err = snd_mixer_selem_register(mixer, NULL, NULL)) < 0) {
        pa_log_warn("Unable to register mixer: %s", snd_strerror(err));
        goto fail;
    }

    if ((err = snd_mixer_load(mixer)) < 0) {
        pa_log_warn("Unable to load mixer: %s", snd_strerror(err));
        goto fail;
    }

    pa_log_info("Successfully attached to mixer '%s'", dev);

    return mixer;

fail:
    if (mixer)
        snd_mixer_close(mixer);

    return NULL;
}
