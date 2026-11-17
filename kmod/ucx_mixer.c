// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Mixer controls for the Fireface UCX: output volumes and the DSP routing
 * matrix, sent as DSP command words on interface 1 the way TotalMix does
 * (bulk OUT 0x0c; see ucx_proto.h). The same words as RME's FireWire
 * "latter" protocol.
 *
 * Nothing is written on load. The device dumps its registers on request
 * (ucx_mixer_init()) and reports later changes, which the controls read
 * back. The mix matrix is the exception: the device never reports mixer
 * gains, so the mix controls read -1 ("unknown") for every gain not written
 * since load. Writing -1 leaves a gain alone; any other value is sent.
 *
 * The hardware-input half of the matrix survives a reload another way, the
 * one TotalMix uses: with every input gain the driver also writes the level
 * into the unit's stand-alone record (ucx_sa_follow()), which the dump
 * returns, and input gains the driver doesn't know are taken from that
 * record (ucx_sa_reply()). The record is a note the unit keeps, not a
 * measurement: it is right as long as every writer of input gains keeps it,
 * holds 0.1 dB steps and no phase. Playback gains have no such record.
 *
 * Every register with a known meaning has a control (docs/PROTOCOL.md has
 * the map): per-channel values of the 18 inputs or 18 outputs are one
 * control with 18 values, in channel order.
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/int_log.h>
#include <linux/math64.h>
#include <linux/moduleparam.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <sound/control.h>
#include <sound/hwdep.h>
#include <sound/tlv.h>

#include "ucx.h"
#include "ucx_hwdep.h"

static bool dsp_debug;
module_param(dsp_debug, bool, 0644);
MODULE_PARM_DESC(dsp_debug, "Log every DSP command transfer");

static unsigned int poll = 50;
module_param(poll, uint, 0444);
MODULE_PARM_DESC(poll, "Send the TotalMix idle poll word 0x3f00000N every N ms (0 = off)");

static bool drain_replies = true;
module_param(drain_replies, bool, 0444);
MODULE_PARM_DESC(drain_replies, "Keep reading DSP replies on 0x8d like the Windows driver");

/* TotalMix queues at most 63 words per transfer for the UCX (0x140078870) */
#define UCX_DSP_MAX_WORDS	UCX_HWDEP_DSP_MAX_WORDS
#define UCX_REPLY_BYTES		UCX_HWDEP_METER_BYTES
#define UCX_DSP_WORD_US		500
/*
 * The unit takes a full transfer (63 words) out of its buffer in about 5 ms,
 * so a transfer that waits a second won't go through: the buffer
 * overflowed, and the endpoint stays wedged for 20 s to 2 min.
 * While wedged, each probe waits only UCX_DSP_PROBE_MS.
 */
#define UCX_DSP_TIMEOUT_MS	1000
#define UCX_DSP_PROBE_MS	200

/* Output order, all confirmed with the front-panel encoder pages L1..A7 */
static const char *const ucx_output_names[UCX_MIX_OUTS] = {
	"Line 1", "Line 2", "Line 3", "Line 4", "Line 5", "Line 6",
	"Phones L", "Phones R", "S/PDIF L", "S/PDIF R",
	"ADAT 1", "ADAT 2", "ADAT 3", "ADAT 4", "ADAT 5", "ADAT 6", "ADAT 7", "ADAT 8",
	/* Mixer outputs 0x1e/0x1f: what goes into reverb and echo (meter ch 18/19) */
	"FX Send L", "FX Send R",
};

static const DECLARE_TLV_DB_SCALE(ucx_vol_tlv, UCX_DSP_VOL_MIN * 10, 10, 0);
static const DECLARE_TLV_DB_LINEAR(ucx_mix_tlv, TLV_DB_GAIN_MUTE, 602);

static int ucx_dsp_send(struct ucx *ucx, const u32 *words, unsigned int n)
{
	unsigned int pipe = usb_sndbulkpipe(ucx->udev, UCX_EP_AUX_OUT), i;
	int sent, err;

	lockdep_assert_held(&ucx->dsp_lock);
	if (ucx->dsp_gone)
		return -ENODEV;
	if (ucx->dsp_suspended)
		return -EBUSY;
	if (WARN_ON(n > UCX_DSP_MAX_WORDS))
		return -EINVAL;
	/* Fail at once while wedged, apart from one probe per second */
	if (ucx->dsp_wedged && time_before(jiffies, ucx->dsp_retry))
		return -ETIMEDOUT;
	/* One transfer per batch, as TotalMix does; the device applies the words in order */
	for (i = 0; i < n; i++)
		ucx->dsp_buf[i] = cpu_to_le32(words[i]);
	err = usb_bulk_msg(ucx->udev, pipe, ucx->dsp_buf, n * sizeof(*ucx->dsp_buf), &sent,
			   ucx->dsp_wedged ? UCX_DSP_PROBE_MS : UCX_DSP_TIMEOUT_MS);
	if (err == -ETIMEDOUT) {
		if (!ucx->dsp_wedged)
			dev_warn(&ucx->intf->dev,
				 "DSP commands time out (the unit's word buffer is full?); retrying every second\n");
		ucx->dsp_wedged = true;
		ucx->dsp_retry = jiffies + HZ;
	} else if (!err && ucx->dsp_wedged) {
		/*
		 * Whether the words of the transfer that timed out were
		 * applied is unknown: ask for the dump again (ucx_poll_work()).
		 */
		dev_info(&ucx->intf->dev, "DSP commands accepted again\n");
		ucx->dsp_wedged = false;
		WRITE_ONCE(ucx->dump_seen, false);
	} else if (err == -EPIPE) {
		dev_warn(&ucx->intf->dev, "DSP command endpoint stalled, clearing\n");
		usb_clear_halt(ucx->udev, pipe);
	}
	/*
	 * The unit buffers 128 words and takes about 12 per ms out; a full
	 * buffer makes every transfer time out for up to two minutes. Hold
	 * all senders to 2 words per ms.
	 */
	if (!err)
		usleep_range(n * UCX_DSP_WORD_US, n * UCX_DSP_WORD_US + 100);
	/* Skip the periodic poll words, which would flood the log */
	if (dsp_debug && !(n == 1 && (words[0] & 0x7ffffff0) == 0x3f000000))
		for (i = 0; i < n; i++)
			dev_info(&ucx->intf->dev, "dsp word %u/%u: %08x -> %d\n", i + 1, n,
				 words[i], err);
	return err;
}

/*
 * The register table (filled from the device's state dump and change
 * reports, see ucx_store_replies()) is the controls' source of truth.
 * Registers not reported yet read as the control's minimum.
 */
static bool ucx_reg_read(struct ucx *ucx, unsigned int reg, u16 *val)
{
	unsigned long flags;
	bool seen;

	spin_lock_irqsave(&ucx->regs_lock, flags);
	seen = test_bit(reg, ucx->reg_seen);
	*val = ucx->regs[reg];
	spin_unlock_irqrestore(&ucx->regs_lock, flags);
	return seen;
}

/* Record a value we wrote, so reads see it before the device reports it */
static void ucx_reg_store(struct ucx *ucx, unsigned int reg, u16 val)
{
	unsigned long flags;

	spin_lock_irqsave(&ucx->regs_lock, flags);
	set_bit(reg, ucx->reg_seen);
	ucx->regs[reg] = val;
	spin_unlock_irqrestore(&ucx->regs_lock, flags);
}

static unsigned int ucx_vol_reg(unsigned int out)
{
	return (UCX_DSP_PHYS_INPUTS + out) << 8 | UCX_DSP_OUTPUT_VOL;
}

/* Mixer output of the DSP for mix control @out: the 18 outputs, then the FX send bus */
static unsigned int ucx_mix_dev_out(unsigned int out)
{
	return out < UCX_DSP_OUTPUTS ? out : UCX_DSP_FX_OUT + out - UCX_DSP_OUTPUTS;
}

static unsigned int ucx_mix_reg(unsigned int kind, unsigned int out, unsigned int src)
{
	return UCX_DSP_MIXER >> 16 |
	       (ucx_mix_dev_out(out) * UCX_DSP_MIX_STEP + kind * UCX_DSP_PLAYBACK + src);
}

/* Inverse of ucx_dsp_mix_coef(): negative for a phase-inverted route */
static int ucx_mix_signed(u16 coef)
{
	int gain = sign_extend32(coef, 14);

	return coef & 0x8000 ? gain * 8 : gain;
}

static unsigned int ucx_mix_gain(u16 coef)
{
	return min_t(unsigned int, abs(ucx_mix_signed(coef)), UCX_DSP_MIX_MAX);
}

/*
 * Write the stand-alone record for the input gains among the mixer words
 * @mix, which were just sent and stored. Both channels of the output pair
 * are written, so that the unit holds per-channel levels whatever form the
 * pair had before; a channel whose gain the driver doesn't know is recorded
 * as off. The dump registers in the table are updated to what the unit will
 * report. Call with dsp_lock held.
 */
static int ucx_sa_follow(struct ucx *ucx, const u32 *mix, unsigned int n)
{
	u32 words[UCX_DSP_MAX_WORDS];
	unsigned int i, k, m = 0;
	int err = 0;

	for (i = 0; i < n && !err; i++) {
		unsigned int reg = mix[i] >> 16 & 0x7fff, out, src;
		int level[2];
		u16 a, b, coef;

		if (reg < UCX_DSP_MIXER >> 16)
			continue;
		out = (reg - (UCX_DSP_MIXER >> 16)) / UCX_DSP_MIX_STEP;
		src = (reg - (UCX_DSP_MIXER >> 16)) % UCX_DSP_MIX_STEP;
		if (out >= UCX_DSP_OUTPUTS || src >= UCX_DSP_PHYS_INPUTS)
			continue;
		out &= ~1u;
		for (k = 0; k < 2; k++) {
			level[k] = UCX_DSP_SA_MIX_OFF;
			if (ucx_reg_read(ucx, ucx_mix_reg(UCX_DSP_KIND_INPUT, out + k, src), &coef))
				level[k] = ucx_dsp_sa_level(ucx_mix_gain(coef));
			words[m++] = ucx_dsp_sa_cmd(out + k, src, level[k]);
		}
		ucx_dsp_sa_pack(level[0], level[1], &a, &b);
		ucx_reg_store(ucx, ucx_dsp_sa_reg(out, src), a);
		ucx_reg_store(ucx, ucx_dsp_sa_reg(out, src) + 1, b);
		if (m > UCX_DSP_MAX_WORDS - 2) {
			err = ucx_dsp_send(ucx, words, m);
			m = 0;
		}
	}
	if (m && !err)
		err = ucx_dsp_send(ucx, words, m);
	return err;
}

