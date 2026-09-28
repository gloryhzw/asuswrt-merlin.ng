asuswrt-merlin New Gen (version 382.xx and higher)
==================================================

#### Support is available via the forums at [SNBForums](https://www.snbforums.com/forums/asuswrt-merlin.42/).

Asuswrt-Merlin is an enhanced version of Asuswrt, the firmware used by Asus's modern routers.

The goal of this project is to fix issues and bring some minor functionality adjustments to the 
original Asus firmware.  While some features do get added, this is not the main focus of this project.  
It is not meant to replace existing projects such as Tomato or DD-WRT, but rather to offer an alternative 
for people who prefer the original firmware featureset.

This is the new development branch, originally based on Asus's 
3.0.0.4.382_xxxx firmware release.  Development of the 380.xx 
legacy branch has been dropped.

Please consult the Wiki for an up-to-date list of supported models:

https://github.com/RMerl/asuswrt-merlin.ng/wiki/Supported-Devices

RT-BE92U / Broadcom Brahma-B53 Timer Errata & Fixes
---------------------------------------------------

This fork contains the independent discovery, comprehensive root-cause analysis, and bespoke kernel-level fixes for two critical hardware silicon errata in the **Broadcom Brahma-B53** CPU core (used in BCM6765, BCM4908, BCM4916, and related high-performance Wi-Fi 7 / Wi-Fi 6 router SoCs).

> **Note:** These hardware errata were **independently discovered, analyzed, and resolved by this project (`gloryhzw`)**. They are neither documented nor resolved in Broadcom's official SDK/reference code, nor in upstream Linux.

### 1. The Hardware Errata

#### Erratum 1: Asynchronous Ripple-Carry Counter Glitch & Sched Clock Underflow
The Brahma-B53 architectural system counter (`cntpct_el0` / `cntvct_el0`) uses an internal asynchronous ripple-carry counter design. During carry propagation across bit stages, counter reads can transiently glitch and read lower values. 

Furthermore, cross-core skew between CPU cores during periodic `sched_clock` epoch updates causes unsigned 56-bit modular cycle underflows (~28.5-year jumps into the future). This triggers Linux scheduler real-time (RT) budget throttling and eventual hardware watchdog reboot loops.

#### Erratum 2: Timer Down-Counter Silicon TVAL Addition Glitch (`CNTP_TVAL_EL0`)
In stock Linux on ARM64, the high-resolution timer (`hrtimer`) subsystem programs one-shot timer events by writing the requested delta into the 32-bit signed timer value register `CNTP_TVAL_EL0`. In Brahma-B53 silicon, writing to `TVAL` causes the CPU hardware to automatically compute:

$$\text{CVAL} = \text{Raw\_Counter} + \text{TVAL}$$

internally in silicon using an un-filtered hardware adder connected directly to the raw ripple counter.

If software writes to `TVAL` at the exact clock cycle where an internal carry ripple is propagating across the physical counter, the hardware computes `CVAL` with a corrupted counter state far into the future. When this occurs, the hardware timer interrupt (`CNTP_CTL_EL0` condition $\text{Raw\_Counter} \ge \text{CVAL}$) fails to fire at the scheduled time. Sleeping threads—such as the userspace watchdog feeder daemon `wdtd` inside `clock_nanosleep()`—become stranded in uninterruptible sleep, triggering an unrecoverable hardware watchdog reboot.

### 2. The Bespoke Kernel Fixes

1. **Per-CPU 56-bit Modular Monotonic Enforcer (`arm_arch_timer.c`)**:
   - Replaces lock-heavy cross-core atomic CAS spinlocks with ultra-fast $O(1)$ per-CPU tracking (`__this_cpu_read/write`), eliminating multi-core lock contention.
   - Evaluates cycle deltas in the 56-bit modular domain:
     $$\Delta = (\text{raw} - \text{prev}) \ \& \ \text{CLOCKSOURCE\_MASK}(56)$$
     Any negative delta (indicated by sign bit 55) is detected as a ripple cascade glitch and clamped to the previous monotonic value.
   - Seamlessly handles natural 56-bit counter rollover without deadlock.

2. **Scheduler Clock Underflow Protection (`sched_clock.c`)**:
   - Protects `sched_clock()` and `update_sched_clock()` against cross-core epoch skew by clamping negative cycle deltas to zero, preventing 28.5-year scheduler budget underflows.

