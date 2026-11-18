/* SPDX-License-Identifier: MIT */
/*
 * libucx: the declarative configuration of a Fireface UCX and the device
 * access that applies it, for ucxcfg and later front ends.
 *
 * Two halves:
 *  - config.c: struct ucx_config, the whole desired state of the unit, and
 *    its JSON file format. No device needed.
 *  - device.c: reads what snd-fireface-ucx knows, works out the writes that
 *    make the device match a config, and sends them through the driver's
 *    ALSA controls. The stereo link of a channel pair goes through the
 *    hwdep device as a DSP word; that path refuses every register outside
 *    the per-channel range, so it cannot reach the flash-store registers.
 *    The library sends no vendor requests itself; ucx_dev_status() asks the
 *    driver for the unit's status words, which the driver reads with four.
 *
 * Everything the driver has a control for can be said in the file: see
 * docs/PROTOCOL.md for what each register does.
 */
#ifndef UCXCFG_H
#define UCXCFG_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#define UCX_OUTPUTS	18
#define UCX_INPUTS	18
#define UCX_PCMS	18
/* Rows of the mixer: the outputs, then the FX send bus left and right */
#define UCX_MIX_OUTS	(UCX_OUTPUTS + 2)
/* Mixer sources of one output: the playback channels, then the inputs */
#define UCX_SOURCES	(UCX_PCMS + UCX_INPUTS)
#define UCX_SRC_PCM	0
#define UCX_SRC_INPUT	UCX_PCMS
/* Channels 2n and 2n + 1 form pair n, for inputs and for outputs */
#define UCX_PAIRS	9

/* A value the driver has not seen or written (device state only) */
#define UCX_UNKNOWN	INT_MIN
/* A mixer route that is off */
#define UCX_MIX_OFF	(INT_MIN + 1)

/* Output volume, 0.1 dB */
#define UCX_VOL_MIN	(-650)
#define UCX_VOL_MAX	60
/* Mixer gain, 0.01 dB. -90 dB is the smallest coefficient the DSP has */
#define UCX_GAIN_MIN	(-9000)
#define UCX_GAIN_MAX	600

/* The settings that are one ALSA control and one DSP register each */
enum ucx_setting_type {
	UCX_T_MIC_GAIN,		/* value in dB: 0 or 10..65 */
	UCX_T_INST_GAIN,	/* value in 0.5 dB: 0..24 */
	UCX_T_BOOL,		/* with @invert the file says the opposite of the control */
	UCX_T_IN_LEVEL,		/* index into ucx_in_levels[] */
	UCX_T_OUT_LEVEL,	/* index into ucx_out_levels[] */
	UCX_T_NUM,		/* file value = (value + off) * mul / div, @min..@max */
	UCX_T_ENUM,		/* index into @items */
};

/* A value in the file's units: what ucx_config.set[] holds is the control's value */
struct ucx_setting {
	const char *ctl;	/* ALSA control name */
	unsigned int reg;	/* DSP register, to ask the driver if it is known */
	bool output;		/* channel is an output, else an input */
	int ch;			/* -1: a setting of the whole unit (reverb, echo) */
	const char *key;	/* key in the channel's JSON object; dots nest objects */
	enum ucx_setting_type type;
	int def;		/* value of a channel the file doesn't mention */
	int index, count;	/* value @index of the control's @count values */
	int min, max, off, mul, div;
	const char *const *items;
	int nitems;
	bool invert;
};

/* 14 preamp and level settings, 29 per input, 32 per output, 34 of the unit */
#define UCX_SETTINGS	(14 + 29 * UCX_INPUTS + 32 * UCX_OUTPUTS + 34)
extern struct ucx_setting ucx_settings[UCX_SETTINGS];

