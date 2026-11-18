// SPDX-License-Identifier: MIT
/*
 * ucxtui: edit a Fireface UCX config on screen and send it to the unit.
 *
 * An editor for the JSON config of ucxcfg (docs/CONFIG.md): what is on the
 * screen is a struct ucx_config, loaded from and saved to a file, and made
 * the unit's state with the same plan and apply code as `ucxcfg apply`.
 * Nothing reaches the unit until it is applied, or "live" is switched on,
 * which applies every edit as it is made.
 *
 * term.c is the terminal, doc.c the config and its edits, this file the
 * screens. docs/TUI.md is the manual.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "doc.h"
#include "term.h"
#include "ucxcfg.h"

/* Colours */
#define C_BG		0x14161b
#define C_BAR		0x1f232b
#define C_PANEL		0x1b1e25
#define C_TRACK		0x272c36
#define C_SEL		0x2c3340
#define C_SEL_DIM	0x1e222a
#define C_HOVER		0x242933
#define C_LINE		0x3a414e
#define C_FAINT		0x5c6675
#define C_DIM		0x8b95a5
#define C_TEXT		0xd5dbe5
#define C_BRIGHT	0xffffff
#define C_ACCENT	0x6cb6ff
#define C_GREEN		0x7ee787
#define C_YELLOW	0xe3b341
#define C_ORANGE	0xf0883e
#define C_RED		0xff7b72
#define C_INK		0x0d1117	/* text on a coloured field */

/* Glyphs */
#define G_KNOB		0x25cf	/* ● */
#define G_FILL		0x2501	/* ━ */
#define G_TRACK		0x2500	/* ─ */
#define G_MARK		0x258c	/* ▌ */
#define G_VLINE		0x2502	/* │ */
#define G_DOT		0x00b7	/* · */
#define G_SQUARE	0x25a0	/* ■ */
#define G_UPPER		0x2580	/* ▀ */

enum { TAB_INPUTS, TAB_OUTPUTS, TAB_ROUTING, TAB_EFFECTS, TAB_HARDWARE, TABS };
static const char *const tab_names[TABS] = { "Inputs", "Outputs", "Routing", "Effects", "Hardware" };

enum mode { M_NORMAL, M_EDIT, M_PROMPT, M_CONFIRM, M_APPLY, M_SELECT, M_HELP };
enum { MSG_INFO, MSG_OK, MSG_WARN, MSG_ERR };

/* What the keys of a setting are called on screen, and what they do */
struct meta {
	const char *key, *label, *unit, *help;
};

static const struct meta metas[] = {
	{ "gain", "Gain", "dB", "Preamp gain" },
	{ "phantom", "Phantom 48V", "", "48 V on the XLR input, for microphones that need it" },
	{ "hi-z", "Hi-Z", "", "High impedance, for a guitar or bass plugged straight in" },
	{ "autoset", "AutoSet", "", "The unit lowers the gain by itself on overload" },
	{ "level", "Ref level", "", "Reference level: the analog level that is full scale" },
	{ "fx-send", "FX send", "dB", "How much of this input goes to reverb and echo" },
	{ "fx-mute", "FX send mute", "", "Silences this input's FX send only" },
	{ "mute", "Mute", "", "Silences this output (while output mutes are enabled: Hardware)" },
	{ "balance", "Balance", "", "Left / right balance, after the volume" },
	{ "phase-invert", "Phase invert", "", "Inverts the polarity of this output" },
	{ "fx-return", "FX return", "dB", "Reverb and echo into this output" },
	{ "loopback", "Loopback", "", "The computer records this output (after its EQ, before "
	  "the volume) in place of the input of the same number" },
	{ "lowcut.on", "LOW CUT", "", "High-pass filter" },
	{ "lowcut.freq", "Frequency", "Hz", "Corner frequency of the low cut" },
	{ "lowcut.slope", "Slope", "dB/oct", "Steepness of the low cut" },
	{ "eq.on", "EQ", "", "Three-band parametric equaliser" },
	{ "eq.low.type", "Low type", "", "Shape of the low band" },
	{ "eq.low.gain", "Low gain", "dB", "" },
	{ "eq.low.freq", "Low freq", "Hz", "" },
	{ "eq.low.q", "Low Q", "", "Width of the band: higher is narrower" },
	{ "eq.mid.gain", "Mid gain", "dB", "" },
	{ "eq.mid.freq", "Mid freq", "Hz", "" },
	{ "eq.mid.q", "Mid Q", "", "Width of the band: higher is narrower" },
	{ "eq.high.type", "High type", "", "Shape of the high band" },
	{ "eq.high.gain", "High gain", "dB", "" },
	{ "eq.high.freq", "High freq", "Hz", "" },
	{ "eq.high.q", "High Q", "", "Width of the band: higher is narrower" },
	{ "dynamics.on", "DYNAMICS", "", "Compressor and expander" },
	{ "dynamics.gain", "Make-up gain", "dB", "Gain after the compressor" },
	{ "dynamics.attack", "Attack", "ms", "" },
	{ "dynamics.release", "Release", "ms", "" },
	{ "dynamics.comp-threshold", "Comp threshold", "dB", "The compressor works above this level" },
	{ "dynamics.comp-ratio", "Comp ratio", ":1", "1:1 is no compression" },
	{ "dynamics.exp-threshold", "Exp threshold", "dB", "The expander works below this level" },
	{ "dynamics.exp-ratio", "Exp ratio", ":1", "1:1 is no expansion" },
	{ "autolevel.on", "AUTO LEVEL", "", "Raises quiet signals by itself" },
	{ "autolevel.max-gain", "Max gain", "dB", "The most Auto Level adds" },
	{ "autolevel.headroom", "Headroom", "dB", "" },
	{ "autolevel.rise-time", "Rise time", "s", "How fast the gain comes back up" },
	{ "hardware.clock-source", "Clock source", "", "What the unit's sample clock follows. "
	  "A source without a signal can stop audio" },
	{ "hardware.clock-32000", "32k clock", "Hz", "Real rate of the 32 kHz family "
	  "(64 and 128 kHz are 2x and 4x)" },
	{ "hardware.clock-44100", "44.1k clock", "Hz", "Real rate of the 44.1 kHz family "
	  "(88.2 and 176.4 kHz are 2x and 4x)" },
	{ "hardware.clock-48000", "48k clock", "Hz", "Real rate of the 48 kHz family "
	  "(96 and 192 kHz are 2x and 4x)" },
	{ "hardware.optical-out", "Optical out", "", "ADAT or S/PDIF on the optical output" },
	{ "hardware.spdif-professional", "S/PDIF out pro", "", "Professional channel status on "
	  "the S/PDIF output instead of consumer" },
	{ "hardware.word-clock-single-speed", "WC single speed", "", "Word clock output stays at "
	  "single speed (32 to 48 kHz) at higher rates" },
	{ "hardware.word-clock-termination", "WC termination", "", "75 ohm termination of the "
	  "word clock input" },
	{ "hardware.eqd-record", "Record EQ+D", "", "The computer records inputs after their low "
	  "cut, EQ and dynamics" },
	{ "hardware.line-in-level", "Line In 5-8", "", "One reference level for all four rear "
	  "inputs" },
	{ "hardware.line-out-level", "Line Out 1-6", "", "One reference level for Line Out 1-6. "
	  "Hi Gain is 6 dB louder" },
	{ "hardware.phones-level", "Phones", "", "One reference level for Phones L and R. "
	  "Hi Gain is 6 dB louder" },
	{ "mute-enable", "Output mutes", "", "Off makes the unit ignore the mute of every output" },
	{ "reverb.on", "REVERB", "", "Fed by the FX sends, heard through each output's FX return" },
	{ "reverb.type", "Type", "", "" },
	{ "reverb.pre-delay", "Pre-delay", "ms", "" },
	{ "reverb.low-cut", "Low cut", "Hz", "" },
	{ "reverb.high-cut", "High cut", "Hz", "" },
	{ "reverb.room-scale", "Room scale", "", "" },
	{ "reverb.smooth", "Smooth", "", "" },
	{ "reverb.width", "Width", "", "0 is mono" },
	{ "reverb.volume", "Volume", "dB", "" },
	{ "reverb.attack", "Attack", "ms", "Measured to act on the Envelope type" },
	{ "reverb.hold", "Hold", "ms", "Goes with Attack; its effect has not been measured" },
	{ "reverb.release", "Release", "ms", "Goes with Attack; its effect has not been measured" },
	{ "reverb.time", "Reverb time", "s", "For the Space type" },
	{ "reverb.high-damp", "High damp", "Hz", "Measured to act on the Space type" },
	{ "echo.on", "ECHO", "", "Fed by the FX sends, heard through each output's FX return" },
	{ "echo.type", "Type", "", "" },
	{ "echo.delay", "Delay", "s", "" },
	{ "echo.feedback", "Feedback", "", "" },
	{ "echo.high-cut", "High cut", "", "" },
	{ "echo.width", "Width", "", "0 is mono" },
	{ "echo.volume", "Volume", "dB", "" },
};

/* A line of a form */
enum row_kind { R_GAP, R_HEAD, R_NOTE, R_SET, R_VOL, R_LINK, R_MIX };

struct row {
	enum row_kind kind;
	int col;		/* column of the form when it is wide enough for two */
	const char *label;
	char text[72];		/* R_NOTE: the value; R_MIX: the source's name */
	uint32_t colour;	/* R_NOTE */
	int setting;		/* R_SET; R_HEAD: the switch of the block, or -1 */
	int block;		/* the switch that this row belongs to, or -1 */
	struct mixrow mix;
};

/* What stays focused when the rows are built again */
struct rowid {
	enum row_kind kind;
	const char *key;
	int src;
};

struct view {
	int ch;			/* first channel of the strip the form shows */
	int list_top;
	bool in_list;
	int focus;
	int want;		/* the line that was chosen, see resolve() */
	struct rowid id;
	int pane_focus[2];	/* where the focus was in each column */
	int scroll[2];
};

struct pane {
	int x, y, w, h;
	int first, count;
};

/* The config, its file, and the device */
static struct doc doc;
static struct ucx_config saved;
static char path[PATH_MAX];
static bool from_device;
static const char *card = "UCX";

static struct ucx_dev *dev;
static struct ucx_dev_state dev_state;
static struct ucx_plan plan;		/* from dev_state to the config */
static struct ucx_meters meters;
static float shown[sizeof(struct ucx_meters) / sizeof(int)];
static struct ucx_status status;
static bool status_ok;
static unsigned int stream_rate;
static struct ucx_stream_info stream;
static bool live, live_dirty;
static long long last_open, last_meter, last_state, last_status;

/* What differs from the device: per value, and counted per place it shows */
static bool pend_vol[UCX_OUTPUTS], pend_mix[UCX_MIX_OUTS][UCX_SOURCES], pend_set[UCX_SETTINGS];
static bool pend_link[2][UCX_PAIRS];
static int pend_ch[2][UCX_MIX_OUTS], pend_tab[TABS];

/* The screen */
static int W, H;
static int tab;
static bool in_tabs;	/* the focus is on the row of pages, above the list and the form */
static struct view views[TABS];
static enum mode mode;
static bool quit;
static struct strip strips[UCX_MIX_OUTS], cur;
static int nstrips, sel;
static struct row rows[192];
static int nrows;
static struct pane panes[2], list_pane;
static int npanes;
static int tab_x[TABS + 1];
static int rt_out, rt_src, rt_top, rt_x, rt_y, rt_cw, rt_rows;
static int last_gain[UCX_MIX_OUTS][UCX_SOURCES];

static char message[400];
static int message_kind;
static long long message_time;

/* Text being typed: a value (M_EDIT) or a file name (M_PROMPT) */
static char input[PATH_MAX];
static int input_pos;
static enum { PR_SAVE, PR_OPEN } prompt_kind;
static bool quit_after_save;

/* M_CONFIRM */
enum confirm { CF_QUIT, CF_DANGER, CF_OPEN, CF_DEVICE, CF_PAGE };
static enum confirm confirm_kind;
static char confirm_title[80], confirm_text[3][200];
static const char *confirm_keys;	/* one letter per choice, in the order of confirm_labels */
static const char *confirm_labels[3];

/* M_APPLY */
struct apply_line {
	char text[160];
	bool danger;
};
static struct apply_line *apply_lines;
static int apply_n, apply_top;
static bool apply_live;

/* M_SELECT */
static int select_cur, select_top;
static struct pane select_box;

/* Where the mouse is, -1 while the keyboard is in use */
static int hover_x = -1, hover_y = -1;

/* A slider held with the mouse: it moves by as much as the mouse does */
static struct {
	bool on;
	int row, x0, v0, sw;
	double f0;
} drag;

/* The route a cell of the Routing page belongs to */
struct rcell {
	struct strip strip;
	struct mixrow row;
};

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

__attribute__((format(printf, 2, 3)))
static void say(int kind, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	message_kind = kind;
	message_time = now_ms();
}

__attribute__((format(printf, 7, 8)))
static int text(int x, int y, int maxw, uint32_t fg, uint32_t bg, int attr, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return maxw > 0 ? term_text(x, y, maxw, buf, fg, bg, attr) : 0;
}

/* Text that ends at @xr; returns where it starts */
__attribute__((format(printf, 6, 7)))
static int text_right(int xr, int y, uint32_t fg, uint32_t bg, int attr, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int w;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	w = term_width(buf);
	term_text(xr - w, y, w, buf, fg, bg, attr);
	return xr - w;
}

/* Settings: ranges, steps and text */

static const struct meta *meta_of(const char *key)
{
	unsigned int i;

	for (i = 0; i < sizeof(metas) / sizeof(*metas); i++)
		if (!strcmp(metas[i].key, key))
			return &metas[i];
	return NULL;
}

static const char *label_of(int setting)
{
	const struct meta *m = meta_of(ucx_settings[setting].key);

	return m ? m->label : ucx_settings[setting].key;
}

/* A switch as the file says it: "mute" is on when the control is off */
static bool is_on(int setting)
{
	return (doc.s.cfg.set[setting] != 0) != ucx_settings[setting].invert;
}

static bool is_number(int setting)
{
	enum ucx_setting_type t = ucx_settings[setting].type;

	return t == UCX_T_NUM || t == UCX_T_MIC_GAIN || t == UCX_T_INST_GAIN;
}

static void setting_range(int setting, int *min, int *max)
{
	const struct ucx_setting *s = &ucx_settings[setting];

	*min = 0;
	switch (s->type) {
	case UCX_T_MIC_GAIN:
		*max = 65;
		break;
	case UCX_T_INST_GAIN:
		*max = 24;
		break;
	case UCX_T_BOOL:
		*max = 1;
		break;
	case UCX_T_IN_LEVEL:
	case UCX_T_OUT_LEVEL:
		*max = 2;
		break;
	case UCX_T_ENUM:
		*max = s->nitems - 1;
		break;
	case UCX_T_NUM:
		*min = s->min;
		*max = s->max;
		break;
	}
}

/* Frequencies that span octaves move and show by ratio */
static bool is_log(const struct ucx_setting *s)
{
	return s->type == UCX_T_NUM && s->mul == 1 && s->div == 1 && s->min > 0 &&
	       s->max / s->min >= 20;
}

static void fmt_setting(int setting, int v, char *buf, size_t len)
{
	const struct ucx_setting *s = &ucx_settings[setting];
	const struct meta *m = meta_of(s->key);
	const char *unit = m ? m->unit : "";
	double x;

	if (v == UCX_UNKNOWN || s->type != UCX_T_NUM) {
		ucx_setting_format(setting, v, buf, len);
		return;
	}
	x = (double)(v + s->off) * s->mul / s->div;
	if (s->off == UCX_VOL_MIN && v == s->min)
		snprintf(buf, len, "off");
	else if (!strcmp(s->key, "balance"))
		snprintf(buf, len, v ? "%c %d" : "centre", v < 0 ? 'L' : 'R', abs(v));
	else if (!strcmp(unit, ":1"))
		snprintf(buf, len, "%.1f:1", x);
	else
		snprintf(buf, len, "%.*f%s%s", s->div == 100 ? 2 : s->div == 10 ? 1 : 0, x,
			 *unit ? " " : "", unit);
}

/* The largest of 1, 2, 5, 10, 20 ... that gives about 70 steps over @range */
static int nice_step(int range)
{
	static const int digits[] = { 1, 2, 5 };
	int best = 1, mag, i;

	for (mag = 1; mag <= range / 70; mag *= 10)
		for (i = 0; i < 3; i++)
			if (digits[i] * mag <= range / 70)
				best = digits[i] * mag;
	return best;
}

/* The next multiple of @step from @x in direction @dir */
static int snap(int x, int step, int dir)
{
	int r = ((x % step) + step) % step;

	if (dir > 0)
		return x - r + step;
	return r ? x - r : x - step;
}

