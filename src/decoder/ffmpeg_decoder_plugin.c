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

#include "config.h"
#include "decoder_api.h"
#include "audio_check.h"

#include <glib.h>

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef OLD_FFMPEG_INCLUDES
#include <avcodec.h>
#include <avformat.h>
#include <avio.h>
#else
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/log.h>
#endif

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "ffmpeg"

#ifndef OLD_FFMPEG_INCLUDES

static GLogLevelFlags
level_ffmpeg_to_glib(int level)
{
	if (level <= AV_LOG_FATAL)
		return G_LOG_LEVEL_CRITICAL;

	if (level <= AV_LOG_ERROR)
		return G_LOG_LEVEL_WARNING;

	if (level <= AV_LOG_INFO)
		return G_LOG_LEVEL_MESSAGE;

	return G_LOG_LEVEL_DEBUG;
}

static void
mpd_ffmpeg_log_callback(G_GNUC_UNUSED void *ptr, int level,
			const char *fmt, va_list vl)
{
	const AVClass *cls = *(const AVClass *const*)ptr;
	char *domain = g_strconcat(G_LOG_DOMAIN, "/", cls->item_name(ptr), NULL);

	g_logv(domain, level_ffmpeg_to_glib(level), fmt, vl);
	g_free(domain);
}

#endif /* !OLD_FFMPEG_INCLUDES */

struct ffmpeg_stream {
	/** hack - see url_to_struct() */
	char url[64];

	struct decoder *decoder;
	struct input_stream *input;
};

/**
 * Convert a faked mpd:// URL to a ffmpeg_stream structure.  This is a
 * hack because ffmpeg does not provide a nice API for passing a
 * user-defined pointer to mpdurl_open().
 */
static struct ffmpeg_stream *url_to_struct(const char *url)
{
	union {
		const char *in;
		struct ffmpeg_stream *out;
	} u = { .in = url };
	return u.out;
}

static int mpd_ffmpeg_open(URLContext *h, const char *filename,
			   G_GNUC_UNUSED int flags)
{
	struct ffmpeg_stream *stream = url_to_struct(filename);
	h->priv_data = stream;
	h->is_streamed = stream->input->seekable ? 0 : 1;
	return 0;
}

static int mpd_ffmpeg_read(URLContext *h, unsigned char *buf, int size)
{
	struct ffmpeg_stream *stream = (struct ffmpeg_stream *) h->priv_data;

	return decoder_read(stream->decoder, stream->input,
			    (void *)buf, size);
}

static int64_t mpd_ffmpeg_seek(URLContext *h, int64_t pos, int whence)
{
	struct ffmpeg_stream *stream = (struct ffmpeg_stream *) h->priv_data;
	bool ret;

	if (whence == AVSEEK_SIZE)
		return stream->input->size;

	ret = input_stream_seek(stream->input, pos, whence, NULL);
	if (!ret)
		return -1;

	return stream->input->offset;
}

static int mpd_ffmpeg_close(URLContext *h)
{
	h->priv_data = NULL;
	return 0;
}

static URLProtocol mpd_ffmpeg_fileops = {
	.name = "mpd",
	.url_open = mpd_ffmpeg_open,
	.url_read = mpd_ffmpeg_read,
	.url_seek = mpd_ffmpeg_seek,
	.url_close = mpd_ffmpeg_close,
};

static bool
ffmpeg_init(G_GNUC_UNUSED const struct config_param *param)
{
#ifndef OLD_FFMPEG_INCLUDES
	av_log_set_callback(mpd_ffmpeg_log_callback);
#endif

	av_register_all();
	register_protocol(&mpd_ffmpeg_fileops);
	return true;
}

static int
ffmpeg_find_audio_stream(const AVFormatContext *format_context)
{
	for (unsigned i = 0; i < format_context->nb_streams; ++i)
		if (format_context->streams[i]->codec->codec_type ==
		    CODEC_TYPE_AUDIO)
			return i;

	return -1;
}

/**
 * Append the suffix of the original URI to the virtual stream URI.
 * Without this, libavformat cannot detect some of the codecs
 * (e.g. "shorten").
 */
static void
append_uri_suffix(struct ffmpeg_stream *stream, const char *uri)
{
	assert(stream != NULL);
	assert(uri != NULL);

	char *base = g_path_get_basename(uri);

	const char *suffix = strrchr(base, '.');
	if (suffix != NULL && suffix[1] != 0)
		g_strlcat(stream->url, suffix, sizeof(stream->url));

	g_free(base);
}

