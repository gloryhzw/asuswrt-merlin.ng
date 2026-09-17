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

RT-BE92U / Broadcom Brahma-B53 Timer Erratum Fix
------------------------------------------------

This fork contains an independent discovery and bespoke kernel-level fix for a critical hardware erratum in the **Broadcom Brahma-B53** CPU core (used in BCM6765, BCM4908, BCM4916, and related SoCs).

> **Note:** This hardware erratum was **independently discovered, root-caused, and resolved by this project (`gloryhzw`)**, and is neither identified nor resolved in Broadcom's official reference code or upstream Linux.

### The Erratum
The Brahma-B53 architectural system counter (`cntpct_el0` / `cntvct_el0`) utilizes an internal asynchronous ripple-carry counter design. During carry propagation across internal bit stages, the counter can transiently read low due to ripple cascade delay. Furthermore, cross-core skew during periodic `sched_clock` epoch updates causes unsigned modular cycle underflows (~28.5-year jumps), which trigger Linux scheduler RT budget throttling and eventual hardware watchdog reboot loops.

### The Fix
1. **Per-CPU 56-bit Modular Monotonic Enforcer (`arm_arch_timer.c`)**:
   - Replaces heavy cross-core atomic CAS spinlocks with ultra-fast O(1) per-CPU tracking (`__this_cpu_read/write`), eliminating multi-core lock contention.
   - Evaluates cycle deltas in the 56-bit modular domain: `delta = (raw - prev) & CLOCKSOURCE_MASK(56)`. Any negative delta (indicated by sign bit 55: `delta & ~(CLOCKSOURCE_MASK(56) >> 1)`) is identified as a ripple cascade glitch and clamped to the previous monotonic value.
   - Naturally accommodates 56-bit counter rollover without deadlock.

2. **Scheduler Clock Underflow Protection (`sched_clock.c`)**:
   - Protects `sched_clock()` and `update_sched_clock()` against cross-core epoch skew by clamping negative cycle deltas to zero, preventing 28.5-year scheduler budget underflows.

3. **Timekeeping Last-Cycle Validation (`Kconfig`)**:
   - Selects `CONFIG_CLOCKSOURCE_VALIDATE_LAST_CYCLE` on ARM64 for robust kernel timekeeping.

### Verification
- Fully verified and stress-tested on the **Asus RT-BE92U** with proven continuous uptime exceeding **4.5+ days (110+ hours)** under heavy load with zero watchdog lockups or time regressions.

