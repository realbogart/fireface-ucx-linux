#!/usr/bin/env bash
# Run tools/looptest.c at every sample rate the driver offers.
#
#   tools/ratetest.sh [RATE...]        # default: all nine rates
#
# Needs the loopback cable looptest uses (Line Out 1 -> Line In 5) and
# `make build/looptest`. Each rate prints the measured frame rate, the
# round-trip latency and the glitch count; see looptest.c. Extra looptest
# options go in LOOPTEST_ARGS, e.g. LOOPTEST_ARGS="-p 64 -s 10".
dir=$(dirname "$0")
rates=${*:-32000 44100 48000 64000 88200 96000 128000 176400 192000}
fail=0
for rate in $rates; do
	# shellcheck disable=SC2086
	"$dir/../build/looptest" -r "$rate" ${LOOPTEST_ARGS:-} || fail=1
done
exit $fail
