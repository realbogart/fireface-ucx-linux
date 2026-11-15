// SPDX-License-Identifier: MIT
/*
 * libFuzzer harness for the config file parser (lib/config.c): `make fuzz`.
 *
 * Any bytes go to ucx_config_parse(). A rejected file must come with a
 * message; an accepted one must print (ucx_config_format(), both the short
 * and the --full form) as text that loads back as the same struct
 * ucx_config, and printing that again must give the same text. This is the
 * property tests/config.sh checks on its valid files, for every input the
 * fuzzer makes. Needs only jansson: no ALSA, libusb or device.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxcfg.h"

static void dump(const char *what, const char *text)
{
	fprintf(stderr, "--- %s ---\n%s\n", what, text ? text : "(null)");
}

/* The first field where @a and @b differ, as a name a person can look up */
static void report(const struct ucx_config *a, const struct ucx_config *b)
{
	char name[96];
	int i, j;

	for (i = 0; i < UCX_OUTPUTS; i++)
		if (a->volume[i] != b->volume[i])
			fprintf(stderr, "volume of %s: %d vs %d\n", ucx_output_name(i),
				a->volume[i], b->volume[i]);
	for (i = 0; i < UCX_MIX_OUTS; i++)
		for (j = 0; j < UCX_SOURCES; j++)
			if (a->mix[i][j] != b->mix[i][j] || a->invert[i][j] != b->invert[i][j])
				fprintf(stderr, "mix %s <- %s: %d/%d vs %d/%d\n", ucx_output_name(i),
					ucx_source_name(j), a->mix[i][j], a->invert[i][j],
					b->mix[i][j], b->invert[i][j]);
	for (i = 0; i < UCX_SETTINGS; i++)
		if (a->set[i] != b->set[i]) {
			ucx_setting_name(i, name, sizeof(name));
			fprintf(stderr, "%s: %d vs %d\n", name, a->set[i], b->set[i]);
		}
	for (i = 0; i < UCX_PAIRS; i++) {
		if (a->link_out[i] != b->link_out[i])
			fprintf(stderr, "link of output pair %d: %d vs %d\n", i, a->link_out[i],
				b->link_out[i]);
		if (a->link_in[i] != b->link_in[i])
			fprintf(stderr, "link of input pair %d: %d vs %d\n", i, a->link_in[i],
				b->link_in[i]);
	}
}

/* Print @cfg with @flags, load it back, print again: same struct, same text */
static void round_trip(const struct ucx_config *cfg, unsigned int flags)
{
	static struct ucx_config back;
	char err[512], *text, *again;

	text = ucx_config_format(cfg, flags);
	if (!text)
		abort();	/* out of memory: libFuzzer reports its own limit first */
	if (ucx_config_parse(text, strlen(text), "formatted", &back, err, sizeof(err))) {
		dump("formatted", text);
		fprintf(stderr, "flags %u: own output rejected: %s\n", flags, err);
		abort();
	}
	if (memcmp(cfg, &back, sizeof(back))) {
		dump("formatted", text);
		fprintf(stderr, "flags %u: loads back as a different config:\n", flags);
		report(cfg, &back);
		abort();
	}
	again = ucx_config_format(&back, flags);
	if (!again || strcmp(text, again)) {
		dump("first", text);
		dump("second", again);
		fprintf(stderr, "flags %u: printing is not stable\n", flags);
		abort();
	}
	free(again);
	free(text);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	/* Static: the struct is about 30 kB, and a fresh one each run would hide nothing */
	static struct ucx_config cfg;
	char err[512];

	/* A canary: a rejected file must replace it with a message */
	memset(err, 'x', sizeof(err) - 1);
	err[sizeof(err) - 1] = '\0';
	/* @data is not NUL-terminated: the parser must keep to @size */
	if (ucx_config_parse((const char *)data, size, "fuzz", &cfg, err, sizeof(err))) {
		if (!err[0] || err[0] == 'x' || !memchr(err, '\0', sizeof(err))) {
			fprintf(stderr, "rejected without a message\n");
			abort();
		}
		return 0;
	}
	if (ucx_config_has_unknown(&cfg)) {
		fprintf(stderr, "a parsed file holds an unknown value\n");
		abort();
	}
	round_trip(&cfg, 0);
	round_trip(&cfg, UCX_FORMAT_FULL);
	return 0;
}
