// SPDX-License-Identifier: MIT
/*
 * Device access through snd-fireface-ucx; see ucxcfg.h.
 *
 * Writes go through the ALSA controls, not the hwdep device: the driver then
 * keeps its register table right, skips values the device already has, and
 * tells other clients (alsamixer, a later TUI) about the change. The hwdep
 * device is read for what the controls can't say: whether the driver knows a
 * register at all (an unknown one reads as the control's minimum). It is
 * written only for the stereo link registers; see reg_write().
 */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ucx_hwdep.h"
#include "ucx_proto.h"
#include "ucxcfg.h"

#define REGS 0x8000

/*
 * The unit buffers 128 DSP words and takes about 12 per millisecond out of
 * the buffer (measured at 48 kHz). A host that fills the buffer wedges the
 * command endpoint: every transfer then times out for 20 s or more. So
 * after each write, wait this long per word it may have sent: 2 words per
 * millisecond, a sixth of what the unit can take.
 */
#define WORD_PACE_US 500

struct ucx_dev {
	snd_ctl_t *ctl;
	int card;
	char hwdep[32];
	int query_fd;	/* hwdep, read-only, for the meter and status ioctls; -1 until used */
};

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
	return -1;
}

int ucx_dev_open(const char *card, struct ucx_dev **devp, char *err, size_t errlen)
{
	snd_ctl_card_info_t *info;
	struct ucx_dev *dev;
	char name[64];
	int ret;

	snd_ctl_card_info_alloca(&info);
	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return fail(err, errlen, "out of memory");
	snprintf(name, sizeof(name), "hw:%s", card);
	ret = snd_ctl_open(&dev->ctl, name, 0);
	if (ret < 0) {
		free(dev);
		return fail(err, errlen, "no ALSA card %s (%s); is snd-fireface-ucx loaded and "
			    "the unit on?", card, snd_strerror(ret));
	}
	ret = snd_ctl_card_info(dev->ctl, info);
	if (ret < 0 || strcmp(snd_ctl_card_info_get_driver(info), "FirefaceUCX")) {
		snd_ctl_close(dev->ctl);
		free(dev);
		return fail(err, errlen, "card %s is not driven by snd-fireface-ucx", card);
	}
	dev->card = snd_ctl_card_info_get_card(info);
	dev->query_fd = -1;
	snprintf(dev->hwdep, sizeof(dev->hwdep), "/dev/snd/hwC%dD0", dev->card);
	if (access(dev->hwdep, R_OK)) {
		ret = fail(err, errlen, "%s: %s", dev->hwdep, strerror(errno));
		snd_ctl_close(dev->ctl);
		free(dev);
		return ret;
	}
	*devp = dev;
	return 0;
}

void ucx_dev_close(struct ucx_dev *dev)
{
	if (!dev)
		return;
	if (dev->query_fd >= 0)
		close(dev->query_fd);
	snd_ctl_close(dev->ctl);
	free(dev);
}

/*
 * Read or write all @n values of the mixer control @name. A write returns 1
 * if the driver sent something, 0 if the device already had the values.
 */
