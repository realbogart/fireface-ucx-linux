// SPDX-License-Identifier: MIT
/* Replay the apply plan one operation at a time. Every prefix is a possible
 * stopping point after a write error. The hardware matrix can start out of
 * sync with the cache, as it can after a module reload. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxcfg.h"

static struct ucx_plan plan;
static struct ucx_dev_state cache;
static struct ucx_config want;
static int matrix[UCX_MIX_OUTS][UCX_SOURCES]; /* actual signed linear gains */
enum { phones = 6 };
static int mic_gain, mute, mute_enable, phones_level;

static int setting(const char *ctl)
{
	for (int i = 0; i < UCX_SETTINGS; i++)
		if (!strcmp(ucx_settings[i].ctl, ctl))
			return i;
	abort();
}

static void reset(void)
{
	ucx_config_defaults(&cache.cfg);
	memset(cache.linear, 0, sizeof(cache.linear));
	memset(matrix, 0, sizeof(matrix));
	cache.cfg.link_out[phones / 2] = 0;
	cache.cfg.volume[phones] = -200;
	cache.cfg.set[mic_gain] = 10;
	cache.cfg.mix[phones][UCX_SRC_INPUT] = 0;
	cache.linear[phones][UCX_SRC_INPUT] = 32768;
	matrix[phones][UCX_SRC_INPUT] = 32768;
	want = cache.cfg;
}

static void apply_op(const struct ucx_op *op)
{
	int out = op->out, src = op->src;

	switch (op->kind) {
	case UCX_OP_PUSH_MIX:
		matrix[out][src] = op->new;
		cache.linear[out][src] = abs(op->new);
		cache.cfg.mix[out][src] = ucx_gain_from_linear(abs(op->new));
		cache.cfg.invert[out][src] = op->new < 0;
		break;
	case UCX_OP_VOLUME:
		cache.cfg.volume[out] = op->new;
		break;
	case UCX_OP_SETTING:
		cache.cfg.set[op->setting] = op->new;
		break;
	default:
		assert(!"unexpected operation");
	}
}

static void check_monitor(void)
{
	if (cache.cfg.set[mute_enable] && !cache.cfg.set[mute])
		return;
	if (matrix[phones][UCX_SRC_INPUT])
		assert(cache.cfg.set[mic_gain] <= 10);
}

/* Both directions, including starting or finishing with globally ignored mutes. */
static void test_mutes(bool reverse, bool toggle_global)
{
	reset();
	cache.cfg.set[mic_gain] = reverse ? 40 : 10;
	cache.cfg.set[mute] = reverse ? 0 : 1; /* Playback Switch */
	cache.cfg.set[mute_enable] = reverse || !toggle_global;
	want = cache.cfg;
	want.set[mic_gain] = reverse ? 10 : 40;
	want.set[mute] = reverse ? 1 : 0;
	want.set[mute_enable] = !reverse || !toggle_global;
	ucx_plan_make(&cache, &want, &plan);
	assert(plan.n == (toggle_global ? 3u : 2u));
	for (unsigned int i = 0; i < plan.n; i++) {
		apply_op(&plan.ops[i]);
		check_monitor();
	}
	assert(!memcmp(&want, &cache.cfg, sizeof(want)));
}

static void test_release(void)
{
	reset();
	cache.cfg.set[mute] = 0;
	want.volume[phones] = -100;
	want.set[phones_level] = 2;
	ucx_plan_make(&cache, &want, &plan);
	assert(plan.n == 3);
	for (unsigned int i = 0; i < plan.n; i++) {
		apply_op(&plan.ops[i]);
		if (cache.cfg.set[mute]) {
			assert(cache.cfg.volume[phones] == want.volume[phones]);
			assert(cache.cfg.set[phones_level] == want.set[phones_level]);
		}
	}
	assert(!memcmp(&want, &cache.cfg, sizeof(want)));
}

/* The target mic route is inverted. It is physically loud despite reading
 * as attenuated in the cache. A second live route reads as already off. */
static unsigned int test_push(unsigned int stop)
{
	const int mic = UCX_SRC_INPUT, stale = UCX_SRC_PCM;
	unsigned int forced[UCX_MIX_OUTS][UCX_SOURCES] = { 0 };
	unsigned int steps;
	reset();
	cache.cfg.mix[phones][mic] = -4000;
	cache.linear[phones][mic] = ucx_gain_to_linear(-4000);
	matrix[phones][stale] = 32768;
	want = cache.cfg;
	want.set[mic_gain] = 40;
	want.volume[phones] = -100;
	want.invert[phones][mic] = 1;
	ucx_plan_make_push(&cache, &want, &plan);
	steps = plan.n;
	for (unsigned int i = 0; i < plan.n;) {
		if (i == stop) {
			/* A fresh command reads the partially updated cache. */
			ucx_plan_make_push(&cache, &want, &plan);
			i = 0;
			stop = UINT_MAX;
		}
		const struct ucx_op *op = &plan.ops[i++];
		apply_op(op);
		if (op->kind == UCX_OP_PUSH_MIX)
			forced[op->out][op->src]++;
		if (cache.cfg.set[mic_gain] == 40) {
			assert(!matrix[phones][stale]);
			assert(abs(matrix[phones][mic]) <= ucx_gain_to_linear(-4000));
		}
		if (cache.cfg.volume[phones] == -100) {
			assert(!matrix[phones][stale]);
			assert(matrix[phones][mic] == -ucx_gain_to_linear(-4000));
		}
	}
	for (int out = 0; out < UCX_MIX_OUTS; out++)
		for (int src = 0; src < UCX_SOURCES; src++)
			assert(forced[out][src]);
	ucx_plan_make(&cache, &want, &plan);
	assert(!plan.n);
	return steps;
}

static void test_fader(void)
{
	reset();
	want.volume[phones] = -100;
	ucx_plan_make(&cache, &want, &plan);
	assert(plan.n == 1 && plan.ops[0].kind == UCX_OP_VOLUME);
	matrix[phones][UCX_SRC_PCM] = 32768;
	ucx_plan_make_push(&cache, &want, &plan);
	assert(plan.n == UCX_MIX_OUTS * UCX_SOURCES + 1);
	for (unsigned int i = 0; i < plan.n; i++) {
		apply_op(&plan.ops[i]);
		assert(matrix[phones][UCX_SRC_INPUT] == 32768);
		if (cache.cfg.volume[phones] == -100)
			assert(!matrix[phones][UCX_SRC_PCM]);
	}
}

int main(void)
{
	mic_gain = setting("Mic 1 Capture Volume");
	mute = setting("Phones L Playback Switch");
	mute_enable = setting("Output Mute Enable Switch");
	phones_level = setting("Phones Output Level");
	assert(!strcmp(ucx_output_name(phones), "Phones L"));
	test_mutes(false, false);
	test_mutes(true, false);
	test_mutes(false, true);
	test_mutes(true, true);
	test_release();
	test_fader();
	unsigned int steps = test_push(UINT_MAX);
	for (unsigned int stop = 0; stop < steps; stop++)
		test_push(stop);
	puts("apply ordering and retry tests passed");
	return 0;
}
