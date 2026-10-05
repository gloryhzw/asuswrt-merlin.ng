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

This fork contains the independent discovery, root-cause analysis, and kernel-level fixes for a system counter erratum in the **Broadcom Brahma-B53** CPU core, as found on the **Asus RT-BE92U** (kernel 4.19, `release/src-rt-5.04behnd.4916`), plus the watchdog and scheduler changes made while chasing the resulting reboots.

> **Note:** This erratum was **independently discovered, analyzed, and worked around by this project (`gloryhzw`)**. It is neither documented nor handled in Broadcom's SDK/reference code, nor in upstream Linux (upstream knows the Brahma-B53 MIDR only for Spectre/KPTI whitelists, not for any timer workaround).

### 1. The Hardware Erratum

#### Measured: counter reads that go backwards
Reads of the 80 MHz architectural system counter (`cntpct_el0` / `cntvct_el0`) occasionally return a value **behind the previous read**. Measured on the RT-BE92U with `test_b53_bidir` and `b53_timer_test` (unfiltered kernel):

- Drop sizes measured by the kernel's own `/proc/b53_timer` histogram (Kernel #19):
  - **Normal load**: 1 to 127 ticks (12.5 ns to 1.6 µs), about 2 clamped reads per second across all CPUs.
  - **All 4 cores reading in a tight loop** (`b53_timer_test`, 185 M reads in 10 s): about 60 glitch events per second, with two groups of sizes: 1 to 127 ticks, and **2,048 to 16,383 ticks (25 to 205 µs)**, plus a few in between. No drop reached 2^14 ticks or more.

  ```
  clamp size (ticks)   cpu0   cpu1   cpu2   cpu3
       4-7              361    356    380    352
      32-63             292    294    310    298
    2048-4095           122    121    121    121
    4096-8191           234    232    232    233
    8192-16383          120    119    119    119
  ```
- All cores see **the same bad value at the same moment** (e.g. `0x3e3ab847a -> 0x3e3ab842b` on cores 0 and 1), and the histogram counts are nearly identical on every CPU, so it is the shared counter, not per-core skew.
- The 2,048 to 16,383 tick group matches a carry into bits 11 to 13 being read before it settles. Every measured drop is still well inside the 1 ms (80,000 tick) clamp window.
- The suspected mechanism is an asynchronous ripple-carry counter whose low bits are briefly read before a carry has settled. This is a hypothesis consistent with the data, not a confirmed silicon description.

#### Why a tiny glitch reboots the router
Linux assumes the counter never goes backwards. A drop of even 1 tick produces a huge unsigned delta in code that does `(now - last) & mask`:

- `sched_clock()` computes `(cyc - epoch_cyc) & 56-bit mask`. A negative delta becomes ~2^56 ticks (~28.5 years), which trips RT throttling and stalls the scheduler.
- Without `CONFIG_CLOCKSOURCE_VALIDATE_LAST_CYCLE`, `clocksource_delta()` has the same problem in timekeeping.

The end result observed in crash logs was CPU 0 stalling, `wdtd` not petting `/dev/watchdog`, and a hardware watchdog reset (`BOOT REASON WATCHDOG 0x3424`).

#### Not measured: large glitches and forward spikes
Earlier analysis assumed glitches could also be much larger (a carry into bit 20, 32 or 44, i.e. 13 ms, 53.7 s or days) or **forward** (+53.7 s for bit 32), and that the TVAL hardware adder (`CVAL = counter + TVAL`) could latch such a value and push a timer far into the future. **None of this has been observed.** The 0.6 to 13 ms "forward glitches" reported by `test_b53_bidir` are gaps where the test thread was preempted (its threshold flags any gap over 625 µs, and no matching backward correction ever follows). The current filter still defends against large glitches in both directions, because doing so costs almost nothing.

### 2. The Kernel Fixes

