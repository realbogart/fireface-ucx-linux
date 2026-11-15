#!/bin/sh
# File format tests for ucxcfg; no device needed.  tests/config.sh build/ucxcfg
ucxcfg=${1:-build/ucxcfg}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
failed=0

# ok NAME JSON: valid, and print -> load -> print gives the same text
ok() {
	printf '%s' "$2" >"$tmp/in.json"
	if ! "$ucxcfg" check -p "$tmp/in.json" >"$tmp/a.json" 2>"$tmp/err"; then
		echo "FAIL $1: rejected: $(cat "$tmp/err")"; failed=1; return
	fi
	"$ucxcfg" check -p "$tmp/a.json" >"$tmp/b.json" 2>"$tmp/err"
	if ! cmp -s "$tmp/a.json" "$tmp/b.json"; then
		echo "FAIL $1: doesn't round-trip"; diff "$tmp/a.json" "$tmp/b.json"; failed=1
	fi
}

# same NAME JSON1 JSON2: two spellings of one state
same() {
	printf '%s' "$2" | "$ucxcfg" check -p - >"$tmp/a.json" 2>"$tmp/err"
	printf '%s' "$3" | "$ucxcfg" check -p - >"$tmp/b.json" 2>>"$tmp/err"
	if [ -s "$tmp/err" ] || ! cmp -s "$tmp/a.json" "$tmp/b.json"; then
		echo "FAIL $1: differ"; cat "$tmp/err"; diff "$tmp/a.json" "$tmp/b.json"; failed=1
	fi
}

# bad NAME JSON PATTERN: exit status 1 and a message matching PATTERN
bad() {
	printf '%s' "$2" | "$ucxcfg" check - >"$tmp/out" 2>"$tmp/err"
	status=$?
	if [ $status -ne 1 ] || ! grep -q -- "$3" "$tmp/err"; then
		echo "FAIL $1: status $status, message: $(cat "$tmp/err")"; failed=1
	fi
}

# differ NAME JSON1 JSON2: a pair entry is a linked pair, single entries are not
differ() {
	printf '%s' "$2" | "$ucxcfg" check -p - >"$tmp/a.json" 2>"$tmp/err"
	printf '%s' "$3" | "$ucxcfg" check -p - >"$tmp/b.json" 2>>"$tmp/err"
	if [ -s "$tmp/err" ] || cmp -s "$tmp/a.json" "$tmp/b.json"; then
		echo "FAIL $1: same"; cat "$tmp/err"; failed=1
	fi
}

# UCX_SEEDS=DIR: write each test's JSON to a file in DIR instead (the corpus
# seeds of `make fuzz`), and run nothing
if [ -n "$UCX_SEEDS" ]; then
	nseed=0
	seed() { nseed=$((nseed + 1)); printf '%s' "$1" >"$UCX_SEEDS/config-$nseed.json"; }
	ok() { seed "$2"; }
	bad() { seed "$2"; }
	same() { seed "$2"; seed "$3"; }
	differ() { seed "$2"; seed "$3"; }
fi

ok empty '{"version": 1}'
ok example "$(cat "$(dirname "$0")/../examples/desktop.json")"
ok everything '{"version": 1,
  "inputs": {
    "Mic 1": {"gain": 30, "phantom": true, "autoset": true},
    "Mic 2": {"gain": 0},
    "Inst 3/4": {"gain": 6.5, "hi-z": true, "level": "Lo Gain"},
    "Line 8": {}, "S/PDIF L": {}},
  "hardware": {"line-in-level": "Lo Gain", "line-out-level": "Hi Gain", "phones-level": "-10 dBV"},
  "outputs": {
    "Line 1/2": {"volume": -20.5, "mix": {"PCM 1/2": 0, "Mic 1": -12.34}},
    "Phones L/R": {"volume": -40},
    "ADAT 3": {"volume": -30, "mix": {"PCM 3/4": -6, "ADAT 1": 6, "S/PDIF L/R": -90}},
    "ADAT 4": {"volume": -31, "mix": {"PCM 4": -6}},
    "ADAT 7/8": {"volume": 6, "mix": {"PCM 17": -3, "PCM 18": -4, "Line 7/8": 0.5}}}}'
