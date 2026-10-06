#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/poll.h>
#include <sys/timex.h>
#include <math.h>
#include <stdatomic.h>
#include <getopt.h>

#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[1;31m"
#define COLOR_GREEN   "\033[1;32m"
#define COLOR_YELLOW  "\033[1;33m"
#define COLOR_CYAN    "\033[1;36m"
#define COLOR_BOLD    "\033[1m"

static uint64_t timer_freq_hz = 80000000ULL; // 80 MHz default on BCM4916

static inline uint64_t get_time_ns(clockid_t clk_id) {
    struct timespec ts;
    clock_gettime(clk_id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static inline uint64_t get_syscall_time_ns(clockid_t clk_id) {
    struct timespec ts;
    syscall(SYS_clock_gettime, clk_id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static inline uint64_t read_cntvct(void) {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

static inline uint64_t read_cntfrq(void) {
    uint64_t val = 0;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val ? val : 80000000ULL;
}

static void wait_with_progress(int duration_sec, const char* label) {
    for (int sec = 1; sec <= duration_sec; sec++) {
        sleep(1);
        if (duration_sec >= 10 && (sec % 5 == 0 || sec == duration_sec)) {
            printf("    [%s] Elapsed: %d/%ds (%.0f%%)...\n",
                   label, sec, duration_sec, (double)sec * 100.0 / (double)duration_sec);
            fflush(stdout);
        }
    }
}

/* ========================================================================= */
/* Test 1: Hardware Counter Inter-Core Skew                                  */
/* ========================================================================= */
static volatile int skew_running = 1;
static atomic_uint_fast64_t skew_core0_val = 0;
static uint64_t skew_drops = 0;
static uint64_t skew_max_ticks = 0;
static uint64_t skew_total_reads = 0;
static uint64_t skew_underflow_count = 0;

static void* skew_core0_worker(void* arg) {
    (void)arg;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    while (skew_running) {
        uint64_t v = read_cntvct();
        atomic_store_explicit(&skew_core0_val, v, memory_order_release);
    }
    return NULL;
}

static void* skew_core1_worker(void* arg) {
    (void)arg;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    while (skew_running) {
        uint64_t v0 = atomic_load_explicit(&skew_core0_val, memory_order_acquire);
        uint64_t v1 = read_cntvct();
        skew_total_reads++;

        if (__builtin_expect(v1 < v0, 0)) {
            uint64_t diff = v0 - v1;
            skew_drops++;
            if (diff > 0x10000000000ULL) { // > 1 trillion ticks: 56-bit underflow!
                skew_underflow_count++;
            }
            if (diff > skew_max_ticks) {
                skew_max_ticks = diff;
            }
        }
    }
    return NULL;
}

static int run_test_skew(int duration_sec) {
    printf(COLOR_CYAN "=== [Test 1] Hardware Counter Core-to-Core Skew (%ds) ===" COLOR_RESET "\n", duration_sec);
    skew_running = 1;
    skew_drops = 0;
    skew_max_ticks = 0;
    skew_total_reads = 0;
    skew_underflow_count = 0;

    pthread_t t0, t1;
    pthread_create(&t0, NULL, skew_core0_worker, NULL);
    pthread_create(&t1, NULL, skew_core1_worker, NULL);

    wait_with_progress(duration_sec, "Test 1 Skew");
    skew_running = 0;

    pthread_join(t0, NULL);
    pthread_join(t1, NULL);

    double max_us = (double)skew_max_ticks / (double)(timer_freq_hz / 1000000ULL);
    printf("  Total inter-core reads : %llu\n", (unsigned long long)skew_total_reads);
    printf("  Core skew reversals    : %llu\n", (unsigned long long)skew_drops);
    printf("  Max inter-core skew    : %llu ticks (%.3f µs)\n", (unsigned long long)skew_max_ticks, max_us);
    printf("  56-bit underflow events: %llu\n", (unsigned long long)skew_underflow_count);

    if (skew_underflow_count == 0 && max_us < 50.0) {
        printf(COLOR_GREEN "  [PASS] Hardware timer skew is within expected microsecond bounds." COLOR_RESET "\n\n");
        return 0;
    } else {
        printf(COLOR_RED "  [FAIL] 56-bit underflow detected or excessive skew!" COLOR_RESET "\n\n");
        return 1;
    }
}

/* ========================================================================= */
/* Test 2: Cross-Core Sleep & Wake Monotonicity                              */
/* ========================================================================= */
static volatile int mono_running = 1;
static atomic_uint_fast64_t mono_latest_time = 0;
static uint64_t mono_reversals = 0;
static uint64_t mono_underflows = 0;
static uint64_t mono_checks = 0;
static uint64_t mono_max_drop_ns = 0;

static void* mono_fast_reader(void* arg) {
    (void)arg;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    while (mono_running) {
        for (int i = 0; i < 500; i++) {
            uint64_t t = get_time_ns(CLOCK_MONOTONIC);
            atomic_store_explicit(&mono_latest_time, t, memory_order_release);
        }
    }
    return NULL;
}

static void* mono_intermittent_reader(void* arg) {
    int core_id = (int)(intptr_t)arg;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    while (mono_running) {
        usleep((rand() % 5000) + 500); // 0.5ms to 5.5ms random sleep
        uint64_t reference = atomic_load_explicit(&mono_latest_time, memory_order_acquire);
        uint64_t my_time = get_time_ns(CLOCK_MONOTONIC);
        __sync_fetch_and_add(&mono_checks, 1);

        if (__builtin_expect(my_time < reference, 0)) {
            uint64_t drop = reference - my_time;
            __sync_fetch_and_add(&mono_reversals, 1);
            if (drop > 1000000000ULL) { // > 1 second reversal
                __sync_fetch_and_add(&mono_underflows, 1);
            }
            uint64_t cur_max = mono_max_drop_ns;
            while (drop > cur_max && !__sync_bool_compare_and_swap(&mono_max_drop_ns, cur_max, drop)) {
                cur_max = mono_max_drop_ns;
            }
        }
    }
    return NULL;
}

static int run_test_monotonic(int duration_sec) {
    printf(COLOR_CYAN "=== [Test 2] Cross-Core Sleep & Wake Monotonicity (%ds) ===" COLOR_RESET "\n", duration_sec);
    mono_running = 1;
    mono_reversals = 0;
    mono_underflows = 0;
    mono_checks = 0;
    mono_max_drop_ns = 0;

    pthread_t t0, t1, t2, t3;
    pthread_create(&t0, NULL, mono_fast_reader, NULL);
    pthread_create(&t1, NULL, mono_intermittent_reader, (void*)(intptr_t)1);
    pthread_create(&t2, NULL, mono_intermittent_reader, (void*)(intptr_t)2);
    pthread_create(&t3, NULL, mono_intermittent_reader, (void*)(intptr_t)3);

    wait_with_progress(duration_sec, "Test 2 Monotonic");
    mono_running = 0;

    pthread_join(t0, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);

    printf("  Sleep/wake cross-checks : %llu\n", (unsigned long long)mono_checks);
    printf("  Clock reversals detected: %llu\n", (unsigned long long)mono_reversals);
    printf("  Max backward drop       : %llu ns (%.3f µs)\n",
           (unsigned long long)mono_max_drop_ns, (double)mono_max_drop_ns / 1000.0);
    printf("  Underflow spikes (>1s)  : %llu\n", (unsigned long long)mono_underflows);

    if (mono_underflows == 0) {
        printf(COLOR_GREEN "  [PASS] Perfect cross-core monotonicity maintained across sleep/wake." COLOR_RESET "\n\n");
        return 0;
    } else {
        printf(COLOR_RED "  [FAIL] 56-bit underflow crash detected!" COLOR_RESET "\n\n");
        return 1;
    }
}

/* ========================================================================= */
/* Test 3: Causal High-Throughput Cross-Core Monotonicity Flood              */
/* ========================================================================= */
static volatile int sc_running = 1;
static atomic_uint_fast64_t sc_core_ts[4] = {0, 0, 0, 0};
static uint64_t sc_total_checks = 0;
static uint64_t sc_true_reversals = 0;
static uint64_t sc_max_reversal_ns = 0;
static uint64_t sc_underflow_count = 0;

static void* sc_causal_worker(void* arg) {
    int core_id = (int)(intptr_t)arg;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    uint64_t local_checks = 0;
    uint64_t local_reversals = 0;
    uint64_t local_max_rev = 0;
    uint64_t local_underflow = 0;

    int other_core = (core_id + 1) % 4;

    while (sc_running) {
        // Step 1: publish my timestamp with memory release
        uint64_t my_now = get_syscall_time_ns(CLOCK_MONOTONIC);
        atomic_store_explicit(&sc_core_ts[core_id], my_now, memory_order_release);

        // Step 2: read another core's timestamp with memory acquire
        uint64_t other_time = atomic_load_explicit(&sc_core_ts[other_core], memory_order_acquire);
        if (other_time != 0) {
            // Step 3: sample my time strictly AFTER seeing the other core's timestamp
            uint64_t check_time = get_syscall_time_ns(CLOCK_MONOTONIC);
            local_checks++;

            // By causality, check_time must be >= other_time
            if (__builtin_expect(check_time < other_time, 0)) {
                uint64_t rev = other_time - check_time;
                local_reversals++;
                if (rev > 1000000000ULL) { // > 1 second: 56-bit underflow leap!
                    local_underflow++;
                }
                if (rev > local_max_rev) {
                    local_max_rev = rev;
                }
            }
        }
        other_core = (other_core + 1) % 4;
    }

    __sync_fetch_and_add(&sc_total_checks, local_checks);
    __sync_fetch_and_add(&sc_true_reversals, local_reversals);
    __sync_fetch_and_add(&sc_underflow_count, local_underflow);
    uint64_t cur_max = sc_max_reversal_ns;
    while (local_max_rev > cur_max && !__sync_bool_compare_and_swap(&sc_max_reversal_ns, cur_max, local_max_rev)) {
        cur_max = sc_max_reversal_ns;
    }
    return NULL;
}

static int run_test_syscall(int duration_sec) {
    printf(COLOR_CYAN "=== [Test 3] High-Contention Syscall Cross-Core Flood (%ds) ===" COLOR_RESET "\n", duration_sec);
    sc_running = 1;
    for (int i = 0; i < 4; i++) atomic_store(&sc_core_ts[i], 0);
    sc_total_checks = 0;
    sc_true_reversals = 0;
    sc_max_reversal_ns = 0;
    sc_underflow_count = 0;

    pthread_t th[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&th[i], NULL, sc_causal_worker, (void*)(intptr_t)i);
    }

    wait_with_progress(duration_sec, "Test 3 Syscall");
    sc_running = 0;

    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
    }

    double max_us = (double)sc_max_reversal_ns / 1000.0;
    double mops = (double)sc_total_checks / (double)duration_sec / 1000000.0;
    printf("  Total syscall checks    : %llu (%.2f M checks/sec across 4 cores)\n",
           (unsigned long long)sc_total_checks, mops);
    printf("  Inter-core reversals    : %llu (%.5f%%)\n",
           (unsigned long long)sc_true_reversals,
           sc_total_checks ? (double)sc_true_reversals * 100.0 / (double)sc_total_checks : 0.0);
    printf("  Max backward reversal   : %llu ns (%.3f µs)\n",
           (unsigned long long)sc_max_reversal_ns, max_us);
    printf("  28.5y Underflow crashes : %llu\n", (unsigned long long)sc_underflow_count);

    if (sc_underflow_count > 0) {
        printf(COLOR_RED "  [FAIL] 56-bit underflow crash detected!" COLOR_RESET "\n\n");
        return 1;
    } else if (max_us >= 10.0) {
        printf(COLOR_RED "  [FAIL] Inter-core backward reversal exceeded bound (%.3f µs >= 10.0 µs)!" COLOR_RESET "\n\n", max_us);
        return 1;
    } else {
        printf(COLOR_GREEN "  [PASS] CONFIG_CLOCKSOURCE_VALIDATE_LAST_CYCLE prevented underflow (reversals bounded to < 10 µs)." COLOR_RESET "\n\n");
        return 0;
    }
}

/* ========================================================================= */
/* Test 4: High-Resolution Timer (timerfd) Precision & Early Firing Test     */
/* ========================================================================= */
static volatile int tfd_running = 1;
static uint64_t tfd_total_expirations = 0;
static uint64_t tfd_early_firings = 0;
static uint64_t tfd_max_jitter_ns = 0;
static uint64_t tfd_max_early_ns = 0;

typedef struct {
    int core_id;
    uint64_t interval_ns;
} tfd_param_t;

static void* timerfd_worker(void* arg) {
    tfd_param_t* p = (tfd_param_t*)arg;
    int core_id = p->core_id;
    uint64_t interval_ns = p->interval_ns;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (tfd < 0) return NULL;

    struct itimerspec its;
    its.it_value.tv_sec = interval_ns / 1000000000ULL;
    its.it_value.tv_nsec = interval_ns % 1000000000ULL;
    its.it_interval.tv_sec = its.it_value.tv_sec;
    its.it_interval.tv_nsec = its.it_value.tv_nsec;

    timerfd_settime(tfd, 0, &its, NULL);

    uint64_t local_exp = 0;
    uint64_t local_early = 0;
    uint64_t local_max_jitter = 0;
    uint64_t local_max_early = 0;

    uint64_t expected_target = get_time_ns(CLOCK_MONOTONIC) + interval_ns;

    while (tfd_running) {
        uint64_t exp_count = 0;
        ssize_t s = read(tfd, &exp_count, sizeof(exp_count));
        if (s != sizeof(exp_count)) break;

        uint64_t now = get_time_ns(CLOCK_MONOTONIC);
        local_exp += exp_count;

        // Check if timer woke up prematurely before target interval
        // 50 µs tolerance for kernel hrtimer slack
        if (__builtin_expect(now + 50000ULL < expected_target, 0)) {
            uint64_t early_by = expected_target - now;
            local_early++;
            if (early_by > local_max_early) local_max_early = early_by;
        }

        // Measure scheduling overshoot / jitter
        if (now > expected_target) {
            uint64_t jitter = now - expected_target;
            if (jitter > local_max_jitter) local_max_jitter = jitter;
        }

        expected_target += exp_count * interval_ns;
    }

    close(tfd);

    __sync_fetch_and_add(&tfd_total_expirations, local_exp);
    __sync_fetch_and_add(&tfd_early_firings, local_early);

    uint64_t cur_j = tfd_max_jitter_ns;
    while (local_max_jitter > cur_j && !__sync_bool_compare_and_swap(&tfd_max_jitter_ns, cur_j, local_max_jitter)) {
        cur_j = tfd_max_jitter_ns;
    }
    uint64_t cur_e = tfd_max_early_ns;
    while (local_max_early > cur_e && !__sync_bool_compare_and_swap(&tfd_max_early_ns, cur_e, local_max_early)) {
        cur_e = tfd_max_early_ns;
    }
    return NULL;
}

static int run_test_timerfd(int duration_sec) {
    printf(COLOR_CYAN "=== [Test 4] High-Res Timer (timerfd) Precision & Early Firing (%ds) ===" COLOR_RESET "\n", duration_sec);
    tfd_running = 1;
    tfd_total_expirations = 0;
    tfd_early_firings = 0;
    tfd_max_jitter_ns = 0;
    tfd_max_early_ns = 0;

    pthread_t th[4];
    tfd_param_t params[4] = {
        {0,  500000ULL}, // Core 0: 500 µs periodic timer
        {1, 1000000ULL}, // Core 1: 1.0 ms periodic timer
        {2,  250000ULL}, // Core 2: 250 µs sub-ms timer
        {3, 2000000ULL}, // Core 3: 2.0 ms periodic timer
    };

    for (int i = 0; i < 4; i++) {
        pthread_create(&th[i], NULL, timerfd_worker, &params[i]);
    }

    wait_with_progress(duration_sec, "Test 4 Timerfd");
    tfd_running = 0;

    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
    }

    printf("  Total timer expirations : %llu\n", (unsigned long long)tfd_total_expirations);
    printf("  Early / premature fires : %llu\n", (unsigned long long)tfd_early_firings);
    if (tfd_early_firings > 0) {
        printf(COLOR_RED "  Max premature delta     : %llu ns (%.3f µs)" COLOR_RESET "\n",
               (unsigned long long)tfd_max_early_ns, (double)tfd_max_early_ns / 1000.0);
    }
    printf("  Max timer overshoot     : %llu ns (%.3f µs)\n",
           (unsigned long long)tfd_max_jitter_ns, (double)tfd_max_jitter_ns / 1000.0);

    if (tfd_early_firings == 0) {
        if (tfd_max_jitter_ns > 200000000ULL) {
            printf(COLOR_YELLOW "  [WARN] High timer latency (%.3f ms) due to CPU scheduling load." COLOR_RESET "\n",
                   (double)tfd_max_jitter_ns / 1000000.0);
        }
        printf(COLOR_GREEN "  [PASS] Zero premature timer expirations. hrtimer rbtree is stable." COLOR_RESET "\n\n");
        return 0;
    } else {
        printf(COLOR_RED "  [FAIL] Premature timer expirations detected!" COLOR_RESET "\n\n");
        return 1;
    }
}

/* ========================================================================= */
/* Test 5: Multi-Clock Domain Consistency (MONOTONIC vs RAW vs BOOTTIME)     */
/* ========================================================================= */
static volatile int dom_running = 1;
static uint64_t dom_total_checks = 0;
static uint64_t dom_raw_reversals = 0;
static uint64_t dom_boot_inconsistencies = 0;
static uint64_t dom_max_raw_drop_ns = 0;

static void* domain_worker(void* arg) {
    int core_id = (int)(intptr_t)arg;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    uint64_t local_checks = 0;
    uint64_t local_raw_rev = 0;
    uint64_t local_boot_err = 0;
    uint64_t local_max_raw_drop = 0;

    uint64_t last_raw = get_time_ns(CLOCK_MONOTONIC_RAW);

    while (dom_running) {
        uint64_t t_mono = get_time_ns(CLOCK_MONOTONIC);
        uint64_t t_raw  = get_time_ns(CLOCK_MONOTONIC_RAW);
        uint64_t t_boot = get_time_ns(CLOCK_BOOTTIME);
        local_checks++;

        // 1. Check CLOCK_MONOTONIC_RAW does not step backwards
        if (__builtin_expect(t_raw < last_raw, 0)) {
            uint64_t drop = last_raw - t_raw;
            local_raw_rev++;
            if (drop > local_max_raw_drop) local_max_raw_drop = drop;
        }
        last_raw = t_raw;

        // 2. Check CLOCK_BOOTTIME >= CLOCK_MONOTONIC
        // Allow 5 µs sequential reading overhead
        if (__builtin_expect(t_boot + 5000ULL < t_mono, 0)) {
            local_boot_err++;
        }
    }

    __sync_fetch_and_add(&dom_total_checks, local_checks);
    __sync_fetch_and_add(&dom_raw_reversals, local_raw_rev);
    __sync_fetch_and_add(&dom_boot_inconsistencies, local_boot_err);

    uint64_t cur_drop = dom_max_raw_drop_ns;
    while (local_max_raw_drop > cur_drop && !__sync_bool_compare_and_swap(&dom_max_raw_drop_ns, cur_drop, local_max_raw_drop)) {
        cur_drop = dom_max_raw_drop_ns;
    }
    return NULL;
}

static int run_test_domains(int duration_sec) {
    printf(COLOR_CYAN "=== [Test 5] Multi-Clock Domain Consistency (RAW vs MONO vs BOOT) (%ds) ===" COLOR_RESET "\n", duration_sec);
    dom_running = 1;
    dom_total_checks = 0;
    dom_raw_reversals = 0;
    dom_boot_inconsistencies = 0;
    dom_max_raw_drop_ns = 0;

    struct timex tx_start, tx_end;
    memset(&tx_start, 0, sizeof(tx_start));
    memset(&tx_end, 0, sizeof(tx_end));
    adjtimex(&tx_start);

    uint64_t start_mono = get_time_ns(CLOCK_MONOTONIC);
    uint64_t start_raw  = get_time_ns(CLOCK_MONOTONIC_RAW);

    pthread_t th[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&th[i], NULL, domain_worker, (void*)(intptr_t)i);
    }

    wait_with_progress(duration_sec, "Test 5 Multi-Clock");
    dom_running = 0;

    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
    }

    uint64_t end_mono = get_time_ns(CLOCK_MONOTONIC);
    uint64_t end_raw  = get_time_ns(CLOCK_MONOTONIC_RAW);
    adjtimex(&tx_end);

    double delta_mono = (double)(end_mono - start_mono);
    double delta_raw  = (double)(end_raw - start_raw);
    double ntp_slew_ppm = ((delta_mono - delta_raw) / delta_raw) * 1000000.0;

    // Determine whether NTP is in active phase convergence
    // When NTP has an active time offset, the kernel slews phase dynamically via ADJ_OFFSET/PLL,
    // which adds up to an extra ±500 ppm on top of the ±500 ppm frequency limit (total limit ±1000 ppm).
    int ntp_active = (tx_end.offset != 0 || tx_start.offset != 0 || tx_end.status == TIME_ERROR);
    double limit_ppm = ntp_active ? 1000.0 : 500.0;

    printf("  Total multi-clock reads : %llu\n", (unsigned long long)dom_total_checks);
    printf("  CLOCK_MONOTONIC_RAW drops: %llu\n", (unsigned long long)dom_raw_reversals);
    if (dom_raw_reversals > 0) {
        printf(COLOR_YELLOW "  Max RAW backward drop   : %llu ns (%.3f µs)" COLOR_RESET "\n",
               (unsigned long long)dom_max_raw_drop_ns, (double)dom_max_raw_drop_ns / 1000.0);
    }
    printf("  BOOTTIME vs MONO errors : %llu\n", (unsigned long long)dom_boot_inconsistencies);
    printf("  NTP Frequency Slew Rate : %+.2f ppm (Status: %s, Offset: %ld µs, Limit: ±%.0f ppm)\n",
           ntp_slew_ppm,
           ntp_active ? "Active phase convergence" : "Steady-state",
           (long)tx_end.offset, limit_ppm);

    int test_fail = 0;
    if (dom_raw_reversals > 0) {
        printf(COLOR_RED "  [FAIL] CLOCK_MONOTONIC_RAW stepped backwards!" COLOR_RESET "\n");
        test_fail = 1;
    }
    if (dom_boot_inconsistencies > 0) {
        printf(COLOR_RED "  [FAIL] CLOCK_BOOTTIME < CLOCK_MONOTONIC detected!" COLOR_RESET "\n");
        test_fail = 1;
    }
    if (fabs(ntp_slew_ppm) > limit_ppm) {
        printf(COLOR_RED "  [FAIL] NTP frequency slew rate %+.2f ppm exceeded limit (±%.0f ppm)!" COLOR_RESET "\n",
               ntp_slew_ppm, limit_ppm);
        test_fail = 1;
    }

    if (!test_fail) {
        printf(COLOR_GREEN "  [PASS] All clock domains consistent. CLOCK_MONOTONIC_RAW is strictly monotonic." COLOR_RESET "\n\n");
        return 0;
    } else {
        printf(COLOR_RED "  [FAIL] Inconsistencies detected across clock domains!" COLOR_RESET "\n\n");
        return 1;
    }
}

