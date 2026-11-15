#!/usr/bin/env python3
"""Play "In the Hall of the Mountain King" (Grieg) on MIDI Out 1 of the UCX.

Monophonic, MIDI channel 1, four passes that get faster and climb an octave.
    tools/midi_tune.py [transpose]
"""
import os
import sys
import time

# (semitones above the starting B, length in eighth notes)
PHRASE = [(0, 1), (2, 1), (3, 1), (5, 1), (7, 1), (3, 1), (7, 2),
          (6, 1), (2, 1), (6, 2), (5, 1), (1, 1), (5, 2),
          (0, 1), (2, 1), (3, 1), (5, 1), (7, 1), (3, 1), (7, 1), (12, 1),
          (10, 1), (7, 1), (3, 1), (7, 1), (10, 4)]
PASSES = [(0, 0.24), (0, 0.18), (12, 0.13), (12, 0.095)]  # (octave shift, seconds per eighth)

card = os.readlink("/proc/asound/UCX").removeprefix("card")
fd = os.open(f"/dev/snd/midiC{card}D0", os.O_WRONLY)
base = 59 + (int(sys.argv[1]) if len(sys.argv) > 1 else 0)


def play(note, seconds, velocity=100):
    os.write(fd, bytes([0x90, note, velocity]))
    time.sleep(seconds * 0.8)
    os.write(fd, bytes([0x80, note, 0]))
    time.sleep(seconds * 0.2)


try:
    for shift, eighth in PASSES:
        for step, length in PHRASE:
            play(base + shift + step, eighth * length)
    for step in (12, 7, 3, 0):  # fall back down to the low B
        play(base + step, 0.12)
    play(base - 12, 2.0, 127)
finally:
    os.write(fd, bytes([0xB0, 123, 0]))  # all notes off
    os.close(fd)
