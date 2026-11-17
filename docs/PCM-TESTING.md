# Testing PCM

`make check-pcm` runs without a UCX and needs Python 3 and a C compiler. It
compiles the driver's PCM callbacks with simulated USB and ALSA calls and
checks recovery ordering, frame alignment at 48 and 192 kHz, failed restart
handling, and waiting for a pending playback notification while capture keeps
running. At single, double, and quad speed it also checks every byte across
a ring wrap in both directions with two- and four-block transfers, and
checks S/PDIF status decoding across missing and early block markers.

## Comparing PCM callback cost

The benchmark builds the working tree and the specified Git revision with
the same compiler, flags, and simulated USB and ALSA calls. To reproduce
the x86-64 run below, pick an available CPU for `taskset` (CPU 8 here).

```sh
nix develop --command taskset -c 8 env CFLAGS='-O2 -Wall -Wextra -mgeneral-regs-only' \
  python3 tests/pcm.py --bench-against aec8a59
```

One slice runs one completion per endpoint in full duplex with the default
two-block transfers, a 128-frame ALSA ring, one transfer per period, and a
synchronized S/PDIF input. Each slice covers 333.3 µs of audio at these
rates. The benchmark times 360,000 consecutive slices per rate and reports
the median of nine batch averages after warm-up. The builds alternate running
first. Both pass the PCM correctness checks before timing.

| Sample rate | Baseline `aec8a59` | Optimized | Time saved |
| --- | ---: | ---: | ---: |
| 48 kHz | 31.9 ns/slice | 29.6 ns/slice | 7.2% |
| 96 kHz | 45.2 ns/slice | 40.1 ns/slice | 11.2% |
| 192 kHz | 465.0 ns/slice | 134.8 ns/slice | 71.0% |

Measured on a Ryzen 9 9950X3D with GCC 16.2. At 192 kHz the driver
interleaves two six-channel endpoint pairs. Fixed-size copies and handling
ring wrap once per transfer remove work from the inner loop. Primary
capture also selects the start sequence for pending streams under the same lock used to advance
their positions, saving one lock acquisition per slice at every rate.

These timings cover host-side callback code with hot buffers. USB scheduling,
kernel lock contention, ALSA wakeups, and converter delay are outside the
measurement. Cabled round-trip latency and XRUN rates under load require a
hardware run.

## On a Fireface UCX

Build the test module and ALSA test program with the running kernel's headers.
The Nix dev shell selects those headers on NixOS.

```sh
nix develop --command make module-pcm-test
```

Close applications using the UCX, then unload the driver and load
`build/pcm-kmod/kmod/snd-fireface-ucx.ko` from a privileged shell. Run
`build/pcm-device-test` from a shell that can write
`/sys/module/snd_fireface_ucx/parameters/pcm_fault`. The test opens
`hw:UCX` directly for exclusive access. It streams digital silence at 48 and
192 kHz and waits ten seconds between rates so each can exercise automatic
restarts.

At each rate, the test injects a USB error, a short capture transfer, and a
device restart word. It checks for an XRUN on both streams and resumes
duplex I/O after each fault. It then reopens playback 24 times while
capture runs. The test hook is compiled into this module build only.

After testing, reload the regular module from `kmod/snd-fireface-ucx.ko`
or the installed module. For a cabled measurement of the resulting
round-trip latency, use `tools/ratetest.sh` with the appropriate loopback
setup.