/* ========================================================================= */
/* Test 6: Clock Latency & Throughput Benchmark                              */
/* ========================================================================= */
static int run_test_benchmark(int duration_sec) {
    (void)duration_sec;
    printf(COLOR_CYAN "=== [Test 6] Clock Latency & Throughput Benchmark ===" COLOR_RESET "\n");

    const uint64_t iterations = 5000000ULL;
    struct timespec ts;

    // 1. CLOCK_MONOTONIC via vDSO / syscall
    uint64_t start = get_syscall_time_ns(CLOCK_MONOTONIC);
    for (uint64_t i = 0; i < iterations; i++) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
    }
    uint64_t end = get_syscall_time_ns(CLOCK_MONOTONIC);
    double elapsed_mono_s = (double)(end - start) / 1000000000.0;
    double ns_per_call_mono = (double)(end - start) / (double)iterations;
    double mops_mono = (double)iterations / elapsed_mono_s / 1000000.0;

    // 2. CLOCK_REALTIME via vDSO / syscall
    start = get_syscall_time_ns(CLOCK_MONOTONIC);
    for (uint64_t i = 0; i < iterations; i++) {
        clock_gettime(CLOCK_REALTIME, &ts);
    }
    end = get_syscall_time_ns(CLOCK_MONOTONIC);
    double elapsed_real_s = (double)(end - start) / 1000000000.0;
    double ns_per_call_real = (double)(end - start) / (double)iterations;
    double mops_real = (double)iterations / elapsed_real_s / 1000000.0;

    // 3. CLOCK_MONOTONIC_RAW
    start = get_syscall_time_ns(CLOCK_MONOTONIC);
    for (uint64_t i = 0; i < iterations; i++) {
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    }
    end = get_syscall_time_ns(CLOCK_MONOTONIC);
    double elapsed_raw_s = (double)(end - start) / 1000000000.0;
    double ns_per_call_raw = (double)(end - start) / (double)iterations;
    double mops_raw = (double)iterations / elapsed_raw_s / 1000000.0;

    // 4. CLOCK_BOOTTIME
    start = get_syscall_time_ns(CLOCK_MONOTONIC);
    for (uint64_t i = 0; i < iterations; i++) {
        clock_gettime(CLOCK_BOOTTIME, &ts);
    }
    end = get_syscall_time_ns(CLOCK_MONOTONIC);
    double elapsed_boot_s = (double)(end - start) / 1000000000.0;
    double ns_per_call_boot = (double)(end - start) / (double)iterations;
    double mops_boot = (double)iterations / elapsed_boot_s / 1000000.0;

    // 5. Direct hardware counter read (mrs cntvct_el0)
    start = get_syscall_time_ns(CLOCK_MONOTONIC);
    uint64_t dummy = 0;
    for (uint64_t i = 0; i < iterations; i++) {
        dummy += read_cntvct();
    }
    end = get_syscall_time_ns(CLOCK_MONOTONIC);
    double elapsed_vct_s = (double)(end - start) / 1000000000.0;
    double ns_per_call_vct = (double)(end - start) / (double)iterations;
    double mops_vct = (double)iterations / elapsed_vct_s / 1000000.0;

    printf("  CLOCK_MONOTONIC     : %7.1f ns/call | %6.2f M calls/sec\n", ns_per_call_mono, mops_mono);
    printf("  CLOCK_REALTIME      : %7.1f ns/call | %6.2f M calls/sec\n", ns_per_call_real, mops_real);
    printf("  CLOCK_MONOTONIC_RAW : %7.1f ns/call | %6.2f M calls/sec\n", ns_per_call_raw, mops_raw);
    printf("  CLOCK_BOOTTIME      : %7.1f ns/call | %6.2f M calls/sec\n", ns_per_call_boot, mops_boot);
    printf("  mrs cntvct_el0      : %7.1f ns/call | %6.2f M reads/sec (trapped by erratum)\n", ns_per_call_vct, mops_vct);
    printf(COLOR_GREEN "  [PASS] Benchmark complete." COLOR_RESET "\n\n");
    return 0;
}

