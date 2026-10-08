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

#### Measured: what a bad value looks like (Kernel #20 episode samples)
- **Torn carries.** A bad value appears right after a carry. Each bit is either its old or its new value, as if part of the carry had not arrived yet. The split is usually around bit 10/11.
- **The offset persists and keeps counting.** A bad value is not a single wrong read. The counter view as a whole is shifted and keeps counting at the normal rate with that offset until the stale bits catch up:
  - small group: 64–94 ticks behind, lasting ~18 µs;
  - large group (the 2,048–16,383 histogram rows): up to ~15,500 ticks (194 µs) behind.
- **Mostly backward, but not always.** Almost every offset reads low. One forward offset of +2,026 ticks has been seen.
- **One momentary error of 1 ms or more** (`rejected = 1`). It vanished on the next read, so the pair check dropped it.
- **The view gains time.** Compared with the Broadcom peripheral timer, the arch counter view gains ~250–300 ppm in small forward steps of 25–90 µs that never come back. The kernel measured up to ~900 ppm in the first minutes after boot. The peripheral timer itself tracks NTP time within ~30 ppm.

#### Root cause: carries that do not complete (captured bit by bit)
A test module (`b53_jump.ko`) read the raw counter in a tight loop on one CPU, bypassing the kernel filter, for one hour with about 45% coverage. It saved the exact values around every step of 2^15 ticks or more and compared each one with the peripheral reference timer.

**Every captured event happened at a carry**, i.e. when adding 1 had to flip many bits at once (`0x…ffff → 0x…0000`, 12 to 23 bits in the captures). In each case the counter ended up with a mix of old and new bits. There are three outcomes:

| Outcome | What the bits show | Error | Example (carry into bit) |
|---|---|---|---|
| **Stale lower bits** (most common) | bit k sets, but some lower bits keep their old 1s | forward, up to +2^k ("2^k minus a little") | `…c1fffe → …c3dc16`, +121,880 (17); `…4b7ffffe → …4ba000ba`, +2,097,340 = 2^21 + 188 (23) |
| **Lost carry** | the lower bits clear, but bit k never sets | backward, −2^k | `…3efffe → …3e00ba`, −65,348 = −2^16 + 188 (16) |
| **Torn read only** | one read is torn, the next read is correct | heals at once | `…bfffe → …c7fff → …c0007`, +32,769 (18) |

In the first two cases **the counter keeps counting normally from the wrong value**: every later read is consistent, and the window ends off from the reference by the full amount. So the error is a new starting point, not a bad read, and it never undoes itself.

Totals for the hour:
- **17 captured events**: 10 permanent (8 forward, 2 backward) and 7 read-only tears, the largest −360,611 ticks (−4.5 ms) at a carry through 21 bits;
- 94 more windows with a net error of 4,096 ticks or more against the reference (smaller permanent steps, below the capture threshold);
- ~53,000 steps of 64–32,767 ticks.

**The failures repeat exactly.** Events 4 and 15, almost an hour apart, were both carries into bit 17 and both ended with the low 18 bits at exactly `0x3dc16` (+121,880 ticks, the same to the tick). Four other permanent events (carries into bits 16, 18 and 23, both directions) all ended with low bits `0x…0ba`. The bits are 0x0ba = 186 ticks (2.3 µs) past the carry, as if the bad value is always stored at the same moment after the carry starts. So the failure looks like a fixed logic flaw in specific bits, not random electrical noise.

What follows from this:
- **The jump size is the value of the stale bit(s).** The forward steps of the ~300 ppm gain are failed carries into bits 11–14, and the 2^17–2^29 corrections in the soak (section 4) are the same failure in higher bits.
- **Any bit can fail.** Large jumps are rare only because a carry into bit k happens once every 2^k ticks. Up to 2^29 (+6.66 s) has been seen, and nothing in the data rules out higher bits.
- **The fault is in the counter shared by all cores**, which is why every CPU sees the same jump.

Two real events in the Kernel #23 soak show the same thing at higher bits:

