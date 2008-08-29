/* the Music Player Daemon (MPD)
 * Copyright (C) 2008 Max Kellermann <max@duempel.org>
 * This project's homepage is: http://www.musicpd.org
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

#include "player_control.h"
#include "path.h"
#include "log.h"
#include "ack.h"
#include "os_compat.h"
#include "main_notify.h"

struct player_control pc;

void pc_init(unsigned int buffered_before_play)
{
	pc.buffered_before_play = buffered_before_play;
	notify_init(&pc.notify);
	pc.command = PLAYER_COMMAND_NONE;
	pc.error = PLAYER_ERROR_NOERROR;
	pc.state = PLAYER_STATE_STOP;
	pc.queueState = PLAYER_QUEUE_BLANK;
	pc.queueLockState = PLAYER_QUEUE_UNLOCKED;
	pc.crossFade = 0;
	pc.softwareVolume = 1000;
}

static void set_current_song(Song *song)
{
	assert(song != NULL);
	assert(song->url != NULL);

	pc.fileTime = song->tag ? song->tag->time : 0;
	pc.next_song = song;
}

static void player_command(enum player_command cmd)
{
	pc.command = cmd;
	while (pc.command != PLAYER_COMMAND_NONE) {
		notify_signal(&pc.notify);
		wait_main_task();
	}
}

void playerPlay(Song * song)
{
	assert(pc.queueLockState == PLAYER_QUEUE_UNLOCKED);

	if (pc.state != PLAYER_STATE_STOP)
		player_command(PLAYER_COMMAND_STOP);

	pc.queueState = PLAYER_QUEUE_BLANK;

	set_current_song(song);
	player_command(PLAYER_COMMAND_PLAY);
}

void playerWait(void)
{
	player_command(PLAYER_COMMAND_CLOSE_AUDIO);

	assert(pc.queueLockState == PLAYER_QUEUE_UNLOCKED);

	player_command(PLAYER_COMMAND_CLOSE_AUDIO);

	pc.queueState = PLAYER_QUEUE_BLANK;
}

void playerKill(void)
{
	player_command(PLAYER_COMMAND_EXIT);
}

void playerPause(void)
{
	if (pc.state != PLAYER_STATE_STOP)
		player_command(PLAYER_COMMAND_PAUSE);
}

void playerSetPause(int pause_flag)
{
	switch (pc.state) {
	case PLAYER_STATE_STOP:
		break;

	case PLAYER_STATE_PLAY:
		if (pause_flag)
			playerPause();
		break;
	case PLAYER_STATE_PAUSE:
		if (!pause_flag)
			playerPause();
		break;
	}
}

int getPlayerElapsedTime(void)
{
	return (int)(pc.elapsedTime + 0.5);
}

unsigned long getPlayerBitRate(void)
{
	return pc.bitRate;
}

int getPlayerTotalTime(void)
{
	return (int)(pc.totalTime + 0.5);
}

enum player_state getPlayerState(void)
{
	return pc.state;
}

void clearPlayerError(void)
{
	pc.error = 0;
}

int getPlayerError(void)
{
	return pc.error;
}

char *getPlayerErrorStr(void)
{
	/* static OK here, only one user in main task */
	static char error[MPD_PATH_MAX + 64]; /* still too much */
	static const size_t errorlen = sizeof(error);
	char path_max_tmp[MPD_PATH_MAX];
	*error = '\0'; /* likely */

	switch (pc.error) {
	case PLAYER_ERROR_FILENOTFOUND:
		snprintf(error, errorlen,
			 "file \"%s\" does not exist or is inaccessible",
			 get_song_url(path_max_tmp, pc.errored_song));
		break;
	case PLAYER_ERROR_FILE:
		snprintf(error, errorlen, "problems decoding \"%s\"",
			 get_song_url(path_max_tmp, pc.errored_song));
		break;
	case PLAYER_ERROR_AUDIO:
		strcpy(error, "problems opening audio device");
		break;
	case PLAYER_ERROR_SYSTEM:
		strcpy(error, "system error occured");
		break;
	case PLAYER_ERROR_UNKTYPE:
		snprintf(error, errorlen, "file type of \"%s\" is unknown",
			 get_song_url(path_max_tmp, pc.errored_song));
	}
	return *error ? error : NULL;
}

void queueSong(Song * song)
{
	assert(pc.queueState == PLAYER_QUEUE_BLANK);

	set_current_song(song);
	pc.queueState = PLAYER_QUEUE_FULL;
}

enum player_queue_state getPlayerQueueState(void)
{
	return pc.queueState;
}

void setQueueState(enum player_queue_state queueState)
{
	pc.queueState = queueState;
	notify_signal(&pc.notify);
}

void playerQueueLock(void)
{
	assert(pc.queueLockState == PLAYER_QUEUE_UNLOCKED);
	player_command(PLAYER_COMMAND_LOCK_QUEUE);
	assert(pc.queueLockState == PLAYER_QUEUE_LOCKED);
}

void playerQueueUnlock(void)
{
	if (pc.queueLockState == PLAYER_QUEUE_LOCKED)
		player_command(PLAYER_COMMAND_UNLOCK_QUEUE);

	assert(pc.queueLockState == PLAYER_QUEUE_UNLOCKED);
}

int playerSeek(Song * song, float seek_time)
{
	assert(song != NULL);

	if (pc.state == PLAYER_STATE_STOP)
		return -1;

	if (pc.next_song != song)
		set_current_song(song);

	if (pc.error == PLAYER_ERROR_NOERROR) {
		pc.seekWhere = seek_time;
		player_command(PLAYER_COMMAND_SEEK);
	}

	return 0;
}

float getPlayerCrossFade(void)
{
	return pc.crossFade;
}

void setPlayerCrossFade(float crossFadeInSeconds)
{
	if (crossFadeInSeconds < 0)
		crossFadeInSeconds = 0;
	pc.crossFade = crossFadeInSeconds;
}

void setPlayerSoftwareVolume(int volume)
{
	volume = (volume > 1000) ? 1000 : (volume < 0 ? 0 : volume);
	pc.softwareVolume = volume;
}

double getPlayerTotalPlayTime(void)
{
	return pc.totalPlayTime;
}

unsigned int getPlayerSampleRate(void)
{
	return pc.sampleRate;
}

int getPlayerBits(void)
{
	return pc.bits;
}

int getPlayerChannels(void)
{
	return pc.channels;
}

/* this actually creates a dupe of the current metadata */
Song *playerCurrentDecodeSong(void)
{
	return NULL;
}