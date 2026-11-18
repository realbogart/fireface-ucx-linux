// SPDX-License-Identifier: MIT
/*
 * Loopback test for the snd-fireface-ucx PCM: needs a cable from an output
 * to an input (default Line Out 1 -> Line In 5).
 *
 *   looptest [-D hw:UCX] [-r rate] [-p period] [-b buffer] [-o out_ch]
 *            [-i in_ch[,in_ch...]] [-s seconds] [-a dBFS] [-w capture.raw]
 *
 * Opens playback and capture, links them so they start together, and plays
 * on channel out_ch: 8 single-sample clicks (one every 100 ms) and then a
 * 1 kHz sine, both at -20 dBFS
 * unless -a gives another level. From each capture channel in_ch it prints
 * (several channels compare paths within one start; -w writes the first)
 *
 *  - the round-trip latency: where each click arrives minus where it was
 *    written, in frames. Both streams count frames from the same start, so
 *    this is what an application has to compensate for;
 *  - glitches in the sine: samples that break x[n] = c*x[n-1] - x[n-2], which
 *    every pure sine obeys. Dropped, repeated or misplaced frames show up
 *    here;
 *  - xruns and the measured frame rate against the system clock.
 *
 * The channel count follows the rate: 18 up to 48 kHz, 14 up to 96 kHz,
 * 12 above.
 */
#include <alsa/asoundlib.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CLICKS		8
#define MAX_IN		18
static double amplitude = 0.1;	/* -20 dBFS */

static unsigned int rate = 48000, channels, out_ch, in_ch[MAX_IN] = { 4 }, ins = 1;
static snd_pcm_uframes_t period = 256, buffer, click_gap, sine_start;

static snd_pcm_t *open_pcm(const char *name, snd_pcm_stream_t dir)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_uframes_t got_period = period, got_buffer = buffer;
	snd_pcm_t *pcm;
	int err;

	err = snd_pcm_open(&pcm, name, dir, 0);
	if (err < 0) {
		fprintf(stderr, "open %s: %s\n", name, snd_strerror(err));
		exit(1);
	}
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(pcm, hw, channels)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &got_period, NULL)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &got_buffer)) < 0 ||
	    (err = snd_pcm_hw_params(pcm, hw)) < 0) {
		fprintf(stderr, "hw_params (%s, %u Hz, %u ch): %s\n",
			dir == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture", rate, channels,
			snd_strerror(err));
		exit(1);
	}
	period = got_period;
	buffer = got_buffer;

	/* Start only through the link, and never stop on xrun-free silence */
	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(pcm, sw);
	snd_pcm_sw_params_set_start_threshold(pcm, sw, INT32_MAX);
	snd_pcm_sw_params_set_avail_min(pcm, sw, period);
	if ((err = snd_pcm_sw_params(pcm, sw)) < 0) {
		fprintf(stderr, "sw_params: %s\n", snd_strerror(err));
		exit(1);
	}
	return pcm;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int parse_ins(char *arg)
{
	char *tok, *end;

	ins = 0;
	for (tok = strtok(arg, ","); tok; tok = strtok(NULL, ",")) {
		long ch = strtol(tok, &end, 10);

		if (*end || ch < 0 || ins == MAX_IN)
			return -1;
		in_ch[ins++] = ch;
	}
	return ins ? 0 : -1;
}

/* Clicks: the largest sample in the 100 ms after each one */
static void report_clicks(const double *x, snd_pcm_uframes_t got, unsigned int ch)
{
	long lat_min = 1 << 30, lat_max = -1;
	double peak_min = 1;
	unsigned int k, found = 0;
	snd_pcm_uframes_t i;

	for (k = 1; k <= CLICKS; k++) {
		snd_pcm_uframes_t at = k * click_gap, best = at;

		for (i = at; i < at + click_gap && i < got; i++)
			if (fabs(x[i]) > fabs(x[best]))
				best = i;
		if (fabs(x[best]) < amplitude / 30)
			continue;
		found++;
		if ((long)(best - at) < lat_min)
			lat_min = best - at;
		if ((long)(best - at) > lat_max)
			lat_max = best - at;
		if (fabs(x[best]) < peak_min)
			peak_min = fabs(x[best]);
	}
	if (!found)
		printf("clicks: none found on input channel %u (cable?)\n", ch);
	else
		printf("clicks: %u/%d found, round trip %ld..%ld frames (%.2f ms), peak %.1f dBFS\n",
		       found, CLICKS, lat_min, lat_max, lat_max * 1000.0 / rate,
		       20 * log10(peak_min));
}