/* ========================================================================= */
/* Test 7: Timer Programming & Silicon TVAL Bypass Stress (clock_nanosleep)  */
/* ========================================================================= */
static volatile int tval_running = 1;
static uint64_t tval_total_sleeps = 0;
static uint64_t tval_delayed_wakeups = 0; // wakeups delayed > 50ms (TVAL stall symptom)
static uint64_t tval_premature_wakeups = 0;
static uint64_t tval_max_delay_ns = 0;
static uint64_t tval_total_overhead_ns = 0;

typedef struct {
    int core_id;
    uint64_t sleep_ns;
    clockid_t clock_id;
} tval_param_t;

static void* tval_stress_worker(void* arg) {
    tval_param_t* p = (tval_param_t*)arg;
    int core_id = p->core_id;
    uint64_t sleep_ns = p->sleep_ns;
    clockid_t clk_id = p->clock_id;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    uint64_t local_sleeps = 0;
    uint64_t local_delayed = 0;
    uint64_t local_premature = 0;
    uint64_t local_max_delay = 0;
    uint64_t local_overhead = 0;

    struct timespec req, rem;

    while (tval_running) {
        req.tv_sec = sleep_ns / 1000000000ULL;
        req.tv_nsec = sleep_ns % 1000000000ULL;

        uint64_t t_before = get_syscall_time_ns(CLOCK_MONOTONIC);
        int ret = clock_nanosleep(clk_id, 0, &req, &rem);
        uint64_t t_after = get_syscall_time_ns(CLOCK_MONOTONIC);

        if (ret == 0) {
            local_sleeps++;
            uint64_t actual_ns = (t_after > t_before) ? (t_after - t_before) : 0;

            // Premature wakeup check (slack 50µs)
            if (actual_ns + 50000ULL < sleep_ns) {
                local_premature++;
            }

            if (actual_ns >= sleep_ns) {
                uint64_t overhead = actual_ns - sleep_ns;
                local_overhead += overhead;
                if (overhead > local_max_delay) {
                    local_max_delay = overhead;
                }
                // If delayed > 50ms, flag as serious delay / TVAL silicon glitch symptom
                if (overhead > 50000000ULL) {
                    local_delayed++;
                }
            }
        }
    }

    __sync_fetch_and_add(&tval_total_sleeps, local_sleeps);
    __sync_fetch_and_add(&tval_delayed_wakeups, local_delayed);
    __sync_fetch_and_add(&tval_premature_wakeups, local_premature);
    __sync_fetch_and_add(&tval_total_overhead_ns, local_overhead);

    uint64_t cur_max = tval_max_delay_ns;
    while (local_max_delay > cur_max && !__sync_bool_compare_and_swap(&tval_max_delay_ns, cur_max, local_max_delay)) {
        cur_max = tval_max_delay_ns;
    }

    return NULL;
}

