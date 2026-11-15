# Fireface UCX USB protocol ("RME driver" mode)

What is known about how the first-generation Fireface UCX talks to a host
when it is not in Class Compliant mode. It was recovered from RME's Windows
driver (`fireface_usb_amd.sys` 1.2.77) and TotalMix FX, then checked on a
unit. `include/ucx_proto.h` is the detailed reference: every constant there
carries the address in the Windows binary it came from, and is marked
UNVERIFIED where it has not been confirmed on hardware. This page gives an
overview and the register map.

The DSP command words turned out to be the ones of RME's FireWire "latter"
protocol, so [snd-firewire-ctl-services] (`protocols/fireface/src/latter.rs`)
and [oscmix]'s notes on the Fireface UCX II and 802 were a great help.

[snd-firewire-ctl-services]: https://github.com/alsa-project/snd-firewire-ctl-services
[oscmix]: https://github.com/michaelforney/oscmix

> **Never send** vendor requests `0x18`/`0x19` or DSP words for registers
> `0x3e05`/`0x3e06`. They program the unit's flash (firmware or stored
> setups). The driver, its hwdep device and `ucxctl` refuse them.

## USB device

| | |
|---|---|
| Vendor IDs | `0x2a39` (RME), `0x0424` (Microchip) |
| Product ID | `0x3fc9` in RME driver mode, `0x3fb9` in Class Compliant mode (snd-usb-audio) |

All interfaces are vendor class (`0xff`) except MIDI. Audio runs over
**interrupt** endpoints, not isochronous ones as in Class Compliant mode.

