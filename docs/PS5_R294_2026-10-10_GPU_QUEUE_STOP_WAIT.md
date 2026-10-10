# Eden Encore R294 — GPU back-pressure without polling, crash-only progress

Date: 10 October 2026. Branch: `dev/ps5-sparse-jit`.
This is an experimental native PS5 change awaiting SDK compilation and
on-console A/B, **not** a claim that FC27/BOTW stutter has been eliminated.

## Why this change

FC27 #2 showed a 5-second window at 5.900 FPS with one 5141 ms present
interval and roughly 4575 ms *cumulative* `gpu_full_ns` wait. The
three guest CPU JIT workers simultaneously accumulated ~9935 ms of JIT
compilation CPU time. This demonstrates concurrent work and GPU-producer
back-pressure, but not which produced the other.

Until now the native GPU producer tried `TryEmplace` on the eight-slot
SPSC queue every 100 microseconds while full, calling
`std::this_thread::sleep_for(100us)` each time. The PS5 scheduler is not
obliged to wake it at precisely 100us. Moreover the guest writer kept its
`state.write_lock` across those sleeps. The pinned SPSC queue already
notifies its producer's condition variable on every pop.

## Changes

- Add an event-driven native-only
  `Common::SPSCQueue::EmplaceWaitWithStopToken` in
  `headless/gpu_queue_stop_wait.inc`, derived into the pinned queue header
  during native CMake generation. The first `TryEmplace` remains lock-free
  for an empty/not-full queue.
- If full, the producer uses
  `producer.cv.wait(lock, stop_token, free_slot_predicate)` instead of
  retrying after arbitrary sleeps. A pop wakes the producer; shutdown
  requests wake it even if the consumer never pops again. It must not
  publish a newly queued command after cancellation is observed.
- Keep command order, **queue capacity = 8**, fence counters and the existing
  producer `write_lock` unchanged. The new block/wake path does **not**
  change Vulkan rendering, guest accuracy, shader compilation or memory
  budgets.
- Timing the full wait remains an opt-in R293 `DiagnosticTimer`. No
  periodic logs in quiet gameplay.
- In crash-only mode GPU progress can no longer be inferred from the R293
  per-command development counters (which are deliberately disabled).
  Publish one relaxed GPU `gpu_completed_commands` atomic increment
  per 64 **completed** GPU worker commands, and let the watchdog sample
  that count on its existing 1 Hz thread.
- Suspected freezes are counted in memory only. With Detailed Logging OFF
  the watchdog does not print its own kernel-log messages. If a real
  fatal signal occurs, `crash-*.txt` includes coarse GPU completed-command
  progress and suspected-stall count. These hints do not prove a stall cause.

## Safety gates and limitations

- `tools/check-ps5-gpu-producer-wake.py` uses the exact injected wait
  method in a C++20 fixture. It checks FIFO preservation, full-queue
  cancellation and a 4000-command producer/consumer run.
- Existing native source tests must use the **generated** SPSC queue header,
  not upstream's original header without the stop-aware method.
- The native generated-worker comparison strips only the audited
  instrumentation and the new queue behavior; any additional unexpected
  changes remain test failures.
- An event-driven wait reduces producer-side polling/scheduler wakeups;
  it does **not** make GPU rendering commands faster, eliminate lengthy JIT
  compilation, nor guarantee stable 30/60 FPS.
- A 64-command sampling stride may miss activity under 64 commands in
  a long idle interval; watchdog hints are diagnostic, never a reason to
  kill or auto-restart a paused game.
- The known PS5 CPU topology remains unverified. JIT shared caches and
  successor batching remain disabled pending separate qualified work.
- R293's prior native build on commit `767c043d` passed, but **does not
  contain R294**. Compile from the exact R294 commit before any device test.

## Diagnostic-only lock wait refinement

The old `cache_lock_contended` counter counts failed guest-side cache-lock
acquisitions. It is **not** a duration. With Detailed Logging enabled, R294
now also samples the time spent in the actual blocking `mutex.lock()` call
and reports `cache_lock_wait_ns` (cumulative) and `cache_wait_ms` (the
five-second delta). This distinguishes frequent but cheap contention from
expensive CPU/renderer serialization. The measurement deliberately excludes
optional try-lock spinning and cannot by itself prove GPU hardware occupancy.
With Detailed Logging OFF the mutex uses the original direct lock and never
samples a clock or updates the diagnostic wait counter.

## Native build validation strengthening

Before all-on native compilation, `tools/build-headless-native.sh` now also
runs the real generated-worker lifecycle and full-queue cancellation tests.
The generated SPSC header is used by the cancellation harness, with the
optional per-request DEV timer removed *only inside that fixture*. This
prevents a successful source-only check from masking a broken CMake-derived
GPU worker, and catches a missed stop-token wake before distributing a PKG.

## Native build gate

The complete host regression suite passed on GitHub Actions
[#38085456806](https://github.com/niakw/Prospero.Eden-Encore/actions/runs/38085456806).
A separate PS5 all-on test compilation is required to verify actual CMake
generation, PS5 libc++ stop-token CV support and linker behavior before
any physical console A/B. Do not publish an immutable release from this test.