- **+2^32 (+53.7 s), 2026-10-07 ~04:09.** At the carry `0x747_FFFFFFFF` → `0x748_00000000`, the stored value was `0x749_00000016`. Bits 33–35 changed but bit 32 kept its old 1. This is beyond the 26.8 s RCU-stall limit; #23 fixed it 14 µs later.
- **−24,113,417 ticks (−301 ms), 2026-10-07 ~09:00.** At a bit-25 carry, bit 25 lost its carry while bits 23 and 20 kept their old 1s: −2^25 + 2^23 + 2^20. One carry can fail in both directions at once, so sizes are not always powers of 2.
- **−2^31 (−26.8 s), 2026-10-08 ~04:02.** At the carry `0xd89_7FFFFFFF` → `0xd89_80000000`, bits 0–30 cleared but bit 31 never received the carry: the stored value was `0xd89_00000000`. This is the size of backward jump that underflows sched_clock and stalls timers; #23 fixed it about 1.3 ms later.

**Likely mechanism: a clock-domain crossing (CDC) bug.** A synchronous 56-bit counter at 80 MHz has plenty of timing margin, and static timing analysis would catch a slow carry, so the counter logic itself is unlikely to be at fault. The likely fault is where the count crosses from the system-counter clock into the B53 cluster's clock (the CPU clock, which BIUCFG varies). If the value crosses as plain binary with one synchronizer per bit, each bit settles on its own whenever many bits change at once. The receiver then latches a mix of old and new bits. Timing analysis ignores asynchronous crossings, so the tools would not flag it; Gray code or a handshake would avoid it. This fits every observation: failures only at multi-bit changes, mixed bits in both directions, the same shift on all 4 CPUs (one shared cluster copy), permanence (the copy keeps counting from what it latched), healing tears (the same race on a read-only path), and exact repeats (the crossing happens at a fixed point relative to the count). Only Broadcom could confirm it. A test that would separate the two: read the source counter over MMIO (ARM `CNTCV`, if the chip exposes it) alongside `cntvct_el0`. If the source stays clean while `cntvct_el0` jumps, the bug is in the crossing.

#### Why a tiny glitch reboots the router
Linux assumes the counter never goes backwards. A drop of even 1 tick produces a huge unsigned delta in code that does `(now - last) & mask`:

- `sched_clock()` computes `(cyc - epoch_cyc) & 56-bit mask`. A negative delta becomes ~2^56 ticks (~28.5 years), which trips RT throttling and stalls the scheduler.
- Without `CONFIG_CLOCKSOURCE_VALIDATE_LAST_CYCLE`, `clocksource_delta()` has the same problem in timekeeping.

The end result observed in crash logs was CPU 0 stalling, `wdtd` not petting `/dev/watchdog`, and a hardware watchdog reset (`BOOT REASON WATCHDOG 0x3424`).

#### Large permanent jumps (measured in the Kernel #23 soak)
Because a failed carry changes the counter itself, a failure in a high bit shifts time for good. In the first 8 hours of the #23 soak (Oct 6 2026) the kernel corrected **65 jumps of 1 ms or more**:
- most were ≈ 2^17 (1.6 ms) forward, one every 5–10 minutes;
- several ≈ 2^18 / 2^19 forward;
- three +2^20 and **four −2^20 (13 ms backward)**;
- **one +2^28 (+3.34 s)**, plus a +2^29 (+6.66 s) a few hours later.

How each kind of jump affects an unprotected kernel:
- **Backward (lost carry):** time goes backwards permanently, triggering the `sched_clock` / timekeeping underflow described above. The hardware timer comparator compares against the same counter, so a large backward jump also makes every timer late by that amount.
- **Forward:** time skips ahead, and every timer due in that interval fires at once. That is harmless up to about 25 s. From 2^31 (26.8 s) up, jiffies jump past `rcu_cpu_stall_timeout` (25 s), and with `panic_on_rcu_stall=1` (set in `init-start`) that would cause a false RCU stall panic. No jump that large has been seen yet.
- The 0928 crash (watchdog pretimeout with CPU 0 idle and no other stall detector firing) fits a large backward jump stalling all timers. This is consistent with the data but not proven.
- The 0.6 to 13 ms "forward glitches" reported by `test_b53_bidir` are gaps where the test thread was preempted (its threshold flags any gap over 625 µs, and no matching backward correction ever follows).

