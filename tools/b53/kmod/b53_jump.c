// SPDX-License-Identifier: GPL-2.0
/*
 * b53_jump: capture the raw bit pattern of Brahma-B53 arch counter jumps.
 *
 * A kthread pinned to one CPU reads the raw counter (read_sysreg, bypassing
 * the kernel's B53 filter) in a tight loop, in IRQ-off windows of ~1 ms with
 * a ~1 ms sleep between them (about 50% coverage). Any step between two
 * consecutive reads of 2^15 ticks (410 us) or more, either way, is an event
 * (plus the first 4 steps of 2^11..2^14, the torn-read kind, as samples):
 * the 8 reads before and 16 after are kept, and the rest of the window is
 * watched for a step back (healed = a torn/transient offset; not healed =
 * the counter itself moved).
 *
 * Each window (and each gap between windows) is also measured against the
 * kernel's free-running reference timer (TimerCnt<ref>, 200 MHz): net =
 * arch elapsed - ref elapsed in arch ticks. A jump that happens during a
 * sleep shows up only there ("gap jump", no bit pattern).
 *
 * insmod b53_jump.ko secs=3600 cpu=2 ref=2   (results in dmesg, tag b53_jump)
 * rmmod b53_jump stops it early and prints the summary.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <asm/sysreg.h>
#include <asm/barrier.h>

MODULE_LICENSE("GPL");

static int secs = 3600;
module_param(secs, int, 0444);
static int cpu = 2;
module_param(cpu, int, 0444);
static int ref = 2;
module_param(ref, int, 0444);

#define TIMER_CNT_PHYS	0xff800420UL	/* TimerCnt0; TimerCntN at +8*N */
#define TIMER_CNT_MASK	0x3FFFFFFFFFFFFFFFULL
#define WINDOW_TICKS	80000ULL	/* 1 ms of arch time per IRQ-off window */
#define WINDOW_MAXREADS	400000		/* bound the window if time stops */
#define JUMP_TICKS	32768		/* step that counts as an event (2^15) */
#define NSAMPLE_SMALL	4		/* also keep the first few 2^11..2^14 steps */
#define SMALL_TICKS	2048
#define NET_TICKS	4096		/* window/gap net error that counts */
#define NBEFORE		8
#define NAFTER		16
#define MAX_EV		64

struct ev {
	u64 v[NBEFORE + NAFTER];	/* reads; the jump is from v[7] to v[8] */
	int nafter;			/* reads captured after the jump */
	s64 step;			/* v[8] - v[7] */
	s64 healed_after;		/* ticks until a step back by ~-step, or -1 */
	s64 win_net;			/* window net vs ref, arch ticks */
	unsigned long jiff;
};

static struct ev evs[MAX_EV];
static int nev, nprinted;
static unsigned long nwin, ngapjump, nwinjump, nlost;
static unsigned long small_hist[16];	/* |step| 64..32767 by log2 */
static int nsmall;
static s64 gap_jumps[MAX_EV];
static void __iomem *refcnt;
static struct task_struct *task;

static inline u64 raw(void)
{
	isb();
	return read_sysreg(cntvct_el0);
}

static inline u64 ref_arch(void)	/* reference time in arch ticks */
{
	return ((readq(refcnt) & TIMER_CNT_MASK) * 2) / 5;
}

static void print_ev(int i)
{
	struct ev *e = &evs[i];
	int k, n = NBEFORE + e->nafter;

	pr_info("b53_jump: event %d at +%lus: step %+lld (%s), %s, window net %+lld\n",
		i, (e->jiff - evs[0].jiff) / HZ, e->step,
		e->step > 0 ? "fwd" : "back",
		e->healed_after >= 0 ? "healed" : "NOT healed in window",
		e->win_net);
	if (e->healed_after >= 0)
		pr_info("b53_jump:   healed %lld ticks after the jump\n", e->healed_after);
	pr_info("b53_jump:   before %016llx  after %016llx  xor %016llx\n",
		e->v[NBEFORE - 1], e->v[NBEFORE], e->v[NBEFORE - 1] ^ e->v[NBEFORE]);
	for (k = 0; k < n; k++)
		pr_info("b53_jump:   [%+3d] %016llx  step %+lld\n", k - NBEFORE,
			e->v[k], k ? (s64)(e->v[k] - e->v[k - 1]) : 0LL);
}

