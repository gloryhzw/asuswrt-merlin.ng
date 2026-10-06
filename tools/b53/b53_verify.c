/*
 * b53_verify: check that time keeps flowing and timers fire on time, judged
 * by the Broadcom peripheral reference timer (read via /dev/mem, so it does
 * not depend on the arch counter).
 *
 * Loops clock_nanosleep(1ms) and prints one line per second:
 *   drift   = CLOCK_MONOTONIC elapsed - reference elapsed (should stay ~0)
 *   sleep   = longest 1ms sleep that second, by the reference (late timers)
 *   mono_min= smallest CLOCK_MONOTONIC step across a sleep (frozen clock -> 0)
 * Usage: b53_verify <ref_timer_index> <seconds>
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define TIMER_PAGE	0xff800000UL
#define CNT_OFF		0x420
#define CNT_MASK	0x3FFFFFFFFFFFFFFFULL
#define REF_HZ		200000000.0

static volatile uint64_t *cnt;

static uint64_t ref(void) { return *cnt & CNT_MASK; }

static int64_t mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int main(int argc, char **argv)
{
	int idx = argc > 1 ? atoi(argv[1]) : 2;
	int secs = argc > 2 ? atoi(argv[2]) : 30;
	struct timespec ms = { 0, 1000000 };
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	void *base;
	uint64_t r0, rs;
	int64_t m0, ms0;

	if (fd < 0) { perror("/dev/mem"); return 1; }
	base = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, TIMER_PAGE);
	if (base == MAP_FAILED) { perror("mmap"); return 1; }
	cnt = (volatile uint64_t *)((char *)base + CNT_OFF + 8 * idx);

	r0 = ref();
	m0 = mono_ns();
	for (int s = 0; s < secs; s++) {
		double max_sleep_us = 0;
		int64_t min_step = INT64_MAX;

		rs = ref();
		ms0 = mono_ns();
		while ((ref() - rs) / REF_HZ < 1.0) {
			uint64_t a = ref();
			int64_t ma = mono_ns(), mb;
			double us;

			clock_nanosleep(CLOCK_MONOTONIC, 0, &ms, NULL);
			us = (ref() - a) / (REF_HZ / 1e6);
			mb = mono_ns();
			if (us > max_sleep_us)
				max_sleep_us = us;
			if (mb - ma < min_step)
				min_step = mb - ma;
		}
		printf("t=%3d drift=%+9.3f ms  sec_mono=%8.3f ms  max_sleep=%9.1f us  mono_min=%7.1f us\n",
		       s + 1,
		       (mono_ns() - m0) / 1e6 - (ref() - r0) / (REF_HZ / 1e3),
		       (mono_ns() - ms0) / 1e6,
		       max_sleep_us, min_step / 1e3);
		fflush(stdout);
	}
	return 0;
}