### 2. The Kernel Fixes

1. **Pair-verified counter filter (`drivers/clocksource/arm_arch_timer.c`)**:
   Hooked through the arm64 out-of-line timer erratum framework (`ARM64_WORKAROUND_B53_TIMER`, matched on the Brahma-B53 MIDR), so every kernel read, `sched_clock()` and every userspace `mrs cntvct_el0` (trapped) goes through it. Each CPU remembers the last value it returned:
   - **Forward step < 1 ms**: normal case, returned as-is.
   - **Backward step < 1 ms**: a read glitch. The previous value is returned again, so the clock holds for a few reads instead of going backwards.
   - **Any larger jump, either direction** (first read on a CPU, wake from idle, or a large offset):
     - The value is re-read until two back-to-back reads agree (in order, within 12.8 µs), which drops momentary glitches.
     - The agreed value is then checked against an **independent reference**: a Broadcom peripheral timer (`brcm,bcm-timers`, 200 MHz, 62-bit). Each reference read is bracketed by two arch reads and used only if the bracket is under 0.8 µs, because the bus read can stall for tens of µs.
     - If the arch view is off from the reference by **1 ms or more**, the view is offset. A shared compensation `b53_comp`, added to every read, is corrected by the measured error, so time keeps flowing at the right value. When the offset ends, the compensation is undone the same way.
   - **Never backwards**: whatever happens, a value below the CPU's last returned value is never returned. A large backward step that the reference does not confirm is held (`bigheld`) instead.
   - **Small drift is left alone**: the ~300 ppm gain of the arch view is shared by every CPU and every comparator, and NTP corrects wall time, so only offsets of 1 ms or more are corrected. (Kernels #21/#22 also corrected the drift and kept stepping time, see section 4.)
   - **Heartbeat**: a second peripheral timer interrupts every 10 ms, independent of the arch counter. It catches an offset that starts while every CPU is idle (nobody reads the counter, and every timer is late). It also keeps the arch/reference anchor and rate up to date. A new offset must be seen on two beats in a row; an offset that ends is undone at once.
   - **Early boot**: before the reference is set up (`late_initcall`), a confirmed backward step is compensated by its own size instead.
   - **Diagnostics**: `/proc/b53_timer` shows:
     - per-CPU counts: `clamped`, `episodes` (runs of consecutive clamped reads), `checked`, `rejected`, `bigheld`, `unstable`, `refchecks`;
     - the current `compensation`, reference state and rate, `rearms`, and `fixes` (a ring of the last 16 corrections);
     - a log2 histogram of clamp sizes and the clamp-episode samples.

     Writing to it resets the statistics. `echo "inject <ticks>" > /proc/b53_timer` fakes a counter view offset for testing; reads and every CPU's comparator see it, like the real fault.
   - Per-CPU state with no locks on the fast path. The fast path adds a compare and a store to each read; the read path makes no function calls (safe for `notrace` / `sched_clock`).

2. **Timer programming in the raw view (`drivers/clocksource/arm_arch_timer.c`)**:
   The hardware comparator fires when the **raw** (possibly offset) counter view reaches CVAL. `b53_set_next_event` therefore programs `CVAL = filtered time − b53_comp + delta`, so a timer fires on time even during an offset. Whenever `b53_comp` changes, every CPU re-programs its next event through `irq_work`. Kernels #14–#20 used `CVAL = filtered counter + delta`, which is late by the offset while an offset lasts; with small offsets that error was ≤ 194 µs, but a large offset would delay every timer by its full size.

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
- **Kernel #19 (+ clamp-size histogram)**: `b53_timer_test` 185.3 M reads with **0 glitches**; `rejected`, `resynced` and `unstable` all 0. Glitch sizes are documented in section 1.
- **Kernel #20 (+ clamp-episode samples, full histogram)**: the samples showed that bad values are torn carries and that the offset persists (section 1). That means a large offset would stall timers, which the filter alone cannot prevent.
- **Kernel #21 (+ reference timer, compensation, heartbeat, raw-view CVAL)**: first version of the reference check. It kept correcting all the time. Measuring the reference against the build host's NTP clock showed the reference is accurate and the arch view gains ~300 ppm, so #21 was chasing the drift.
- **Kernel #22 (+ bracketed reference reads)**: removed the error from reference read stalls; the constant corrections remained, which confirmed they came from the drift.
- **Kernel #23 (correct only ≥ 1 ms offsets, current)**: tolerance raised to 1 ms, a backward step the reference does not confirm is held (`bigheld`), and the early-boot fallback can no longer go backwards. Idle after boot: `fixes` 0, `compensation` 0. **Injection tests** (Oct 6 2026, `b53_timer_test` on all 4 cores plus a verifier comparing `CLOCK_MONOTONIC` and 1 ms sleeps against the reference):

  | Injected offset | Backward reads | Detected / undone (error, ticks) | Step in monotonic time | Worst sleep |
  |---|---|---|---|---|
  | none | 0 / 261 M | — | — | 5.1 ms |
  | −5 ms | 0 / 313 M | −400,224 / +399,951 | none | 9.8 ms |
  | −2 ms | 0 / 312 M | −160,484 / +159,912 | none | 4.2 ms |
  | +5 ms | 0 / 315 M | +399,817 / −400,280 | none | 10.1 ms |
  | −60 s (held 70 s) | 0 / 1.4 G | −4,800,000,136 / +4,799,999,819 | none | 9.9 ms |

  Every offset was caught on the next read and corrected to within ~6 µs, and timers kept firing through the 60 s offset. The ~10 ms worst sleeps also occur with nothing injected (CPU contention from the 4-core read test). Each catch/undo pair leaves a few hundred ticks in `compensation` (reference read noise; 1,625 ticks ≈ 20 µs after 4 pairs).

  **More injection tests (Oct 6 2026):**
  - forward +2^31 (26.8 s) and +2^32 (53.7 s), each held 60 s: corrected to within ~100 ticks, no RCU stall or lockup;
  - a sweep of ±2^33 … ±2^53 (`k23_sweep.sh`), 84 cases under 4-core load and idle: all corrected and undone, 0 backward reads in 3.1 G reads.

  These tests do not cover two paths:
  - **Heartbeat-only detection:** even with the router idle, every jump was caught by the next counter read; testing this needs a kernel test knob.
  - **The first ~3 s after boot**, before the reference timer starts.

  **Status: soak test started Oct 6 2026.** In the first 8 hours #23 corrected 65 real jumps (section 1), including +3.34 s and four −13 ms, with no backward time and no reboot. Treat it as proven only after more than 7 days of uptime (well past #14's 30–78 h failure window).

### 5. Key File Locations & Source Code Map

#### A. Kernel source (under [`release/src-rt-5.04behnd.4916/kernel/linux-4.19/`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/))
- **Counter filter, reference check, CVAL timer programming, `/proc/b53_timer`**:
  [`drivers/clocksource/arm_arch_timer.c`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/drivers/clocksource/arm_arch_timer.c) — `b53_read_counter`, `b53_read_pair`, `b53_ref_sample` / `b53_ref_expect` / `b53_comp_fix` (reference check and compensation), `b53_heartbeat`, `b53_ref_init` (allocates the two peripheral timers via `ext_timer_alloc`), `b53_set_next_event` / `b53_rearm_fn`, `b53_read_cntpct_el0` / `b53_read_cntvct_el0`, and the `ool_workarounds[]` entry.
- **Erratum capability & CPU match**:
  [`arch/arm64/kernel/cpu_errata.c`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/arch/arm64/kernel/cpu_errata.c), [`arch/arm64/include/asm/cpucaps.h`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/arch/arm64/include/asm/cpucaps.h), [`arch/arm64/include/asm/cputype.h`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/arch/arm64/include/asm/cputype.h) — `ARM64_WORKAROUND_B53_TIMER`, `MIDR_BRAHMA_B53`. ([`arch/arm64/kernel/cpufeature.c`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/arch/arm64/kernel/cpufeature.c) adds the B53 to the KPTI safe list.)
- **Scheduler clock guard**:
  [`kernel/time/sched_clock.c`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/kernel/time/sched_clock.c) — negative-delta clamp and epoch hold.
- **Timekeeping validation**:
  [`arch/arm64/Kconfig`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/arch/arm64/Kconfig) — selects `CONFIG_CLOCKSOURCE_VALIDATE_LAST_CYCLE`.
- **Kernel config for this platform**:
  [`config_base.6a.6765`](release/src-rt-5.04behnd.4916/kernel/linux-4.19/config_base.6a.6765) — watchdog pretimeout panic governor and `CONFIG_PRINTK_TIME`. (The panic governor itself is the stock `drivers/watchdog/pretimeout_panic.c`, only enabled here.)

#### B. Validation & test tools ([`tools/b53/`](tools/b53/))
- [`b53_bench.c`](tools/b53/b53_bench.c) — 7-test benchmark; deployed on the router as `/jffs/b53_bench`.
- [`test_b53_bidir.c`](tools/b53/test_b53_bidir.c) — backward/forward glitch logger (the forward column counts preemption gaps; see section 1).
- `b53_timer_test` — multi-core glitch detector; deployed as `/jffs/b53_timer_test` (binary only, on the build host at `/home/glory/merlin/b53_timer_test`).
- [`b53_verify.c`](tools/b53/b53_verify.c) — compares `CLOCK_MONOTONIC` and 1 ms sleeps against the peripheral reference timer (via `/dev/mem`); usage `b53_verify <ref_timer> <secs>`.
- [`k21_test.sh`](tools/b53/k21_test.sh) — the #23 injection test (table in section 4).
- [`k23_fwd_test.sh`](tools/b53/k23_fwd_test.sh) — forward injections of +2^31 and +2^32, each held 60 s.
- [`k23_sweep.sh`](tools/b53/k23_sweep.sh) — injection sweep of ±2^33 … ±2^53, matching corrections by size.
- [`kmod/b53_jump.c`](tools/b53/kmod/b53_jump.c) — loadable module that captures the raw counter bits around each jump (section 1); `insmod b53_jump.ko secs=3600 cpu=2 ref=2`, results in dmesg.
- [`kmod/b53_ref.c`](tools/b53/kmod/b53_ref.c) — loadable module that times arch offsets (start to end) against a peripheral reference timer; `insmod b53_ref.ko secs=60 cpu=2`, results in dmesg, unloads itself.
- Build the modules with `make -C tools/b53/kmod` as user `glory` after sourcing `setup_env.sh`. Test logs (`k23_*.log`, `b53_jump_run.log`) stay on the build host in `/home/glory/b53_exp/`.

#### C. Build & flashing tools (local build host, not in this repo)
- `/home/glory/merlin/setup_env.sh` — toolchain paths and the `build_be92u` helper (build as user `glory`, never root).
- `/home/glory/upload_firmware.py` — Asuswrt Login v2 Web UI firmware flasher.
- `/home/glory/check_router_time.sh` — hourly cron check for router reboots, sending ntfy alerts; logs `/proc/b53_timer` to `~/router_b53_timer.log` and alerts only on non-zero `bigheld` / `unstable` (`rejected` and a growing `fixes` count are the filter working and are only logged).

#### D. Router startup scripts (persistent in `/jffs/scripts/`)
- `/jffs/scripts/init-start` — routes the watchdog IRQ 41 and the B53 heartbeat timer IRQ (found from `/proc/b53_timer`, IRQ 38 on this unit) to CPUs 1–3 (`smp_affinity: e`), so neither depends on CPU 0; enables `printk.time=Y`.
- `/jffs/scripts/services-start` — sets `sched_rt_runtime_us = -1` and runs `wdtd` at `nice -20`.
