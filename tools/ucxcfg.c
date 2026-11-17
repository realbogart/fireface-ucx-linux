// SPDX-License-Identifier: MIT
/*
 * ucxcfg: make a Fireface UCX match a JSON config file, through the
 * snd-fireface-ucx driver. The work is in lib/ (libucx); this is the
 * command line. See examples/desktop.json for the format.
 */
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ucxcfg.h"

enum {
	EXIT_OK = 0,
	EXIT_INVALID = 1,	/* the config file is not valid */
	EXIT_USAGE = 2,
	EXIT_NO_DEVICE = 3,	/* no card, or no access to it */
	EXIT_IO = 4,		/* the device failed while reading or applying */
	EXIT_CHANGES = 5,	/* dry run: the device differs from the file */
};

static const char usage[] =
	"usage: ucxcfg [options] check FILE          validate FILE; no device needed\n"
	"       ucxcfg [options] apply [-n] [FILE]   make the device match FILE\n"
	"       ucxcfg [options] dump [-o FILE]      write what the driver knows as a config\n"
	"\n"
	"FILE may be - for standard input. Without FILE, apply uses your own config,\n"
	"$XDG_CONFIG_HOME/ucx/config.json (~/.config/ucx/config.json), which is also\n"
	"what ucxcfg-apply.service applies whenever the unit appears.\n"
	"  -c, --card NAME    ALSA card (default UCX)\n"
	"  -n, --dry-run      apply: print what would change, write nothing\n"
	"      --push         apply: write every mixer gain, including those the\n"
	"                     driver believes are right (the unit never reports its mixer)\n"
	"  -p, --print        check: print the file in its shortest form\n"
	"  -o, --output FILE  dump: write to FILE instead of standard output\n"
	"      --assume-off   dump: write unknown values as off instead of listing them\n"
	"      --full         dump, check -p: every channel and setting, defaults included\n"
	"  -v, --verbose      print every change; apply is silent without it\n"
	"  -q, --quiet        no notes on standard error, only errors\n"
	"  -h, --help\n"
	"\n"
	"exit status: 0 ok, 1 invalid file, 2 usage, 3 no device, 4 device error\n"
	"(the state may be partly applied), 5 dry run found changes\n";

static bool verbose, quiet;

/*
 * The plan as text. "unknown -> off" is most of a first apply (the driver
 * knows no mixer gain after load), so without -v it is one line per output.
 */
static void print_plan(const struct ucx_plan *plan)
{
	char text[160];
	unsigned int i;

	for (i = 0; i < plan->n; i++) {
		const struct ucx_op *op = &plan->ops[i];
		bool force_off = op->kind == UCX_OP_PUSH_MIX && !op->new;
		bool unknown_off = op->kind == UCX_OP_MIX && op->old == UCX_UNKNOWN &&
				   op->new == UCX_MIX_OFF;

		if (!verbose && (force_off || unknown_off)) {
			unsigned int first = i;

			while (i + 1 < plan->n) {
				const struct ucx_op *next = &plan->ops[i + 1];

				if (next->kind != op->kind || next->out != op->out ||
				    next->new != op->new || (unknown_off && next->old != UCX_UNKNOWN))
					break;
				i++;
			}
			printf(force_off ? "%s mix: force %u routes off\n" :
					   "%s mix: %u unknown gains -> off\n",
			       ucx_output_name(op->out), i - first + 1);
		} else {
			ucx_op_describe(op, text, sizeof(text));
			puts(text);
		}
	}
}

/*
 * At double and quad speed the last ADAT channels don't exist. Their gains
 * are written all the same, so the state is right when the rate drops.
 */
static void note_rate(struct ucx_dev *dev, const struct ucx_config *cfg)
{
	unsigned int rate = ucx_dev_rate(dev), channels = ucx_rate_channels(rate);
	int out, src, n = 0;

	for (out = 0; out < UCX_OUTPUTS; out++)
		for (src = 0; src < UCX_SOURCES; src++)
			if (cfg->mix[out][src] != UCX_MIX_OFF &&
			    ((unsigned int)out >= channels ||
			     (unsigned int)src % UCX_PCMS >= channels))
				n++;
	if (n && !quiet)
		fprintf(stderr, "note: at %u Hz the unit has %u channels; %d routes in the file "
			"use channels above that and have no effect until the rate drops\n",
			rate, channels, n);
}

