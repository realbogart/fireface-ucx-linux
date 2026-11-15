// SPDX-License-Identifier: MIT
/*
 * The config model and its JSON file format; see ucxcfg.h and
 * examples/desktop.json.
 *
 * The file is sparse (only what differs from "everything off"), the struct
 * is complete. Loading fills the struct with defaults and then with the
 * file; saving finds the shortest file for a struct. So load -> save -> load
 * gives the same struct, although not always the same text.
 */
#include <errno.h>
#include <jansson.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ucxcfg.h"

#define VERSION 1

static const char *const output_names[UCX_MIX_OUTS] = {
	"Line 1", "Line 2", "Line 3", "Line 4", "Line 5", "Line 6",
	"Phones L", "Phones R", "S/PDIF L", "S/PDIF R",
	"ADAT 1", "ADAT 2", "ADAT 3", "ADAT 4", "ADAT 5", "ADAT 6", "ADAT 7", "ADAT 8",
	/* Mixer rows without an output behind them: what reverb and echo are fed */
	"FX Send L", "FX Send R",
};

static const char *const input_names[UCX_INPUTS] = {
	"Mic 1", "Mic 2", "Inst 3", "Inst 4", "Line 5", "Line 6", "Line 7", "Line 8",
	"S/PDIF L", "S/PDIF R",
	"ADAT 1", "ADAT 2", "ADAT 3", "ADAT 4", "ADAT 5", "ADAT 6", "ADAT 7", "ADAT 8",
};

static const char *const pcm_names[UCX_PCMS] = {
	"PCM 1", "PCM 2", "PCM 3", "PCM 4", "PCM 5", "PCM 6", "PCM 7", "PCM 8", "PCM 9",
	"PCM 10", "PCM 11", "PCM 12", "PCM 13", "PCM 14", "PCM 15", "PCM 16", "PCM 17", "PCM 18",
};

/* Register values, as in the driver's ucx_reg_ctls[] */
static const char *const in_levels[] = { "-10 dBV", "+4 dBu", "Lo Gain" };
static const char *const out_levels[] = { "-10 dBV", "+4 dBu", "Hi Gain" };
#define LEVEL_DEFAULT 1 /* +4 dBu, what the unit's dump shows */

#define IN(name, ch, key, type, reg) { name, reg, false, ch, key, type, 0, .count = 1 }
#define IN_LEVEL(name, ch, reg) { name " Input Level", reg, false, ch, "level", UCX_T_IN_LEVEL, LEVEL_DEFAULT, .count = 1 }

#define BASE_SETTINGS 14
static const struct ucx_setting base_settings[BASE_SETTINGS] = {
	IN("Mic 1 Capture Volume", 0, "gain", UCX_T_MIC_GAIN, 0x0007),
	IN("Mic 2 Capture Volume", 1, "gain", UCX_T_MIC_GAIN, 0x0107),
	IN("Mic 1 Phantom Power Switch", 0, "phantom", UCX_T_BOOL, 0x0008),
	IN("Mic 2 Phantom Power Switch", 1, "phantom", UCX_T_BOOL, 0x0108),
	IN("Inst 3 Capture Volume", 2, "gain", UCX_T_INST_GAIN, 0x0207),
	IN("Inst 4 Capture Volume", 3, "gain", UCX_T_INST_GAIN, 0x0307),
	IN_LEVEL("Inst 3", 2, 0x0208),
	IN_LEVEL("Inst 4", 3, 0x0308),
	IN("Inst 3 Hi-Z Switch", 2, "hi-z", UCX_T_BOOL, 0x0209),
	IN("Inst 4 Hi-Z Switch", 3, "hi-z", UCX_T_BOOL, 0x0309),
	IN("Mic 1 AutoSet Switch", 0, "autoset", UCX_T_BOOL, 0x000a),
	IN("Mic 2 AutoSet Switch", 1, "autoset", UCX_T_BOOL, 0x010a),
	IN("Inst 3 AutoSet Switch", 2, "autoset", UCX_T_BOOL, 0x020a),
	IN("Inst 4 AutoSet Switch", 3, "autoset", UCX_T_BOOL, 0x030a),
	/* Line In 5-8, Line Out 1-6 and Phones have one level each: see global_tmpl[] */
};

/*
 * The settings every input or output has, and those of the whole unit, as
 * templates: ucx_settings[] is filled from them before main() runs. Control
 * names, ranges and register numbers are the driver's (ucx_reg_ctls[]);
 * defaults are what the unit's dump shows after a factory reset.
 */
struct tmpl {
	const char *ctl;
	unsigned int reg;
	const char *key;
	enum ucx_setting_type type;
	int def, min, max, off, mul, div;
	const char *const *items;
	int nitems;
	bool invert;
};

#define T_BOOL(ctl, reg, key, def) { ctl, reg, key, UCX_T_BOOL, def, 0, 1, 0, 1, 1, NULL, 0, false }
/* A switch that is on when the thing is not muted; the file says "mute" */
#define T_MUTE(ctl, reg, key) { ctl, reg, key, UCX_T_BOOL, 1, 0, 1, 0, 1, 1, NULL, 0, true }
#define T_NUM(ctl, reg, key, def, min, max, off, mul, div) \
	{ ctl, reg, key, UCX_T_NUM, def, min, max, off, mul, div, NULL, 0, false }
#define T_ENUM(ctl, reg, key, def, items) \
	{ ctl, reg, key, UCX_T_ENUM, def, 0, sizeof(items) / sizeof(*(items)) - 1, 0, 1, 1, items, \
	  sizeof(items) / sizeof(*(items)), false }
/* 0.1 dB on a control that starts at -65 dB (= off) */
#define T_FX_VOL(ctl, reg, key, def, max) T_NUM(ctl, reg, key, def, 0, max, -650, 1, 10)

static const char *const eq_low_types[] = { "peak", "shelf" };
static const char *const eq_high_types[] = { "peak", "shelf", "low pass" };
static const char *const reverb_types[] = {
	"Small Room", "Medium Room", "Large Room", "Walls", "Shorty", "Attack", "Swagger",
	"Old School", "Echoistic", "8plus9", "Grand Wide", "Thicker", "Envelope", "Gated", "Space",
};
static const char *const echo_types[] = { "Stereo Echo", "Stereo Cross", "Pong Echo" };
static const char *const echo_cuts[] = { "off", "16 kHz", "12 kHz", "8 kHz", "4 kHz", "2 kHz" };

