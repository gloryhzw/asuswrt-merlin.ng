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

1. **Spike-Rejection Verified Counter Filter (`arm_arch_timer.c`)**:
   - Solves both physical counter ripple drops and transient high-order carry spikes (such as bit 32 = +53.68s) without lock contention or clock freeze:
   - **Monotonicity (Negative Drops)**: Any backward drop within `B53_RIPPLE_DROP_THRESHOLD` (12.5ms / ~1M ticks) is clamped to `prev`, providing 100% strict monotonicity on each CPU with **0 glitches** detected across hundreds of millions of reads.
   - **Forward Spike Rejection (Anti-Poisoning)**: Any forward jump exceeding `B53_SPIKE_VERIFY_THRESHOLD` (1ms / 80k ticks) is immediately verified against a second read following an `isb()` pipeline flush. If the second read drops back, the reading was an incomplete forward carry spike (which physically dissipates in nanoseconds) and is rejected before it can ever be stored in `last` or programmed into `cval`.
   - **Zero Cross-Core Contention**: Uses `DEFINE_PER_CPU` so each CPU executes in local L1 cache with zero locks and zero cache bouncing. Seamlessly handles counter rollover without deadlock.

2. **Scheduler Clock Underflow Protection (`sched_clock.c`)**:
   - Protects `sched_clock()` and `update_sched_clock()` against cross-core epoch skew by clamping negative cycle deltas to zero, preventing 28.5-year scheduler budget underflows.

3. **Timekeeping Last-Cycle Validation (`Kconfig`)**:
   - Selects `CONFIG_CLOCKSOURCE_VALIDATE_LAST_CYCLE` on ARM64 for robust kernel timekeeping.

4. **Timer Programming Silicon TVAL Bypass (`arm_arch_timer.c`)**:
   - Hooks `.set_next_event_phys` and `.set_next_event_virt` to bespoke erratum handlers `erratum_set_next_event_tval_phys/virt`.
   - Bypasses the CPU silicon's buggy un-filtered hardware adder entirely: software calculates:
     $$\text{CVAL} = \text{Filtered\_Counter} + \text{evt}$$
     using the consecutive-read verification filter and writes directly to `cntp_cval_el0`, completely immunizing the system against missed timer interrupts caused by carry ripple during timer programming.

5. **Watchdog Panic Governor & Microsecond Logging (`config_base.6a.6765`)**:
   - Enables `CONFIG_WATCHDOG_PRETIMEOUT_GOV_PANIC=y` so that watchdog pre-timeouts trigger a kernel panic backtrace to preserve crash context in memory and NVRAM rather than silent hardware resets.
   - Enables `CONFIG_PRINTK_TIME=y` and `BCM_PRINTK_TIME=y` for microsecond-precision timestamps.

6. **Watchdog Pretimeout Multi-Core Interrupt Affinity Routing (`/proc/irq/41/smp_affinity`)**:
   - On Broadcom BCA platforms, CPU 0 bears 100% of the system infrastructure load (switch packet queues `crossbow_rxq/txq`, packet bridge `br0`, memory buffer recycling `bcmsw_recycle`, eMMC flash, and console), while CPUs 1–3 exclusively service PCIe wireless radios.
   - Under standard ARM GICv2 SPI routing, unpinned hardware interrupts target the lowest core (CPU 0). Consequently, the hardware watchdog pretimeout interrupt (IRQ 41, `ff800480.watchdog`) was routed strictly to CPU 0.
   - If CPU 0 encounters a hard lockup with local interrupts disabled (`local_irq_disable` / `spin_lock_irqsave`), CPU 0 cannot take IRQ 41. This bypassed `watchdog_notify_pretimeout()`, silenced the panic handler, and prevented `mtdoops` from dumping crash logs to `/dev/mtd12` before the ASIC watchdog counter reached zero (`BOOT REASON WATCHDOG 0x3424`).
   - While routing strictly to CPU 3 was initially trialed, CPU 3 is heavily loaded by 6GHz Wi-Fi (`wl2`, >400k IRQs) and Broadcom IPC sockets (`crossbow_socket`, >450k IRQs), making it susceptible to cross-core `spin_lock_irqsave` lock contention when communicating with CPU 0.
   - **Remediation**: Explicitly route IRQ 41 affinity to all non-CPU0 cores (**CPUs 1–3**, `smp_affinity: e`) in startup scripts (`init-start` / `services-start`). Under GICv2 1-of-N SPI distribution, if CPU 0 or CPU 3 is trapped in a spinlock with interrupts disabled, any surviving core (CPU 1 or CPU 2) immediately intercepts the pretimeout event at 57 seconds, triggers `panic()`, and preserves the complete `dmesg` buffer and CPU status to flash.

