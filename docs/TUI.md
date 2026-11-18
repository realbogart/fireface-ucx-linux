# ucxtui

`ucxtui` is an editor for the config file of `ucxcfg` (`CONFIG.md`) that
runs in a terminal: every setting of the file on screen, changed with the
keyboard, saved back as JSON, and sent to the unit with the same code as
`ucxcfg apply`.

```
ucxtui FILE        edit FILE (it is created on save if it doesn't exist)
ucxtui             edit your own config, ~/.config/ucx/config.json
ucxtui -c CARD     another ALSA card than UCX
```

It needs a terminal of at least 64 × 16 characters; from about 125 columns
the forms have two columns. No curses: it writes escape sequences itself,
with 24-bit colour where `COLORTERM` says the terminal has it and 256 colours
elsewhere. Build with `make ucxtui` (`build/ucxtui`).

## Nothing is sent until you say so

The screen shows the config, not the unit. Changes stay in the program until
one of:

- **`a`, apply.** Lists every value that differs from what the driver holds
  for the unit; Enter sends them, in the order `ucxcfg apply` uses. Requested output
  mutes go on before gain changes and come off after the final levels are set.
  `p` writes all 720 mixer gains during the apply,
  like `ucxcfg apply --push`.
- **`L`, live.** From then on every change goes to the unit as it is made.
  If the unit differs when you press `L`, the same list comes up first.
  Switching phantom power on, or changing the clock source or a clock
  frequency, still asks, since one wrong key would be enough. `L` again
  switches live off. Opening another file or reading the unit's state
  switches it off too.

A yellow ● marks what differs from the unit: on a line, on a channel in the
list, on a page, and as a count at the top right ("● 12 to apply"). The line
under the form says what the unit has instead. "✓ in sync" means the driver's
record of the unit matches the config. The driver's record is not the whole
truth: the unit never reports its mixer gains (see `CONFIG.md`, "What the
unit reports"), so after a module load or after TotalMix has had the unit,
apply with `p`.

Live sends the differences when you change something; it does not watch the
unit. If another program changes a control meanwhile, the ● count shows it,
and your next change puts the config's value back.

Without a file, ucxtui edits your own config,
`$XDG_CONFIG_HOME/ucx/config.json` (`~/.config/ucx/config.json`): the one
that is applied whenever the unit appears (`CONFIG.md`, "Your own config").
If it doesn't exist yet, ucxtui starts from the state of the unit, and the
first save creates it.

Starting from the state of the unit (`r`, or no file of your own yet) takes
the driver's record as the config. Values the
driver doesn't know (most mixer gains after a module load) show as off, a
message says how many there are, and they count as differences: applying
writes them as shown. If the unit is playing through routes the driver has
not seen, that silences them, so start from a file in that case.

## The pages

`1`–`5` (or F1–F5, or the mouse) switch pages.

1. **Inputs.** A list of the inputs, with preamp gain, 48V, which of low cut,
   EQ, dynamics and Auto Level are on (`LEDA`), and a level meter. The form
   on the right has everything of the selected input.
2. **Outputs.** The same for the outputs, with volume in the list. The form
   has the output's settings, its mix (every playback channel and input as a
   source), and its DSP blocks. The last entry is the FX send bus.
3. **Routing.** The whole mixer as a grid, outputs down and sources across,
   changed in place: see "The Routing grid" below.
4. **Effects.** Reverb and echo, with meters of the FX send bus and the FX
   return.