/* Low cut, EQ, dynamics and Auto Level: the same on every input and output */
static const struct tmpl strip_tmpl[] = {
	T_BOOL("Low Cut Switch", 0x20, "lowcut.on", 0),
	T_NUM("Low Cut Frequency", 0x21, "lowcut.freq", 50, 20, 500, 0, 1, 1),
	T_NUM("Low Cut Slope", 0x22, "lowcut.slope", 2, 0, 3, 1, 6, 1),	/* 6..24 dB/oct */
	T_BOOL("EQ Switch", 0x40, "eq.on", 0),
	T_ENUM("EQ Low Type", 0x41, "eq.low.type", 1, eq_low_types),
	T_NUM("EQ Low Gain", 0x42, "eq.low.gain", 0, -200, 200, 0, 1, 10),
	T_NUM("EQ Low Frequency", 0x43, "eq.low.freq", 100, 20, 20000, 0, 1, 1),
	T_NUM("EQ Low Q", 0x44, "eq.low.q", 10, 7, 50, 0, 1, 10),
	T_NUM("EQ Mid Gain", 0x45, "eq.mid.gain", 0, -200, 200, 0, 1, 10),
	T_NUM("EQ Mid Frequency", 0x46, "eq.mid.freq", 1000, 20, 20000, 0, 1, 1),
	T_NUM("EQ Mid Q", 0x47, "eq.mid.q", 10, 7, 50, 0, 1, 10),
	T_ENUM("EQ High Type", 0x48, "eq.high.type", 1, eq_high_types),
	T_NUM("EQ High Gain", 0x49, "eq.high.gain", 0, -200, 200, 0, 1, 10),
	T_NUM("EQ High Frequency", 0x4a, "eq.high.freq", 10000, 20, 20000, 0, 1, 1),
	T_NUM("EQ High Q", 0x4b, "eq.high.q", 10, 7, 50, 0, 1, 10),
	T_BOOL("Dynamics Switch", 0x60, "dynamics.on", 0),
	T_NUM("Dynamics Gain", 0x61, "dynamics.gain", 0, -300, 300, 0, 1, 10),
	T_NUM("Dynamics Attack", 0x62, "dynamics.attack", 10, 0, 200, 0, 1, 1),
	T_NUM("Dynamics Release", 0x63, "dynamics.release", 100, 100, 999, 0, 1, 1),
	T_NUM("Compressor Threshold", 0x64, "dynamics.comp-threshold", -300, -600, 0, 0, 1, 10),
	T_NUM("Compressor Ratio", 0x65, "dynamics.comp-ratio", 10, 10, 100, 0, 1, 10),
	T_NUM("Expander Threshold", 0x66, "dynamics.exp-threshold", -600, -999, -200, 0, 1, 10),
	T_NUM("Expander Ratio", 0x67, "dynamics.exp-ratio", 10, 10, 100, 0, 1, 10),
	T_BOOL("Auto Level Switch", 0x80, "autolevel.on", 0),
	T_NUM("Auto Level Max Gain", 0x81, "autolevel.max-gain", 60, 0, 180, 0, 1, 10),
	T_NUM("Auto Level Headroom", 0x82, "autolevel.headroom", 60, 30, 120, 0, 1, 10),
	T_NUM("Auto Level Rise Time", 0x83, "autolevel.rise-time", 50, 1, 99, 0, 1, 10),
};

static const struct tmpl input_tmpl[] = {
	T_FX_VOL("FX Send Volume", 0x01, "fx-send", 0, 650),
	/* TotalMix's input mute register: it silences only the FX send */
	T_MUTE("FX Send Switch", 0x00, "fx-mute"),
};

static const struct tmpl output_tmpl[] = {
	T_NUM("Balance", 0x01, "balance", 0, -100, 100, 0, 1, 1),
	T_BOOL("Phase Invert Switch", 0x07, "phase-invert", 0),
	T_FX_VOL("FX Return Volume", 0x03, "fx-return", 0, 650),
};

/* Order of the unit's register 3d20: s/pdif and optical measured, word clock inferred */
static const char *const clock_sources[] = { "internal", "s/pdif", "optical", "word clock" };
static const char *const optical_outs[] = { "adat", "s/pdif" };

static const struct tmpl global_tmpl[] = {
	/*
	 * Clock and hardware settings (REG1 bits; the registers are where the
	 * unit reports them). 0x7f10 is the driver's own record of EQ+D record,
	 * which the unit doesn't report: unknown, and so written, once per load.
	 */
	T_ENUM("Clock Source", 0x3d20, "hardware.clock-source", 0, clock_sources),
	T_ENUM("Optical Out", 0x3d41, "hardware.optical-out", 0, optical_outs),
	T_BOOL("S/PDIF Out Professional Switch", 0x3d42, "hardware.spdif-professional", 0),
	T_BOOL("Word Clock Out Single Speed Switch", 0x3d22, "hardware.word-clock-single-speed", 0),
	T_BOOL("Word Clock In Termination Switch", 0x3d40, "hardware.word-clock-termination", 1),
	/* On = bit 0x100 clear, the unit's state before any host wrote it */
	T_BOOL("EQ+D Record Switch", 0x7f10, "hardware.eqd-record", 1),
	/*
	 * One reference level for all four rear inputs and one for Line Out
	 * 1-6 (measured 2026-11-13). The switches are 0407 and 1208; 0507 and
	 * 1308 act only through the stereo link, which copies their writes to
	 * the first channel (measured unlinked, 2026-11-14). Phones the same:
	 * 1808 sets both sides, 1908 neither (2026-11-14). The driver's
	 * control writes both registers of each.
	 */
	T_ENUM("Line 5-8 Input Level", 0x0407, "hardware.line-in-level", 1, in_levels),
	T_ENUM("Line 1-6 Output Level", 0x1208, "hardware.line-out-level", 1, out_levels),
	T_ENUM("Phones Output Level", 0x1808, "hardware.phones-level", 1, out_levels),
	T_BOOL("Output Mute Enable Switch", 0x3d01, "mute-enable", 1),
	T_BOOL("Reverb Switch", 0x3c00, "reverb.on", 0),
	T_ENUM("Reverb Type", 0x3c01, "reverb.type", 0, reverb_types),
	T_NUM("Reverb Pre Delay", 0x3c02, "reverb.pre-delay", 20, 0, 999, 0, 1, 1),
	T_NUM("Reverb Low Cut", 0x3c03, "reverb.low-cut", 20, 20, 500, 0, 1, 1),
	T_NUM("Reverb Room Scale", 0x3c04, "reverb.room-scale", 100, 50, 300, 0, 1, 100),
	T_NUM("Reverb Attack", 0x3c05, "reverb.attack", 100, 5, 400, 0, 1, 1),
	T_NUM("Reverb Hold", 0x3c06, "reverb.hold", 80, 5, 400, 0, 1, 1),
	T_NUM("Reverb Release", 0x3c07, "reverb.release", 200, 5, 400, 0, 1, 1),
	T_NUM("Reverb High Cut", 0x3c08, "reverb.high-cut", 20000, 5000, 20000, 0, 1, 1),
	T_NUM("Reverb Time", 0x3c09, "reverb.time", 20, 1, 49, 0, 1, 10),
	T_NUM("Reverb High Damp", 0x3c0a, "reverb.high-damp", 18000, 5000, 20000, 0, 1, 1),
	T_NUM("Reverb Smooth", 0x3c0b, "reverb.smooth", 100, 0, 100, 0, 1, 1),
	T_FX_VOL("Reverb Volume", 0x3c0c, "reverb.volume", 620, 710),
	T_NUM("Reverb Width", 0x3c0d, "reverb.width", 60, 0, 100, 0, 1, 100),
	T_BOOL("Echo Switch", 0x3c20, "echo.on", 0),
	T_ENUM("Echo Type", 0x3c21, "echo.type", 0, echo_types),
	T_NUM("Echo Delay", 0x3c22, "echo.delay", 60, 0, 200, 0, 1, 100),
	T_NUM("Echo Feedback", 0x3c23, "echo.feedback", 50, 0, 100, 0, 1, 1),
	T_ENUM("Echo High Cut", 0x3c24, "echo.high-cut", 0, echo_cuts),
	T_FX_VOL("Echo Volume", 0x3c25, "echo.volume", 620, 710),
	T_NUM("Echo Width", 0x3c26, "echo.width", 60, 0, 100, 0, 1, 100),
};

