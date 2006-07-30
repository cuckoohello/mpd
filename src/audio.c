/* the Music Player Daemon (MPD)
 * (c)2003-2006 by Warren Dukes (warren.dukes@gmail.com)
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

#include "audio.h"
#include "audioOutput.h"
#include "conf.h"
#include "log.h"
#include "sig_handlers.h"
#include "command.h"
#include "playerData.h"
#include "utils.h"
#include "playlist.h"
#include "state_file.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <errno.h>
#include <unistd.h>

#define AUDIO_DEVICE_STATE	"audio_device_state:"
#define AUDIO_DEVICE_STATE_LEN	19	/* strlen(AUDIO_DEVICE_STATE) */
#define AUDIO_BUFFER_SIZE	2*MAXPATHLEN

static AudioFormat audio_format;

static AudioFormat *audio_configFormat = NULL;

static AudioOutput **audioOutputArray = NULL;
static mpd_uint8 audioOutputArraySize = 0;
/* the audioEnabledArray should be stuck into shared memory, and then disable
   and enable in playAudio() routine */
static mpd_sint8 *pdAudioDevicesEnabled = NULL;
static mpd_sint8 myAudioDevicesEnabled[AUDIO_MAX_DEVICES];

static mpd_uint8 audioOpened = 0;

static mpd_sint32 audioBufferSize = 0;
static char *audioBuffer = NULL;
static mpd_sint32 audioBufferPos = 0;

void copyAudioFormat(AudioFormat * dest, AudioFormat * src)
{
	if (!src)
		return;

	memcpy(dest, src, sizeof(AudioFormat));
}

int cmpAudioFormat(AudioFormat * f1, AudioFormat * f2)
{
	if (f1 && f2 && (f1->sampleRate == f2->sampleRate) &&
	    (f1->bits == f2->bits) && (f1->channels == f2->channels))
		return 0;
	return 1;
}

extern AudioOutputPlugin alsaPlugin;
extern AudioOutputPlugin aoPlugin;
extern AudioOutputPlugin ossPlugin;
extern AudioOutputPlugin osxPlugin;
extern AudioOutputPlugin pulsePlugin;
extern AudioOutputPlugin mvpPlugin;
extern AudioOutputPlugin shoutPlugin;

void loadAudioDrivers(void)
{
	initAudioOutputPlugins();
	loadAudioOutputPlugin(&alsaPlugin);
	loadAudioOutputPlugin(&aoPlugin);
	loadAudioOutputPlugin(&ossPlugin);
	loadAudioOutputPlugin(&osxPlugin);
	loadAudioOutputPlugin(&pulsePlugin);
	loadAudioOutputPlugin(&mvpPlugin);
	loadAudioOutputPlugin(&shoutPlugin);
}

/* make sure initPlayerData is called before this function!! */
void initAudioDriver(void)
{
	ConfigParam *param = NULL;
	int i;

	loadAudioDrivers();

	pdAudioDevicesEnabled = (getPlayerData())->audioDeviceEnabled;

	for (i = 0; i < AUDIO_MAX_DEVICES; i++) {
		pdAudioDevicesEnabled[i] = 1;
		myAudioDevicesEnabled[i] = 1;
	}

	param = getNextConfigParam(CONF_AUDIO_OUTPUT, param);

	do {
		AudioOutput *output;
		int j;

		if (audioOutputArraySize == AUDIO_MAX_DEVICES) {
			ERROR("only up to 255 audio output devices are "
			      "supported");
			exit(EXIT_FAILURE);
		}

		i = audioOutputArraySize++;

		audioOutputArray = realloc(audioOutputArray,
					   audioOutputArraySize *
					   sizeof(AudioOutput *));

		output = newAudioOutput(param);
		if (!output && param) {
			ERROR("problems configuring output device defined at "
			      "line %i\n", param->line);
			exit(EXIT_FAILURE);
		}

		/* require output names to be unique: */
		for (j = i - 1; j >= 0; --j) {
			if (!strcmp(output->name, audioOutputArray[j]->name)) {
				ERROR("output devices with identical "
				      "names: %s\n", output->name);
				exit(EXIT_FAILURE);
			}
		}
		audioOutputArray[i] = output;
	} while ((param = getNextConfigParam(CONF_AUDIO_OUTPUT, param)));
}

void getOutputAudioFormat(AudioFormat * inAudioFormat,
			  AudioFormat * outAudioFormat)
{
	if (audio_configFormat) {
		copyAudioFormat(outAudioFormat, audio_configFormat);
	} else
		copyAudioFormat(outAudioFormat, inAudioFormat);
}