/* Output volume: control value 0..710 is -65.0..+6.0 dB */
static int ucx_vol_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = UCX_DSP_VOL_MAX - UCX_DSP_VOL_MIN;
	return 0;
}

static int ucx_vol_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *value)
{
	struct ucx *ucx = snd_kcontrol_chip(kctl);
	u16 val;

	if (ucx_reg_read(ucx, ucx_vol_reg(kctl->private_value), &val))
		value->value.integer.value[0] =
			clamp_t(int, (s16)val, UCX_DSP_VOL_MIN, UCX_DSP_VOL_MAX) - UCX_DSP_VOL_MIN;
	else
		value->value.integer.value[0] = 0;
	return 0;
}

static int ucx_vol_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *value)
{
	struct ucx *ucx = snd_kcontrol_chip(kctl);
	unsigned int out = kctl->private_value;
	long v = value->value.integer.value[0];
	int vol, err;
	u32 word;
	u16 cur;

	if (v < 0 || v > UCX_DSP_VOL_MAX - UCX_DSP_VOL_MIN)
		return -EINVAL;
	vol = v + UCX_DSP_VOL_MIN;

	mutex_lock(&ucx->dsp_lock);
	if (ucx_reg_read(ucx, ucx_vol_reg(out), &cur) && (s16)cur == vol) {
		mutex_unlock(&ucx->dsp_lock);
		return 0;
	}
	word = ucx_dsp_output_vol_cmd(out, vol);
	err = ucx_dsp_send(ucx, &word, 1);
	if (!err)
		ucx_reg_store(ucx, ucx_vol_reg(out), vol);
	mutex_unlock(&ucx->dsp_lock);
	return err ? err : 1;
}

/*
 * Mix: one value per source, linear gain 0..65536 (32768 = 0 dB), or
 * UCX_MIX_UNKNOWN for a gain the driver hasn't written
 */
#define UCX_MIX_UNKNOWN			(-1)
#define UCX_MIX_PRIVATE(kind, out)	((kind) << 8 | (out))
#define UCX_MIX_KIND(pv)		((pv) >> 8)
#define UCX_MIX_OUT(pv)			((pv) & 0xff)

static int ucx_mix_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = UCX_MIX_SOURCES;
	info->value.integer.min = UCX_MIX_UNKNOWN;
	info->value.integer.max = UCX_DSP_MIX_MAX;
	return 0;
}

static int ucx_mix_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *value)
{
	struct ucx *ucx = snd_kcontrol_chip(kctl);
	unsigned int kind = UCX_MIX_KIND(kctl->private_value);
	unsigned int out = UCX_MIX_OUT(kctl->private_value);
	unsigned int i;
	u16 coef;

	for (i = 0; i < UCX_MIX_SOURCES; i++) {
		if (ucx_reg_read(ucx, ucx_mix_reg(kind, out, i), &coef))
			value->value.integer.value[i] = ucx_mix_gain(coef);
		else
			value->value.integer.value[i] = UCX_MIX_UNKNOWN;
	}
	return 0;
}

static int ucx_mix_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *value)
{
	struct ucx *ucx = snd_kcontrol_chip(kctl);
	unsigned int kind = UCX_MIX_KIND(kctl->private_value);
	unsigned int out = UCX_MIX_OUT(kctl->private_value);
	u32 words[UCX_MIX_SOURCES];
	unsigned int i, n = 0;
	int err = 0;
	u16 cur;

	for (i = 0; i < UCX_MIX_SOURCES; i++)
		if (value->value.integer.value[i] < UCX_MIX_UNKNOWN ||
		    value->value.integer.value[i] > UCX_DSP_MIX_MAX)
			return -EINVAL;

	mutex_lock(&ucx->dsp_lock);
	/*
	 * Send the gains that are unknown or differ. A client that reads the
	 * control and writes it back (alsactl restore, or changing one source
	 * of the 18) writes -1 for the unknown ones, which leaves the device's
	 * routes alone; writing 0 mutes a route whether it was known or not.
	 */
	for (i = 0; i < UCX_MIX_SOURCES; i++) {
		long gain = value->value.integer.value[i];

		bool known;

		if (gain == UCX_MIX_UNKNOWN)
			continue;
		/* The sign belongs to the Invert switch: keep it */
		known = ucx_reg_read(ucx, ucx_mix_reg(kind, out, i), &cur);
		if (known && ucx_mix_signed(cur) < 0)
			gain = -gain;
		if (!known || cur != ucx_dsp_mix_coef(gain))
			words[n++] = ucx_dsp_mix_cmd(ucx_mix_dev_out(out),
						     kind * UCX_DSP_PLAYBACK + i, gain);
	}
	if (n)
		err = ucx_dsp_send(ucx, words, n);
	for (i = 0; n && !err && i < n; i++)
		ucx_reg_store(ucx, (words[i] >> 16) & 0x7fff, words[i] & 0xffff);
	if (n && !err)
		err = ucx_sa_follow(ucx, words, n);
	mutex_unlock(&ucx->dsp_lock);
	return err ? err : n > 0;
}

/*
 * Phase invert of each route: the sign of its mixer gain (measured: gain
 * -1.0 = coefficient 0xf000 flips the sum of two paths). A route whose gain
 * is unknown or 0 has no sign to flip and stays as it is.
 */
static int ucx_mixinv_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	info->count = UCX_MIX_SOURCES;
	info->value.integer.min = 0;
	info->value.integer.max = 1;
	return 0;
}

static int ucx_mixinv_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *value)
{
	struct ucx *ucx = snd_kcontrol_chip(kctl);
	unsigned int kind = UCX_MIX_KIND(kctl->private_value);
	unsigned int out = UCX_MIX_OUT(kctl->private_value);
	unsigned int i;
	u16 coef;

	for (i = 0; i < UCX_MIX_SOURCES; i++)
		value->value.integer.value[i] =
			ucx_reg_read(ucx, ucx_mix_reg(kind, out, i), &coef) &&
			ucx_mix_signed(coef) < 0;
	return 0;
}

static int ucx_mixinv_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *value)
{
	struct ucx *ucx = snd_kcontrol_chip(kctl);
	unsigned int kind = UCX_MIX_KIND(kctl->private_value);
	unsigned int out = UCX_MIX_OUT(kctl->private_value);
	u32 words[UCX_MIX_SOURCES];
	unsigned int i, n = 0;
	int err = 0;
	u16 cur;

	mutex_lock(&ucx->dsp_lock);
	for (i = 0; i < UCX_MIX_SOURCES; i++) {
		bool invert = value->value.integer.value[i];
		int gain;

		if (!ucx_reg_read(ucx, ucx_mix_reg(kind, out, i), &cur))
			continue;
		gain = ucx_mix_signed(cur);
		if (gain && (gain < 0) != invert)
			words[n++] = ucx_dsp_mix_cmd(ucx_mix_dev_out(out),
						     kind * UCX_DSP_PLAYBACK + i, -gain);
	}
	if (n)
		err = ucx_dsp_send(ucx, words, n);
	for (i = 0; n && !err && i < n; i++)
		ucx_reg_store(ucx, (words[i] >> 16) & 0x7fff, words[i] & 0xffff);
	mutex_unlock(&ucx->dsp_lock);
	return err ? err : n > 0;
}

static int ucx_add_control(struct ucx *ucx, const struct snd_kcontrol_new *tmpl,
			   const char *name, unsigned long private_value,
			   struct snd_kcontrol **kctl)
{
	struct snd_kcontrol_new knew = *tmpl;
	struct snd_kcontrol *new;
	int err;

	knew.name = name;
	knew.private_value = private_value;
	new = snd_ctl_new1(&knew, ucx);
	err = snd_ctl_add(ucx->card, new);

	/* snd_ctl_add() frees the control on failure; replies must not notify it */
	if (!err)
		*kctl = new;
	return err;
}

/*
 * Controls backed by DSP registers: register = control value * mult + bias.
 * With @count, the control has one value per channel and value i is register
 * reg + (i << 8). With @shared, the one value goes to @shared registers
 * reg + (i << 8): for switches the unit shares between channels. Each entry
 * is verified on hardware (see docs/PROTOCOL.md).
 */
struct ucx_reg_ctl {
	const char *name;
	unsigned int reg;
	int min, max, mult;
	const unsigned int *tlv;
	const char *const *items;	/* enumerated: register value = item index */
	unsigned int count;		/* 0: one register */
	int bias;
	bool invert;			/* switch that is on when the register is 0 */
	unsigned int shared;		/* 0: one register; else one value, @shared registers */
};

/*
 * Analog output reference level (manual ch. 20.1), measured by looping Line
 * Out 1 into input 5: relative to +4 dBu, 0 reads 8.7 dB quieter and 2 reads
 * 6 dB hotter. Same encoding as FireWire latter.rs. Line Out 1-6 share one
 * switch, and it is 1208: with Line 1/2 unlinked, 1308 changes nothing
 * (measured 2026-11-14); linked, a write to 1308 acts only because the link
 * copies it to 1208. 1408-1708 change nothing (2026-11-13). The control
 * writes 1208 and 1308 both, so the pair agrees. Phones has its own
 * switch, the same way: 1808 sets Phones L and R (-10 dBV 8.8 dB quieter,
 * Hi Gain 6.0 dB hotter), 1908 changes neither side while unlinked
 * (measured 2026-11-14); 1808/1908 don't touch the line outputs.
 */