/**
 * On some platforms, libavcodec wants the output buffer aligned to 16
 * bytes (because it uses SSE/Altivec internally).  This function
 * returns the aligned version of the specified buffer, and corrects
 * the buffer size.
 */
static void *
align16(void *p, size_t *length_p)
{
	unsigned add = 16 - (size_t)p % 16;

	*length_p -= add;
	return (char *)p + add;
}

static enum decoder_command
ffmpeg_send_packet(struct decoder *decoder, struct input_stream *is,
		   const AVPacket *packet,
		   AVCodecContext *codec_context,
		   const AVRational *time_base)
{
	enum decoder_command cmd = DECODE_COMMAND_NONE;
	uint8_t audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2 + 16];
	int16_t *aligned_buffer;
	size_t buffer_size;
	int len, audio_size;
	uint8_t *packet_data;
	int packet_size;

	if (packet->pts != (int64_t)AV_NOPTS_VALUE)
		decoder_timestamp(decoder,
				  av_rescale_q(packet->pts, *time_base,
					       (AVRational){1, 1}));

	packet_data = packet->data;
	packet_size = packet->size;

	buffer_size = sizeof(audio_buf);
	aligned_buffer = align16(audio_buf, &buffer_size);

	while ((packet_size > 0) && (cmd == DECODE_COMMAND_NONE)) {
		audio_size = buffer_size;
		len = avcodec_decode_audio2(codec_context,
					    aligned_buffer, &audio_size,
					    packet_data, packet_size);

		if (len < 0) {
			/* if error, we skip the frame */
			g_message("decoding failed\n");
			break;
		}

		packet_data += len;
		packet_size -= len;

		if (audio_size <= 0)
			continue;

		cmd = decoder_data(decoder, is,
				   aligned_buffer, audio_size,
				   codec_context->bit_rate / 1000);
	}
	return cmd;
}

static enum sample_format
ffmpeg_sample_format(G_GNUC_UNUSED const AVCodecContext *codec_context)
{
#if LIBAVCODEC_VERSION_INT >= ((51<<16)+(41<<8)+0)
	int bits = (uint8_t) av_get_bits_per_sample_format(codec_context->sample_fmt);

	/* XXX implement & test other sample formats */

	switch (bits) {
	case 16:
		return SAMPLE_FORMAT_S16;
	}

	return SAMPLE_FORMAT_UNDEFINED;
#else
	/* XXX fixme 16-bit for older ffmpeg (13 Aug 2007) */
	return SAMPLE_FORMAT_S16;
#endif
}

static void
ffmpeg_decode(struct decoder *decoder, struct input_stream *input)
{
	struct ffmpeg_stream stream = {
		.url = "mpd://X", /* only the mpd:// prefix matters */
		.decoder = decoder,
		.input = input,
	};
	AVFormatContext *format_context;
	AVCodecContext *codec_context;
	AVCodec *codec;
	int audio_stream;

	if (input->uri != NULL)
		append_uri_suffix(&stream, input->uri);

	//ffmpeg works with ours "fileops" helper
	if (av_open_input_file(&format_context, stream.url, NULL, 0, NULL) != 0) {
		g_warning("Open failed\n");
		return;
	}

	if (av_find_stream_info(format_context)<0) {
		g_warning("Couldn't find stream info\n");
		av_close_input_file(format_context);
		return;
	}

	audio_stream = ffmpeg_find_audio_stream(format_context);
	if (audio_stream == -1) {
		g_warning("No audio stream inside\n");
		av_close_input_file(format_context);
		return;
	}

	codec_context = format_context->streams[audio_stream]->codec;
	if (codec_context->codec_name[0] != 0)
		g_debug("codec '%s'", codec_context->codec_name);

	codec = avcodec_find_decoder(codec_context->codec_id);

	if (!codec) {
		g_warning("Unsupported audio codec\n");
		av_close_input_file(format_context);
		return;
	}

	if (avcodec_open(codec_context, codec)<0) {
		g_warning("Could not open codec\n");
		av_close_input_file(format_context);
		return;
	}

	GError *error = NULL;
	struct audio_format audio_format;
	if (!audio_format_init_checked(&audio_format,
				       codec_context->sample_rate,
				       ffmpeg_sample_format(codec_context),
				       codec_context->channels, &error)) {
		g_warning("%s", error->message);
		g_error_free(error);
		avcodec_close(codec_context);
		av_close_input_file(format_context);
		return;
	}

	int total_time = format_context->duration != (int64_t)AV_NOPTS_VALUE
		? format_context->duration / AV_TIME_BASE
		: 0;

	decoder_initialized(decoder, &audio_format,
			    input->seekable, total_time);

	enum decoder_command cmd;
	do {
		AVPacket packet;
		if (av_read_frame(format_context, &packet) < 0)
			/* end of file */
			break;

		if (packet.stream_index == audio_stream)
			cmd = ffmpeg_send_packet(decoder, input,
						 &packet, codec_context,
						 &format_context->streams[audio_stream]->time_base);
		else
			cmd = decoder_get_command(decoder);

		av_free_packet(&packet);

		if (cmd == DECODE_COMMAND_SEEK) {
			int64_t where =
				decoder_seek_where(decoder) * AV_TIME_BASE;

			if (av_seek_frame(format_context, -1, where, 0) < 0)
				decoder_seek_error(decoder);
			else
				decoder_command_finished(decoder);
		}
	} while (cmd != DECODE_COMMAND_STOP);

	avcodec_close(codec_context);
	av_close_input_file(format_context);
}

