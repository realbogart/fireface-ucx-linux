#!/usr/bin/env python3
"""Measure the playback routing, which the unit can't report.

    tools/routes.py [dbfs]     # default -60; needs the PCM free (PipeWire profile off)

Plays a quiet tone on each playback channel in turn and reads the meters:
bank 2 = playback, 3 = outputs before the output volume, 6 = after it.
Output peak minus tone level = the gain of that route.
"""
import fcntl, math, os, struct, subprocess, sys, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVEL = float(sys.argv[1]) if len(sys.argv) > 1 else -60.0
SIZE = 5 * 516
IOC = 2 << 30 | SIZE << 16 | ord("U") << 8 | 1
idx = os.readlink("/proc/asound/UCX").removeprefix("card")
fd = os.open(f"/dev/snd/hwC{idx}D0", os.O_RDONLY)

def meters():
    buf = bytearray(SIZE); fcntl.ioctl(fd, IOC, buf); out = {}
    for b in range(5):
        off = b * 516; (n,) = struct.unpack_from("=I", buf, off); d = bytes(buf[off + 4:off + 4 + n])
        if n < 0xF4: continue
        (tag,) = struct.unpack_from("<I", d, 0xF0)
        out[tag >> 28] = [(lambda p: 20 * math.log10(p / 2**27) if p else None)(struct.unpack_from("<I", d, 0xA0 + 4 * c)[0] & ~0xF) for c in range(20)]
    return out

def show(m):
    return {k: {c: round(v, 1) for c, v in enumerate(vs) if v is not None} for k, vs in m.items()}

meters(); time.sleep(1.5)
print("idle:", show(meters()), flush=True)
res = {}
for ch in range(18):
    tone = subprocess.Popen([sys.executable, f"{ROOT}/tools/tone.py", str(ch), "--dbfs", str(LEVEL), "--seconds", "3"], stdout=subprocess.PIPE)
    play = subprocess.Popen(["aplay", "-q", "-D", "hw:CARD=UCX", "-t", "raw", "-c", "18", "-f", "S32_LE", "-r", "48000"], stdin=tone.stdout, stderr=subprocess.PIPE)
    time.sleep(1.2); meters(); time.sleep(1.0); m = meters()
    err = play.communicate()[1].decode().strip(); tone.wait()
    s = show(m); res[ch] = s
    print(f"pb {ch}: playback {s.get(2)} pre {s.get(3)} post {s.get(6)} {err}", flush=True)
    time.sleep(0.5)
