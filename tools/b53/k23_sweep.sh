#!/bin/sh
# Kernel #23 sweep: inject +/-2^b for b = $LO..$HI, hold 2 s, undo, check the
# correction pair in /proc/b53_timer. Mode "load": b53_timer_test + b53_verify
# run throughout; mode "idle": nothing else running (heartbeat must catch it).
# busybox ash arithmetic is 32-bit: sizes are listed here, math is done in awk
# (doubles, exact to 2^53).
LO=33; HI=53; HOLD=2
SIZES="33:8589934592 34:17179869184 35:34359738368 36:68719476736 37:137438953472 38:274877906944 39:549755813888 40:1099511627776 41:2199023255552 42:4398046511104 43:8796093022208 44:17592186044416 45:35184372088832 46:70368744177664 47:140737488355328 48:281474976710656 49:562949953421312 50:1125899906842624 51:2251799813685248 52:4503599627370496 53:9007199254740992"
REF=$(sed -n 's/.*ref: on (timer \([0-9]*\).*/\1/p' /proc/b53_timer)
[ -n "$REF" ] || { echo "reference timer not running"; exit 1; }
comp() { sed -rn 's/^compensation: (-?[0-9]+) ticks.*/\1/p' /proc/b53_timer; }
fixn() { sed -rn 's/.*fixes: ([0-9]+).*/\1/p' /proc/b53_timer; }
ring() { sed -n '/^fixes (src/,/^$/p' /proc/b53_timer | grep '^ [rhb] ' | tail -n "$1"; }
bad() { awk '/^cpu +clamped/{t=1;next} NF==0{t=0} t&&NF==8{s+=$5+$6+$7} END{print s+0}' /proc/b53_timer; }
TOL=10000	# 125 us
# Judge one case from its corrections (stdin, one per fix: src raw comp_before
# error). The start and end of the injection are matched by size (closest to
# +v and -v); any other correction is a real counter jump that landed during
# the case, reported separately and allowed for in the compensation check.
judge() {
	awk -v m=$mode -v b=$b -v dir=$dir -v v=$v -v n=$((n1 - n0)) \
	    -v c0=$c0 -v c1=$c1 -v tol=$TOL '
	function a(x) { return x < 0 ? -x : x }
	{ src[NR] = $1; e[NR] = $4 }
	END {
		i = j = 0
		for (k = 1; k <= NR; k++) if (!i || a(e[k] - v) < a(e[i] - v)) i = k
		for (k = 1; k <= NR; k++) if (k != i && (!j || a(e[k] + v) < a(e[j] + v))) j = k
		d1 = i ? e[i] - v : v; d2 = j ? e[j] + v : v
		real = ""; rsum = 0
		for (k = 1; k <= NR; k++) if (k != i && k != j) { real = real " " e[k]; rsum += e[k] }
		dc = c1 - c0 + rsum	# a real jump stays corrected: comp moves by -error
		r = (n >= 2 && n <= 16 && a(d1) <= tol && a(d2) <= tol && a(dc) <= tol) ? "PASS" : "FAIL"
		printf "%-5s %4d %-4s %5d %10.0f %10.0f %10.0f  %s src=%s/%s%s\n", m, b, dir, n, d1, d2, dc, r, src[i], src[j], \
		    real == "" ? "" : "  real jump(s):" real
	}'
}
sweep() {	# mode
	mode=$1
	echo "===== mode $mode  bits $LO..$HI  hold ${HOLD}s  $(date +%T)"
	printf "%-5s %4s %-4s %5s %10s %10s %10s  %s\n" mode bit dir fixes err-inj undo+inj comp-diff result
	for bs in $SIZES; do
		b=${bs%%:*}; p=${bs#*:}
		for dir in fwd back; do
			[ $dir = fwd ] && v=$p || v=-$p
			c0=$(comp); n0=$(fixn)
			echo "inject $v" > /proc/b53_timer; sleep $HOLD
			echo "inject 0" > /proc/b53_timer; sleep 1
			c1=$(comp); n1=$(fixn); k=$((n1 - n0)); [ $k -gt 16 ] && k=16
			ring $k | judge
		done
	done
	echo "bad counters (rejected+bigheld+unstable): $(bad)"
}
N=$(( (HI - LO + 1) * 2 * (HOLD + 2) + 10 ))	# small: 32-bit is fine
/tmp/b53_verify $REF $N > /tmp/sweep_v.txt &
nice -n 10 /jffs/b53_timer_test $N 4 > /tmp/sweep_g.txt 2>&1 &
sleep 3
sweep load
wait
grep -E "Total reads|Total glitches" /tmp/sweep_g.txt
awk '{for(i=1;i<=NF;i++){if($i~/^drift=/)d=$(i+1); if($i~/^max_sleep=/)m=$(i+1)} if(m>mx)mx=m; if(n++&&(d-pd>5||pd-d>5))j++; pd=d} END{printf "verifier: %d s, worst sleep %.1f us, drift steps >5 ms: %d, final drift %s ms\n",n,mx,j,pd}' /tmp/sweep_v.txt
sleep 5
sweep idle
echo "===== DONE $(date +%T) uptime: $(cut -d' ' -f1 /proc/uptime)"