ok cross '{"version": 1, "outputs": {
    "ADAT 1": {"volume": 0, "mix": {"PCM 2": 0}}, "ADAT 2": {"volume": 0, "mix": {"PCM 1": 0}}}}'

ok dsp '{"version": 1, "mute-enable": false,
  "reverb": {"on": true, "type": "Space", "pre-delay": 30, "low-cut": 100, "room-scale": 1.5,
    "attack": 5, "hold": 400, "release": 5, "high-cut": 8000, "time": 4.9, "high-damp": 5000,
    "smooth": 50, "volume": 6, "width": 1},
  "echo": {"on": true, "type": "Pong Echo", "delay": 0.25, "feedback": 30, "high-cut": "8 kHz",
    "volume": -65, "width": 0},
  "inputs": {
    "Mic 1": {"fx-send": -12.5, "fx-mute": true, "lowcut": {"on": true, "freq": 80, "slope": 24},
      "eq": {"on": true, "low": {"type": "peak", "gain": -3.5, "freq": 200, "q": 0.7},
        "mid": {"gain": 20, "freq": 20000, "q": 5}, "high": {"type": "low pass", "freq": 20}},
      "dynamics": {"on": true, "gain": -30, "attack": 0, "release": 999, "comp-threshold": -60,
        "comp-ratio": 2.5, "exp-threshold": -99.9, "exp-ratio": 10},
      "autolevel": {"on": true, "max-gain": 18, "headroom": 3, "rise-time": 0.1}},
    "Line 5/6": {"eq": {"mid": {"gain": 1}}}},
  "outputs": {
    "Line 1/2": {"volume": 0, "mute": true, "balance": -100, "phase-invert": true,
      "fx-return": -20, "eq": {"on": true}, "mix": {"PCM 1/2": {"gain": -6, "invert": true}}},
    "ADAT 1": {"volume": 0, "mix": {"PCM 1": 0, "PCM 2": {"gain": 0, "invert": true}}},
    "ADAT 2": {"volume": 0},
    "FX Send L/R": {"mix": {"PCM 1/2": -10, "Mic 1": {"gain": -3, "invert": true}}}}}'
ok hardware '{"version": 1, "hardware": {"clock-source": "s/pdif", "optical-out": "s/pdif",
  "spdif-professional": true, "word-clock-single-speed": true, "word-clock-termination": false,
  "eqd-record": false, "line-in-level": "-10 dBV", "line-out-level": "-10 dBV"}}'
bad hw-in-level '{"version": 1, "hardware": {"line-in-level": "Hi Gain"}}' 'hardware.line-in-level: must be one of "-10 dBV"'
bad line-in-level '{"version": 1, "inputs": {"Line 7/8": {"level": "Lo Gain"}}}' 'the unit has one level for Line In 5-8, "hardware.line-in-level"'
bad line-out-level '{"version": 1, "outputs": {"Line 3": {"volume": 0, "level": "Hi Gain"}}}' 'the unit has one level for Line Out 1-6, "hardware.line-out-level"'
bad hw-clock '{"version": 1, "hardware": {"clock-source": "adat"}}' 'hardware.clock-source: must be one of "internal"'
bad hw-key '{"version": 1, "hardware": {"tms": true}}' 'hardware.tms: unknown key'
ok fx-single '{"version": 1, "outputs": {"FX Send R": {"mix": {"PCM 4": -1}}}}'
ok fx-left '{"version": 1, "outputs": {"FX Send L": {"mix": {"PCM 3": -2, "Mic 1": {"gain": -4, "invert": true}}},
  "FX Send R": {"mix": {"PCM 3": -2}}}}'
ok loopback '{"version": 1, "outputs": {"Line 5/6": {"volume": 0, "loopback": true},
  "ADAT 1": {"volume": -65, "loopback": true}, "ADAT 2": {"volume": -65}}}'