1. **Pair-verified counter filter (`drivers/clocksource/arm_arch_timer.c`)**:
   Hooked through the arm64 out-of-line timer erratum framework (`ARM64_WORKAROUND_B53_TIMER`, matched on the Brahma-B53 MIDR), so every kernel read, `sched_clock()` and every userspace `mrs cntvct_el0` (trapped) goes through it. Each CPU remembers the last value it returned:
   - **Forward step < 1 ms**: normal case, returned as-is.
   - **Backward step < 1 ms**: a read glitch. The previous value is returned again, so the clock holds for a few reads instead of going backwards.
   - **Any larger jump, either direction** (first read on a CPU, wake from idle, or a large glitch): the value is re-read until two back-to-back reads agree (in order, within 12.8 µs). A single bad read is therefore never returned, stored, or used to program a timer. If the agreed value is still ≥ 1 ms behind the stored one, the stored value was the bad one and the CPU resyncs to the counter.
   - **Diagnostics**: `/proc/b53_timer` shows per-CPU counts of each path (`clamped`, `episodes` = runs of consecutive clamped reads, `checked`, `rejected`, `resynced`, `unstable`) and a log2 histogram of clamp sizes. `echo 0 > /proc/b53_timer` resets the statistics. A non-zero `rejected` would be the first evidence of a glitch of 1 ms or more.
   - Per-CPU state with no locks or shared cache lines. The fast path adds a compare and a store to each read; the read path makes no function calls (safe for `notrace` / `sched_clock`).

2. **Timer programming via CVAL (`drivers/clocksource/arm_arch_timer.c`)**:
   `.set_next_event_phys/virt` use the framework's `erratum_set_next_event_tval_*`, which computes `CVAL = filtered counter + delta` in software and writes `cntp_cval_el0`/`cntv_cval_el0`, instead of writing `TVAL` and letting hardware add it to a possibly glitched counter. With measured glitch sizes the error from `TVAL` would be ≤ 205 µs (a timer firing early or late by at most that), so this is defensive.

