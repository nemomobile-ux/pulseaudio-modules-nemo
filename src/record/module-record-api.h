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
#ifndef module_record_api_h
#define module_record_api_h

#define RECORD_API_VERSION "0.1"

/* Type: meego_algorithm_hook_data      Channels: 2 */
#define RECORD_HOOK_DYNAMIC_ENHANCE              "x-meego.record.dynamic_enhance"

#define PA_PROP_SOURCE_RECORD_API_EXTENSION_PROPERTY_NAME "source.api-extension.meego.record"
#define PA_PROP_SOURCE_RECORD_API_EXTENSION_PROPERTY_VALUE RECORD_API_VERSION

#endif /* module_record_api_h */
