#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile the driver callbacks against the small USB/ALSA adapter in pcm.c."""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
pcm = (ROOT / "kmod/ucx_pcm.c").read_text()
ucx = (ROOT / "kmod/ucx.h").read_text()


def extract(source, start, end):
    match = re.search(f"^{start}.*?^{end}", source, re.MULTILINE | re.DOTALL)
    if not match:
        raise ValueError(f"cannot find {start}")
    return match[0]


def function(name):
    result = r"(?:bool|int|void|unsigned int|struct snd_pcm_substream \*|struct ucx_stream \*)"
    return extract(pcm, rf"(?:static )?{result}\s*{name}\(", r"\}$")


def define(name, source=pcm):
    return re.search(rf"^#define {name}\b.*$", source, re.MULTILINE)[0]


with tempfile.TemporaryDirectory(prefix="ucx-pcm-test-") as directory:
    scratch = Path(directory)
    (scratch / "pcm-structs.h").write_text("\n".join([
        define("UCX_MAX_PAIRS", ucx),
        extract(ucx, "struct ucx_stream \\{", r"\};$"),
        extract(ucx, "enum ucx_pcm_state \\{", r"\};$"),
    ]))
    (scratch / "pcm-callbacks.h").write_text("\n\n".join([
        define("UCX_EVENT_START_MS"),
        define("UCX_STREAM_RESTARTS"),
        define("UCX_STREAM_ERR_WINDOW"),
        *map(function, (
            "ucx_stream", "ucx_copy", "ucx_tick", "ucx_urb_pair",
            "ucx_stream_fail_locked", "ucx_stream_error", "ucx_pcm_events",
            "ucx_urb_stopped", "ucx_resubmit", "ucx_period_elapsed",
            "ucx_playback_complete", "ucx_capture_complete", "ucx_engine_stop",
            "ucx_stream_err_work", "ucx_pcm_sync_stop", "ucx_pcm_trigger",
        )),
    ]))
    exe = scratch / "pcm-test"
    subprocess.run([
        *shlex.split(os.environ.get("CC", "cc")),
        *shlex.split(os.environ.get("CFLAGS", "-O2 -g -Wall -Wextra")),
        "-I", str(scratch), "-I", str(ROOT / "include"),
        str(ROOT / "tests/pcm.c"), "-o", str(exe),
    ], check=True)
    subprocess.run([exe], check=True)
