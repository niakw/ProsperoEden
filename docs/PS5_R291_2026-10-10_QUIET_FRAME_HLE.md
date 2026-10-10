# Eden Encore R291 — FC27 / BOTW: quiet-frame and HLE hot-path pass (10 October 2026)

## Scope and status

Working branch: `dev/ps5-sparse-jit`. This pass changes native DEV instrumentation
and how quiet gameplay records frame pressure. It does **not** change approved
launcher visuals, CPU accuracy, GPU accuracy, JIT size, memory safety thresholds,
Vulkan presentation scheduling, shaders or game assets.

**Source changes do not prove faster hardware frame pacing.** Compile a new PS5
PKG and compare the same scenes before/after; retain the old build for rollback.

## Evidence from the 10 October field logs

- FC27's 30 FPS plateau hides long present intervals and bursts of drops.
- In the third FC27 session, two sub-23 FPS five-second windows near rendered
  frame 13064 and 16024 recorded ~12,628 and ~14,863 new cache lock
  contentions. The GPU owner accumulated ~3.65 / ~3.33 **seconds of queue
  idle** in those windows, while dispatch accumulated ~1.39 / ~1.69 seconds.
- In those windows JIT compilation elapsed only a few milliseconds. JIT cannot
  independently explain every heavy window, although other windows show
  significant JIT work.
- GPU thread queue-idle time is **not GPU hardware utilization**. Do not
  infer GPU hardware occupancy without timestamps and firmware-qualified
  GPU timing.
- `EDEN_DEV_HLE` contributed nearly 12,000 lines to the supplied combined
  `heap(9).log`; prior DEV dispatcher instrumentation executed two wall-clock
  probes and updated a service/command counter on every HLE request, regardless
  of whether the player enabled detailed logging.

## Implemented in this pass

1. Per-title `Eden::Performance::detailed_gpu_profile` defaults to **false**.
   Deep tracing is enabled when Detailed Logging is on or `frame-profile.txt`
   exists in the app data directory. `performance-run.txt` overrides both
   and forces quiet profiling.
2. The GPU-thread `ReportGpuThread` call (HLE hash table traversal, large
   counters, CPU snapshots and associated output) now runs only with that
   opt-in. `EDEN_VULKAN_FRAME` and per-game pacing bins still run normally.
3. The pinned CMake HLE dispatcher instrumentation now samples the atomic
   profile flag. When disabled, **neither HLE wall-clock probe nor
   `RecordHle` is executed**; normal and TIPC service dispatch still execute
   unchanged. `EDEN_DEV_HLE` measurements require explicit diagnostics.
4. OpenGL's automatic 150s `capture_passes` is also diagnostic-only, and its
   timer/state now belong to the graphics session rather than a static shared
   across games.
5. Lightweight `EDEN_FRAME_PRESSURE` line every five seconds, aligned with
   the existing `EDEN_VULKAN_FRAME` sample:
   - `gpu_idle_ms`: time GPU owner waited for commands (not physical GPU idle)
   - `gpu_dispatch_ms`, `gpu_fence_ms`, `gpu_present_ms`, `gpu_full_ms`
   - `guest_dequeue_ms`, `guest_sync_ms`, `guest_ipc_ms`
   - `cache_contended`, `cache_blocked`, `jit_ms`.
   
   These are per-window **counter deltas**, not a decomposition of one frame
   into nonoverlapping stages: concurrent threads can overlap and must not be
   summed as frame time. A negative/resetted counter marks the window invalid.
   Baselines belong to each new `GraphicsWindow` instance.
6. Does **not** disable `PS5VK_CAPTURE_SCANOUT`: its relationship to real
   PS5 presentation needs proof before modifying it.
7. Existing sparse JIT, conservative physical-memory policy and guest-network
   isolation remain unchanged.

## Validation protocol

1. Run the host-only core preflight and confirm the HLE CMake patch
   transformation/source guard. This cannot prove PS5 native compilation.
2. Compile a new development PS5 binary from **this exact commit** and retain
   a known-good binary to roll back. Do not interpret a green preflight alone
   as a working PKG.
3. In FC27, keep one fixed menu camera/match scenario, resolution, settings
   and shader cache; compare 5–10 minute quiet sessions. Record mean FPS,
   distribution of `late38`, `late50`, `late100`, worst present interval,
   and the new `EDEN_FRAME_PRESSURE` counters. Repeat BOTW Ultra with the
   same scene and conditions.
4. If quiet is still poor, run a **separate** short profile with
   `frame-profile.txt` and Detailed Logging off. This captures the deep
   CPU/GPU/HLE breakdown without enabling the verbose PS5 RADV driver logs.
   This profile deliberately adds overhead, so do not compare its raw FPS
   directly to quiet runs.
5. Compare `gpu_dispatch_ms` and `gpu_idle_ms` with lock contention,
   HLE/IPC and JIT; do not assert a GPU bottleneck from a CPU-side timer.
6. Once a full PS5 title session and an FPS A/B show improvement, qualify
   the change; otherwise investigate actual guest producer stalls, the HLE
   binder/fence queue and physical GPU timestamps next.

## Outstanding

The persistent FC27 and BOTW stutter is **not yet fixed or causally attributed
to a single subsystem**. PS5 GPU utilization, HLE wait causal chain,
correctness of the compiler-generated HLE changes on console, and the
user-observed frame pacing all await hardware validation.