static const char *const ucx_out_level_items[] = { "-10 dBV", "+4 dBu", "Hi Gain" };

/*
 * Input reference level, measured with a Line Out 1 loopback into inputs 3
 * and 5: relative to +4 dBu, 0 reads 8.8 dB hotter and 2 reads 5.8 dB quieter,
 * which matches the manual's -10 dBV and Lo Gain (ch. 19.1). Line In 5-8
 * share one switch, and it is 0407: with Line 5/6 unlinked, 0507 changes
 * nothing (measured 2026-11-14); linked, the link copies a 0507 write to
 * 0407. 0607 and 0707 change nothing (2026-11-13). Inst 3 and 4 have a switch
 * each: input 3 follows 0208 only and input 4 0308 only (measured
 * 2026-11-14, Line Out 6 looped into each in turn).
 */
static const char *const ucx_in_level_items[] = { "-10 dBV", "+4 dBu", "Lo Gain" };

/* Mic preamp gain: 0 dB or 10..60 dB, register value in 0.1 dB */
static const DECLARE_TLV_DB_SCALE(ucx_mic_gain_tlv, 0, 100, 0);
/* Instrument/line gain: 0..12 dB in 0.5 dB steps (register in 0.1 dB) */
static const DECLARE_TLV_DB_SCALE(ucx_inst_gain_tlv, 0, 50, 0);

/* FX send and return: -65.0 (off) .. 0 dB; reverb and echo volume go to +6 dB */
static const DECLARE_TLV_DB_SCALE(ucx_fx_tlv, -6500, 10, 1);
static const DECLARE_TLV_DB_SCALE(ucx_eq_gain_tlv, -2000, 10, 0);
static const DECLARE_TLV_DB_SCALE(ucx_dyn_gain_tlv, -3000, 10, 0);
static const DECLARE_TLV_DB_SCALE(ucx_comp_thr_tlv, -6000, 10, 0);
static const DECLARE_TLV_DB_SCALE(ucx_exp_thr_tlv, -9990, 10, 0);
static const DECLARE_TLV_DB_SCALE(ucx_al_gain_tlv, 0, 10, 0);
static const DECLARE_TLV_DB_SCALE(ucx_al_head_tlv, 300, 10, 0);

static const char *const ucx_slope_items[] = { "6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct" };
static const char *const ucx_eq_low_items[] = { "Peak", "Shelf" };
static const char *const ucx_eq_high_items[] = { "Peak", "Shelf", "Low Pass" };
/* Names from the manual (25.6), in the order of FireWire latter.rs */
static const char *const ucx_reverb_items[] = {
	"Small Room", "Medium Room", "Large Room", "Walls", "Shorty", "Attack", "Swagger",
	"Old School", "Echoistic", "8plus9", "Grand Wide", "Thicker", "Envelope", "Gated", "Space",
};
static const char *const ucx_echo_items[] = { "Stereo Echo", "Stereo Cross", "Pong Echo" };
/* Measured as six steps of falling treble; the frequencies are from latter.rs */
static const char *const ucx_echo_cut_items[] = {
	"Off", "16 kHz", "12 kHz", "8 kHz", "4 kHz", "2 kHz",
};

#define UCX_CH	UCX_DSP_PHYS_INPUTS	/* values per control: 18 inputs or 18 outputs */
#define UCX_OUT_REG(param)	(UCX_DSP_PHYS_INPUTS << 8 | (param))

/* Output mute, on when the register is 0. Pairs up with "<out> Playback Volume" */
#define UCX_OUT_MUTE(name, ch) \
	{ name " Playback Switch", (ch) << 8 | 0x02, 0, 1, 1, NULL, NULL, 0, 0, true }

/*
 * Low cut, 3-band EQ, dynamics and Auto Level of every input and output
 * (same registers as FireWire latter.rs; measured 2026-11-10 on input 5 and
 * Line Out 1). Gains and thresholds in 0.1 dB, Q and ratios in tenths.
 */
#define UCX_STRIP(side, base) \
	{ side " Low Cut Switch", (base) | 0x20, 0, 1, 1, NULL, NULL, UCX_CH }, \
	{ side " Low Cut Frequency", (base) | 0x21, 20, 500, 1, NULL, NULL, UCX_CH }, \
	{ side " Low Cut Slope", (base) | 0x22, 0, 3, 1, NULL, ucx_slope_items, UCX_CH }, \
	{ side " EQ Switch", (base) | 0x40, 0, 1, 1, NULL, NULL, UCX_CH }, \
	{ side " EQ Low Type", (base) | 0x41, 0, 1, 1, NULL, ucx_eq_low_items, UCX_CH }, \
	{ side " EQ Low Gain", (base) | 0x42, -200, 200, 1, ucx_eq_gain_tlv, NULL, UCX_CH }, \
	{ side " EQ Low Frequency", (base) | 0x43, 20, 20000, 1, NULL, NULL, UCX_CH }, \
	{ side " EQ Low Q", (base) | 0x44, 7, 50, 1, NULL, NULL, UCX_CH }, \
	{ side " EQ Mid Gain", (base) | 0x45, -200, 200, 1, ucx_eq_gain_tlv, NULL, UCX_CH }, \
	{ side " EQ Mid Frequency", (base) | 0x46, 20, 20000, 1, NULL, NULL, UCX_CH }, \
	{ side " EQ Mid Q", (base) | 0x47, 7, 50, 1, NULL, NULL, UCX_CH }, \
	{ side " EQ High Type", (base) | 0x48, 0, 2, 1, NULL, ucx_eq_high_items, UCX_CH }, \
	{ side " EQ High Gain", (base) | 0x49, -200, 200, 1, ucx_eq_gain_tlv, NULL, UCX_CH }, \
	{ side " EQ High Frequency", (base) | 0x4a, 20, 20000, 1, NULL, NULL, UCX_CH }, \
	{ side " EQ High Q", (base) | 0x4b, 7, 50, 1, NULL, NULL, UCX_CH }, \
	{ side " Dynamics Switch", (base) | 0x60, 0, 1, 1, NULL, NULL, UCX_CH }, \
	{ side " Dynamics Gain", (base) | 0x61, -300, 300, 1, ucx_dyn_gain_tlv, NULL, UCX_CH }, \
	{ side " Dynamics Attack", (base) | 0x62, 0, 200, 1, NULL, NULL, UCX_CH }, \
	{ side " Dynamics Release", (base) | 0x63, 100, 999, 1, NULL, NULL, UCX_CH }, \
	{ side " Compressor Threshold", (base) | 0x64, -600, 0, 1, ucx_comp_thr_tlv, NULL, UCX_CH }, \
	{ side " Compressor Ratio", (base) | 0x65, 10, 100, 1, NULL, NULL, UCX_CH }, \
	{ side " Expander Threshold", (base) | 0x66, -999, -200, 1, ucx_exp_thr_tlv, NULL, UCX_CH }, \
	{ side " Expander Ratio", (base) | 0x67, 10, 100, 1, NULL, NULL, UCX_CH }, \
	{ side " Auto Level Switch", (base) | 0x80, 0, 1, 1, NULL, NULL, UCX_CH }, \
	{ side " Auto Level Max Gain", (base) | 0x81, 0, 180, 1, ucx_al_gain_tlv, NULL, UCX_CH }, \
	{ side " Auto Level Headroom", (base) | 0x82, 30, 120, 1, ucx_al_head_tlv, NULL, UCX_CH }, \
	{ side " Auto Level Rise Time", (base) | 0x83, 1, 99, 1, NULL, NULL, UCX_CH }

