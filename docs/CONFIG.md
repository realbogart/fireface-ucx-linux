# The ucxcfg config file

`ucxcfg` makes the Fireface UCX match a JSON file. This is the reference for
that file (format version 1). The parser is `lib/config.c`; the registers behind
each setting are in `PROTOCOL.md`.

```
ucxcfg check FILE            validate, no device needed
ucxcfg check -p FILE         print FILE in its shortest form
ucxcfg check -p --full FILE  print every channel and key, defaults included
ucxcfg apply -n FILE         show what would change
ucxcfg apply FILE            make the unit match FILE
ucxcfg apply --push FILE     the same, then send all 720 mixer gains again
ucxcfg apply                 apply your own config (below)
ucxcfg dump [--full]         write what the driver knows as a config
```

`ucxtui FILE` edits the same file on screen and can apply it, also change
by change while you listen: see `TUI.md`.

## Your own config, applied whenever the unit appears

`$XDG_CONFIG_HOME/ucx/config.json`, normally `~/.config/ucx/config.json`, is
the setup you want the unit in. `ucxtui` opens it when started without a
file (and creates it on the first save), and `ucxcfg apply` without a file
applies it.

The unit keeps its state by itself, through a sleep and even when it is
switched off, but after the driver loads it can't read back what the unit
never reports (the mixer's playback gains, loopback, EQ+D record). So
`udev/70-snd-fireface-ucx.rules` starts the user unit `ucxcfg-apply.service`
(`systemd/ucxcfg-apply.service.in`) when the card appears: when the unit
is plugged in or switched on, and after a module reload. The unit also
starts with your session (`default.target`), because udev's request only
reaches a user session that is already running: at boot the card is there
first. Without a unit plugged in it does nothing. It runs
`ucxcfg apply --push` on the file, 2 s after the card appears so the state
the unit reports has arrived. A user without the file is left alone. Nothing
runs after a system sleep: the driver keeps what it knew.

The NixOS module (`nix/module.nix`) installs the rule, the unit and the
tools. Elsewhere, `make install-ucxcfg` installs them under `PREFIX`; udev
and systemd must find them there (`/usr` rather than `/usr/local` on most
systems), and `systemctl --user enable ucxcfg-apply` adds the start with
the session. To see what it did: `journalctl --user -u ucxcfg-apply`.

## The file describes the whole unit

A file is not a list of changes. Everything it does not mention is set to its
default on `apply`: routes off, output volumes at −65 dB, DSP blocks off,
effects off. So a file with one output in it silences all the others.

The defaults are the unit's own factory values, so a short file is normal:

```json
{
  "version": 1,
  "outputs": {
    "Line 1/2":   { "volume": 0,   "mix": { "PCM 1/2": 0 } },
    "Phones L/R": { "volume": -40, "mix": { "PCM 1/2": 0 } }
  }
}
```

Top-level keys:

| Key | Content |
|---|---|
| `version` | required, `1` |
| `inputs` | settings per input: preamp, FX send, low cut, EQ, dynamics |
| `outputs` | per output: volume, **routes (`mix`)**, and the same DSP blocks |
| `hardware` | clock source and digital I/O options |
| `mute-enable` | `false` makes the unit ignore every output `mute` (default `true`) |
| `reverb`, `echo` | the two effects |

Unknown keys, wrong types, values out of range or off their step, and
duplicate keys are errors: `check` names the place and the allowed values.

## Channel names

| | Names |
|---|---|
| Inputs | `Mic 1`, `Mic 2`, `Inst 3`, `Inst 4`, `Line 5` … `Line 8`, `S/PDIF L`, `S/PDIF R`, `ADAT 1` … `ADAT 8` |
| Outputs | `Line 1` … `Line 6`, `Phones L`, `Phones R`, `S/PDIF L`, `S/PDIF R`, `ADAT 1` … `ADAT 8` |
| Playback (what the computer plays) | `PCM 1` … `PCM 18`, in the order of the outputs |
| FX bus | `FX Send L`, `FX Send R` (under `outputs`, only a `mix`) |

Channels come in fixed pairs: 1/2, 3/4, and so on. A pair is written with a
slash: `"Mic 1/2"`, `"Line 5/6"`, `"Phones L/R"`, `"S/PDIF L/R"`,
`"ADAT 7/8"`, `"PCM 1/2"`, `"FX Send L/R"`. `"Line 2/3"` is not a pair.

Under `inputs` and `outputs`, the way a channel is written also sets its
stereo link:

- A pair entry (`"Phones L/R"`) links the two channels; every key applies to
  both.
- Single entries (`"ADAT 5"`, `"ADAT 6"`) unlink them, so they can differ.
- A pair the file does not mention gets the unit's default: linked, except
  Mic 1/2 and Inst 3/4.

A channel may appear once only, as a pair or on its own.

## Routes: `mix`

A route is written under the output it feeds, as `source: gain`:

```json
"outputs": {
  "Phones L/R": {
    "volume": -20,
    "mix": {
      "PCM 1/2": 0,
      "Mic 1": -6,
      "Line 5/6": -3.5,
      "Line 7": { "gain": 0, "invert": true }
    }
  },
  "ADAT 7": { "volume": 0, "mix": { "Line 7": 0 } },
  "ADAT 8": { "volume": 0, "mix": { "Line 8": 0 } }
}
```

- **Source:** an input, a playback channel, or a pair of either.
- **Gain:** dB from −90 to 6, at most two decimals. 0 is unity.
- **Phase:** `{"gain": dB, "invert": true}` instead of the number.
- **Off:** leave the source out. There is no "off" value.

How pairs connect:

| Output | Source | Result |
|---|---|---|
| pair | pair | left to left, right to right; no cross feed |
| pair | single | the source on both sides at the gain (centred mono) |
| single | pair | both sources into that output at the gain (mono sum) |
| single | single | that one route |

Anything else (a pan position, a cross feed, different gains left and right)
is written with single entries on the output or the source side:

```json
"Phones L": { "volume": -20, "mix": { "Mic 1": -3, "Mic 2": -12 } },
"Phones R": { "volume": -20, "mix": { "Mic 1": -12, "Mic 2": -3 } }
```

That also unlinks Phones L and R; give both the same `volume` and settings
to keep them equal.

Measured order in an output: the sum of its `mix` routes, then its EQ, then
`volume`, with `mute` and `balance` acting after the volume. Where
`fx-return` joins has not been measured.

**What the unit reports.** The unit never reports its mixer gains. After the
driver loads, playback routes are unknown and input routes come from a record
the driver keeps in the unit. `apply` writes what is unknown or different;
`apply --push` writes all of it, and is the way to be sure the unit and the
file agree (after power-up, or after TotalMix has had the unit).

### Recording what the computer plays

`examples/desktop.json` routes `PCM 1/2` to `ADAT 1/2` as well and sets
`"loopback": true` there. The computer then records that output instead of
the ADAT 1/2 inputs: capture channels 11/12 (`AUX10`/`AUX11` in PipeWire's
pro-audio profile) carry the desktop sound. The loopback is taken before the
output's `volume`, so the volume doesn't change the recording. ADAT 1/2 is
used because the stream keeps channels 11/12 at every sample rate (12 to 18
channels); the loopback has been tried at 48 kHz only.

Verified 2026-11-13: a −40 dBFS tone on PCM 17, temporarily routed to ADAT 1
only, recorded at −40.0 dBFS on capture channel 11 and nowhere else.

## Outputs

| Key | Value | Default |
|---|---|---|
| `volume` | **required.** dB, −65 to 6, steps of 0.1 | −65 if the output is not in the file |
| `mix` | routes, see above | all off |
| `mute` | `true` / `false` | `false` |
| `balance` | −100 (left) to 100 (right) | 0 |
| `phase-invert` | `true` / `false` | `false` |
| `fx-return` | dB, −65 (off) to 0, steps of 0.1: reverb and echo into this output | −65 |
| `loopback` | `true` / `false`: the computer records this output (after its EQ, before `volume`) in place of the input of the same number | `false` |
| `lowcut`, `eq`, `dynamics`, `autolevel` | see "DSP blocks" | off |

`"FX Send L/R"` (or `L`, `R` alone) takes only `mix`: playback channels sent
to the effects. Inputs are sent with their own `fx-send`.

## Inputs

| Key | Value | Default | Channels |
|---|---|---|---|
| `gain` | dB: 0, or 10 to 65 in whole steps | 0 | Mic 1, 2 |
| `gain` | dB: 0 to 12, steps of 0.5 | 0 | Inst 3, 4 |
| `phantom` | `true` / `false`: 48 V | `false` | Mic 1, 2 |
| `hi-z` | `true` / `false` | `false` | Inst 3, 4 |
| `autoset` | `true` / `false`: the unit lowers the gain on overload | `false` | Mic 1, 2, Inst 3, 4 |
| `level` | `"-10 dBV"`, `"+4 dBu"`, `"Lo Gain"` | `"+4 dBu"` | Inst 3, 4 (Line 5–8 share `hardware.line-in-level`) |
| `fx-send` | dB, −65 (off) to 0, steps of 0.1 | −65 | all |
| `fx-mute` | `true` / `false`: silences the FX send only | `false` | all |
| `lowcut`, `eq`, `dynamics`, `autolevel` | see "DSP blocks" | off | all |

An input has no fader of its own: how loud it is on an output is the gain of
the route in that output's `mix`. What the computer records is the input
after its DSP blocks (or before them, see `hardware.eqd-record`).

## DSP blocks

The same four objects on every input and output. Each key is optional.

```json
"Mic 1": {
  "gain": 30,
  "lowcut":   { "on": true, "freq": 80, "slope": 12 },
  "eq":       { "on": true, "mid": { "gain": -3, "freq": 400, "q": 1.4 } },
  "dynamics": { "on": true, "comp-threshold": -20, "comp-ratio": 3 }
}
```

| Key | Value | Default |
|---|---|---|
| `lowcut.on` | `true` / `false` | `false` |
| `lowcut.freq` | Hz, 20 to 500 | 50 |
| `lowcut.slope` | dB/octave: 6, 12, 18, 24 | 18 |
| `eq.on` | `true` / `false` | `false` |
| `eq.low.type` | `"peak"`, `"shelf"` | `"shelf"` |
| `eq.high.type` | `"peak"`, `"shelf"`, `"low pass"` | `"shelf"` |
| `eq.low.gain`, `eq.mid.gain`, `eq.high.gain` | dB, −20 to 20, steps of 0.1 | 0 |
| `eq.low.freq`, `eq.mid.freq`, `eq.high.freq` | Hz, 20 to 20000 | 100, 1000, 10000 |
| `eq.low.q`, `eq.mid.q`, `eq.high.q` | 0.7 to 5, steps of 0.1 | 1 |
| `dynamics.on` | `true` / `false` | `false` |
| `dynamics.gain` | make-up gain, dB, −30 to 30, steps of 0.1 | 0 |
| `dynamics.attack` | ms, 0 to 200 | 10 |
| `dynamics.release` | ms, 100 to 999 | 100 |
| `dynamics.comp-threshold` | dB, −60 to 0, steps of 0.1 | −30 |
| `dynamics.comp-ratio` | 1 to 10, steps of 0.1 | 1 |
| `dynamics.exp-threshold` | dB, −99.9 to −20, steps of 0.1 | −60 |
| `dynamics.exp-ratio` | 1 to 10, steps of 0.1 | 1 |
| `autolevel.on` | `true` / `false` | `false` |
| `autolevel.max-gain` | dB, 0 to 18, steps of 0.1 | 6 |
| `autolevel.headroom` | dB, 3 to 12, steps of 0.1 | 6 |
| `autolevel.rise-time` | s, 0.1 to 9.9, steps of 0.1 | 5 |

## `hardware`

```json
"hardware": { "clock-source": "internal", "optical-out": "adat" }
```

| Key | Value | Default |
|---|---|---|
| `clock-source` | `"internal"`, `"s/pdif"`, `"optical"`, `"word clock"` | `"internal"` |
| `optical-out` | `"adat"`, `"s/pdif"` | `"adat"` |
| `spdif-professional` | `true` / `false`: channel status of the S/PDIF output | `false` |
| `word-clock-single-speed` | `true` / `false` | `false` |
| `word-clock-termination` | `true` / `false` | `true` |
| `eqd-record` | `true`: record inputs after low cut, EQ and dynamics | `true` |
| `line-in-level` | `"-10 dBV"`, `"+4 dBu"`, `"Lo Gain"`: reference level of all four rear inputs, Line 5–8 | `"+4 dBu"` |
| `line-out-level` | `"-10 dBV"`, `"+4 dBu"`, `"Hi Gain"`: reference level of Line Out 1–6 | `"+4 dBu"` |
| `phones-level` | `"-10 dBV"`, `"+4 dBu"`, `"Hi Gain"`: reference level of Phones L and R | `"+4 dBu"` |

The unit has one level switch for Line In 5–8, one for Line Out 1–6 and
one for both sides of Phones, although TotalMix shows a Level per channel
(measured; see the register map in `PROTOCOL.md`). A file that gives one of those channels its own `level` is
refused with a pointer to these keys; outputs have no `level` key at all
(before 2026-11-14 Phones had one: move it to `phones-level`).
`"Hi Gain"` raises Line Out 1–6 by 6 dB, the speakers included, and
`phones-level` `"Hi Gain"` raises Phones by 6 dB.

A clock source with nothing connected to it can stop audio. `"word clock"`
has not been tried on a unit; its place in the list is inferred.

## `reverb` and `echo`

Both are fed by the FX bus (`fx-send` of the inputs, `mix` of
`"FX Send L/R"`) and come back on each output through its `fx-return`.

```json
"reverb": { "on": true, "type": "Medium Room", "volume": -6 }
```

| `reverb` key | Value | Default |
|---|---|---|
| `on` | `true` / `false` | `false` |
| `type` | `"Small Room"`, `"Medium Room"`, `"Large Room"`, `"Walls"`, `"Shorty"`, `"Attack"`, `"Swagger"`, `"Old School"`, `"Echoistic"`, `"8plus9"`, `"Grand Wide"`, `"Thicker"`, `"Envelope"`, `"Gated"`, `"Space"` | `"Small Room"` |
| `pre-delay` | ms, 0 to 999 | 20 |
| `low-cut` | Hz, 20 to 500 | 20 |
| `high-cut` | Hz, 5000 to 20000 | 20000 |
| `room-scale` | 0.5 to 3, steps of 0.01 | 1 |
| `smooth` | 0 to 100 | 100 |
| `width` | 0 to 1, steps of 0.01 | 0.6 |
| `volume` | dB, −65 (off) to 6, steps of 0.1 | −3 |
| `attack`, `hold`, `release` | ms, 5 to 400 (attack measured on Envelope; hold and release not measured) | 100, 80, 200 |
| `time` | s, 0.1 to 4.9, steps of 0.1 (Space) | 2 |
| `high-damp` | Hz, 5000 to 20000 (measured on Space) | 18000 |

| `echo` key | Value | Default |
|---|---|---|
| `on` | `true` / `false` | `false` |
| `type` | `"Stereo Echo"`, `"Stereo Cross"`, `"Pong Echo"` | `"Stereo Echo"` |
| `delay` | s, 0 to 2, steps of 0.01 | 0.6 |
| `feedback` | 0 to 100 | 50 |
| `high-cut` | `"off"`, `"16 kHz"`, `"12 kHz"`, `"8 kHz"`, `"4 kHz"`, `"2 kHz"` | `"off"` |
| `volume` | dB, −65 (off) to 6, steps of 0.1 | −3 |
| `width` | 0 to 1, steps of 0.01 | 0.6 |

## Dumps and `unknown`

`ucxcfg dump` writes the unit's state as the driver knows it. Values the
driver has not seen (playback routes after a load, mostly) are listed under
a top-level `"unknown"` key. `check` and `apply` refuse a file with that
key: decide what those values should be, write them in, and remove the key.
`dump --assume-off` writes them as off instead. A mixer gain the unit holds
outside what a file can say is written as the nearest end: TotalMix's top
fader position (+6.02 dB) as 6, and the smallest gain (−90.3 dB) as −90.

## Not in the file

Sample rate (set by the audio stream), storing to the unit's flash, and
TotalMix's own notions: snapshots, groups, solo, pan as a position, and the
main-out / phones assignment.

## Testing the parser

`make check` runs the file format tests (`tests/config.sh`: every `ok` file
must print and load back to the same text, every `bad` one must be refused
with the expected message), the protocol helpers and the TUI's edits. No
device is needed.

`make fuzz` runs libFuzzer on the parser (`tests/fuzz_config.c`) for
`FUZZ_TIME` seconds (60 by default). Every input must either be refused with a
message, or print, with and without `--full`, as a file that loads back as the
same config and prints the same again. It needs clang and jansson only, and is
not part of `make all` or `make check`. The corpus is kept in
`build/fuzz/corpus`, seeded from `examples/` and the JSON in `tests/config.sh`;
a failing input is saved as `build/fuzz/crash-*` (or `timeout-*`), and
`build/fuzz/fuzz_config FILE` replays it. More libFuzzer options go in
`FUZZ_ARGS`, for example `make fuzz FUZZ_TIME=1800 FUZZ_ARGS=-fork=4`.