same loopback-default '{"version": 1, "outputs": {"ADAT 1/2": {"volume": 0}}}' \
  '{"version": 1, "outputs": {"ADAT 1/2": {"volume": 0, "loopback": false}}}'
bad loopback-type '{"version": 1, "outputs": {"ADAT 1/2": {"volume": 0, "loopback": 1}}}' 'loopback'
same dsp-defaults '{"version": 1}' \
  '{"version": 1, "mute-enable": true, "reverb": {"on": false, "volume": -3, "width": 0.6, "time": 2},
    "echo": {"delay": 0.6, "high-cut": "off"},
    "inputs": {"Mic 1": {"fx-send": -65, "fx-mute": false, "lowcut": {"freq": 50, "slope": 18},
      "eq": {"low": {"type": "shelf", "q": 1}}, "dynamics": {"comp-threshold": -30}}},
    "outputs": {"Line 1/2": {"volume": -65, "mute": false, "balance": 0, "fx-return": -65, "mix": {}}}}'
same invert-false \
  '{"version": 1, "outputs": {"ADAT 1/2": {"volume": 0, "mix": {"PCM 1/2": {"gain": -6, "invert": false}}}}}' \
  '{"version": 1, "outputs": {"ADAT 1/2": {"volume": 0, "mix": {"PCM 1/2": -6}}}}'
differ invert '{"version": 1, "outputs": {"ADAT 1/2": {"volume": 0, "mix": {"PCM 1/2": {"gain": -6, "invert": true}}}}}' \
  '{"version": 1, "outputs": {"ADAT 1/2": {"volume": 0, "mix": {"PCM 1/2": -6}}}}'
same pair-spellings \
  '{"version": 1, "outputs": {"ADAT 1/2": {"volume": -3, "mix": {"PCM 1/2": -6, "Mic 1": 0}}}}' \
  '{"version": 1, "outputs": {"ADAT 1/2": {"volume": -3.0, "mix": {"Mic 1": 0.0, "PCM 1/2": -6.00}}}}'
same defaults '{"version": 1}' \
  '{"version": 1, "inputs": {"Mic 1": {"gain": 0, "phantom": false}, "Inst 3": {"level": "+4 dBu"}},
    "hardware": {"line-in-level": "+4 dBu", "line-out-level": "+4 dBu", "phones-level": "+4 dBu"},
    "outputs": {"Line 1/2": {"volume": -65, "mix": {}}, "Phones L/R": {"volume": -65}}}'
same mono-sum \
  '{"version": 1, "outputs": {"ADAT 1/2": {"volume": 0, "mix": {"PCM 1": -6}}}}' \
  '{"version": 1, "outputs": {"ADAT 1/2": {"volume": 0, "mix": {"PCM 1": -6}}}, "inputs": {"ADAT 1/2": {}}}'

differ link-outputs \
  '{"version": 1, "outputs": {"ADAT 1/2": {"volume": -3}}}' \
  '{"version": 1, "outputs": {"ADAT 1": {"volume": -3}, "ADAT 2": {"volume": -3}}}'
differ link-mics '{"version": 1}' '{"version": 1, "inputs": {"Mic 1/2": {}}}'
differ unlink-lines '{"version": 1}' '{"version": 1, "inputs": {"Line 5": {}}}'
ok unlinked-default '{"version": 1, "outputs": {"ADAT 1": {"volume": -65}}, "inputs": {"ADAT 3": {}, "Mic 1/2": {"gain": 20}}}'