/* Entries without a dB scale and a 0..1 range are switches */
static const struct ucx_reg_ctl ucx_reg_ctls[] = {
	/* Manual ch. 19.2: 0 dB or 10..65 dB; any value in between is accepted */
	{ "Mic 1 Capture Volume", 0x0007, 0, 65, 10, ucx_mic_gain_tlv },
	{ "Mic 2 Capture Volume", 0x0107, 0, 65, 10, ucx_mic_gain_tlv },
	{ "Mic 1 Phantom Power Switch", 0x0008, 0, 1, 1, NULL },
	{ "Mic 2 Phantom Power Switch", 0x0108, 0, 1, 1, NULL },
	{ "Inst 3 Capture Volume", 0x0207, 0, 24, 5, ucx_inst_gain_tlv },
	{ "Inst 4 Capture Volume", 0x0307, 0, 24, 5, ucx_inst_gain_tlv },
	{ "Inst 3 Input Level", 0x0208, 0, 2, 1, NULL, ucx_in_level_items },
	{ "Inst 4 Input Level", 0x0308, 0, 2, 1, NULL, ucx_in_level_items },
	/* Hi-Z instrument mode: measured +6.0 dB on inputs 3 and 4, as in the manual */
	{ "Inst 3 Hi-Z Switch", 0x0209, 0, 1, 1, NULL },
	{ "Inst 4 Hi-Z Switch", 0x0309, 0, 1, 1, NULL },
	/*
	 * AutoSet: lowers the gain by itself to keep 6 dB of headroom (manual
	 * ch. 19.2). Measured on input 1: 56 dB at -2.3 dBFS dropped to 52 dB
	 * within 3 s of setting 000a; the device reports the new gain on 0x8d
	 */
	{ "Mic 1 AutoSet Switch", 0x000a, 0, 1, 1, NULL },
	{ "Mic 2 AutoSet Switch", 0x010a, 0, 1, 1, NULL },
	{ "Inst 3 AutoSet Switch", 0x020a, 0, 1, 1, NULL },
	{ "Inst 4 AutoSet Switch", 0x030a, 0, 1, 1, NULL },
	{ "Line 5-8 Input Level", 0x0407, 0, 2, 1, NULL, ucx_in_level_items, .shared = 2 },
	{ "Line 1-6 Output Level", 0x1208, 0, 2, 1, NULL, ucx_out_level_items, .shared = 2 },
	{ "Phones Output Level", 0x1808, 0, 2, 1, NULL, ucx_out_level_items, .shared = 2 },
	UCX_OUT_MUTE("Line 1", 0x12),
	UCX_OUT_MUTE("Line 2", 0x13),
	UCX_OUT_MUTE("Line 3", 0x14),
	UCX_OUT_MUTE("Line 4", 0x15),
	UCX_OUT_MUTE("Line 5", 0x16),
	UCX_OUT_MUTE("Line 6", 0x17),
	UCX_OUT_MUTE("Phones L", 0x18),
	UCX_OUT_MUTE("Phones R", 0x19),
	UCX_OUT_MUTE("S/PDIF L", 0x1a),
	UCX_OUT_MUTE("S/PDIF R", 0x1b),
	UCX_OUT_MUTE("ADAT 1", 0x1c),
	UCX_OUT_MUTE("ADAT 2", 0x1d),
	UCX_OUT_MUTE("ADAT 3", 0x1e),
	UCX_OUT_MUTE("ADAT 4", 0x1f),
	UCX_OUT_MUTE("ADAT 5", 0x20),
	UCX_OUT_MUTE("ADAT 6", 0x21),
	UCX_OUT_MUTE("ADAT 7", 0x22),
	UCX_OUT_MUTE("ADAT 8", 0x23),
	/* Off makes every output mute (register 02) inactive; measured on ADAT 7 */
	{ "Output Mute Enable Switch", 0x3d01, 0, 1, 1, NULL },
	/*
	 * Input FX send, 0.1 dB, read on the FX bus meters. Input register 00
	 * ("mute" in TotalMix) silences only this send, not the input's routes.
	 */
	{ "Input FX Send Volume", 0x0001, 0, 650, 1, ucx_fx_tlv, NULL, UCX_CH, -650 },
	{ "Input FX Send Switch", 0x0000, 0, 1, 1, NULL, NULL, UCX_CH, 0, true },
	/*
	 * Stereo link: a linked pair has one volume and one of each setting.
	 * The first channel of a pair decides; the unit reports 1 on both.
	 */
	{ "Input Stereo Link Switch", 0x0002, 0, 1, 1, NULL, NULL, UCX_CH },
	{ "Output Stereo Link Switch", UCX_OUT_REG(0x04), 0, 1, 1, NULL, NULL, UCX_CH },
	/* Balance after the fader: -100 left only .. 100 right only, 50 = other side -6 dB */
	{ "Output Balance", UCX_OUT_REG(0x01), -100, 100, 1, NULL, NULL, UCX_CH },
	{ "Output Phase Invert Switch", UCX_OUT_REG(0x07), 0, 1, 1, NULL, NULL, UCX_CH },
	{ "Output FX Return Volume", UCX_OUT_REG(0x03), 0, 650, 1, ucx_fx_tlv, NULL, UCX_CH, -650 },
	UCX_STRIP("Input", 0x0000),
	UCX_STRIP("Output", UCX_OUT_REG(0)),
	/*
	 * Reverb (measured 2026-11-12 on the FX return meters): times in ms,
	 * filters in Hz, room scale and width in 1/100, reverb time in 0.1 s.
	 * Attack/hold/release act on Envelope and Gated, time and high damp
	 * on Space. Hold, release and time were not timed.
	 */
	{ "Reverb Switch", 0x3c00, 0, 1, 1, NULL },
	{ "Reverb Type", 0x3c01, 0, 14, 1, NULL, ucx_reverb_items },
	{ "Reverb Pre Delay", 0x3c02, 0, 999, 1, NULL },
	{ "Reverb Low Cut", 0x3c03, 20, 500, 1, NULL },
	{ "Reverb Room Scale", 0x3c04, 50, 300, 1, NULL },
	{ "Reverb Attack", 0x3c05, 5, 400, 1, NULL },
	{ "Reverb Hold", 0x3c06, 5, 400, 1, NULL },
	{ "Reverb Release", 0x3c07, 5, 400, 1, NULL },
	{ "Reverb High Cut", 0x3c08, 5000, 20000, 1, NULL },
	{ "Reverb Time", 0x3c09, 1, 49, 1, NULL },
	{ "Reverb High Damp", 0x3c0a, 5000, 20000, 1, NULL },
	{ "Reverb Smooth", 0x3c0b, 0, 100, 1, NULL },
	{ "Reverb Volume", 0x3c0c, 0, 710, 1, ucx_vol_tlv, NULL, 0, -650 },
	{ "Reverb Width", 0x3c0d, 0, 100, 1, NULL },
	/* Echo: delay in 0.01 s (measured 0.1 .. 2.0 s), feedback 0..100 */
	{ "Echo Switch", 0x3c20, 0, 1, 1, NULL },
	{ "Echo Type", 0x3c21, 0, 2, 1, NULL, ucx_echo_items },
	{ "Echo Delay", 0x3c22, 0, 200, 1, NULL },
	{ "Echo Feedback", 0x3c23, 0, 100, 1, NULL },
	{ "Echo High Cut", 0x3c24, 0, 5, 1, NULL, ucx_echo_cut_items },
	{ "Echo Volume", 0x3c25, 0, 710, 1, ucx_vol_tlv, NULL, 0, -650 },
	{ "Echo Width", 0x3c26, 0, 100, 1, NULL },
};
static_assert(ARRAY_SIZE(ucx_reg_ctls) == UCX_REG_CTLS);

static unsigned int ucx_reg_ctl_count(const struct ucx_reg_ctl *rc)
{
	return rc->count ? rc->count : 1;
}

static int ucx_reg_ctl_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *info)
{
	const struct ucx_reg_ctl *rc = &ucx_reg_ctls[kctl->private_value];

	if (rc->items)
		return snd_ctl_enum_info(info, ucx_reg_ctl_count(rc), rc->max + 1, rc->items);
	info->type = rc->tlv || rc->max != 1 ? SNDRV_CTL_ELEM_TYPE_INTEGER :
					       SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	info->count = ucx_reg_ctl_count(rc);
	info->value.integer.min = rc->min;
	info->value.integer.max = rc->max;
	return 0;
}

/* Register value of control value @v */
static int ucx_reg_ctl_raw(const struct ucx_reg_ctl *rc, long v)
{
	return rc->invert ? !v : v * rc->mult + rc->bias;
}

static int ucx_reg_ctl_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *value)
{
	const struct ucx_reg_ctl *rc = &ucx_reg_ctls[kctl->private_value];
	struct ucx *ucx = snd_kcontrol_chip(kctl);
	unsigned int i;
	u16 val;

	for (i = 0; i < ucx_reg_ctl_count(rc); i++) {
		/* Unknown: the minimum, or "on" for a mute */
		int v = rc->invert ? 1 : rc->min;

		if (ucx_reg_read(ucx, rc->reg + (i << 8), &val)) {
			if (rc->invert)
				v = !val;
			else
				v = clamp_t(int, ((s16)val - rc->bias) / rc->mult, rc->min,
					    rc->max);
		}
		if (rc->items)
			value->value.enumerated.item[i] = v;
		else
			value->value.integer.value[i] = v;
	}
	return 0;
}

static int ucx_reg_ctl_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *value)
{
	const struct ucx_reg_ctl *rc = &ucx_reg_ctls[kctl->private_value];
	struct ucx *ucx = snd_kcontrol_chip(kctl);
	unsigned int i, n = 0, count = ucx_reg_ctl_count(rc);
	u32 words[UCX_CH];
	int err = 0;
	u16 cur;

	if (WARN_ON(max(count, rc->shared) > ARRAY_SIZE(words)))
		return -EINVAL;
	for (i = 0; i < count; i++) {
		long v = rc->items ? value->value.enumerated.item[i] :
				     value->value.integer.value[i];

		if (v < rc->min || v > rc->max)
			return -EINVAL;
	}
	mutex_lock(&ucx->dsp_lock);
	for (i = 0; i < max(count, rc->shared); i++) {
		unsigned int reg = rc->reg + (i << 8), k = rc->shared ? 0 : i;
		int raw = ucx_reg_ctl_raw(rc, rc->items ? value->value.enumerated.item[k] :
							  value->value.integer.value[k]);

		if (!ucx_reg_read(ucx, reg, &cur) || (s16)cur != raw)
			words[n++] = ucx_dsp_word(reg << 16 | (u16)raw);
	}
	if (n)
		err = ucx_dsp_send(ucx, words, n);
	for (i = 0; n && !err && i < n; i++)
		ucx_reg_store(ucx, (words[i] >> 16) & 0x7fff, words[i] & 0xffff);
	mutex_unlock(&ucx->dsp_lock);
	return err ? err : n > 0;
}

static const struct snd_kcontrol_new ucx_reg_ctl_tmpl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.info = ucx_reg_ctl_info,
	.get = ucx_reg_ctl_get,
	.put = ucx_reg_ctl_put,
};

/*
 * DSP load in percent: the low byte of the heartbeat register 0x3f00.
 * Measured 0 idle, 17 with reverb and echo, 44 with 12 EQs and compressors.
 */
static int ucx_load_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = 255;
	return 0;
}

static int ucx_load_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *value)
{
	u16 val;

	value->value.integer.value[0] =
		ucx_reg_read(snd_kcontrol_chip(kctl), 0x3f00, &val) ? val & 0xff : 0;
	return 0;
}

static const struct snd_kcontrol_new ucx_load_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_CARD,
	.name = "DSP Load",
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info = ucx_load_info,
	.get = ucx_load_get,
};

/*
 * Clock and hardware settings: bits of REG1, written with a mask so each
 * control touches only its own (see UCX_REG1_* in ucx_proto.h). REG1 can't be
 * read, but the unit mirrors each setting in a DSP register, which is where
 * the controls read it. The exception is EQ+D record: no mirror, so it reads
 * what the driver wrote since load (kept at UCX_HWDEP_REG_EQD_RECORD) and
 * "off" until then, whatever the unit is doing.
 */
