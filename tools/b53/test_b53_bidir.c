/*
 * B53 Timer Bidirectional Glitch Detector
 * Detects both backward (cur < prev) and forward (abnormally large jump) glitches.
 *
 * Cross-compile:
 *   aarch64-linux-gnu-gcc -O2 -o test_b53_bidir test_b53_bidir.c -lpthread
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#define NUM_CORES       4
#define DURATION_SECS   10
#define TIMER_FREQ      80000000ULL

/*
 * Forward glitch threshold: if two consecutive reads differ by more than
 * this many ticks, flag it as a potential forward glitch.
 * At 80MHz, 50000 ticks = 625us. A trapped mrs roundtrip is ~1-2us,
 * so normal consecutive reads differ by ~100-200 ticks.
 * 50000 is generous enough to avoid false positives from minor scheduling
 * jitter, but catches the ~32K-tick glitches we've observed.
 */
#define FORWARD_THRESHOLD  50000ULL

static volatile int running = 1;

static inline uint64_t read_cntvct(void)
{
	uint64_t val;
	asm volatile("mrs %0, cntvct_el0" : "=r"(val));
	return val;
}

struct thread_result {
	int core;
	uint64_t total_reads;
	uint64_t backward_glitches;
	uint64_t forward_glitches;
	/* Log first few of each type */
#define MAX_LOG 8
	uint64_t bk_prev[MAX_LOG], bk_cur[MAX_LOG];
	uint64_t fw_prev[MAX_LOG], fw_cur[MAX_LOG];
};

static void *worker(void *arg)
{
	struct thread_result *res = (struct thread_result *)arg;
	cpu_set_t cpuset;
	uint64_t prev, cur, diff;

	CPU_ZERO(&cpuset);
	CPU_SET(res->core, &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

	prev = read_cntvct();
	res->total_reads = 1;

	while (running) {
		cur = read_cntvct();
		res->total_reads++;

		if (__builtin_expect(cur < prev, 0)) {
			/* Backward glitch */
			if (res->backward_glitches < MAX_LOG) {
				res->bk_prev[res->backward_glitches] = prev;
				res->bk_cur[res->backward_glitches] = cur;
			}
			res->backward_glitches++;
		} else {
			diff = cur - prev;
			if (__builtin_expect(diff > FORWARD_THRESHOLD, 0)) {
				/* Forward glitch (or preemption) */
				if (res->forward_glitches < MAX_LOG) {
					res->fw_prev[res->forward_glitches] = prev;
					res->fw_cur[res->forward_glitches] = cur;
				}
				res->forward_glitches++;
			}
		}

		prev = cur;
	}

	return NULL;
}

int main(void)
{
	pthread_t threads[NUM_CORES];
	struct thread_result results[NUM_CORES];
	uint64_t total_reads = 0, total_bk = 0, total_fw = 0;

	printf("=== B53 Bidirectional Glitch Detector ===\n");
	printf("Timer frequency: %llu Hz\n", TIMER_FREQ);
	printf("Duration: %d seconds\n", DURATION_SECS);
	printf("Threads: %d (one per core)\n", NUM_CORES);
	printf("Forward threshold: %llu ticks (%.1f us)\n",
	       FORWARD_THRESHOLD, (double)FORWARD_THRESHOLD / (TIMER_FREQ / 1000000ULL));
	printf("Running...\n\n");

	memset(results, 0, sizeof(results));
	for (int i = 0; i < NUM_CORES; i++) {
		results[i].core = i;
		pthread_create(&threads[i], NULL, worker, &results[i]);
	}

	sleep(DURATION_SECS);
	running = 0;

	for (int i = 0; i < NUM_CORES; i++)
		pthread_join(threads[i], NULL);

	/* Print per-core results */
	for (int i = 0; i < NUM_CORES; i++) {
		struct thread_result *r = &results[i];
		total_reads += r->total_reads;
		total_bk += r->backward_glitches;
		total_fw += r->forward_glitches;

		printf("--- Core %d ---\n", r->core);
		printf("  Reads: %llu\n", (unsigned long long)r->total_reads);
		printf("  Backward glitches: %llu\n", (unsigned long long)r->backward_glitches);
		printf("  Forward  glitches: %llu\n", (unsigned long long)r->forward_glitches);

		for (uint64_t j = 0; j < r->backward_glitches && j < MAX_LOG; j++) {
			int64_t delta = (int64_t)(r->bk_cur[j] - r->bk_prev[j]);
			printf("    BK[%llu]: 0x%llx -> 0x%llx (delta: %lld)\n",
			       (unsigned long long)j,
			       (unsigned long long)r->bk_prev[j],
			       (unsigned long long)r->bk_cur[j], (long long)delta);
		}
		for (uint64_t j = 0; j < r->forward_glitches && j < MAX_LOG; j++) {
			uint64_t delta = r->fw_cur[j] - r->fw_prev[j];
			printf("    FW[%llu]: 0x%llx -> 0x%llx (delta: +%llu = %.1f us)\n",
			       (unsigned long long)j,
			       (unsigned long long)r->fw_prev[j],
			       (unsigned long long)r->fw_cur[j],
			       (unsigned long long)delta,
			       (double)delta / (TIMER_FREQ / 1000000ULL));
		}
		printf("\n");
	}

	printf("=== Summary ===\n");
	printf("Total reads:      %llu\n", (unsigned long long)total_reads);
	printf("Backward glitches: %llu\n", (unsigned long long)total_bk);
	printf("Forward  glitches: %llu\n", (unsigned long long)total_fw);

	if (total_bk == 0 && total_fw == 0)
		printf("PASS: No glitches detected!\n");
	else {
		if (total_bk > 0)
			printf("FAIL: Backward glitches detected!\n");
		if (total_fw > 0)
			printf("NOTE: Forward anomalies detected (could be preemption or glitch)\n");
	}

	return (total_bk > 0) ? 1 : 0;
}