| Interface | Endpoints | Use |
|---|---|---|
| 0 | `0x0e` / `0x8f` isochronous | not used by the Windows driver |
| 1 | `0x0c` bulk OUT, `0x8d` bulk IN | DSP command words and replies |
| 1 | `0x85` bulk IN | level meters |
| 2 | `0x07` / `0x86` bulk | USB-MIDI class, two ports |
| 3 | `0x09` / `0x88` bulk | flash data, 256-byte blocks (don't write) |
| 4 | `0x8b` interrupt IN | 4-byte device events |
| 5 | `0x01` OUT, `0x82` IN, plus `0x03` / `0x84` at quad speed | audio |

## Audio streams

Interface 5's alt setting is the speed class: 1 for 32–48 kHz, 2 for
64–96 kHz, 3 for 128–192 kHz. REG1 bits 5:4 pick the base rate within the
class (32k / 44.1k / 48k / 50k). Write them only when the rate changes: any
write, even of the current rate, makes the unit reload its stored mixer
setup a few seconds later.

Transfers are made of blocks. One block is one interrupt packet:

| Speed | Frames per block | Bytes per block | Channels |
|---|---|---|---|
| single | 8 | `0x240` | 18 |
| double | 16 | `0x380` | 14 |
| quad | 32 | `0x300` per endpoint pair | 12 (6 on each pair) |

There is no packet header. Frames are interleaved, and each sample is a
little-endian 32-bit word with 24 valid bits at the top (ALSA `S32_LE`,
24 msbits). On the S/PDIF input channels the low byte carries the S/PDIF
channel status bits (`UCX_SPDIF_CS*`).

A stream starts with:

1. `0x16` START_RESET, wValue 0, for wIndex 0–29 and `0x20`–`0x3d` (meaning unknown).
2. REG1 with the rate bits, if the rate changed.
3. `0x1d` START_AT: 0 starts now.
4. `0x14` CHANNELS with the channel mask, after the streams run. Without it
   the unit accepts OUT packets but the DSP never sees them.

## Vendor requests

Device requests, `bmRequestType` `0x40` (OUT) or `0xc0` (IN). The numbers
match RME's Babyface Pro (`SND_BBFPRO_USBREQ_*` in the kernel).

| Request | Name | Use |
|---|---|---|
| `0x10` | REG1 | masked write: wValue = bits, wIndex = mask. Clock and hardware settings, see below |
| `0x11` | STATUS | IN, 4 bytes: rate, clock source, lock and sync (`UCX_STATUS_*`) |
| `0x12` | MIXER | used for other models, not the UCX |
| `0x13` | CHANNELS_OFF | releases channels, same encoding as CHANNELS |
| `0x14` | CHANNELS | enables channels (`ucx_channels_req()`) |
| `0x15` | LOOPBACK | wIndex = output 0–29, wValue = 1/0: the output replaces the matching input in the recording |
| `0x16` | START_RESET | sent first at every start; no measured effect |
| `0x17` | REG2 | masked write; the UCX ignores the level fields Windows writes |
| `0x18`, `0x19` | FLASH_WRITE, FLASH_STATUS | **flash programming: never send** |
| `0x1a` | GAIN | not used by this driver |
| `0x1b` | CLOCK | rate-family frequencies. Don't send: it detunes the sample clock until a power cycle |
| `0x1c` | | IN, 4 bytes: probably the firmware version |
| `0x1d` | START_AT | 0 = now, or a USB frame number with bit 15 set |
| `0x1e`, `0x1f` | | IN, 4 bytes each: serial number |

### REG1 bits

| Bits | Meaning | Mirrored in |
|---|---|---|
| `0x0007` | clock source: 0 internal, 2 S/PDIF coaxial, 4 optical, 6 word clock | `0x3d20` (value / 2) |
| `0x0008` | word clock input termination | `0x3d40` |
| `0x0030` | base sample rate | `0x3d21` (rate index) |
| `0x0040` | word clock out stays at single speed | `0x3d22` |
| `0x0080` | S/PDIF coaxial out professional format | `0x3d42` |
| `0x0100` | record dry: capture without the input EQ and dynamics | none |
| `0x0400` | optical out sends S/PDIF instead of ADAT | `0x3d41` |
| `0x0800` | MIDI on | none |
| `0x8000` | large buffer (more than 32 blocks) | none |

The `0x3dxx` registers only mirror REG1. Change these settings through REG1.

## DSP command words

The mixer, the per-channel DSP and most settings are DSP registers. They are
written with 32-bit little-endian words sent as bulk OUT on `0x0c`:

```
bit 31      odd parity over the whole word
bits 30:16  register
bits 15:0   value (signed where it makes sense)
```

Per-channel registers are `channel << 8 | parameter`. The channels are the
18 inputs (0–17: Mic 1–2, Inst 3–4, Line 5–8, S/PDIF L/R, ADAT 1–8),
followed by the 18 outputs (18–35: Line 1–6, Phones L/R, S/PDIF L/R,
ADAT 1–8). Mixer gains have bit 30 set: register `0x4000 + 64·output + source`.

**The unit buffers 128 words and drains about 12 per millisecond.** If the
buffer overflows, every later transfer on `0x0c` times out for 20 s to over
2 minutes. Words are applied in order, sometimes with a delay of seconds.
The driver paces its writes at 0.5 ms per word.

### Readback

The unit reports register changes, including the ones made on the front
panel, as words in the same format on `0x8d`, as long as the host polls with
`0x3f00000N` (N cycling 0–15). Writing `0x3f99 = 0x0812` makes it send every
register (1636 of them). The dump has no mixer gains. The unit keeps a
record of the hardware-input part of the mixer (the "stand-alone input mix"),
but nothing that records the playback gains, loopback or the record-dry bit.
That is why `ucxcfg apply --push` exists.

### Register map

| Register | Meaning | Status |
|---|---|---|
| `ch<<8 \| 0x00` (inputs) | input mute: removes the input from the FX send only | verified |
| `… 0x01` | input FX send, 0.1 dB, −650 = off | verified |
| `… 0x02` | input stereo link | verified |
| `… 0x03`–`0x06` | parameters from TotalMix's table | no effect found |
| `… 0x07` | gain (ch 0–3, 0.1 dB) / reference level (ch 4–7) | verified: ch 4 is the one switch for inputs 5–8 |
| `… 0x08` | 48 V phantom (ch 0–1) / reference level (ch 2–3) | verified |
| `… 0x09` | Inst / Hi-Z (ch 2–3) | verified |
| `… 0x0a` | AutoSet | verified (ch 0) |
| `(18+o)<<8 \| 0x00` | output volume, 0.1 dB, −650…+60 | verified |
| `… 0x01` | output balance, −100 … +100 | verified |
| `… 0x02` | output mute (needs `0x3d01` = 1) | verified |
| `… 0x03` | FX return, 0.1 dB, −650 = off | verified |
| `… 0x04` | output stereo link | verified |
| `… 0x05` / `0x06` | parameters from TotalMix's table | no effect found |
| `… 0x07` | output phase invert | verified |
| `… 0x08` | output reference level | verified: ch 18 (`0x1208`) switches Line Out 1–6, ch 24 (`0x1808`) both sides of Phones |
| `… 0x20`–`0x22` | low cut on / frequency / slope (inputs and outputs) | verified |
| `… 0x40`–`0x4b` | 3-band EQ | verified |
| `… 0x60`–`0x67` | dynamics | verified (not attack/release) |
| `… 0x80`–`0x83` | Auto Level | verified (not rise time) |
| `… 0xa0`–`0xef` | stand-alone input mix, as the dump reports it | decoded |
| `(18+o)<<8 \| 0xe0 + input` | stand-alone input mix, write form (a record only; no effect on audio) | verified |
| `0x3c00`–`0x3c0d` | reverb | verified (not hold/release) |
| `0x3c20`–`0x3c26` | echo | verified |
| `0x3d00` | main out pair | no effect found |
| `0x3d01` | mute enable (global) | verified |
| `0x3d02`–`0x3d04` | dim amount / dim / recall volume | no effect found |
| `0x3d20` | clock source (mirror of REG1) | verified |
| `0x3d21` | sample rate index 0–8 (32 … 192 kHz), read-only | verified |
| `0x3d22`, `0x3d40`–`0x3d42` | REG1 mirrors, see above | verified |
| `0x3d43`–`0x3d46` | unknown | never written |
| `0x3f00` | host: idle poll. Unit: firmware (high byte), DSP load % (low byte) | verified |
| `0x3f02` | per-channel DSP status | observed |
| `0x3f01`, `0x3f03`, `0x3f10`, `0x3f50` | status reported while polled | unknown |
| `0x3f99` | `0x0812` = dump request | verified |
| `0x3f97`, `0x3f98`, `0x3f9e` | sent by TotalMix (setup recall, main out) | not sent, don't send |
| `0x3e05`, `0x3e06` | **flash store, never send** | |
| `0x4000 + 64·out + src` | mixer gain. src = input, or `0x20` + playback channel. Linear, 32768 = 0 dB, max 65536; negative inverts | verified |
| `0x4000 + 64·0x1e/0x1f + src` | FX send bus left / right, same sources | verified |

The encodings of each value (EQ types, slopes, dynamics, reverb parameters)
are in the driver's control table (`ucx_reg_ctls[]` in `kmod/ucx_mixer.c`)
and the config parser (`lib/config.c`).

## Level meters

A bulk read on `0x85` returns one bank of 20 channels: `u64` RMS[20] at
`0x00`, `u32` peak[20] at `0xa0` (scale 2^-27, low 4 bits are flags) and a
bank tag at `0xf0`. Five reads give the five banks:

| Tag | Bank |
|---|---|
| `0x11111111` | inputs after their DSP blocks (FX return on 18/19) |
| `0x55555555` | inputs before them |
| `0x22222222` | playback |
| `0x33333333` | outputs before the volume (FX send on 18/19) |
| `0x66666666` | outputs after the volume |

## The driver's hwdep device

`/dev/snd/hwC<card>D0` gives the audio group raw access without root
(`include/ucx_hwdep.h`): `write()` sends DSP words, `read()` returns the
driver's register table, and two ioctls read the meters and the status
words. `tools/ucxdsp.py` wraps it.

## Still unknown

- Interface 0 and the 4-byte events on `0x8b`.
- Requests `0x16` and `0x1b`: why Windows' clock words work for Windows.
- Registers with no effect found: inputs `0x03`–`0x06`, outputs `0x05`/`0x06`,
  `0x3d00`, `0x3d02`–`0x3d04`, `0x3d43`–`0x3d46`, the `0x3fxx` status words.
- Dynamics attack/release, Auto Level rise time, reverb hold/release scales.