struct ucx_reg1_ctl {
	const char *name;
	u16 mask;
	u8 shift;
	u16 mirror;			/* register holding the control's value */
	bool invert;			/* on = bit clear */
	const char *const *items;	/* NULL: a switch */
	u8 nitems;
};

/* Order of 0x3d20 (the FireWire UCX's): S/PDIF and optical measured, word clock inferred */
static const char *const ucx_clock_items[] = { "Internal", "S/PDIF", "Optical", "Word Clock" };
static const char *const ucx_optical_items[] = { "ADAT", "S/PDIF" };

static const struct ucx_reg1_ctl ucx_reg1_ctls[UCX_REG1_CTLS] = {
	{ "Clock Source", UCX_REG1_CLOCK_MASK, 1, UCX_DSP_CLOCK_SOURCE, false, ucx_clock_items, 4 },
	{ "Optical Out", UCX_REG1_OPTICAL_SPDIF, 10, UCX_DSP_OPTICAL_SPDIF, false,
	  ucx_optical_items, 2 },
	{ "S/PDIF Out Professional Switch", UCX_REG1_SPDIF_PRO, 7, UCX_DSP_SPDIF_PRO },
	{ "Word Clock Out Single Speed Switch", UCX_REG1_WCK_SINGLE, 6, UCX_DSP_WCK_SINGLE },
	{ "Word Clock In Termination Switch", UCX_REG1_WCK_TERM, 3, UCX_DSP_WCK_TERM },
	/* Measured: capture has the input EQ with the bit clear, the raw input with it set */
	{ "EQ+D Record Switch", UCX_REG1_RECORD_DRY, 8, UCX_HWDEP_REG_EQD_RECORD, true },
};

static int ucx_reg1_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *info)
{
	const struct ucx_reg1_ctl *rc = &ucx_reg1_ctls[kctl->private_value];

	if (rc->items)
		return snd_ctl_enum_info(info, 1, rc->nitems, rc->items);
	info->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = 1;
	return 0;
}

/* The control's value as the unit (or, for EQ+D, the driver) has it; false if unknown */
static bool ucx_reg1_read(struct ucx *ucx, const struct ucx_reg1_ctl *rc, unsigned int *v)
{
	u16 val;

	*v = 0;
	if (!ucx_reg_read(ucx, rc->mirror, &val))
		return false;
	*v = min_t(unsigned int, val, rc->items ? rc->nitems - 1 : 1);
	return true;
}

static int ucx_reg1_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *value)
{
	const struct ucx_reg1_ctl *rc = &ucx_reg1_ctls[kctl->private_value];
	unsigned int v;

	ucx_reg1_read(snd_kcontrol_chip(kctl), rc, &v);
	if (rc->items)
		value->value.enumerated.item[0] = v;
	else
		value->value.integer.value[0] = v;
	return 0;
}

static int ucx_reg1_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *value)
{
	const struct ucx_reg1_ctl *rc = &ucx_reg1_ctls[kctl->private_value];
	struct ucx *ucx = snd_kcontrol_chip(kctl);
	unsigned int v = rc->items ? value->value.enumerated.item[0] :
				     !!value->value.integer.value[0];
	unsigned int cur;
	int err;

	if (v >= (rc->items ? rc->nitems : 2))
		return -EINVAL;
	mutex_lock(&ucx->dsp_lock);
	if (ucx->dsp_gone) {
		err = -ENODEV;
	} else if (ucx_reg1_read(ucx, rc, &cur) && cur == v) {
		err = 0;
	} else {
		err = ucx_vendor_write(ucx, UCX_REQ_REG1, (rc->invert ? !v : v) << rc->shift,
				       rc->mask);
		/* The unit reports the mirror itself; store it for readers before that */
		if (!err) {
			ucx_reg_store(ucx, rc->mirror, v);
			err = 1;
		}
	}
	mutex_unlock(&ucx->dsp_lock);
	return err;
}

static const struct snd_kcontrol_new ucx_reg1_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = ucx_reg1_info,
	.get = ucx_reg1_get,
	.put = ucx_reg1_put,
};

/*
 * Loopback: the output's signal goes to the recording in place of the
 * matching hardware input (manual 27.6). Vendor request 0x15 per output, as
 * the Windows driver sends it for TotalMix (UCX_REQ_LOOPBACK). Nothing reports
 * it, so like EQ+D the control shows what the driver wrote (kept in the
 * register table at UCX_HWDEP_REG_LOOPBACK + output) and "off" until then.
 */
static int ucx_loopback_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *value)
{
	u16 v;

	ucx_reg_read(snd_kcontrol_chip(kctl), UCX_HWDEP_REG_LOOPBACK + kctl->private_value, &v);
	value->value.integer.value[0] = !!v;
	return 0;
}

static int ucx_loopback_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *value)
{
	struct ucx *ucx = snd_kcontrol_chip(kctl);
	unsigned int reg = UCX_HWDEP_REG_LOOPBACK + kctl->private_value;
	u16 v = !!value->value.integer.value[0], cur;
	int err;

	mutex_lock(&ucx->dsp_lock);
	if (ucx->dsp_gone) {
		err = -ENODEV;
	} else if (ucx_reg_read(ucx, reg, &cur) && cur == v) {
		err = 0;
	} else {
		err = ucx_vendor_write(ucx, UCX_REQ_LOOPBACK, v, kctl->private_value);
		if (!err) {
			ucx_reg_store(ucx, reg, v);
			err = 1;
		}
	}
	mutex_unlock(&ucx->dsp_lock);
	return err;
}

static const struct snd_kcontrol_new ucx_loopback_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = snd_ctl_boolean_mono_info,
	.get = ucx_loopback_get,
	.put = ucx_loopback_put,
};

static const struct snd_kcontrol_new ucx_mixinv_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = ucx_mixinv_info,
	.get = ucx_mixinv_get,
	.put = ucx_mixinv_put,
};

static const struct snd_kcontrol_new ucx_vol_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.info = ucx_vol_info,
	.get = ucx_vol_get,
	.put = ucx_vol_put,
	.tlv = { .p = ucx_vol_tlv },
};

static const struct snd_kcontrol_new ucx_mix_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.info = ucx_mix_info,
	.get = ucx_mix_get,
	.put = ucx_mix_put,
	.tlv = { .p = ucx_mix_tlv },
};

/*
 * Reply words are register notifications: parity bit 31, register in bits
 * 30:16, value in 15:0 (same as oscmix's UCX II protocol). Keep the latest
 * value of each register; with dsp_debug, log changes outside the noisy
 * 0x3fxx status range.
 */
/* Tell ALSA clients that the control behind @reg changed on the device */
static void ucx_notify_reg(struct ucx *ucx, unsigned int reg)
{
	struct snd_kcontrol *kctl = NULL;
	unsigned int ch = reg >> 8, i;

	if (ch >= UCX_DSP_PHYS_INPUTS && ch < UCX_DSP_PHYS_INPUTS + UCX_DSP_OUTPUTS &&
	    (reg & 0xff) == UCX_DSP_OUTPUT_VOL) {
		kctl = ucx->vol_kctl[ch - UCX_DSP_PHYS_INPUTS];
	} else if (reg >= UCX_DSP_MIXER >> 16) {
		unsigned int i = reg - (UCX_DSP_MIXER >> 16);
		unsigned int out = i / UCX_DSP_MIX_STEP, kind = i % UCX_DSP_MIX_STEP / UCX_DSP_PLAYBACK;

		if (out >= UCX_DSP_FX_OUT)
			out -= UCX_DSP_FX_OUT - UCX_DSP_OUTPUTS;
		else if (out >= UCX_DSP_OUTPUTS)
			return;
		if (out < UCX_MIX_OUTS && i % UCX_DSP_PLAYBACK < UCX_MIX_SOURCES) {
			kctl = ucx->mix_kctl[kind][out];
			if (ucx->mixinv_kctl[kind][out])
				snd_ctl_notify(ucx->card, SNDRV_CTL_EVENT_MASK_VALUE,
					       &ucx->mixinv_kctl[kind][out]->id);
		}
	}
	for (i = 0; !kctl && i < ARRAY_SIZE(ucx_reg_ctls); i++) {
		const struct ucx_reg_ctl *rc = &ucx_reg_ctls[i];

		if ((rc->reg & 0xff) == (reg & 0xff) && ch >= rc->reg >> 8 &&
		    ch < (rc->reg >> 8) + max(ucx_reg_ctl_count(rc), rc->shared))
			kctl = ucx->reg_kctl[i];
	}
	for (i = 0; !kctl && i < ARRAY_SIZE(ucx_reg1_ctls); i++)
		if (ucx_reg1_ctls[i].mirror == reg)
			kctl = ucx->reg1_kctl[i];
	if (kctl)
		snd_ctl_notify(ucx->card, SNDRV_CTL_EVENT_MASK_VALUE, &kctl->id);
}

/*
 * A register of the stand-alone record arrived from the device: once both
 * registers of an input are there, give the two outputs of the pair their
 * input gain from it, unless the driver already knows that gain (what it
 * wrote itself is exact; the record has 0.1 dB steps and no phase). Called
 * with regs_lock held.
 */
static void ucx_sa_reply(struct ucx *ucx, unsigned int reg)
{
	unsigned int ch = reg >> 8, out, k;
	int src = ucx_dsp_sa_src(reg & 0xff), level[2];

	if (src < 0 || ch < UCX_DSP_PHYS_INPUTS || ch >= UCX_DSP_PHYS_INPUTS + UCX_DSP_OUTPUTS ||
	    (ch - UCX_DSP_PHYS_INPUTS) & 1)
		return;
	out = ch - UCX_DSP_PHYS_INPUTS;
	reg &= ~1u;
	if (!test_bit(reg, ucx->reg_seen) || !test_bit(reg + 1, ucx->reg_seen) ||
	    !ucx_dsp_sa_unpack(ucx->regs[reg], ucx->regs[reg + 1], &level[0], &level[1]))
		return;
	for (k = 0; k < 2; k++) {
		unsigned int mix = ucx_mix_reg(UCX_DSP_KIND_INPUT, out + k, src);

		if (test_and_set_bit(mix, ucx->reg_seen))
			continue;
		ucx->regs[mix] = ucx_dsp_mix_coef(ucx_dsp_sa_gain(level[k]));
		ucx_notify_reg(ucx, mix);
	}
}