5. **Hardware.** Clock (source, word clock, and the real frequency of each
   rate family, for varipitch), digital outputs, the three shared analog
   levels, EQ+D record, output mutes. To the right: what the unit says
   about its clock (rate, active source, lock and sync of the three
   inputs), the streams, and how much of the mixer the driver knows.

   The streams are shown, not set: their rate, period and buffer belong to
   the program that opened them (PipeWire's `clock.rate` and
   `clock.quantum`, or a JACK or ALSA program's own settings), and
   `urb_blocks` is a module parameter of the driver (2 or 4, read when a
   stream starts). The round trip is the unit's and the driver's part of a
   loopback, from measurements, for that rate and `urb_blocks`; the
   program's buffer comes on top. **Errors** counts, since the unit
   appeared, the stream errors the unit reported and the driver's restarts
   of the streams (after a failed transfer, or when the unit asked for
   one): the two numbers of "Errors" in RME's Windows dialog. They are
   transmission errors, not a program missing its deadline (PipeWire
   counts those: `pw-top`, column ERR). The line is missing with a driver older than the
   counters.

Meters show the peak since the previous read, 16 times a second: inputs
before their DSP blocks, outputs after their volume, and next to each source
in a mix a small bar of what that source carries. They are there whenever the
unit is, live or not.

## Keys

| Key | |
|---|---|
| `1`–`5`, F1–F5 | pages |
| ↑ ↓, `j` `k` | channel in the list, line in a form |
| Enter, → | from the list into the form |
| Tab, Shift-Tab | next column: list, form, second form column |
| Esc | up a level: from the form to the list, from the list to the row of pages |
| PgUp PgDn | previous / next channel, staying on the same line |
| Home End | first / last line |
| ← →, `h` `l`, `-` `+` | adjust (in the Routing grid the arrows move: see below) |
| Shift-← →, `,` `.` | adjust by the smallest step the unit has |
| Space | a switch: on / off. A route: off, and back to the gain it had |
| Enter | a number: type it. A switch: toggle. A choice: pick from a list |
| `d`, Del | back to the default (a route: off). On a heading, a channel or a page: all of it |
| `i` | a route: invert its phase |
| `s` | a route: show a source pair as two rows, or as one again |
| `m` | mute the selected output |
| `u`, `U` | undo, redo (also Ctrl-Z, Ctrl-R); 256 steps |
| `a` | apply |
| `L` | live on / off |
| `r` | start again from the state of the unit |
| Ctrl-S, `w` | save |
| `W` | save under another name |
| `o` | open another file |
| `?` | the keys |
| `q` | quit (asks if there are unsaved changes) |

`d` works at every level, on whatever has the focus:

- **A line:** that value.
- **A section heading** (INPUT, OUTPUT, MIX: PLAYBACK, EQ, DYNAMICS, CLOCK …;
  they can be selected like any line): everything under it, the section's
  own switch included.
- **A channel in the list:** every section of that channel, for an output
  its mix too.
- **A page:** everything on it. The row of pages gets the focus with Esc
  (once or twice), with ↑ from the top of a list, form or grid, or with a
  click on a page; ← → change page there and ↓ goes back in. `d` asks
  first, with the number of values it changes. On the Routing page it
  switches every route off.

Each is one step for undo. A section or channel reset leaves two things
alone: the stereo link, and the level a channel shares with others (Line In
5-8, Line Out 1-6, Phones; reset that on its own line or on the Hardware page). A
page reset puts the links of the page back at their defaults as well, so
the page is what a file without it would give. An output's defaults include
a volume of −65 dB.

Steps: one press of ← → is about a seventieth of the range, rounded to a
round number (1 dB for a volume, 0.5 dB for an EQ gain); frequencies move by
a sixth of an octave. Mixer gains move by 1 dB down to −40 dB, by 5 dB below,
and from −90 dB to off. Anything finer is typed: Enter, the number, Enter.

## The mouse

- **Sliders:** press anywhere on a slider and drag. The value moves by as
  much as the mouse does, in the steps of the arrow keys; the press itself
  changes nothing, so a slider never jumps to where you clicked (a volume
  doesn't leap to +6 dB under a stray click). The knob is yellow while held,
  and the mouse may leave the line. Dragging a route to the left end
  switches it off.
- **Numbers:** click the value at the right of a line to type it.
- **Switches:** click the `on` / `off` field.
- **Choices:** click `‹` or `›` to step, or the name for the list; in the
  list, click an entry.
- **Routing grid:** click a cell to put the cursor there, click it again to
  switch the route on or off.
- Pages, channels and lines are selected with a click; a click on the
  selected channel goes into its form. The wheel moves the selection and
  never changes a value.

What is under the mouse lights up: a line, a channel, a page, a grid cell.
On a line, the part that a click would act on shows it: a slider's knob
turns yellow, as while it is held, and its track brightens; a value is
underlined (click to type); a switch and the `‹` `›` arrows brighten. The
highlight goes away when you use the keyboard and comes back when the mouse
moves.

Hold Shift to select text with the terminal instead. The terminal has to
report mouse movement (xterm's mode 1003): xterm, kitty, foot, alacritty,
wezterm and tmux (with `mouse on`) do. Where it doesn't, clicks and drags
still work and nothing lights up.

## The Routing grid

Every gain of the mixer is a cell: ■ on, ø on with inverted phase, · off,
brighter for louder (and the gain as a number from about 160 columns). The
line under the grid says what the cell under the cursor is.

| Key | |
|---|---|
| ← ↑ ↓ →, `h` `j` `k` `l` | move |
| Space | route on / off (back to the gain it had, or 0 dB) |
| `+` `-` | gain, 1 dB (5 dB below −40 dB) |
| `.` `,` | gain, 0.1 dB |
| Enter | type the gain |
| `d`, Del | off |
| `i` | invert the phase |
| `s` | linked output: source pair as a stereo route, or as two mono sources |
| `g` | go to this route in the Outputs page, where the output's other settings are |

Outputs joined by a bracket (┐ ┘) are a linked pair, and a linked pair has
no single gains: where it meets a source pair, the square of four cells is
one stereo route, left to left and right to right, and a key anywhere in the
square changes that route (both of its cells light up). A source shown as
two mono rows in the pair's mix (`s`, or an unlinked input such as Mic 1)
goes to both sides, so its two cells change together. To set the four gains
one by one (a pan, a cross feed), unlink the output: `g`, then Stereo link.

## Stereo pairs

The file writes a linked pair as one entry (`"Phones L/R"`) and an unlinked
one as two (`"Phones L"`, `"Phones R"`). The lists do the same: a linked pair
is one line, and **Stereo link** in the form parts or joins it. Linking gives
the right channel the left one's values.

In the mix of a pair, a source pair is one row: a stereo route, left to left
and right to right. Two single rows instead mean each source goes to both
sides, centred (the only two things a pair entry in the file can say). `s`
switches between the two; since they sound different, it only does so while
the route is off. Anything else (a pan, different gains left and right)
needs the output unlinked, as in the file. In the mix of a single output,
`s` only changes how the two sources are shown, and joins them when their
gains are equal.

Off sources start out the way the source itself is linked: `Mic 1` and
`Mic 2` as two rows, `Line 5/6` as one.

## Saving

A save writes the shortest file for the config (`ucxcfg check -p` form):
defaults are left out, and a comment-free JSON file is rewritten as a whole.
The file is written next to the old one and renamed over it. What was
written is read back and compared with the screen before the save is
reported.

## How it is built

- `tui/term.c`: raw mode, a cell buffer sent as differences, key and mouse
  decoding (presses, releases, every motion of the mouse, focus leaving
  the window).
- `tui/doc.c`: the config being edited: strips (a channel or a linked pair),
  mix rows, links, undo. Every edit keeps linked pairs to what one file entry
  can say; `tests/doc.c` (`make check`) does 20000 random edits and checks
  that the saved file loads back as the same config.
- `tui/ucxtui.c`: pages and keys. A setting that is added to
  `ucx_settings[]` in `lib/config.c` shows up in its form by itself, under
  its key in a "MORE" section, until it gets a label in `metas[]` and a
  place in the form.
- `lib/device.c`: `ucx_dev_meters()` and `ucx_dev_status()` were added for
  the meters and the clock state.

Verified on the unit.