static int elem_rw(struct ucx_dev *dev, const char *name, long *values, unsigned int n,
		   bool write, char *err, size_t errlen)
{
	bool mix = strstr(name, " Mix Playback Volume");
	snd_ctl_elem_id_t *id;
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_value_t *value;
	snd_ctl_elem_type_t type;
	unsigned int i;
	int ret;

	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_value_alloca(&value);
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, name);
	snd_ctl_elem_info_set_id(info, id);
	ret = snd_ctl_elem_info(dev->ctl, info);
	if (ret < 0)
		return fail(err, errlen, "control '%s': %s (driver too old for this tool?)", name,
			    snd_strerror(ret));
	type = snd_ctl_elem_info_get_type(info);
	if (snd_ctl_elem_info_get_count(info) != n)
		return fail(err, errlen, "control '%s' has %u values, expected %u", name,
			    snd_ctl_elem_info_get_count(info), n);
	/*
	 * A mix control without -1 can't tell "off" from "unknown", and writing
	 * 0 to it would not reach the device (driver before 2026-11-12)
	 */
	if (mix && snd_ctl_elem_info_get_min(info) != -1)
		return fail(err, errlen, "control '%s' has no -1 (unknown) value: this "
			    "snd-fireface-ucx is too old", name);

	snd_ctl_elem_value_set_id(value, id);
	if (!write) {
		ret = snd_ctl_elem_read(dev->ctl, value);
		if (ret < 0)
			return fail(err, errlen, "reading '%s': %s", name, snd_strerror(ret));
	}
	for (i = 0; i < n; i++) {
		switch (type) {
		case SND_CTL_ELEM_TYPE_BOOLEAN:
			if (write)
				snd_ctl_elem_value_set_boolean(value, i, values[i]);
			else
				values[i] = snd_ctl_elem_value_get_boolean(value, i);
			break;
		case SND_CTL_ELEM_TYPE_ENUMERATED:
			if (write)
				snd_ctl_elem_value_set_enumerated(value, i, values[i]);
			else
				values[i] = snd_ctl_elem_value_get_enumerated(value, i);
			break;
		case SND_CTL_ELEM_TYPE_INTEGER:
			if (write)
				snd_ctl_elem_value_set_integer(value, i, values[i]);
			else
				values[i] = snd_ctl_elem_value_get_integer(value, i);
			break;
		default:
			return fail(err, errlen, "control '%s' has an unexpected type", name);
		}
	}
	if (!write)
		return 0;
	ret = snd_ctl_elem_write(dev->ctl, value);
	if (ret == -ETIMEDOUT)
		return fail(err, errlen, "writing '%s': the unit stopped taking DSP words (too "
			    "many at once, from this or another program?); it recovers by itself "
			    "within two minutes", name);
	if (ret < 0)
		return fail(err, errlen, "writing '%s': %s", name, snd_strerror(ret));
	for (i = 0; i < n; i++)
		if (!mix || values[i] != -1)
			usleep(WORD_PACE_US);
	return ret;
}

/* Stereo link register of a channel: inputs are channels 0-17, outputs 18-35 */
static unsigned int link_reg(bool output, int ch)
{
	return (output ? UCX_DSP_PHYS_INPUTS + ch : ch) << 8 | (output ? 0x04 : 0x02);
}

static int words_write(struct ucx_dev *dev, const uint32_t *words, unsigned int n, char *err,
		       size_t errlen);

/*
 * Write per-channel registers as DSP words through the hwdep device; the
 * driver records them in its table. Only registers of the 36 channels are
 * let through: everything from 0x3e00 up is a command to the unit, and
 * 0x3e05/0x3e06 store its state to flash.
 */
static int reg_write(struct ucx_dev *dev, const unsigned int *regs, const int *values,
		     unsigned int n, char *err, size_t errlen)
{
	uint32_t words[UCX_HWDEP_DSP_MAX_WORDS];
	unsigned int i;

	if (n > UCX_HWDEP_DSP_MAX_WORDS)
		return fail(err, errlen, "too many DSP words in one write");
	for (i = 0; i < n; i++) {
		if (regs[i] >> 8 >= UCX_DSP_PHYS_INPUTS + UCX_DSP_OUTPUTS)
			return fail(err, errlen, "refusing to write DSP register %04x", regs[i]);
		words[i] = regs[i] << 16 | (values[i] & 0xffff);
	}
	return words_write(dev, words, n, err, errlen);
}

/* One transfer of at most UCX_HWDEP_DSP_MAX_WORDS words, then the pause that paces them */
static int words_write(struct ucx_dev *dev, const uint32_t *words, unsigned int n, char *err,
		       size_t errlen)
{
	ssize_t ret;
	int fd;

	fd = open(dev->hwdep, O_WRONLY | O_CLOEXEC);
	ret = fd < 0 ? -1 : write(fd, words, n * sizeof(*words));
	if (ret < 0)
		fail(err, errlen, "%s: %s", dev->hwdep, errno == ETIMEDOUT ?
		     "the unit stopped taking DSP words; it recovers by itself within two minutes" :
		     strerror(errno));
	if (fd >= 0)
		close(fd);
	if (ret < 0)
		return -1;
	usleep(n * WORD_PACE_US);
	return 0;
}