/* @from_device: replies from the unit, as opposed to words user space sent */
static void ucx_store_replies(struct ucx *ucx, const __le32 *buf, unsigned int n,
			      bool from_device)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&ucx->regs_lock, flags);
	for (i = 0; i < n; i++) {
		u32 w = le32_to_cpu(buf[i]);
		unsigned int reg = (w >> 16) & 0x7fff;
		u16 val = w & 0xffff;
		bool seen;

		if (!(hweight32(w) & 1))
			continue;	/* bad parity */
		if (from_device && reg == UCX_DSP_DUMP_ONLY_REG)
			ucx->dump_seen = true;
		seen = test_and_set_bit(reg, ucx->reg_seen);
		if (seen && ucx->regs[reg] == val)
			continue;
		if (dsp_debug && reg < 0x3f00)
			dev_info(&ucx->intf->dev, "reg %04x: %04x -> %04x\n", reg,
				 seen ? ucx->regs[reg] : 0, val);
		ucx->regs[reg] = val;
		ucx_notify_reg(ucx, reg);
		if (from_device)
			ucx_sa_reply(ucx, reg);
	}
	spin_unlock_irqrestore(&ucx->regs_lock, flags);
}

/* debugfs "regs": every register reported so far */
static int ucx_regs_show(struct seq_file *m, void *unused)
{
	struct ucx *ucx = m->private;
	unsigned int reg;

	for_each_set_bit(reg, ucx->reg_seen, UCX_REGS)
		seq_printf(m, "%04x %04x\n", reg, READ_ONCE(ucx->regs[reg]));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ucx_regs);

/*
 * Drain DSP replies on 0x8d like the Windows driver does, so they can't back
 * up and stall the command endpoint, and record them as registers.
 */
/* Killed, or the device is gone or asleep: nothing to recover */
static bool ucx_urb_gone(int status)
{
	return status == -ENOENT || status == -ECONNRESET || status == -ESHUTDOWN ||
	       status == -ENODEV || status == -EPERM || status == -EHOSTUNREACH;
}

/* Delay before the next recovery attempt: 100 ms, doubling up to 6.4 s */
static unsigned long ucx_recover_delay(struct ucx *ucx)
{
	return msecs_to_jiffies(100 << min(READ_ONCE(ucx->recover_count), 6u));
}

/*
 * A reply or event URB stopped on an error (@status, from a completion or a
 * resubmit). Resubmitting at once could spin on a stalled endpoint, so
 * ucx_recover_work() does it later.
 */
static void ucx_urb_failed(struct ucx *ucx, int *slot, int status, const char *what)
{
	if (ucx_urb_gone(status))
		return;
	dev_warn_ratelimited(&ucx->intf->dev, "%s stopped: %d, restarting\n", what, status);
	WRITE_ONCE(*slot, status);
	schedule_delayed_work(&ucx->recover_work, ucx_recover_delay(ucx));
}

static void ucx_reply_complete(struct urb *urb)
{
	struct ucx *ucx = urb->context;
	int err;

	if (urb->status) {
		ucx_urb_failed(ucx, &ucx->reply_status, urb->status, "DSP replies");
		return;
	}
	ucx_store_replies(ucx, urb->transfer_buffer, urb->actual_length / 4, true);
	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err)
		ucx_urb_failed(ucx, &ucx->reply_status, err, "DSP replies");
}

/* Meter peak (2^27 = full scale) in 0.1 dBFS; intlog10() returns log10(x) << 24 */
static int ucx_peak_ddb(u32 peak)
{
	return div_s64(((s64)intlog10(peak) - intlog10(1u << 27)) * 200, 1 << 24);
}

/*
 * One read of each of the 5 meter banks on 0x85, the way TotalMix polls them
 * (0x140077e00). Each bank covers the time since its previous read.
 */
static int ucx_meters_read(struct ucx *ucx, struct ucx_hwdep_meters *meters)
{
	unsigned int bank;
	int got, err = 0;

	mutex_lock(&ucx->dsp_lock);
	for (bank = 0; bank < UCX_HWDEP_METER_BANKS && !err; bank++) {
		if (ucx->dsp_gone) {
			err = -ENODEV;
			break;
		}
		/* kmalloc'ed by the caller, so DMA-able */
		err = usb_bulk_msg(ucx->udev, usb_rcvbulkpipe(ucx->udev, UCX_EP_METERS),
				   meters->bank[bank].data, UCX_HWDEP_METER_BYTES, &got,
				   USB_CTRL_GET_TIMEOUT);
		meters->bank[bank].len = err ? 0 : got;
	}
	mutex_unlock(&ucx->dsp_lock);
	return err;
}

/* debugfs "meters": shows where a signal is inside the DSP */
static int ucx_meters_show(struct seq_file *m, void *unused)
{
	struct ucx_hwdep_meters *meters;
	unsigned int bank, ch;
	int err;

	meters = kzalloc(sizeof(*meters), GFP_KERNEL);
	if (!meters)
		return -ENOMEM;
	err = ucx_meters_read(m->private, meters);
	for (bank = 0; bank < UCX_HWDEP_METER_BANKS && !err; bank++) {
		const u8 *buf = meters->bank[bank].data;

		if (meters->bank[bank].len < UCX_METER_MIN_BYTES) {
			seq_printf(m, "short meter read: %u bytes\n", meters->bank[bank].len);
			continue;
		}
		seq_printf(m, "bank %08x peak dBFS:", get_unaligned_le32(buf + UCX_METER_TAG_OFFSET));
		for (ch = 0; ch < UCX_METER_CHANNELS; ch++) {
			u32 peak = get_unaligned_le32(buf + UCX_METER_PEAK_OFFSET + 4 * ch) & ~0xfu;
			int ddb;

			if (!peak)
				continue;
			ddb = ucx_peak_ddb(peak);
			seq_printf(m, " %u:%s%d.%d", ch, ddb < 0 ? "-" : "", abs(ddb) / 10, abs(ddb) % 10);
		}
		seq_putc(m, '\n');
	}
	kfree(meters);
	return err;
}
DEFINE_SHOW_ATTRIBUTE(ucx_meters);

/* debugfs "status": the 4-byte status reads (see UCX_REQ_STATUS in ucx_proto.h) */
static int ucx_status_show(struct seq_file *m, void *unused)
{
	static const u8 requests[] = { UCX_REQ_STATUS, 0x1c, 0x1e, 0x1f };
	struct ucx *ucx = m->private;
	unsigned int i;
	u32 value;
	int err;

	for (i = 0; i < ARRAY_SIZE(requests); i++) {
		err = ucx_vendor_read32(ucx, requests[i], &value);
		if (err) {
			seq_printf(m, "0x%02x: error %d\n", requests[i], err);
			continue;
		}
		seq_printf(m, "0x%02x: %08x", requests[i], value);
		if (requests[i] == UCX_REQ_STATUS)
			seq_printf(m, " (rate %u Hz)", ucx_status_rate(value));
		seq_putc(m, '\n');
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ucx_status);

/*
 * Send words from user space (debugfs "dsp", hwdep) as one transfer, adding
 * parity. Refuses the flash store command 0x3e06 (and 0x3e05, unknown and
 * next to it): the device would overwrite its stored state.
 */
static int ucx_dsp_send_user(struct ucx *ucx, u32 *words, unsigned int n)
{
	__le32 le[UCX_DSP_MAX_WORDS];
	unsigned int i, m = 0;
	int err;

	if (!n || n > UCX_DSP_MAX_WORDS)
		return -EINVAL;
	for (i = 0; i < n; i++) {
		unsigned int reg = words[i] >> 16 & 0x7fff;

		/* Flash store, and the driver's own slots in the table */
		if (reg == 0x3e05 || reg == 0x3e06 || reg >= 0x7f00)
			return -EPERM;
		words[i] = ucx_dsp_word(words[i]);
	}
	mutex_lock(&ucx->dsp_lock);
	err = ucx_dsp_send(ucx, words, n);
	if (err)
		goto unlock;

	/*
	 * The device doesn't echo host writes, so record register writes like
	 * replies. 0x3e00-0x3fff are commands (polls, dump requests); the
	 * mixer gains from 0x4000 up are state, and a gain written here that
	 * the mix controls didn't know about could never be removed by them.
	 * Words for the stand-alone record (output channel, 0xe0 + input) are
	 * not registers: the dump reports the record at other addresses, and
	 * these would land on the ADAT inputs' part of it.
	 */
	for (i = 0; i < n; i++) {
		unsigned int reg = words[i] >> 16 & 0x7fff;

		if (reg >> 8 >= UCX_DSP_PHYS_INPUTS &&
		    reg >> 8 < UCX_DSP_PHYS_INPUTS + UCX_DSP_OUTPUTS &&
		    (reg & 0xff) >= UCX_DSP_SA_MIX_CMD)
			continue;
		if (reg < 0x3e00 || reg >= UCX_DSP_MIXER >> 16)
			le[m++] = cpu_to_le32(words[i]);
	}
	ucx_store_replies(ucx, le, m, false);
	/* Input gains sent this way get their record like those of the controls */
	err = ucx_sa_follow(ucx, words, n);
unlock:
	mutex_unlock(&ucx->dsp_lock);
	return err;
}

/* debugfs "dsp": write hex words to send them as one DSP transfer */
static ssize_t ucx_dsp_write(struct file *file, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct ucx *ucx = file->private_data;
	u32 words[UCX_DSP_MAX_WORDS];
	unsigned int n = 0;
	char *text, *pos, *tok;
	int err;

	text = memdup_user_nul(ubuf, min_t(size_t, len, 1024));
	if (IS_ERR(text))
		return PTR_ERR(text);
	pos = text;
	while ((tok = strsep(&pos, " \t\n")) && n < UCX_DSP_MAX_WORDS) {
		if (!*tok)
			continue;
		err = kstrtou32(tok, 16, &words[n]);
		if (err)
			goto out;
		n++;
	}
	err = ucx_dsp_send_user(ucx, words, n);
out:
	kfree(text);
	return err ? err : len;
}

static const struct file_operations ucx_dsp_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = ucx_dsp_write,
};