3. **Timekeeping Last-Cycle Validation (`Kconfig`)**:
   - Selects `CONFIG_CLOCKSOURCE_VALIDATE_LAST_CYCLE` on ARM64 for robust kernel timekeeping.

4. **Timer Programming Silicon TVAL Bypass (`arm_arch_timer.c`)**:
   - Hooks `.set_next_event_phys` and `.set_next_event_virt` to bespoke erratum handlers `erratum_set_next_event_tval_phys/virt`.
   - Bypasses the CPU silicon's buggy un-filtered hardware adder entirely: software calculates:
     $$\text{CVAL} = \text{Filtered\_Counter} + \text{evt}$$
     using the 56-bit monotonic filter and writes directly to `cntp_cval_el0`, completely immunizing the system against missed timer interrupts caused by carry ripple during timer programming.

5. **Watchdog Panic Governor & Microsecond Logging (`config_base.6a.6765`)**:
   - Enables `CONFIG_WATCHDOG_PRETIMEOUT_GOV_PANIC=y` so that watchdog pre-timeouts trigger a kernel panic backtrace to preserve crash context in memory and NVRAM rather than silent hardware resets.
   - Enables `CONFIG_PRINTK_TIME=y` and `BCM_PRINTK_TIME=y` for microsecond-precision timestamps.

### 3. Validation Suite: `b53_bench`

A dedicated bare-metal C test suite ([`b53_bench.c`](https://github.com/gloryhzw/asuswrt-merlin.ng/releases/download/0.99/b53_bench.c)) was authored to validate Brahma-B53 timer monotonicity, cross-core skew, and TVAL bypass stability under live SMP loads.

#### Test Coverage
- **Test 1: Hardware Counter Core-to-Core Skew**: Pins workers to cores 0 and 1; reads `cntvct_el0` millions of times to detect backward skew and 56-bit underflows.
- **Test 2: Cross-Core Sleep & Wake Monotonicity**: Fast clock reader on core 0 and intermittent readers on cores 1-3 testing `CLOCK_MONOTONIC` consistency across sleep cycles.
- **Test 3: High-Contention Syscall Cross-Core Flood**: Causal verification of `SYS_clock_gettime` across all 4 cores with memory barrier acquire/release semantics.
- **Test 4: High-Res Timer (`timerfd`) Precision**: Sub-millisecond periodic timers (250 µs, 500 µs, 1 ms, 2 ms) verifying zero premature expirations and bounded jitter.
- **Test 5: Multi-Clock Domain Consistency**: Validates monotonic relationship across `CLOCK_MONOTONIC`, `CLOCK_MONOTONIC_RAW`, and `CLOCK_BOOTTIME`, plus NTP frequency slew bounds.
- **Test 6: Clock Latency & Throughput Benchmark**: Measures calls/sec and nanosecond latency for all major POSIX clocks and raw `mrs cntvct_el0`.
- **Test 7: Timer Programming & Silicon TVAL Bypass Stress**: Concurrently hammers `clock_nanosleep()` across all 4 CPU cores using `CLOCK_REALTIME` and `CLOCK_MONOTONIC` to ensure zero missed or delayed (>50 ms) timer wakeups.

#### Running `b53_bench` on the Router
A statically linked aarch64 binary is available for download in [Release 0.99](https://github.com/gloryhzw/asuswrt-merlin.ng/releases/tag/0.99):

```sh
# Download and execute on the router
wget -O /tmp/b53_bench https://github.com/gloryhzw/asuswrt-merlin.ng/releases/download/0.99/b53_bench_static
chmod +x /tmp/b53_bench

# Run complete validation suite (3 seconds per test)
/tmp/b53_bench -a 3
```

### 4. Verification & Live Operational Results

- **Uptime Verification**: Confirmed continuous uptime exceeding **16 continuous days (almost 384 hours, 1,381,758 seconds)** on the **Asus RT-BE92U** under heavy live home routing traffic with zero clock regressions, zero lock contention, and zero rollover deadlocks.
- **Synthetic Stress Results (Kernel #14)**:
  - Over **14.8 million** cross-core syscall checks at **4.96 Mops**: **0 underflows**, max skew bounded to 0.325 µs.
  - Over **29,000** rapid timer reprogrammings across all 4 cores: **0 missed timer interrupts**, **0 delayed wakeups**, **0 premature firings**.
  - All 7 validation tests pass with 100% success.