#define COUNT(a) ((int)(sizeof(a) / sizeof(*(a))))

struct ucx_setting ucx_settings[UCX_SETTINGS];

/* One setting from a template; @ch < 0 for the unit's own. Returns the next free slot */
static int tmpl_add(int n, const struct tmpl *t, const char *ctl, bool output, int ch, bool array)
{
	struct ucx_setting *s = &ucx_settings[n];

	*s = (struct ucx_setting){
		.ctl = ctl, .output = output, .ch = ch, .key = t->key, .type = t->type,
		.def = t->def, .index = array ? ch : 0, .count = array ? UCX_INPUTS : 1,
		.min = t->min, .max = t->max, .off = t->off, .mul = t->mul, .div = t->div,
		.items = t->items, .nitems = t->nitems, .invert = t->invert,
	};
	s->reg = ch < 0 ? t->reg : (unsigned int)(ch + (output ? UCX_INPUTS : 0)) << 8 | t->reg;
	return n + 1;
}

__attribute__((constructor))
static void settings_init(void)
{
	static const struct tmpl out_mute = T_MUTE("Playback Switch", 0x02, "mute");
	/*
	 * Vendor request 0x15; 0x7f20 + output is the driver's record of it, as
	 * for EQ+D: unknown, and so written, once per load. Off at power-up.
	 */
	static const struct tmpl out_loopback = T_BOOL("Loopback Switch", 0x7f20, "loopback", 0);
	static char names[2][COUNT(strip_tmpl) + COUNT(input_tmpl) + COUNT(output_tmpl)][48];
	static char mutes[UCX_OUTPUTS][48], loopbacks[UCX_OUTPUTS][48];
	int n, side, ch, i;

	for (n = 0; n < BASE_SETTINGS; n++) {
		ucx_settings[n] = base_settings[n];
		ucx_settings[n].count = 1;
	}
	/* The controls of all channels of one side follow each other: see ucx_dev_apply() */
	for (side = 0; side < 2; side++) {
		const struct tmpl *own = side ? output_tmpl : input_tmpl;
		int nown = side ? COUNT(output_tmpl) : COUNT(input_tmpl);

		for (i = 0; i < nown + COUNT(strip_tmpl); i++) {
			const struct tmpl *t = i < nown ? &own[i] : &strip_tmpl[i - nown];

			snprintf(names[side][i], sizeof(names[side][i]), "%s %s",
				 side ? "Output" : "Input", t->ctl);
			for (ch = 0; ch < UCX_INPUTS; ch++)
				n = tmpl_add(n, t, names[side][i], side, ch, true);
		}
	}
	for (ch = 0; ch < UCX_OUTPUTS; ch++) {
		snprintf(mutes[ch], sizeof(mutes[ch]), "%s %s", output_names[ch], out_mute.ctl);
		n = tmpl_add(n, &out_mute, mutes[ch], true, ch, false);
	}
	for (ch = 0; ch < UCX_OUTPUTS; ch++) {
		snprintf(loopbacks[ch], sizeof(loopbacks[ch]), "%s %s", output_names[ch],
			 out_loopback.ctl);
		n = tmpl_add(n, &out_loopback, loopbacks[ch], true, ch, false);
		ucx_settings[n - 1].reg = out_loopback.reg + ch;
	}
	for (i = 0; i < COUNT(global_tmpl); i++)
		n = tmpl_add(n, &global_tmpl[i], global_tmpl[i].ctl, false, -1, false);
	if (n != UCX_SETTINGS)
		abort();
}

const char *ucx_output_name(int out)
{
	return output_names[out];
}

const char *ucx_input_name(int in)
{
	return input_names[in];
}

const char *ucx_source_name(int src)
{
	return src < UCX_SRC_INPUT ? pcm_names[src - UCX_SRC_PCM] : input_names[src - UCX_SRC_INPUT];
}

void ucx_pair_name(bool output, int pair, char *buf, size_t len)
{
	const char *const *names = output ? output_names : input_names;

	snprintf(buf, len, "%s/%s", names[2 * pair], strrchr(names[2 * pair + 1], ' ') + 1);
}

/* The unit's own state: every pair linked except the four preamp inputs */
static int link_default(bool output, int pair)
{
	return output || pair >= 2;
}

void ucx_setting_name(int setting, char *buf, size_t len)
{
	const struct ucx_setting *s = &ucx_settings[setting];

	if (s->ch < 0)
		snprintf(buf, len, "%s", s->key);
	else
		snprintf(buf, len, "%s %s %s", s->output ? "output" : "input",
			 s->output ? output_names[s->ch] : input_names[s->ch], s->key);
}