/* Sine: least-squares c, then the residual of the recurrence. Returns glitches. */
static int report_sine(const double *x, snd_pcm_uframes_t got)
{
	snd_pcm_uframes_t from = sine_start + rate / 5, glitches = 0, last = 0, first = 0, i;
	double num = 0, den = 0, c, peak = 0, worst = 0, sum = 0;

	if (got < from + rate / 2) {
		printf("sine: capture too short\n");
		return -1;
	}
	for (i = from + 2; i < got; i++) {
		num += x[i - 1] * (x[i] + x[i - 2]);
		den += x[i - 1] * x[i - 1];
		if (fabs(x[i]) > peak)
			peak = fabs(x[i]);
	}
	if (den == 0) {
		printf("sine: digital silence\n");
		return -1;
	}
	c = num / den;
	for (i = from + 2; i < got; i++)
		sum += fabs(x[i] - c * x[i - 1] + x[i - 2]);
	sum /= got - from - 2;
	for (i = from + 2; i < got; i++) {
		double r = fabs(x[i] - c * x[i - 1] + x[i - 2]);

		if (r > worst)
			worst = r;
		if (r > 30 * sum && r > peak * 1e-3) {
			if (!glitches || i - last > 64) {
				glitches++;
				if (glitches == 1)
					first = i;
			}
			last = i;
		}
	}
	printf("sine: peak %.1f dBFS, %.2f Hz, residual mean %.1e max %.1e, glitches %lu",
	       20 * log10(peak), acos(fmax(-1, fmin(1, c / 2))) * rate / (2 * M_PI), sum,
	       worst, glitches);
	if (glitches)
		printf(" (first at frame %lu)", first);
	printf("\n");
	return glitches != 0;
}

