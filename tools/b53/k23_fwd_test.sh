#!/bin/sh
# Kernel #23 forward-jump injection tests (no stats reset: soak running). Each test runs the verifier (CLOCK_MONOTONIC and
# 1ms sleeps vs the reference timer) and b53_timer_test (counter reads on all
# 4 cores; any backward step is reported as a glitch).
REF=$(sed -n 's/.*ref: on (timer \([0-9]*\).*/\1/p' /proc/b53_timer)
[ -n "$REF" ] || { echo "reference timer not running"; exit 1; }
inj() { echo "inject $1" > /proc/b53_timer; echo "--- $(date +%T) inject $1"; }
state() { sed -n '1,5p;/^injected/,/^clamp size/p' /proc/b53_timer | grep -v "^clamp size"; }
run() {	# name, injection, hold seconds
	echo "===== $1: inject $2 for $3 s"
	T=$(( $3 + 10 ))
	/tmp/b53_verify $REF $T > /tmp/v.txt &
	V=$!
	nice -n 10 /jffs/b53_timer_test $T 4 > /tmp/g.txt 2>&1 &
	G=$!
	sleep 4; inj $2; sleep $3; inj 0
	wait $V; wait $G
	cat /tmp/v.txt
	grep -E "Total reads|Total glitches" /tmp/g.txt
	state
}
run "F1 forward 2^31 (26.8 s)" 2147483648 60
run "F2 forward 2^32 (53.7 s)" 4294967296 60
echo "===== DONE $(date +%T) uptime: $(cut -d' ' -f1 /proc/uptime)"
