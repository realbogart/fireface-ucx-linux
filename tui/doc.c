// SPDX-License-Identifier: MIT
/* The config being edited in ucxtui; see doc.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "doc.h"

/* The same setting on the other channel of the pair, -1 for those of the unit */
static int peers[UCX_SETTINGS];
static bool peers_ready;

int doc_setting(bool output, int ch, const char *key)
{
	int i;

	for (i = 0; i < UCX_SETTINGS; i++)
		if (ucx_settings[i].output == output && ucx_settings[i].ch == ch &&
		    !strcmp(ucx_settings[i].key, key))
			return i;
	return -1;
}

int doc_global(const char *key)
{
	return doc_setting(false, -1, key);
}

static int peer(int setting)
{
	const struct ucx_setting *s;
	int i;

	if (!peers_ready) {
		for (i = 0; i < UCX_SETTINGS; i++) {
			s = &ucx_settings[i];
			peers[i] = s->ch < 0 ? -1 : doc_setting(s->output, s->ch ^ 1, s->key);
		}
		peers_ready = true;
	}
	return peers[setting];
}

static bool off(const struct ucx_config *cfg, int out, int src)
{
	return cfg->mix[out][src] == UCX_MIX_OFF;
}

static bool same(const struct ucx_config *cfg, int o1, int s1, int o2, int s2)
{
	return cfg->mix[o1][s1] == cfg->mix[o2][s2] && cfg->invert[o1][s1] == cfg->invert[o2][s2];
}

static void route(struct ucx_config *cfg, int out, int src, int gain, int invert)
{
	cfg->mix[out][src] = gain;
	cfg->invert[out][src] = gain == UCX_MIX_OFF ? 0 : !!invert;
}

/* Left to left and right to right at one gain, nothing across (or all off) */
static bool stereo(const struct ucx_config *cfg, int out, int s)
{
	return same(cfg, out, s, out + 1, s + 1) && off(cfg, out, s + 1) && off(cfg, out + 1, s);
}

/* Each source at one gain to both outputs (or all off) */
static bool monos(const struct ucx_config *cfg, int out, int s)
{
	return same(cfg, out, s, out + 1, s) && same(cfg, out, s + 1, out + 1, s + 1);
}

static bool mix_pairable(const struct ucx_config *cfg, int out)
{
	int s;

	for (s = 0; s < UCX_SOURCES; s += 2)
		if (!stereo(cfg, out, s) && !monos(cfg, out, s))
			return false;
	return true;
}

static bool settings_equal(const struct ucx_config *cfg, bool output, int ch)
{
	int i;

	for (i = 0; i < UCX_SETTINGS; i++)
		if (ucx_settings[i].output == output && ucx_settings[i].ch == ch && peer(i) >= 0 &&
		    cfg->set[i] != cfg->set[peer(i)])
			return false;
	return true;
}

int doc_load(struct doc *doc, const struct ucx_config *config)
{
	struct ucx_config *cfg = &doc->s.cfg, def;
	int out, src, i, unknown = 0;

	if (!doc->history)
		doc->history = malloc(sizeof(*doc->history) * (DOC_HISTORY + 1));
	if (!doc->history)
		abort();
	doc->pos = doc->top = 0;
	doc->last_tag = 0;

	*cfg = *config;
	ucx_config_defaults(&def);
	for (out = 0; out < UCX_MIX_OUTS; out++) {
		if (out < UCX_OUTPUTS && cfg->volume[out] == UCX_UNKNOWN) {
			cfg->volume[out] = def.volume[out];
			unknown++;
		}
		for (src = 0; src < UCX_SOURCES; src++) {
			if (cfg->mix[out][src] == UCX_UNKNOWN) {
				cfg->mix[out][src] = UCX_MIX_OFF;
				unknown++;
			}
			if (cfg->mix[out][src] == UCX_MIX_OFF)
				cfg->invert[out][src] = 0;
		}
	}
	for (i = 0; i < UCX_SETTINGS; i++) {
		if (cfg->set[i] == UCX_UNKNOWN) {
			cfg->set[i] = def.set[i];
			unknown++;
		}
	}
	for (i = 0; i < UCX_PAIRS; i++) {
		if (cfg->link_out[i] == UCX_UNKNOWN) {
			cfg->link_out[i] = def.link_out[i];
			unknown++;
		}
		if (cfg->link_in[i] == UCX_UNKNOWN) {
			cfg->link_in[i] = def.link_in[i];
			unknown++;
		}
		if (cfg->volume[2 * i] != cfg->volume[2 * i + 1] || !settings_equal(cfg, true, 2 * i) ||
		    !mix_pairable(cfg, 2 * i))
			cfg->link_out[i] = 0;
		if (!settings_equal(cfg, false, 2 * i))
			cfg->link_in[i] = 0;
	}
	doc->s.fx_link = mix_pairable(cfg, UCX_OUTPUTS);
	/* Sources that are off show the way the source itself is linked */
	for (out = 0; out < UCX_MIX_OUTS; out++)
		for (i = 0; i < DOC_SRC_PAIRS; i++)
			doc->s.split[out][i] = 2 * i >= UCX_SRC_INPUT &&
				!cfg->link_in[i - UCX_SRC_INPUT / 2];
	return unknown;
}