static int jump_fn(void *unused)
{
	unsigned long end = jiffies + (unsigned long)secs * HZ;
	u64 last_a = 0, last_r = 0;

	while (!kthread_should_stop() && time_before(jiffies, end)) {
		u64 hist[NBEFORE], a0, r0, a1, r1, prev, cur;
		unsigned long flags;
		int h = 0, i, pend = -1, reads = 0;
		s64 net;

		local_irq_save(flags);
		r0 = ref_arch();
		a0 = raw();
		prev = a0;
		for (i = 0; i < NBEFORE; i++)
			hist[i] = a0;
		while (reads++ < WINDOW_MAXREADS) {
			s64 d;

			cur = raw();
			d = cur - prev;
			if (pend >= 0) {
				struct ev *e = &evs[pend];

				if (e->nafter < NAFTER)
					e->v[NBEFORE + e->nafter++] = cur;
				/* healed: a step back by the same amount (within 1/16) */
				if (e->healed_after < 0 &&
				    abs(d + e->step) <= max(64LL, abs(e->step) / 16))
					e->healed_after = max(0LL, (s64)(cur - e->v[NBEFORE - 1]));
			}
			if ((abs(d) >= JUMP_TICKS || (abs(d) >= SMALL_TICKS && nsmall < NSAMPLE_SMALL)) &&
			    (pend < 0 || evs[pend].nafter >= NAFTER)) {
				if (abs(d) < JUMP_TICKS)
					nsmall++;
				if (nev < MAX_EV) {
					struct ev *e = &evs[nev];
					int k;

					for (k = 0; k < NBEFORE - 1; k++)
						e->v[k] = hist[(h + 1 + k) % NBEFORE];
					e->v[NBEFORE - 1] = prev;
					e->v[NBEFORE] = cur;
					e->nafter = 1;
					e->step = d;
					e->healed_after = -1;
					e->jiff = jiffies;
					pend = nev++;
				} else {
					nlost++;
				}
			}
			if (abs(d) >= 64 && abs(d) < JUMP_TICKS)
				small_hist[min(15, fls64(abs(d)) - 1)]++;
			hist[h] = cur;
			h = (h + 1) % NBEFORE;
			prev = cur;
			if (cur - a0 >= WINDOW_TICKS && (pend < 0 || evs[pend].nafter >= NAFTER))
				break;
		}
		a1 = raw();
		r1 = ref_arch();
		local_irq_restore(flags);

		nwin++;
		net = (s64)(a1 - a0) - (s64)(r1 - r0);
		if (abs(net) >= NET_TICKS)
			nwinjump++;
		if (pend >= 0)
			evs[pend].win_net = net;
		if (last_a) {
			s64 gap = (s64)(a0 - last_a) - (s64)(r0 - last_r);

			if (abs(gap) >= NET_TICKS) {
				if (ngapjump < MAX_EV)
					gap_jumps[ngapjump] = gap;
				ngapjump++;
				pr_info("b53_jump: gap jump %+lld ticks (during sleep, no pattern)\n", gap);
			}
		}
		last_a = a1;
		last_r = r1;

		while (nprinted < nev)
			print_ev(nprinted++);
		usleep_range(1000, 1200);
	}
	return 0;
}

static int __init b53_jump_init(void)
{
	refcnt = ioremap(TIMER_CNT_PHYS + 8 * ref, 8);
	if (!refcnt)
		return -ENOMEM;
	task = kthread_create(jump_fn, NULL, "b53_jump");
	if (IS_ERR(task)) {
		iounmap(refcnt);
		return PTR_ERR(task);
	}
	kthread_bind(task, cpu);
	get_task_struct(task);
	wake_up_process(task);
	pr_info("b53_jump: sampling raw cntvct on cpu %d for %d s (ref timer %d)\n",
		cpu, secs, ref);
	return 0;
}

static void __exit b53_jump_exit(void)
{
	int i;

	kthread_stop(task);
	put_task_struct(task);
	iounmap(refcnt);
	pr_info("b53_jump: done: %lu windows, %d events (%lu lost), %lu windows and %lu gaps with net >= %d ticks\n",
		nwin, nev, nlost, nwinjump, ngapjump, NET_TICKS);
	for (i = 0; i < 16; i++)
		if (small_hist[i])
			pr_info("b53_jump: small steps %5lu..%5lu: %lu\n",
				1UL << i, (2UL << i) - 1, small_hist[i]);
}

module_init(b53_jump_init);
module_exit(b53_jump_exit);
