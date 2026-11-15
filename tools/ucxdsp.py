#!/usr/bin/env python3
"""DSP words, registers and meters through the driver's hwdep device (no root).

    tools/ucxdsp.py dsp 04400001 044601b8   # send words (hex; parity is added)
    tools/ucxdsp.py regs [0445 ...]         # register table: "reg value" (hex)
    tools/ucxdsp.py meters [-r]             # peak dBFS per bank, like debugfs;
                                            # -r adds RMS (scale unverified:
                                            # read as mean square, 2^-55)
    tools/ucxdsp.py standalone              # the unit's own input mix (see below)
    tools/ucxdsp.py status                  # firmware, DSP load, rate, FX bus levels

The device is /dev/snd/hwC<card>D0 of card UCX; see include/ucx_hwdep.h.
"""
import argparse
import fcntl
import math
import os
import struct
import sys

DSP_MAX_WORDS = 63
REGS_BYTES = 0x8000 * 4
METER_BANKS = 5
METER_BYTES = 512
METER_CHANNELS = 20
METER_PEAK_OFFSET = 0xA0
METER_TAG_OFFSET = 0xF0
METER_MIN_BYTES = 0xF4
METERS_SIZE = METER_BANKS * (4 + METER_BYTES)
# _IOR('U', 0x01, struct ucx_hwdep_meters)
UCX_HWDEP_METERS = 2 << 30 | METERS_SIZE << 16 | ord("U") << 8 | 0x01
# _IOR('U', 0x02, struct ucx_hwdep_status): four u32
UCX_HWDEP_STATUS = 2 << 30 | 16 << 16 | ord("U") << 8 | 0x02


def device(card):
    try:
        index = os.readlink(f"/proc/asound/{card}").removeprefix("card")
    except OSError:
        sys.exit(f"no ALSA card {card}; is snd-fireface-ucx loaded?")
    return f"/dev/snd/hwC{index}D0"


def cmd_dsp(fd, args):
    words = [int(w, 16) for w in args.words]
    for i in range(0, len(words), DSP_MAX_WORDS):
        chunk = words[i:i + DSP_MAX_WORDS]
        os.write(fd, struct.pack(f"={len(chunk)}I", *chunk))


def cmd_regs(fd, args):
    data = os.read(fd, REGS_BYTES)
    want = {int(r, 16) for r in args.regs}
    for (w,) in struct.iter_unpack("=I", data):
        reg, val = w >> 16, w & 0xFFFF
        if not want or reg in want:
            print(f"{reg:04x} {val:04x}")


def cmd_meters(fd, args):
    buf = bytearray(METERS_SIZE)
    fcntl.ioctl(fd, UCX_HWDEP_METERS, buf)
    for bank in range(METER_BANKS):
        off = bank * (4 + METER_BYTES)
        (length,) = struct.unpack_from("=I", buf, off)
        data = bytes(buf[off + 4:off + 4 + length])
        if length < METER_MIN_BYTES:
            print(f"short meter read: {length} bytes")
            continue
        (tag,) = struct.unpack_from("<I", data, METER_TAG_OFFSET)
        line = [f"bank {tag:08x} peak dBFS:"]
        for ch in range(METER_CHANNELS):
            (peak,) = struct.unpack_from("<I", data, METER_PEAK_OFFSET + 4 * ch)
            peak &= ~0xF
            if peak:
                line.append(f"{ch}:{20 * math.log10(peak / 2**27):.1f}")
        print(" ".join(line))
        if args.rms:
            line = [f"bank {tag:08x} rms dBFS: "]
            for ch in range(METER_CHANNELS):
                (rms,) = struct.unpack_from("<Q", data, 8 * ch)
                if rms:
                    line.append(f"{ch}:{10 * math.log10(rms / 2**55):.1f}")
            print(" ".join(line))


OUTPUT_PAIRS = ["Line 1/2", "Line 3/4", "Line 5/6", "Phones", "S/PDIF",
                "ADAT 1/2", "ADAT 3/4", "ADAT 5/6", "ADAT 7/8"]
