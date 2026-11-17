# Testing PCM recovery

`make check-pcm` runs without a UCX and needs Python 3 and a C compiler. It
compiles the driver's PCM callbacks with simulated USB and ALSA calls and
checks recovery ordering, frame alignment at 48 and 192 kHz, failed restart
handling, and waiting for a pending playback notification while capture keeps
running.

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