/* hwdep: the debugfs tools for the audio group; see ucx_hwdep.h */
static long ucx_hwdep_write(struct snd_hwdep *hw, const char __user *buf, long count,
			    loff_t *offset)
{
	u32 words[UCX_DSP_MAX_WORDS];
	int err;

	if (count <= 0 || count % sizeof(u32) || count > sizeof(words))
		return -EINVAL;
	if (copy_from_user(words, buf, count))
		return -EFAULT;
	err = ucx_dsp_send_user(hw->private_data, words, count / sizeof(u32));
	return err ? err : count;
}

static long ucx_hwdep_read(struct snd_hwdep *hw, char __user *buf, long count, loff_t *offset)
{
	struct ucx *ucx = hw->private_data;
	unsigned long flags;
	unsigned int reg, n = 0;
	size_t bytes;
	u32 *words;
	long ret;

	if (count < 0 || *offset < 0)
		return -EINVAL;
	words = kvmalloc_array(UCX_REGS, sizeof(*words), GFP_KERNEL);
	if (!words)
		return -ENOMEM;
	spin_lock_irqsave(&ucx->regs_lock, flags);
	for_each_set_bit(reg, ucx->reg_seen, UCX_REGS)
		words[n++] = reg << 16 | ucx->regs[reg];
	spin_unlock_irqrestore(&ucx->regs_lock, flags);

	bytes = n * sizeof(*words);
	ret = 0;
	if (*offset < bytes) {
		ret = min_t(size_t, count, bytes - *offset);
		if (copy_to_user(buf, (u8 *)words + *offset, ret))
			ret = -EFAULT;
		else
			*offset += ret;
	}
	kvfree(words);
	return ret;
}

static int ucx_hwdep_ioctl(struct snd_hwdep *hw, struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct ucx_hwdep_meters *meters;
	int err;

	if (cmd == UCX_HWDEP_STATUS) {
		static const u8 requests[] = { UCX_REQ_STATUS, 0x1c, 0x1e, 0x1f };
		struct ucx *ucx = hw->private_data;
		u32 words[ARRAY_SIZE(requests)];
		unsigned int i;

		BUILD_BUG_ON(sizeof(words) != sizeof(struct ucx_hwdep_status));
		for (i = 0; i < ARRAY_SIZE(requests); i++) {
			err = ucx_vendor_read32(ucx, requests[i], &words[i]);
			if (err)
				return err;
		}
		return copy_to_user((void __user *)arg, words, sizeof(words)) ? -EFAULT : 0;
	}
	if (cmd != UCX_HWDEP_METERS)
		return -ENOIOCTLCMD;
	meters = kzalloc(sizeof(*meters), GFP_KERNEL);
	if (!meters)
		return -ENOMEM;
	err = ucx_meters_read(hw->private_data, meters);
	if (!err && copy_to_user((void __user *)arg, meters, sizeof(*meters)))
		err = -EFAULT;
	kfree(meters);
	return err;
}

static int ucx_hwdep_init(struct ucx *ucx)
{
	struct snd_hwdep *hw;
	int err;

	err = snd_hwdep_new(ucx->card, "Fireface UCX DSP", 0, &hw);
	if (err < 0)
		return err;
	strscpy(hw->name, "Fireface UCX DSP", sizeof(hw->name));
	hw->private_data = ucx;
	hw->ops.write = ucx_hwdep_write;
	hw->ops.read = ucx_hwdep_read;
	hw->ops.ioctl = ucx_hwdep_ioctl;
	/* No pointers in struct ucx_hwdep_meters */
	hw->ops.ioctl_compat = ucx_hwdep_ioctl;
	return 0;
}

/*
 * Device events, interrupt IN 0x8b (Windows: 0x140010520 -> 0x14000c530).
 * Word flags: bit 31 stream restart needed, bit 30 counter, bit 29 a 16-bit
 * value. Unflagged words are front-panel events for models 0/1/10/0xd only;
 * for the UCX (model 3) the Windows driver ignores them.
 * Observed: at every stream start the UCX sends a burst of about 17 ms,
 * counting in steps of 12 (one word per 250 us = 48 kHz), flags 0x4 and
 * then 0xc (bit 31 from about 4.5 ms in), then nothing while it streams.
 */
#define UCX_EVENT_BYTES		8

/* The unit's burst at a stream start lasts about 17 ms; this leaves room */
#define UCX_EVENT_START_MS	100

static void ucx_log_event(struct ucx *ucx, u32 w0, u32 w1)
{
	dev_info(&ucx->intf->dev, "event %08x %08x\n", w0, w1);
}

/*
 * Act on the stream events of a running stream, as the Windows driver does
 * (0x14000c530): count the errors the unit reports (UCX_EVENT_ERROR), and
 * restart the streams when it asks for that (UCX_EVENT_RESTART). Not the
 * bursts every start and every stop bring (struct ucx): the start burst
 * carries restart words too. Nothing comes here while the streams run
 * cleanly.
 */
static void ucx_count_events(struct ucx *ucx, const __le32 *buf, unsigned int words)
{
	unsigned int i;
	u32 w;

	if (!READ_ONCE(ucx->count_events) ||
	    time_before(jiffies, READ_ONCE(ucx->started) + msecs_to_jiffies(UCX_EVENT_START_MS)))
		return;
	for (i = 0; i < words; i++) {
		w = le32_to_cpu(buf[i]);
		if (w & UCX_EVENT_VALUE)	/* checked first, as in Windows */
			continue;
		if (w & UCX_EVENT_RESTART) {
			ucx_stream_restart_request(ucx, w);
			return;
		}
		if (w & UCX_EVENT_ERROR)
			atomic_inc(&ucx->unit_errors);
	}
}

static void ucx_event_complete(struct urb *urb)
{
	struct ucx *ucx = urb->context;
	const __le32 *buf = urb->transfer_buffer;
	int err;

	if (urb->status) {
		ucx_urb_failed(ucx, &ucx->event_status, urb->status, "device events");
		return;
	}
	if (dsp_debug && urb->actual_length >= 4)
		ucx_log_event(ucx, le32_to_cpu(buf[0]),
			      urb->actual_length >= 8 ? le32_to_cpu(buf[1]) : 0);
	ucx_count_events(ucx, buf, urb->actual_length / 4);
	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err)
		ucx_urb_failed(ucx, &ucx->event_status, err, "device events");
}

/*
 * Resubmit a reply or event URB that stopped on @status. A stalled endpoint
 * is cleared first; usb_clear_halt() is not sent otherwise, since what the
 * unit does with it on a running endpoint is unknown.
 */
static int ucx_urb_restart(struct ucx *ucx, struct urb *urb, int status)
{
	if (status == -EPIPE) {
		int err = usb_clear_halt(ucx->udev, urb->pipe);

		if (err)
			return err;
	}
	return usb_submit_urb(urb, GFP_KERNEL);
}

/*
 * Restart the reply and event URBs that stopped on errors, with a growing
 * delay while they keep failing (the count starts over after 30 s without
 * a failure). Replies may have been lost, so the dump is asked for again.
 */
void ucx_recover_work(struct work_struct *work)
{
	struct ucx *ucx = container_of(to_delayed_work(work), struct ucx, recover_work);
	int status, err, failed = 0;
	u32 word;

	mutex_lock(&ucx->dsp_lock);
	if (ucx->dsp_gone || ucx->dsp_suspended)
		goto unlock;
	if (time_after(jiffies, ucx->recover_jiffies + 30 * HZ))
		ucx->recover_count = 0;
	ucx->recover_jiffies = jiffies;
	ucx->recover_count++;

	status = xchg(&ucx->reply_status, 0);
	if (status) {
		err = ucx_urb_restart(ucx, ucx->reply_urb, status);
		if (err && !ucx_urb_gone(err)) {
			WRITE_ONCE(ucx->reply_status, err);
			failed = err;
		} else if (!err) {
			word = ucx_dsp_word(UCX_DSP_DUMP_REQUEST);
			ucx_dsp_send(ucx, &word, 1);
		}
	}
	status = xchg(&ucx->event_status, 0);
	if (status) {
		err = ucx_urb_restart(ucx, ucx->event_urb, status);
		if (err && !ucx_urb_gone(err)) {
			WRITE_ONCE(ucx->event_status, err);
			failed = err;
		}
	}
	if (failed) {
		dev_warn_ratelimited(&ucx->intf->dev, "restarting DSP reply/event URBs: %d\n",
				     failed);
		schedule_delayed_work(&ucx->recover_work, ucx_recover_delay(ucx));
	}
unlock:
	mutex_unlock(&ucx->dsp_lock);
}

static int ucx_events_start(struct ucx *ucx)
{
	struct usb_host_interface *alt = ucx->status_intf->cur_altsetting;
	void *buf;

	if (alt->desc.bNumEndpoints < 1 ||
	    alt->endpoint[0].desc.bEndpointAddress != UCX_EP_STATUS_IN)
		return -ENODEV;
	ucx->event_urb = usb_alloc_urb(0, GFP_KERNEL);
	buf = kzalloc(UCX_EVENT_BYTES, GFP_KERNEL);
	if (!ucx->event_urb || !buf) {
		kfree(buf);
		return -ENOMEM;
	}
	usb_fill_int_urb(ucx->event_urb, ucx->udev, usb_rcvintpipe(ucx->udev, UCX_EP_STATUS_IN),
			 buf, UCX_EVENT_BYTES, ucx_event_complete, ucx,
			 alt->endpoint[0].desc.bInterval);
	ucx->event_urb->transfer_flags |= URB_FREE_BUFFER;
	return usb_submit_urb(ucx->event_urb, GFP_KERNEL);
}