struct ucx_config {
	int volume[UCX_OUTPUTS];		/* 0.1 dB */
	int mix[UCX_MIX_OUTS][UCX_SOURCES];	/* 0.01 dB, or UCX_MIX_OFF */
	/* Phase invert of a route (a negative gain in the DSP); 0 for a route that is off */
	int invert[UCX_MIX_OUTS][UCX_SOURCES];
	int set[UCX_SETTINGS];			/* by ucx_settings[] index */
	/*
	 * Stereo link of each pair, 0 or 1. The unit gives a linked pair one
	 * volume and one of each setting: a write to either channel changes
	 * both, also from the front panel. The mixer gains stay separate. In
	 * the file a pair entry ("Phones L/R") is a linked pair and
	 * single-channel entries are an unlinked one.
	 */
	int link_out[UCX_PAIRS];
	int link_in[UCX_PAIRS];
};

/* Channel names. A source is UCX_SRC_PCM + n or UCX_SRC_INPUT + n */
const char *ucx_output_name(int out);	/* also the mixer rows "FX Send L" / "FX Send R" */
/* "output Line 1 eq.mid.gain", "reverb.type" */
void ucx_setting_name(int setting, char *buf, size_t len);
const char *ucx_input_name(int in);
const char *ucx_source_name(int src);
/* "Phones L/R" */
void ucx_pair_name(bool output, int pair, char *buf, size_t len);
/* A setting's value as text ("30 dB", "on", "+4 dBu", "unknown") */
void ucx_setting_format(int setting, int value, char *buf, size_t len);

/* Everything off: the state an empty file describes */
void ucx_config_defaults(struct ucx_config *cfg);
bool ucx_config_has_unknown(const struct ucx_config *cfg);

/*
 * Parse a config file. Returns 0, or -1 with a message in @err that names
 * the file and the place in it. @cfg is complete on success: every value is
 * either from the file or the documented default.
 */
int ucx_config_load(const char *path, struct ucx_config *cfg, char *err, size_t errlen);

/*
 * The user's own config, which ucxcfg-apply.service applies whenever the
 * unit appears, and which `ucxcfg apply` and ucxtui use when given no file:
 * $XDG_CONFIG_HOME/ucx/config.json, or ~/.config/ucx/config.json. Returns
 * -1 if neither variable gives an absolute directory or the path doesn't fit.
 */
int ucx_config_default_path(char *buf, size_t len);
/* Create the missing directories above @path (mkdir -p of its dirname) */
int ucx_config_make_dir(const char *path, char *err, size_t errlen);
int ucx_config_parse(const char *text, size_t len, const char *name, struct ucx_config *cfg,
		     char *err, size_t errlen);

/*
 * The config as JSON text (malloc'ed, NULL if out of memory), in the most
 * compact form: stereo pairs where both sides agree, defaults left out.
 * Loading it back gives the same struct ucx_config, with one exception: a
 * linked pair whose two channels differ in a way a pair entry can't say
 * (volume, settings, or a source on one side only) is written as two
 * channels, so it comes back unlinked. UCX_UNKNOWN values are
 * written as their defaults and listed under an "unknown" key, which makes
 * the file invalid as input until that key is removed.
 */
#define UCX_FORMAT_ASSUME_OFF	1	/* no "unknown" key */
/*
 * Every channel and every setting, defaults included: the whole state, and
 * a list of what the format can say. Routes that are off are still left
 * out, since a missing source is how the format says "off".
 */
#define UCX_FORMAT_FULL		2
char *ucx_config_format(const struct ucx_config *cfg, unsigned int flags);

/* Mixer gain as the mix controls take it: linear, 32768 = 0 dB, 0 = off */
int ucx_gain_to_linear(int centi_db);
int ucx_gain_from_linear(int linear);

/* Device access */
struct ucx_dev;

struct ucx_dev_state {
	struct ucx_config cfg;			/* may hold UCX_UNKNOWN */
	int linear[UCX_MIX_OUTS][UCX_SOURCES];	/* mix as the driver has it; -1 unknown */
};

enum ucx_op_kind { UCX_OP_VOLUME, UCX_OP_MIX, UCX_OP_SETTING, UCX_OP_LINK, UCX_OP_INVERT,
		   UCX_OP_PUSH_MIX };

/* One value to write, with the device's old value for reporting */
struct ucx_op {
	enum ucx_op_kind kind;
	int phase;	/* apply ordering and write batching */
	int out;	/* volume, mix; link: the pair */
	int src;	/* mix; link: 1 for an input pair, 0 for an output pair */
	int setting;
	int old, new;	/* config units; PUSH_MIX: old linear magnitude, new signed linear gain */
};

