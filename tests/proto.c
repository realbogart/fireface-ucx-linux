// SPDX-License-Identifier: MIT
/*
 * The stand-alone record helpers of ucx_proto.h against libm and against
 * register values read from the unit (measured 2026-11-13).
 */
#include <math.h>
#include <stdio.h>
#include "ucx_proto.h"

static int failed;

static void expect(int ok, const char *what, int a, int b)
{
	if (ok)
		return;
	printf("FAIL %s: %d %d\n", what, a, b);
	failed = 1;
}

static void unpack(unsigned short a, unsigned short b, int known, int left, int right)
{
	int l = 0, r = 0, k = ucx_dsp_sa_unpack(a, b, &l, &r);

	expect(k == known && (!known || (l == left && r == right)), "unpack", a, b);
}

int main(void)
{
	unsigned short a, b;
	int t, l, r, l2, r2;

	/* The integer gain is the one ucxcfg computes with pow() */
	for (t = UCX_DSP_SA_MIX_MAX - 1023; t <= UCX_DSP_SA_MIX_MAX; t++) {
		double x = round(32768.0 * pow(10.0, t / 200.0));
		unsigned int gain = ucx_tenth_db_to_gain(t);

		expect(gain == (x < 1 ? 1 : (unsigned int)x), "gain of level", t, gain);
		if (t >= UCX_DSP_SA_MIX_MIN)
			expect(ucx_tenth_db_to_gain(ucx_gain_to_tenth_db(gain)) == gain,
			       "gain -> level -> gain", t, gain);
		if (t >= -500)	/* below, several levels share a gain */
			expect(ucx_gain_to_tenth_db(gain) == t, "level -> gain -> level", t, gain);
	}
	expect(ucx_dsp_sa_level(0) == UCX_DSP_SA_MIX_OFF && ucx_dsp_sa_gain(UCX_DSP_SA_MIX_OFF) == 0,
	       "off", 0, 0);
	for (t = 1; t <= 65536; t++)
		expect(ucx_dsp_sa_level(t) != UCX_DSP_SA_MIX_OFF, "a gain recorded as off", t, 0);
	expect(ucx_dsp_sa_level(1) == UCX_DSP_SA_MIX_MIN, "smallest gain", 0, 0);

	for (l = UCX_DSP_SA_MIX_MIN; l <= UCX_DSP_SA_MIX_MAX; l += 7)
		for (r = UCX_DSP_SA_MIX_MIN; r <= UCX_DSP_SA_MIX_MAX; r += 11) {
			ucx_dsp_sa_pack(l, r, &a, &b);
			expect(ucx_dsp_sa_unpack(a, b, &l2, &r2) && l2 == l && r2 == r, "pack", l, r);
		}

	/* Read from the unit after writing these per-channel levels */
	unpack(0x000f, 0xfff6, 1, 0, -10);
	unpack(0xff6d, 0x0076, 1, -10, -650);
	unpack(0xc7c8, 0x0026, 1, -900, 38);
	/* Pair level and pan, as TotalMix leaves it: off, hard left, hard right, in between */
	unpack(0xd760, 0x0000, 1, -650, -650);
	unpack(0xf380, 0xff9c, 1, -200, -650);
	unpack(0xf380, 0x0064, 1, -650, -200);
	unpack(0x0000, 0x0032, 0, 0, 0);

	/* Line In 7 on ADAT 7 at -20 dB: the word, and where the dump shows it */
	expect((ucx_dsp_sa_cmd(16, 6, -200) & 0x7fffffff) == 0x22e6f384, "word", 0, 0);
	expect(ucx_dsp_sa_reg(17, 6) == 0x22ac && ucx_dsp_sa_reg(0, 8) == 0x12c0 &&
	       ucx_dsp_sa_reg(2, 17) == 0x14ee, "dump register", 0, 0);
	expect(ucx_dsp_sa_src(0xad) == 6 && ucx_dsp_sa_src(0xc3) == 9 && ucx_dsp_sa_src(0xef) == 17 &&
	       ucx_dsp_sa_src(0xb0) < 0 && ucx_dsp_sa_src(0x9f) < 0 && ucx_dsp_sa_src(0xf0) < 0,
	       "input of dump register", 0, 0);

	/* CLOCK (0x1b) words of a Windows start, from 0x14000c380 */
	{
		static const unsigned int hz[4] = { 32000, 44100, 48000, 50000 };
		static const unsigned int val[4] = { 0x3500, 0xdb92, 0x2355, 0xd000 };
		static const unsigned int idx[4] = { 0x0000, 0xdd01, 0x5502, 0x0003 };
		unsigned int v, i, s;

		for (s = 0; s < 4; s++) {
			ucx_dds_req(s, hz[s], &v, &i);
			expect(v == val[s], "dds value", v, val[s]);
			expect(i == idx[s], "dds index", i, idx[s]);
		}
	}
	expect(UCX_REG2_WINDOWS_START == 0x0f00, "reg2 start", UCX_REG2_WINDOWS_START, 0x0f00);
	/*
	 * The flash store words, as ucxctl dsp sees them (with and without parity),
	 * and words next to them that must not be taken for one: a mixer word
	 * (bit 30) whose index bits look like 3e06, the dump request, a volume
	 */
	expect(UCX_DSP_WORD_REG(ucx_dsp_word(0x3e060910)) == UCX_DSP_REG_STORE, "store word",
	       UCX_DSP_WORD_REG(ucx_dsp_word(0x3e060910)), UCX_DSP_REG_STORE);
	expect(UCX_DSP_WORD_REG(0x3e060911u) == UCX_DSP_REG_STORE, "store word, no parity",
	       UCX_DSP_WORD_REG(0x3e060911u), UCX_DSP_REG_STORE);
	expect(UCX_DSP_WORD_REG(ucx_dsp_word(0x3e053481)) == UCX_DSP_REG_STORE_NEAR,
	       "3e05 word", UCX_DSP_WORD_REG(ucx_dsp_word(0x3e053481)), UCX_DSP_REG_STORE_NEAR);
	expect(UCX_DSP_WORD_REG(ucx_dsp_word(UCX_DSP_MIXER | 0x3e060000)) != UCX_DSP_REG_STORE,
	       "mixer word", UCX_DSP_WORD_REG(ucx_dsp_word(UCX_DSP_MIXER | 0x3e060000)), 0);
	expect(UCX_DSP_WORD_REG(ucx_dsp_word(UCX_DSP_DUMP_REQUEST)) == 0x3f99, "dump request",
	       UCX_DSP_WORD_REG(ucx_dsp_word(UCX_DSP_DUMP_REQUEST)), 0x3f99);
	expect(UCX_DSP_WORD_REG(ucx_dsp_output_vol_cmd(0, -650)) == UCX_DSP_PHYS_INPUTS << 8,
	       "volume word", UCX_DSP_WORD_REG(ucx_dsp_output_vol_cmd(0, -650)), 0);
	if (!failed)
		puts("proto tests passed");
	return failed;
}