bad syntax '{"version": 1,}' ':1:15:'
bad duplicate '{"version": 1, "outputs": {}, "outputs": {}}' 'duplicate'
bad no-version '{}' '"version" is required'
bad version '{"version": 2}' 'version: must be 1'
bad top-key '{"version": 1, "output": {}}' 'output: unknown key'
bad reverb-key '{"version": 1, "reverb": {"size": 1}}' 'reverb.size: unknown key'
bad reverb-type '{"version": 1, "reverb": {"type": "Hall"}}' 'reverb.type: must be one of "Small Room"'
bad reverb-flat '{"version": 1, "reverb": true}' 'reverb: must be an object'
bad echo-delay '{"version": 1, "echo": {"delay": 2.5}}' 'echo.delay: must be 0 to 2 in steps of 0.01'
bad dump '{"version": 1, "unknown": ["x"]}' 'unknown: this is a dump'
bad no-output '{"version": 1, "outputs": {"Line 7": {"volume": 0}}}' 'outputs."Line 7": no such output'
bad no-volume '{"version": 1, "outputs": {"Line 1": {}}}' 'outputs."Line 1": "volume" is required'
bad volume-range '{"version": 1, "outputs": {"Line 1": {"volume": 6.1}}}' 'outputs."Line 1".volume: must be -65 to 6'
bad volume-step '{"version": 1, "outputs": {"Line 1": {"volume": -20.05}}}' 'outputs."Line 1".volume'
bad volume-type '{"version": 1, "outputs": {"Line 1": {"volume": "0"}}}' 'outputs."Line 1".volume'
bad overlap '{"version": 1, "outputs": {"Phones L/R": {"volume": 0}, "Phones L": {"volume": 0}}}' 'described twice'
bad odd-pair '{"version": 1, "outputs": {"Line 2/3": {"volume": 0}}}' 'outputs."Line 2/3": no such output'
bad source '{"version": 1, "outputs": {"Phones L/R": {"volume": 0, "mix": {"PCM 19": 0}}}}' 'outputs."Phones L/R".mix."PCM 19": no such source'
bad source-twice '{"version": 1, "outputs": {"Line 1": {"volume": 0, "mix": {"PCM 1/2": 0, "PCM 2": 0}}}}' 'listed twice'
bad gain-high '{"version": 1, "outputs": {"Line 1": {"volume": 0, "mix": {"PCM 1": 6.5}}}}' 'mix."PCM 1": gain must be'
bad gain-null '{"version": 1, "outputs": {"Line 1": {"volume": 0, "mix": {"PCM 1": null}}}}' 'mix."PCM 1": gain must be'
bad mix-type '{"version": 1, "outputs": {"Line 1": {"volume": 0, "mix": []}}}' 'outputs."Line 1".mix: must be an object'
bad out-key '{"version": 1, "outputs": {"Line 1": {"volume": 0, "solo": true}}}' 'outputs."Line 1".solo: unknown key'
bad out-mute '{"version": 1, "outputs": {"Line 1": {"volume": 0, "mute": 1}}}' 'outputs."Line 1".mute: must be true or false'
bad out-eq '{"version": 1, "outputs": {"Line 1": {"volume": 0, "eq": {"mid": {"gain": 21}}}}}' 'outputs."Line 1".eq.mid.gain: must be -20 to 20 in steps of 0.1'
bad out-eq-key '{"version": 1, "outputs": {"Line 1": {"volume": 0, "eq": {"mid": {"type": "peak"}}}}}' 'eq.mid.type: unknown key'
bad out-fx-send '{"version": 1, "outputs": {"Line 1": {"volume": 0, "fx-send": -10}}}' 'output Line 1 has no "fx-send" setting'
bad balance '{"version": 1, "outputs": {"Line 1": {"volume": 0, "balance": 101}}}' 'balance: must be -100 to 100'
bad slope '{"version": 1, "inputs": {"Mic 1": {"lowcut": {"slope": 9}}}}' 'lowcut.slope: must be 6 to 24 in steps of 6'
bad fx-volume '{"version": 1, "outputs": {"FX Send L/R": {"volume": 0}}}' 'the FX send bus has only a "mix"'
bad invert-obj '{"version": 1, "outputs": {"Line 1": {"volume": 0, "mix": {"PCM 1": {"gain": 0, "phase": true}}}}}' 'an object here must be'
bad invert-type '{"version": 1, "outputs": {"Line 1": {"volume": 0, "mix": {"PCM 1": {"gain": 0, "invert": 1}}}}}' 'an object here must be'
bad digital-level '{"version": 1, "outputs": {"ADAT 1": {"volume": 0, "level": "+4 dBu"}}}' 'output ADAT 1 has no "level" setting'
bad out-level '{"version": 1, "hardware": {"phones-level": "Lo Gain"}}' 'hardware.phones-level: must be one of "-10 dBV"'
bad phones-level '{"version": 1, "outputs": {"Phones R": {"volume": 0, "level": "Hi Gain"}}}' 'the unit has one level for Phones, "hardware.phones-level"'
bad no-input '{"version": 1, "inputs": {"Mic 3": {}}}' 'inputs."Mic 3": no such input'
bad mic-gain '{"version": 1, "inputs": {"Mic 1": {"gain": 5}}}' 'inputs."Mic 1".gain: must be 0 or'
bad inst-gain '{"version": 1, "inputs": {"Inst 3/4": {"gain": 6.3}}}' 'inputs."Inst 3/4".gain: must be 0 to 12'
bad phantom '{"version": 1, "inputs": {"Mic 1": {"phantom": 1}}}' 'inputs."Mic 1".phantom: must be true or false'
bad phantom-inst '{"version": 1, "inputs": {"Inst 3": {"phantom": true}}}' 'input Inst 3 has no "phantom" setting'
bad line-gain '{"version": 1, "inputs": {"Line 5": {"gain": 3}}}' 'input Line 5 has no "gain" setting'
bad in-key '{"version": 1, "inputs": {"Mic 1": {"pad": true}}}' 'inputs."Mic 1".pad: unknown key'
bad in-lowcut '{"version": 1, "inputs": {"Mic 1": {"lowcut": true}}}' 'inputs."Mic 1".lowcut: must be an object'
bad in-mute '{"version": 1, "inputs": {"Mic 1": {"mute": true}}}' 'input Mic 1 has no "mute" setting'
bad top-type '[]' 'top level must be an object'