void ucx_setting_format(int setting, int value, char *buf, size_t len)
{
	const struct ucx_setting *s = &ucx_settings[setting];

	if (value == UCX_UNKNOWN) {
		snprintf(buf, len, "unknown");
		return;
	}
	switch (ucx_settings[setting].type) {
	case UCX_T_MIC_GAIN:
		snprintf(buf, len, "%d dB", value);
		break;
	case UCX_T_INST_GAIN:
		snprintf(buf, len, "%.1f dB", value / 2.0);
		break;
	case UCX_T_BOOL:
		snprintf(buf, len, "%s", !value == s->invert ? "on" : "off");
		break;
	case UCX_T_IN_LEVEL:
		snprintf(buf, len, "%s", in_levels[value]);
		break;
	case UCX_T_OUT_LEVEL:
		snprintf(buf, len, "%s", out_levels[value]);
		break;
	case UCX_T_NUM:
		snprintf(buf, len, "%g", (double)(value + s->off) * s->mul / s->div);
		break;
	case UCX_T_ENUM:
		snprintf(buf, len, "%s", value >= 0 && value < s->nitems ? s->items[value] : "?");
		break;
	}
}

void ucx_config_defaults(struct ucx_config *cfg)
{
	int out, src, i;

	for (out = 0; out < UCX_MIX_OUTS; out++) {
		if (out < UCX_OUTPUTS)
			cfg->volume[out] = UCX_VOL_MIN;
		for (src = 0; src < UCX_SOURCES; src++) {
			cfg->mix[out][src] = UCX_MIX_OFF;
			cfg->invert[out][src] = 0;
		}
	}
	for (i = 0; i < UCX_SETTINGS; i++)
		cfg->set[i] = ucx_settings[i].def;
	for (i = 0; i < UCX_PAIRS; i++) {
		cfg->link_out[i] = link_default(true, i);
		cfg->link_in[i] = link_default(false, i);
	}
}

bool ucx_config_has_unknown(const struct ucx_config *cfg)
{
	int out, src, i;

	for (out = 0; out < UCX_MIX_OUTS; out++) {
		if (out < UCX_OUTPUTS && cfg->volume[out] == UCX_UNKNOWN)
			return true;
		for (src = 0; src < UCX_SOURCES; src++)
			if (cfg->mix[out][src] == UCX_UNKNOWN)
				return true;
	}
	for (i = 0; i < UCX_SETTINGS; i++)
		if (cfg->set[i] == UCX_UNKNOWN)
			return true;
	for (i = 0; i < UCX_PAIRS; i++)
		if (cfg->link_out[i] == UCX_UNKNOWN || cfg->link_in[i] == UCX_UNKNOWN)
			return true;
	return false;
}

/*
 * The mix controls are linear with 32768 = 0 dB. The smallest gain, 1, is
 * -90.3 dB; a gain that is "on" in the file never becomes 0. The largest,
 * 65536 (TotalMix's top), is +6.02 dB: read back, both ends are clamped to
 * what a file can say (-90 to +6 dB), or a dump would not load. The plan
 * compares linear values, so this changes what is shown, not what is sent.
 */
int ucx_gain_to_linear(int centi_db)
{
	double linear;

	if (centi_db == UCX_MIX_OFF)
		return 0;
	linear = round(32768.0 * pow(10.0, centi_db / 2000.0));
	return linear < 1 ? 1 : linear > 65536 ? 65536 : (int)linear;
}

int ucx_gain_from_linear(int linear)
{
	int centi_db;

	if (linear <= 0)
		return UCX_MIX_OFF;
	centi_db = (int)round(2000.0 * log10(linear / 32768.0));
	if (centi_db < UCX_GAIN_MIN)
		return UCX_GAIN_MIN;
	return centi_db > UCX_GAIN_MAX ? UCX_GAIN_MAX : centi_db;
}

/* Channel names in the file */

enum chan_kind { CHAN_OUTPUT, CHAN_INPUT, CHAN_SOURCE };

static int chan_count(enum chan_kind kind)
{
	return kind == CHAN_SOURCE ? UCX_SOURCES : kind == CHAN_OUTPUT ? UCX_MIX_OUTS : UCX_INPUTS;
}

static const char *chan_name(enum chan_kind kind, int ch)
{
	return kind == CHAN_OUTPUT ? output_names[ch] :
	       kind == CHAN_INPUT ? input_names[ch] : ucx_source_name(ch);
}

/* "Phones L" and "Phones R" make "Phones L/R": @ch is the even channel of a pair */
static void pair_name(enum chan_kind kind, int ch, char *buf, size_t len)
{
	const char *right = chan_name(kind, ch + 1);

	snprintf(buf, len, "%s/%s", chan_name(kind, ch), strrchr(right, ' ') + 1);
}

/* A single channel or a pair: sets the first channel and the count (1 or 2) */
static bool chan_parse(enum chan_kind kind, const char *name, int *first, int *count)
{
	char pair[32];
	int ch;

	for (ch = 0; ch < chan_count(kind); ch++) {
		if (!strcmp(name, chan_name(kind, ch))) {
			*first = ch;
			*count = 1;
			return true;
		}
		if (ch % 2)
			continue;
		pair_name(kind, ch, pair, sizeof(pair));
		if (!strcmp(name, pair)) {
			*first = ch;
			*count = 2;
			return true;
		}
	}
	return false;
}

/* Parsing */

struct parser {
	const char *name;
	char *err;
	size_t errlen;
};

__attribute__((format(printf, 3, 4)))
static int fail(struct parser *p, const char *path, const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (*path)
		snprintf(p->err, p->errlen, "%s: %s: %s", p->name, path, msg);
	else
		snprintf(p->err, p->errlen, "%s: %s", p->name, msg);
	return -1;
}

/* A number that is a whole count of @step, within 1e-6 steps */
static bool to_steps(const json_t *json, double step, int *steps)
{
	double v, r;

	if (!json_is_number(json))
		return false;
	v = json_number_value(json) / step;
	r = round(v);
	if (!isfinite(v) || fabs(v - r) > 1e-6 || fabs(r) > 1e6)
		return false;
	*steps = (int)r;
	return true;
}

static int setting_find(bool output, int ch, const char *key)
{
	int i;

	for (i = 0; i < UCX_SETTINGS; i++)
		if (ucx_settings[i].output == output && ucx_settings[i].ch == ch &&
		    !strcmp(ucx_settings[i].key, key))
			return i;
	return -1;
}

static bool setting_key_exists(const char *key)
{
	int i;

	for (i = 0; i < UCX_SETTINGS; i++)
		if (ucx_settings[i].ch >= 0 && !strcmp(ucx_settings[i].key, key))
			return true;
	return false;
}