#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(31<<8)+0)
static bool
ffmpeg_copy_metadata(struct tag *tag, AVMetadata *m,
		     enum tag_type type, const char *name)
{
	AVMetadataTag *mt = NULL;

	while ((mt = av_metadata_get(m, name, mt, 0)) != NULL)
		tag_add_item(tag, type, mt->value);
	return mt != NULL;
}
#endif

//no tag reading in ffmpeg, check if playable
static struct tag *
ffmpeg_stream_tag(struct input_stream *is)
{
	struct ffmpeg_stream stream = {
		.url = "mpd://X", /* only the mpd:// prefix matters */
		.decoder = NULL,
		.input = is,
	};

	if (is->uri != NULL)
		append_uri_suffix(&stream, is->uri);

	AVFormatContext *f;
	if (av_open_input_file(&f, stream.url, NULL, 0, NULL) != 0)
		return NULL;

	if (av_find_stream_info(f) < 0) {
		av_close_input_file(f);
		return NULL;
	}

	struct tag *tag = tag_new();

	tag->time = f->duration != (int64_t)AV_NOPTS_VALUE
		? f->duration / AV_TIME_BASE
		: 0;

#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(31<<8)+0)
	av_metadata_conv(f, NULL, f->iformat->metadata_conv);

	ffmpeg_copy_metadata(tag, f->metadata, TAG_TITLE, "title");
#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(50<<8))
	ffmpeg_copy_metadata(tag, f->metadata, TAG_ARTIST, "artist");
	ffmpeg_copy_metadata(tag, f->metadata, TAG_DATE,   "date");
#else
	ffmpeg_copy_metadata(tag, f->metadata, TAG_ARTIST, "author");
	ffmpeg_copy_metadata(tag, f->metadata, TAG_DATE,   "year");
#endif
	ffmpeg_copy_metadata(tag, f->metadata, TAG_ALBUM, "album");
	ffmpeg_copy_metadata(tag, f->metadata, TAG_COMMENT, "comment");
	ffmpeg_copy_metadata(tag, f->metadata, TAG_GENRE, "genre");
	ffmpeg_copy_metadata(tag, f->metadata, TAG_TRACK, "track");
	ffmpeg_copy_metadata(tag, f->metadata, TAG_ARTIST_SORT, "author-sort");
	ffmpeg_copy_metadata(tag, f->metadata, TAG_ALBUM_ARTIST, "album_artist");
	ffmpeg_copy_metadata(tag, f->metadata, TAG_ALBUM_ARTIST_SORT, "album_artist-sort");
	ffmpeg_copy_metadata(tag, f->metadata, TAG_COMPOSER, "composer");
	ffmpeg_copy_metadata(tag, f->metadata, TAG_PERFORMER, "performer");
	ffmpeg_copy_metadata(tag, f->metadata, TAG_DISC, "disc");
#else
	if (f->author[0])
		tag_add_item(tag, TAG_ARTIST, f->author);
	if (f->title[0])
		tag_add_item(tag, TAG_TITLE, f->title);
	if (f->album[0])
		tag_add_item(tag, TAG_ALBUM, f->album);

	if (f->track > 0) {
		char buffer[16];
		snprintf(buffer, sizeof(buffer), "%d", f->track);
		tag_add_item(tag, TAG_TRACK, buffer);
	}

	if (f->comment[0])
		tag_add_item(tag, TAG_COMMENT, f->comment);
	if (f->genre[0])
		tag_add_item(tag, TAG_GENRE, f->genre);
	if (f->year > 0) {
		char buffer[16];
		snprintf(buffer, sizeof(buffer), "%d", f->year);
		tag_add_item(tag, TAG_DATE, buffer);
	}

#endif

	av_close_input_file(f);

	return tag;
}