int main(int argc, char **argv)
{
	const char *dev = "hw:UCX", *dump = NULL;
	double seconds = 5;
	snd_pcm_t *play, *cap;
	snd_pcm_uframes_t total, written = 0, got = 0, i;
	int32_t *out, *in;
	double *x, t0 = 0, t1 = 0;
	snd_pcm_uframes_t rate_first = 0;
	unsigned int play_xruns = 0, cap_xruns = 0, c;
	int opt, err, ret = 1, bad = 0;

	while ((opt = getopt(argc, argv, "D:r:p:b:o:i:s:a:w:")) != -1) {
		switch (opt) {
		case 'D': dev = optarg; break;
		case 'r': rate = atoi(optarg); break;
		case 'p': period = atoi(optarg); break;
		case 'b': buffer = atoi(optarg); break;
		case 'o': out_ch = atoi(optarg); break;
		case 'i':
			if (parse_ins(optarg)) {
				fprintf(stderr, "-i: channel numbers separated by commas, at most %d\n",
					MAX_IN);
				return 1;
			}
			break;
		case 's': seconds = atof(optarg); break;
		case 'a': amplitude = pow(10, atof(optarg) / 20); break;
		case 'w': dump = optarg; break;
		default:
			fprintf(stderr, "usage: looptest [-D dev] [-r rate] [-p period] [-b buffer] "
				"[-o out_ch] [-i in_ch[,in_ch...]] [-s seconds] [-a dBFS] [-w capture.raw]\n");
			return 1;
		}
	}
	channels = rate > 100000 ? 12 : rate > 50000 ? 14 : 18;
	if (!buffer)
		buffer = 4 * period;
	for (c = 0; c < ins; c++)
		bad |= in_ch[c] >= channels;
	if (out_ch >= channels || bad) {
		fprintf(stderr, "channels are 0-%u at %u Hz\n", channels - 1, rate);
		return 1;
	}

	cap = open_pcm(dev, SND_PCM_STREAM_CAPTURE);
	play = open_pcm(dev, SND_PCM_STREAM_PLAYBACK);
	if ((err = snd_pcm_link(cap, play)) < 0) {
		fprintf(stderr, "link: %s\n", snd_strerror(err));
		return 1;
	}

	click_gap = rate / 10;
	sine_start = (CLICKS + 2) * click_gap;
	total = (snd_pcm_uframes_t)(seconds * rate) / period * period;
	if (total < sine_start + rate)
		total = (sine_start + rate) / period * period;
	out = calloc(period * channels, sizeof(*out));
	in = calloc(period * channels, sizeof(*in));
	x = calloc(total * ins, sizeof(*x));
	if (!out || !in || !x) {
		fprintf(stderr, "out of memory\n");
		goto done;
	}

	/* Fill the playback buffer, then start both streams */
#define FILL(n) do { \
		memset(out, 0, period * channels * sizeof(*out)); \
		for (i = 0; i < period; i++) { \
			snd_pcm_uframes_t f = (n) + i; \
			double v = 0; \
			if (f >= sine_start) \
				v = amplitude * sin(2 * M_PI * 1000 * (f - sine_start) / rate); \
			else if (f && f % click_gap == 0 && f / click_gap <= CLICKS) \
				v = amplitude; \
			out[i * channels + out_ch] = (int32_t)lrint(v * 8388607) * 256; \
		} \
	} while (0)

	while (written + period <= buffer) {
		FILL(written);
		if ((err = snd_pcm_writei(play, out, period)) < 0) {
			fprintf(stderr, "prefill: %s\n", snd_strerror(err));
			goto done;
		}
		written += period;
	}
	if ((err = snd_pcm_start(cap)) < 0) {
		fprintf(stderr, "start: %s\n", snd_strerror(err));
		goto done;
	}

	while (got < total) {
		snd_pcm_sframes_t n = snd_pcm_readi(cap, in, period);

		if (n < 0) {
			cap_xruns++;
			fprintf(stderr, "capture at frame %lu: %s\n", got, snd_strerror(n));
			break;
		}
		for (c = 0; c < ins; c++)
			for (i = 0; i < (snd_pcm_uframes_t)n && got + i < total; i++)
				x[c * total + got + i] = in[i * channels + in_ch[c]] / 2147483648.0;
		got += n;
		/* Rate: from 1 s in, so that start-up doesn't count */
		if (!rate_first && got >= rate) {
			rate_first = got;
			t0 = now();
		}
		t1 = now();

		FILL(written);
		n = snd_pcm_writei(play, out, period);
		if (n < 0) {
			play_xruns++;
			fprintf(stderr, "playback at frame %lu: %s\n", written, snd_strerror(n));
			break;
		}
		written += n;
	}
	snd_pcm_drop(play);
	snd_pcm_drop(cap);
	if (got > total)
		got = total;

	printf("%u Hz, %u ch, period %lu, buffer %lu: %lu frames, xruns play %u capture %u",
	       rate, channels, period, buffer, got, play_xruns, cap_xruns);
	if (rate_first && t1 > t0)
		printf(", measured %.0f frames/s", (got - rate_first) / (t1 - t0));
	printf("\n");

	if (dump) {
		FILE *f = fopen(dump, "wb");

		if (f) {
			fwrite(x, sizeof(*x), got, f);
			fclose(f);
		}
	}

	ret = play_xruns || cap_xruns;
	for (c = 0; c < ins; c++) {
		if (ins > 1)
			printf("in %u: ", in_ch[c]);
		report_clicks(x + c * total, got, in_ch[c]);
		if (ins > 1)
			printf("in %u: ", in_ch[c]);
		if (report_sine(x + c * total, got))
			ret = 1;
	}
done:
	free(out);
	free(in);
	free(x);
	return ret;
}