3. **Scheduler clock underflow guard (`kernel/time/sched_clock.c`)**:
   - `sched_clock()` treats a negative delta against the epoch as 0 instead of ~28.5 years.
   - `update_sched_clock()` **keeps the old epoch** when its reading is behind it. (Kernel #17 set `epoch_cyc` to the low reading while keeping `epoch_ns`, which would make `sched_clock()` jump forward by the size of the drop.)

4. **Timekeeping last-cycle validation (`arch/arm64/Kconfig`)**:
   Selects `CONFIG_CLOCKSOURCE_VALIDATE_LAST_CYCLE` so `clocksource_delta()` clamps negative deltas to 0.

5. **Watchdog panic governor & printk timestamps (`config_base.6a.6765`)**:
   - `CONFIG_WATCHDOG_PRETIMEOUT_GOV_PANIC=y` and `CONFIG_WATCHDOG_PRETIMEOUT_DEFAULT_GOV_PANIC=y`: a watchdog pretimeout panics and leaves a backtrace (dumped to flash by `mtdoops`) instead of a silent reset.
   - `CONFIG_PRINTK_TIME=y` for microsecond timestamps in `dmesg`.

6. **Watchdog pretimeout interrupt affinity (`/proc/irq/41/smp_affinity`, startup script)**:
   - On Broadcom BCA platforms CPU 0 carries the switch packet queues (`crossbow_rxq/txq`), bridge `br0`, buffer recycling `bcmsw_recycle`, eMMC and console, while CPUs 1–3 mostly service the PCIe radios.
   - Unpinned SPIs land on CPU 0, so the watchdog pretimeout interrupt (IRQ 41, `ff800480.watchdog`) went to CPU 0. If CPU 0 hard-locks with interrupts disabled, it cannot take IRQ 41: `watchdog_notify_pretimeout()` never runs, nothing is dumped, and the ASIC watchdog resets the board.
   - Routing only to CPU 3 was tried, but CPU 3 is heavily loaded by 6 GHz Wi-Fi (`wl2`) and `crossbow_socket` IRQs.
   - **Remediation**: route IRQ 41 to CPUs 1–3 (`smp_affinity: e`) in `/jffs/scripts/init-start`. Any surviving core can take the pretimeout at 57 s, panic, and preserve the log.

7. **RT throttling disabled & watchdog daemon priority (startup script)**:
   - **Diagnosis of the CPU 0 lockup**:
     - *RCU starvation*: crash dumps showed `rcu: rcu_sched kthread starved for 15023 jiffies! ... ->cpu=0`, i.e. CPU 0 did not schedule for over 15 s.
     - *RT network thread*: `bcmsw_recycle` (PID 535) runs on CPU 0 at `SCHED_FIFO 75` and spends time inside `spin_lock_irqsave(&crossbow_enet_g.rx_lock, flags)`.
     - *RT throttling*: with `sched_rt_runtime_us = 950000`, `sched: RT throttling activated` suspended all RT tasks on the core, including `wdtd` at `SCHED_FIFO 98`, so it missed the 60 s watchdog window.
     - *The `-30` red herring*: `WLC_SCB_DEAUTHORIZE error (-30)` (`BCME_NOTFOUND`, a station that already aged out) appeared in crash logs but occurred 4,079 s before the panic and is unrelated.
   - **Remediation** (`/jffs/scripts/services-start`):
     - `/proc/sys/kernel/sched_rt_runtime_us = -1` (RT throttling disabled).
     - `wdtd` / `wdtctl` moved from `SCHED_FIFO 98` to `SCHED_OTHER` at `nice -20`, so watchdog petting no longer depends on the RT throttle group.

### 3. Validation Suite: `b53_bench`

A dedicated C test suite ([`b53_bench.c`](https://github.com/gloryhzw/asuswrt-merlin.ng/releases/download/0.99/b53_bench.c)) checks counter monotonicity, cross-core consistency, and timer behaviour under SMP load.

#### Test Coverage
- **Test 1: Hardware Counter Core-to-Core Skew**: Pins workers to cores 0 and 1; reads `cntvct_el0` millions of times to detect backward skew and 56-bit underflows.
- **Test 2: Cross-Core Sleep & Wake Monotonicity**: Fast clock reader on core 0 and intermittent readers on cores 1-3 testing `CLOCK_MONOTONIC` consistency across sleep cycles.
- **Test 3: High-Contention Syscall Cross-Core Flood**: Causal verification of `SYS_clock_gettime` across all 4 cores with memory barrier acquire/release semantics.
- **Test 4: High-Res Timer (`timerfd`) Precision**: Sub-millisecond periodic timers (250 µs, 500 µs, 1 ms, 2 ms) verifying zero premature expirations and bounded jitter.
- **Test 5: Multi-Clock Domain Consistency**: Validates monotonic relationship across `CLOCK_MONOTONIC`, `CLOCK_MONOTONIC_RAW`, and `CLOCK_BOOTTIME`, plus NTP frequency slew bounds.
- **Test 6: Clock Latency & Throughput Benchmark**: Measures calls/sec and nanosecond latency for all major POSIX clocks and `mrs cntvct_el0`.
- **Test 7: Timer Programming Stress**: Concurrently hammers `clock_nanosleep()` across all 4 CPU cores using `CLOCK_REALTIME` and `CLOCK_MONOTONIC` to ensure zero missed or delayed (>50 ms) timer wakeups.

> Because the kernel traps userspace `cntvct_el0` reads on this CPU, `mrs cntvct_el0` in these tests (and the "RAW" mode of `b53_timer_test`) measures the **filtered** value userspace sees, not the raw hardware counter.

#### Running `b53_bench` on the Router
A statically linked aarch64 binary is available in [Release 0.99](https://github.com/gloryhzw/asuswrt-merlin.ng/releases/tag/0.99):

```sh
# Download and execute on the router
wget -O /tmp/b53_bench https://github.com/gloryhzw/asuswrt-merlin.ng/releases/download/0.99/b53_bench_static
chmod +x /tmp/b53_bench

# Run complete validation suite (3 seconds per test)
/tmp/b53_bench -a 3
```

### 4. Kernel History & Results

- **Kernel #13 (full backward clamp)**: ran **16 days (1,381,758 s)** on the RT-BE92U under normal home traffic without a reboot.
- **Kernel #14 (+ CVAL timer programming)**: watchdog resets every 30–78 hours. The explanation at the time was a forward spike being stored as the last value and freezing the clock; since no forward spike has ever been measured, **the cause of these resets is unconfirmed**. The IRQ 41 affinity and RT throttling changes (fixes 6 and 7) were made in the same period.
- **Kernel #15 / #16 (stateless double-read)**: comparing two consecutive reads within 32 ticks let 81–239 tick drops through, because both reads can land in the same glitch. Per-CPU state is required.
- **Kernel #17 (12.5 ms clamp + forward spike check)**: `b53_timer_test` 186.3 M reads with **0 glitches**; `b53_bench -a 3` all 7 tests pass (13.6 M inter-core reads with 0 underflows; 13.7 M syscall checks at 4.57 Mops; 29,000+ timer reprogrammings with 0 missed or delayed wakeups). Two weaknesses: any drop of 12.5 ms or more was returned as a "rollover" (the counter cannot roll over for ~28 years), and `update_sched_clock()` could move the epoch backwards.
- **Kernel #18 (pair-verified filter)**: fixes both weaknesses and adds `/proc/b53_timer`.
- **Kernel #19 (+ clamp-size histogram, current)**: `b53_timer_test` 185.3 M reads with **0 glitches**; `rejected`, `resynced` and `unstable` all 0. Glitch sizes are documented in section 1. **Status: soak test started Oct 5 2026.** Treat it as proven only after more than 7 days of uptime (well past #14's 30–78 h failure window) with `rejected`, `resynced` and `unstable` still at 0.

### 5. Key File Locations & Source Code Map

#### A. Kernel source (under [`release/src-rt-5.04behnd.4916/kernel/linux-4.19/`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/))
- **Counter filter, CVAL timer programming, `/proc/b53_timer`**:
  [`drivers/clocksource/arm_arch_timer.c`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/drivers/clocksource/arm_arch_timer.c) — `b53_read_counter`, `b53_read_pair`, `b53_read_cntpct_el0` / `b53_read_cntvct_el0`, and the `ool_workarounds[]` entry.
- **Erratum capability & CPU match**:
  [`arch/arm64/kernel/cpu_errata.c`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/arch/arm64/kernel/cpu_errata.c), [`arch/arm64/include/asm/cpucaps.h`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/arch/arm64/include/asm/cpucaps.h), [`arch/arm64/include/asm/cputype.h`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/arch/arm64/include/asm/cputype.h) — `ARM64_WORKAROUND_B53_TIMER`, `MIDR_BRAHMA_B53`. ([`arch/arm64/kernel/cpufeature.c`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/arch/arm64/kernel/cpufeature.c) adds the B53 to the KPTI safe list.)
- **Scheduler clock guard**:
  [`kernel/time/sched_clock.c`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/kernel/time/sched_clock.c) — negative-delta clamp and epoch hold.
- **Timekeeping validation**:
  [`arch/arm64/Kconfig`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/arch/arm64/Kconfig) — selects `CONFIG_CLOCKSOURCE_VALIDATE_LAST_CYCLE`.
- **Kernel config for this platform**:
  [`config_base.6a.6765`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/config_base.6a.6765) — watchdog pretimeout panic governor and `CONFIG_PRINTK_TIME`. (The panic governor itself is the stock `drivers/watchdog/pretimeout_panic.c`, only enabled here.)

#### B. Validation & test tools (local build host, not in this repo)
- `/home/glory/b53_bench.c` — 7-test benchmark source; deployed on the router as `/jffs/b53_bench`.
- `/home/glory/merlin/test_b53_bidir.c` — backward/forward glitch logger (the forward column counts preemption gaps; see section 1).
- `/home/glory/merlin/b53_timer_test` — multi-core glitch detector; deployed as `/jffs/b53_timer_test`.

#### C. Build & flashing tools (local build host, not in this repo)
- `/home/glory/merlin/setup_env.sh` — toolchain paths and the `build_be92u` helper (build as user `glory`, never root).
- `/home/glory/upload_firmware.py` — Asuswrt Login v2 Web UI firmware flasher.
- `/home/glory/check_router_time.sh` — hourly cron check for router reboots, sending ntfy alerts.

#### D. Router startup scripts (persistent in `/jffs/scripts/`)
- `/jffs/scripts/init-start` — routes IRQ 41 to CPUs 1–3 (`smp_affinity: e`) and enables `printk.time=Y`.
- `/jffs/scripts/services-start` — sets `sched_rt_runtime_us = -1` and runs `wdtd` at `nice -20`.
