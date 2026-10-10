# Eden Encore R293 — Quiet CPU/GPU hot paths and FC27 queue analysis

Date: 10 October 2026. Branch: `dev/ps5-sparse-jit`.

## Problem and objective

The user's FC27 runs still have severe present-interval spikes and persistent
microstutter after cache/memory/JIT improvements. R292 made native logs
crash-only when Detailed Logging is OFF, but DEV-profile code still paid
per-command wall clocks and global atomic counters on the critical path.

R293 eliminates this *instrumentation overhead*, **not GPU emulation itself**.
It changes no Vulkan accuracy flags, game texture logic, JIT cache budgets,
GPU queue depth/order, CPU affinity, or approved UI.

## Evidence (10 October test `heap(9).log`)

The report pairs `EDEN_VULKAN_FRAME` with `EDEN_DEV_GPU` per 5s window.
Counters are cumulative and may reset between sessions.

| Session | 5s windows | FPS average | Windows with GPU-full delta | Cumulative positive full-queue wait deltas |
| --- | ---: | ---: | ---: | ---: |
| FC27 1 | 64 | 28.13 | 13 | ~1.98s |
| FC27 2 | 170 | 28.77 | 30 | ~9.80s |
| FC27 3 | 196 | 28.71 | 36 | ~1.81s |
| BOTW Ultra | 16 | 27.45 | 1 | ~0.12s |

**One standout FC27 #2 window:** `fps=5.900`, `worst_ms=5141.190`,
`gpu_full_ns` delta ~4.58s, `gpu_dispatch_ns` delta ~5.10s. The native GPU
producer blocked behind a full queue during this event. However this may be
**a downstream symptom of slow GPU dispatch** (graphics translation, cache
waiting, pipeline compilation or driver scheduling), not a fault in the
queue size itself.

Other FC27 windows show substantial lock contention even when JIT compilation
in the same window is just a few milliseconds. Neither physical GPU
utilization nor the exact cause inside an individual GPU dispatch is measured.
There is therefore **no basis for claiming that increasing queue depth or
shrinking sleep() will cure the 5s stall**.

## Source changes

1. `GuestCacheLock` in `headless/performance.h`: with Detailed Logging OFF,
   use the original blocking mutex acquisition, with no preliminary try_lock,
   diagnostic atomic increments or experimental cache spin. This preserves
   lock ownership and order.
2. New `DiagnosticTimer`: when quiet, it only observes one relaxed mode flag
   and performs no wall-clock probes, counter increments or log formatting.
   Explicit diagnostics retain the existing measured calls and durations.
3. Native GPU worker in `headless/CMakeLists.txt`: queue idle, dispatch and
   producer full waits now use `DiagnosticTimer`. The 8-entry queue, stop
   token and blocking/signal semantics are unmodified.
4. Vulkan source adaptation in `tools/prepare-vulkan-port.py`: cache CPU
   read/write, fence drain and present-frame wait timers are diagnostic-only.
   Per-draw `rasterizer_draw.calls` global atomic increments are also
   diagnostic-only.
5. `tools/check-ps5-quiet-gpu-hotpaths.py`: compiles and runs the actual
   extracted lock and timer code with 8 contending host threads (160,000
   critical sections), verifies quiet counters remain zero, and checks that
   enabled diagnostics still accumulate clocks and lock contention. This is a
   **host test, not PS5-native gameplay validation**.

## Next comparison

Build the PS5 test PKG from the exact post-R293 commit. Use **identical game
settings and unchanged shader cache** across FC27 and BOTW before/after
comparisons. During quiet runs, compare subjective pacing and any native
FPS overlay without enabling detailed logging. For short forensic runs
enable the Detailed Logging UI option; compare slow windows, `gpu_full_ns`,
`gpu_dispatch_ns`, `guest_dequeue_ns`, `cache_lock_blocked`, and JIT
compile deltas.

A genuinely large GPU dispatcher stall will **not** be fixed solely by
eliminating instrumentation. Follow-up work must trace slow GPU submissions
and their shader/texture/driver stages on real hardware. Neither 60fps nor
zero stutter can presently be claimed.