struct ucx_plan {
	unsigned int n;
	struct ucx_op ops[2 * UCX_OUTPUTS + 2 * UCX_MIX_OUTS * UCX_SOURCES + UCX_SETTINGS +
			  4 * UCX_PAIRS];
};

int ucx_dev_open(const char *card, struct ucx_dev **dev, char *err, size_t errlen);
void ucx_dev_close(struct ucx_dev *dev);
int ucx_dev_read(struct ucx_dev *dev, struct ucx_dev_state *state, char *err, size_t errlen);
/* Sample rate of the running stream, 0 if none is open */
unsigned int ucx_dev_rate(struct ucx_dev *dev);

/*
 * The streams as the driver runs them: what the client that opened them
 * (PipeWire, JACK, an ALSA program) asked for, in frames
 */
struct ucx_stream_info {
	struct {
		bool open;
		unsigned int rate, channels, period, buffer;
	} dir[2];			/* 0 playback, 1 capture */
	int urb_blocks;			/* as the driver uses it (2 or 4), -1 if unreadable */
	/*
	 * Since the unit appeared: stream errors the unit reported, and restarts
	 * after a failed transfer (Windows: "Errors" x/y). -1: the driver has none.
	 */
	long errors, restarts;
};
void ucx_dev_stream(struct ucx_dev *dev, struct ucx_stream_info *info);
/* Frames of a loopback round trip in the unit and driver, without the ALSA buffer (measured) */
unsigned int ucx_round_trip(unsigned int rate, unsigned int urb_blocks);
/* Channels per direction at @rate: 18, 14 or 12 */
unsigned int ucx_rate_channels(unsigned int rate);

/*
 * Peak levels in 0.1 dBFS since the previous read by any program, or
 * UCX_METER_NONE for digital silence. One ioctl, five bulk reads in the
 * driver, about a millisecond; nothing is sent to the DSP.
 */
#define UCX_METER_NONE	INT_MIN
struct ucx_meters {
	int input[UCX_INPUTS];		/* before the input's low cut, EQ and dynamics */
	int input_post[UCX_INPUTS];	/* after them */
	int playback[UCX_PCMS];
	int output_pre[UCX_MIX_OUTS];	/* before the volume; the last two: the FX send bus */
	int output[UCX_OUTPUTS];	/* after the volume */
	int fx_return[2];
};
int ucx_dev_meters(struct ucx_dev *dev, struct ucx_meters *meters, char *err, size_t errlen);

/* What the unit says about its clock; the three inputs it can follow, in this order */
enum { UCX_SYNC_SPDIF, UCX_SYNC_OPTICAL, UCX_SYNC_WORD, UCX_SYNC_INPUTS };
struct ucx_status {
	unsigned int rate;		/* the unit's sample rate, 0 if it reports none */
	int clock;			/* index into the "clock-source" values, -1 if not one of them */
	struct {
		bool lock, sync;
		unsigned int rate;	/* 0: no signal */
	} in[UCX_SYNC_INPUTS];
};
int ucx_dev_status(struct ucx_dev *dev, struct ucx_status *status, char *err, size_t errlen);

/* The writes that take the device from @cur to @want, in a safe order */
void ucx_plan_make(const struct ucx_dev_state *cur, const struct ucx_config *want,
		   struct ucx_plan *plan);
/* Same ordering, with an unconditional DSP write for each of the 720 routes. */
void ucx_plan_make_push(const struct ucx_dev_state *cur, const struct ucx_config *want,
			struct ucx_plan *plan);
int ucx_dev_apply(struct ucx_dev *dev, const struct ucx_plan *plan, char *err, size_t errlen);
/*
 * Send every mixer gain of @want again, also those the driver believes are
 * already right (the unit can't be asked). Returns the word count, or -1.
 */
int ucx_dev_push_mix(struct ucx_dev *dev, const struct ucx_config *want, char *err,
		     size_t errlen);
void ucx_op_describe(const struct ucx_op *op, char *buf, size_t len);

#endif /* UCXCFG_H */
