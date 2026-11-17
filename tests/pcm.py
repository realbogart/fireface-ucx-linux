#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile the driver callbacks against the small USB/ALSA adapter in pcm.c."""

import argparse
import os
from pathlib import Path
import re
import shlex
from statistics import median
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def extract(source, start, end):
    match = re.search(f"^{start}.*?^{end}", source, re.MULTILINE | re.DOTALL)
    if not match:
        raise ValueError(f"cannot find {start}")
    return match[0]


def build(scratch, revision=None):
    scratch.mkdir()

    def source(path):
        if revision is None:
            return (ROOT / path).read_text()
        return subprocess.check_output(["git", "show", f"{revision}:{path}"], cwd=ROOT, text=True)

    pcm = source("kmod/ucx_pcm.c")
    ucx = source("kmod/ucx.h")
    (scratch / "ucx_proto.h").write_text(source("include/ucx_proto.h"))

    def function(name):
        result = r"(?:bool|int|void|unsigned int|struct snd_pcm_substream \*|struct ucx_stream \*)"
        return extract(pcm, rf"(?:static )?{result}\s*{name}\(", r"\}$")

    def define(name, source=pcm):
        return re.search(rf"^#define {name}\b.*$", source, re.MULTILINE)[0]

    (scratch / "pcm-structs.h").write_text("\n".join([
        define("UCX_MAX_PAIRS", ucx),
        extract(ucx, "struct ucx_stream \\{", r"\};$"),
        extract(ucx, "enum ucx_pcm_state \\{", r"\};$"),
    ]))
    (scratch / "pcm-callbacks.h").write_text("\n\n".join([
        define("UCX_EVENT_START_MS"),
        define("UCX_STREAM_RESTARTS"),
        define("UCX_STREAM_ERR_WINDOW"),
        *([function("ucx_copy_span")] if "static void ucx_copy_span(" in pcm else []),
        *map(function, (
            "ucx_stream", "ucx_tick", "ucx_copy", "ucx_urb_pair",
            "ucx_stream_fail_locked", "ucx_stream_error", "ucx_pcm_events",
            "ucx_urb_stopped", "ucx_resubmit", "ucx_period_elapsed",
            "ucx_spdif_status", "ucx_playback_complete", "ucx_capture_complete", "ucx_engine_stop",
            "ucx_stream_err_work", "ucx_pcm_sync_stop", "ucx_pcm_trigger",
        )),
    ]))
    exe = scratch / "pcm-test"
    subprocess.run([
        *shlex.split(os.environ.get("CC", "cc")),
        *shlex.split(os.environ.get("CFLAGS", "-O2 -g -Wall -Wextra")),
        "-I", str(scratch),
        str(ROOT / "tests/pcm.c"), "-o", str(exe),
    ], check=True)
    return exe


def compare(before, after):
    samples = {exe: {} for exe in (before, after)}
    for i in range(10):
        for exe in ((before, after) if i % 2 == 0 else (after, before)):
            output = subprocess.check_output([exe, "--bench"], text=True)
            for line in output.splitlines():
                rate, ps = map(int, line.split())
                if i:  # discard the warm-up
                    samples[exe].setdefault(rate, []).append(ps / 1000)
    print("rate (Hz)   before (ns/slice)   after (ns/slice)   time saved")
    for rate, times in samples[before].items():
        old, new = median(times), median(samples[after][rate])
        print(f"{rate:9}   {old:17.1f}   {new:16.1f}   {1 - new / old:9.1%}")


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--bench-against", metavar="REV", help="compare duplex callbacks against a git revision")
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="ucx-pcm-test-") as directory:
    scratch = Path(directory)
    current = build(scratch / "current")
    subprocess.run([current], check=True)
    if args.bench_against:
        baseline = build(scratch / "baseline", args.bench_against)
        subprocess.run([baseline], check=True)
        compare(baseline, current)