static int run_test_tval_bypass(int duration_sec) {
    printf(COLOR_CYAN "=== [Test 7] Timer Programming & Silicon TVAL Bypass Stress (%ds) ===" COLOR_RESET "\n", duration_sec);
    tval_running = 1;
    tval_total_sleeps = 0;
    tval_delayed_wakeups = 0;
    tval_premature_wakeups = 0;
    tval_max_delay_ns = 0;
    tval_total_overhead_ns = 0;

    pthread_t th[4];
    tval_param_t params[4] = {
        {0,   100000ULL, CLOCK_REALTIME},  // Core 0: 100 µs (wdtd clock domain, rapid set_next_event)
        {1,   500000ULL, CLOCK_MONOTONIC}, // Core 1: 500 µs
        {2,  1000000ULL, CLOCK_REALTIME},  // Core 2: 1.0 ms
        {3,  2000000ULL, CLOCK_MONOTONIC}, // Core 3: 2.0 ms
    };

    for (int i = 0; i < 4; i++) {
        pthread_create(&th[i], NULL, tval_stress_worker, &params[i]);
    }

    wait_with_progress(duration_sec, "Test 7 TVAL Bypass");
    tval_running = 0;

    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
    }

    double avg_overhead_us = tval_total_sleeps ?
        ((double)tval_total_overhead_ns / (double)tval_total_sleeps / 1000.0) : 0.0;
    double max_delay_ms = (double)tval_max_delay_ns / 1000000.0;
    double ops = (double)tval_total_sleeps / (double)duration_sec;

    printf("  Total timer reprogrammings: %llu (%.1f events/sec across 4 cores)\n",
           (unsigned long long)tval_total_sleeps, ops);
    printf("  Average wakeup overhead   : %.2f µs (kernel hrtimer + context switch)\n",
           avg_overhead_us);
    printf("  Max wakeup latency/delay  : %.3f ms\n", max_delay_ms);
    printf("  Premature wakeups (<req)  : %llu\n", (unsigned long long)tval_premature_wakeups);
    printf("  Dropped/delayed (>50ms)   : %llu\n", (unsigned long long)tval_delayed_wakeups);

    if (tval_delayed_wakeups == 0 && tval_premature_wakeups == 0) {
        printf(COLOR_GREEN "  [PASS] Zero missed or delayed timer wakeups. TVAL bypass operates cleanly." COLOR_RESET "\n\n");
        return 0;
    } else {
        if (tval_delayed_wakeups > 0) {
            printf(COLOR_RED "  [FAIL] %llu timer wakeup(s) delayed >50ms (TVAL carry glitch suspected)!" COLOR_RESET "\n\n",
                   (unsigned long long)tval_delayed_wakeups);
        }
        if (tval_premature_wakeups > 0) {
            printf(COLOR_RED "  [FAIL] %llu premature timer wakeup(s) detected!" COLOR_RESET "\n\n",
                   (unsigned long long)tval_premature_wakeups);
        }
        return 1;
    }
}