void initAudioConfig(void)
{
	ConfigParam *param = getConfigParam(CONF_AUDIO_OUTPUT_FORMAT);

	if (NULL == param || NULL == param->value)
		return;

	audio_configFormat = malloc(sizeof(AudioFormat));

	if (0 != parseAudioConfig(audio_configFormat, param->value)) {
		ERROR("error parsing \"%s\" at line %i\n",
		      CONF_AUDIO_OUTPUT_FORMAT, param->line);
		exit(EXIT_FAILURE);
	}
}

int parseAudioConfig(AudioFormat * audioFormat, char *conf)
{
	char *test;

	memset(audioFormat, 0, sizeof(AudioFormat));

	audioFormat->sampleRate = strtol(conf, &test, 10);

	if (*test != ':') {
		ERROR("error parsing audio output format: %s\n", conf);
		return -1;
	}

	/*switch(audioFormat->sampleRate) {
	   case 48000:
	   case 44100:
	   case 32000:
	   case 16000:
	   break;
	   default:
	   ERROR("sample rate %i can not be used for audio output\n",
	   (int)audioFormat->sampleRate);
	   return -1
	   } */

	if (audioFormat->sampleRate <= 0) {
		ERROR("sample rate %i is not >= 0\n",
		      (int)audioFormat->sampleRate);
		return -1;
	}

	audioFormat->bits = strtol(test + 1, &test, 10);

	if (*test != ':') {
		ERROR("error parsing audio output format: %s\n", conf);
		return -1;
	}

	switch (audioFormat->bits) {
	case 16:
		break;
	default:
		ERROR("bits %i can not be used for audio output\n",
		      (int)audioFormat->bits);
		return -1;
	}

	audioFormat->channels = strtol(test + 1, &test, 10);

	if (*test != '\0') {
		ERROR("error parsing audio output format: %s\n", conf);
		return -1;
	}

	switch (audioFormat->channels) {
	case 1:
	case 2:
		break;
	default:
		ERROR("channels %i can not be used for audio output\n",
		      (int)audioFormat->channels);
		return -1;
	}

	return 0;
}

void finishAudioConfig(void)
{
	if (audio_configFormat)
		free(audio_configFormat);
}

void finishAudioDriver(void)
{
	int i;

	for (i = 0; i < audioOutputArraySize; i++) {
		finishAudioOutput(audioOutputArray[i]);
	}

	free(audioOutputArray);
	audioOutputArray = NULL;
	audioOutputArraySize = 0;
}

int isCurrentAudioFormat(AudioFormat * audioFormat)
{
	if (!audioFormat)
		return 1;

	if (cmpAudioFormat(audioFormat, &audio_format) != 0)
		return 0;

	return 1;
}

static void syncAudioDevicesEnabledArrays(void)
{
	int i;

	memcpy(myAudioDevicesEnabled, pdAudioDevicesEnabled, AUDIO_MAX_DEVICES);

	for (i = 0; i < audioOutputArraySize; i++) {
		if (myAudioDevicesEnabled[i]) {
			openAudioOutput(audioOutputArray[i], &audio_format);
		} else {
			dropBufferedAudioOutput(audioOutputArray[i]);
			closeAudioOutput(audioOutputArray[i]);
		}
	}
}

static int flushAudioBuffer(void)
{
	int ret = -1;
	int i, err;

	if (audioBufferPos == 0)
		return 0;

	if (0 != memcmp(pdAudioDevicesEnabled, myAudioDevicesEnabled,
			AUDIO_MAX_DEVICES)) {
		syncAudioDevicesEnabledArrays();
	}

	for (i = 0; i < audioOutputArraySize; i++) {
		if (!myAudioDevicesEnabled[i])
			continue;
		err = playAudioOutput(audioOutputArray[i], audioBuffer,
				      audioBufferPos);
		if (!err)
			ret = 0;
		else if (err < 0)
			/* device should already be closed if the play func
			 * returned an error */
			myAudioDevicesEnabled[i] = 0;
	}

	audioBufferPos = 0;

	return ret;
}