bool doc_linked(const struct doc *doc, bool output, int pair)
{
	if (!output)
		return doc->s.cfg.link_in[pair];
	return pair == DOC_FX_PAIR ? doc->s.fx_link : doc->s.cfg.link_out[pair];
}

int doc_strips(const struct doc *doc, bool output, struct strip *strips)
{
	int pair, n = 0;

	for (pair = 0; pair < (output ? UCX_PAIRS + 1 : UCX_PAIRS); pair++) {
		if (doc_linked(doc, output, pair)) {
			strips[n++] = (struct strip){ output, 2 * pair, 2 };
		} else {
			strips[n++] = (struct strip){ output, 2 * pair, 1 };
			strips[n++] = (struct strip){ output, 2 * pair + 1, 1 };
		}
	}
	return n;
}

void doc_strip_name(const struct strip *strip, char *buf, size_t len)
{
	if (strip->n == 2 && strip->output && strip->ch >= UCX_OUTPUTS)
		snprintf(buf, len, "FX Send L/R");
	else if (strip->n == 2)
		ucx_pair_name(strip->output, strip->ch / 2, buf, len);
	else
		snprintf(buf, len, "%s", strip->output ? ucx_output_name(strip->ch) :
							 ucx_input_name(strip->ch));
}

/* History */

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

void doc_checkpoint(struct doc *doc, long tag)
{
	long long now = now_ms();
	bool merge = tag && tag == doc->last_tag && now - doc->last_time < 1000;

	doc->last_tag = tag;
	doc->last_time = now;
	if (merge)
		return;
	if (doc->pos == DOC_HISTORY) {
		memmove(doc->history, doc->history + 1, sizeof(*doc->history) * (DOC_HISTORY - 1));
		doc->pos--;
	}
	doc->history[doc->pos++] = doc->s;
	doc->top = doc->pos;
}

bool doc_undo(struct doc *doc)
{
	if (!doc->pos)
		return false;
	doc->history[doc->pos] = doc->s;
	doc->s = doc->history[--doc->pos];
	doc->last_tag = 0;
	return true;
}

bool doc_redo(struct doc *doc)
{
	if (doc->pos >= doc->top)
		return false;
	doc->s = doc->history[++doc->pos];
	doc->last_tag = 0;
	return true;
}

/* Edits */

void doc_set(struct doc *doc, int setting, int value)
{
	const struct ucx_setting *s = &ucx_settings[setting];

	doc->s.cfg.set[setting] = value;
	if (s->ch >= 0 && doc_linked(doc, s->output, s->ch / 2) && peer(setting) >= 0)
		doc->s.cfg.set[peer(setting)] = value;
}

void doc_set_volume(struct doc *doc, int out, int value)
{
	doc->s.cfg.volume[out] = value;
	if (doc->s.cfg.link_out[out / 2])
		doc->s.cfg.volume[out ^ 1] = value;
}

/*
 * Make the mix of the pair from @out one that a pair entry can say, keeping
 * what it has where possible: a route to one side only goes to both.
 */
