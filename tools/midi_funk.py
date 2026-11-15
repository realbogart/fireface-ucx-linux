#!/usr/bin/env python3
"""A funk bass line for a mono synth on MIDI Out 1 of the UCX, with pitch
bend scoops and falls and mod wheel swells. MIDI channel 1; assumes the
synth's usual bend range of 2 semitones.

    tools/midi_funk.py [transpose] [bpm]
"""
import os
import sys
import time

transpose = int(sys.argv[1]) if len(sys.argv) > 1 else 0
bpm = float(sys.argv[2]) if len(sys.argv) > 2 else 104
STEP = 60 / bpm / 4  # one sixteenth, in seconds
E = 52 + transpose   # root

events = []  # (time in sixteenths, order, bytes)


def note(t, n, length, vel=100):
    events.append((t, 2, bytes([0x90, E + n, vel])))
    events.append((t + length, 0, bytes([0x80, E + n, 0])))


def ramp(t0, t1, a, b, make):
    n = max(1, int((t1 - t0) * 6))
    for i in range(n + 1):
        events.append((t0 + (t1 - t0) * i / n, 1, make(a + (b - a) * i / n)))


def bend(t0, t1, a, b):  # in semitones, -2..2
    def msg(semi):
        v = max(0, min(16383, round(8192 + semi / 2 * 8191)))
        return bytes([0xE0, v & 0x7F, v >> 7])
    ramp(t0, t1, a, b, msg)


def mod(t0, t1, a, b):  # mod wheel 0..127
    ramp(t0, t1, a, b, lambda v: bytes([0xB0, 1, round(v)]))


def bar_a(t, root=0):
    note(t, root, 1.6, 115)
    note(t + 3, root + 12, 0.5, 90)
    note(t + 6, root + 3, 0.6)
    bend(t + 7, t + 8, -2, 0)           # scoop into the fourth
    note(t + 7, root + 5, 1.8, 110)
    note(t + 10, root + 10, 0.5, 85)
    note(t + 11, root + 12, 0.7, 105)
    note(t + 14, root - 2, 0.7)
    note(t + 15, root - 1, 0.7)         # chromatic step back to the root


def bar_b(t, root=0):
    note(t, root, 1.6, 115)
    note(t + 3, root + 12, 0.5, 90)
    note(t + 6, root + 7, 0.6)
    note(t + 7, root + 10, 0.7)
    note(t + 8, root + 12, 4.2, 120)    # held: wheel swells, then it falls off
    mod(t + 8.5, t + 11.5, 0, 110)
    bend(t + 11.2, t + 12.2, 0, -2)
    mod(t + 12.2, t + 12.4, 110, 0)
    bend(t + 12.6, t + 12.7, -2, 0)
    note(t + 13, root + 5, 0.6)
    note(t + 14, root + 3, 0.6)
    note(t + 15, root + 2, 0.7)


def bar_turn(t):
    note(t, 7, 1.5, 115)
    note(t + 2, 7, 0.5, 85)
    note(t + 4, 10, 1.2)
    bend(t + 6, t + 8, 0, 2)            # slow bend up a whole tone
    note(t + 6, 12, 5.6, 125)
    mod(t + 7, t + 11, 0, 127)
    bend(t + 11, t + 11.6, 2, 0)
    mod(t + 11.4, t + 11.8, 127, 0)
    for i, n in enumerate((12, 10, 7, 5)):
        note(t + 12 + i, n, 0.7, 100 - 5 * i)


for i, bar in enumerate((bar_a, bar_b, bar_a, bar_b)):
    bar(16 * i)
bar_a(64, 5)                            # up a fourth
bar_b(80, 5)
bar_a(96)
bar_turn(112)
# Last note: low root, wheel up, long dive
note(128, -12, 14, 127)
mod(130, 136, 0, 127)
bend(134, 141, 0, -2)

card = os.readlink("/proc/asound/UCX").removeprefix("card")
fd = os.open(f"/dev/snd/midiC{card}D0", os.O_WRONLY)
try:
    os.write(fd, bytes([0xE0, 0, 0x40, 0xB0, 1, 0]))
    start = time.monotonic() + 0.1
    for t, _, msg in sorted(events):
        delay = start + t * STEP - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        os.write(fd, msg)
    time.sleep(0.3)
finally:
    os.write(fd, bytes([0xB0, 123, 0, 0xB0, 1, 0, 0xE0, 0, 0x40]))  # notes off, wheel 0, bend centre
    os.close(fd)
