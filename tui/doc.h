/* SPDX-License-Identifier: MIT */
/*
 * The config being edited in ucxtui, and the edits that can be made to it.
 *
 * The file format writes a linked pair as one entry ("Phones L/R"), which
 * can say less than two entries: one volume, one of each setting, and per
 * source pair either a straight stereo route or the same mono routes to both
 * sides. Every edit here keeps linked pairs within that, so what is on the
 * screen is what a saved file loads back as (tests/doc.c checks that).
 */
#ifndef TUI_DOC_H
#define TUI_DOC_H

#include "ucxcfg.h"

#define DOC_SRC_PAIRS	(UCX_SOURCES / 2)
/* The FX send bus as the tenth output pair */
#define DOC_FX_PAIR	UCX_PAIRS
#define DOC_HISTORY	256

/* One or two channels edited as one: what the lists and the forms show */
struct strip {
	bool output;
	int ch, n;
};

/* A source of a strip's mix: one channel or a pair */
struct mixrow {
	int src, n;
};

struct doc_state {
	struct ucx_config cfg;
	/* The FX send rows have no link in the unit; here it is how they are shown */
	bool fx_link;
	/*
	 * A source pair shown as two rows in the mix of the strip that starts
	 * at this output. Only decides when the gains fit both ways.
	 */
	bool split[UCX_MIX_OUTS][DOC_SRC_PAIRS];
};

struct doc {
	struct doc_state s;
	struct doc_state *history;	/* DOC_HISTORY + 1 states */
	int pos, top;
	long last_tag;
	long long last_time;
};

/*
 * Start from @cfg, which may be a device state: unknown values become the
 * defaults, and a pair that is linked but can't be written as one entry is
 * unlinked, as saving and loading it would do. Returns how many values were
 * unknown. Forgets the history.
 */
int doc_load(struct doc *doc, const struct ucx_config *cfg);

/* The strips of one side, in channel order; the outputs end with the FX send bus */
int doc_strips(const struct doc *doc, bool output, struct strip *strips);
void doc_strip_name(const struct strip *strip, char *buf, size_t len);
bool doc_linked(const struct doc *doc, bool output, int pair);

/* A setting of a channel by its key in the file (-1: no such), and of the whole unit */
int doc_setting(bool output, int ch, const char *key);
int doc_global(const char *key);

/*
 * Call before an edit. Edits with the same non-zero @tag that follow each
 * other within a second are one step of the history.
 */
void doc_checkpoint(struct doc *doc, long tag);
bool doc_undo(struct doc *doc);
bool doc_redo(struct doc *doc);

/* These write both channels of a linked pair */
void doc_set(struct doc *doc, int setting, int value);
void doc_set_volume(struct doc *doc, int out, int value);
/* Linking gives the right channel the left one's values */
void doc_set_link(struct doc *doc, bool output, int pair, bool on);

int doc_mix_rows(const struct doc *doc, const struct strip *strip, struct mixrow *rows);
int doc_mix_get(const struct doc *doc, const struct strip *strip, const struct mixrow *row,
		int *invert);
void doc_mix_set(struct doc *doc, const struct strip *strip, const struct mixrow *row, int gain,
		 int invert);
/*
 * Show the source pair of @row as two rows, or as one again. Refused (false)
 * when that would change what is heard: the route has to be off, or for a
 * mono output the two gains equal.
 */
bool doc_mix_split(struct doc *doc, const struct strip *strip, const struct mixrow *row);

#endif /* TUI_DOC_H */