static int step_setting(int setting, int v, int dir, bool fine)
{
	const struct ucx_setting *s = &ucx_settings[setting];
	int min, max, next;
	double f, mag;

	setting_range(setting, &min, &max);
	switch (s->type) {
	case UCX_T_BOOL:
		return (dir > 0) != s->invert;
	case UCX_T_MIC_GAIN:
		/* The preamp has no gain between 0 and 10 dB */
		v += dir;
		if (v > 0 && v < 10)
			v = dir > 0 ? 10 : 0;
		break;
	case UCX_T_NUM:
		if (is_log(s) && fine) {
			v += dir * (v >= 200 ? v / 100 : 1);
		} else if (is_log(s)) {
			/* A sixth of an octave, to two digits */
			f = v * pow(2.0, dir / 6.0);
			mag = pow(10.0, floor(log10(f)) - 1);
			next = (int)(round(f / mag) * mag);
			v = next == v ? v + dir : next;
		} else {
			next = !strcmp(s->key, "balance") ? 5 : nice_step(max - min);
			v = snap(v + s->off, fine ? 1 : next, dir) - s->off;
		}
		break;
	default:
		v += dir;
	}
	return clampi(v, min, max);
}

static double setting_frac(int setting, int v)
{
	const struct ucx_setting *s = &ucx_settings[setting];
	int min, max;

	setting_range(setting, &min, &max);
	if (is_log(s))
		return log((double)v / min) / log((double)max / min);
	return (double)(v - min) / (max - min);
}

/* Where the filled part of a slider starts: at zero for a value that goes both ways */
static double setting_origin(int setting)
{
	const struct ucx_setting *s = &ucx_settings[setting];

	if (s->type == UCX_T_NUM && s->min + s->off < 0 && s->max + s->off > 0 && s->def + s->off == 0)
		return setting_frac(setting, -s->off);
	return 0;
}

static void fmt_gain(int gain, char *buf, size_t len)
{
	if (gain == UCX_UNKNOWN)
		snprintf(buf, len, "unknown");
	else if (gain == UCX_MIX_OFF)
		snprintf(buf, len, "off");
	else if (gain % 10)
		snprintf(buf, len, "%.2f dB", gain / 100.0);
	else
		snprintf(buf, len, "%.1f dB", gain / 100.0);
}

/* 1 dB steps down to -40 dB, 5 dB below, then off; fine is 0.1 dB */
static int step_gain(int v, int dir, bool fine)
{
	if (v == UCX_MIX_OFF)
		return dir > 0 ? UCX_GAIN_MIN : UCX_MIX_OFF;
	if (v == UCX_GAIN_MIN && dir < 0)
		return UCX_MIX_OFF;
	v = snap(v, fine ? 10 : v + (dir > 0) > -4000 ? 100 : 500, dir);
	return clampi(v, UCX_GAIN_MIN, UCX_GAIN_MAX);
}

static void source_name(const struct mixrow *row, char *buf, size_t len)
{
	if (row->n == 1)
		snprintf(buf, len, "%s", ucx_source_name(row->src));
	else if (row->src < UCX_SRC_INPUT)
		snprintf(buf, len, "PCM %d/%d", row->src + 1, row->src + 2);
	else
		ucx_pair_name(false, (row->src - UCX_SRC_INPUT) / 2, buf, len);
}

/* The device */

static float level_of(const int *field)
{
	return dev ? shown[field - (const int *)&meters] : -1000;
}

static float strip_level(const struct strip *s, int side)
{
	int ch = s->ch + (side < s->n ? side : 0);

	if (!s->output)
		return level_of(&meters.input[ch]);
	return level_of(ch < UCX_OUTPUTS ? &meters.output[ch] : &meters.output_pre[ch]);
}

static float source_level(const struct mixrow *row)
{
	const int *m = row->src < UCX_SRC_INPUT ? &meters.playback[row->src] :
						  &meters.input_post[row->src - UCX_SRC_INPUT];
	float l = level_of(m), r = row->n == 2 ? level_of(m + 1) : l;

	return l > r ? l : r;
}

static int tab_of(const struct ucx_setting *s)
{
	if (s->ch >= 0)
		return s->output ? TAB_OUTPUTS : TAB_INPUTS;
	return !strncmp(s->key, "reverb.", 7) || !strncmp(s->key, "echo.", 5) ? TAB_EFFECTS :
									       TAB_HARDWARE;
}

/* The plan from the device's state to the config, and what it touches */
static void diff(void)
{
	unsigned int i;

	memset(pend_vol, 0, sizeof(pend_vol));
	memset(pend_mix, 0, sizeof(pend_mix));
	memset(pend_set, 0, sizeof(pend_set));
	memset(pend_link, 0, sizeof(pend_link));
	memset(pend_ch, 0, sizeof(pend_ch));
	memset(pend_tab, 0, sizeof(pend_tab));
	plan.n = 0;
	if (!dev)
		return;
	ucx_plan_make(&dev_state, &doc.s.cfg, &plan);
	for (i = 0; i < plan.n; i++) {
		const struct ucx_op *op = &plan.ops[i];
		const struct ucx_setting *s = &ucx_settings[op->setting];

		switch (op->kind) {
		case UCX_OP_VOLUME:
			pend_vol[op->out] = true;
			pend_ch[1][op->out]++;
			pend_tab[TAB_OUTPUTS]++;
			break;
		case UCX_OP_MIX:
		case UCX_OP_PUSH_MIX:
		case UCX_OP_INVERT:
			pend_mix[op->out][op->src] = true;
			pend_ch[1][op->out]++;
			pend_tab[TAB_OUTPUTS]++;
			pend_tab[TAB_ROUTING]++;
			break;
		case UCX_OP_SETTING:
			pend_set[op->setting] = true;
			if (s->ch >= 0)
				pend_ch[s->output][s->ch]++;
			pend_tab[tab_of(s)]++;
			break;
		case UCX_OP_LINK:
			pend_link[op->src][op->out] = true;
			pend_ch[!op->src][2 * op->out]++;
			pend_tab[op->src ? TAB_INPUTS : TAB_OUTPUTS]++;
			break;
		}
	}
}

static void device_lost(const char *why)
{
	ucx_dev_close(dev);
	dev = NULL;
	if (live)
		say(MSG_ERR, "Live is off: %s", why);
	else
		say(MSG_ERR, "%s", why);
	live = false;
	status_ok = false;
	diff();
}

/* Read what the driver holds; false if the device is gone */
static bool device_sync(void)
{
	char err[512];

	if (!dev)
		return false;
	last_state = now_ms();
	if (ucx_dev_read(dev, &dev_state, err, sizeof(err))) {
		device_lost(err);
		return false;
	}
	/*
	 * The status is four vendor requests to the unit, where everything else
	 * here is the driver's own record: once a second only where it is on
	 * screen in full
	 */
	if (last_state - last_status >= (tab == TAB_HARDWARE ? 1000 : 5000) || !last_status) {
		status_ok = !ucx_dev_status(dev, &status, err, sizeof(err));
		last_status = last_state;
	}
	stream_rate = ucx_dev_rate(dev);
	ucx_dev_stream(dev, &stream);
	diff();
	return true;
}

static bool device_open(void)
{
	char err[512];
	unsigned int i;

	last_open = now_ms();
	if (ucx_dev_open(card, &dev, err, sizeof(err))) {
		dev = NULL;
		return false;
	}
	for (i = 0; i < sizeof(shown) / sizeof(*shown); i++)
		shown[i] = -1000;
	last_status = 0;
	return device_sync();
}

/* Make the unit match the config. The screen is not drawn while this runs */
static bool device_apply(bool push)
{
	char err[512];
	unsigned int n;

	if (!device_sync())
		return false;
	n = plan.n;
	if (push)
		ucx_plan_make_push(&dev_state, &doc.s.cfg, &plan);
	if (ucx_dev_apply(dev, &plan, err, sizeof(err))) {
		/* The unit may hold part of the plan now: show what is left */
		if (live)
			say(MSG_ERR, "Live is off: %s", err);
		else
			say(MSG_ERR, "%s", err);
		live = false;
		device_sync();
		return false;
	}
	if (!device_sync())
		return false;
	if (plan.n) {
		ucx_op_describe(&plan.ops[0], err, sizeof(err));
		say(MSG_WARN, "%u values still differ after the apply, first: %s", plan.n, err);
	} else if (push) {
		say(MSG_OK, "Applied %u changes and wrote all %d mixer gains", n, UCX_MIX_OUTS * UCX_SOURCES);
	} else if (!live) {
		say(MSG_OK, "Applied %u change%s: the unit matches", n, n == 1 ? "" : "s");
	}
	return true;
}

/* A write that should not happen by a slip of the finger while live */
static bool dangerous(const struct ucx_op *op, char *why, size_t len)
{
	const struct ucx_setting *s = &ucx_settings[op->setting];

	if (op->kind != UCX_OP_SETTING)
		return false;
	if (!strcmp(s->key, "phantom") && op->new) {
		snprintf(why, len, "48 V phantom power goes on for %s.", ucx_input_name(s->ch));
		return true;
	}
	if (!strcmp(s->key, "hardware.clock-source")) {
		snprintf(why, len, "The clock source changes to %s.", s->items[op->new]);
		return true;
	}
	if (!strncmp(s->key, "hardware.clock-", 15) && strcmp(s->key, "hardware.clock-source")) {
		snprintf(why, len, "The real sample clock changes to %d Hz.", op->new);
		return true;
	}
	return false;
}

static void confirm_open(enum confirm kind, const char *title, const char *keys, const char *a,
			 const char *b, const char *c)
{
	confirm_kind = kind;
	snprintf(confirm_title, sizeof(confirm_title), "%s", title);
	confirm_keys = keys;
	confirm_labels[0] = a;
	confirm_labels[1] = b;
	confirm_labels[2] = c;
	mode = M_CONFIRM;
}

static void live_apply(void)
{
	unsigned int i;

	live_dirty = false;
	if (!device_sync())
		return;
	for (i = 0; i < plan.n; i++) {
		if (!dangerous(&plan.ops[i], confirm_text[0], sizeof(confirm_text[0])))
			continue;
		snprintf(confirm_text[1], sizeof(confirm_text[1]),
			 "Live is on, so this reaches the unit at once.");
		confirm_text[2][0] = 0;
		confirm_open(CF_DANGER, "Send this to the unit?", "yn", "Yes, send it",
			     "No, undo the change", NULL);
		return;
	}
	if (plan.n)
		device_apply(false);
}

/* The config changed */
static void changed(void)
{
	diff();
	if (live)
		live_dirty = true;
}

static void device_tick(void)
{
	long long now = now_ms();
	struct ucx_meters m;
	char err[512];
	unsigned int i;
	float v, fall;

	if (!dev) {
		if (now - last_open >= 2000 && device_open())
			say(MSG_OK, "Connected to the unit on card %s", card);
		return;
	}
	if (now - last_meter >= 60) {
		if (ucx_dev_meters(dev, &m, err, sizeof(err))) {
			device_lost(err);
			return;
		}
		/* Each read is the peak since the last one: let the display fall at 40 dB/s */
		fall = last_meter ? (now - last_meter) * 0.04f : 0;
		for (i = 0; i < sizeof(shown) / sizeof(*shown); i++) {
			v = ((const int *)&m)[i] == UCX_METER_NONE ? -1000 :
				((const int *)&m)[i] / 10.0f;
			shown[i] = v > shown[i] - fall ? v : shown[i] - fall;
		}
		last_meter = now;
	}
	if (now - last_state >= 1000)
		device_sync();
}

/* Files */

static bool modified(void)
{
	return memcmp(&doc.s.cfg, &saved, sizeof(saved)) != 0;
}

static void doc_replace(const struct ucx_config *cfg)
{
	int t;

	doc_load(&doc, cfg);
	saved = doc.s.cfg;
	for (t = 0; t < TABS; t++)
		views[t] = (struct view){ .in_list = t == TAB_INPUTS || t == TAB_OUTPUTS, .want = 1,
					  .pane_focus = { 1, 0 } };
	memset(last_gain, 0, sizeof(last_gain));
	in_tabs = false;
	/* A new config is looked at before it is sent */
	live = false;
	diff();
}

static bool file_open(const char *name)
{
	struct ucx_config cfg;
	char err[512];

	if (ucx_config_load(name, &cfg, err, sizeof(err))) {
		say(MSG_ERR, "%s", err);
		return false;
	}
	doc_replace(&cfg);
	snprintf(path, sizeof(path), "%s", name);
	from_device = false;
	say(MSG_OK, "Opened %s", name);
	return true;
}

/* The config from what the driver knows of the unit */
static bool device_load(void)
{
	int unknown;

	if (!device_sync()) {
		say(MSG_WARN, "No unit on card %s: is it on and snd-fireface-ucx loaded?", card);
		return false;
	}
	unknown = doc_load(&doc, &dev_state.cfg);
	doc_replace(&doc.s.cfg);
	from_device = true;
	if (unknown)
		say(MSG_WARN, "The driver doesn't know %d values of the unit (mixer gains, mostly). "
		    "They show as off here, and applying writes them", unknown);
	else
		say(MSG_OK, "Read the unit's state");
	return true;
}