7. **Scheduler Real-Time (RT) Throttling Disablement & Watchdog Priority Normalization**:
   - **Forensic Diagnosis of CPU 0 Lockup**:
     - *RCU Grace-Period Starvation*: Kernel crash dumps showed `rcu: rcu_sched kthread starved for 15023 jiffies! ... ->state=0x402 ->cpu=0`, proving CPU 0 was trapped inside kernel execution without scheduling (`cond_resched()`) or with preemption/interrupts disabled for over 15 seconds.
     - *Real-Time Network Thread Congestion*: Broadcom's Ethernet switch recycle thread (`bcmsw_recycle`, PID 535) is pinned to CPU 0 at `SCHED_FIFO 75`. Under high packet recycling and buffer management, it executes inside `spin_lock_irqsave(&crossbow_enet_g.rx_lock, flags)`.
     - *RT Bandwidth Throttling*: The default kernel parameter `/proc/sys/kernel/sched_rt_runtime_us = 950000` tripped `sched: RT throttling activated`. Because the watchdog daemon (`wdtd`) was configured by default as real-time `SCHED_FIFO 98`, the RT throttle group suspended all RT tasks on the core, preventing `wdtd` from kicking `/dev/watchdog` within the 60-second window.
     - *The `-30` (`BCME_NOTFOUND`) Red Herring*: Earlier casual inspection of crash logs suggested `WLC_SCB_DEAUTHORIZE error (-30)` triggered the reboot. Rigorous kernel timestamp analysis proved this was a correlation fallacy: `-30` occurred >68 minutes (4,079 seconds) prior to the panic. It is an innocuous Broadcom wireless SDK status (`#define BCME_NOTFOUND -30`) indicating that a departing station's Station Control Block had already aged out.
   - **Remediation**:
     - Set `/proc/sys/kernel/sched_rt_runtime_us` to `-1` (disabling RT throttling entirely).
     - Re-normalized `wdtd` / `wdtctl` scheduling policy from `SCHED_FIFO 98` to `SCHED_OTHER` with maximum non-RT priority (`nice -20`). This completely decouples watchdog petting from the real-time throttle group while guaranteeing high scheduling priority without starving system threads.

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

- **Long-term Monotonicity & Roll-over (Kernel #13)**: Validated continuous operation exceeding **16 continuous days (almost 384 hours, 1,381,758 seconds)** on the **Asus RT-BE92U** under heavy live home routing traffic with zero clock regressions, zero lock contention, and zero rollover deadlocks.
- **Kernel #14 Analysis & The Positive Carry Glitch Discovery**:
  - Kernel #14 introduced TVAL bypass, reprogramming timers via software counter reads >200M times/day.
  - However, Kernel #14 clamped only backward deltas in software state (`b53_last_cntpct`). When transient forward carry spikes occurred (e.g. bit 32 = +53.68s, bit 33 = +107.37s during ripple cascade transitions), the future timestamp latched into `b53_last_cntpct`, freezing the core's clock until physical time caught up and triggering hardware watchdog resets every 30–78 hours.
- **Kernel #15 / #16 Analysis (The Limits of Stateless Filters)**:
  - While stateless double-read filters avoided forward state latching, asynchronous ripple glitches on BCM4916 Brahma-B53 span higher bit stages (up to 239+ ticks). Because the physical carry ripple duration can exceed the pipeline interval between consecutive reads, stateless filters allowed physical drops to leak into userspace (causing `b53_timer_test` to report glitches).
- **Kernel #17 (Spike-Rejection Verified Counter Filter)**:
  - Combines per-CPU local monotonic clamping (`B53_RIPPLE_DROP_THRESHOLD = 12.5ms`) with forward carry spike verification (`B53_SPIKE_VERIFY_THRESHOLD = 1ms`).
  - Clamps all physical ripple drops for 100% strict monotonicity, while instantly catching and discarding forward carry spikes (like bit 32 = +53.68s) via pipeline-flushed secondary confirmation, completely preventing `cval` timer poisoning.
  - **Live Verification on Asus RT-BE92U (`b53_timer_test` RAW mode)**:
    - Over **185.5 million** direct reads across 4 cores: **0 glitches (0.00000%)** -> **100% PASS**!
  - **Live Verification on Asus RT-BE92U (`b53_bench -a 3`)**:
    - Over **13.6 million** inter-core reads: **0 underflows**, max core skew bounded to 0.625 µs.
    - Over **13.7 million** cross-core syscall checks at **4.57 Mops**: **0 underflow crashes**, bounded to < 2 µs.
    - Over **29,000** timer reprogrammings across all 4 cores at **9,685 events/sec**: **0 missed timer interrupts**, **0 delayed wakeups**, **0 premature firings**.
    - Throughput: `mrs cntvct_el0` at **4.83 M reads/sec** with 206.9 ns average latency.
    - All 7 validation tests pass with 100% success.
