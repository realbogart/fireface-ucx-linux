# Fireface UCX on Linux

A kernel driver and tools for the RME Fireface UCX (first generation, USB,
"RME driver" mode), built from reverse engineering. `README.md` says what it
is; `docs/PROTOCOL.md` and `include/ucx_proto.h` describe the protocol.

## Everything known is implemented

A finding is not finished when it is written down. Each one must end up in:

1. `docs/PROTOCOL.md` (the register map) and `include/ucx_proto.h`
   (constants, with the Windows driver address they came from, and
   UNVERIFIED until measured on a unit).
2. The driver (`kmod/`): an ALSA control (or other interface) for every
   register whose meaning is known.
3. The tools: `lib/` + `tools/ucxcfg.c` (the JSON config must be able to
   express it, and `docs/CONFIG.md` documents the key), `tui/ucxtui.c` (a
   label in `metas[]` and a place in its form; until then it shows under
   "MORE"; `docs/TUI.md` is its manual), and `tools/ucxdsp.py` where a raw
   view helps.

Registers whose meaning is only guessed, or that showed no effect, are
documented but get no control until they are understood.

## Layout

- `include/ucx_proto.h`, `include/ucx_hwdep.h`: protocol and hwdep interface.
- `kmod/`: snd-fireface-ucx (`ucx_mixer.c` holds the controls).
- `lib/`, `tools/ucxcfg.c`, `tests/config.sh`: the config tool (`make check`;
  `tests/fuzz_config.c` fuzzes its parser, `make fuzz`).
- `tui/`, `tests/doc.c`: ucxtui.
- `tools/`: `ucxctl.c` (libusb, without the driver) and the measurement
  helpers (`ucxdsp.py`, `inprobe.sh`, `routes.py`, `tone.py`, `looptest.c`).
- `nix/`, `udev/`, `systemd/`: packaging.

## Rules for working on the hardware

- Never send DSP words for registers `3e05` or `3e06` (store to flash) and
  never send vendor requests `0x18` or `0x19`. Ask before any other `3exx`
  word.
- Clock and hardware settings are REG1 bits (vendor request `0x10`, see
  `UCX_REG1_*`); `3d20`, `3d22`, `3d40`–`3d42` only mirror them. Say so
  before changing the clock source (a clock loop or missing source can stop
  audio), and ask before writing any `3d2x`/`3d4x` register as a DSP word.
- The unit buffers 128 DSP words and drains about 12 per ms; overflowing it
  wedges the command endpoint for up to two minutes.
- Test tones at −40 dBFS or lower. Phantom power stays off unless asked.
  Ask what is cabled before a loopback test.
- No sudo. Loading or reloading the module is the user's step: ask. Before
  changing calls into kernel APIs, read the source of the running kernel
  version.
- Verify on hardware where possible, and say which claims are measured and
  which are inferred. Don't commit unless asked.