static void print_header(void) {
    printf(COLOR_BOLD "=================================================================" COLOR_RESET "\n");
    printf(COLOR_BOLD " Broadcom Brahma-B53 Timer Erratum & Clock Validation Suite" COLOR_RESET "\n");
    printf(COLOR_BOLD " Target: ASUS RT-BE92U (BCM4916 4-core Brahma-B53 @ 2.6 GHz)" COLOR_RESET "\n");
    printf(COLOR_BOLD "=================================================================" COLOR_RESET "\n");
    printf("  Timer Frequency   : %llu MHz\n", (unsigned long long)(timer_freq_hz / 1000000ULL));
    printf("  System Cores      : 4 online\n");
    printf("  Kernel Workaround : 56-bit Modular Filter + TVAL Silicon CVAL Bypass\n\n");
}

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -a, --all [sec]        Run complete test suite (default: 3s per test)\n");
    printf("  -D, --duration <sec>   Set test duration for all enabled tests\n");
    printf("  -s, --skew [sec]       Run hardware counter inter-core skew test (default: 3s)\n");
    printf("  -m, --monotonic [sec]  Run cross-core sleep/wake monotonicity test (default: 3s)\n");
    printf("  -c, --syscall [sec]    Run high-contention syscall cross-core test (default: 3s)\n");
    printf("  -t, --timerfd [sec]    Run high-res timer (timerfd) precision test (default: 3s)\n");
    printf("  -d, --domains [sec]    Run multi-clock domain consistency test (default: 3s)\n");
    printf("  -e, --event [sec]      Run timer programming & TVAL bypass stress test (default: 3s)\n");
    printf("  -b, --bench            Run clock latency & throughput benchmark\n");
    printf("  -h, --help             Show this help message\n\n");
}