static void volume_ctl(int out, char *name, size_t len)
{
	snprintf(name, len, "%s Playback Volume", ucx_output_name(out));
}

/* @src selects the control: the PCM mix or the input mix of @out */
static void mix_ctl(int out, int src, char *name, size_t len)
{
	snprintf(name, len, "%s %s Mix Playback Volume", ucx_output_name(out),
		 src < UCX_SRC_INPUT ? "PCM" : "Input");
}

/* The phase invert switches of the same 18 routes */
static void invert_ctl(int out, int src, char *name, size_t len)
{
	snprintf(name, len, "%s %s Mix Invert Switch", ucx_output_name(out),
		 src < UCX_SRC_INPUT ? "PCM" : "Input");
}

int ucx_dev_read(struct ucx_dev *dev, struct ucx_dev_state *state, char *err, size_t errlen)
{
	static uint32_t words[REGS];
	static bool known[REGS];
	static uint16_t regs[REGS];
	long values[UCX_PCMS];
	const char *last = "";
	char name[64];
	ssize_t got;
	int out, src, i, fd;

	/*
	 * The register table: one word per register the driver knows, all in
	 * one read. Opened read-only: this library never sends DSP words itself
	 */
	fd = open(dev->hwdep, O_RDONLY | O_CLOEXEC);
	got = fd < 0 ? -1 : read(fd, words, sizeof(words));
	if (got < 0)
		fail(err, errlen, "%s: %s", dev->hwdep, strerror(errno));
	if (fd >= 0)
		close(fd);
	if (got < 0)
		return -1;
	memset(known, 0, sizeof(known));
	for (i = 0; i < got / 4; i++)
		known[words[i] >> 16 & (REGS - 1)] = true;
	for (i = 0; i < got / 4; i++)
		regs[words[i] >> 16 & (REGS - 1)] = words[i] & 0xffff;

	for (out = 0; out < UCX_MIX_OUTS; out++) {
		if (out < UCX_OUTPUTS) {
			volume_ctl(out, name, sizeof(name));
			if (elem_rw(dev, name, values, 1, false, err, errlen) < 0)
				return -1;
			state->cfg.volume[out] = known[(UCX_DSP_PHYS_INPUTS + out) << 8] ?
				(int)values[0] + UCX_VOL_MIN : UCX_UNKNOWN;
		}

		for (src = 0; src < UCX_SOURCES; src += UCX_PCMS) {
			invert_ctl(out, src, name, sizeof(name));
			if (elem_rw(dev, name, values, UCX_PCMS, false, err, errlen) < 0)
				return -1;
			for (i = 0; i < UCX_PCMS; i++)
				state->cfg.invert[out][src + i] = values[i];
			mix_ctl(out, src, name, sizeof(name));
			if (elem_rw(dev, name, values, UCX_PCMS, false, err, errlen) < 0)
				return -1;
			for (i = 0; i < UCX_PCMS; i++) {
				state->linear[out][src + i] = values[i];
				state->cfg.mix[out][src + i] = values[i] < 0 ? UCX_UNKNOWN :
					ucx_gain_from_linear(values[i]);
			}
		}
	}
	/* The settings of one control follow each other: read each control once */
	for (i = 0; i < UCX_SETTINGS; i++) {
		const struct ucx_setting *s = &ucx_settings[i];

		if (strcmp(last, s->ctl) &&
		    elem_rw(dev, s->ctl, values, s->count, false, err, errlen) < 0)
			return -1;
		last = s->ctl;
		state->cfg.set[i] = known[s->reg] ? (int)values[s->index] : UCX_UNKNOWN;
	}
	/* The unit keeps the link on both channels of a pair; the first one decides */
	for (i = 0; i < UCX_PAIRS; i++) {
		unsigned int out = link_reg(true, 2 * i), in = link_reg(false, 2 * i);

		state->cfg.link_out[i] = known[out] ? regs[out] != 0 : UCX_UNKNOWN;
		state->cfg.link_in[i] = known[in] ? regs[in] != 0 : UCX_UNKNOWN;
	}
	return 0;
}