/* Is @key an object of settings ("eq", "eq.low") on this side? */
static bool setting_group_exists(bool output, bool global, const char *key)
{
	size_t len = strlen(key);
	int i;

	for (i = 0; i < UCX_SETTINGS; i++)
		if (ucx_settings[i].output == output && (ucx_settings[i].ch < 0) == global &&
		    !strncmp(ucx_settings[i].key, key, len) && ucx_settings[i].key[len] == '.')
			return true;
	return false;
}

static int level_parse(struct parser *p, const char *path, const json_t *json,
		       const char *const *items, int *value)
{
	int i;

	for (i = 0; json_is_string(json) && i < 3; i++) {
		if (!strcmp(json_string_value(json), items[i])) {
			*value = i;
			return 0;
		}
	}
	return fail(p, path, "must be \"%s\", \"%s\" or \"%s\"", items[0], items[1], items[2]);
}

static int setting_parse(struct parser *p, const char *path, int setting, const json_t *json,
			 int *value)
{
	const struct ucx_setting *s = &ucx_settings[setting];
	char list[256];
	int i, len;

	switch (s->type) {
	case UCX_T_MIC_GAIN:
		if (!to_steps(json, 1, value) || *value < 0 || *value > 65 ||
		    (*value > 0 && *value < 10))
			return fail(p, path, "must be 0 or a whole number of dB from 10 to 65");
		return 0;
	case UCX_T_INST_GAIN:
		if (!to_steps(json, 0.5, value) || *value < 0 || *value > 24)
			return fail(p, path, "must be 0 to 12 dB in steps of 0.5");
		return 0;
	case UCX_T_BOOL:
		if (!json_is_boolean(json))
			return fail(p, path, "must be true or false");
		*value = json_is_true(json) != s->invert;
		return 0;
	case UCX_T_IN_LEVEL:
		return level_parse(p, path, json, in_levels, value);
	case UCX_T_OUT_LEVEL:
		return level_parse(p, path, json, out_levels, value);
	case UCX_T_NUM:
		if (!to_steps(json, (double)s->mul / s->div, value) || *value - s->off < s->min ||
		    *value - s->off > s->max)
			return fail(p, path, "must be %g to %g in steps of %g",
				    (double)(s->min + s->off) * s->mul / s->div,
				    (double)(s->max + s->off) * s->mul / s->div,
				    (double)s->mul / s->div);
		*value -= s->off;
		return 0;
	case UCX_T_ENUM:
		for (i = 0, len = 0; i < s->nitems; i++) {
			if (json_is_string(json) && !strcmp(json_string_value(json), s->items[i])) {
				*value = i;
				return 0;
			}
			len += snprintf(list + len, sizeof(list) - len, "%s\"%s\"", i ? ", " : "",
					s->items[i]);
		}
		return fail(p, path, "must be one of %s", list);
	}
	return -1;
}

/*
 * One key of a channel object that should be a setting of @count channels
 * from @first, or an object of settings ("eq": {"mid": {"gain": 3}}).
 * @first < 0: a setting of the whole unit.
 */
static int channel_setting(struct parser *p, const char *path, bool output, int first, int count,
			   const char *key, const json_t *json, struct ucx_config *cfg)
{
	const char *side = output ? "output" : "input";
	const char *name;
	json_t *value;
	int ch, setting;

	if (json_is_object(json) && setting_group_exists(output, first < 0, key)) {
		char sub[96], subpath[224];

		json_object_foreach((json_t *)json, name, value) {
			snprintf(sub, sizeof(sub), "%s.%s", key, name);
			snprintf(subpath, sizeof(subpath), "%s.%s", path, name);
			if (channel_setting(p, subpath, output, first, count, sub, value, cfg))
				return -1;
		}
		return 0;
	}
	if (setting_group_exists(output, first < 0, key))
		return fail(p, path, "must be an object");
	for (ch = first; ch < first + count; ch++) {
		setting = ch < UCX_OUTPUTS ? setting_find(output, ch, key) : -1;
		if (setting < 0 && first >= 0 && !strcmp(key, "level") &&
		    (output ? ch < 8 : ch >= 4 && ch < 8))
			return fail(p, path, "%s %s has no \"level\" of its own: the unit has one "
				    "level for %s, \"hardware.%s\"", side,
				    chan_name(output ? CHAN_OUTPUT : CHAN_INPUT, ch),
				    !output ? "Line In 5-8" : ch < 6 ? "Line Out 1-6" : "Phones",
				    !output ? "line-in-level" : ch < 6 ? "line-out-level" :
				    "phones-level");
		if (setting < 0 && first >= 0 && setting_key_exists(key))
			return fail(p, path, "%s %s has no \"%s\" setting", side,
				    chan_name(output ? CHAN_OUTPUT : CHAN_INPUT, ch), key);
		if (setting < 0)
			return fail(p, path, "unknown key");
		if (setting_parse(p, path, setting, json, &cfg->set[setting]))
			return -1;
	}
	return 0;
}

/* Marks @count channels from @first as described; false if one already was */
static bool claim(bool *seen, int first, int count)
{
	bool fresh = true;
	int ch;

	for (ch = first; ch < first + count; ch++) {
		fresh = fresh && !seen[ch];
		seen[ch] = true;
	}
	return fresh;
}

static int parse_inputs(struct parser *p, const json_t *inputs, struct ucx_config *cfg)
{
	bool seen[UCX_INPUTS] = { false };
	char path[128], kpath[160];
	const char *name, *key;
	json_t *entry, *value;
	int first, count;

	if (!json_is_object(inputs))
		return fail(p, "inputs", "must be an object");
	json_object_foreach((json_t *)inputs, name, entry) {
		snprintf(path, sizeof(path), "inputs.\"%s\"", name);
		if (!chan_parse(CHAN_INPUT, name, &first, &count))
			return fail(p, path, "no such input or input pair");
		if (!claim(seen, first, count))
			return fail(p, path, "an input is described twice (as a pair and on its own)");
		cfg->link_in[first / 2] = count == 2;
		if (!json_is_object(entry))
			return fail(p, path, "must be an object");
		json_object_foreach(entry, key, value) {
			snprintf(kpath, sizeof(kpath), "%s.%s", path, key);
			if (channel_setting(p, kpath, false, first, count, key, value, cfg))
				return -1;
		}
	}
	return 0;
}

/*
 * "mix" of @ocount outputs from @out. Pair to pair is left to left and right
 * to right; every other combination connects each source to each output.
 */
static int parse_mix(struct parser *p, const char *opath, const json_t *mix, int out, int ocount,
		     struct ucx_config *cfg)
{
	bool seen[UCX_SOURCES] = { false };
	char path[160];
	const char *name;
	json_t *value, *gjson, *ijson;
	int src, scount, gain, invert, o, s;