static bool file_save(const char *name)
{
	char tmp[PATH_MAX + 8], err[512], *json;
	struct ucx_config back;
	bool same, ok;
	FILE *f;
	int why;

	json = ucx_config_format(&doc.s.cfg, 0);
	if (!json) {
		say(MSG_ERR, "Out of memory");
		return false;
	}
	/* What was written must load as what is on the screen */
	same = !ucx_config_parse(json, strlen(json), name, &back, err, sizeof(err)) &&
	       !memcmp(&back, &doc.s.cfg, sizeof(back));
	/* ~/.config/ucx may not exist yet */
	if (ucx_config_make_dir(name, err, sizeof(err))) {
		say(MSG_ERR, "%s", err);
		free(json);
		return false;
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", name);
	/* Closed whatever happened, and no NAME.tmp left behind; the first error is the one shown */
	f = fopen(tmp, "w");
	ok = f && fprintf(f, "%s\n", json) >= 0;
	why = errno;
	if (f && fclose(f) && ok) {
		ok = false;
		why = errno;
	}
	if (ok && rename(tmp, name)) {
		ok = false;
		why = errno;
	}
	if (!ok) {
		unlink(tmp);
		say(MSG_ERR, "%s: %s", name, strerror(why));
		free(json);
		return false;
	}
	free(json);
	if (name != path)
		snprintf(path, sizeof(path), "%s", name);
	saved = doc.s.cfg;
	from_device = false;
	if (same)
		say(MSG_OK, "Saved %s", path);
	else
		say(MSG_WARN, "Saved %s, but it does not load back as what is shown: a bug in "
		    "ucxtui, please report it", path);
	return true;
}

/* The forms */

static struct row *add(enum row_kind kind, int col, const char *label)
{
	struct row *r = &rows[nrows++];

	*r = (struct row){ .kind = kind, .col = col, .label = label, .setting = -1, .block = -1,
			   .colour = C_TEXT };
	return r;
}

static void add_set(int col, int setting, int block)
{
	struct row *r;

	if (setting < 0)
		return;
	r = add(R_SET, col, label_of(setting));
	r->setting = setting;
	r->block = block;
}

__attribute__((format(printf, 3, 4)))
static struct row *add_note(int col, const char *label, const char *fmt, ...)
{
	struct row *r = add(R_NOTE, col, label);
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(r->text, sizeof(r->text), fmt, ap);
	va_end(ap);
	return r;
}

/* A section with a switch: "eq" gives the head EQ with eq.on, then eq.* in the order of @keys */
static void add_block(int col, bool output, int ch, const char *name, const char *const *keys)
{
	char key[64];
	int on, i;

	snprintf(key, sizeof(key), "%s.on", name);
	on = doc_setting(output, ch, key);
	if (nrows)
		add(R_GAP, col, NULL);
	add(R_HEAD, col, label_of(on))->setting = on;
	for (i = 0; keys[i]; i++) {
		snprintf(key, sizeof(key), "%s.%s", name, keys[i]);
		add_set(col, doc_setting(output, ch, key), on);
	}
}

/*
 * Whatever the library has for this channel (@ch < 0: for the unit, with keys
 * that start with @prefix) and the form doesn't show yet. A setting added to
 * ucx_settings[] is then on screen at once, under its key, until it is given
 * a label in metas[] and a place in its form.
 */
static void add_missing(int col, bool output, int ch, const char *prefix)
{
	const struct ucx_setting *s;
	bool head = false;
	int i, j;

	for (i = 0; i < UCX_SETTINGS; i++) {
		s = &ucx_settings[i];
		if (s->output != output || s->ch != ch || strncmp(s->key, prefix, strlen(prefix)))
			continue;
		/* Without a prefix: all of the unit's that are not an effect's */
		if (ch < 0 && !*prefix && tab_of(s) != TAB_HARDWARE)
			continue;
		for (j = 0; j < nrows && rows[j].setting != i; j++)
			;
		if (j < nrows)
			continue;
		if (!head) {
			add(R_GAP, col, NULL);
			add(R_HEAD, col, "MORE");
			head = true;
		}
		add_set(col, i, -1);
	}
}

static const char *const lowcut_keys[] = { "freq", "slope", NULL };
static const char *const eq_keys[] = { "low.type", "low.gain", "low.freq", "low.q", "mid.gain",
				       "mid.freq", "mid.q", "high.type", "high.gain", "high.freq",
				       "high.q", NULL };
static const char *const dynamics_keys[] = { "gain", "attack", "release", "comp-threshold",
					     "comp-ratio", "exp-threshold", "exp-ratio", NULL };
static const char *const autolevel_keys[] = { "max-gain", "headroom", "rise-time", NULL };

static void build_input(void)
{
	static const char *const keys[] = { "gain", "phantom", "hi-z", "autoset", "level" };
	unsigned int i;

	add(R_HEAD, 0, "INPUT");
	for (i = 0; i < sizeof(keys) / sizeof(*keys); i++)
		add_set(0, doc_setting(false, cur.ch, keys[i]), -1);
	if (cur.ch >= 4 && cur.ch < 8)
		add_set(0, doc_global("hardware.line-in-level"), -1);
	add_set(0, doc_setting(false, cur.ch, "fx-send"), -1);
	add_set(0, doc_setting(false, cur.ch, "fx-mute"), -1);
	add(R_LINK, 0, "Stereo link");
	add_block(0, false, cur.ch, "lowcut", lowcut_keys);
	add_block(0, false, cur.ch, "autolevel", autolevel_keys);
	add_block(1, false, cur.ch, "eq", eq_keys);
	add_block(1, false, cur.ch, "dynamics", dynamics_keys);
	add_missing(1, false, cur.ch, "");
}

static void build_mix(int col)
{
	struct mixrow mix[UCX_SOURCES];
	int n = doc_mix_rows(&doc, &cur, mix), i;
	struct row *r;

	for (i = 0; i < n; i++) {
		if (!i || (mix[i].src >= UCX_SRC_INPUT && mix[i - 1].src < UCX_SRC_INPUT)) {
			if (nrows)
				add(R_GAP, col, NULL);
			add(R_HEAD, col, i ? "MIX: INPUTS" : "MIX: PLAYBACK");
		}
		r = add(R_MIX, col, NULL);
		r->mix = mix[i];
		source_name(&mix[i], r->text, sizeof(r->text));
		r->label = r->text;
	}
}

static void build_output(void)
{
	static const char *const keys[] = { "mute", "balance", "phase-invert", "fx-return",
					    "loopback", "level" };
	unsigned int i;

	if (cur.ch >= UCX_OUTPUTS) {
		add(R_HEAD, 0, "FX SEND BUS");
		add_note(0, "", "What reverb and echo are fed: playback here,");
		add_note(0, "", "inputs with their own FX send.");
		add(R_LINK, 0, "Stereo link");
		build_mix(0);
		return;
	}
	add(R_HEAD, 0, "OUTPUT");
	add(R_VOL, 0, "Volume");
	for (i = 0; i < sizeof(keys) / sizeof(*keys); i++)
		add_set(0, doc_setting(true, cur.ch, keys[i]), -1);
	if (cur.ch < 6)
		add_set(0, doc_global("hardware.line-out-level"), -1);
	else if (cur.ch < 8)
		add_set(0, doc_global("hardware.phones-level"), -1);
	add(R_LINK, 0, "Stereo link");
	build_mix(0);
	add_block(1, true, cur.ch, "lowcut", lowcut_keys);
	add_block(1, true, cur.ch, "eq", eq_keys);
	add_block(1, true, cur.ch, "dynamics", dynamics_keys);
	add_block(1, true, cur.ch, "autolevel", autolevel_keys);
	add_missing(1, true, cur.ch, "");
}

static void build_effects(void)
{
	static const char *const reverb[] = { "type", "volume", "pre-delay", "low-cut", "high-cut",
					      "room-scale", "smooth", "width", "attack", "hold",
					      "release", "time", "high-damp", NULL };
	static const char *const echo[] = { "type", "volume", "delay", "feedback", "high-cut",
					    "width", NULL };

	add_block(0, false, -1, "reverb", reverb);
	add_missing(0, false, -1, "reverb.");
	add_block(1, false, -1, "echo", echo);
	add_missing(1, false, -1, "echo.");
}

/*
 * The streams: set by whoever opened them (PipeWire's quantum and rate, a
 * JACK or ALSA program's buffer), not by the unit, so shown and not edited
 */
static void build_stream(void)
{
	static const char *const names[] = { "Playback", "Capture" };
	unsigned int rate = 0, frames, i;

	add(R_HEAD, 1, "STREAM");
	if (stream.errors >= 0)
		add_note(1, "Errors", "%ld unit, %ld restarts", stream.errors,
			 stream.restarts)->colour = stream.errors || stream.restarts ? C_YELLOW : C_TEXT;
	for (i = 0; i < 2; i++) {
		if (!stream.dir[i].open) {
			add_note(1, names[i], "not open")->colour = C_DIM;
			continue;
		}
		rate = stream.dir[i].rate;
		add_note(1, names[i], "%u ch, period %u, buffer %u", stream.dir[i].channels,
			 stream.dir[i].period, stream.dir[i].buffer);
	}
	if (stream.urb_blocks < 0) {
		add_note(1, "urb_blocks", "-")->colour = C_FAINT;
		return;
	}
	add_note(1, "urb_blocks", "%d", stream.urb_blocks);
	if (!rate)
		return;
	frames = ucx_round_trip(rate, stream.urb_blocks);
	add_note(1, "Round trip", "%u frames, %.2f ms", frames, frames * 1000.0 / rate);
}

static void sync_note(const char *label, int input)
{
	uint32_t colour = C_DIM;
	char rate[32] = "";
	const char *state = "no signal";

	if (!status_ok) {
		add_note(1, label, "-")->colour = C_FAINT;
		return;
	}
	if (status.in[input].rate)
		snprintf(rate, sizeof(rate), ", %g kHz", status.in[input].rate / 1000.0);
	if (status.in[input].sync) {
		state = "in sync";
		colour = C_GREEN;
	} else if (status.in[input].lock) {
		state = "locked, not in sync";
		colour = C_YELLOW;
	}
	add_note(1, label, "%s%s", state, rate)->colour = colour;
}

static void build_hardware(void)
{
	static const char *const sources[] = { "internal", "S/PDIF", "optical", "word clock" };
	int out, src, known = 0;

	add(R_HEAD, 0, "CLOCK");
	add_set(0, doc_global("hardware.clock-source"), -1);
	add_set(0, doc_global("hardware.word-clock-single-speed"), -1);
	add_set(0, doc_global("hardware.word-clock-termination"), -1);
	add_set(0, doc_global("hardware.clock-32000"), -1);
	add_set(0, doc_global("hardware.clock-44100"), -1);
	add_set(0, doc_global("hardware.clock-48000"), -1);
	add(R_GAP, 0, NULL);
	add(R_HEAD, 0, "DIGITAL OUTPUTS");
	add_set(0, doc_global("hardware.optical-out"), -1);
	add_set(0, doc_global("hardware.spdif-professional"), -1);
	add(R_GAP, 0, NULL);
	add(R_HEAD, 0, "ANALOG LEVELS");
	add_set(0, doc_global("hardware.line-in-level"), -1);
	add_set(0, doc_global("hardware.line-out-level"), -1);
	add_set(0, doc_global("hardware.phones-level"), -1);
	add(R_GAP, 0, NULL);
	add(R_HEAD, 0, "OPTIONS");
	add_set(0, doc_global("hardware.eqd-record"), -1);
	add_set(0, doc_global("mute-enable"), -1);
	add_missing(0, false, -1, "");

	add(R_HEAD, 1, "UNIT");
	if (!dev) {
		add_note(1, "Card", "%s: not found", card)->colour = C_YELLOW;
		add_note(1, "", "Is the unit on and snd-fireface-ucx")->colour = C_DIM;
		add_note(1, "", "loaded? Looking again every 2 s.")->colour = C_DIM;
		return;
	}
	add_note(1, "Card", "%s", card);
	if (status_ok && status.rate)
		add_note(1, "Sample rate", "%g kHz%s", status.rate / 1000.0,
			 stream_rate ? "" : " (no stream open)");
	else
		add_note(1, "Sample rate", stream_rate ? "%g kHz" : "-", stream_rate / 1000.0);
	add_note(1, "Clock now", "%s", status_ok && status.clock >= 0 ? sources[status.clock] : "-");
	sync_note("S/PDIF in", UCX_SYNC_SPDIF);
	sync_note("Optical in", UCX_SYNC_OPTICAL);
	sync_note("Word clock in", UCX_SYNC_WORD);
	add(R_GAP, 1, NULL);
	build_stream();
	add(R_GAP, 1, NULL);
	add(R_HEAD, 1, "WHAT THE DRIVER KNOWS");
	for (out = 0; out < UCX_MIX_OUTS; out++)
		for (src = 0; src < UCX_SOURCES; src++)
			known += dev_state.linear[out][src] >= 0;
	add_note(1, "Mixer gains", "%d of %d", known, UCX_MIX_OUTS * UCX_SOURCES)->colour =
		known == UCX_MIX_OUTS * UCX_SOURCES ? C_TEXT : C_YELLOW;
	add_note(1, "To apply", plan.n ? "%u values" : "nothing: the unit matches", plan.n)->colour =
		plan.n ? C_YELLOW : C_GREEN;
	if (known < UCX_MIX_OUTS * UCX_SOURCES) {
		add_note(1, "", "The unit never reports its mixer. Apply")->colour = C_DIM;
		add_note(1, "", "writes the gains the driver hasn't seen.")->colour = C_DIM;
	}
}

/* A line with a value of its own: all but the headings without a switch */
static bool has_value(const struct row *r)
{
	return r->kind >= R_SET || (r->kind == R_HEAD && r->setting >= 0);
}

/*
 * A heading can have the focus when there is something under it to put back
 * at its defaults (d), so not the ones over notes
 */
static bool focusable(const struct row *r)
{
	const struct row *below;

	if (r->kind != R_HEAD)
		return r->kind >= R_SET;
	for (below = r + 1; below < rows + nrows && below->kind != R_HEAD; below++)
		if (below->kind >= R_SET)
			return true;
	return r->setting >= 0;
}

/* A heading without a switch is known by its text */
static struct rowid id_of(const struct row *r)
{
	return (struct rowid){ r->kind, r->setting >= 0 ? ucx_settings[r->setting].key :
			       r->kind == R_HEAD ? r->label : NULL, r->mix.src };
}

static bool id_matches(const struct row *r, const struct rowid *id)
{
	if (r->kind != id->kind || !focusable(r))
		return false;
	if (r->kind == R_MIX)
		return id->src >= r->mix.src && id->src < r->mix.src + r->mix.n;
	if (r->setting >= 0)
		return id->key && !strcmp(ucx_settings[r->setting].key, id->key);
	if (r->kind == R_HEAD)
		return id->key && !strcmp(r->label, id->key);
	return true;
}

static void set_focus(int i)
{
	struct view *v = &views[tab];

	v->focus = v->want = i;
	v->id = id_of(&rows[i]);
	v->pane_focus[rows[i].col] = i;
}

static bool has_list(void)
{
	return tab == TAB_INPUTS || tab == TAB_OUTPUTS;
}

/* The strips, the rows and the focus for the config as it is now */
static void resolve(void)
{
	struct view *v = &views[tab];
	int i, d;

	nrows = 0;
	nstrips = 0;
	if (has_list()) {
		nstrips = doc_strips(&doc, tab == TAB_OUTPUTS, strips);
		for (sel = 0; sel < nstrips - 1; sel++)
			if (v->ch < strips[sel].ch + strips[sel].n)
				break;
		cur = strips[sel];
		v->ch = cur.ch;
		if (tab == TAB_INPUTS)
			build_input();
		else
			build_output();
	} else if (tab == TAB_EFFECTS) {
		build_effects();
	} else if (tab == TAB_HARDWARE) {
		build_hardware();
	}
	if (!has_list())
		v->in_list = false;
	if (!nrows)
		return;
	/*
	 * The focus follows what was last chosen, not where it had to land: a
	 * line that this channel doesn't have is found again on the next one
	 */
	for (i = 0; i < nrows; i++) {
		if (id_matches(&rows[i], &v->id)) {
			v->focus = i;
			return;
		}
	}
	v->focus = clampi(v->want, 0, nrows - 1);
	for (d = 0; d < nrows; d++) {
		for (i = v->focus - d; i <= v->focus + d; i += d ? 2 * d : 1) {
			if (i >= 0 && i < nrows && focusable(&rows[i])) {
				v->focus = i;
				return;
			}
		}
	}
}

/* Values of rows */

static int row_value(const struct row *r)
{
	switch (r->kind) {
	case R_HEAD:
	case R_SET:
		/* A heading without a switch has no value */
		return r->setting >= 0 ? doc.s.cfg.set[r->setting] : 0;
	case R_VOL:
		return doc.s.cfg.volume[cur.ch];
	case R_LINK:
		return doc_linked(&doc, cur.output, cur.ch / 2);
	case R_MIX:
		return doc_mix_get(&doc, &cur, &r->mix, NULL);
	default:
		return 0;
	}
}

static void fmt_row(const struct row *r, int v, char *buf, size_t len)
{
	switch (r->kind) {
	case R_HEAD:
	case R_SET:
		fmt_setting(r->setting, v, buf, len);
		break;
	case R_VOL:
		if (v == UCX_UNKNOWN)
			snprintf(buf, len, "unknown");
		else
			snprintf(buf, len, "%.1f dB", v / 10.0);
		break;
	case R_LINK:
		snprintf(buf, len, "%s", v == UCX_UNKNOWN ? "unknown" : v ? "on" : "off");
		break;
	case R_MIX:
		fmt_gain(v, buf, len);
		break;
	default:
		buf[0] = 0;
	}
}

static bool row_is_number(const struct row *r)
{
	return r->kind == R_VOL || r->kind == R_MIX || (r->kind == R_SET && is_number(r->setting));
}

static bool row_is_switch(const struct row *r)
{
	return r->kind == R_LINK || (r->kind == R_HEAD && r->setting >= 0) ||
	       (r->kind == R_SET && ucx_settings[r->setting].type == UCX_T_BOOL);
}

static bool row_on(const struct row *r)
{
	return r->kind == R_LINK ? row_value(r) : is_on(r->setting);
}

static bool row_pending(const struct row *r)
{
	const struct ucx_setting *s;
	int o, i;

	switch (r->kind) {
	case R_HEAD:
	case R_SET:
		if (r->setting < 0)
			return false;
		s = &ucx_settings[r->setting];
		return pend_set[r->setting] ||
		       (s->ch >= 0 && cur.n == 2 &&
			pend_set[doc_setting(s->output, s->ch ^ 1, s->key)]);
	case R_VOL:
		return pend_vol[cur.ch] || (cur.n == 2 && pend_vol[cur.ch + 1]);
	case R_LINK:
		return cur.ch < UCX_OUTPUTS && pend_link[!cur.output][cur.ch / 2];
	case R_MIX:
		/* A stereo route is four gains: two on, two off */
		for (o = cur.ch; o < cur.ch + cur.n; o++)
			for (i = r->mix.src; i < r->mix.src + r->mix.n; i++)
				if (pend_mix[o][i])
					return true;
		return false;
	default:
		return false;
	}
}

/* What the driver holds for this row, as text */
static void row_device(const struct row *r, char *buf, size_t len)
{
	const struct ucx_config *d = &dev_state.cfg;
	int v;

	buf[0] = 0;
	if (!has_value(r))
		return;
	switch (r->kind) {
	case R_HEAD:
	case R_SET:
		v = d->set[r->setting];
		break;
	case R_VOL:
		v = d->volume[cur.ch];
		break;
	case R_LINK:
		v = cur.ch >= UCX_OUTPUTS ? UCX_UNKNOWN :
		    cur.output ? d->link_out[cur.ch / 2] : d->link_in[cur.ch / 2];
		break;
	case R_MIX:
		v = d->mix[cur.ch][r->mix.src];
		break;
	default:
		buf[0] = 0;
		return;
	}
	fmt_row(r, v, buf, len);
}

static long row_tag(const struct row *r)
{
	switch (r->kind) {
	case R_SET:
		return 1000 + r->setting;
	case R_VOL:
		return 100 + cur.ch;
	case R_MIX:
		return 100000 + cur.ch * 100 + r->mix.src;
	default:
		return 0;
	}
}

static void row_set(const struct row *r, int v)
{
	int invert;

	if (v == row_value(r))
		return;
	doc_checkpoint(&doc, row_tag(r));
	switch (r->kind) {
	case R_HEAD:
	case R_SET:
		doc_set(&doc, r->setting, v);
		break;
	case R_VOL:
		doc_set_volume(&doc, cur.ch, v);
		break;
	case R_LINK:
		doc_set_link(&doc, cur.output, cur.ch / 2, v);
		break;
	case R_MIX:
		doc_mix_get(&doc, &cur, &r->mix, &invert);
		doc_mix_set(&doc, &cur, &r->mix, v, invert);
		break;
	default:
		break;
	}
	changed();
}

static void row_toggle(const struct row *r)
{
	int v = row_value(r), *last;

	if (r->kind == R_LINK) {
		row_set(r, !v);
	} else if (r->kind == R_MIX) {
		/* Off and back to where it was; a route that never was on comes on at 0 dB */
		last = &last_gain[cur.ch][r->mix.src];
		if (v != UCX_MIX_OFF)
			*last = v;
		row_set(r, v == UCX_MIX_OFF ? *last : UCX_MIX_OFF);
	} else if (row_is_switch(r)) {
		row_set(r, step_setting(r->setting, v, row_on(r) ? -1 : 1, false));
	}
}

static void row_adjust(const struct row *r, int dir, bool fine)
{
	int v = row_value(r);

	if (!has_value(r))
		return;
	switch (r->kind) {
	case R_HEAD:
	case R_SET:
		row_set(r, step_setting(r->setting, v, dir, fine));
		break;
	case R_VOL:
		row_set(r, clampi(snap(v, fine ? 1 : 10, dir), UCX_VOL_MIN, UCX_VOL_MAX));
		break;
	case R_LINK:
		row_set(r, dir > 0);
		break;
	case R_MIX:
		row_set(r, step_gain(v, dir, fine));
		break;
	default:
		break;
	}
}

static void row_reset(const struct row *r)
{
	switch (r->kind) {
	case R_SET:
		row_set(r, ucx_settings[r->setting].def);
		break;
	case R_VOL:
		row_set(r, UCX_VOL_MIN);
		break;
	case R_MIX:
		row_set(r, UCX_MIX_OFF);
		break;
	default:
		break;
	}
}

/*
 * The lines @first up to @last of the form back at their defaults, as one
 * step for undo. Two things stay: the stereo link, which is not a value but
 * how the channels are shown, and in the form of a channel a level that it
 * shares with others (Line In 5-8, Line Out 1-6).
 */
static void rows_reset(int first, int last, const char *what)
{
	const struct row *r;
	int i, pass, n = 0;
	bool differs;

	/* Counted first: nothing to do leaves nothing to undo */
	for (pass = 0; pass < 2; pass++) {
		if (pass && !n) {
			say(MSG_INFO, "%s is at its defaults already", what);
			return;
		}
		if (pass)
			doc_checkpoint(&doc, 0);
		for (i = first; i < last; i++) {
			r = &rows[i];
			if (r->kind == R_VOL)
				differs = row_value(r) != UCX_VOL_MIN;
			else if (r->kind == R_MIX)
				differs = row_value(r) != UCX_MIX_OFF;
			else
				differs = r->setting >= 0 &&
					  !(has_list() && ucx_settings[r->setting].ch < 0) &&
					  row_value(r) != ucx_settings[r->setting].def;
			if (!differs)
				continue;
			n += !pass;
			if (pass && r->kind == R_VOL)
				doc_set_volume(&doc, cur.ch, UCX_VOL_MIN);
			else if (pass && r->kind == R_MIX)
				doc_mix_set(&doc, &cur, &r->mix, UCX_MIX_OFF, 0);
			else if (pass)
				doc_set(&doc, r->setting, ucx_settings[r->setting].def);
		}
	}
	changed();
	say(MSG_OK, "%s: %d value%s back at the default (u undoes it)", what, n, n == 1 ? "" : "s");
}

/* Everything under the heading @head, the heading's own switch included */
static void section_reset(int head)
{
	int last;

	for (last = head + 1; last < nrows && rows[last].kind != R_HEAD; last++)
		;
	rows_reset(head, last, rows[head].label);
}

/* Every section of the channel that the form shows */
static void channel_reset(void)
{
	char name[32];

	doc_strip_name(&cur, name, sizeof(name));
	rows_reset(0, nrows, name);
}

/*
 * A whole page: @state with everything that page shows at its default, the
 * stereo links too, so that the page is what a file without it would give.
 * The Routing page is the mixer: every route off.
 */
static void page_defaults(int t, struct doc_state *state)
{
	struct ucx_config def, *cfg = &state->cfg;
	int i;

	ucx_config_defaults(&def);
	for (i = 0; i < UCX_SETTINGS; i++)
		if (tab_of(&ucx_settings[i]) == t)
			cfg->set[i] = def.set[i];
	if (t == TAB_INPUTS)
		memcpy(cfg->link_in, def.link_in, sizeof(def.link_in));
	if (t == TAB_OUTPUTS) {
		memcpy(cfg->volume, def.volume, sizeof(def.volume));
		memcpy(cfg->link_out, def.link_out, sizeof(def.link_out));
		state->fx_link = true;
	}
	if (t == TAB_OUTPUTS || t == TAB_ROUTING) {
		memcpy(cfg->mix, def.mix, sizeof(def.mix));
		memcpy(cfg->invert, def.invert, sizeof(def.invert));
	}
}

/* How many values of the config a reset of page @t would change */
static int page_differs(int t)
{
	static struct doc_state state;
	const int *a = (const int *)&state.cfg, *b = (const int *)&doc.s.cfg;
	const int *skip = (const int *)state.cfg.invert;
	unsigned int i;
	int n = 0;

	state = doc.s;
	page_defaults(t, &state);
	/* The phase of a route is not counted beside its gain */
	for (i = 0; i < sizeof(state.cfg) / sizeof(int); i++)
		n += a[i] != b[i] && (a + i < skip || a + i >= skip + sizeof(state.cfg.invert) / sizeof(int));
	return n;
}

/* Where a number is on its slider, 0 to 1 */
static double row_frac(const struct row *r)
{
	int v = row_value(r);
	double f;

	if (r->kind == R_VOL)
		f = (double)(v - UCX_VOL_MIN) / (UCX_VOL_MAX - UCX_VOL_MIN);
	else if (r->kind == R_MIX)
		f = v == UCX_MIX_OFF ? 0 : (v + 6000) / 6600.0;
	else
		f = setting_frac(r->setting, v);
	return f < 0 ? 0 : f > 1 ? 1 : f;
}

/* The value at a place on the slider, to the step the arrow keys take */
static int row_at_frac(const struct row *r, double f)
{
	const struct ucx_setting *s;
	int min, max, step, v;
	double x, mag;

	f = f < 0 ? 0 : f > 1 ? 1 : f;
	if (r->kind == R_VOL)
		return (int)lround((UCX_VOL_MIN + f * (UCX_VOL_MAX - UCX_VOL_MIN)) / 10) * 10;
	if (r->kind == R_MIX)
		/* The slider starts at -60 dB; its left end is off */
		return f <= 0 ? UCX_MIX_OFF : (int)lround(-60 + f * 66) * 100;
	s = &ucx_settings[r->setting];
	setting_range(r->setting, &min, &max);
	if (is_log(s)) {
		x = min * pow((double)max / min, f);
		mag = pow(10.0, floor(log10(x)) - 1);
		v = (int)(round(x / mag) * mag);
	} else {
		step = s->type != UCX_T_NUM ? 1 : !strcmp(s->key, "balance") ? 5 :
		       nice_step(max - min);
		v = (int)lround((min + f * (max - min) + s->off) / step) * step - s->off;
		if (s->type == UCX_T_MIC_GAIN && v > 0 && v < 10)
			v = v < 5 ? 0 : 10;
	}
	return clampi(v, min, max);
}

static void mix_invert(const struct row *r)
{
	int invert, gain = doc_mix_get(&doc, &cur, &r->mix, &invert);

	if (gain == UCX_MIX_OFF) {
		say(MSG_INFO, "This route is off: there is no phase to invert");
		return;
	}
	doc_checkpoint(&doc, 0);
	doc_mix_set(&doc, &cur, &r->mix, gain, !invert);
	changed();
}

static void mix_split(const struct row *r)
{
	doc_checkpoint(&doc, 0);
	if (doc_mix_split(&doc, &cur, &r->mix))
		return;
	doc_undo(&doc);
	say(MSG_INFO, cur.n == 2 ? "Switch this route off first: as one row it is stereo, "
	    "as two rows each source is mono on both sides" :
	    "Set both to the same gain first");
}

/* A typed value. False with a message if it isn't one */
static bool row_enter(const struct row *r, const char *typed)
{
	const struct ucx_setting *s = r->kind == R_SET ? &ucx_settings[r->setting] : NULL;
	char buf[64], *end, *p;
	int min, max, v;
	double x;

	snprintf(buf, sizeof(buf), "%.60s", typed);
	for (p = buf; *p; p++)
		if (*p == ',')
			*p = '.';
	if (r->kind == R_MIX && (!strcmp(buf, "off") || !strcmp(buf, "-"))) {
		row_set(r, UCX_MIX_OFF);
		return true;
	}
	x = strtod(buf, &end);
	while (*end == ' ')
		end++;
	if (end == buf || *end || !isfinite(x)) {
		say(MSG_WARN, "\"%s\" is not a number", typed);
		return false;
	}
	if (r->kind == R_VOL) {
		v = clampi((int)lround(x * 10), UCX_VOL_MIN, UCX_VOL_MAX);
	} else if (r->kind == R_MIX) {
		v = clampi((int)lround(x * 100), UCX_GAIN_MIN, UCX_GAIN_MAX);
	} else {
		setting_range(r->setting, &min, &max);
		if (s->type == UCX_T_MIC_GAIN)
			v = x > 0 && x < 10 ? 10 : (int)lround(x);
		else if (s->type == UCX_T_INST_GAIN)
			v = (int)lround(x * 2);
		else
			v = (int)lround(x * s->div / s->mul) - s->off;
		v = clampi(v, min, max);
	}
	row_set(r, v);
	return true;
}

/* Drawing */

/* Is the mouse over these cells, with nothing in front of them? */
static bool hot(int x, int y, int w)
{
	/* Not while a slider is held: the mouse then belongs to it, wherever it is */
	return mode == M_NORMAL && !drag.on && hover_y == y && hover_x >= x && hover_x < x + w;
}

/* @a with @pct percent of @b */
static uint32_t blend(uint32_t a, uint32_t b, int pct)
{
	uint32_t out = 0;
	int shift;

	for (shift = 0; shift < 24; shift += 8)
		out |= (((a >> shift & 0xff) * (100 - pct) + (b >> shift & 0xff) * pct) / 100) << shift;
	return out;
}

static void box(int x, int y, int w, int h, const char *title)
{
	int i;

	term_fill(x, y, w, h, ' ', C_TEXT, C_PANEL);
	for (i = 1; i < w - 1; i++) {
		term_put(x + i, y, G_TRACK, C_FAINT, C_PANEL, 0);
		term_put(x + i, y + h - 1, G_TRACK, C_FAINT, C_PANEL, 0);
	}
	for (i = 1; i < h - 1; i++) {
		term_put(x, y + i, G_VLINE, C_FAINT, C_PANEL, 0);
		term_put(x + w - 1, y + i, G_VLINE, C_FAINT, C_PANEL, 0);
	}
	term_put(x, y, 0x256d, C_FAINT, C_PANEL, 0);
	term_put(x + w - 1, y, 0x256e, C_FAINT, C_PANEL, 0);
	term_put(x, y + h - 1, 0x2570, C_FAINT, C_PANEL, 0);
	term_put(x + w - 1, y + h - 1, 0x256f, C_FAINT, C_PANEL, 0);
	text(x + 2, y, w - 4, C_BRIGHT, C_PANEL, ATTR_BOLD, " %s ", title);
}

/* "key what  key what": returns the next free column */
static int hints(int x, int y, int xmax, uint32_t bg, const char *const *pairs)
{
	int i;

	for (i = 0; pairs[i] && x < xmax; i += 2) {
		if (x + term_width(pairs[i]) + term_width(pairs[i + 1]) + 1 > xmax)
			break;
		x += text(x, y, xmax - x, C_TEXT, bg, ATTR_BOLD, "%s", pairs[i]);
		x += text(x + 1, y, xmax - x - 1, C_DIM, bg, 0, "%s", pairs[i + 1]) + 3;
	}
	return x;
}

static uint32_t meter_colour(float db)
{
	return db > -6 ? C_RED : db > -18 ? C_YELLOW : C_GREEN;
}

/* A level bar over -60 .. 0 dBFS; two channels share the line as its upper and lower half */
static void draw_meter(int x, int y, int w, float l, float r, bool stereo)
{
	int i, eighths;
	float lo, hi;
	uint32_t c;

	for (i = 0; i < w; i++) {
		lo = -60 + 60.0f * i / w;
		hi = -60 + 60.0f * (i + 1) / w;
		c = meter_colour(hi);
		if (stereo) {
			term_put(x + i, y, G_UPPER, l > (lo + hi) / 2 ? c : C_TRACK,
				 r > (lo + hi) / 2 ? c : C_TRACK, 0);
			continue;
		}
		eighths = clampi((int)lroundf((l - lo) / (hi - lo) * 8), 0, 8);
		term_put(x + i, y, eighths ? 0x2590 - eighths : ' ', c, C_TRACK, 0);
	}
}

/*
 * The smallest peak the unit reports is 16 of 2^27, -138.5 dBFS. A number
 * below that is only the display falling after the signal stopped.
 */
static void fmt_level(float db, char *buf, size_t len)
{
	if (db < -138.5f)
		snprintf(buf, len, "-inf");
	else
		snprintf(buf, len, "%.1f", db);
}

static void draw_slider(int x, int y, int w, double frac, double origin, uint32_t fill, uint32_t knob,
			uint32_t bg)
{
	int k = frac < 0 ? -1 : (int)lround(clampi((int)(frac * 1000), 0, 1000) / 1000.0 * (w - 1));
	int o = (int)lround(origin * (w - 1)), i;
	/* Under the mouse the track stands out and the knob has the colour of a held one */
	bool over = hot(x, y, w);

	for (i = 0; i < w; i++) {
		if (i == k)
			term_put(x + i, y, G_KNOB, over ? C_YELLOW : knob, bg, 0);
		else if (k >= 0 && i >= (k < o ? k : o) && i <= (k > o ? k : o))
			term_put(x + i, y, G_FILL, over ? blend(fill, C_BRIGHT, 30) : fill, bg, 0);
		else
			term_put(x + i, y, G_TRACK, over ? C_DIM : C_LINE, bg, 0);
	}
}

/* An on/off field. Things that are better off (mute, phantom power) are red when on */
static int draw_switch(int x, int y, const struct row *r, bool muted)
{
	const char *key = r->setting >= 0 ? ucx_settings[r->setting].key : "";
	bool on = row_on(r);
	bool alarm = !strcmp(key, "mute") || !strcmp(key, "phantom") || !strcmp(key, "fx-mute");
	uint32_t c = alarm ? C_RED : muted ? C_DIM : C_GREEN;

	if (hot(x, y, 5))
		return text(x, y, 5, on ? C_INK : C_BRIGHT, on ? blend(c, C_BRIGHT, 40) : C_FAINT,
			    ATTR_BOLD, on ? " on " : " off ");
	if (on)
		return text(x, y, 5, C_INK, c, ATTR_BOLD, " on ");
	return text(x, y, 5, C_DIM, C_TRACK, 0, " off ");
}

#define LABEL_W	15
#define VALUE_W	10

/* Where the lines of a pane have their control (a slider of @sw cells) and their value */
static void row_span(const struct pane *p, int *cx, int *sw, int *vx)
{
	*cx = p->x + 3 + LABEL_W + 1;
	*vx = p->x + p->w - 3 - VALUE_W;
	*sw = *vx - 1 - *cx;
}

static void draw_row(const struct pane *p, int idx, int y, bool active)
{
	const struct row *r = &rows[idx];
	bool focused = idx == views[tab].focus && !views[tab].in_list;
	bool lit = focused && active;
	/* A block that is switched off shows its settings, dimmed */
	bool muted = r->block >= 0 && !is_on(r->block);
	bool over = focusable(r) && hot(p->x, y, p->w);
	uint32_t bg = lit ? over ? blend(C_SEL, C_BRIGHT, 6) : C_SEL : over ? C_HOVER :
		      focused ? C_SEL_DIM : C_BG;
	uint32_t fg = lit ? C_BRIGHT : muted ? C_FAINT : C_TEXT;
	int x = p->x, w = p->w, lx = x + 3, cx, vx, sw, v, invert = 0, n, i, nw;
	char buf[64];
	double frac = 0, origin = 0;

	row_span(p, &cx, &sw, &vx);
	term_fill(x, y, w, 1, ' ', fg, bg);
	if (lit)
		term_put(x, y, G_MARK, C_ACCENT, bg, 0);
	switch (r->kind) {
	case R_GAP:
		return;
	case R_HEAD:
		muted = r->setting >= 0 && !is_on(r->setting);
		n = text(x + 1, y, w - 10, lit ? C_BRIGHT : muted ? C_DIM : C_ACCENT, bg, ATTR_BOLD,
			 "%s", r->label);
		for (i = x + n + 2; i < x + w - (r->setting >= 0 ? 9 : 2); i++)
			term_put(i, y, G_TRACK, C_LINE, bg, 0);
		if (r->setting >= 0)
			draw_switch(x + w - 8, y, r, false);
		break;
	case R_NOTE:
		n = text(lx, y, LABEL_W, C_DIM, bg, 0, "%s", r->label);
		text(*r->label ? cx : lx, y, x + w - 1 - (*r->label ? cx : lx), r->colour, bg, 0, "%s",
		     r->text);
		return;
	default:
		break;
	}
	if (r->kind < R_SET) {
		if (row_pending(r))
			term_put(x + w - 2, y, G_KNOB, C_YELLOW, bg, 0);
		return;
	}

	text(lx, y, LABEL_W, fg, bg, 0, "%s", r->label);
	v = row_value(r);
	if (row_is_switch(r)) {
		draw_switch(cx, y, r, muted);
	} else if (!row_is_number(r)) {
		/* One of a list */
		fmt_row(r, v, buf, sizeof(buf));
		setting_range(r->setting, &n, &i);
		nw = term_width(buf);
		term_put(cx, y, 0x2039, v <= n ? C_LINE : hot(cx - 1, y, 3) ? C_BRIGHT : C_DIM, bg, 0);
		text(cx + 2, y, x + w - 6 - cx, fg, bg, hot(cx + 2, y, nw) ? ATTR_UNDERLINE : 0, "%s",
		     buf);
		term_put(cx + nw + 3, y, 0x203a,
			 v >= i ? C_LINE : hot(cx + nw + 2, y, 3) ? C_BRIGHT : C_DIM, bg, 0);
	} else {
		if (r->kind == R_VOL) {
			frac = (double)(v - UCX_VOL_MIN) / (UCX_VOL_MAX - UCX_VOL_MIN);
		} else if (r->kind == R_MIX) {
			doc_mix_get(&doc, &cur, &r->mix, &invert);
			frac = v == UCX_MIX_OFF ? -1 : (v + 6000) / 6600.0;
			if (v == UCX_MIX_OFF && !lit)
				fg = C_FAINT;
			text(lx, y, LABEL_W, fg, bg, 0, "%s", r->label);
			/* What the source carries right now */
			n = dev ? clampi((int)((source_level(&r->mix) + 60) / 60 * 8), 0, 8) : 0;
			if (n)
				term_put(cx - 2, y, 0x2580 + n, meter_colour(source_level(&r->mix)),
					 bg, 0);
		} else {
			frac = setting_frac(r->setting, v);
			origin = setting_origin(r->setting);
		}
		if (sw >= 6)
			draw_slider(cx, y, sw, frac, origin, muted ? C_FAINT : C_ACCENT,
				    drag.on && drag.row == idx ? C_YELLOW : muted ? C_DIM : C_BRIGHT,
				    bg);
		fmt_row(r, v, buf, sizeof(buf));
		if (mode == M_EDIT && focused) {
			term_fill(vx, y, VALUE_W, 1, ' ', C_BRIGHT, C_BG);
			n = text(vx, y, VALUE_W, C_BRIGHT, C_BG, 0, "%s", input);
			if (!*input)
				text(vx, y, VALUE_W, C_FAINT, C_BG, 0, "%s", buf);
			term_cursor(vx + n, y);
		} else {
			/* Underlined under the mouse: a click types it */
			text_right(x + w - 3, y, r->kind == R_MIX && v == UCX_MIX_OFF ? C_FAINT : fg,
				   bg, hot(vx, y, VALUE_W) ? ATTR_UNDERLINE : 0, "%s%s",
				   invert ? "ø " : "", buf);
		}
	}
	if (row_pending(r))
		term_put(x + w - 2, y, G_KNOB, C_YELLOW, bg, 0);
}

static void draw_pane(int p)
{
	struct pane *pn = &panes[p];
	struct view *v = &views[tab];
	int focus = v->focus - pn->first, *top = &v->scroll[p], i, t0, t1;
	bool active = !v->in_list && !in_tabs && focus >= 0 && focus < pn->count;

	if (active) {
		/* Keep the focus in view, with its heading if that is the line above */
		i = focus > 0 && rows[v->focus - 1].kind == R_HEAD ? focus - 1 : focus;
		if (i < *top)
			*top = i;
		if (focus >= *top + pn->h)
			*top = focus - pn->h + 1;
	}
	*top = clampi(*top, 0, pn->count > pn->h ? pn->count - pn->h : 0);
	term_clip(pn->x, pn->y, pn->w, pn->h);
	for (i = *top; i < pn->count && i < *top + pn->h; i++)
		draw_row(pn, pn->first + i, pn->y + i - *top, active);
	if (pn->count > pn->h) {
		t0 = *top * pn->h / pn->count;
		t1 = (*top + pn->h) * pn->h / pn->count;
		for (i = 0; i < pn->h; i++)
			term_put(pn->x + pn->w - 1, pn->y + i, i >= t0 && i <= t1 ? 0x2595 : ' ',
				 C_FAINT, C_BG, 0);
	}
	term_unclip();
}

/* The rows in one column or two, in the rectangle given */
static void layout_panes(int x, int y, int w, int h)
{
	int split, pw;

	for (split = 0; split < nrows && rows[split].col == 0; split++)
		;
	if (w >= 86 && split < nrows) {
		pw = (w - 3) / 2 > 72 ? 72 : (w - 3) / 2;
		npanes = 2;
		panes[0] = (struct pane){ x, y, pw, h, 0, split };
		/* The gap that parts the columns when they are one */
		split += rows[split].kind == R_GAP;
		panes[1] = (struct pane){ x + pw + 3, y, pw, h, split, nrows - split };
	} else {
		npanes = 1;
		panes[0] = (struct pane){ x, y, w > 76 ? 76 : w, h, 0, nrows };
	}
}

static void draw_list(int x, int y, int w, int h)
{
	static const char *const blocks[] = { "lowcut.on", "eq.on", "dynamics.on", "autolevel.on" };
	struct view *v = &views[tab];
	bool flags = w >= 36;
	int i, b, n, mx, set;
	char name[32], buf[32];

	list_pane = (struct pane){ x, y, w, h, 0, nstrips };
	if (sel < v->list_top)
		v->list_top = sel;
	if (sel >= v->list_top + h)
		v->list_top = sel - h + 1;
	v->list_top = clampi(v->list_top, 0, nstrips > h ? nstrips - h : 0);
	for (i = v->list_top; i < nstrips && i < v->list_top + h; i++) {
		const struct strip *s = &strips[i];
		bool lit = i == sel && v->in_list && !in_tabs;
		bool over = hot(x, y + i - v->list_top, w);
		uint32_t bg = lit ? over ? blend(C_SEL, C_BRIGHT, 6) : C_SEL : over ? C_HOVER :
			      i == sel ? C_SEL_DIM : C_BG;
		int ry = y + i - v->list_top, pending = pend_ch[s->output][s->ch];

		if (s->n == 2)
			pending += pend_ch[s->output][s->ch + 1];
		term_fill(x, ry, w, 1, ' ', C_TEXT, bg);
		if (i == sel)
			term_put(x, ry, G_MARK, lit ? C_ACCENT : C_FAINT, bg, 0);
		doc_strip_name(s, name, sizeof(name));
		text(x + 2, ry, 11, i == sel ? C_BRIGHT : C_TEXT, bg, i == sel ? ATTR_BOLD : 0, "%s",
		     name);

		/* The one number that matters most: volume, or preamp gain */
		buf[0] = 0;
		if (s->output && s->ch < UCX_OUTPUTS) {
			set = doc_setting(true, s->ch, "mute");
			if (is_on(set))
				text_right(x + 22, ry, C_RED, bg, 0, "muted");
			else
				text_right(x + 22, ry, C_DIM, bg, 0, "%.1f dB",
					   doc.s.cfg.volume[s->ch] / 10.0);
		} else if (!s->output && (set = doc_setting(false, s->ch, "gain")) >= 0) {
			fmt_setting(set, doc.s.cfg.set[set], buf, sizeof(buf));
			n = text_right(x + 22, ry, C_DIM, bg, 0, "%s", buf);
			set = doc_setting(false, s->ch, "phantom");
			if (set >= 0 && is_on(set))
				text_right(n - 1, ry, C_RED, bg, 0, "48V");
		}
		mx = x + 24;
		if (flags && !(s->output && s->ch >= UCX_OUTPUTS)) {
			for (b = 0; b < 4; b++)
				term_put(mx + b, ry, "LEDA"[b], is_on(doc_setting(s->output, s->ch,
				         blocks[b])) ? C_ACCENT : C_LINE, bg, 0);
		}
		if (flags)
			mx += 5;
		if (dev && x + w - 3 - mx >= 3)
			draw_meter(mx, ry, x + w - 3 - mx, strip_level(s, 0), strip_level(s, 1),
				   s->n == 2);
		if (pending)
			term_put(x + w - 2, ry, G_KNOB, C_YELLOW, bg, 0);
	}
}

/* Name and level of the strip the form shows */
static void draw_strip_header(int x, int y, int w)
{
	char name[32], l[16], r[16];
	int n, mw = w > 60 ? 40 : w - 22;

	doc_strip_name(&cur, name, sizeof(name));
	n = text(x + 1, y, w - 2, C_BRIGHT, C_BG, ATTR_BOLD, "%s", name);
	text(x + n + 3, y, w - n - 4, C_FAINT, C_BG, 0, "%s",
	     cur.output && cur.ch >= UCX_OUTPUTS ? "to reverb and echo" :
	     cur.n == 2 ? "stereo pair" : cur.output ? "mono output" : "mono input");
	if (!dev) {
		text(x + 1, y + 1, w - 2, C_FAINT, C_BG, 0, "no unit: levels are not shown");
		return;
	}
	draw_meter(x + 1, y + 1, mw, strip_level(&cur, 0), strip_level(&cur, 1), cur.n == 2);
	fmt_level(strip_level(&cur, 0), l, sizeof(l));
	fmt_level(strip_level(&cur, 1), r, sizeof(r));
	if (cur.n == 2)
		text(x + mw + 3, y + 1, w - mw - 4, C_DIM, C_BG, 0, "%s / %s dBFS", l, r);
	else
		text(x + mw + 3, y + 1, w - mw - 4, C_DIM, C_BG, 0, "%s dBFS", l);
}

static void draw_effects_header(int x, int y, int w)
{
	int mw = clampi((w - 44) / 2, 6, 30), n;

	n = text(x + 1, y, w, C_BRIGHT, C_BG, ATTR_BOLD, "Effects");
	text(x + n + 3, y, w - n - 4, C_FAINT, C_BG, 0,
	     "fed by the FX sends, heard through each output's FX return");
	if (!dev)
		return;
	n = x + 1 + text(x + 1, y + 1, 10, C_DIM, C_BG, 0, "send ");
	draw_meter(n, y + 1, mw, level_of(&meters.output_pre[UCX_OUTPUTS]),
		   level_of(&meters.output_pre[UCX_OUTPUTS + 1]), true);
	n += mw + 3;
	n += text(n, y + 1, 10, C_DIM, C_BG, 0, "return ");
	draw_meter(n, y + 1, mw, level_of(&meters.fx_return[0]), level_of(&meters.fx_return[1]),
		   true);
}

static int routing_x(int src)
{
	return rt_x + src * rt_cw + (src >= UCX_SRC_INPUT ? 2 : 0);
}

/*
 * The route that the gain at @out, @src belongs to. A cell of the grid is one
 * gain, but a linked pair has routes of two: a stereo route (left to left,
 * right to right) or a mono source to both sides. The square of four cells
 * where a linked pair meets a source pair is that one stereo route, wherever
 * in it the cursor is: the two gains across can't be set while it is linked.
 */
static void routing_cell(int out, int src, struct rcell *c)
{
	struct strip list[UCX_MIX_OUTS];
	struct mixrow mix[UCX_SOURCES];
	int n = doc_strips(&doc, true, list), i;

	for (i = 0; i < n - 1 && out >= list[i].ch + list[i].n; i++)
		;
	c->strip = list[i];
	c->row = (struct mixrow){ src, 1 };
	if (c->strip.n == 1)
		return;
	n = doc_mix_rows(&doc, &c->strip, mix);
	for (i = 0; i < n - 1 && src >= mix[i].src + mix[i].n; i++)
		;
	c->row = mix[i];
}

/* Is the gain at @out, @src part of the route @c? */
static bool routing_in(const struct rcell *c, int out, int src)
{
	if (out < c->strip.ch || out >= c->strip.ch + c->strip.n || src < c->row.src ||
	    src >= c->row.src + c->row.n)
		return false;
	return c->strip.n == 1 || c->row.n == 1 || out - c->strip.ch == src - c->row.src;
}

/*
 * The cell under the cursor as a line of a mix, so that it is changed by
 * the code that changes those. Sets @cur, which no form uses on this page.
 */
static const struct row *routing_row(struct rcell *c)
{
	static struct row row;

	routing_cell(rt_out, rt_src, c);
	cur = c->strip;
	row = (struct row){ .kind = R_MIX, .setting = -1, .block = -1, .mix = c->row };
	return &row;
}

/* The mixer as a grid, outputs down and sources across, changed in place */
static void draw_routing(int x, int y, int w, int h)
{
	static const char *const groups[] = { "Mic", "Ins", "Line", "SPD", "ADAT" };
	static const char *const narrow[] = { "Mc", "In", "Line", "SP", "ADAT" };
	static const int starts[] = { 0, 2, 4, 8, 10 };
	char buf[80], name[32], source[32];
	const char *chan;
	struct rcell c;
	int out, src, g, i, gain, invert, iy = y + h - 1;
	uint32_t fg, bg;

	rt_cw = clampi((w - 17) / UCX_SOURCES, 1, 4);
	rt_x = x + 13;
	rt_y = y + 3;
	rt_rows = clampi(h - 5, 1, UCX_MIX_OUTS);
	if (rt_out < rt_top)
		rt_top = rt_out;
	if (rt_out >= rt_top + rt_rows)
		rt_top = rt_out - rt_rows + 1;
	routing_cell(rt_out, rt_src, &c);

	text(x + 1, y, w - 2, C_BRIGHT, C_BG, ATTR_BOLD, "Routing");
	text(x + 11, y, w - 12, C_FAINT, C_BG, 0, "the whole mixer: outputs down, sources across");
	text(routing_x(0), y + 1, 8, C_DIM, C_BG, ATTR_BOLD, "Playback");
	for (g = 0; g < 5; g++)
		text(routing_x(UCX_SRC_INPUT + starts[g]), y + 1, 8, g % 2 ? C_FAINT : C_DIM, C_BG,
		     ATTR_BOLD, "%s", rt_cw > 1 ? groups[g] : narrow[g]);
	for (src = 0; src < UCX_SOURCES; src++) {
		chan = strrchr(ucx_source_name(src), ' ') + 1;
		text(routing_x(src), y + 2, rt_cw, src == rt_src ? C_BRIGHT :
		     hover_y >= rt_y && hover_y < rt_y + rt_rows && hot(routing_x(src), hover_y, rt_cw) ?
		     C_TEXT : C_FAINT, C_BG, 0, "%s",
		     rt_cw > 2 || !chan[1] ? chan : chan + 1);
	}
	for (i = 0; i < rt_rows; i++) {
		out = rt_top + i;
		text(x + 1, rt_y + i, 10, out == rt_out ? C_BRIGHT :
		     hot(rt_x, rt_y + i, routing_x(UCX_SOURCES - 1) + rt_cw - rt_x) ? C_TEXT : C_DIM,
		     C_BG, out == rt_out ? ATTR_BOLD : 0, "%s", ucx_output_name(out));
		/* A bracket joins the two outputs of a linked pair */
		if (doc_linked(&doc, true, out / 2))
			term_put(x + 11, rt_y + i, out % 2 ? 0x2518 : 0x2510, C_FAINT, C_BG, 0);
		for (src = 0; src < UCX_SOURCES; src++) {
			gain = doc.s.cfg.mix[out][src];
			/* The cursor, the rest of its route, then its row and column */
			bg = out == rt_out && src == rt_src ? C_ACCENT :
			     routing_in(&c, out, src) ? 0x3d5a80 :
			     hot(routing_x(src), rt_y + i, rt_cw) ? C_FAINT :
			     out == rt_out || src == rt_src ? C_SEL_DIM : C_BG;
			fg = gain == UCX_MIX_OFF ? C_LINE : gain > 0 ? C_ORANGE :
			     gain >= -600 ? C_ACCENT : gain >= -2000 ? 0x4f86c6 : 0x3a5f8a;
			if (pend_mix[out][src])
				fg = C_YELLOW;
			if (bg == C_ACCENT)
				fg = C_INK;
			else if (bg == 0x3d5a80)
				fg = C_BRIGHT;
			term_fill(routing_x(src), rt_y + i, rt_cw, 1, ' ', fg, bg);
			if (gain != UCX_MIX_OFF && rt_cw == 4)
				text(routing_x(src), rt_y + i, 3, fg, bg, 0, "%3d",
				     (int)lround(gain / 100.0));
			else
				term_put(routing_x(src), rt_y + i, gain == UCX_MIX_OFF ? G_DOT :
					 doc.s.cfg.invert[out][src] ? 0xf8 : G_SQUARE, fg, bg, 0);
		}
	}

	/* What the cell under the cursor is */
	doc_strip_name(&c.strip, name, sizeof(name));
	source_name(&c.row, source, sizeof(source));
	gain = doc_mix_get(&doc, &c.strip, &c.row, &invert);
	fmt_gain(gain, buf, sizeof(buf));
	i = x + 1 + text(x + 1, iy, w - 2, C_TEXT, C_BG, 0, "%s ← %s", name, source);
	i += text(i, iy, w - i, C_FAINT, C_BG, 0, "%s",
		  c.strip.n == 1 ? "  " : c.row.n == 2 ? "  stereo, L to L and R to R  " :
		  "  to both sides  ");
	if (mode == M_EDIT) {
		term_fill(i, iy, VALUE_W, 1, ' ', C_BRIGHT, C_SEL);
		g = text(i, iy, VALUE_W, C_BRIGHT, C_SEL, 0, "%s", input);
		if (!*input)
			text(i, iy, VALUE_W, C_FAINT, C_SEL, 0, "%s", buf);
		term_cursor(i + g, iy);
		return;
	}
	i += text(i, iy, 24, gain == UCX_MIX_OFF ? C_DIM : C_BRIGHT, C_BG, ATTR_BOLD, "%s%s", buf,
		  invert ? ", inverted" : "");
	if (pend_mix[rt_out][rt_src]) {
		fmt_gain(dev_state.cfg.mix[rt_out][rt_src], buf, sizeof(buf));
		text(i + 3, iy, w - i - 3, C_YELLOW, C_BG, 0, "● the unit has %s", buf);
	}
}

static void draw_header(void)
{
	const char *name = path[0] ? strrchr(path, '/') ? strrchr(path, '/') + 1 : path :
			   from_device ? "state of the unit" : "new config";
	int x = 1, xr = W - 1;

	term_fill(0, 0, W, 1, ' ', C_TEXT, C_BAR);
	x += text(x, 0, W, C_ACCENT, C_BAR, ATTR_BOLD, "◆ ");
	x += text(x, 0, W, C_BRIGHT, C_BAR, ATTR_BOLD, "Fireface UCX");
	x += text(x + 2, 0, W, C_LINE, C_BAR, 0, "│") + 4;
	x += text(x, 0, W - x - 30, path[0] ? C_TEXT : C_DIM, C_BAR, path[0] ? 0 : ATTR_ITALIC, "%s",
		  name);
	if (modified())
		text(x + 1, 0, 12, C_YELLOW, C_BAR, 0, "● modified");

	if (!dev) {
		text_right(xr, 0, C_DIM, C_BAR, 0, "○ no unit");
		return;
	}
	if (live)
		xr = text_right(xr, 0, C_INK, C_GREEN, ATTR_BOLD, " LIVE ") - 2;
	if (plan.n)
		xr = text_right(xr, 0, C_YELLOW, C_BAR, 0, "● %u to apply", plan.n) - 3;
	else
		xr = text_right(xr, 0, C_GREEN, C_BAR, 0, "✓ in sync") - 3;
	if (status_ok && status.rate)
		text_right(xr, 0, C_DIM, C_BAR, 0, "%g kHz", status.rate / 1000.0);
}

static void draw_tabs(void)
{
	int t, x = 1;

	term_fill(0, 1, W, 1, ' ', C_TEXT, C_BAR);
	for (t = 0; t < TABS; t++) {
		/* Where the tab was in the last frame: it doesn't move */
		bool over = t != tab && hot(tab_x[t], 1, tab_x[t + 1] - tab_x[t] - 1);
		/* With the focus on this row, the page is what the keys act on */
		bool cursor = in_tabs && t == tab;
		uint32_t bg = cursor ? C_ACCENT : t == tab ? C_BG : over ? C_HOVER : C_BAR;

		tab_x[t] = x;
		x += text(x, 1, W, cursor ? C_INK : t == tab ? C_ACCENT : C_FAINT, bg, 0, " %d ", t + 1);
		x += text(x, 1, W, cursor ? C_INK : t == tab || over ? C_BRIGHT : C_DIM, bg,
			  t == tab ? ATTR_BOLD : 0, "%s ", tab_names[t]);
		if (pend_tab[t])
			x += text(x, 1, W, C_YELLOW, bg, 0, "● ");
		x++;
	}
	tab_x[TABS] = x;
}

/* The line above the keys: a message, or what the focused thing is */
static void draw_status(void)
{
	static const uint32_t colours[] = { C_DIM, C_GREEN, C_YELLOW, C_RED };
	const struct row *r = nrows ? &rows[views[tab].focus] : NULL;
	const struct meta *m;
	struct mixrow mix[UCX_SOURCES];
	char buf[400] = "", val[64], lo[32], hi[32];
	int y = H - 2, x = 1, n, i, min, max, len = 0;

	if (message[0] && now_ms() - message_time < (message_kind >= MSG_WARN ? 12000 : 5000)) {
		text(1, y, W - 2, colours[message_kind], C_BG, 0, "%s", message);
		return;
	}
	message[0] = 0;
	if (in_tabs && mode == M_NORMAL) {
		text(1, y, W - 2, C_FAINT, C_BG, 0, tab == TAB_ROUTING ?
		     "d: every route of the mixer off" :
		     "d: everything on this page back at its default");
		return;
	}
	if (tab == TAB_ROUTING || mode != M_NORMAL)
		return;
	if (views[tab].in_list) {
		if (tab != TAB_OUTPUTS)
			return;
		/* What this output plays */
		n = doc_mix_rows(&doc, &cur, mix);
		for (i = 0; i < n && len < (int)sizeof(buf) - 80; i++) {
			if (doc_mix_get(&doc, &cur, &mix[i], NULL) == UCX_MIX_OFF)
				continue;
			source_name(&mix[i], val, sizeof(val));
			fmt_gain(doc_mix_get(&doc, &cur, &mix[i], NULL), lo, sizeof(lo));
			len += snprintf(buf + len, sizeof(buf) - len, "%s%s %s", len ? ", " : "", val,
					lo);
		}
		text(1, y, W - 2, C_DIM, C_BG, 0, len ? "← %s" : "nothing is routed here%s", buf);
		return;
	}
	if (!r || !focusable(r))
		return;
	if (r->kind == R_HEAD) {
		m = r->setting >= 0 ? meta_of(ucx_settings[r->setting].key) : NULL;
		if (m && *m->help)
			x += text(x, y, W - 2, C_DIM, C_BG, 0, "%s", m->help) + 3;
		x += text(x, y, W - x, C_FAINT, C_BG, 0, "d: everything under this heading back at "
			  "its default") + 3;
	} else if (r->setting >= 0) {
		m = meta_of(ucx_settings[r->setting].key);
		if (m && *m->help)
			x += text(x, y, W - 2, C_DIM, C_BG, 0, "%s", m->help) + 3;
		if (is_number(r->setting)) {
			setting_range(r->setting, &min, &max);
			fmt_setting(r->setting, min + (ucx_settings[r->setting].off == UCX_VOL_MIN),
				    lo, sizeof(lo));
			fmt_setting(r->setting, max, hi, sizeof(hi));
			x += text(x, y, W - x, C_FAINT, C_BG, 0, "%s to %s", lo, hi) + 3;
		}
	} else if (r->kind == R_VOL) {
		x += text(x, y, W - 2, C_DIM, C_BG, 0, "Output volume") + 3;
		x += text(x, y, W - x, C_FAINT, C_BG, 0, "-65.0 to 6.0 dB") + 3;
	} else if (r->kind == R_LINK) {
		x += text(x, y, W - 2, C_DIM, C_BG, 0, cur.ch >= UCX_OUTPUTS ?
			  "One stereo bus, or left and right on their own" :
			  "Linked: one set of values for both channels, written as one "
			  "entry in the file") + 3;
	} else if (r->kind == R_MIX) {
		x += text(x, y, W - 2, C_DIM, C_BG, 0, "%s",
			  cur.n == 2 && r->mix.n == 2 ? "Stereo route: left to left, right to right" :
			  cur.n == 2 ? "Mono source, centred on both sides" :
			  r->mix.n == 2 ? "Both channels of the pair into this output" :
			  "Route into this output") + 3;
		x += text(x, y, W - x, C_FAINT, C_BG, 0, "-90 to 6 dB") + 3;
	}
	if (dev && row_pending(r)) {
		row_device(r, val, sizeof(val));
		text(x, y, W - x - 1, C_YELLOW, C_BG, 0, "● the unit has %s", val);
	}
}

static void draw_footer(void)
{
	static const char *const list[] = { "↑↓", "channel", "⏎", "edit", "d",
					    "defaults for the channel", NULL };
	static const char *const pages[] = { "←→", "page", "↓", "into it", "d",
					     "defaults for the page", NULL };
	static const char *const number[] = { "←→", "adjust", "⇧←→", "fine",
					      "⏎", "type", "d", "default", NULL };
	static const char *const toggle[] = { "space", "switch", "d", "default", NULL };
	static const char *const block[] = { "space", "switch", "d", "defaults for the section",
					     NULL };
	static const char *const section[] = { "d", "defaults for the section", NULL };
	static const char *const choice[] = { "←→", "change", "⏎", "list", "d",
					      "default", NULL };
	static const char *const route[] = { "←→", "gain", "space", "on/off", "i", "invert",
					     "s", "split", "⏎", "type", NULL };
	static const char *const grid[] = { "space", "on/off", "+ -", "gain", "⏎", "type", "i",
					    "invert", "s", "split", "g", "its output", NULL };
	static const char *const global[] = { "a", "apply", "L", "live", "^S", "save", "u", "undo",
					      "?", "help", "q", "quit", NULL };
	static const char *const typing[] = { "⏎", "set", "esc", "cancel", NULL };
	const struct row *r = nrows ? &rows[views[tab].focus] : NULL;
	const char *const *left = NULL;
	int x = 1, y = H - 1, gw = 0, i;

	term_fill(0, y, W, 1, ' ', C_TEXT, C_BAR);
	if (mode == M_EDIT || mode == M_PROMPT) {
		hints(x, y, W - 1, C_BAR, typing);
		return;
	}
	if (mode != M_NORMAL)
		return;
	if (in_tabs)
		left = pages;
	else if (tab == TAB_ROUTING)
		left = grid;
	else if (views[tab].in_list)
		left = list;
	else if (r && r->kind == R_MIX)
		left = route;
	else if (r && r->kind == R_HEAD)
		left = r->setting >= 0 ? block : section;
	else if (r && row_is_switch(r))
		left = toggle;
	else if (r && row_is_number(r))
		left = number;
	else if (r && focusable(r))
		left = choice;
	for (i = 0; global[i]; i += 2)
		gw += term_width(global[i]) + term_width(global[i + 1]) + 3;
	if (left)
		x = hints(x, y, W - gw - 2, C_BAR, left);
	hints(W - gw, y, W, C_BAR, global);
}

static void draw_popup_input(const char *title)
{
	int w = clampi(W - 8, 30, 72), x = (W - w) / 2, y = H / 2 - 2, shown_from;

	box(x, y, w, 4, title);
	shown_from = term_width(input) > w - 5 ? term_width(input) - (w - 5) : 0;
	term_fill(x + 2, y + 1, w - 4, 1, ' ', C_BRIGHT, C_BG);
	text(x + 2, y + 1, w - 4, C_BRIGHT, C_BG, 0, "%s", input + shown_from);
	term_cursor(x + 2 + input_pos - shown_from, y + 1);
	text(x + 2, y + 2, w - 4, C_FAINT, C_PANEL, 0, "enter: ok   esc: cancel");
}

static void draw_confirm(void)
{
	int w = clampi(W - 8, 30, 66), x = (W - w) / 2, y = H / 2 - 4, i, cx;

	box(x, y, w, 8, confirm_title);
	for (i = 0; i < 3; i++)
		text(x + 2, y + 1 + i, w - 4, C_TEXT, C_PANEL, 0, "%s", confirm_text[i]);
	cx = x + 2;
	for (i = 0; i < 3 && confirm_labels[i]; i++) {
		cx += text(cx, y + 6, 4, C_INK, C_ACCENT, ATTR_BOLD, " %c ", confirm_keys[i]);
		cx += text(cx + 1, y + 6, w - 4, C_TEXT, C_PANEL, 0, "%s", confirm_labels[i]) + 4;
	}
	text_right(x + w - 2, y + 6, C_FAINT, C_PANEL, 0, "esc: back");
}

static void draw_apply(void)
{
	int w = clampi(W - 6, 40, 96), h = clampi(apply_n + 6, 9, H - 4), x = (W - w) / 2;
	int y = (H - h) / 2, rows_h = h - 5, i;
	char title[80];

	if (apply_n)
		snprintf(title, sizeof(title), apply_live ? "Go live: %u changes go to the unit first" :
			 "Apply %u changes to the unit", plan.n);
	else
		snprintf(title, sizeof(title), "The unit already matches");
	box(x, y, w, h, title);
	apply_top = clampi(apply_top, 0, apply_n > rows_h ? apply_n - rows_h : 0);
	if (!apply_n) {
		text(x + 2, y + 2, w - 4, C_DIM, C_PANEL, 0,
		     "Nothing differs from what the driver holds for the unit.");
		text(x + 2, y + 3, w - 4, C_DIM, C_PANEL, 0,
		     "The unit never reports its mixer: p sends every gain again to be sure.");
	}
	for (i = 0; i < rows_h && apply_top + i < apply_n; i++)
		text(x + 2, y + 1 + i, w - 4, apply_lines[apply_top + i].danger ? C_RED : C_TEXT,
		     C_PANEL, 0, "%s", apply_lines[apply_top + i].text);
	if (apply_n > rows_h)
		text_right(x + w - 2, y + h - 3, C_FAINT, C_PANEL, 0, "%d-%d of %d  ↑↓",
			   apply_top + 1, clampi(apply_top + rows_h, 0, apply_n), apply_n);
	i = x + 2;
	i += text(i, y + h - 2, 9, C_INK, C_GREEN, ATTR_BOLD, " enter ");
	i += text(i + 1, y + h - 2, 20, C_TEXT, C_PANEL, 0, apply_live ? "apply, go live" : "apply") + 4;
	i += text(i, y + h - 2, 4, C_INK, C_ACCENT, ATTR_BOLD, " p ");
	text(i + 1, y + h - 2, w - (i - x) - 14, C_TEXT, C_PANEL, 0,
	     w >= 84 ? "apply with all 720 mixer gains" : "apply + resend every gain");
	text_right(x + w - 2, y + h - 2, C_FAINT, C_PANEL, 0, "esc: back");
}

static void draw_select(void)
{
	const struct row *r = &rows[views[tab].focus];
	int min, max, i, w = 30, h, x, y, top;
	char buf[64];

	setting_range(r->setting, &min, &max);
	h = clampi(max + 3, 3, H - 4);
	x = clampi(panes[rows[views[tab].focus].col < npanes ? rows[views[tab].focus].col : 0].x + 18,
		   0, W - w);
	y = (H - h) / 2;
	top = clampi(select_cur - (h - 3), 0, max);
	select_top = top;
	select_box = (struct pane){ x, y, w, h, 0, 0 };
	box(x, y, w, h, r->label);
	for (i = top; i <= max && i - top < h - 2; i++) {
		fmt_setting(r->setting, i, buf, sizeof(buf));
		term_fill(x + 1, y + 1 + i - top, w - 2, 1, ' ', C_TEXT, i == select_cur ? C_SEL : C_PANEL);
		text(x + 3, y + 1 + i - top, w - 5, i == select_cur ? C_BRIGHT : C_TEXT,
		     i == select_cur ? C_SEL : C_PANEL, 0, "%s", buf);
		if (i == select_cur)
			term_put(x + 1, y + 1 + i - top, G_MARK, C_ACCENT, C_SEL, 0);
		if (i == row_value(r))
			text(x + w - 4, y + 1 + i - top, 2, C_GREEN, i == select_cur ? C_SEL : C_PANEL,
			     0, "✓");
	}
}

static void draw_help(void)
{
	static const char *const lines[][2] = {
		{ "Moving", NULL },
		{ "1-5", "the five pages (also F1-F5)" },
		{ "\u2191 \u2193", "channel in the list, line in a form (also j k)" },
		{ "\u23ce tab esc", "into the form, next column; esc: up a level, to the pages" },
		{ "PgUp PgDn", "previous / next channel, staying on the same line" },
		{ "Changing", NULL },
		{ "\u2190 \u2192", "adjust; with shift (or , .): the smallest step" },
		{ "space \u23ce", "switch; \u23ce types a number or picks from a list" },
		{ "d", "back to the default; on a heading, channel or page: all of it" },
		{ "i s m", "route: invert, split a pair; m: mute this output" },
		{ "+ -", "Routing grid: gain of a cell; space: on/off, g: its output" },
		{ "u U", "undo, redo (also ^Z, ^R)" },
		{ "The unit", NULL },
		{ "a", "show what differs, and apply it" },
		{ "L", "live: every change goes to the unit as you make it" },
		{ "r", "start again from the state of the unit" },
		{ "\u25cf yellow", "this differs from what the unit has" },
		{ "The file", NULL },
		{ "^S W o q", "save, save under another name, open, quit" },
	};
	int n = sizeof(lines) / sizeof(*lines), w = clampi(W - 4, 40, 72), h = clampi(n + 3, 5, H - 2);
	int x = (W - w) / 2, y = (H - h) / 2, i;

	box(x, y, w, h, "Keys");
	for (i = 0; i < n && i < h - 3; i++) {
		if (!lines[i][1]) {
			text(x + 2, y + 1 + i, w - 4, C_ACCENT, C_PANEL, ATTR_BOLD, "%s", lines[i][0]);
			continue;
		}
		text(x + 4, y + 1 + i, 12, C_BRIGHT, C_PANEL, 0, "%s", lines[i][0]);
		text(x + 17, y + 1 + i, w - 19, C_DIM, C_PANEL, 0, "%s", lines[i][1]);
	}
	text(x + 2, y + h - 2, w - 4, C_FAINT, C_PANEL, 0,
	     "A file is the whole unit: what it leaves out is at its default.");
}

static void draw(void)
{
	int cy = 2, ch, lw, fx, i;

	term_size(&W, &H);
	term_unclip();
	term_fill(0, 0, W, H, ' ', C_TEXT, C_BG);
	term_cursor(-1, 0);
	if (W < 64 || H < 16) {
		text(1, 0, W - 1, C_YELLOW, C_BG, 0, "ucxtui needs 64 x 16 characters");
		term_flush();
		return;
	}
	resolve();
	ch = H - 4;
	draw_header();
	draw_tabs();
	npanes = 0;
	list_pane.w = 0;
	switch (tab) {
	case TAB_INPUTS:
	case TAB_OUTPUTS:
		lw = W >= 150 ? 44 : W >= 100 ? 38 : 32;
		draw_list(0, cy, lw, ch);
		for (i = 0; i < ch; i++)
			term_put(lw, cy + i, G_VLINE, C_LINE, C_BG, 0);
		fx = lw + 1;
		draw_strip_header(fx, cy, W - fx);
		layout_panes(fx, cy + 3, W - fx, ch - 3);
		break;
	case TAB_ROUTING:
		draw_routing(0, cy, W, ch);
		break;
	case TAB_EFFECTS:
		draw_effects_header(0, cy, W);
		layout_panes(0, cy + 3, W, ch - 3);
		break;
	case TAB_HARDWARE:
		layout_panes(0, cy + 1, W, ch - 1);
		break;
	}
	for (i = 0; i < npanes; i++)
		draw_pane(i);
	draw_status();
	draw_footer();
	switch (mode) {
	case M_PROMPT:
		draw_popup_input(prompt_kind == PR_SAVE ? "Save as" : "Open");
		break;
	case M_CONFIRM:
		draw_confirm();
		break;
	case M_APPLY:
		draw_apply();
		break;
	case M_SELECT:
		draw_select();
		break;
	case M_HELP:
		draw_help();
		break;
	default:
		break;
	}
	term_flush();
}

/* Actions */

static void apply_open(bool go_live)
{
	int unknown_off[UCX_MIX_OUTS] = { 0 };
	unsigned int i;
	int out;

	if (!dev) {
		say(MSG_WARN, "No unit on card %s: is it on and snd-fireface-ucx loaded?", card);
		return;
	}
	if (!device_sync())
		return;
	if (go_live && !plan.n) {
		live = true;
		say(MSG_OK, "Live: every change now goes to the unit as you make it");
		return;
	}
	free(apply_lines);
	apply_lines = calloc(plan.n + UCX_MIX_OUTS + 1, sizeof(*apply_lines));
	if (!apply_lines)
		abort();
	apply_n = apply_top = 0;
	/* Dangerous ones first; gains the driver never saw that stay off, one line per output */
	for (i = 0; i < plan.n; i++) {
		if (dangerous(&plan.ops[i], apply_lines[apply_n].text, sizeof(apply_lines->text)))
			apply_lines[apply_n++].danger = true;
	}
	for (i = 0; i < plan.n; i++) {
		const struct ucx_op *op = &plan.ops[i];

		if (op->kind == UCX_OP_MIX && op->old == UCX_UNKNOWN && op->new == UCX_MIX_OFF) {
			unknown_off[op->out]++;
			continue;
		}
		ucx_op_describe(op, apply_lines[apply_n++].text, sizeof(apply_lines->text));
	}
	for (out = 0; out < UCX_MIX_OUTS; out++)
		if (unknown_off[out])
			snprintf(apply_lines[apply_n++].text, sizeof(apply_lines->text),
				 "%s mix: %d gains the driver has not seen -> off",
				 ucx_output_name(out), unknown_off[out]);
	apply_live = go_live;
	mode = M_APPLY;
}

static void apply_run(bool push)
{
	mode = M_NORMAL;
	say(MSG_INFO, "Applying...");
	draw();
	if (device_apply(push) && apply_live) {
		live = true;
		say(MSG_OK, "Live: every change now goes to the unit as you make it");
	}
}

static void prompt_open(int kind)
{
	prompt_kind = kind;
	snprintf(input, sizeof(input), "%s", kind == PR_SAVE && path[0] ? path :
		 kind == PR_SAVE ? "ucx.json" : "");
	input_pos = strlen(input);
	mode = M_PROMPT;
}

static void save(bool ask)
{
	if (ask || !path[0])
		prompt_open(PR_SAVE);
	else if (file_save(path) && quit_after_save)
		quit = true;
}

static void ask_quit(void)
{
	if (!modified() || (!path[0] && from_device)) {
		quit = true;
		return;
	}
	snprintf(confirm_text[0], sizeof(confirm_text[0]), "%.150s has changes that are not saved.",
		 path[0] ? path : "This config");
	confirm_text[1][0] = confirm_text[2][0] = 0;
	confirm_open(CF_QUIT, "Quit", "sd", "Save and quit", "Discard", NULL);
}

/* Opening a file or reading the unit drops the config: ask if it has changes */
static void ask_replace(enum confirm kind)
{
	if (!modified()) {
		if (kind == CF_OPEN)
			prompt_open(PR_OPEN);
		else
			device_load();
		return;
	}
	snprintf(confirm_text[0], sizeof(confirm_text[0]), "The changes to %.150s are not saved.",
		 path[0] ? path : "this config");
	snprintf(confirm_text[1], sizeof(confirm_text[1]), kind == CF_OPEN ?
		 "Opening another file drops them." : "Reading the unit's state drops them.");
	confirm_text[2][0] = 0;
	confirm_open(kind, kind == CF_OPEN ? "Open" : "Read the unit", "y", "Drop the changes",
		     NULL, NULL);
}

static void confirm_done(char key)
{
	mode = M_NORMAL;
	switch (confirm_kind) {
	case CF_QUIT:
		if (key == 'd') {
			quit = true;
		} else if (key == 's') {
			quit_after_save = true;
			save(false);
		}
		break;
	case CF_DANGER:
		if (key == 'y') {
			device_apply(false);
		} else {
			doc_undo(&doc);
			diff();
			say(MSG_INFO, "Undone: nothing was sent");
		}
		break;
	case CF_OPEN:
		if (key == 'y')
			prompt_open(PR_OPEN);
		break;
	case CF_DEVICE:
		if (key == 'y')
			device_load();
		break;
	case CF_PAGE:
		if (key != 'y')
			break;
		doc_checkpoint(&doc, 0);
		page_defaults(tab, &doc.s);
		changed();
		say(MSG_OK, "%s is back at its defaults (u undoes it)", tab_names[tab]);
		break;
	}
}

/* d on the row of pages: all of the page, which is asked first */
static void page_reset_ask(void)
{
	int n = page_differs(tab);

	if (!n) {
		say(MSG_INFO, "%s is at its defaults already", tab_names[tab]);
		return;
	}
	snprintf(confirm_text[0], sizeof(confirm_text[0]), tab == TAB_ROUTING ?
		 "Every route of the mixer is switched off." :
		 "Everything on the %s page goes back to its default.", tab_names[tab]);
	snprintf(confirm_text[1], sizeof(confirm_text[1]), "%d value%s change%s. u undoes it.", n,
		 n == 1 ? "" : "s", n == 1 ? "s" : "");
	snprintf(confirm_text[2], sizeof(confirm_text[2]), "%s",
		 live ? "Live is on: the unit follows at once." : "");
	confirm_open(CF_PAGE, "Defaults for the whole page", "y", "Yes, reset it", NULL, NULL);
}

static void switch_tab(int t)
{
	tab = clampi(t, 0, TABS - 1);
	resolve();
}

/* The next line that can have the focus; the columns of a form follow each other */
static bool move_focus(int dir)
{
	int i;

	for (i = views[tab].focus + dir; i >= 0 && i < nrows; i += dir) {
		if (focusable(&rows[i])) {
			set_focus(i);
			return true;
		}
	}
	return false;
}

/* The first line from @i in direction @dir that can have the focus */
static void focus_from(int i, int dir)
{
	for (; i >= 0 && i < nrows; i += dir) {
		if (focusable(&rows[i])) {
			set_focus(i);
			return;
		}
	}
}

static void select_strip(int i)
{
	struct view *v = &views[tab];

	if (!has_list())
		return;
	i = clampi(i, 0, nstrips - 1);
	v->ch = strips[i].ch;
	resolve();
}

/* Into column @p of the form, at the line that had the focus there last */
static void enter_pane(int p)
{
	struct view *v = &views[tab];
	int i = clampi(v->pane_focus[p], panes[p].first, panes[p].first + panes[p].count - 1);

	v->in_list = false;
	focus_from(i, 1);
	if (v->focus < panes[p].first || v->focus >= panes[p].first + panes[p].count)
		focus_from(i, -1);
}

static int focus_pane(void)
{
	return npanes == 2 && views[tab].focus >= panes[1].first;
}

/* Tab: the list, then each column of the form */
static void next_pane(int dir)
{
	struct view *v = &views[tab];
	int n = npanes + has_list(), at;

	if (n < 2 || !nrows)
		return;
	at = v->in_list ? 0 : has_list() + focus_pane();
	at = (at + dir + n) % n;
	if (has_list() && at == 0)
		v->in_list = true;
	else
		enter_pane(at - has_list());
}

static void edit_begin(void)
{
	input[0] = 0;
	input_pos = 0;
	mode = M_EDIT;
}

static void key_form(const struct event *ev)
{
	struct view *v = &views[tab];
	const struct row *r = &rows[v->focus];
	bool fine = ev->mods & MOD_SHIFT;

	switch (ev->key) {
	case KEY_UP:
	case 'k':
		/* Above a form without a list is the row of pages */
		if (!move_focus(-1) && !has_list())
			in_tabs = true;
		break;
	case KEY_DOWN:
	case 'j':
		move_focus(1);
		break;
	case KEY_HOME:
		focus_from(0, 1);
		break;
	case KEY_END:
		focus_from(nrows - 1, -1);
		break;
	case KEY_LEFT:
	case 'h':
		row_adjust(r, -1, fine);
		break;
	case KEY_RIGHT:
	case 'l':
		row_adjust(r, 1, fine);
		break;
	case ',':
	case '<':
		row_adjust(r, -1, true);
		break;
	case '.':
	case '>':
		row_adjust(r, 1, true);
		break;
	case '-':
		row_adjust(r, -1, false);
		break;
	case '+':
	case '=':
		row_adjust(r, 1, false);
		break;
	case ' ':
		row_toggle(r);
		break;
	case KEY_ENTER:
		if (row_is_number(r)) {
			edit_begin();
		} else if (row_is_switch(r)) {
			row_toggle(r);
		} else if (has_value(r)) {
			select_cur = row_value(r);
			mode = M_SELECT;
		}
		break;
	case 'd':
	case KEY_DELETE:
	case KEY_BACKSPACE:
		/* A value, or on a heading all that is under it */
		if (r->kind == R_HEAD)
			section_reset(v->focus);
		else
			row_reset(r);
		break;
	case 'i':
		if (r->kind == R_MIX)
			mix_invert(r);
		break;
	case 's':
		if (r->kind == R_MIX)
			mix_split(r);
		break;
	case KEY_ESC:
		/* One level up: the list, or the row of pages */
		if (has_list())
			v->in_list = true;
		else
			in_tabs = true;
		break;
	}
}

static void key_routing(const struct event *ev)
{
	struct rcell c;
	const struct row *r = routing_row(&c);
	int dir = 0;
	bool fine = false;

	switch (ev->key) {
	case KEY_UP:
	case 'k':
		if (!rt_out)
			in_tabs = true;
		rt_out = clampi(rt_out - 1, 0, UCX_MIX_OUTS - 1);
		return;
	case KEY_ESC:
		in_tabs = true;
		return;
	case KEY_DOWN:
	case 'j':
		rt_out = clampi(rt_out + 1, 0, UCX_MIX_OUTS - 1);
		return;
	case KEY_LEFT:
	case 'h':
		rt_src = clampi(rt_src - 1, 0, UCX_SOURCES - 1);
		return;
	case KEY_RIGHT:
	case 'l':
		rt_src = clampi(rt_src + 1, 0, UCX_SOURCES - 1);
		return;
	case KEY_HOME:
		rt_src = 0;
		return;
	case KEY_END:
		rt_src = UCX_SOURCES - 1;
		return;
	case 'g':
		/* The same route in the form of its output, where its EQ and the rest are */
		tab = TAB_OUTPUTS;
		views[tab].ch = rt_out;
		views[tab].in_list = false;
		views[tab].id = (struct rowid){ R_MIX, NULL, rt_src };
		resolve();
		return;
	case ' ':
	case KEY_ENTER:
	case '+':
	case '=':
	case '-':
	case '.':
	case '>':
	case ',':
	case '<':
	case 'd':
	case KEY_DELETE:
	case KEY_BACKSPACE:
	case 'i':
	case 's':
		break;
	default:
		return;
	}
	switch (ev->key) {
	case ' ':
		row_toggle(r);
		break;
	case KEY_ENTER:
		edit_begin();
		break;
	case '.':
	case '>':
		fine = true;
		/* fall through */
	case '+':
	case '=':
		dir = 1;
		break;
	case ',':
	case '<':
		fine = true;
		/* fall through */
	case '-':
		dir = -1;
		break;
	case 'i':
		mix_invert(r);
		break;
	case 's':
		if (c.strip.n == 2)
			mix_split(r);
		else
			say(MSG_INFO, "%s is not linked: each of its cells is a route of its own",
			    ucx_output_name(rt_out));
		break;
	default:
		row_reset(r);
		break;
	}
	if (dir)
		row_adjust(r, dir, fine);
}

static void key_normal(const struct event *ev)
{
	struct view *v = &views[tab];
	int key = ev->key, set;

	if (ev->mods & MOD_CTRL) {
		switch (key) {
		case 's': save(false); break;
		case 'o': ask_replace(CF_OPEN); break;
		case 'z': key = 'u'; break;
		case 'r': case 'y': key = 'U'; break;
		case 'c': case 'q': ask_quit(); break;
		case 'l': term_size(&W, &H); break;
		}
		if (key != 'u' && key != 'U')
			return;
	}
	/* Alt with a digit is the digit: some terminals keep the plain ones */
	if (ev->mods & MOD_ALT && (key < '1' || key > '0' + TABS))
		return;

	switch (key) {
	case 'q':
		ask_quit();
		return;
	case '?':
		mode = M_HELP;
		return;
	case 'a':
		apply_open(false);
		return;
	case 'L':
		if (live) {
			live = false;
			say(MSG_INFO, "Live is off: changes wait until you apply them");
		} else {
			apply_open(true);
		}
		return;
	case 'u':
	case 'U':
		if (key == 'u' ? doc_undo(&doc) : doc_redo(&doc))
			changed();
		else
			say(MSG_INFO, key == 'u' ? "Nothing to undo" : "Nothing to redo");
		return;
	case 'w':
		save(false);
		return;
	case 'W':
		save(true);
		return;
	case 'o':
		ask_replace(CF_OPEN);
		return;
	case 'r':
		ask_replace(CF_DEVICE);
		return;
	case 'm':
		set = tab == TAB_OUTPUTS ? doc_setting(true, cur.ch, "mute") : -1;
		if (set >= 0) {
			doc_checkpoint(&doc, 0);
			doc_set(&doc, set, !doc.s.cfg.set[set]);
			changed();
		}
		return;
	case KEY_TAB:
		if (in_tabs)
			in_tabs = false;
		else
			next_pane(1);
		return;
	case KEY_BACKTAB:
		next_pane(-1);
		return;
	case KEY_PGUP:
		select_strip(sel - 1);
		return;
	case KEY_PGDN:
		select_strip(sel + 1);
		return;
	}
	if (key >= '1' && key <= '0' + TABS) {
		switch_tab(key - '1');
		return;
	}
	if (key >= KEY_F1 && key < KEY_F1 + TABS) {
		switch_tab(key - KEY_F1);
		return;
	}
	if (in_tabs) {
		switch (key) {
		case KEY_LEFT:
		case 'h':
			switch_tab(tab - 1);
			break;
		case KEY_RIGHT:
		case 'l':
			switch_tab(tab + 1);
			break;
		case KEY_DOWN:
		case 'j':
		case KEY_ENTER:
			in_tabs = false;
			break;
		case 'd':
		case KEY_DELETE:
		case KEY_BACKSPACE:
			page_reset_ask();
			break;
		}
		return;
	}
	if (tab == TAB_ROUTING) {
		key_routing(ev);
		return;
	}
	if (!v->in_list) {
		if (nrows)
			key_form(ev);
		return;
	}
	switch (key) {
	case KEY_UP:
	case 'k':
		/* Above the first channel is the row of pages */
		if (!sel)
			in_tabs = true;
		select_strip(sel - 1);
		break;
	case KEY_ESC:
		in_tabs = true;
		break;
	case 'd':
	case KEY_DELETE:
	case KEY_BACKSPACE:
		channel_reset();
		break;
	case KEY_DOWN:
	case 'j':
		select_strip(sel + 1);
		break;
	case KEY_HOME:
		select_strip(0);
		break;
	case KEY_END:
		select_strip(nstrips - 1);
		break;
	case KEY_ENTER:
	case KEY_RIGHT:
	case 'l':
		v->in_list = false;
		break;
	}
}

/* Typing into @input. True when the key was taken */
static bool key_input(const struct event *ev, bool number)
{
	int len = strlen(input), n;

	if (ev->mods & (MOD_CTRL | MOD_ALT))
		return false;
	switch (ev->key) {
	case KEY_BACKSPACE:
		if (!input_pos)
			return true;
		for (n = 1; input_pos - n > 0 && (input[input_pos - n] & 0xc0) == 0x80; n++)
			;
		memmove(input + input_pos - n, input + input_pos, len - input_pos + 1);
		input_pos -= n;
		return true;
	case KEY_DELETE:
		if (input_pos < len)
			memmove(input + input_pos, input + input_pos + 1, len - input_pos);
		return true;
	case KEY_LEFT:
		input_pos = clampi(input_pos - 1, 0, len);
		return true;
	case KEY_RIGHT:
		input_pos = clampi(input_pos + 1, 0, len);
		return true;
	case KEY_HOME:
		input_pos = 0;
		return true;
	case KEY_END:
		input_pos = len;
		return true;
	}
	/* File names are typed as ASCII here; a number needs nothing else */
	if (ev->key < 0x20 || ev->key > 0x7e || len >= (number ? 12 : (int)sizeof(input) - 2))
		return false;
	if (number && !strchr("0123456789.,-+of", ev->key))
		return true;
	memmove(input + input_pos + 1, input + input_pos, len - input_pos + 1);
	input[input_pos++] = ev->key;
	return true;
}

static void key_event(const struct event *ev)
{
	struct rcell cell;
	const struct row *r = tab == TAB_ROUTING ? routing_row(&cell) :
			      nrows ? &rows[views[tab].focus] : NULL;
	const char *p;
	int min, max;

	/* A key ends a drag: the slider stays where it is. And the mouse is out of use */
	drag.on = false;
	hover_x = hover_y = -1;
	switch (mode) {
	case M_NORMAL:
		key_normal(ev);
		break;
	case M_EDIT:
		if (ev->key == KEY_ESC) {
			mode = M_NORMAL;
		} else if (ev->key == KEY_ENTER) {
			if (!*input || row_enter(r, input))
				mode = M_NORMAL;
		} else if (tab != TAB_ROUTING && (ev->key == KEY_UP || ev->key == KEY_DOWN)) {
			/* Leaving the line takes what was typed */
			if (!*input || row_enter(r, input)) {
				mode = M_NORMAL;
				key_normal(ev);
			}
		} else {
			key_input(ev, true);
		}
		break;
	case M_PROMPT:
		if (ev->key == KEY_ESC) {
			mode = M_NORMAL;
			quit_after_save = false;
		} else if (ev->key == KEY_ENTER && *input) {
			mode = M_NORMAL;
			if (prompt_kind == PR_OPEN)
				file_open(input);
			else if (file_save(input) && quit_after_save)
				quit = true;
			quit_after_save = false;
		} else {
			key_input(ev, false);
		}
		break;
	case M_CONFIRM:
		if (ev->key == KEY_ESC && confirm_kind == CF_DANGER) {
			confirm_done('n');
		} else if (ev->key == KEY_ESC) {
			mode = M_NORMAL;
		} else if (ev->key < 0x80 && ev->key > 0 && (p = strchr(confirm_keys, ev->key)) &&
			   confirm_labels[p - confirm_keys]) {
			confirm_done(ev->key);
		}
		break;
	case M_APPLY:
		if (ev->key == KEY_ESC || ev->key == 'q')
			mode = M_NORMAL;
		else if (ev->key == KEY_ENTER)
			apply_run(false);
		else if (ev->key == 'p')
			apply_run(true);
		else if (ev->key == KEY_UP || ev->key == 'k')
			apply_top--;
		else if (ev->key == KEY_DOWN || ev->key == 'j')
			apply_top++;
		else if (ev->key == KEY_PGUP)
			apply_top -= 10;
		else if (ev->key == KEY_PGDN || ev->key == ' ')
			apply_top += 10;
		break;
	case M_SELECT:
		setting_range(r->setting, &min, &max);
		if (ev->key == KEY_ESC)
			mode = M_NORMAL;
		else if (ev->key == KEY_UP || ev->key == 'k')
			select_cur = clampi(select_cur - 1, min, max);
		else if (ev->key == KEY_DOWN || ev->key == 'j')
			select_cur = clampi(select_cur + 1, min, max);
		else if (ev->key == KEY_HOME)
			select_cur = min;
		else if (ev->key == KEY_END)
			select_cur = max;
		else if (ev->key == KEY_ENTER || ev->key == ' ') {
			mode = M_NORMAL;
			row_set(r, select_cur);
		}
		break;
	case M_HELP:
		mode = M_NORMAL;
		break;
	}
}

static bool inside(const struct pane *p, int x, int y)
{
	return p->w && x >= p->x && x < p->x + p->w && y >= p->y && y < p->y + p->h;
}

/* A press on line @i of a form, which has the focus if @again */
static void mouse_row(const struct pane *p, int i, int x, bool again)
{
	const struct row *r = &rows[i];
	int cx, sw, vx, n;
	char buf[64];

	row_span(p, &cx, &sw, &vx);
	views[tab].in_list = false;
	set_focus(i);
	if (r->kind == R_HEAD) {
		if (r->setting >= 0 && (again || x >= p->x + p->w - 8))
			row_toggle(r);
	} else if (row_is_switch(r)) {
		if (again || (x >= cx && x < cx + 5))
			row_toggle(r);
	} else if (row_is_number(r)) {
		if (sw >= 6 && x >= cx && x < cx + sw)
			/* Held, not moved: a press alone never changes a value */
			drag.on = true, drag.row = i, drag.x0 = x, drag.v0 = row_value(r), drag.sw = sw,
			drag.f0 = row_frac(r);
		else if (x >= vx)
			edit_begin();
		else if (again && r->kind == R_MIX)
			row_toggle(r);
	} else {
		/* One of a list: the arrows step through it, the name opens it */
		fmt_row(r, row_value(r), buf, sizeof(buf));
		n = term_width(buf);
		if (x >= cx - 1 && x <= cx + 1) {
			row_adjust(r, -1, false);
		} else if (x >= cx + n + 2 && x <= cx + n + 4) {
			row_adjust(r, 1, false);
		} else if (again || (x >= cx + 2 && x < cx + n + 2)) {
			select_cur = row_value(r);
			mode = M_SELECT;
		}
	}
}

static void mouse_event(const struct event *ev)
{
	struct view *v = &views[tab];
	const struct row *r;
	int t, p, i, dx, dir = ev->button;

	hover_x = ev->x;
	hover_y = ev->y;
	if (ev->key == KEY_MOVE) {
		/* In a list that is open, the mouse carries the selection as in a menu */
		if (mode == M_SELECT && inside(&select_box, ev->x, ev->y) && ev->y > select_box.y &&
		    ev->y < select_box.y + select_box.h - 1)
			select_cur = select_top + ev->y - select_box.y - 1;
		return;
	}
	if (ev->key == KEY_RELEASE) {
		drag.on = false;
		return;
	}
	if (ev->key == KEY_DRAG) {
		if (!drag.on || mode != M_NORMAL || v->in_list || drag.row != v->focus ||
		    drag.row >= nrows || !row_is_number(&rows[drag.row]))
			return;
		/* By as much as the mouse moved since the press; back where it began, as it was */
		r = &rows[drag.row];
		dx = ev->x - drag.x0;
		row_set(r, dx ? row_at_frac(r, drag.f0 + (double)dx / (drag.sw - 1)) : drag.v0);
		return;
	}
	drag.on = false;
	if (ev->key == KEY_MOUSE && ev->button != 0)
		return;
	switch (mode) {
	case M_NORMAL:
		break;
	case M_APPLY:
		if (ev->key == KEY_WHEEL)
			apply_top += 3 * dir;
		return;
	case M_SELECT:
		r = &rows[v->focus];
		if (ev->key == KEY_WHEEL) {
			setting_range(r->setting, &i, &p);
			select_cur = clampi(select_cur + dir, i, p);
			return;
		}
		mode = M_NORMAL;
		if (inside(&select_box, ev->x, ev->y) && ev->y > select_box.y &&
		    ev->y < select_box.y + select_box.h - 1)
			row_set(r, select_top + ev->y - select_box.y - 1);
		return;
	case M_HELP:
		if (ev->key == KEY_MOUSE)
			mode = M_NORMAL;
		return;
	default:
		return;
	}
	if (ev->key == KEY_MOUSE && ev->y == 1) {
		/* A click on a page selects it, as one on a channel or a line does */
		for (t = 0; t < TABS; t++) {
			if (ev->x >= tab_x[t] && ev->x < tab_x[t + 1]) {
				switch_tab(t);
				in_tabs = true;
			}
		}
		return;
	}
	in_tabs = false;
	if (tab == TAB_ROUTING) {
		if (ev->key == KEY_WHEEL) {
			rt_out = clampi(rt_out + dir, 0, UCX_MIX_OUTS - 1);
			return;
		}
		if (ev->y < rt_y || ev->y >= rt_y + rt_rows)
			return;
		for (i = UCX_SOURCES - 1; i >= 0; i--) {
			if (ev->x < routing_x(i) || ev->x >= routing_x(i) + rt_cw)
				continue;
			/* A click moves the cursor; a click on the cursor switches the route */
			if (rt_src == i && rt_out == rt_top + ev->y - rt_y) {
				key_routing(&(struct event){ .key = ' ' });
			} else {
				rt_src = i;
				rt_out = rt_top + ev->y - rt_y;
			}
			break;
		}
		return;
	}
	if (inside(&list_pane, ev->x, ev->y)) {
		i = v->list_top + ev->y - list_pane.y;
		if (ev->key == KEY_WHEEL) {
			v->in_list = true;
			select_strip(sel + dir);
		} else if (i < nstrips) {
			/* A click on the selected channel goes into its form */
			v->in_list = !(v->in_list && i == sel);
			select_strip(i);
		}
		return;
	}
	for (p = 0; p < npanes; p++) {
		if (!inside(&panes[p], ev->x, ev->y))
			continue;
		if (ev->key == KEY_WHEEL) {
			/* The wheel moves the focus: it never changes a value */
			if (v->in_list || focus_pane() != p)
				enter_pane(p);
			else
				move_focus(dir);
			return;
		}
		i = panes[p].first + v->scroll[p] + ev->y - panes[p].y;
		if (i < panes[p].first + panes[p].count && focusable(&rows[i]))
			mouse_row(&panes[p], i, ev->x, !v->in_list && i == v->focus);
		return;
	}
}

static const char usage[] =
	"usage: ucxtui [-c CARD] [FILE]\n"
	"\n"
	"Edit a Fireface UCX config (docs/CONFIG.md) on screen and send it to the unit.\n"
	"With FILE: edit that file; it is created when you save if it does not exist.\n"
	"Without: your own config, $XDG_CONFIG_HOME/ucx/config.json\n"
	"(~/.config/ucx/config.json), which is applied whenever the unit appears;\n"
	"if it doesn't exist yet, start from what the driver knows of the unit.\n"
	"Nothing is sent to the unit until you apply (a) or go live (L).\n"
	"\n"
	"  -c, --card NAME   ALSA card (default UCX)\n"
	"  -h, --help\n"
	"\n"
	"Press ? in the program for the keys.\n";

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "card", required_argument, NULL, 'c' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct ucx_config cfg;
	struct event ev;
	char err[512], own[PATH_MAX];
	const char *name;
	int opt, null;

	while ((opt = getopt_long(argc, argv, "c:h", options, NULL)) != -1) {
		switch (opt) {
		case 'c':
			card = optarg;
			break;
		case 'h':
			fputs(usage, stdout);
			return 0;
		default:
			fputs(usage, stderr);
			return 2;
		}
	}
	if (argc - optind > 1) {
		fputs(usage, stderr);
		return 2;
	}

	/* Without FILE: the user's own config, the one applied when the unit appears */
	name = optind < argc ? argv[optind] :
	       !ucx_config_default_path(own, sizeof(own)) ? own : NULL;

	ucx_config_defaults(&cfg);
	doc_replace(&cfg);
	device_open();
	if (name && !access(name, F_OK)) {
		/* A file that is not valid is said before the screen is taken */
		if (ucx_config_load(name, &cfg, err, sizeof(err))) {
			fprintf(stderr, "ucxtui: %s\n", err);
			return 1;
		}
		file_open(name);
		message[0] = 0;
	} else {
		if (dev)
			device_load();
		if (name) {
			snprintf(path, sizeof(path), "%s", name);
			from_device = false;
			if (name == own && dev)
				say(MSG_INFO, "This is the unit's state; saving writes your config, "
				    "%s, which is applied whenever the unit appears", own);
			else if (name == own)
				say(MSG_WARN, "No unit on card %s: a new config for %s", card, own);
			else if (!message[0] || message_kind < MSG_WARN)
				say(MSG_INFO, "A new file: it is written when you save");
		} else if (!dev) {
			say(MSG_WARN, "No unit on card %s: a new config, everything at its default",
			    card);
		}
	}
	if (term_init()) {
		fprintf(stderr, "ucxtui: needs a terminal\n");
		return 2;
	}
	/*
	 * alsa-lib prints its own errors (a card that is not there, every time
	 * it is looked for), and they would land in the middle of the screen
	 */
	null = open("/dev/null", O_WRONLY | O_CLOEXEC);
	if (null >= 0)
		dup2(null, STDERR_FILENO);

	while (!quit) {
		draw();
		if (term_poll(&ev, dev ? 60 : 500)) {
			/* All that is waiting, so that a held key doesn't lag behind */
			do {
				resolve();
				if (ev.key >= KEY_MOUSE && ev.key <= KEY_WHEEL)
					mouse_event(&ev);
				else if (ev.key == KEY_FOCUS_OUT)
					hover_x = hover_y = -1;
				else if (ev.key != KEY_RESIZE)
					key_event(&ev);
			} while (!quit && mode != M_APPLY && term_poll(&ev, 0));
			if (live && live_dirty && mode == M_NORMAL)
				live_apply();
		}
		device_tick();
	}
	term_exit();
	ucx_dev_close(dev);
	return 0;
}