/**
 * A list of extensions found for the formats supported by ffmpeg.
 * This list is current as of 02-23-09; To find out if there are more
 * supported formats, check the ffmpeg changelog since this date for
 * more formats.
 */
static const char *const ffmpeg_suffixes[] = {
	"16sv", "3g2", "3gp", "4xm", "8svx", "aa3", "aac", "ac3", "afc", "aif",
	"aifc", "aiff", "al", "alaw", "amr", "anim", "apc", "ape", "asf",
	"atrac", "au", "aud", "avi", "avm2", "avs", "bap", "bfi", "c93", "cak",
	"cin", "cmv", "cpk", "daud", "dct", "divx", "dts", "dv", "dvd", "dxa",
	"eac3", "film", "flac", "flc", "fli", "fll", "flx", "flv", "g726",
	"gsm", "gxf", "iss", "m1v", "m2v", "m2t", "m2ts", "m4a", "m4v", "mad",
	"mj2", "mjpeg", "mjpg", "mka", "mkv", "mlp", "mm", "mmf", "mov", "mp+",
	"mp1", "mp2", "mp3", "mp4", "mpc", "mpeg", "mpg", "mpga", "mpp", "mpu",
	"mve", "mvi", "mxf", "nc", "nsv", "nut", "nuv", "oga", "ogm", "ogv",
	"ogx", "oma", "ogg", "omg", "psp", "pva", "qcp", "qt", "r3d", "ra",
	"ram", "rl2", "rm", "rmvb", "roq", "rpl", "rvc", "shn", "smk", "snd",
	"sol", "son", "spx", "str", "swf", "tgi", "tgq", "tgv", "thp", "ts",
	"tsp", "tta", "xa", "xvid", "uv", "uv2", "vb", "vid", "vob", "voc",
	"vp6", "vmd", "wav", "wma", "wmv", "wsaud", "wsvga", "wv", "wve",
	NULL
};

static const char *const ffmpeg_mime_types[] = {
	"application/m4a",
	"application/mp4",
	"application/octet-stream",
	"application/ogg",
	"application/x-ms-wmz",
	"application/x-ms-wmd",
	"application/x-ogg",
	"application/x-shockwave-flash",
	"application/x-shorten",
	"audio/8svx",
	"audio/16sv",
	"audio/aac",
	"audio/ac3",
	"audio/aiff"
	"audio/amr",
	"audio/basic",
	"audio/flac",
	"audio/m4a",
	"audio/mp4",
	"audio/mpeg",
	"audio/musepack",
	"audio/ogg",
	"audio/qcelp",
	"audio/vorbis",
	"audio/vorbis+ogg",
	"audio/x-8svx",
	"audio/x-16sv",
	"audio/x-aac",
	"audio/x-ac3",
	"audio/x-aiff"
	"audio/x-alaw",
	"audio/x-au",
	"audio/x-dca",
	"audio/x-eac3",
	"audio/x-flac",
	"audio/x-gsm",
	"audio/x-mace",
	"audio/x-matroska",
	"audio/x-monkeys-audio",
	"audio/x-mpeg",
	"audio/x-ms-wma",
	"audio/x-ms-wax",
	"audio/x-musepack",
	"audio/x-ogg",
	"audio/x-vorbis",
	"audio/x-vorbis+ogg",
	"audio/x-pn-realaudio",
	"audio/x-pn-multirate-realaudio",
	"audio/x-speex",
	"audio/x-tta"
	"audio/x-voc",
	"audio/x-wav",
	"audio/x-wma",
	"audio/x-wv",
	"video/anim",
	"video/quicktime",
	"video/msvideo",
	"video/ogg",
	"video/theora",
	"video/x-dv",
	"video/x-flv",
	"video/x-matroska",
	"video/x-mjpeg",
	"video/x-mpeg",
	"video/x-ms-asf",
	"video/x-msvideo",
	"video/x-ms-wmv",
	"video/x-ms-wvx",
	"video/x-ms-wm",
	"video/x-ms-wmx",
	"video/x-nut",
	"video/x-pva",
	"video/x-theora",
	"video/x-vid",
	"video/x-wmv",
	"video/x-xvid",
	NULL
};

const struct decoder_plugin ffmpeg_decoder_plugin = {
	.name = "ffmpeg",
	.init = ffmpeg_init,
	.stream_decode = ffmpeg_decode,
	.stream_tag = ffmpeg_stream_tag,
	.suffixes = ffmpeg_suffixes,
	.mime_types = ffmpeg_mime_types
};