	snprintf(path, sizeof(path), "%s.mix", opath);
	if (!json_is_object(mix))
		return fail(p, path, "must be an object of source: gain in dB");
	json_object_foreach((json_t *)mix, name, value) {
		snprintf(path, sizeof(path), "%s.mix.\"%s\"", opath, name);
		if (!chan_parse(CHAN_SOURCE, name, &src, &scount))
			return fail(p, path, "no such source (an input, \"PCM 1\"..\"PCM 18\", or a pair)");
		if (!claim(seen, src, scount))
			return fail(p, path, "a source is listed twice (as a pair and on its own)");
		/* A gain, or {"gain": dB, "invert": true} for a phase-inverted route */
		gjson = json_is_object(value) ? json_object_get(value, "gain") : value;
		ijson = json_is_object(value) ? json_object_get(value, "invert") : NULL;
		if (json_is_object(value) &&
		    (json_object_size(value) != (ijson ? 2 : 1) || (ijson && !json_is_boolean(ijson))))
			return fail(p, path, "an object here must be {\"gain\": dB, \"invert\": "
				    "true or false}");
		invert = ijson && json_is_true(ijson);
		if (!gjson || !to_steps(gjson, 0.01, &gain) || gain < UCX_GAIN_MIN ||
		    gain > UCX_GAIN_MAX)
			return fail(p, path,
				    "gain must be a number of dB from -90 to 6, at most 2 decimals "
				    "(leave the source out to turn it off)");
		for (o = 0; o < ocount; o++)
			for (s = 0; s < scount; s++)
				if (ocount < 2 || scount < 2 || o == s) {
					cfg->mix[out + o][src + s] = gain;
					cfg->invert[out + o][src + s] = invert;
				}
	}
	return 0;
}

static int parse_outputs(struct parser *p, const json_t *outputs, struct ucx_config *cfg)
{
	bool seen[UCX_MIX_OUTS] = { false };
	char path[128], kpath[160];
	const char *name, *key;
	json_t *entry, *value;
	int first, count, volume, o;
	bool fx;

	if (!json_is_object(outputs))
		return fail(p, "outputs", "must be an object");
	json_object_foreach((json_t *)outputs, name, entry) {
		snprintf(path, sizeof(path), "outputs.\"%s\"", name);
		if (!chan_parse(CHAN_OUTPUT, name, &first, &count))
			return fail(p, path, "no such output or output pair");
		if (!claim(seen, first, count))
			return fail(p, path, "an output is described twice (as a pair and on its own)");
		/* The FX send bus is two rows of the mixer: no volume, link or settings */
		fx = first >= UCX_OUTPUTS;
		if (!fx)
			cfg->link_out[first / 2] = count == 2;
		if (!json_is_object(entry))
			return fail(p, path, "must be an object");
		if (!fx && !json_object_get(entry, "volume"))
			return fail(p, path, "\"volume\" is required");
		json_object_foreach(entry, key, value) {
			snprintf(kpath, sizeof(kpath), "%s.%s", path, key);
			if (fx && strcmp(key, "mix"))
				return fail(p, kpath, "the FX send bus has only a \"mix\"");
			if (!strcmp(key, "volume")) {
				if (!to_steps(value, 0.1, &volume) || volume < UCX_VOL_MIN ||
				    volume > UCX_VOL_MAX)
					return fail(p, kpath,
						    "must be -65 to 6 dB in steps of 0.1");
				for (o = first; o < first + count; o++)
					cfg->volume[o] = volume;
			} else if (!strcmp(key, "mix")) {
				if (parse_mix(p, path, value, first, count, cfg))
					return -1;
			} else if (channel_setting(p, kpath, true, first, count, key, value, cfg)) {
				return -1;
			}
		}
	}
	return 0;
}

static int parse_root(struct parser *p, const json_t *root, struct ucx_config *cfg)
{
	const char *key;
	json_t *value;

	if (!json_is_object(root))
		return fail(p, "", "the top level must be an object");
	value = json_object_get(root, "version");
	if (!value)
		return fail(p, "", "\"version\" is required (this is version %d)", VERSION);
	if (!json_is_integer(value) || json_integer_value(value) != VERSION)
		return fail(p, "version", "must be %d", VERSION);

	ucx_config_defaults(cfg);
	json_object_foreach((json_t *)root, key, value) {
		if (!strcmp(key, "version"))
			continue;
		if (!strcmp(key, "inputs")) {
			if (parse_inputs(p, value, cfg))
				return -1;
		} else if (!strcmp(key, "outputs")) {
			if (parse_outputs(p, value, cfg))
				return -1;
		} else if (!strcmp(key, "unknown")) {
			return fail(p, key,
				    "this is a dump with parts the driver doesn't know; the listed "
				    "values are written as off here. Set them as you want them and "
				    "remove this key");
		} else if (channel_setting(p, key, false, -1, 1, key, value, cfg)) {
			return -1;
		}
	}
	return 0;
}

int ucx_config_parse(const char *text, size_t len, const char *name, struct ucx_config *cfg,
		     char *err, size_t errlen)
{
	struct parser p = { name, err, errlen };
	json_error_t jerr;
	json_t *root;
	int ret;

	/* Duplicate keys would silently drop one of the two values */
	root = json_loadb(text, len, JSON_REJECT_DUPLICATES, &jerr);
	if (!root) {
		snprintf(err, errlen, "%s:%d:%d: %s", name, jerr.line, jerr.column, jerr.text);
		return -1;
	}
	ret = parse_root(&p, root, cfg);
	json_decref(root);
	return ret;
}

int ucx_config_default_path(char *buf, size_t len)
{
	const char *base = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	int n;

	/* The XDG spec: a relative XDG_CONFIG_HOME is invalid and ignored */
	if (base && base[0] == '/')
		n = snprintf(buf, len, "%s/ucx/config.json", base);
	else if (home && home[0] == '/')
		n = snprintf(buf, len, "%s/.config/ucx/config.json", home);
	else
		return -1;
	return n < 0 || (size_t)n >= len ? -1 : 0;
}

int ucx_config_make_dir(const char *path, char *err, size_t errlen)
{
	char dir[4096], *p;

	if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir)) {
		snprintf(err, errlen, "%s: path too long", path);
		return -1;
	}
	p = strrchr(dir, '/');
	if (!p || p == dir)
		return 0;
	*p = '\0';
	for (p = dir + 1; ; p++) {
		char c = *p;

		if (c && c != '/')
			continue;
		*p = '\0';
		if (mkdir(dir, 0755) && errno != EEXIST) {
			snprintf(err, errlen, "%s: %s", dir, strerror(errno));
			return -1;
		}
		*p = c;
		if (!c)
			return 0;
	}
}