INPUTS = ["Mic 1", "Mic 2", "Inst 3", "Inst 4", "Line 5", "Line 6", "Line 7", "Line 8",
          "S/PDIF L", "S/PDIF R"] + [f"ADAT {n}" for n in range(1, 9)]


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def cmd_standalone(fd, args):
    """The unit's record of the hardware-input mix, from its state dump.

    Two registers per input on the first channel of each output pair: a0.. for
    the 8 analog inputs, c0.. for S/PDIF, e0.. for ADAT. Either a level per
    output channel, or a pair level with a pan (decoded as TotalMix does,
    0x14003cba0). It is a note next to the mixer, not the mixer: it is right
    when whoever wrote the input gains also wrote it. The driver does (one
    word per output channel on register output << 8 | 0xe0 + input, level in
    0.1 dB << 4 | 4), and fills input gains it doesn't know from it at load.
    """
    regs = {w >> 16: w & 0xFFFF for (w,) in struct.iter_unpack("=I", os.read(fd, REGS_BYTES))}
    for pair, name in enumerate(OUTPUT_PAIRS):
        line = []
        for i, inp in enumerate(INPUTS):
            cmd = 0xA0 + 2 * i if i < 8 else 0xC0 + 2 * (i - 8) if i < 10 else 0xE0 + 2 * (i - 10)
            reg = (0x12 + 2 * pair) << 8 | cmd
            if reg not in regs or reg + 1 not in regs:
                continue
            a, b = regs[reg], regs[reg + 1]
            if a & 8:  # separate left and right levels, 0.1 dB, 12 and 11 bits
                left = s16(a & 0xFFF0) / 160
                right = s16(((a << 8 | b & 0xFF) << 5) & 0xFFFF) / 320
                if left > -65 or right > -65:
                    line.append(f"{inp}: L {left:.1f} dB, R {right:.1f} dB")
            elif s16(a) / 160 > -65:  # volume in 1/160 dB, pan -100..100
                line.append(f"{inp}: {s16(a) / 160:.1f} dB, pan {s16(b) / 100:+.2f}")
        print(f"{name}: {'; '.join(line) if line else 'all inputs off'}")


# _IOWR('U', 0x12, struct snd_ctl_elem_value): id 64 bytes, value union at 72
SNDRV_CTL_IOCTL_ELEM_READ = 3 << 30 | 1224 << 16 | ord("U") << 8 | 0x12
SNDRV_CTL_ELEM_IFACE_PCM = 3


def spdif_status(card):
    """The 24 channel status bytes the S/PDIF input receives, or None.

    The driver's "IEC958 Capture Default" control; it collects them from the
    capture stream, so they are zero unless a PCM is open and the input locked.
    """
    buf = bytearray(1224)
    struct.pack_into("=IiII44sI", buf, 0, 0, SNDRV_CTL_ELEM_IFACE_PCM, 0, 0,
                     b"IEC958 Capture Default", 0)
    try:
        index = os.readlink(f"/proc/asound/{card}").removeprefix("card")
        fd = os.open(f"/dev/snd/controlC{index}", os.O_RDONLY)
        try:
            fcntl.ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, buf)
        finally:
            os.close(fd)
    except OSError:
        return None
    return bytes(buf[72:96])


def spdif_describe(cs):
    """Channel status in words (IEC 60958-3 consumer, AES3 professional)."""
    if cs[0] & 1:
        rate = {0: "rate not indicated", 1: "44.1 kHz", 2: "48 kHz", 3: "32 kHz"}[cs[0] >> 6]
        out = ["professional", "non-audio" if cs[0] & 2 else "audio", rate]
        if cs[0] & 0x20:
            out.append("source unlocked")
        if cs[2] & 0x3F == 0x2C:
            out.append("24 bit")
        origin = cs[6:10].rstrip(b"\0")
        if origin and all(32 <= c < 127 for c in origin):
            out.append(f'origin "{origin.decode()}"')
    else:
        rates = {0: "44.1 kHz", 2: "48 kHz", 3: "32 kHz", 8: "88.2 kHz", 10: "96 kHz",
                 12: "176.4 kHz", 14: "192 kHz"}
        out = ["consumer", "non-audio" if cs[0] & 2 else "audio",
               "copy permitted" if cs[0] & 4 else "copy protected",
               rates.get(cs[3] & 15, f"rate code {cs[3] & 15}")]
        if cs[4] & 15 == 0x0B:
            out.append("24 bit")
        elif cs[4] & 15 == 0x02:
            out.append("16 bit")
    return ", ".join(out)