[ -n "$UCX_SEEDS" ] && exit 0

# --full lists every setting and loads back as the same state
for f in '{"version": 1}' "$(cat "$(dirname "$0")/../examples/vocal-fx.json")"; do
	printf '%s' "$f" | "$ucxcfg" check -p - >"$tmp/a.json"
	printf '%s' "$f" | "$ucxcfg" check -p --full - >"$tmp/full.json"
	"$ucxcfg" check -p "$tmp/full.json" >"$tmp/b.json" 2>"$tmp/err"
	if ! cmp -s "$tmp/a.json" "$tmp/b.json" || [ "$(grep -c '"comp-ratio"' "$tmp/full.json")" -lt 20 ]; then
		echo "FAIL full: $(cat "$tmp/err")"; diff "$tmp/a.json" "$tmp/b.json" | head; failed=1
	fi
done

# apply without FILE: the user's own config, and a clear refusal (before any
# device is opened) when there is none. XDG_CONFIG_HOME wins over HOME; a
# relative one is ignored.
own() {	# NAME EXPECTED-PATH ENV...
	name=$1 want=$2
	shift 2
	env "$@" "$ucxcfg" apply -c no-such-card >/dev/null 2>"$tmp/err"
	st=$?
	if [ $st -ne 2 ] || ! grep -qF "$want doesn't exist" "$tmp/err"; then
		echo "FAIL own-$name: exit $st: $(cat "$tmp/err")"; failed=1
	fi
}
own xdg "$tmp/xdg/ucx/config.json" XDG_CONFIG_HOME="$tmp/xdg" HOME="$tmp/home"
own home "$tmp/home/.config/ucx/config.json" -u XDG_CONFIG_HOME HOME="$tmp/home"
own relative "$tmp/home/.config/ucx/config.json" XDG_CONFIG_HOME=rel HOME="$tmp/home"
# With the file there, it is what gets applied (the missing card is the error)
mkdir -p "$tmp/xdg/ucx" && printf '{"version": 1}' >"$tmp/xdg/ucx/config.json"
env XDG_CONFIG_HOME="$tmp/xdg" "$ucxcfg" apply -c no-such-card >/dev/null 2>"$tmp/err"
st=$?
[ $st -eq 3 ] || { echo "FAIL own-present: exit $st: $(cat "$tmp/err")"; failed=1; }

[ $failed -eq 0 ] && echo "config tests passed"
exit $failed
