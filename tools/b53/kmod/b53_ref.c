// SPDX-License-Identifier: GPL-2.0
/*
 * b53_ref: measure Brahma-B53 arch counter offsets against an independent
 * Broadcom peripheral timer (200 MHz, separate MMIO path).
 *
 * The two clocks come from different sources (~150 ppm apart, slowly
 * wandering), so only sudden changes are compared: between two back-to-back
 * samples (IRQs off), ddev = arch step - ref step (in arch ticks) is ~0.
 * An arch offset starts with ddev = -X and ends with ddev = +X; the ref
 * timer measures how long it lasted. Steps across the 1 ms sleeps between
 * windows are not compared, so an offset that ends during a sleep is
 * reported with its last-seen time as a lower bound.
 *
 * insmod b53_ref.ko secs=60 cpu=2   (results in dmesg; init returns -EAGAIN
 * so the module unloads itself and frees the timer)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <asm/sysreg.h>
#include <asm/barrier.h>
#include <bcm_ext_timer.h>

MODULE_LICENSE("GPL");

static int secs = 60;
module_param(secs, int, 0444);
static int cpu = 2;
module_param(cpu, int, 0444);

#define TIMER_CNT_PHYS	0xff800420UL	/* TimerCnt0; TimerCntN at +8*N */
#define TIMER_CNT_MASK	0x3FFFFFFFFFFFFFFFULL
#define WINDOW_TICKS	80000ULL	/* 1 ms of arch time per IRQ-off window */
#define THR		32		/* |ddev| above this is a jump (arch ticks) */
#define MAX_EV		48

struct ev {
	u64 arch;		/* arch value before the jump */
	s64 arch_step;		/* raw arch step into the bad sample */
	s64 ref_step;		/* ref step for the same samples (arch ticks) */
	s64 x;			/* offset (negative = arch low) */
	u64 dur;		/* arch ticks from start to end, by the ref clock */
	bool closed;		/* end seen inside a window */
};

static struct ev evs[MAX_EV];
static int nev;
static unsigned long size_hist[64], dur_hist[64], open_end;
static unsigned long fwd_jumps, short_blips;
static s64 jitter_min, jitter_max;

static void noop_cb(unsigned long p) { }

static int __init b53_ref_init(void)
{
	void __iomem *cnt;
	int i, tmr;
	unsigned long end, flags;
	u64 a, r, pa, pr, samples = 0, arch_back = 0, ref_back = 0;
	u64 cal_a, cal_r, start_r = 0, last_r = 0;
	s64 ddev, off = 0;
	bool active = false;
	struct ev cur = {0};

	if (set_cpus_allowed_ptr(current, cpumask_of(cpu)))
		return -EINVAL;

	/* 1 hour period: count runs 0 .. 7.2e11 at 200 MHz without wrapping */
	tmr = ext_timer_alloc(EXT_TIMER_INVALID, 3600UL * 1000000UL, noop_cb, 0);
	if (tmr < 0) {
		pr_err("b53_ref: no free ext timer\n");
		return -EBUSY;
	}
	cnt = ioremap(TIMER_CNT_PHYS + 8 * tmr, 8);
	if (!cnt) {
		ext_timer_free(tmr);
		return -ENOMEM;
	}

	/* rate calibration over 200 ms; only used to scale short ref steps */
	{
		u64 sa = read_sysreg(cntvct_el0), sr = readq(cnt) & TIMER_CNT_MASK;

		msleep(200);
		cal_a = read_sysreg(cntvct_el0) - sa;
		cal_r = (readq(cnt) & TIMER_CNT_MASK) - sr;
	}
	pr_info("b53_ref: timer %d, cpu %d, %d s, arch/ref = %llu/%llu\n",
		tmr, cpu, secs, cal_a, cal_r);

	end = jiffies + secs * HZ;
	while (time_before(jiffies, end)) {
		u64 w0;

		local_irq_save(flags);
		isb();
		pa = read_sysreg(cntvct_el0);
		pr = readq(cnt) & TIMER_CNT_MASK;
		w0 = pa;
		do {
			isb();
			a = read_sysreg(cntvct_el0);
			r = readq(cnt) & TIMER_CNT_MASK;
			samples++;
			if (a < pa)
				arch_back++;
			if (r < pr)
				ref_back++;

			ddev = (s64)(a - pa) - (s64)(r - pr) * (s64)cal_a / (s64)cal_r;
			if (!active) {
				if (ddev < -THR) {
					active = true;
					off = ddev;
					start_r = r;
					cur = (struct ev){ pa, (s64)(a - pa),
						(s64)(r - pr) * (s64)cal_a / (s64)cal_r,
						ddev, 0, false };
				} else if (ddev > THR) {
					fwd_jumps++;
				} else {
					if (ddev < jitter_min)
						jitter_min = ddev;
					if (ddev > jitter_max)
						jitter_max = ddev;
				}
			} else {
				off += ddev;
				if (off < cur.x)
					cur.x = off;
				if (off > -THR / 2) {
					cur.dur = (r - start_r) * cal_a / cal_r;
					cur.closed = true;
					if (cur.dur < 64) {
						short_blips++;	/* one-sample read latency blip */
					} else {
						size_hist[fls64(-cur.x)]++;
						dur_hist[fls64(cur.dur)]++;
						if (nev < MAX_EV)
							evs[nev++] = cur;
					}
					active = false;
				}
			}
			last_r = r;
			pa = a;
			pr = r;
		} while (a - w0 < WINDOW_TICKS && (s64)(a - w0) > -(s64)WINDOW_TICKS);
		local_irq_restore(flags);

		/* an offset still open at the end of a window: the sleep hides
		 * its end, so close it with the last-seen time as a lower bound */
		if (active) {
			cur.dur = (last_r - start_r) * cal_a / cal_r;
			cur.closed = false;
			open_end++;
			size_hist[fls64(-cur.x)]++;
			dur_hist[fls64(cur.dur)]++;
			if (nev < MAX_EV)
				evs[nev++] = cur;
			active = false;
		}
		usleep_range(500, 1000);
	}

	pr_info("b53_ref: samples=%llu arch_backward=%llu ref_backward=%llu events=%d open_at_window_end=%lu short_blips=%lu fwd_jumps=%lu jitter=[%lld,%lld] ticks\n",
		samples, arch_back, ref_back, nev, open_end, short_blips,
		fwd_jumps, jitter_min, jitter_max);
	for (i = 1; i < 64; i++)
		if (size_hist[i])
			pr_info("b53_ref: offset %llu-%llu ticks: %lu\n",
				1ULL << (i - 1), (1ULL << i) - 1, size_hist[i]);
	for (i = 0; i < 64; i++)
		if (dur_hist[i])
			pr_info("b53_ref: duration %llu-%llu ticks: %lu\n",
				i ? 1ULL << (i - 1) : 0, i ? (1ULL << i) - 1 : 0, dur_hist[i]);
	for (i = 0; i < nev; i++)
		pr_info("b53_ref: ev arch=%llx arch_step=%lld ref_step=%lld offset=%lld dur=%llu%s\n",
			evs[i].arch, evs[i].arch_step, evs[i].ref_step, evs[i].x,
			evs[i].dur, evs[i].closed ? "" : " (open: lower bound)");

	iounmap(cnt);
	ext_timer_free(tmr);
	return -EAGAIN;
}

module_init(b53_ref_init);