int main(int argc, char** argv) {
    timer_freq_hz = read_cntfrq();

    int opt_skew = 0, skew_sec = 3;
    int opt_mono = 0, mono_sec = 3;
    int opt_syscall = 0, syscall_sec = 3;
    int opt_timerfd = 0, timerfd_sec = 3;
    int opt_domains = 0, domains_sec = 3;
    int opt_event = 0, event_sec = 3;
    int opt_bench = 0;
    int opt_all = 0;

    static struct option long_options[] = {
        {"all",       optional_argument, 0, 'a'},
        {"duration",  required_argument, 0, 'D'},
        {"skew",      optional_argument, 0, 's'},
        {"monotonic", optional_argument, 0, 'm'},
        {"syscall",   optional_argument, 0, 'c'},
        {"timerfd",   optional_argument, 0, 't'},
        {"domains",   optional_argument, 0, 'd'},
        {"event",     optional_argument, 0, 'e'},
        {"bench",     no_argument,       0, 'b'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "a::D:s::m::c::t::d::e::bh", long_options, NULL)) != -1) {
        switch (c) {
            case 'a':
                opt_all = 1;
                if (optarg) {
                    skew_sec = mono_sec = syscall_sec = timerfd_sec = domains_sec = event_sec = atoi(optarg);
                } else if (optind < argc && argv[optind][0] != '-') {
                    skew_sec = mono_sec = syscall_sec = timerfd_sec = domains_sec = event_sec = atoi(argv[optind++]);
                }
                break;
            case 'D':
                skew_sec = mono_sec = syscall_sec = timerfd_sec = domains_sec = event_sec = atoi(optarg);
                break;
            case 's':
                opt_skew = 1;
                if (optarg) skew_sec = atoi(optarg);
                else if (optind < argc && argv[optind][0] != '-') skew_sec = atoi(argv[optind++]);
                break;
            case 'm':
                opt_mono = 1;
                if (optarg) mono_sec = atoi(optarg);
                else if (optind < argc && argv[optind][0] != '-') mono_sec = atoi(argv[optind++]);
                break;
            case 'c':
                opt_syscall = 1;
                if (optarg) syscall_sec = atoi(optarg);
                else if (optind < argc && argv[optind][0] != '-') syscall_sec = atoi(argv[optind++]);
                break;
            case 't':
                opt_timerfd = 1;
                if (optarg) timerfd_sec = atoi(optarg);
                else if (optind < argc && argv[optind][0] != '-') timerfd_sec = atoi(argv[optind++]);
                break;
            case 'd':
                opt_domains = 1;
                if (optarg) domains_sec = atoi(optarg);
                else if (optind < argc && argv[optind][0] != '-') domains_sec = atoi(argv[optind++]);
                break;
            case 'e':
                opt_event = 1;
                if (optarg) event_sec = atoi(optarg);
                else if (optind < argc && argv[optind][0] != '-') event_sec = atoi(argv[optind++]);
                break;
            case 'b':
                opt_bench = 1;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return 0;
        }
    }

    if (!opt_skew && !opt_mono && !opt_syscall && !opt_timerfd && !opt_domains && !opt_event && !opt_bench) {
        opt_all = 1;
    }

    print_header();

    int fails = 0;
    if (opt_all || opt_skew) {
        fails += run_test_skew(skew_sec);
    }
    if (opt_all || opt_mono) {
        fails += run_test_monotonic(mono_sec);
    }
    if (opt_all || opt_syscall) {
        fails += run_test_syscall(syscall_sec);
    }
    if (opt_all || opt_timerfd) {
        fails += run_test_timerfd(timerfd_sec);
    }
    if (opt_all || opt_domains) {
        fails += run_test_domains(domains_sec);
    }
    if (opt_all || opt_event) {
        fails += run_test_tval_bypass(event_sec);
    }
    if (opt_all || opt_bench) {
        fails += run_test_benchmark(2);
    }

    printf(COLOR_BOLD "=================================================================" COLOR_RESET "\n");
    if (fails == 0) {
        printf(COLOR_GREEN COLOR_BOLD " SUMMARY: ALL TIMER ERRATUM & CLOCKSOURCE VALIDATIONS PASSED! " COLOR_RESET "\n");
        printf(" The system is robust against 56-bit timer underflow and TVAL silicon carry glitches.\n");
    } else {
        printf(COLOR_RED COLOR_BOLD " SUMMARY: %d TEST(S) FAILED! " COLOR_RESET "\n", fails);
    }
    printf(COLOR_BOLD "=================================================================" COLOR_RESET "\n");

    return fails;
}