static void mix_pair_up(struct ucx_config *cfg, int out)
{
	int s, i, from;

	for (s = 0; s < UCX_SOURCES; s += 2) {
		if (stereo(cfg, out, s) || monos(cfg, out, s))
			continue;
		if (off(cfg, out, s + 1) && off(cfg, out + 1, s)) {
			/* Straight, but the two sides differ: the left one counts */
			if (off(cfg, out, s))
				route(cfg, out, s, cfg->mix[out + 1][s + 1], cfg->invert[out + 1][s + 1]);
			else
				route(cfg, out + 1, s + 1, cfg->mix[out][s], cfg->invert[out][s]);
			continue;
		}
		for (i = s; i < s + 2; i++) {
			from = off(cfg, out, i) ? out + 1 : out;
			route(cfg, out, i, cfg->mix[from][i], cfg->invert[from][i]);
			route(cfg, out + 1, i, cfg->mix[from][i], cfg->invert[from][i]);
		}
	}
}

void doc_set_link(struct doc *doc, bool output, int pair, bool on)
{
	struct ucx_config *cfg = &doc->s.cfg;
	int i;

	if (output && pair == DOC_FX_PAIR) {
		doc->s.fx_link = on;
		if (on)
			mix_pair_up(cfg, UCX_OUTPUTS);
		return;
	}
	*(output ? &cfg->link_out[pair] : &cfg->link_in[pair]) = on;
	if (!on)
		return;
	for (i = 0; i < UCX_SETTINGS; i++)
		if (ucx_settings[i].output == output && ucx_settings[i].ch == 2 * pair &&
		    peer(i) >= 0)
			cfg->set[peer(i)] = cfg->set[i];
	if (output) {
		cfg->volume[2 * pair + 1] = cfg->volume[2 * pair];
		mix_pair_up(cfg, 2 * pair);
	}
}

/* The mix */

/* Is source pair @s one row in the mix of @strip? */
static bool row_paired(const struct doc *doc, const struct strip *strip, int s)
{
	const struct ucx_config *cfg = &doc->s.cfg;
	bool split = doc->s.split[strip->ch][s / 2];

	if (strip->n == 2)
		return stereo(cfg, strip->ch, s) && !(split && monos(cfg, strip->ch, s));
	return same(cfg, strip->ch, s, strip->ch, s + 1) && !split;
}

int doc_mix_rows(const struct doc *doc, const struct strip *strip, struct mixrow *rows)
{
	int s, n = 0;

	for (s = 0; s < UCX_SOURCES; s += 2) {
		if (row_paired(doc, strip, s)) {
			rows[n++] = (struct mixrow){ s, 2 };
		} else {
			rows[n++] = (struct mixrow){ s, 1 };
			rows[n++] = (struct mixrow){ s + 1, 1 };
		}
	}
	return n;
}

int doc_mix_get(const struct doc *doc, const struct strip *strip, const struct mixrow *row,
		int *invert)
{
	if (invert)
		*invert = doc->s.cfg.invert[strip->ch][row->src];
	return doc->s.cfg.mix[strip->ch][row->src];
}

void doc_mix_set(struct doc *doc, const struct strip *strip, const struct mixrow *row, int gain,
		 int invert)
{
	struct ucx_config *cfg = &doc->s.cfg;
	int out = strip->ch, src = row->src, o, s;

	/* The row stays the kind it is, also when the gains would now fit the other */
	doc->s.split[out][src / 2] = row->n == 1;
	if (strip->n == 2 && row->n == 2) {
		route(cfg, out, src, gain, invert);
		route(cfg, out + 1, src + 1, gain, invert);
		route(cfg, out, src + 1, UCX_MIX_OFF, 0);
		route(cfg, out + 1, src, UCX_MIX_OFF, 0);
		return;
	}
	for (o = out; o < out + strip->n; o++)
		for (s = src; s < src + row->n; s++)
			route(cfg, o, s, gain, invert);
}

bool doc_mix_split(struct doc *doc, const struct strip *strip, const struct mixrow *row)
{
	const struct ucx_config *cfg = &doc->s.cfg;
	int s = row->src & ~1;
	bool *split = &doc->s.split[strip->ch][s / 2];

	if (strip->n == 2 ? !(stereo(cfg, strip->ch, s) && monos(cfg, strip->ch, s)) :
			    !same(cfg, strip->ch, s, strip->ch, s + 1))
		return false;
	*split = row->n == 2;
	return true;
}