static int cmd_check(const char *file, bool print, bool full)
{
	struct ucx_config cfg;
	char err[512], *text;

	if (ucx_config_load(file, &cfg, err, sizeof(err))) {
		fprintf(stderr, "ucxcfg: %s\n", err);
		return EXIT_INVALID;
	}
	if (print) {
		text = ucx_config_format(&cfg, full ? UCX_FORMAT_FULL : 0);
		if (!text)
			return EXIT_IO;
		puts(text);
		free(text);
	} else if (verbose) {
		printf("%s: valid\n", file);
	}
	return EXIT_OK;
}

static int cmd_apply(const char *card, const char *file, bool dry_run, bool push)
{
	static struct ucx_dev_state state;
	static struct ucx_plan plan;
	struct ucx_config cfg;
	struct ucx_dev *dev;
	char err[512];
	bool changes;

	/* The whole file is checked before the device is even opened */
	if (ucx_config_load(file, &cfg, err, sizeof(err))) {
		fprintf(stderr, "ucxcfg: %s\n", err);
		return EXIT_INVALID;
	}
	if (ucx_dev_open(card, &dev, err, sizeof(err))) {
		fprintf(stderr, "ucxcfg: %s\n", err);
		return EXIT_NO_DEVICE;
	}
	if (ucx_dev_read(dev, &state, err, sizeof(err)))
		goto fail;
	ucx_plan_make(&state, &cfg, &plan);
	changes = plan.n != 0;
	if (push)
		ucx_plan_make_push(&state, &cfg, &plan);
	if (dry_run || verbose) {
		print_plan(&plan);
		note_rate(dev, &cfg);
	}
	if (dry_run) {
		if (push)
			printf("push: all %d mixer gains would be written\n",
			       UCX_MIX_OUTS * UCX_SOURCES);
		ucx_dev_close(dev);
		return changes ? EXIT_CHANGES : EXIT_OK;
	}
	if (ucx_dev_apply(dev, &plan, err, sizeof(err)))
		goto fail;

	/* Read back what the driver now holds: nothing may be left to do */
	if (ucx_dev_read(dev, &state, err, sizeof(err)))
		goto fail;
	ucx_plan_make(&state, &cfg, &plan);
	if (plan.n) {
		ucx_op_describe(&plan.ops[0], err, sizeof(err));
		fprintf(stderr, "ucxcfg: %u values differ after the apply, first: %s\n", plan.n,
			err);
		ucx_dev_close(dev);
		return EXIT_IO;
	}
	ucx_dev_close(dev);
	return EXIT_OK;
fail:
	fprintf(stderr, "ucxcfg: %s\n", err);
	ucx_dev_close(dev);
	return EXIT_IO;
}

