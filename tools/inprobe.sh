#!/usr/bin/env bash
# Measure how an input register changes a looped-back test tone.
#
#   tools/inprobe.sh REG METER_CH VALUE...
#   tools/inprobe.sh 0208 2 1 0 2 1    # input 3 reference level
#
# Plays tools/tone.py on playback channel 0 (Line Out 1 with the usual
# routing), writes each VALUE (decimal, may be negative) to DSP register REG
# and prints the input meters of METER_CH: bank 55555555 ("pre", the raw
# input) and bank 11111111 ("post", after the channel's EQ). Cable Line Out 1 to the
# input being probed. REG's original value is restored on exit. Needs no
# root: it goes through the driver's hwdep device (tools/ucxdsp.py).
# FREQ=<Hz> changes the tone (default 440), SETTLE=<s> the wait after each
# write (default 2).
set -u

dir=$(dirname "$0")
ucxdsp() { python3 "$dir/ucxdsp.py" "$@"; }

if [ $# -lt 3 ]; then
	sed -n '2,14s/^# \{0,1\}//p' "$0" >&2
	exit 1
fi
reg=$1 ch=$2
shift 2

# Peaks of meter channel $1 in banks $2...; the first read discards the old peak
peak() {
	local ch=$1 meters bank
	shift
	ucxdsp meters >/dev/null
	sleep 1
	meters=$(ucxdsp meters)
	for bank; do
		echo "$meters" | grep "^bank $bank" | grep -o " $ch:[-0-9.]*" | cut -d: -f2
	done | paste -sd/
}

dsp() {
	ucxdsp dsp "$reg$(printf %04x $(($1 & 0xffff)))"
}

orig=$(ucxdsp regs "$reg" | awk '{ print $2 }')

# aplay holds the device until it has fully exited
pkill -x aplay
while pgrep -x aplay >/dev/null; do sleep 0.1; done
python3 "$dir/tone.py" 0 --seconds 600 --freq "${FREQ:-440}" 2>/dev/null |
	aplay -q -D hw:UCX -t raw -c 18 -f S32_LE -r 48000 &
aplay=$!

cleanup() {
	[ -n "$orig" ] && dsp $((16#$orig)) && echo "restored $reg=$orig"
	kill "$aplay" 2>/dev/null
	wait "$aplay" 2>/dev/null
}
trap cleanup EXIT

sleep 2
# A tone on the meters could come from someone else's aplay
if ! kill -0 "$aplay" 2>/dev/null; then
	echo "aplay failed to start; see its error above" >&2
	exit 1
fi
play=$(peak 0 22222222)
echo "playback ch 0: ${play:-none} dBFS"
if [ -z "$play" ]; then
	echo "no tone reaching the device; is something else using hw:UCX?" >&2
	exit 1
fi
echo "$reg was ${orig:-unknown}"

for v; do
	dsp "$v"
	sleep "${SETTLE:-2}"
	echo "$reg=$v: ch $ch pre/post: $(peak "$ch" 55555555 11111111) dBFS"
done
