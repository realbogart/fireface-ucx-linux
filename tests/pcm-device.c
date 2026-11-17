// SPDX-License-Identifier: MIT
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ucx_proto.h"

static const char *device = "hw:UCX";
static const char *fault_path = "/sys/module/snd_fireface_ucx/parameters/pcm_fault";

static void check(const char *operation, long result)
{
	if (result >= 0)
		return;
	fprintf(stderr, "%s: %s\n", operation, snd_strerror(result));
	exit(1);
}

static void require(int condition, const char *message)
{
	if (condition)
		return;
	fprintf(stderr, "%s\n", message);
	exit(1);
}

static snd_pcm_t *open_pcm(snd_pcm_stream_t direction, unsigned int rate)
{
	snd_pcm_sw_params_t *sw;
	snd_pcm_uframes_t boundary;
	snd_pcm_t *pcm;

	check("open", snd_pcm_open(&pcm, device, direction, SND_PCM_NONBLOCK));
	check("set parameters", snd_pcm_set_params(pcm, SND_PCM_FORMAT_S32_LE,
						    SND_PCM_ACCESS_RW_INTERLEAVED,
						    ucx_channels(rate), rate, 0, 120000));
	if (direction == SND_PCM_STREAM_PLAYBACK) {
		snd_pcm_sw_params_alloca(&sw);
		check("get software parameters", snd_pcm_sw_params_current(pcm, sw));
		check("get boundary", snd_pcm_sw_params_get_boundary(sw, &boundary));
		check("disable auto start", snd_pcm_sw_params_set_start_threshold(pcm, sw, boundary));
		check("set software parameters", snd_pcm_sw_params(pcm, sw));
	}
	return pcm;
}

static void prefill(snd_pcm_t *play, const int32_t *silence, snd_pcm_uframes_t frames)
{
	snd_pcm_sframes_t written = snd_pcm_writei(play, silence, frames);

	check("prefill playback", written);
	require((snd_pcm_uframes_t)written == frames, "short playback prefill");
}

static snd_pcm_sframes_t capture(snd_pcm_t *cap, int32_t *samples, snd_pcm_uframes_t period)
{
	int ready = snd_pcm_wait(cap, 1000);

	if (ready < 0)
		return ready;
	require(ready, "capture timed out");
	return snd_pcm_readi(cap, samples, period);
}

static void exchange(snd_pcm_t *cap, snd_pcm_t *play, int32_t *samples,
		     const int32_t *silence, snd_pcm_uframes_t period, unsigned int frames)
{
	while (frames) {
		snd_pcm_sframes_t n = capture(cap, samples, period);
		snd_pcm_sframes_t written;

		check("read capture", n);
		require(n > 0, "empty capture after poll");
		written = snd_pcm_writei(play, silence, n);
		check("write playback", written);
		require(written == n, "short playback write");
		frames -= n < frames ? n : frames;
	}
}

static void inject(unsigned int fault)
{
	FILE *file = fopen(fault_path, "w");

	if (!file) {
		perror(fault_path);
		exit(1);
	}
	if (fprintf(file, "%u\n", fault) < 0 || fclose(file)) {
		perror(fault_path);
		exit(1);
	}
}

static void test_rate(unsigned int rate)
{
	const char *faults[] = { "URB error", "short capture", "device restart" };
	snd_pcm_t *cap = open_pcm(SND_PCM_STREAM_CAPTURE, rate);
	snd_pcm_t *play = open_pcm(SND_PCM_STREAM_PLAYBACK, rate);
	snd_pcm_uframes_t cap_buffer, cap_period, play_buffer, play_period;
	int32_t *samples, *silence;
	unsigned int i;

	check("capture buffer size", snd_pcm_get_params(cap, &cap_buffer, &cap_period));
	check("playback buffer size", snd_pcm_get_params(play, &play_buffer, &play_period));
	require(play_buffer > cap_period, "playback buffer too small for capture period");
	samples = calloc(cap_period * ucx_channels(rate), sizeof(*samples));
	silence = calloc(play_buffer * ucx_channels(rate), sizeof(*silence));
	require(samples && silence, "allocating audio buffers failed");
	check("link duplex", snd_pcm_link(cap, play));
	prefill(play, silence, play_buffer - cap_period);
	check("start duplex", snd_pcm_start(cap));

	for (i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
		unsigned int reads;

		/* Stream for longer than the restart-event start window. */
		exchange(cap, play, samples, silence, cap_period, rate / 5);
		inject(i + 1);
		for (reads = 0; reads < 16; reads++) {
			snd_pcm_sframes_t n = capture(cap, samples, cap_period);

			if (n == -EPIPE)
				break;
			check("read while waiting for XRUN", n);
			n = snd_pcm_writei(play, silence, n);
			if (n == -EPIPE)
				break;
			check("write while waiting for XRUN", n);
		}
		require(reads < 16 && snd_pcm_state(cap) == SND_PCM_STATE_XRUN &&
			snd_pcm_state(play) == SND_PCM_STATE_XRUN, "expected duplex XRUN");
		check("prepare after XRUN", snd_pcm_prepare(cap));
		prefill(play, silence, play_buffer - cap_period);
		check("restart duplex", snd_pcm_start(cap));
		exchange(cap, play, samples, silence, cap_period, rate / 10);
		printf("%u Hz %s: duplex XRUN and recovery passed\n", rate, faults[i]);
	}

	check("unlink duplex", snd_pcm_unlink(cap));
	for (i = 0; i < 24; i++) {
		check("close playback", snd_pcm_close(play));
		check("capture with playback closed", capture(cap, samples, cap_period));
		require(snd_pcm_state(cap) == SND_PCM_STATE_RUNNING, "capture stopped on playback close");
		play = open_pcm(SND_PCM_STREAM_PLAYBACK, rate);
		prefill(play, silence, play_buffer - cap_period);
		check("restart playback only", snd_pcm_start(play));
		exchange(cap, play, samples, silence, cap_period, cap_period);
	}
	check("close playback", snd_pcm_close(play));
	check("close capture", snd_pcm_close(cap));
	printf("%u Hz playback close/reopen while capturing passed\n", rate);
	free(silence);
	free(samples);
}

int main(int argc, char **argv)
{
	if (argc > 2) {
		fprintf(stderr, "usage: %s [ALSA hardware device]\n", argv[0]);
		return 1;
	}
	if (argc == 2)
		device = argv[1];
	inject(0);
	test_rate(48000);
	/* Let the driver's ten-second restart window expire between rates. */
	sleep(10);
	test_rate(192000);
	return 0;
}