static int cmd_dump(const char *card, const char *output, bool assume_off, bool full)
{
	static struct ucx_dev_state state;
	struct ucx_dev *dev;
	char err[512], *text;
	FILE *f = stdout;
	int out, src, unknown = 0;

	if (ucx_dev_open(card, &dev, err, sizeof(err))) {
		fprintf(stderr, "ucxcfg: %s\n", err);
		return EXIT_NO_DEVICE;
	}
	if (ucx_dev_read(dev, &state, err, sizeof(err))) {
		fprintf(stderr, "ucxcfg: %s\n", err);
		ucx_dev_close(dev);
		return EXIT_IO;
	}
	ucx_dev_close(dev);

	text = ucx_config_format(&state.cfg, (assume_off ? UCX_FORMAT_ASSUME_OFF : 0) |
						(full ? UCX_FORMAT_FULL : 0));
	if (!text) {
		fprintf(stderr, "ucxcfg: out of memory\n");
		return EXIT_IO;
	}
	if (output && strcmp(output, "-")) {
		f = fopen(output, "w");
		if (!f) {
			fprintf(stderr, "ucxcfg: %s: %s\n", output, strerror(errno));
			free(text);
			return EXIT_USAGE;
		}
	}
	fprintf(f, "%s\n", text);
	free(text);
	if ((f != stdout ? fclose(f) : fflush(f)) == EOF) {
		fprintf(stderr, "ucxcfg: %s: %s\n", output ? output : "standard output",
			strerror(errno));
		return EXIT_IO;
	}

	for (out = 0; out < UCX_OUTPUTS; out++)
		for (src = 0; src < UCX_SOURCES; src++)
			unknown += state.cfg.mix[out][src] == UCX_UNKNOWN;
	if (!quiet && ucx_config_has_unknown(&state.cfg))
		fprintf(stderr, "ucxcfg: the driver doesn't know all of the device's state "
			"(%d of %d mixer gains are unknown: the unit reports no mixer gains, "
			"only the record the driver keeps of input gains, so playback gains "
			"are known once written since the driver loaded). %s\n",
			unknown, UCX_OUTPUTS * UCX_SOURCES, assume_off ?
			"They are written as off, which may not be what the unit is doing." :
			"They are listed under \"unknown\" and written as off; the file can't "
			"be applied until you remove that key.");
	return EXIT_OK;
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "card", required_argument, NULL, 'c' },
		{ "dry-run", no_argument, NULL, 'n' },
		{ "print", no_argument, NULL, 'p' },
		{ "output", required_argument, NULL, 'o' },
		{ "assume-off", no_argument, NULL, 'A' },
		{ "push", no_argument, NULL, 'P' },
		{ "full", no_argument, NULL, 'F' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const char *card = "UCX", *output = NULL, *cmd, *file;
	bool dry_run = false, assume_off = false, print = false, full = false, push = false;
	int opt;

	while ((opt = getopt_long(argc, argv, "c:npo:vqh", options, NULL)) != -1) {
		switch (opt) {
		case 'c':
			card = optarg;
			break;
		case 'n':
			dry_run = true;
			break;
		case 'p':
			print = true;
			break;
		case 'o':
			output = optarg;
			break;
		case 'A':
			assume_off = true;
			break;
		case 'P':
			push = true;
			break;
		case 'F':
			full = true;
			break;
		case 'v':
			verbose = true;
			break;
		case 'q':
			quiet = true;
			break;
		case 'h':
			fputs(usage, stdout);
			return EXIT_OK;
		default:
			fputs(usage, stderr);
			return EXIT_USAGE;
		}
	}
	cmd = argv[optind];
	file = cmd ? argv[optind + 1] : NULL;
	if (cmd && !strcmp(cmd, "dump") && !file && !dry_run && !print && !push)
		return cmd_dump(card, output, assume_off, full);
	if (cmd && !strcmp(cmd, "apply") && !file && !print && !full && !output && !assume_off) {
		char own[4096];

		if (ucx_config_default_path(own, sizeof(own))) {
			fprintf(stderr, "ucxcfg: no FILE, and neither XDG_CONFIG_HOME nor HOME "
				"says where your config is\n");
			return EXIT_USAGE;
		}
		if (access(own, F_OK)) {
			fprintf(stderr, "ucxcfg: no FILE, and %s doesn't exist (save one with "
				"ucxtui, or `ucxcfg dump -o %s`)\n", own, own);
			return EXIT_USAGE;
		}
		return cmd_apply(card, own, dry_run, push);
	}
	if (cmd && file && !argv[optind + 2] && !output && !assume_off) {
		if (!strcmp(cmd, "check") && !dry_run && !push && (print || !full))
			return cmd_check(file, print, full);
		if (!strcmp(cmd, "apply") && !print && !full)
			return cmd_apply(card, file, dry_run, push);
	}
	fputs(usage, stderr);
	return EXIT_USAGE;
}