int openAudioDevice(AudioFormat * audioFormat)
{
	int isCurrentFormat = isCurrentAudioFormat(audioFormat);
	int ret = -1;
	int i;

	if (!audioOutputArray)
		return -1;

	if (!audioOpened || !isCurrentFormat) {
		flushAudioBuffer();
		copyAudioFormat(&audio_format, audioFormat);
		audioBufferSize = (audio_format.bits >> 3) *
		    audio_format.channels;
		audioBufferSize *= audio_format.sampleRate >> 5;
		audioBuffer = realloc(audioBuffer, audioBufferSize);
	}

	syncAudioDevicesEnabledArrays();

	for (i = 0; i < audioOutputArraySize; i++) {
		if (audioOutputArray[i]->open)
			ret = 0;
	}

	if (ret == 0)
		audioOpened = 1;
	else {
		/* close all devices if there was an error */
		for (i = 0; i < audioOutputArraySize; i++) {
			closeAudioOutput(audioOutputArray[i]);
		}

		audioOpened = 0;
	}

	return ret;
}

int playAudio(char *playChunk, int size)
{
	int send;

	while (size > 0) {
		send = audioBufferSize - audioBufferPos;
		send = send < size ? send : size;

		memcpy(audioBuffer + audioBufferPos, playChunk, send);
		audioBufferPos += send;
		size -= send;
		playChunk += send;

		if (audioBufferPos == audioBufferSize) {
			if (flushAudioBuffer() < 0)
				return -1;
		}
	}

	return 0;
}

int isAudioDeviceOpen(void)
{
	return audioOpened;
}

void dropBufferedAudio(void)
{
	int i;

	if (0 != memcmp(pdAudioDevicesEnabled, myAudioDevicesEnabled,
			AUDIO_MAX_DEVICES)) {
		syncAudioDevicesEnabledArrays();
	}

	audioBufferPos = 0;

	for (i = 0; i < audioOutputArraySize; i++) {
		if (!myAudioDevicesEnabled[i])
			continue;
		dropBufferedAudioOutput(audioOutputArray[i]);
	}
}

void closeAudioDevice(void)
{
	int i;

	flushAudioBuffer();

	free(audioBuffer);
	audioBuffer = NULL;
	audioBufferSize = 0;

	for (i = 0; i < audioOutputArraySize; i++) {
		closeAudioOutput(audioOutputArray[i]);
	}

	audioOpened = 0;
}

void sendMetadataToAudioDevice(MpdTag * tag)
{
	int i;

	for (i = 0; i < audioOutputArraySize; i++) {
		sendMetadataToAudioOutput(audioOutputArray[i], tag);
	}
}

int enableAudioDevice(int fd, int device)
{
	if (device < 0 || device >= audioOutputArraySize) {
		commandError(fd, ACK_ERROR_ARG, "audio output device id %i "
			     "doesn't exist\n", device);
		return -1;
	}

	pdAudioDevicesEnabled[device] = 1;

	return 0;
}

int disableAudioDevice(int fd, int device)
{
	if (device < 0 || device >= audioOutputArraySize) {
		commandError(fd, ACK_ERROR_ARG, "audio output device id %i "
			     "doesn't exist\n", device);
		return -1;
	}

	pdAudioDevicesEnabled[device] = 0;

	return 0;
}

void printAudioDevices(int fd)
{
	int i;

	for (i = 0; i < audioOutputArraySize; i++) {
		fdprintf(fd,
		         "outputid: %i\noutputname: %s\noutputenabled: %i\n",
			 i,
			 audioOutputArray[i]->name,
			 (int)pdAudioDevicesEnabled[i]);
	}
}

void saveAudioDevicesState(FILE *fp)
{
	int i;

	assert(audioOutputArraySize != 0);
	for (i = 0; i < audioOutputArraySize; i++) {
		fprintf(fp, AUDIO_DEVICE_STATE "%d:%s\n",
			(int)pdAudioDevicesEnabled[i],
		        audioOutputArray[i]->name);
	}
}

void readAudioDevicesState(FILE *fp)
{
	char buffer[AUDIO_BUFFER_SIZE];
	int i;

	assert(audioOutputArraySize != 0);

	while (myFgets(buffer, AUDIO_BUFFER_SIZE, fp)) {
		char *c, *name;

		if (strncmp(buffer, AUDIO_DEVICE_STATE, AUDIO_DEVICE_STATE_LEN))
			continue;

		c = strchr(buffer, ':');
		if (!c || !(++c))
			goto errline;

		name = strchr(c, ':');
		if (!name || !(++name))
			goto errline;

		for (i = audioOutputArraySize - 1; i >= 0; --i) {
			if (!strcmp(name, audioOutputArray[i]->name)) {
				pdAudioDevicesEnabled[i] = atoi(c);
				break;
			}
		}
		continue;
	      errline:
		/* nonfatal */
		ERROR("invalid line in state_file: %s\n", buffer);
	}
}

