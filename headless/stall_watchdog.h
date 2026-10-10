// SPDX-License-Identifier: GPL-3.0-or-later
// Development builds: where does a stalled game boot stop? The boot writes trace points to the
// kernel log (EDEN_BOOT), and while armed (game boot, up to the guest's start) a watchdog reports
// every 5 s once the boot has made no progress for 15 s (EDEN_STALL): the last stage and the heap
// arena creation lock's holder and waiters (headless/heap_arenas.inc). Kernel log only: stdout is
// block-buffered, and signals sent to threads during a boot hung or crashed it.
#pragma once
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include "diagnostics.h"
#include "performance.h"
#include "crash_report.h"
#include "game_liveness.h"

extern "C" unsigned long eden_heap_create_lock_state(unsigned* waiters);

namespace Eden::Stall {
inline std::atomic<unsigned> progress{0};
inline std::atomic<bool> armed{false};
inline std::atomic<bool> game_armed{false};
inline std::atomic<unsigned> game_session_epoch{0};
inline std::atomic<const char*> stage{"none"};

inline void Print(const char* line) {
    // Developer progress messages belong to Detailed Logging only.
    // Quiet sessions keep useful fault breadcrumbs in Crash atomics, not klog.
    if (!Performance::detailed_gpu_profile.load(std::memory_order_relaxed)) return;
#if defined(__PROSPERO__)
    (void)sceKernelDebugOutText(0, line);
#else
    std::fputs(line, stderr);
#endif
}

inline void Trace(const char* point) {
    char line[160];
    std::snprintf(line, sizeof(line), "EDEN_BOOT %s\n", point);
    Print(line);
}

// Every thread that names itself (the SetCurrentThreadName hook), first thing.
inline void NoteThread(const char* name) {
    if (!std::strcmp(name, "GPU")) Trace("GPU thread named");
}

inline void Progress(const char* name) {
    stage.store(name, std::memory_order_relaxed);
    progress.fetch_add(1, std::memory_order_relaxed);
}
inline void Tick() { progress.fetch_add(1, std::memory_order_relaxed); }

inline void Loop() {
    unsigned last = progress.load(std::memory_order_relaxed);
    auto changed = std::chrono::steady_clock::now();
    unsigned reports = 0;
    GameLiveness::Probe game_probe;
    bool previous_game_active = false;
    unsigned previous_game_epoch = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const unsigned now_progress = progress.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        // The boot watchdog used to stop at 'main running', leaving long
        // FC27 gameplay freezes wholly unobserved. Observe existing bounded
        // GPU operation counters on THIS 1 Hz developer thread only.
        const bool game_active = game_armed.load(std::memory_order_acquire);
        if (game_active) {
            // Two title sessions may end and re-arm between 1 Hz probes.
            // Boolean-only state would re-use the prior title's counts,
            // spuriously reporting that the next game froze.
            const unsigned epoch = game_session_epoch.load(std::memory_order_acquire);
            if (!previous_game_active || epoch != previous_game_epoch)
                game_probe.Reset();
            previous_game_epoch = epoch;
            previous_game_active = true;
            const auto wall_second = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    now.time_since_epoch()).count());
            const GameLiveness::Counters counters{
                // One relaxed write per 64 completed GPU commands, even when
                // R293 disables costly per-command diagnostics.
                Crash::gpu_completed_commands.load(std::memory_order_relaxed),
                Performance::detailed_gpu_profile.load(std::memory_order_relaxed) ?
                    Performance::rasterizer_draw.calls.load(std::memory_order_relaxed) : 0ull};
            const auto observation = game_probe.Observe(counters, wall_second);
            if (observation.suspected) {
                Crash::gpu_stall_suspicions.fetch_add(1, std::memory_order_relaxed);
                if (!Performance::detailed_gpu_profile.load(std::memory_order_relaxed))
                    continue;  // only the fatal crash report may persist it.
                char line[340];
                std::snprintf(line, sizeof(line),
                    "EDEN_GAME_GPU_STALL_SUSPECT idle_s=%llu dispatch=%llu draws=%llu "
                    "gpu_queue_full=%llu guest_sync_wait=%llu cpu_phases=%u,%u,%u,%u "
                    "note=diagnostic_only\n",
                    static_cast<unsigned long long>(observation.seconds_without_progress),
                    static_cast<unsigned long long>(counters.dispatches),
                    static_cast<unsigned long long>(counters.draws),
                    static_cast<unsigned long long>(
                        Performance::gpu_queue_full.calls.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        Performance::guest_sync_wait.calls.load(std::memory_order_relaxed)),
                    unsigned(Performance::cpu_state[0].phase.load(std::memory_order_relaxed)),
                    unsigned(Performance::cpu_state[1].phase.load(std::memory_order_relaxed)),
                    unsigned(Performance::cpu_state[2].phase.load(std::memory_order_relaxed)),
                    unsigned(Performance::cpu_state[3].phase.load(std::memory_order_relaxed)));
                Print(line);
            }
        } else {
            previous_game_active = false;
        }
        if (!armed.load(std::memory_order_acquire) || now_progress != last) {
            last = now_progress;
            changed = now;
            reports = 0;
            continue;
        }
        const double seconds = std::chrono::duration<double>(now - changed).count();
        if (reports >= 12 || seconds < 15 + 5.0 * reports) continue;
        unsigned waiters = 0;
        const unsigned long holder = eden_heap_create_lock_state(&waiters);
        char line[256];
        std::snprintf(line, sizeof(line), "EDEN_STALL stage=%s seconds=%.0f heap_create_holder=%lx heap_create_waiters=%u\n",
                      stage.load(std::memory_order_relaxed), seconds, holder, waiters);
        Print(line);
        ++reports;
    }
}

// Main thread, once.
inline void Start() { std::thread(Loop).detach(); }

// A game boot starts / its guest runs.
inline void Arm() {
    Progress("arm");
    armed.store(true, std::memory_order_release);
}
inline void Disarm() { armed.store(false, std::memory_order_release); }
inline void ArmGame() {
    Crash::gpu_stall_suspicions.store(0, std::memory_order_relaxed);
    game_session_epoch.fetch_add(1, std::memory_order_acq_rel);
    game_armed.store(true, std::memory_order_release);
}
inline void DisarmGame() { game_armed.store(false, std::memory_order_release); }
}
#endif