unsigned int ucx_dev_rate(struct ucx_dev *dev)
{
	static const char *const dirs[] = { "p", "c" };
	char path[64], line[64];
	unsigned int rate = 0, i;
	FILE *f;

	/* Both directions run at one rate; either may be the open one */
	for (i = 0; i < 2 && !rate; i++) {
		snprintf(path, sizeof(path), "/proc/asound/card%d/pcm0%s/sub0/hw_params",
			 dev->card, dirs[i]);
		f = fopen(path, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f))
			if (sscanf(line, "rate: %u", &rate) == 1)
				break;
		fclose(f);
	}
	return rate;
}

/* hw_params of one direction; "closed" when no stream is set up */
static void stream_dir(int card, const char *d, unsigned int *open, unsigned int v[4])
{
	static const char *const keys[] = { "rate: %u", "channels: %u", "period_size: %u",
					    "buffer_size: %u" };
	char path[64], line[64];
	unsigned int i;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/asound/card%d/pcm0%s/sub0/hw_params", card, d);
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
		for (i = 0; i < 4; i++)
			if (sscanf(line, keys[i], &v[i]) == 1)
				*open = 1;
	fclose(f);
}

void ucx_dev_stream(struct ucx_dev *dev, struct ucx_stream_info *info)
{
	static const char *const dirs[] = { "p", "c" };
	unsigned int i, open, v[4];
	FILE *f;

	memset(info, 0, sizeof(*info));
	for (i = 0; i < 2; i++) {
		open = 0;
		memset(v, 0, sizeof(v));
		stream_dir(dev->card, dirs[i], &open, v);
		info->dir[i].open = open && v[0];
		info->dir[i].rate = v[0];
		info->dir[i].channels = v[1];
		info->dir[i].period = v[2];
		info->dir[i].buffer = v[3];
	}
	info->urb_blocks = -1;
	f = fopen("/sys/module/snd_fireface_ucx/parameters/urb_blocks", "r");
	if (f) {
		if (fscanf(f, "%d", &info->urb_blocks) != 1)
			info->urb_blocks = -1;
		else
			info->urb_blocks = ucx_urb_blocks_used(info->urb_blocks);
		fclose(f);
	}
}

unsigned int ucx_round_trip(unsigned int rate, unsigned int urb_blocks)
{
	return ucx_round_trip_frames(rate, urb_blocks);
}

unsigned int ucx_rate_channels(unsigned int rate)
{
	return rate ? ucx_channels(rate) : UCX_PCMS;
}