/*
 * TotalMix's flush (0x140068d00) sends 0x3f00000N, N = counter & 0xf,
 * whenever it has no other words queued, on every timer tick.
 */
void ucx_poll_work(struct work_struct *work)
{
	struct ucx *ucx = container_of(to_delayed_work(work), struct ucx, poll_work);
	u32 word = ucx_dsp_word(0x3f000000 | (ucx->poll_count++ & 0xf));

	mutex_lock(&ucx->dsp_lock);
	if (!ucx->dsp_gone)
		ucx_dsp_send(ucx, &word, 1);
	/*
	 * After a cold plug the unit does not answer the dump request sent at
	 * probe. Ask again every 40 polls until a register only the dump has
	 * (the stand-alone mix) has arrived from the device.
	 */
	if (!ucx->dsp_gone && !(ucx->poll_count % 40) && !READ_ONCE(ucx->dump_seen)) {
		word = ucx_dsp_word(UCX_DSP_DUMP_REQUEST);
		ucx_dsp_send(ucx, &word, 1);
	}
	mutex_unlock(&ucx->dsp_lock);
	schedule_delayed_work(&ucx->poll_work, msecs_to_jiffies(poll));
}

/*
 * System sleep: stop all DSP traffic. Control writes wait for the card to
 * power up again (snd_power_ref_and_wait()); other senders get -EBUSY.
 */
void ucx_mixer_suspend(struct ucx *ucx)
{
	ucx_pm_log(ucx, "suspend: DSP lock");
	mutex_lock(&ucx->dsp_lock);
	ucx->dsp_suspended = true;
	mutex_unlock(&ucx->dsp_lock);
	ucx_pm_log(ucx, "suspend: poll work");
	/* Not under dsp_lock: both works take it */
	cancel_delayed_work_sync(&ucx->poll_work);
	ucx_pm_log(ucx, "suspend: reply and event URBs");
	usb_kill_urb(ucx->reply_urb);
	usb_kill_urb(ucx->event_urb);
	ucx_pm_log(ucx, "suspend: recover work");
	/* After the kills, so no completion schedules it again */
	cancel_delayed_work_sync(&ucx->recover_work);
}

/*
 * After system sleep. The register table is kept, also after a reset
 * (S3 cuts USB power, so the core resets the unit at every wake):
 * the unit keeps its whole state through a USB reset and even through being
 * switched off and on, including what it never reports (playback gains,
 * loopback; measured). The dump is asked for, which
 * corrects anything the unit reports that changed meanwhile, e.g. from the
 * front panel.
 */
void ucx_mixer_resume(struct ucx *ucx)
{
	u32 word;
	int err;

	ucx_pm_log(ucx, "resume: URBs and dump request");
	mutex_lock(&ucx->dsp_lock);
	ucx->dsp_suspended = false;
	ucx->dsp_wedged = false;
	ucx->reply_status = 0;
	ucx->event_status = 0;
	ucx->recover_count = 0;
	if (drain_replies) {
		err = usb_submit_urb(ucx->reply_urb, GFP_KERNEL);
		if (err)
			ucx_urb_failed(ucx, &ucx->reply_status, err, "DSP replies");
	}
	err = usb_submit_urb(ucx->event_urb, GFP_KERNEL);
	if (err)
		ucx_urb_failed(ucx, &ucx->event_status, err, "device events");
	word = ucx_dsp_word(UCX_DSP_DUMP_REQUEST);
	ucx_dsp_send(ucx, &word, 1);
	mutex_unlock(&ucx->dsp_lock);
	if (poll)
		schedule_delayed_work(&ucx->poll_work, msecs_to_jiffies(poll));
}

int ucx_mixer_init(struct ucx *ucx)
{
	unsigned int out;
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	void *reply;
	u32 word;
	int err;

	ucx->dsp_buf = kmalloc_array(UCX_DSP_MAX_WORDS, sizeof(*ucx->dsp_buf), GFP_KERNEL);
	ucx->reply_urb = usb_alloc_urb(0, GFP_KERNEL);
	reply = kmalloc(UCX_REPLY_BYTES, GFP_KERNEL);
	if (!ucx->dsp_buf || !ucx->reply_urb || !reply) {
		kfree(reply);
		return -ENOMEM;
	}
	usb_fill_bulk_urb(ucx->reply_urb, ucx->udev, usb_rcvbulkpipe(ucx->udev, UCX_EP_AUX_IN),
			  reply, UCX_REPLY_BYTES, ucx_reply_complete, ucx);
	ucx->reply_urb->transfer_flags |= URB_FREE_BUFFER;
	if (drain_replies) {
		err = usb_submit_urb(ucx->reply_urb, GFP_KERNEL);
		if (err)
			return err;
	}

	err = ucx_events_start(ucx);
	if (err)
		return err;

	ucx->debugfs = debugfs_create_dir(dev_name(&ucx->intf->dev), usb_debug_root);
	debugfs_create_file("meters", 0400, ucx->debugfs, ucx, &ucx_meters_fops);
	debugfs_create_file("status", 0400, ucx->debugfs, ucx, &ucx_status_fops);
	debugfs_create_file("dsp", 0200, ucx->debugfs, ucx, &ucx_dsp_fops);
	debugfs_create_file("regs", 0400, ucx->debugfs, ucx, &ucx_regs_fops);

	err = ucx_hwdep_init(ucx);
	if (err < 0)
		return err;

	for (out = 0; out < UCX_MIX_OUTS; out++) {
		const char *o = ucx_output_names[out];
		unsigned int kind;

		if (out < UCX_DSP_OUTPUTS) {
			struct snd_kcontrol *kctl;

			snprintf(name, sizeof(name), "%s Playback Volume", o);
			err = ucx_add_control(ucx, &ucx_vol_ctl, name, out, &ucx->vol_kctl[out]);
			if (err < 0)
				return err;
			snprintf(name, sizeof(name), "%s Loopback Switch", o);
			err = ucx_add_control(ucx, &ucx_loopback_ctl, name, out, &kctl);
			if (err < 0)
				return err;
		}
		for (kind = 0; kind < 2; kind++) {
			snprintf(name, sizeof(name), "%s %s Mix Invert Switch", o,
				 kind == UCX_DSP_KIND_PLAYBACK ? "PCM" : "Input");
			err = ucx_add_control(ucx, &ucx_mixinv_ctl, name,
					      UCX_MIX_PRIVATE(kind, out),
					      &ucx->mixinv_kctl[kind][out]);
			if (err < 0)
				return err;
		}
		snprintf(name, sizeof(name), "%s PCM Mix Playback Volume", o);
		err = ucx_add_control(ucx, &ucx_mix_ctl, name,
				      UCX_MIX_PRIVATE(UCX_DSP_KIND_PLAYBACK, out),
				      &ucx->mix_kctl[UCX_DSP_KIND_PLAYBACK][out]);
		if (err < 0)
			return err;
		snprintf(name, sizeof(name), "%s Input Mix Playback Volume", o);
		err = ucx_add_control(ucx, &ucx_mix_ctl, name,
				      UCX_MIX_PRIVATE(UCX_DSP_KIND_INPUT, out),
				      &ucx->mix_kctl[UCX_DSP_KIND_INPUT][out]);
		if (err < 0)
			return err;
	}

	err = snd_ctl_add(ucx->card, snd_ctl_new1(&ucx_load_ctl, ucx));
	if (err < 0)
		return err;
	for (out = 0; out < ARRAY_SIZE(ucx_reg1_ctls); out++) {
		struct snd_kcontrol_new knew = ucx_reg1_ctl;

		err = ucx_add_control(ucx, &knew, ucx_reg1_ctls[out].name, out,
				      &ucx->reg1_kctl[out]);
		if (err < 0)
			return err;
	}

	for (out = 0; out < ARRAY_SIZE(ucx_reg_ctls); out++) {
		struct snd_kcontrol_new knew = ucx_reg_ctl_tmpl;

		knew.tlv.p = ucx_reg_ctls[out].tlv;
		if (!knew.tlv.p)
			knew.access &= ~SNDRV_CTL_ELEM_ACCESS_TLV_READ;
		err = ucx_add_control(ucx, &knew, ucx_reg_ctls[out].name, out,
				      &ucx->reg_kctl[out]);
		if (err < 0)
			return err;
	}

	/*
	 * Poll like TotalMix (the device then reports changes, e.g. from the
	 * front panel) and ask for a dump of the whole state (0x3f99 = 0x0812,
	 * as in TotalMix's start-up and the 802 map), which fills the register
	 * table that the controls read.
	 */
	if (poll)
		schedule_delayed_work(&ucx->poll_work, msecs_to_jiffies(poll));
	mutex_lock(&ucx->dsp_lock);
	word = ucx_dsp_word(UCX_DSP_DUMP_REQUEST);
	err = ucx_dsp_send(ucx, &word, 1);
	mutex_unlock(&ucx->dsp_lock);
	return err;
}

/* Stop DSP traffic for good; controls then fail with -ENODEV */
void ucx_mixer_disconnect(struct ucx *ucx)
{
	/* Waits for running reads of the meters file */
	debugfs_remove_recursive(ucx->debugfs);
	ucx->debugfs = NULL;

	mutex_lock(&ucx->dsp_lock);
	ucx->dsp_gone = true;
	mutex_unlock(&ucx->dsp_lock);
	/* Not under dsp_lock: the poll work takes it */
	cancel_delayed_work_sync(&ucx->poll_work);

	usb_kill_urb(ucx->reply_urb);
	usb_kill_urb(ucx->event_urb);
	/* After the kills, so no completion schedules it again */
	cancel_delayed_work_sync(&ucx->recover_work);
	usb_free_urb(ucx->reply_urb);
	ucx->reply_urb = NULL;
	usb_free_urb(ucx->event_urb);
	ucx->event_urb = NULL;
	kfree(ucx->dsp_buf);
	ucx->dsp_buf = NULL;
}
