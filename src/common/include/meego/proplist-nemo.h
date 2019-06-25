/*
 * Copyright (C) 2013 Jolla Ltd.
 *
 * Contact: Juho Hämäläinen <juho.hamalainen@oss.tieto.com>
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
#ifndef _PROPLIST_NEMO_H_
#define _PROPLIST_NEMO_H_

/* Generic */
#define PA_NEMO_PROP_CALL_STATE                     "x-nemo.voicecall.status"
#define PA_NEMO_PROP_CALL_STATE_ACTIVE              "active"
#define PA_NEMO_PROP_CALL_STATE_INACTIVE            "inactive"

#define PA_NEMO_PROP_MEDIA_STATE                    "x-nemo.media.state"
#define PA_NEMO_PROP_MEDIA_STATE_INACTIVE           "inactive"
#define PA_NEMO_PROP_MEDIA_STATE_FOREGROUND         "foreground"
#define PA_NEMO_PROP_MEDIA_STATE_BACKGROUND         "background"
#define PA_NEMO_PROP_MEDIA_STATE_ACTIVE             "active"

#define PA_NEMO_PROP_EMERGENCY_CALL_STATE           "x.emergency_call.state"
#define PA_NEMO_PROP_EMERGENCY_CALL_STATE_INACTIVE  "inactive"
#define PA_NEMO_PROP_EMERGENCY_CALL_STATE_ACTIVE    "active"


#endif /* _PROPLIST_NEMO_H_ */