/* The hwdep ioctls only read from the unit, so the file stays open between them */
static int query(struct ucx_dev *dev, unsigned long request, void *arg, char *err, size_t errlen)
{
	if (dev->query_fd < 0)
		dev->query_fd = open(dev->hwdep, O_RDONLY | O_CLOEXEC);
	if (dev->query_fd < 0 || ioctl(dev->query_fd, request, arg) < 0)
		return fail(err, errlen, "%s: %s", dev->hwdep, strerror(errno));
	return 0;
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/*
 * The banks come in the order the unit sends them, so each is placed by its
 * tag. 1: inputs after their DSP blocks, with the FX return on channels 18
 * and 19; 5: inputs before them; 2: playback; 3: outputs before the volume,
 * with the FX send bus on 18 and 19; 6: outputs after the volume. A bank that didn't arrive stays at UCX_METER_NONE.
 */
int ucx_dev_meters(struct ucx_dev *dev, struct ucx_meters *m, char *err, size_t errlen)
{
	struct ucx_hwdep_meters raw;
	int *dest, peaks[UCX_METER_CHANNELS];
	unsigned int bank, count;
	int ch;

	for (dest = (int *)m; dest < (int *)(m + 1); dest++)
		*dest = UCX_METER_NONE;
	if (query(dev, UCX_HWDEP_METERS, &raw, err, errlen))
		return -1;
	for (bank = 0; bank < UCX_HWDEP_METER_BANKS; bank++) {
		const uint8_t *data = raw.bank[bank].data;

		if (raw.bank[bank].len < UCX_METER_MIN_BYTES)
			continue;
		for (ch = 0; ch < UCX_METER_CHANNELS; ch++) {
			/* 2^27 is full scale; the low four bits are flags */
			uint32_t peak = le32(data + UCX_METER_PEAK_OFFSET + 4 * ch) & ~0xfu;

			peaks[ch] = peak ? (int)lround(200.0 * log10(peak / 134217728.0)) :
					   UCX_METER_NONE;
		}
		switch (le32(data + UCX_METER_TAG_OFFSET)) {
		case 0x11111111:
			memcpy(m->fx_return, peaks + UCX_INPUTS, sizeof(m->fx_return));
			dest = m->input_post, count = UCX_INPUTS;
			break;
		case 0x55555555:
			dest = m->input, count = UCX_INPUTS;
			break;
		case 0x22222222:
			dest = m->playback, count = UCX_PCMS;
			break;
		case 0x33333333:
			dest = m->output_pre, count = UCX_MIX_OUTS;
			break;
		case 0x66666666:
			dest = m->output, count = UCX_OUTPUTS;
			break;
		default:
			continue;
		}
		memcpy(dest, peaks, count * sizeof(*dest));
	}
	return 0;
}

int ucx_dev_status(struct ucx_dev *dev, struct ucx_status *status, char *err, size_t errlen)
{
	/* UCX_STATUS_CLOCK() of each "clock-source" value, in the file's order */
	static const int clocks[] = { 7, 1, 2, 3 };
	struct ucx_hwdep_status raw;
	unsigned int s, i;

	if (query(dev, UCX_HWDEP_STATUS, &raw, err, errlen))
		return -1;
	s = raw.status;
	memset(status, 0, sizeof(*status));
	status->rate = ucx_status_rate(s);
	status->clock = -1;
	for (i = 0; i < 4; i++)
		if (UCX_STATUS_CLOCK(s) == (unsigned int)clocks[i])
			status->clock = i;
	status->in[UCX_SYNC_SPDIF].lock = s & UCX_STATUS_LOCK_COAX;
	status->in[UCX_SYNC_SPDIF].sync = s & UCX_STATUS_SYNC_COAX;
	status->in[UCX_SYNC_SPDIF].rate = ucx_status_rate(UCX_STATUS_COAX_RATE(s) << 24);
	status->in[UCX_SYNC_OPTICAL].lock = s & UCX_STATUS_LOCK_OPTICAL;
	status->in[UCX_SYNC_OPTICAL].sync = s & UCX_STATUS_SYNC_OPTICAL;
	status->in[UCX_SYNC_OPTICAL].rate = ucx_status_rate(UCX_STATUS_OPTICAL_RATE(s) << 24);
	status->in[UCX_SYNC_WORD].lock = s & UCX_STATUS_LOCK_WORD;
	status->in[UCX_SYNC_WORD].sync = s & UCX_STATUS_SYNC_WORD;
	status->in[UCX_SYNC_WORD].rate = ucx_status_rate(UCX_STATUS_WORD_RATE(s) << 24);
	return 0;
}

/*
 * Two gains are the same to the device if they give the same coefficient.
 * Above -6 dB the coefficient drops 3 bits, and the driver reads back the
 * coefficient, not what was written.
 */
static bool same_gain(int linear_a, int linear_b)
{
	return ucx_dsp_mix_coef(linear_a) == ucx_dsp_mix_coef(linear_b);
}

static void add_op(struct ucx_plan *plan, enum ucx_op_kind kind, int phase, int out, int src,
		   int setting, int old, int new)
{
	plan->ops[plan->n++] = (struct ucx_op){ kind, phase, out, src, setting, old, new };
}

/*
 * With AutoSet on, the unit lowers the gain by itself (and reports it), so
 * the file's gain is where it starts from: a lower device gain is not a
 * difference. Otherwise every apply would push the gain back up.
 */
static bool autoset_holds(const struct ucx_config *cur, const struct ucx_config *want, int i)
{
	const struct ucx_setting *s = &ucx_settings[i], *a;
	int j;

	if (s->type != UCX_T_MIC_GAIN && s->type != UCX_T_INST_GAIN)
		return false;
	for (j = 0; j < UCX_SETTINGS; j++) {
		a = &ucx_settings[j];
		if (!a->output && a->ch == s->ch && !strcmp(a->key, "autoset"))
			return cur->set[j] == 1 && want->set[j] == 1 &&
			       cur->set[i] != UCX_UNKNOWN && cur->set[i] < want->set[i];
	}
	return false;
}

static void ucx_plan_levels(const struct ucx_dev_state *cur, const struct ucx_config *want,
			    struct ucx_plan *plan);

/*
 * Order, so that no output is ever louder than it is before or after:
 *  0. stereo links that go off (or are unknown): the two channels of a
 *     linked pair can't be given different values
 *  1. output volumes that go down (an unknown one goes to -65 dB)
 *  2. mixer gains that go down or off
 *  3. input and output settings (every path is now at the lower of its old
 *     and new level)
 *  4. mixer gains that go up, or whose old value is unknown; then the
 *     phase of each route (the driver can only flip a gain it knows)
 *  5. output volumes that go up
 *  6. stereo links that go on: both channels have the same values by now,
 *     so linking changes nothing audible
 * The device applies words in the order sent, though sometimes late.
 */
void ucx_plan_make(const struct ucx_dev_state *cur, const struct ucx_config *want,
		   struct ucx_plan *plan)
{
	int i, phase;

	plan->n = 0;
	for (phase = 0; phase <= 6; phase += 6) {
		for (i = 0; i < 2 * UCX_PAIRS; i++) {
			int pair = i % UCX_PAIRS, input = i >= UCX_PAIRS;
			int old = input ? cur->cfg.link_in[pair] : cur->cfg.link_out[pair];
			int new = input ? want->link_in[pair] : want->link_out[pair];

			if (phase == 0 && old != 0 && (new == 0 || old == UCX_UNKNOWN))
				add_op(plan, UCX_OP_LINK, phase, pair, input, 0, old, 0);
			if (phase == 6 && new == 1 && old != 1)
				add_op(plan, UCX_OP_LINK, phase, pair, input, 0,
				       old == UCX_UNKNOWN ? 0 : old, 1);
		}
		if (phase == 0)
			ucx_plan_levels(cur, want, plan);
	}
}

static void ucx_plan_levels(const struct ucx_dev_state *cur, const struct ucx_config *want,
			    struct ucx_plan *plan)
{
	int out, src, i, phase;

	for (phase = 1; phase <= 5; phase++) {
		for (out = 0; out < UCX_MIX_OUTS && phase != 3; out++) {
			/* The FX send rows have no volume: they never change in phases 1 and 5 */
			int old = out < UCX_OUTPUTS ? cur->cfg.volume[out] : 0;
			int new = out < UCX_OUTPUTS ? want->volume[out] : 0;
			bool down = new < old;

			/*
			 * An unknown volume may be above or below the new one, and
			 * the output's routes are about to change: close it first
			 * and open it last
			 */
			if (old == UCX_UNKNOWN) {
				if (phase == 1)
					add_op(plan, UCX_OP_VOLUME, phase, out, 0, 0, old, UCX_VOL_MIN);
				old = UCX_VOL_MIN;
				down = false;
			}
			if ((phase == 1 || phase == 5) && old != new && down == (phase == 1))
				add_op(plan, UCX_OP_VOLUME, phase, out, 0, 0, old, new);
			for (src = 0; src < UCX_SOURCES && (phase == 2 || phase == 4); src++) {
				int have = cur->linear[out][src];
				int need = ucx_gain_to_linear(want->mix[out][src]);

				if (have >= 0 && same_gain(have, need))
					continue;
				/* Unknown and wanted off can only go down */
				down = have < 0 ? need == 0 : need < have;
				if (down == (phase == 2))
					add_op(plan, UCX_OP_MIX, phase, out, src, 0,
					       cur->cfg.mix[out][src], want->mix[out][src]);
			}
			for (src = 0; src < UCX_SOURCES && phase == 4; src++)
				if (want->mix[out][src] != UCX_MIX_OFF &&
				    cur->cfg.invert[out][src] != want->invert[out][src])
					add_op(plan, UCX_OP_INVERT, phase, out, src, 0,
					       cur->cfg.invert[out][src], want->invert[out][src]);
		}
		for (i = 0; i < UCX_SETTINGS && phase == 3; i++)
			if (cur->cfg.set[i] != want->set[i] && !autoset_holds(&cur->cfg, want, i))
				add_op(plan, UCX_OP_SETTING, phase, 0, 0, i, cur->cfg.set[i], want->set[i]);
	}
}

int ucx_dev_apply(struct ucx_dev *dev, const struct ucx_plan *plan, char *err, size_t errlen)
{
	long values[UCX_PCMS];
	char name[64];
	unsigned int n, i;

	for (n = 0; n < plan->n; n++) {
		const struct ucx_op *op = &plan->ops[n];

		switch (op->kind) {
		case UCX_OP_VOLUME:
			volume_ctl(op->out, name, sizeof(name));
			values[0] = op->new - UCX_VOL_MIN;
			if (elem_rw(dev, name, values, 1, true, err, errlen) < 0)
				return -1;
			break;
		case UCX_OP_LINK: {
			unsigned int regs[2] = { link_reg(!op->src, 2 * op->out),
						 link_reg(!op->src, 2 * op->out + 1) };
			int link[2] = { op->new, op->new };

			if (reg_write(dev, regs, link, 2, err, errlen))
				return -1;
			break;
		}
		case UCX_OP_SETTING: {
			const struct ucx_setting *s = &ucx_settings[op->setting];

			/*
			 * One write per control, with the values the plan doesn't
			 * change as they are. The ops of one control follow each
			 * other, and every value the driver doesn't know is among
			 * them, so no register gets a made-up value.
			 */
			if (s->count > 1 &&
			    elem_rw(dev, s->ctl, values, s->count, false, err, errlen) < 0)
				return -1;
			for (;; n++) {
				const struct ucx_op *next = &plan->ops[n + 1];

				values[ucx_settings[plan->ops[n].setting].index] = plan->ops[n].new;
				if (n + 1 == plan->n || next->kind != UCX_OP_SETTING ||
				    strcmp(ucx_settings[next->setting].ctl, s->ctl))
					break;
			}
			if (elem_rw(dev, s->ctl, values, s->count, true, err, errlen) < 0)
				return -1;
			break;
		}
		case UCX_OP_INVERT:
			invert_ctl(op->out, op->src, name, sizeof(name));
			if (elem_rw(dev, name, values, UCX_PCMS, false, err, errlen) < 0)
				return -1;
			for (;; n++) {
				const struct ucx_op *next = &plan->ops[n + 1];

				values[plan->ops[n].src % UCX_PCMS] = plan->ops[n].new;
				if (n + 1 == plan->n || next->kind != UCX_OP_INVERT ||
				    next->out != op->out ||
				    next->src / UCX_PCMS != op->src / UCX_PCMS)
					break;
			}
			if (elem_rw(dev, name, values, UCX_PCMS, true, err, errlen) < 0)
				return -1;
			break;
		case UCX_OP_MIX:
			/*
			 * One write per control: -1 leaves a gain alone, so this pass
			 * touches only its own gains. The ops of one control follow
			 * each other in the plan.
			 */
			for (i = 0; i < UCX_PCMS; i++)
				values[i] = -1;
			for (;; n++) {
				const struct ucx_op *next = &plan->ops[n + 1];

				values[plan->ops[n].src % UCX_PCMS] =
					ucx_gain_to_linear(plan->ops[n].new);
				if (n + 1 == plan->n || next->kind != UCX_OP_MIX ||
				    next->out != op->out ||
				    next->phase != op->phase ||
				    next->src / UCX_PCMS != op->src / UCX_PCMS)
					break;
			}
			mix_ctl(op->out, op->src, name, sizeof(name));
			if (elem_rw(dev, name, values, UCX_PCMS, true, err, errlen) < 0)
				return -1;
			break;
		}
	}
	return 0;
}

/*
 * Send every mixer gain of @want as a DSP word, whatever the driver holds.
 * The mix controls skip a gain the driver believes is already right, and the
 * unit never reports its mixer, so this is the only way to be sure of it
 * (TotalMix does the same at every start: 0x14003e060 wipes its cache of
 * sent gains). Routes that are off go first, so nothing gets louder than
 * @want on the way. The driver records the words in its table.
 * Returns the number of words sent, or -1.
 */
int ucx_dev_push_mix(struct ucx_dev *dev, const struct ucx_config *want, char *err,
		     size_t errlen)
{
	uint32_t words[UCX_HWDEP_DSP_MAX_WORDS];
	unsigned int n = 0;
	int out, src, on, total = 0;

	for (on = 0; on <= 1; on++) {
		for (out = 0; out < UCX_MIX_OUTS; out++) {
			int dev_out = out < UCX_OUTPUTS ? out : UCX_DSP_FX_OUT + out - UCX_OUTPUTS;

			for (src = 0; src < UCX_SOURCES; src++) {
				int gain = ucx_gain_to_linear(want->mix[out][src]);
				int dev_src = src < UCX_SRC_INPUT ? UCX_DSP_PLAYBACK + src :
					src - UCX_SRC_INPUT;

				if ((gain != 0) != on)
					continue;
				if (want->invert[out][src])
					gain = -gain;
				words[n++] = ucx_dsp_mix_cmd(dev_out, dev_src, gain);
				if (n == UCX_HWDEP_DSP_MAX_WORDS) {
					if (words_write(dev, words, n, err, errlen))
						return -1;
					total += n;
					n = 0;
				}
			}
		}
	}
	if (n && words_write(dev, words, n, err, errlen))
		return -1;
	return total + n;
}

static void gain_format(int centi_db, char *buf, size_t len)
{
	if (centi_db == UCX_UNKNOWN)
		snprintf(buf, len, "unknown");
	else if (centi_db == UCX_MIX_OFF)
		snprintf(buf, len, "off");
	else
		snprintf(buf, len, "%.2f dB", centi_db / 100.0);
}

void ucx_op_describe(const struct ucx_op *op, char *buf, size_t len)
{
	char old[32], new[32], what[96];

	switch (op->kind) {
	case UCX_OP_VOLUME:
		if (op->old == UCX_UNKNOWN)
			snprintf(old, sizeof(old), "unknown");
		else
			snprintf(old, sizeof(old), "%.1f dB", op->old / 10.0);
		snprintf(buf, len, "%s volume: %s -> %.1f dB", ucx_output_name(op->out), old,
			 op->new / 10.0);
		break;
	case UCX_OP_MIX:
		gain_format(op->old, old, sizeof(old));
		gain_format(op->new, new, sizeof(new));
		snprintf(buf, len, "%s mix %s: %s -> %s", ucx_output_name(op->out),
			 ucx_source_name(op->src), old, new);
		break;
	case UCX_OP_LINK:
		ucx_pair_name(!op->src, op->out, new, sizeof(new));
		snprintf(buf, len, "%s %s stereo link: %s -> %s", op->src ? "input" : "output", new,
			 op->old == UCX_UNKNOWN ? "unknown" : op->old ? "on" : "off",
			 op->new ? "on" : "off");
		break;
	case UCX_OP_SETTING:
		ucx_setting_format(op->setting, op->old, old, sizeof(old));
		ucx_setting_format(op->setting, op->new, new, sizeof(new));
		ucx_setting_name(op->setting, what, sizeof(what));
		snprintf(buf, len, "%s: %s -> %s", what, old, new);
		break;
	case UCX_OP_INVERT:
		snprintf(buf, len, "%s mix %s phase: %s -> %s", ucx_output_name(op->out),
			 ucx_source_name(op->src), op->old ? "inverted" : "normal",
			 op->new ? "inverted" : "normal");
		break;
	}
}
