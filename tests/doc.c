// SPDX-License-Identifier: MIT
/*
 * ucxtui's edits (tui/doc.c), without a terminal or a device: after any
 * sequence of them, the config written as a file must load back as the same
 * config. That is what makes the screen and the saved file agree: a linked
 * pair is one entry in the file, and can only say what doc.c lets it hold.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc.h"

static struct doc doc;
static unsigned int seed = 1;

static int rnd(int n)
{
	seed = seed * 1103515245u + 12345u;
	return (seed >> 16) % n;
}

static int random_value(int setting)
{
	const struct ucx_setting *s = &ucx_settings[setting];

	switch (s->type) {
	case UCX_T_MIC_GAIN:
		return rnd(4) ? 10 + rnd(56) : 0;
	case UCX_T_INST_GAIN:
		return rnd(25);
	case UCX_T_BOOL:
		return rnd(2);
	case UCX_T_IN_LEVEL:
	case UCX_T_OUT_LEVEL:
		return rnd(3);
	case UCX_T_ENUM:
		return rnd(s->nitems);
	case UCX_T_NUM:
		return s->min + rnd(s->max - s->min + 1);
	}
	return 0;
}

static int random_gain(void)
{
	return rnd(3) ? UCX_GAIN_MIN + rnd(UCX_GAIN_MAX - UCX_GAIN_MIN + 1) : UCX_MIX_OFF;
}

static void random_edit(void)
{
	struct strip strips[UCX_MIX_OUTS], *strip;
	struct mixrow rows[UCX_SOURCES], *row;
	bool output = rnd(2);
	int n, setting;

	n = doc_strips(&doc, output, strips);
	strip = &strips[rnd(n)];
	doc_checkpoint(&doc, rnd(3));
	switch (rnd(8)) {
	case 0:
		if (strip->output && strip->ch < UCX_OUTPUTS)
			doc_set_volume(&doc, strip->ch, UCX_VOL_MIN + rnd(UCX_VOL_MAX - UCX_VOL_MIN + 1));
		break;
	case 1:
		doc_set_link(&doc, strip->output, strip->ch / 2, rnd(2));
		break;
	case 2:
	case 3:
		/* Any setting, also of the other side and of the unit */
		setting = rnd(UCX_SETTINGS);
		doc_set(&doc, setting, random_value(setting));
		break;
	case 4:
		if (doc_undo(&doc) && rnd(2))
			doc_redo(&doc);
		break;
	default:
		if (!strip->output)
			break;
		row = &rows[rnd(doc_mix_rows(&doc, strip, rows))];
		if (rnd(4))
			doc_mix_set(&doc, strip, row, random_gain(), rnd(2));
		else
			doc_mix_split(&doc, strip, row);
		break;
	}
}

/* Returns what differs, NULL if the config survives being written and read */
static const char *round_trip(void)
{
	static char err[512];
	struct ucx_config back;
	char *text = ucx_config_format(&doc.s.cfg, 0);
	const char *what = NULL;
	int i;

	if (!text)
		return "out of memory";
	if (ucx_config_parse(text, strlen(text), "test", &back, err, sizeof(err)))
		what = err;
	else if (memcmp(back.volume, doc.s.cfg.volume, sizeof(back.volume)))
		what = "a volume";
	else if (memcmp(back.mix, doc.s.cfg.mix, sizeof(back.mix)))
		what = "a mixer gain";
	else if (memcmp(back.invert, doc.s.cfg.invert, sizeof(back.invert)))
		what = "the phase of a route";
	else if (memcmp(back.link_out, doc.s.cfg.link_out, sizeof(back.link_out)) ||
		 memcmp(back.link_in, doc.s.cfg.link_in, sizeof(back.link_in)))
		what = "a stereo link";
	for (i = 0; !what && i < UCX_SETTINGS; i++) {
		if (back.set[i] != doc.s.cfg.set[i]) {
			ucx_setting_name(i, err, sizeof(err));
			what = err;
		}
	}
	if (what)
		fprintf(stderr, "%s\n", text);
	free(text);
	return what;
}

/* A device state with unknown values and a linked pair that one entry can't say */
static int test_load(void)
{
	struct ucx_config cfg;
	int unknown;

	ucx_config_defaults(&cfg);
	cfg.volume[3] = UCX_UNKNOWN;
	cfg.mix[0][5] = UCX_UNKNOWN;
	cfg.set[doc_setting(false, 0, "gain")] = UCX_UNKNOWN;
	cfg.link_in[4] = UCX_UNKNOWN;
	/* Line 5/6 out: linked, but a route to the left side only */
	cfg.mix[4][0] = -600;
	/* ADAT 1/2 in: linked, with different settings */
	cfg.set[doc_setting(false, 10, "fx-send")] = 100;
	/*
	 * The driver's extremes of a mix gain (linear 65536 = TotalMix's +6.02
	 * dB, and 1 = -90.3 dB) must come out as gains a file can say
	 */
	cfg.mix[10][0] = ucx_gain_from_linear(65536);
	cfg.mix[10][1] = ucx_gain_from_linear(1);
	unknown = doc_load(&doc, &cfg);
	if (unknown != 4 || ucx_config_has_unknown(&doc.s.cfg) || doc.s.cfg.link_out[2] ||
	    doc.s.cfg.link_in[5] || !doc.s.cfg.link_out[0] || round_trip()) {
		fprintf(stderr, "FAIL doc load: %d unknown, links %d %d %d\n", unknown,
			doc.s.cfg.link_out[2], doc.s.cfg.link_in[5], doc.s.cfg.link_out[0]);
		return 1;
	}
	return 0;
}

int main(void)
{
	struct ucx_config cfg;
	struct strip phones = { true, 6, 2 };
	struct mixrow rows[UCX_SOURCES], pcm = { 0, 2 };
	const char *what;
	int i, failed;

	failed = test_load();

	/* A stereo route of a pair is two gains, and can't be split while it is on */
	ucx_config_defaults(&cfg);
	doc_load(&doc, &cfg);
	doc_mix_set(&doc, &phones, &pcm, -300, 0);
	if (doc.s.cfg.mix[6][0] != -300 || doc.s.cfg.mix[7][1] != -300 ||
	    doc.s.cfg.mix[6][1] != UCX_MIX_OFF || doc_mix_split(&doc, &phones, &pcm) ||
	    doc_mix_rows(&doc, &phones, rows) != 9 + 2 + 2 + 7) {
		fprintf(stderr, "FAIL doc stereo route\n");
		failed = 1;
	}
	/* Unlinked, the right side takes its own volume; linked again, the left one's */
	doc_set_volume(&doc, 6, -200);
	doc_set_link(&doc, true, 3, false);
	doc_set_volume(&doc, 7, -100);
	doc_mix_set(&doc, &(struct strip){ true, 7, 1 }, &(struct mixrow){ 0, 1 }, -1200, 1);
	doc_set_link(&doc, true, 3, true);
	if (doc.s.cfg.volume[7] != -200 || round_trip()) {
		fprintf(stderr, "FAIL doc relink\n");
		failed = 1;
	}

	for (i = 0; i < 20000 && !failed; i++) {
		random_edit();
		what = i % 7 ? NULL : round_trip();
		if (what) {
			fprintf(stderr, "FAIL doc round trip after %d edits: %s differs\n", i + 1, what);
			failed = 1;
		}
	}
	if (!failed)
		puts("doc tests passed");
	return failed;
}
