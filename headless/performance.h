// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include "common/cpu_features.h"

namespace Eden::Performance {
void RegisterWorker(const char* name);
// Development: keep other named threads off guest cores 0-2 and their SMT siblings.
void SetSecondaryPlacement(bool enabled);
// Which logical slots are already dedicated to guest CPU/GPU workers.
// Zero means topology is unproven; shader scheduling must reserve
// conservatively instead of assuming that an affinity mask is secondary.
std::uint64_t PinnedWorkerMask() noexcept;
// Actual kernel-verified logical-secondary affinity inherited by shader
// compiler threads. This is NOT proof of distinct physical/SMT cores.
std::uint64_t VerifiedSecondaryPlacementMask() noexcept;
// Explicit PS5 diagnostic A/B: spread workers across distinct OS logical CPUs,
// without claiming separate physical cores when firmware CPUID is unreliable.
void EnableExperimentalLogicalPlacement();
void PlatformChecks();
// One kernel query at guest start, not in the per-frame performance path.
bool QueryLargestDirectMemoryBlock(std::size_t* largest) noexcept;
// A PS5 app hosts many titles in one process. After all renderer workers
// have joined, invalidate kernel free-block observations from the old title.
// This does NOT free direct memory or change its kernel-exposed extent.
void ResetDirectMemoryProbeForTitle() noexcept;
// Startup/shutdown-only snapshot: no direct-memory region scan in the frame loop.
void ReportDirectMemoryState(const char* phase);
// The 8192-call native kernel region walk is expensive and diagnostic-only.
// Opt in with memory-region-scan.txt; always keep cheap owner/largest-free data.
void SetDirectMemoryRegionScanEnabled(bool enabled) noexcept;
// Lightweight per-stage actual Dynarmic RX/RW ownership; no kernel VA
// enumeration or GPU frame-loop work.
void ReportJitCodeState(const char* phase);
// Serialized main-thread capture or five-second GPU diagnostic snapshot.
// Costly PC processing must never hold the guest CPU worker publication mutex.
void Snapshot();
// GPU worker only: firmware rejects cross-thread CPU-time sampling.
void SampleGpuFrame(unsigned frame);
#ifdef EDEN_DEV_PROFILE
void BeginPcSampling();
void PollGpuPc();
// Development: A64 block compilation split into translate, optimize, emit and the emitter's
// block-range registration (part of emit), per guest core, in nanoseconds.
inline std::array<std::array<std::atomic<unsigned long long>, 4>, 4> jit_phase_ns{};
// Development: A64 blocks per guest core that no core had compiled before, that this core had
// compiled before (after an invalidation), and that another core compiled first, by how long
// before: under 1 ms, 10 ms, 100 ms, 1 s, 10 s, and later. Counted only with dev-settings
// jit_dups=on (a process-wide lock and map insert per compiled block).
inline std::array<std::array<std::atomic<unsigned long long>, 8>, 4> jit_duplicates{};
inline std::atomic<bool> jit_duplicate_tracking{false};
// Guest core whose host PCs the development sampler collects (dev-settings pc_core=N, default 0).
inline std::atomic<unsigned> pc_sample_core{0};
// Development: sample that core at ~500 Hz from its registration (dev-settings pc_fast=on).
inline std::atomic<bool> pc_fast{false};
// Development: honour capture-once.txt from a session's first 30 s segment instead of its fifth
// (dev-settings capture=early; the runner's lifecycle returns need a capture within ~80 s).
inline std::atomic<bool> capture_early{false};
// Development: keep Eden's default library applets, which start some from the firmware
// (dev-settings applets=firmware). The port otherwise uses the built-in ones (main.cpp).
inline std::atomic<bool> firmware_applets{false};
#endif
// One writer per guest core. JIT state is read only by its owning worker after Run.
enum class CpuPhase : unsigned { Kernel, Guest, Idle };
struct alignas(64) CpuState { std::atomic<CpuPhase> phase{}; };
inline std::array<CpuState, 4> cpu_state{};
void SampleCpu(unsigned core, unsigned long long thread, unsigned long long pc, unsigned svc, unsigned fpcr);
struct alignas(64) Totals {
    std::atomic<unsigned long long> calls{}, nanoseconds{}, requested_bytes{};
};
inline std::array<Totals, 4> compilation;
inline std::array<std::atomic<unsigned long long>, 4> evacuations{};
inline Totals storage;
inline Totals jit_protection;
inline Totals rasterizer_draw;
inline Totals gpu_queue_wait, gpu_dispatch;
// Producer-side waits outside the dispatch timer: forced fence drains on the GPU
// thread, free presentation-frame waits, and guest pushes into a full GPU queue.
inline Totals gpu_fence_drain, gpu_present_wait, gpu_queue_full;
// Guest threads entering the GPU caches: CPU writes to tracked pages and flush-area lookups.
inline Totals guest_cpu_write, guest_cpu_read;
// Those entries take the buffer and texture cache locks, which the GPU thread holds for one draw at
// a time. Shipping builds block exactly as upstream: the earlier bounded spin reduced wake latency
// but remains a development-only experiment until it is requalified against PS5 soft hangs.
// dev-settings cache_spin=N can still opt into bounded try_lock retries for profiling.
inline std::atomic<unsigned> cache_lock_spins{0};
inline std::atomic<bool> detailed_gpu_profile{false};
inline std::atomic<unsigned long long> cache_lock_contended{0}, cache_lock_blocked{0};
template <typename Mutex>
inline void GuestCacheLock(Mutex& mutex) {
    // Source of thousands of contended-cache atomic writes in FC27. When
    // diagnostics are OFF use the exact upstream blocking mutex path; do not
    // try_lock first or hammer two shared accounting cache lines per miss.
    // This changes instrumentation, NOT GPU/guest lock ownership or order.
    if (!detailed_gpu_profile.load(std::memory_order_relaxed)) {
        mutex.lock();
        return;
    }
    if (mutex.try_lock()) return;
    cache_lock_contended.fetch_add(1, std::memory_order_relaxed);
    for (unsigned tries = cache_lock_spins.load(std::memory_order_relaxed); tries != 0; --tries) {
        for (int pause = 0; pause < 4; ++pause) __builtin_ia32_pause();
        if (mutex.try_lock()) return;
    }
    cache_lock_blocked.fetch_add(1, std::memory_order_relaxed);
    mutex.lock();
}
// Guest waits: nvhost_ctrl syncpoint event registration -> signal, and
// BufferQueueProducer::DequeueBuffer waiting for a free buffer slot.
inline Totals guest_sync_wait, guest_dequeue_wait;
// fsp-srv IFile/IStorage reads (guest asset streaming): time in the backend and bytes.
inline Totals guest_fs_file, guest_fs_storage;
inline std::atomic<unsigned long long> guest_fs_file_bytes{}, guest_fs_storage_bytes{};
// Guest svcSendSyncRequest latency (queueing + HLE handling), and HLE handling time per
// service command (RecordHle, reported as EDEN_DEV_HLE).
inline Totals guest_ipc_wait;
void RecordHle(const char* service, unsigned command, long long ns);
inline long long NowNs() { return Common::g_wall_clock.GetTimeNS().count(); }
// Texture cache garbage collection (tools/prepare-vulkan-port.py, vulkan_gc_downloads.inc).
// Past its "expected" memory use Eden's collector also evicts images the GPU wrote, and each of
// those is first copied back to guest memory behind a wait for the GPU. On the console a large
// open-world game sat just past that mark (4.8 GiB against 4.5 with the FSR filter's images):
// entering gameplay took ten seconds at 3-9 FPS (15 collector runs took 4.6 s of one 5 s window)
// and frames of 70-115 ms kept coming. Those images are now kept, as they are below the mark,
// until memory is really short: use reaches the "critical" mark, or the largest free block of
// direct memory (what the next allocation needs) is under kShortMemory.
// dev-settings gc_dirty=upstream restores Eden's rule.
inline std::atomic<bool> gc_keep_dirty{true};
inline std::atomic<bool> graphics_memory_short{false};
inline constexpr std::size_t kShortMemory = std::size_t{384} << 20;
// Development: the texture cache reports its memory use and marks every 300 frames.
inline std::atomic<bool> texture_budget_log{false};
// Native DEV: expensive GPU-thread/HLE/CPU-snapshot reporting is opt-in.
// The lightweight frame-pacing counters still run in quiet gameplay.
// Reset per title, rather than inheriting a prior game's diagnostic session.

// "Is memory short?", installed by the PS5 build (performance.cpp, GraphicsMemoryShort). This
// header is compiled into libraries with and without PS5_NATIVE, so the function below must read
// the same in all of them: the platform part is behind this pointer, not behind an #ifdef.
inline std::atomic<bool (*)()> graphics_memory_probe{nullptr};
// Graphics memory still free: the largest block of the direct memory pool, which is what the
// driver's next allocation can be given. Installed by the PS5 build (performance.cpp).
//
// The texture and buffer caches decide what to evict from "memory in use" against a budget. On
// the console that figure is the budget less this free block (tools/prepare-vulkan-port.py,
// Device::GetDeviceMemoryUsage), not the driver's own count, for two reasons. The CPU and the
// GPU share one pool, so what limits graphics is what is left of it, whoever took the rest; and
// the driver reports its allocations twice over (as VRAM and as visible VRAM), which made the
// caches evict at half the use they were set to. dev-settings graphics_usage=driver restores
// the driver's count.
inline std::atomic<unsigned long long (*)()> graphics_memory_free{nullptr};
inline std::atomic<bool> graphics_usage_from_pool{true};
// GPU thread only (the collector).
inline bool KeepDirtyTextures() {
    if (!gc_keep_dirty.load(std::memory_order_relaxed)) return false;
    const auto probe = graphics_memory_probe.load(std::memory_order_relaxed);
    return !probe || !probe();
}
// Development boot trace (dev-settings boot_trace=START:END, milliseconds after the settings
// are read): guest GPU submissions and GPU-thread dispatches inside it are logged one by one.
inline std::atomic<long long> boot_trace_begin{0}, boot_trace_end{0};
// dev-settings fs_callers=on: log the guest caller of each IFileSystem request (its backtrace
// walk delays that request by ~0.1 s, so it is off unless asked for).
inline std::atomic<bool> trace_fs_callers{false};
// Maxwell render-enable evaluations (Maxwell3D::ProcessQueryCondition): [0] host conditional
// rendering, [1]/[2] always/never overrides, [3 + mode * 2 + result] per render_enable mode
// (False, True, Conditional, IfEqual, IfNotEqual) and the CPU-evaluated result. With
// VK_EXT_conditional_rendering, the host path's decisions (tools/prepare-vulkan-port.py): [13] CPU
// evaluation, [14] drawn unconditionally, [15] predicate read from one value, [16] compute
// compare, [17]/[18] hcr=cpu constant predicate draw/skip, [19] hcr=exact pending-query fallback.
inline std::array<std::atomic<unsigned long long>, 20> render_conditions{};
// Guest core idle waits (PhysicalCore::Idle): calls, nanoseconds until the
// interrupt, and calls that went to sleep because nothing arrived while spinning.
struct IdleCounters {
    std::atomic<unsigned long long> calls{0}, nanoseconds{0}, sleeps{0};
};
inline std::array<IdleCounters, 4> guest_idle{};
// PAUSE iterations a guest core spins on its interrupt flag before sleeping (~20 ns each on the
// console). Guest job systems hand work between cores ~150 times a frame; sleeping on each
// handoff put the host's thread wake-up latency on the critical path. 100 us (the default) took
// a racing game's heavy phase from ~55 to ~59.6 FPS; dev-settings
// idle_spin_us=N overrides it (0 sleeps at once).
inline std::atomic<unsigned> idle_spin_iterations{5000};
// Draws between the Vulkan rasterizer's hand-offs to its worker, minus one (a power of two minus
// one; tools/prepare-vulkan-port.py). Shipping uses Eden's upstream 8-draw cadence again;
// dev-settings dispatch_draws=N (8 to 512) can still profile wider batching explicitly.
inline std::atomic<unsigned> dispatch_mask{7};
inline void CountIdle(std::size_t core, long long nanoseconds, bool slept) {
    if (core >= guest_idle.size()) return;
    guest_idle[core].calls.fetch_add(1, std::memory_order_relaxed);
    guest_idle[core].nanoseconds.fetch_add(static_cast<unsigned long long>(nanoseconds), std::memory_order_relaxed);
    if (slept) guest_idle[core].sleeps.fetch_add(1, std::memory_order_relaxed);
}
// Development: reads 32-bit guest words for code dumps (installed by the frontend per session).
inline bool (*guest_read32)(unsigned long long address, unsigned& value) = nullptr;
inline void CountCondition(unsigned slot) {
    if (slot < render_conditions.size()) render_conditions[slot].fetch_add(1, std::memory_order_relaxed);
}
inline bool BootTrace() {
    const auto end = boot_trace_end.load(std::memory_order_relaxed);
    if (end == 0) return false;
    const auto now = NowNs();
    return now >= boot_trace_begin.load(std::memory_order_relaxed) && now < end;
}
inline void AddSince(Totals& totals, long long start) {
    totals.nanoseconds.fetch_add(static_cast<unsigned long long>(NowNs() - start), std::memory_order_relaxed);
    totals.calls.fetch_add(1, std::memory_order_relaxed);
}
// A32/A64 memory accesses that left JIT code through the C++ callback (tracked,
// unmapped or misaligned pages, exclusive stores). Written only by the owning core.
struct alignas(64) JitCallbacks {
    std::atomic<unsigned long long> reads{}, writes{}, exclusive_writes{};
};
inline std::array<JitCallbacks, 4> jit_callbacks{};
inline void CountJit(std::atomic<unsigned long long>& counter) {
    counter.fetch_add(1, std::memory_order_relaxed);
}
// GPU thread only (Vulkan frame report): cumulative GPU-thread idle/dispatch/wait
// totals with the owner's CPU clock, then a guest-core CPU snapshot.
void ReportGpuThread(unsigned frame);
inline std::atomic<unsigned> capture_passes{};

class Timer {
public:
    explicit Timer(Totals& target, unsigned long long bytes = 0)
        : totals(target), requested_bytes(bytes), start(Common::g_wall_clock.GetTimeNS()) {}
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    ~Timer() {
        const auto elapsed = (Common::g_wall_clock.GetTimeNS() - start).count();
        totals.nanoseconds.fetch_add(static_cast<unsigned long long>(elapsed), std::memory_order_relaxed);
        if (requested_bytes) totals.requested_bytes.fetch_add(requested_bytes, std::memory_order_relaxed);
        totals.calls.fetch_add(1, std::memory_order_relaxed);
    }
private:
    Totals& totals;
    unsigned long long requested_bytes;
    std::chrono::nanoseconds start;
};

// GPU/guest waits can execute many thousands of times per second. A normal
// game must never sample two wall clocks or update global Totals for each
// dispatch/lock solely because the binary was compiled as EDEN_DEV_PROFILE.
// Expensive per-command timing is only active with Detailed Logging.
class DiagnosticTimer {
public:
    explicit DiagnosticTimer(Totals& target) noexcept
        : totals(detailed_gpu_profile.load(std::memory_order_relaxed) ? &target : nullptr) {
        if (totals) start = Common::g_wall_clock.GetTimeNS();
    }
    DiagnosticTimer(const DiagnosticTimer&) = delete;
    DiagnosticTimer& operator=(const DiagnosticTimer&) = delete;
    ~DiagnosticTimer() {
        if (!totals) return;
        const auto elapsed = (Common::g_wall_clock.GetTimeNS() - start).count();
        totals->nanoseconds.fetch_add(static_cast<unsigned long long>(elapsed),
                                      std::memory_order_relaxed);
        totals->calls.fetch_add(1, std::memory_order_relaxed);
    }
private:
    Totals* totals{};
    std::chrono::nanoseconds start{};
};

// Opt-in API wall times: calls may overlap across threads, so do not sum them as frame time.
inline std::atomic<bool> vulkan_cost_enabled{false};
inline std::array<Totals, 24> vulkan_api;
inline std::optional<Timer> VulkanTimer(unsigned index) {
    if (vulkan_cost_enabled.load(std::memory_order_relaxed))
        return std::optional<Timer>{std::in_place, vulkan_api.at(index)};
    return std::nullopt;
}
inline void ReportVulkan() {
    if (!vulkan_cost_enabled.load(std::memory_order_relaxed)) return;
    constexpr const char* names[]{"graphics_pipeline", "compute_pipeline", "submit",
                                  "fence_wait", "semaphore_wait", "device_idle",
                                  "guest_fence", "buffer_sync", "texture_sync", "present_sync",
                                  "descriptor_sync", "range_reuse", "astc_submit",
                                  "texture_gc", "texture_cpu_download", "texture_async_release",
                                  "pipeline_ready_wait", "shader_prepare", "pipeline_cache_save",
                                  "shader_pool_reset", "shader_cfg", "shader_ir",
                                  "shader_spirv", "shader_module"};
    static_assert(std::size(names) == vulkan_api.size());
    for (unsigned i = 0; i < vulkan_api.size(); ++i)
        std::printf("EDEN_VULKAN_COST api=%s calls=%llu ns=%llu\n", names[i],
                    vulkan_api[i].calls.load(std::memory_order_relaxed),
                    vulkan_api[i].nanoseconds.load(std::memory_order_relaxed));
}
// Call only before guest startup and after worker shutdown, respectively.
inline void Reset() {
    for (auto& entry : compilation) {
        entry.calls = 0;
        entry.nanoseconds = 0;
        entry.requested_bytes = 0;
    }
    for (auto& count : evacuations) count = 0;
    storage.calls = 0;
    storage.nanoseconds = 0;
    storage.requested_bytes = 0;
    jit_protection.calls = 0;
    jit_protection.nanoseconds = 0;
    jit_protection.requested_bytes = 0;
}
inline void Report() {
    for (unsigned core = 0; core < compilation.size(); ++core) {
        std::printf("EDEN_PERF_JIT core=%u compilations=%llu compile_ns=%llu evacuations=%llu\n",
                    core, compilation[core].calls.load(), compilation[core].nanoseconds.load(),
                    evacuations[core].load());

    }
    std::printf("EDEN_PERF_STORAGE calls=%llu read_ns=%llu requested_bytes=%llu\n",
                storage.calls.load(), storage.nanoseconds.load(), storage.requested_bytes.load());
    std::printf("EDEN_PERF_JIT_PROTECTION calls=%llu elapsed_ns=%llu requested_bytes=%llu\n",
                jit_protection.calls.load(), jit_protection.nanoseconds.load(), jit_protection.requested_bytes.load());
}
}
