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

#include <pulsecore/macro.h>
#include <pulse/xmalloc.h>
#include "alsa-utils.h"
#include "ctrl-element.h"


struct ctrl_element {
    snd_mixer_t *mixer;

    /* We'll store the element name and query the element pointer from the
     * mixer every time we set the element volume. This is how it's done in
     * PulseAudio, I guess in case the element "disappears", so let's just
     * follow the same convention. */
    char *element_name;

};

ctrl_element *ctrl_element_new(snd_mixer_t *mixer, const char* name) {
    pa_assert(mixer);
    pa_assert(name);

    ctrl_element *ctrl = NULL;
    snd_mixer_elem_t *element = NULL;

    ctrl = pa_xnew0(ctrl_element, 1);
    ctrl->mixer = mixer;
    ctrl->element_name = pa_xstrdup(name);

    element = mixer_get_element(mixer, name);
    if(!element) {
        pa_log_error("Unable to open mixer element \"%s\"", name);
        goto fail;
    }

    if(!snd_mixer_selem_is_playback_mono(element) ||
       !snd_mixer_selem_has_playback_volume(element)) {
        pa_log_error("\"%s\", is not a valid sidetone control element", name);
        goto fail;
    }

    return ctrl;

fail:

    pa_xfree(ctrl);

    return NULL;
}

void ctrl_element_free(ctrl_element *ctrl) {
    pa_assert(ctrl);
    pa_xfree(ctrl->element_name);
    pa_xfree(ctrl);
}

int ctrl_element_mute(ctrl_element *ctrl) {
    pa_assert(ctrl);

    snd_mixer_elem_t *element = NULL;

    element = mixer_get_element(ctrl->mixer, ctrl->element_name);
    if(!element) {
        pa_log_error("Element %s has disappeared.", ctrl->element_name);
        return -1;
    }

    if((snd_mixer_selem_set_playback_volume(element, SND_MIXER_SCHN_MONO, 0) < 0)) {
        pa_log_error("Failed to mute sidetone element");
        return -1;
    }

    return 0;
}


int set_ctrl_element_volume(ctrl_element *ctrl,int step) {
    pa_assert(ctrl);

    snd_mixer_elem_t *element = NULL;

    element = mixer_get_element(ctrl->mixer, ctrl->element_name);
    if(!element) {
        pa_log_error("Element %s has disappeared.", ctrl->element_name);
        return -1;
    }

    if((snd_mixer_selem_set_playback_volume(element, SND_MIXER_SCHN_MONO, step) < 0)) {
        pa_log_error("Failed to set the volume step to the sidetone control element");
        return -1;
    }

    return 0;
}