int ucx_config_load(const char *path, struct ucx_config *cfg, char *err, size_t errlen)
{
	char *text = NULL;
	size_t len = 0, cap = 0, got;
	FILE *f = strcmp(path, "-") ? fopen(path, "r") : stdin;
	int ret;

	if (!f) {
		snprintf(err, errlen, "%s: %s", path, strerror(errno));
		return -1;
	}
	do {
		if (cap - len < 4096) {
			char *grown = realloc(text, cap += 65536);

			if (!grown) {
				free(text);
				if (f != stdin)
					fclose(f);
				snprintf(err, errlen, "%s: out of memory", path);
				return -1;
			}
			text = grown;
		}
		got = fread(text + len, 1, cap - len, f);
		len += got;
	} while (got);
	ret = ferror(f);
	if (f != stdin)
		fclose(f);
	if (ret) {
		snprintf(err, errlen, "%s: read error", path);
		free(text);
		return -1;
	}
	ret = ucx_config_parse(text, len, path, cfg, err, errlen);
	free(text);
	return ret;
}

/* Writing */

/* dB from a fixed-point count: whole numbers as JSON integers, so 0 not 0.0 */
static json_t *db_json(int value, int per_db)
{
	if (value % per_db == 0)
		return json_integer(value / per_db);
	return json_real((double)value / per_db);
}

static json_t *setting_json(int setting, int value)
{
	const struct ucx_setting *s = &ucx_settings[setting];

	switch (s->type) {
	case UCX_T_NUM:
		return db_json((value + s->off) * s->mul, s->div);
	case UCX_T_ENUM:
		return json_string(s->items[value]);
	case UCX_T_MIC_GAIN:
		return json_integer(value);
	case UCX_T_INST_GAIN:
		return db_json(value, 2);
	case UCX_T_BOOL:
		return json_boolean(!value == s->invert);
	case UCX_T_IN_LEVEL:
		return json_string(in_levels[value]);
	case UCX_T_OUT_LEVEL:
		return json_string(out_levels[value]);
	}
	return NULL;
}

/* Set @obj's "a.b.c" to @value, making the objects on the way */
static void set_nested(json_t *obj, const char *key, json_t *value)
{
	const char *dot;
	char part[64];
	json_t *sub;

	while ((dot = strchr(key, '.'))) {
		snprintf(part, sizeof(part), "%.*s", (int)(dot - key), key);
		sub = json_object_get(obj, part);
		if (!sub) {
			sub = json_object();
			json_object_set_new(obj, part, sub);
		}
		obj = sub;
		key = dot + 1;
	}
	json_object_set_new(obj, key, value);
}

/*
 * The settings of one channel (@ch < 0: of the unit) that differ from the
 * defaults, or with @full all of them
 */
static void settings_json(const struct ucx_config *cfg, bool output, int ch, json_t *obj,
			  bool full)
{
	int i;

	for (i = 0; i < UCX_SETTINGS; i++)
		if (ucx_settings[i].output == output && ucx_settings[i].ch == ch &&
		    (full || cfg->set[i] != ucx_settings[i].def))
			set_nested(obj, ucx_settings[i].key, setting_json(i, cfg->set[i]));
}

static void set_chan(json_t *parent, enum chan_kind kind, int ch, bool pair, json_t *value)
{
	char name[32];

	if (pair)
		pair_name(kind, ch, name, sizeof(name));
	else
		snprintf(name, sizeof(name), "%s", chan_name(kind, ch));
	json_object_set_new(parent, name, value);
}

/* Do two routes have the same gain and phase? */
static bool route_same(const struct ucx_config *cfg, int o1, int s1, int o2, int s2)
{
	return cfg->mix[o1][s1] == cfg->mix[o2][s2] && cfg->invert[o1][s1] == cfg->invert[o2][s2];
}

static bool route_off(const struct ucx_config *cfg, int out, int src)
{
	return cfg->mix[out][src] == UCX_MIX_OFF;
}

static json_t *route_json(const struct ucx_config *cfg, int out, int src)
{
	json_t *gain = db_json(cfg->mix[out][src], 100);

	return cfg->invert[out][src] ? json_pack("{s:o, s:b}", "gain", gain, "invert", 1) : gain;
}

/* Is the source pair from @s wired straight (left to left, right to right) to @l and @r? */
static bool route_stereo(const struct ucx_config *cfg, int l, int r, int s)
{
	return route_same(cfg, l, s, r, s + 1) && route_off(cfg, l, s + 1) && route_off(cfg, r, s);
}

/* Can outputs @out and @out + 1 be written as one stereo entry? */
static bool mix_pairable(const struct ucx_config *cfg, int out)
{
	int s;

	for (s = 0; s < UCX_SOURCES; s += 2) {
		bool monos = route_same(cfg, out, s, out + 1, s) &&
			     route_same(cfg, out, s + 1, out + 1, s + 1);

		if (!route_stereo(cfg, out, out + 1, s) && !monos)
			return false;
	}
	return true;
}

/* Mix of a stereo entry (see mix_pairable()) or of one output */
static json_t *mix_json(const struct ucx_config *cfg, int out, bool pair)
{
	json_t *mix = json_object();
	int s, i;

	for (s = 0; s < UCX_SOURCES; s += 2) {
		bool as_pair = pair ? route_stereo(cfg, out, out + 1, s) :
				      route_same(cfg, out, s, out, s + 1);

		if (as_pair && !route_off(cfg, out, s)) {
			set_chan(mix, CHAN_SOURCE, s, true, route_json(cfg, out, s));
			continue;
		}
		for (i = s; i < s + 2; i++)
			if (!route_off(cfg, out, i))
				set_chan(mix, CHAN_SOURCE, i, false, route_json(cfg, out, i));
	}
	return mix;
}

static json_t *output_json(const struct ucx_config *cfg, int out, bool pair, bool full)
{
	json_t *obj = json_object(), *mix = mix_json(cfg, out, pair);

	if (out < UCX_OUTPUTS) {
		json_object_set_new(obj, "volume", db_json(cfg->volume[out], 10));
		settings_json(cfg, true, out, obj, full);
	}
	if (json_object_size(mix))
		json_object_set(obj, "mix", mix);
	json_decref(mix);
	return obj;
}

/* An output that needs no entry: it is what an unlisted output is */
static bool output_default(const struct ucx_config *cfg, int out)
{
	json_t *obj = output_json(cfg, out, false, false);
	bool def = out < UCX_OUTPUTS ? cfg->volume[out] == UCX_VOL_MIN && json_object_size(obj) == 1 :
				       !json_object_size(obj);

	json_decref(obj);
	return def;
}