STATUS_RATES = {0: 32000, 1: 44100, 2: 48000, 4: 64000, 5: 88200, 6: 96000,
                8: 128000, 9: 176400, 10: 192000}
RATES = [32000, 44100, 48000, 64000, 88200, 96000, 128000, 176400, 192000]


def cmd_status(fd, args):
    """What the unit reports about itself (all verified)."""
    regs = {w >> 16: w & 0xFFFF for (w,) in struct.iter_unpack("=I", os.read(fd, REGS_BYTES))}
    if 0x3F00 in regs:
        print(f"firmware {regs[0x3F00] >> 8}, DSP load {regs[0x3F00] & 0xFF} %")
    if 0x3D21 in regs:
        rate = RATES[regs[0x3D21]] if regs[0x3D21] < len(RATES) else "?"
        print(f"sample rate {rate} Hz (index {regs[0x3D21]}), clock source register {regs.get(0x3D20, 0)}")
    try:
        buf = bytearray(16)
        fcntl.ioctl(fd, UCX_HWDEP_STATUS, buf)
        st, version, serial_lo, serial_hi = struct.unpack("=4I", buf)
        rate = lambda code: f"{STATUS_RATES[code]} Hz" if code in STATUS_RATES else "none"
        clock = {7: "internal", 3: "word clock", 2: "optical", 1: "S/PDIF coaxial"}
        print(f"status {st:08x}: clock {clock.get(st >> 9 & 7, '?')} at {rate(st >> 24 & 0xF)}")
        for name, shift, lock, sync in (("word clock", 20, 4, 0x40), ("optical", 16, 2, 0x20),
                                        ("S/PDIF coaxial", 12, 1, 0x10)):
            # Without lock the rate field reads 0, which is also the 32 kHz code
            if st & (lock | sync):
                state = "sync" if st & sync else "lock"
                print(f"  {name} in: {state}, {rate(st >> shift & 0xF)}")
            else:
                print(f"  {name} in: no lock")
        print(f"version word {version:08x}, serial {serial_hi:08x}{serial_lo:08x}")
    except OSError:
        print("status words: not available (driver without UCX_HWDEP_STATUS)")
    # The unit's mirror of the REG1 settings (verified by writing each bit)
    if 0x3D41 in regs:
        srcs = ["internal", "S/PDIF coaxial", "optical", "word clock"]
        print(f"setting: clock source {srcs[regs[0x3D20]] if regs.get(0x3D20, 9) < 4 else regs.get(0x3D20)}, "
              f"optical out {'S/PDIF' if regs[0x3D41] else 'ADAT'}, "
              f"S/PDIF out {'professional' if regs.get(0x3D42) else 'consumer'}, "
              f"word clock out {'single speed' if regs.get(0x3D22) else 'follows rate'}, "
              f"word clock in {'terminated' if regs.get(0x3D40) else 'not terminated'}")
    # One analog switch each for Line In 5-8, Line Out 1-6 and Phones: 0407, 1208, 1808
    # (measured; 0507/1308 act only through the stereo link, which copies
    # their writes to the first channel; 0607/0707 and 1408-1708 switch
    # nothing).
    for name, items, a, b in (("Line In 5-8", ["-10 dBV", "+4 dBu", "Lo Gain"], 0x0407, 0x0507),
                              ("Line Out 1-6", ["-10 dBV", "+4 dBu", "Hi Gain"], 0x1208, 0x1308),
                              ("Phones", ["-10 dBV", "+4 dBu", "Hi Gain"], 0x1808, 0x1908)):
        if a in regs and b in regs:
            va, vb = (items[regs[r]] if regs[r] < 3 else str(regs[r]) for r in (a, b))
            print(f"{name} level: {va}" if va == vb else
                  f"{name} level: {va} ({a:04x}; {b:04x} reads {vb}, which switches nothing)")
    cs = spdif_status(args.card)
    if cs is None:
        print("S/PDIF in channel status: not available (driver without the IEC958 control)")
    elif any(cs):
        print(f"S/PDIF in channel status: {spdif_describe(cs)} ({cs[:5].hex(' ')} ...)")
    else:
        print("S/PDIF in channel status: none (needs an open PCM and a locked input)")
    # The driver's record of what it wrote (the unit reports neither)
    if 0x7F10 in regs:
        print(f"EQ+D record: {'on' if regs[0x7F10] else 'off'}")
    names = ["Line 1", "Line 2", "Line 3", "Line 4", "Line 5", "Line 6", "Phones L",
             "Phones R", "S/PDIF L", "S/PDIF R"] + [f"ADAT {n}" for n in range(1, 9)]
    known = [o for o in range(18) if 0x7F20 + o in regs]
    if known:
        on = [names[o] for o in known if regs[0x7F20 + o]]
        print(f"loopback: {', '.join(on) if on else 'off'}"
              + (f" ({18 - len(known)} outputs not written since load)" if len(known) < 18 else ""))
    if 0x3D01 in regs:
        print(f"output mutes {'enabled' if regs[0x3D01] else 'disabled'}")
    for name, reg in (("reverb", 0x3C00), ("echo", 0x3C20)):
        if reg in regs:
            print(f"{name} {'on' if regs[reg] else 'off'}")
    # Meter channels 18/19: FX send bus in the output banks, FX return in the input bank
    buf = bytearray(METERS_SIZE)
    fcntl.ioctl(fd, UCX_HWDEP_METERS, buf)
    for bank in range(METER_BANKS):
        off = bank * (4 + METER_BYTES)
        (length,) = struct.unpack_from("=I", buf, off)
        data = bytes(buf[off + 4:off + 4 + length])
        if length < METER_MIN_BYTES:
            continue
        (tag,) = struct.unpack_from("<I", data, METER_TAG_OFFSET)
        if tag not in (0x11111111, 0x33333333):
            continue
        peaks = []
        for ch in (18, 19):
            (peak,) = struct.unpack_from("<I", data, METER_PEAK_OFFSET + 4 * ch)
            peak &= ~0xF
            peaks.append(f"{20 * math.log10(peak / 2**27):.1f}" if peak else "-inf")
        print(f"FX {'return' if tag == 0x11111111 else 'send'} L/R: {' / '.join(peaks)} dBFS")


parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
parser.add_argument("--card", default="UCX")
sub = parser.add_subparsers(dest="cmd", required=True)
p = sub.add_parser("dsp")
p.add_argument("words", nargs="+")
p.set_defaults(func=cmd_dsp, mode=os.O_WRONLY)
p = sub.add_parser("regs")
p.add_argument("regs", nargs="*")
p.set_defaults(func=cmd_regs, mode=os.O_RDONLY)
p = sub.add_parser("meters")
p.add_argument("-r", "--rms", action="store_true")
p.set_defaults(func=cmd_meters, mode=os.O_RDONLY)
p = sub.add_parser("standalone")
p.set_defaults(func=cmd_standalone, mode=os.O_RDONLY)
p = sub.add_parser("status")
p.set_defaults(func=cmd_status, mode=os.O_RDONLY)
args = parser.parse_args()

fd = os.open(device(args.card), args.mode)
try:
    args.func(fd, args)
except OSError as e:
    sys.exit(f"{args.cmd}: {e.strerror}")
finally:
    os.close(fd)
