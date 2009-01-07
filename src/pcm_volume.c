/*
 * Copyright (C) 2003-2009 The Music Player Daemon Project
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "pcm_volume.h"
#include "pcm_utils.h"
#include "audio_format.h"

#include <glib.h>

#include <stdint.h>
#include <string.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "pcm_volume"

static void
pcm_volume_change_8(int8_t *buffer, unsigned num_samples, int volume)
{
	while (num_samples > 0) {
		int32_t sample = *buffer;

		sample = (sample * volume + pcm_dither() + PCM_VOLUME_1 / 2)
			/ PCM_VOLUME_1;

		*buffer++ = pcm_range(sample, 8);
		--num_samples;
	}
}

static void
pcm_volume_change_16(int16_t *buffer, unsigned num_samples, int volume)
{
	while (num_samples > 0) {
		int32_t sample = *buffer;

		sample = (sample * volume + pcm_dither() + PCM_VOLUME_1 / 2)
			/ PCM_VOLUME_1;

		*buffer++ = pcm_range(sample, 16);
		--num_samples;
	}
}

static void
pcm_volume_change_24(int32_t *buffer, unsigned num_samples, int volume)
{
	while (num_samples > 0) {
		int64_t sample = *buffer;

		sample = (sample * volume + pcm_dither() + PCM_VOLUME_1 / 2)
			/ PCM_VOLUME_1;

		*buffer++ = pcm_range(sample, 24);
		--num_samples;
	}
}

void
pcm_volume(char *buffer, int bufferSize,
	   const struct audio_format *format,
	   int volume)
{
	if (volume == PCM_VOLUME_1)
		return;

	if (volume <= 0) {
		memset(buffer, 0, bufferSize);
		return;
	}

	switch (format->bits) {
	case 8:
		pcm_volume_change_8((int8_t *)buffer, bufferSize, volume);
		break;

	case 16:
		pcm_volume_change_16((int16_t *)buffer, bufferSize / 2,
				     volume);
		break;

	case 24:
		pcm_volume_change_24((int32_t*)buffer, bufferSize / 4,
				     volume);
		break;

	default:
		g_error("%u bits not supported by pcm_volume!\n",
			format->bits);
	}
}