/*
 * Replaces unknown values by the defaults and returns what was unknown as a
 * list of strings for the "unknown" key (NULL if nothing was)
 */
static json_t *take_unknown(struct ucx_config *cfg)
{
	json_t *list = json_array();
	char text[1024];
	int out, src, i, n, total = 0;
	size_t len;

	for (out = 0; out < UCX_OUTPUTS; out++) {
		if (cfg->volume[out] != UCX_UNKNOWN)
			continue;
		cfg->volume[out] = UCX_VOL_MIN;
		snprintf(text, sizeof(text), "%s volume", output_names[out]);
		json_array_append_new(list, json_string(text));
	}
	for (i = 0; i < UCX_SETTINGS; i++) {
		const struct ucx_setting *s = &ucx_settings[i];

		if (cfg->set[i] != UCX_UNKNOWN)
			continue;
		cfg->set[i] = s->def;
		ucx_setting_name(i, text, sizeof(text));
		json_array_append_new(list, json_string(text));
	}
	for (i = 0; i < 2 * UCX_PAIRS; i++) {
		bool output = i < UCX_PAIRS;
		int *link = output ? &cfg->link_out[i] : &cfg->link_in[i - UCX_PAIRS];
		char pair[32];

		if (*link != UCX_UNKNOWN)
			continue;
		*link = link_default(output, i % UCX_PAIRS);
		ucx_pair_name(output, i % UCX_PAIRS, pair, sizeof(pair));
		snprintf(text, sizeof(text), "%s %s stereo link", output ? "output" : "input", pair);
		json_array_append_new(list, json_string(text));
	}
	for (out = 0; out < UCX_MIX_OUTS; out++)
		for (src = 0; src < UCX_SOURCES; src++)
			total += cfg->mix[out][src] == UCX_UNKNOWN;
	if (total == UCX_MIX_OUTS * UCX_SOURCES)
		json_array_append_new(list, json_string("mix: every gain of every output"));
	for (out = 0; out < UCX_MIX_OUTS; out++) {
		len = snprintf(text, sizeof(text), "%s mix:", output_names[out]);
		for (src = 0, n = 0; src < UCX_SOURCES; src++) {
			if (cfg->mix[out][src] != UCX_UNKNOWN)
				continue;
			cfg->mix[out][src] = UCX_MIX_OFF;
			cfg->invert[out][src] = 0;
			len += snprintf(text + len, sizeof(text) - len, "%s %s", n++ ? "," : "",
					ucx_source_name(src));
		}
		if (n == UCX_SOURCES)
			snprintf(text, sizeof(text), "%s mix: every source", output_names[out]);
		if (n && total != UCX_MIX_OUTS * UCX_SOURCES)
			json_array_append_new(list, json_string(text));
	}
	if (!json_array_size(list)) {
		json_decref(list);
		return NULL;
	}
	return list;
}

char *ucx_config_format(const struct ucx_config *config, unsigned int flags)
{
	struct ucx_config cfg = *config;
	json_t *root = json_object(), *inputs = json_object(), *outputs = json_object();
	json_t *unknown = take_unknown(&cfg);
	bool full = flags & UCX_FORMAT_FULL;
	char *text;
	int ch;

	json_object_set_new(root, "version", json_integer(VERSION));
	if (unknown && !(flags & UCX_FORMAT_ASSUME_OFF))
		json_object_set(root, "unknown", unknown);
	json_decref(unknown);

	for (ch = 0; ch < UCX_INPUTS; ch += 2) {
		json_t *l = json_object(), *r = json_object();
		bool link, say;

		settings_json(&cfg, false, ch, l, full);
		settings_json(&cfg, false, ch + 1, r, full);
		link = cfg.link_in[ch / 2] && json_equal(l, r);
		/* An entry is needed for settings, and to say a link that isn't the default */
		say = full || link != link_default(false, ch / 2);
		if (link) {
			if (say || json_object_size(l))
				set_chan(inputs, CHAN_INPUT, ch, true, json_incref(l));
		} else {
			if (say || json_object_size(l))
				set_chan(inputs, CHAN_INPUT, ch, false, json_incref(l));
			if (say || json_object_size(r))
				set_chan(inputs, CHAN_INPUT, ch + 1, false, json_incref(r));
		}
		json_decref(l);
		json_decref(r);
	}

	for (ch = 0; ch < UCX_MIX_OUTS; ch += 2) {
		json_t *l = json_object(), *r = json_object();
		bool pair, fx = ch >= UCX_OUTPUTS;

		if (!fx) {
			settings_json(&cfg, true, ch, l, full);
			settings_json(&cfg, true, ch + 1, r, full);
		}
		pair = (fx || (cfg.link_out[ch / 2] && cfg.volume[ch] == cfg.volume[ch + 1])) &&
		       json_equal(l, r) && mix_pairable(&cfg, ch);
		json_decref(l);
		json_decref(r);
		if (fx && !pair) {
			/* No link to say: only the rows that have something */
			if (!output_default(&cfg, ch))
				set_chan(outputs, CHAN_OUTPUT, ch, false,
					 output_json(&cfg, ch, false, full));
			if (!output_default(&cfg, ch + 1))
				set_chan(outputs, CHAN_OUTPUT, ch + 1, false,
					 output_json(&cfg, ch + 1, false, full));
			continue;
		}
		if (pair) {
			if (full ? !fx || !output_default(&cfg, ch) :
				   !output_default(&cfg, ch) || !output_default(&cfg, ch + 1))
				set_chan(outputs, CHAN_OUTPUT, ch, true,
					 output_json(&cfg, ch, true, full));
			continue;
		}
		/* Unlinked is not the default, so both get an entry */
		set_chan(outputs, CHAN_OUTPUT, ch, false, output_json(&cfg, ch, false, full));
		set_chan(outputs, CHAN_OUTPUT, ch + 1, false,
			 output_json(&cfg, ch + 1, false, full));
	}

	if (json_object_size(inputs))
		json_object_set(root, "inputs", inputs);
	if (json_object_size(outputs))
		json_object_set(root, "outputs", outputs);
	json_decref(inputs);
	json_decref(outputs);
	/* Reverb, echo and the other settings of the unit, at the top level */
	settings_json(&cfg, false, -1, root, full);
	/* 6 significant digits print -12.34 as written, not -12.339999999999999 */
	text = json_dumps(root, JSON_INDENT(2) | JSON_REAL_PRECISION(6));
	json_decref(root);
	return text;
}
