/*
 * Copyright (C) 2003-2010 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPD_INPUT_CURL_H
#define MPD_INPUT_CURL_H

struct input_stream;

extern const struct input_plugin input_plugin_curl;

/**
 * This is a workaround for an input_stream API deficiency; after
 * exchanging the input_stream pointer in input_rewind_open(), this
 * function is called to reinitialize CURL's data pointers.
 */
void
input_curl_reinit(struct input_stream *is);

#endif
