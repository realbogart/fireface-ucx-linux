#!/usr/bin/env python3
"""Write raw 18-channel S32_LE audio with a quiet sine on one channel to stdout.

    python3 tools/tone.py 0 | aplay -D hw:CARD=FirefaceUCX -t raw -c 18 -f S32_LE -r 48000

Same signal as `ucxctl stream-out`: -30 dBFS by default, capped at -20 dBFS,
with 50 ms fades.
"""
import argparse
import math
import struct
import sys

CHANNELS = 18
MAX_DBFS = -20.0

parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
parser.add_argument("channel", type=int, help=f"0-{CHANNELS - 1}")
parser.add_argument("--seconds", type=float, default=3.0)
parser.add_argument("--freq", type=float, default=440.0)
parser.add_argument("--dbfs", type=float, default=-30.0)
parser.add_argument("--rate", type=int, default=48000)
args = parser.parse_args()

if not 0 <= args.channel < CHANNELS:
    sys.exit(f"channel must be 0-{CHANNELS - 1}")
amp = 10 ** (min(args.dbfs, MAX_DBFS) / 20)
total = int(args.seconds * args.rate)
fade = args.rate // 20
silence = [0] * CHANNELS
out = sys.stdout.buffer

for n in range(total):
    gain = min(1.0, n / fade, (total - n) / fade)
    frame = list(silence)
    # 24 valid bits at the top of each 32-bit sample
    frame[args.channel] = round(amp * gain * math.sin(2 * math.pi * args.freq * n / args.rate) * 8388607) << 8
    out.write(struct.pack(f"<{CHANNELS}i", *frame))
