// SPDX-License-Identifier: GPL-3.0-or-later
#include "performance.h"
#include "direct_pool_accounting.h"
#include "direct_pool_probe_policy.h"
#include "direct_pool_region_scan.h"
#include "hle_counters.h"
#include "gpu_fault_rate_limit.h"
#include "crash_report.h"
#include "stall_watchdog.h"
#include "../src/fastmem.h"
#include "common/cpu_features.h"
#include "common/sparse_large_vector.h"
#include <algorithm>
#include <string>
#include <set>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cpuid.h>
#include <cstring>
#include <map>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>
#include <latch>
#include <time.h>
#if defined(EDEN_DEV_PROFILE) || defined(PS5_NATIVE)
#include <signal.h>
#endif
#ifdef PS5_NATIVE
#include <sys/param.h>
#include <sys/cpuset.h>
#endif

// dynarmic A64 dispatch-path counters (headless/dynarmic/jit_group_support.inc).
extern "C" void eden_jit_path_counters(unsigned core, unsigned long long* out) __attribute__((weak));
namespace Common {
// Address space Eden's sparse tables span and the memory they hold (src/memory_pages.cpp).
void SparseUsage(std::size_t* reserved, std::size_t* committed) noexcept;
#ifdef PS5_NATIVE
void SparseJitUsage(std::size_t* reserved, std::size_t* committed) noexcept;
void SparseJitUsageFast(std::size_t* reserved, std::size_t* committed) noexcept;
std::size_t DenseJitDirectBytes() noexcept;
#endif
} // namespace Common

namespace Eden::Performance {
namespace {
constexpr std::array names{"CPUCore_0", "CPUCore_1", "CPUCore_2", "CPUCore_3",
                           "GPU", "HostTiming", "VSyncThread"};
struct Worker {
    pthread_t thread{};
    clockid_t clock{};
    int clock_error = ENOENT, affinity_error = ENOENT, allowed = 0;
    unsigned long long mask = 0;
    long long wall_ns = 0, mono_ns = 0;
    bool registered = false;
};
std::mutex workers_mutex;
std::array<Worker, names.size()> workers;
// Periodic native GPU and optional host-thread capture must not concurrently
// consume the same PC sample cursors. Keep this lock DISTINCT from the
// owner-written worker mutex so costly diagnostic maps/hex never stop cores
// from publishing CPU samples or frontends from polling development PCs.
std::mutex snapshot_mutex;
// A GPU/guest worker is normally created again for the next title inside
// the same process. Registration is NOT a permanent OS thread identity:
// clear it on owner-thread exit before a later profiler tries to signal a
// recycled pthread_t. Thread-local declaration is touched before starting
// the optional fast sampler, so the fast sampler's jthread is joined FIRST.
struct WorkerRegistration {
    unsigned index = unsigned(names.size());
    pthread_t owner{};
    ~WorkerRegistration() {
        if (index >= names.size()) return;
        const std::lock_guard lock(workers_mutex);
        auto& entry = workers[index];
        if (entry.registered && pthread_equal(entry.thread, owner))
            entry.registered = false;
    }
};
thread_local WorkerRegistration owned_worker;
#ifdef EDEN_DEV_PROFILE
static_assert(std::atomic<uintptr_t>::is_always_lock_free);
// Lock-free signal-handler samples: use ring slots so the 500 Hz fast
// sampler does not silently stop after 8192 (~16 seconds) GPU samples.
std::array<std::atomic<uintptr_t>, 8192> sampled_pcs{};
std::atomic<unsigned> pc_count{};
// Guest core 0 host PCs (JIT code, HLE, memory callbacks), sampled with the GPU thread.
std::array<std::atomic<uintptr_t>, 65536> sampled_core_pcs{};
std::atomic<unsigned> core_pc_count{};
unsigned core_pc_reported{};
pthread_t core_sample_thread{};
std::atomic<bool> core_sample_ready{};
unsigned pc_reported{};
// Unlike a detached 500 Hz sampler, this thread-local worker registration
// clears readiness at guest-core exit. Do not signal a stale pthread_t after
// return to the library, nor leave detached samplers across title launches.
struct PcCoreRegistration {
    ~PcCoreRegistration() { if (active) core_sample_ready.store(false, std::memory_order_release); }
    bool active = false;
};
thread_local PcCoreRegistration pc_core_registration;
bool pc_sampling{};
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
// Same firmware6.02 register offsets qualified by the C46 caller sampler.
constexpr size_t runtime_rsp_offset = 248, runtime_rbp_offset = 136;
using CallerChain = std::array<uintptr_t, 8>;
std::array<CallerChain, 8192> sampled_callers{};

CallerChain CaptureCallerChain(uintptr_t rsp, uintptr_t frame, uintptr_t handler_stack) {
    CallerChain callers{};
    // Read only aligned frames in the interrupted stack-top page. No allocation,
    // symbolization or logging in the signal handler. Reject unknown/alternate stacks.
    if (rsp <= 0x10000 || rsp < handler_stack || rsp - handler_stack >= 65536 ||
        (rsp & 4095) > 4096 - 8 * sizeof(uintptr_t)) return callers;
    for (unsigned i = 0; i < callers.size(); ++i) {
        if (frame < rsp || (frame >> 12) != (rsp >> 12) ||
            (frame & (alignof(uintptr_t) - 1)) ||
            (frame & 4095) > 4096 - 2 * sizeof(uintptr_t)) break;
        const auto* chain = reinterpret_cast<const uintptr_t*>(frame);
        callers[i] = chain[1];
        if (chain[0] <= frame || chain[0] - frame > 4096) break;
        frame = chain[0];
    }
    return callers;
}
#endif
// Core 0's A32 blocks in emission order (P0): dynarmic's code cache grows linearly until a full
// clear, so entries stay sorted and a sampled host PC finds its guest block by binary search.
struct JitBlock {
    uintptr_t entry;
    unsigned long long size, location;
};
constexpr std::size_t jit_block_capacity = std::size_t{1} << 20;
JitBlock* core0_blocks = nullptr;
std::atomic<std::size_t> core0_block_count{0};

void PcSignal(int, siginfo_t*, void* context) {
    if (core_sample_ready.load(std::memory_order_acquire) &&
        pthread_equal(pthread_self(), core_sample_thread)) {
        const unsigned core_index = core_pc_count.load(std::memory_order_relaxed);
        sampled_core_pcs[core_index % sampled_core_pcs.size()].store(
            static_cast<const uintptr_t*>(context)[224 / sizeof(uintptr_t)],
            std::memory_order_relaxed);
        core_pc_count.store(core_index + 1, std::memory_order_release);
        return;
    }
    const unsigned index = pc_count.load(std::memory_order_relaxed);
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
    // Caller-chain slots contain eight non-atomic words. Keep this special
    // firmware oracle bounded until its lifetime and concurrency are proven:
    // only the normal GPU/core PC-only samplers are cyclic.
    if (index >= sampled_pcs.size()) return;
#endif
    const unsigned slot = index % sampled_pcs.size();
    // Native firmware 6.02 ABI, qualified by the standalone PC-sampling oracle.
    // SDK FreeBSD ucontext.mc_rip points at the wrong field on this firmware.
    sampled_pcs[slot].store(static_cast<const uintptr_t*>(context)[224 / sizeof(uintptr_t)],
                            std::memory_order_relaxed);
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
    const auto* words = static_cast<const uintptr_t*>(context);
    char handler_stack_marker;
    sampled_callers[slot] = CaptureCallerChain(words[runtime_rsp_offset / sizeof(uintptr_t)],
        words[runtime_rbp_offset / sizeof(uintptr_t)], reinterpret_cast<uintptr_t>(&handler_stack_marker));
#endif
    pc_count.store(index + 1, std::memory_order_release);
}
#endif
#ifdef PS5_NATIVE
std::array<unsigned, 5> worker_cpus{};
bool worker_topology_ready = false;
// Published as ONE atomic snapshot only after a physically verified probe.
// Shader-pool setup must never read worker_cpus while the next title is
// rebuilding the mutable topology, nor mistake logical-only placement
// (physical_verified=0) for measured cores.
std::atomic<std::uint64_t> verified_physical_worker_mask{0};
// Only published after SetSecondaryPlacement(true) confirms exact OS
// affinity. An explicit logical-only A/B split can use its own nonoverlap
// evidence without pretending that CPUID physical topology was verified.
std::atomic<std::uint64_t> verified_secondary_placement_mask{0};
// Physical core (x2APIC above the SMT shift) of each allowed CPU, -1 if unknown.
std::array<int, 64> cpu_core = [] { std::array<int, 64> c{}; c.fill(-1); return c; }();
// CPUs outside guest cores 0-2 and their SMT siblings, and not the GPU thread CPU.
unsigned long long secondary_cpus = 0;
// The process CPU set seen by the topology probe (threads may inherit narrower sets).
cpuset_t topology_allowed{};
bool topology_allowed_valid = false;
std::atomic<bool> placement_secondary{false};
std::atomic<unsigned> secondary_reports{};
void CheckWorkerTopology() {
    // A new title/firmware check may fail before CPUID probing begins.
    // Never reuse a previous game's affinity decision in that case.
    verified_physical_worker_mask.store(0, std::memory_order_release);
    verified_secondary_placement_mask.store(0, std::memory_order_release);
    worker_topology_ready = false;
    secondary_cpus = 0;
    worker_cpus.fill(0);
    cpu_core.fill(-1);
    topology_allowed = {};
    topology_allowed_valid = false;
    cpuset_t original{};
    if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &original)) {
        std::printf("EDEN_WORKER_TOPOLOGY ready=0 reason=affinity_unavailable errno=%d\n", errno);
        return;
    }
    // This is the probe thread's permitted set, not the number of hardware
    // threads on the PS5. Shader workers can inherit a narrower affinity.
    std::uint64_t allowed_mask = 0;
    unsigned allowed_count = 0;
    for (unsigned cpu = 0; cpu < 64; ++cpu) {
        if (CPU_ISSET(cpu, &original)) {
            allowed_mask |= std::uint64_t{1} << cpu;
            ++allowed_count;
        }
    }
    std::printf("EDEN_PS5_CPU_ACCESS hardware_reported=%u affinity_allowed=%u allowed_mask=0x%llx scope=probe_thread\n",
                std::thread::hardware_concurrency(), allowed_count,
                static_cast<unsigned long long>(allowed_mask));
    // AMD family 17h+ can expose physical topology via the extended leaf
    // even if x2APIC leaf 0xB is missing/unusable on this firmware.
    unsigned va{}, vb{}, vc{}, vd{};
    __cpuid(0, va, vb, vc, vd);
    const bool amd_vendor = vb == 0x68747541u &&
        vd == 0x69746e65u && vc == 0x444d4163u;
    const bool has_x2apic = __get_cpuid_max(0, nullptr) >= 0xbu;
    bool has_amd_topology = false;
    if (amd_vendor && __get_cpuid_max(0x80000000u, nullptr) >= 0x8000001eu) {
        __cpuid(1, va, vb, vc, vd);
        const unsigned base_family = (va >> 8) & 15u;
        const unsigned family = base_family == 15u ?
            base_family + ((va >> 20) & 255u) : base_family;
        __cpuid(0x80000001u, va, vb, vc, vd);
        has_amd_topology = family >= 0x17u && (vc & (1u << 22)) != 0;
    }
    // Even when CPUID is unusable retain a valid OS affinity mask for
    // the explicit *logical-only* trial; that trial is not physical proof.
    topology_allowed = original;
    topology_allowed_valid = true;
    if (!has_x2apic && !has_amd_topology) {
        std::printf("EDEN_WORKER_TOPOLOGY ready=0 reason=no_supported_physical_cpuid\n");
        return;
    }
    std::array<unsigned, 5> cores{};
    unsigned count = 0;
    bool changed = false;
    for (unsigned cpu = 0; cpu < 64; ++cpu) {
        if (!CPU_ISSET(cpu, &original)) continue;
        cpuset_t one{};
        CPU_SET(cpu, &one);
        if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &one)) break;
        changed = true;
        // Affinity-mask changes are not necessarily reflected by the immediately following
        // instruction. Give the kernel a scheduling point before sampling x2APIC topology;
        // the old probe observed the same physical core for every allowed CPU on hardware.
        sched_yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        unsigned a{}, b{}, c{}, d{};
        // Accept a physical core only when the requested single-CPU mask
        // is observed after rescheduling; setaffinity success is insufficient.
        cpuset_t verified{};
        if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &verified) ||
            std::memcmp(&one, &verified, 8)) break;
        unsigned core = 0;
        bool decoded = false;
        // On AMD family 17h+, use its verified TopologyExtensions first:
        // some PS5 firmware reports an unusable x2APIC SMT/core partition.
        // EBX[7:0] CoreId, EBX[15:8] threads/core - 1,
        // ECX[7:0] NUMA node ID. Never treat sibling logical IDs as cores.
        if (has_amd_topology) {
            __cpuid_count(0x8000001eu, 0, a, b, c, d);
            const unsigned threads_per_core = ((b >> 8) & 255u) + 1u;
            if (threads_per_core <= 8u) {
                core = ((c & 255u) << 8) | (b & 255u);
                decoded = true;
            }
        }
        if (!decoded && has_x2apic) {
            __cpuid_count(0xb, 0, a, b, c, d);
            if (b && ((c >> 8) & 0xffu) == 1u && (a & 31u) < 16u) {
                core = d >> (a & 31u);
                decoded = true;
            }
        }
        if (!decoded) break;
        cpu_core[cpu] = static_cast<int>(core);
        if (count == cores.size() ||
            std::find(cores.begin(), cores.begin() + count, core) != cores.begin() + count) continue;
        cores[count] = core;
        worker_cpus[count++] = cpu;
    }
    if (changed) {
        cpuset_t restored{};
        if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &original) ||
            cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &restored) ||
            std::memcmp(&original, &restored, 8))
            throw std::runtime_error("Cannot restore topology-probe thread affinity");
    }
    // Classify the actual allowed logical processors by the observed physical
    // core identifier, including SMT siblings. Reject physically impossible
    // topology reports (>8 physical cores on this Zen 2 PS5).
    std::array<int, 64> physical_ids{};
    std::array<std::uint64_t, 64> physical_masks{};
    unsigned physical_count = 0, classified = 0;
    for (unsigned cpu = 0; cpu < 64; ++cpu) {
        if (!CPU_ISSET(cpu, &original) || cpu_core[cpu] < 0) continue;
        ++classified;
        unsigned group = 0;
        while (group < physical_count && physical_ids[group] != cpu_core[cpu]) ++group;
        if (group == physical_count) physical_ids[physical_count++] = cpu_core[cpu];
        physical_masks[group] |= std::uint64_t{1} << cpu;
    }
    const bool physical_ids_plausible = physical_count <= 8;
    worker_topology_ready = count == cores.size() && physical_ids_plausible;
    if (!physical_ids_plausible)
        std::printf("EDEN_PS5_CPU_TOPOLOGY_REJECT reason=more_than_8_physical_ids observed=%u\n",
                    physical_count);
    std::printf("EDEN_PS5_CPU_PHYSICAL_SUMMARY classified=%u of=%u distinct_physical=%u valid=%u\n",
                classified, allowed_count, physical_count, unsigned(physical_ids_plausible));
    if (worker_topology_ready) {
        for (unsigned group = 0; group < physical_count; ++group)
            std::printf("EDEN_PS5_CPU_PHYSICAL core_id=%d logical_mask=0x%llx\n",
                        physical_ids[group], static_cast<unsigned long long>(physical_masks[group]));
        for (unsigned cpu = 0; cpu < 64; ++cpu) {
            if (!CPU_ISSET(cpu, &original) || cpu_core[cpu] < 0) continue;
            const bool guest_core = cpu_core[cpu] == static_cast<int>(cores[0]) ||
                cpu_core[cpu] == static_cast<int>(cores[1]) || cpu_core[cpu] == static_cast<int>(cores[2]);
            if (!guest_core && cpu != worker_cpus[3] && cpu != worker_cpus[4]) secondary_cpus |= 1ULL << cpu;
        }
        std::printf("EDEN_WORKER_SECONDARY mask=%llx\n", secondary_cpus);
        // Publish only after all CPU IDs and affinity/topology checks complete.
        // Logical-only experimental placement deliberately never publishes
        // this snapshot: a logical CPU ID is not physical-core evidence.
        std::uint64_t verified_mask = 0;
        for (unsigned cpu : worker_cpus)
            if (cpu < 64) verified_mask |= std::uint64_t{1} << cpu;
        verified_physical_worker_mask.store(verified_mask, std::memory_order_release);
        // CPU slots withheld from the shader pool are NOT necessarily OS
        // reserved: protect hot guest JIT cores from SMT sibling contention.
        const std::uint64_t excluded = allowed_mask & ~(verified_mask | secondary_cpus);
        std::uint64_t guest_smt_protected = 0;
        for (unsigned cpu = 0; cpu < 64; ++cpu) {
            if (!(excluded & (std::uint64_t{1} << cpu))) continue;
            const bool guest_sibling = cpu_core[cpu] == static_cast<int>(cores[0]) ||
                cpu_core[cpu] == static_cast<int>(cores[1]) ||
                cpu_core[cpu] == static_cast<int>(cores[2]);
            if (guest_sibling) guest_smt_protected |= std::uint64_t{1} << cpu;
        }
        std::printf("EDEN_PS5_CPU_SPLIT allowed=0x%llx primary=0x%llx secondary=0x%llx "
                    "guest_smt_protected=0x%llx other_allowed=0x%llx\n",
                    static_cast<unsigned long long>(allowed_mask),
                    static_cast<unsigned long long>(verified_mask),
                    secondary_cpus,
                    static_cast<unsigned long long>(guest_smt_protected),
                    static_cast<unsigned long long>(excluded & ~guest_smt_protected));
    }
    std::printf("EDEN_WORKER_TOPOLOGY ready=%d distinct_cores=%u cpus=%u,%u,%u,%u,%u amd_ext=%u x2apic=%u\n",
        worker_topology_ready, count, worker_cpus[0], worker_cpus[1], worker_cpus[2],
        worker_cpus[3], worker_cpus[4], unsigned(has_amd_topology), unsigned(has_x2apic));
}
// Explicit logical-CPU A/B trial when x2APIC topology is unverifiable.
// Different OS logical CPU IDs do NOT establish distinct physical cores.
void EnableExperimentalLogicalPlacementImpl() {
    if (worker_topology_ready || !topology_allowed_valid) {
        std::printf("EDEN_EXPERIMENT_CPU result=skipped reason=%s\n",
                    worker_topology_ready ? "physical_topology_available" : "affinity_unavailable");
        return;
    }
    std::array<unsigned, 64> allowed{};
    unsigned count = 0;
    for (unsigned cpu = 0; cpu < 64; ++cpu)
        if (CPU_ISSET(cpu, &topology_allowed)) allowed[count++] = cpu;
    if (count < 7) {
        std::printf("EDEN_EXPERIMENT_CPU result=skipped reason=too_few_logical_cpus count=%u\n", count);
        return;
    }
    std::array<unsigned, 5> candidate{};
    unsigned long long reserved = 0;
    for (unsigned i = 0; i < candidate.size(); ++i) {
        candidate[i] = allowed[i * (count - 1) / (candidate.size() - 1)];
        reserved |= 1ULL << candidate[i];
    }
    unsigned long long secondary = 0;
    for (unsigned i = 0; i < count; ++i)
        if (!(reserved & (1ULL << allowed[i]))) secondary |= 1ULL << allowed[i];
    if (!secondary) return;
    worker_cpus = candidate;
    secondary_cpus = secondary;
    worker_topology_ready = true;
    std::printf("EDEN_EXPERIMENT_CPU result=enabled physical_verified=0 logical=%u,%u,%u,%u,%u secondary_mask=%llx\n",
                worker_cpus[0], worker_cpus[1], worker_cpus[2],
                worker_cpus[3], worker_cpus[4], secondary_cpus);
}
void PlaceSecondary(const char* name) {
    if (!placement_secondary.load(std::memory_order_relaxed) || !secondary_cpus) return;
    cpuset_t mask{};
    for (unsigned cpu = 0; cpu < 64; ++cpu)
        if (secondary_cpus & (1ULL << cpu)) CPU_SET(cpu, &mask);
    const int result = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &mask);
    if (secondary_reports.fetch_add(1, std::memory_order_relaxed) < 64)
        std::printf("EDEN_WORKER_PLACED name=%s mask=%llx error=%d\n", name, secondary_cpus, result ? errno : 0);
}
void PinWorker(unsigned index, cpuset_t& affinity) {
    if (!worker_topology_ready || index >= worker_cpus.size() ||
        !CPU_ISSET(worker_cpus[index], topology_allowed_valid ? &topology_allowed : &affinity)) return;
    cpuset_t one{}, verified{};
    CPU_SET(worker_cpus[index], &one);
    if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &one)) {
        std::printf("EDEN_WORKER_PIN name=%s error=%d\n", names[index], errno);
        return;
    }
    if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &verified) ||
        std::memcmp(&one, &verified, 8)) {
        if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &affinity))
            throw std::runtime_error("Cannot restore worker affinity");
        return;
    }
    affinity = verified;
    std::printf("EDEN_WORKER_PIN name=%s cpu=%u verified=1\n", names[index], worker_cpus[index]);
}
#endif
bool cpu_clocks_valid = false;
bool owner_cpu_clock_valid = false;
std::atomic<unsigned> sample_epoch{};
struct CpuSample {
    unsigned epoch = ~0u;
    long long mono_ns = 0, cpu_ns = -ENOTSUP;
    unsigned long long thread = 0, pc = 0;
    unsigned svc = ~0u, fpcr = 0;
    unsigned long long compilations = 0, compile_ns = 0;
};
std::array<CpuSample, 4> cpu_samples;
// Read/written only by the worker owning this core; samples use workers_mutex.
std::array<unsigned, 4> sampled_epoch{~0u, ~0u, ~0u, ~0u};
clockid_t process_clock{};
int CurrentCpuClock(clockid_t* clock, bool process = false) {
#ifdef PS5_NATIVE
    // Native titles must use the imported kernel APIs. Firmware rejects direct
    // syscalls from the title. Qualification below rejects unusable CPU clocks.
    if (process) { *clock = CLOCK_PROCESS_CPUTIME_ID; return 0; }
    return pthread_getcpuclockid(pthread_self(), clock);
#else
    return process ? clock_getcpuclockid(0, clock) : pthread_getcpuclockid(pthread_self(), clock);
#endif
}
long long ClockNs(clockid_t clock) {
    timespec time{};
    if (clock_gettime(clock, &time) != 0) return -errno;
    return static_cast<long long>(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}
void CheckCpuClocks() {
    clockid_t worker_clock{};
    int worker_error = ENOENT;
    const int process_error = CurrentCpuClock(&process_clock, true);
    long long owner_begin = -1, owner_busy = -1, owner_idle = -1;
    std::binary_semaphore ready{0}, proceed{0};
    std::jthread worker([&] {
        worker_error = CurrentCpuClock(&worker_clock);
        ready.release();
        proceed.acquire();
        owner_begin = ClockNs(CLOCK_THREAD_CPUTIME_ID);
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
        while (std::chrono::steady_clock::now() < end) std::atomic_signal_fence(std::memory_order_seq_cst);
        owner_busy = ClockNs(CLOCK_THREAD_CPUTIME_ID);
        ready.release();
        proceed.acquire();
        owner_idle = ClockNs(CLOCK_THREAD_CPUTIME_ID);
    });
    ready.acquire();
    const auto cpu_begin = worker_error ? -1 : ClockNs(worker_clock);
    proceed.release();
    ready.acquire();
    const auto cpu_busy = worker_error ? -1 : ClockNs(worker_clock);
    const auto process_idle = process_error ? -1 : ClockNs(process_clock);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto cpu_idle = worker_error ? -1 : ClockNs(worker_clock);
    const auto process_end = process_error ? -1 : ClockNs(process_clock);
    proceed.release();
    worker.join();
    const auto own_busy = owner_busy - owner_begin;
    const auto own_idle = owner_idle - owner_busy;
    owner_cpu_clock_valid = owner_begin >= 0 && own_busy >= 5'000'000 &&
        own_busy < 30'000'000 && own_idle >= 0 && own_idle < own_busy / 4 + 1'000'000;
    std::printf("EDEN_PERF_OWNER_CLOCK_CHECK valid=%d busy_ns=%lld idle_ns=%lld\n",
                owner_cpu_clock_valid, own_busy, own_idle);
    const auto busy = cpu_busy - cpu_begin;
    const auto idle = cpu_idle - cpu_busy;
    const auto process_sleep = process_end - process_idle;
    cpu_clocks_valid = !worker_error && !process_error && cpu_begin >= 0 && process_idle >= 0 &&
        busy >= 1'000'000 && idle >= 0 && idle < busy / 4 + 1'000'000 &&
        process_sleep >= 0 && process_sleep < 6'000'000;
    std::printf("EDEN_PERF_CPU_CLOCK_CHECK valid=%d worker_error=%d process_error=%d "
                "worker_clock=%d process_clock=%d busy_ns=%lld idle_ns=%lld process_sleep_ns=%lld\n",
                cpu_clocks_valid, worker_error, process_error, int(worker_clock), int(process_clock),
                busy, idle, process_sleep);
}
}

void EnableExperimentalLogicalPlacement() {
#ifdef PS5_NATIVE
    EnableExperimentalLogicalPlacementImpl();
#endif
}

std::uint64_t PinnedWorkerMask() noexcept {
#ifdef PS5_NATIVE
    // Atomic generation snapshot: PS5 pipeline workers are constructed on
    // different threads, sometimes as the previous title is shutting down.
    // Unknown / logical-only topology returns zero and keeps admission
    // conservative, rather than double-counting unverified host cores.
    return verified_physical_worker_mask.load(std::memory_order_acquire);
#else
    return 0;
#endif
}

std::uint64_t VerifiedSecondaryPlacementMask() noexcept {
#ifdef PS5_NATIVE
    return verified_secondary_placement_mask.load(std::memory_order_acquire);
#else
    return 0;
#endif
}

void SetSecondaryPlacement(bool enabled) {
#ifdef PS5_NATIVE
    // Clear previous title/placement evidence BEFORE trying to repin. A
    // success return from setaffinity alone is not a verified OS mask.
    verified_secondary_placement_mask.store(0, std::memory_order_release);
    placement_secondary.store(enabled, std::memory_order_relaxed);
    if (!secondary_cpus || !topology_allowed_valid) return;
    cpuset_t mask{};
    if (enabled) {
        for (unsigned cpu = 0; cpu < 64; ++cpu)
            if (secondary_cpus & (1ULL << cpu)) CPU_SET(cpu, &mask);
    } else {
        mask = topology_allowed;
    }
    const int result = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &mask);
    cpuset_t actual{};
    const bool verified = enabled && result == 0 &&
        cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &actual) == 0 &&
        std::memcmp(&mask, &actual, 8) == 0;
    if (verified)
        verified_secondary_placement_mask.store(secondary_cpus, std::memory_order_release);
    std::printf("EDEN_WORKER_INHERIT enabled=%d mask=%llx error=%d verified=%u\n", enabled,
                enabled ? secondary_cpus : 0ULL, result ? errno : 0, unsigned(verified));
#else
    (void)enabled;
#endif
}

void SampleGpuFrame(unsigned frame) {
    std::printf("EDEN_PERF_GPU_FRAME frame=%u mono_ns=%lld cpu_ns=%lld\n", frame,
                ClockNs(CLOCK_MONOTONIC),
                owner_cpu_clock_valid ? ClockNs(CLOCK_THREAD_CPUTIME_ID) : -static_cast<long long>(ENOTSUP));
}

extern "C" void ps5_opengl_heap_snapshot(const char* phase, unsigned iteration);
extern "C" unsigned eden_heap_arenas_created(void) __attribute__((weak));
extern "C" std::size_t eden_heap_committed(void) __attribute__((weak));
extern "C" std::size_t eden_heap_tcache_held(void) __attribute__((weak));
extern "C" std::size_t eden_heap_large_held(unsigned* blocks) __attribute__((weak));
extern "C" std::size_t eden_heap_large_physical_held(void) __attribute__((weak));
#ifdef PS5_NATIVE
extern "C" std::int64_t sceKernelGetDirectMemorySize();
extern "C" std::int32_t sceKernelAvailableDirectMemorySize(std::int64_t, std::int64_t, std::size_t, std::int64_t*,
                                                           std::size_t*);
// The allocated region at or after an offset (flag 1: find the next one).
struct DirectMemoryRegion { std::int64_t start; std::int64_t end; std::int32_t type; };
extern "C" std::int32_t sceKernelDirectMemoryQuery(std::int64_t, int, DirectMemoryRegion*, std::size_t);
#endif

// Query the actual contiguous direct-memory headroom available at guest
// launch. Fail closed (no expanded JIT) when firmware cannot report it.
bool QueryLargestDirectMemoryBlock(std::size_t* largest) noexcept {
    if (!largest) return false;
    *largest = 0;
#ifdef PS5_NATIVE
    const std::int64_t total = sceKernelGetDirectMemorySize();
    if (total <= 0) return false;
    std::int64_t start = -1;
    std::size_t observed = 0;
    if (sceKernelAvailableDirectMemorySize(0, total, 0x4000,
                                           &start, &observed) != 0 ||
        !::Eden::DirectPool::IsKernelFreeSpanValid(total, start, observed))
        return false;
    *largest = observed;
    return true;
#else
    return false;
#endif
}

#ifdef PS5_NATIVE
// Native process-global observations must be reset between games: the next
// title can reserve a completely different amount of physical GPU memory.
static std::atomic<unsigned long long> largest_free_block{0};
static std::atomic<long long> checked_ns{0};
static std::atomic<bool> query_in_flight{false};
static std::atomic<unsigned> consecutive_failures{0};
static std::atomic<bool> has_valid_sample{false};
#endif

void ResetDirectMemoryProbeForTitle() noexcept {
#ifdef PS5_NATIVE
    // The caller must have joined all GPU/renderer workers. Refuse to erase
    // a still-running kernel probe, which could publish stale data afterward.
    if (query_in_flight.load(std::memory_order_acquire)) std::abort();
    has_valid_sample.store(false, std::memory_order_release);
    consecutive_failures.store(0, std::memory_order_relaxed);
    largest_free_block.store(0, std::memory_order_relaxed);
    graphics_memory_short.store(true, std::memory_order_relaxed);
    // Zero timestamp forces the new title's first probe immediately, rather
    // than reusing the prior title's result for up to 100 ms.
    checked_ns.store(0, std::memory_order_release);
#endif
}

#ifdef PS5_NATIVE
// The largest free block of direct memory is what the next graphics allocation needs.
// 100 ms minimum (250 ms only with >=4 GiB confirmed free);
// called on the GPU thread by the texture collector.
static void RefreshFreeMemory() {
    const long long now = NowNs();
    long long last = checked_ns.load(std::memory_order_relaxed);
    if (last != 0 && (now <= last || now - last < 100'000'000))
        return;
    // Do not add any new atomics to the common per-frame fast-return path.
    // At most once per 100 ms, a confirmed >=4-GiB contiguous free block
    // permits 250-ms polling instead of blocking the GPU owner every 100 ms.
    // Low memory, first probe, or an error retains the original fast cadence.
    if (last != 0 &&
        now - last < ::Eden::DirectPool::ProbeIntervalNs(
            has_valid_sample.load(std::memory_order_acquire),
            consecutive_failures.load(std::memory_order_relaxed),
            largest_free_block.load(std::memory_order_relaxed)))
        return;
    // Two GPU/cache clients can hit this on the same refresh boundary.
    // The old separate load/store let BOTH perform a synchronous kernel
    // largest-free-range query, and counted two concurrent errors as two
    // consecutive misses. Elect exactly one refresh owner every >=100ms.
    if (!checked_ns.compare_exchange_strong(last, now, std::memory_order_acq_rel,
                                            std::memory_order_relaxed))
        return;
    // If a kernel query itself takes >100 ms, a later timestamp claimant
    // must not overtake it and publish a newer measurement before the older
    // query eventually returns. One in-flight query at a time, without
    // blocking any renderer caller on the query owner's kernel syscall.
    if (query_in_flight.exchange(true, std::memory_order_acq_rel))
        return;
#if defined(EDEN_DEV_PROFILE)
    const long long query_start_ns = ClockNs(CLOCK_MONOTONIC);
#endif
    std::int64_t start = 0;
    std::size_t largest = 0;
    // The physical direct-memory address-space bound is constant for this
    // PS5 process. Re-reading it at each 100 ms GPU pressure check wastes
    // an independent kernel call. Only cache a successful positive result:
    // startup errors still retry and remain fail-closed as before.
    // This variable is accessed exclusively by the elected single-flight
    // query owner above; all other renderer callers return before it.
    static std::int64_t cached_direct_total = 0;
    if (cached_direct_total <= 0) {
        const std::int64_t observed_total = sceKernelGetDirectMemorySize();
        if (observed_total > 0) cached_direct_total = observed_total;
    }
    const bool known = cached_direct_total > 0 &&
        sceKernelAvailableDirectMemorySize(0, cached_direct_total, 0x4000, &start, &largest) == 0 &&
        ::Eden::DirectPool::IsKernelFreeSpanValid(cached_direct_total, start, largest);
    // Unknown is not the same as an observed low-memory condition. A single
    // transient failed query previously flipped memory_short to true and
    // immediately enabled expensive dirty-texture download/eviction. Keep
    // the last kernel-confirmed headroom for ONE missed 100ms refresh only.
    // First-ever failure and consecutive failures fail safely to "short".
    if (known) {
        largest_free_block.store(largest, std::memory_order_relaxed);
        graphics_memory_short.store(largest < kShortMemory, std::memory_order_relaxed);
        consecutive_failures.store(0, std::memory_order_relaxed);
        has_valid_sample.store(true, std::memory_order_release);
    } else {
        const unsigned failed = consecutive_failures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!has_valid_sample.load(std::memory_order_acquire) || failed >= 2) {
            largest_free_block.store(0, std::memory_order_relaxed);
            graphics_memory_short.store(true, std::memory_order_relaxed);
        }
        // Only two lines per failure streak, no per-frame or allocator logging.
        static std::atomic<unsigned> reported{0};
        if ((failed == 1 || failed == 2) &&
            reported.fetch_add(1, std::memory_order_relaxed) < 16)
            std::printf("EDEN_PS5_DMEM_PROBE_FAILED consecutive=%u fallback=%s\n", failed,
                        failed == 1 && has_valid_sample.load(std::memory_order_relaxed)
                            ? "last_confirmed" : "conservative");
    }
#if defined(EDEN_DEV_PROFILE)
    const long long query_end_ns = ClockNs(CLOCK_MONOTONIC);
    if (query_start_ns > 0 && query_end_ns > query_start_ns &&
        query_end_ns - query_start_ns >= 2'000'000) {
        static std::atomic<unsigned> slow_queries_logged{0};
        if (slow_queries_logged.fetch_add(1, std::memory_order_relaxed) < 16)
            std::printf("EDEN_PS5_DMEM_PROBE_SLOW latency_ns=%lld known=%u\n",
                        query_end_ns - query_start_ns, unsigned(known));
    }
#endif
    query_in_flight.store(false, std::memory_order_release);
}
static bool GraphicsMemoryShort() {
    RefreshFreeMemory();
    return graphics_memory_short.load(std::memory_order_relaxed);
}
// What the caches' "memory in use" is measured against (performance.h, graphics_memory_free).
static unsigned long long GraphicsMemoryFree() {
    RefreshFreeMemory();
    return largest_free_block.load(std::memory_order_relaxed);
}
[[maybe_unused]] static const bool graphics_memory_probe_installed =
    (graphics_memory_probe.store(&GraphicsMemoryShort, std::memory_order_relaxed),
     graphics_memory_free.store(&GraphicsMemoryFree, std::memory_order_relaxed), true);
#endif

#ifdef EDEN_DEV_PROFILE
namespace {
// Fixed-size, owned-name counters: no per-request allocator/mutex contention
// between the four guest CPUs during the development all-on profiling pass.
// Shipping neither constructs nor retains this ~400 KiB profiling table.
HleCounters hle_calls;
} // namespace
#endif

void RecordHle(const char* service, unsigned command, long long ns) {
#ifdef EDEN_DEV_PROFILE
    hle_calls.Record(service, command, ns);
#else
    (void)service;
    (void)command;
    (void)ns;
#endif
}

// Report true JIT ownership once at meaningful lifecycle stages; this
// does not enumerate all kernel direct-memory ranges or run on a frame.
void ReportJitCodeState(const char* phase) {
#ifdef PS5_NATIVE
    std::size_t reserved = 0, committed = 0;
    ::Common::SparseJitUsage(&reserved, &committed);
    std::printf("EDEN_JIT_MEMORY phase=%s sparse_reserved=%zu sparse_committed=%zu dense_direct=%zu\n",
                phase, reserved, committed, ::Common::DenseJitDirectBytes());
#else
    (void)phase;
#endif
}

// A Sony region enumeration may cost up to 8192 synchronous kernel calls.
// Do not make every menu -> game -> menu cycle pay for developer diagnostics.
// Explicit opt-in is checked once at title launch; never from a frame callback.
static std::atomic<bool> direct_region_scan_enabled{false};
void SetDirectMemoryRegionScanEnabled(bool enabled) noexcept {
    direct_region_scan_enabled.store(enabled, std::memory_order_release);
}
// GPU frame reports never enumerate kernel direct-memory regions.
void ReportDirectMemoryState(const char* phase) {
#ifdef PS5_NATIVE
    const std::int64_t total = sceKernelGetDirectMemorySize();
    if (total <= 0) {
        std::printf("EDEN_MEMORY_LAYOUT phase=%s status=unavailable\n", phase);
        return;
    }
    std::int64_t largest_start = -1;
    std::size_t largest = 0;
    const int largest_rc = sceKernelAvailableDirectMemorySize(
        0, total, 0x4000, &largest_start, &largest);
    const bool largest_valid = largest_rc == 0 &&
        ::Eden::DirectPool::IsKernelFreeSpanValid(total, largest_start, largest);

    // An early kernel stop can mean EOF or error; never equate structurally
    // valid records with a complete scan. Even a valid free_upper is only
    // an upper bound, NOT allocatable RAM or a GPU/JIT admission value.
    ::Eden::DirectPool::RegionScan scan{total};
    int query_rc = 0;
    const bool scan_requested = direct_region_scan_enabled.load(std::memory_order_acquire);
    // 4=disabled by default; owner ledger and largest free are still reported.
    unsigned scan_stop = scan_requested ? 0u : 4u;
    if (scan_requested) {
        // 0=end-of-extent, 1=query-stop, 2=invalid, 3=record-cap.
        while (scan.cursor < total && scan.regions < 8192) {
            DirectMemoryRegion region{};
            query_rc = sceKernelDirectMemoryQuery(scan.cursor, 1, &region, sizeof(region));
            if (query_rc != 0) {
                scan_stop = 1;
                break;
            }
            if (!scan.Include(region.start, region.end)) {
                scan_stop = 2;
                break;
            }
        }
        if (!scan_stop && !scan.ReachedExtent()) scan_stop = 3;
    }
    const long long free_upper = scan_requested
        ? static_cast<long long>(scan.FreeUpperBound()) : -1LL;
    std::size_t jit_reserved = 0, jit_committed = 0;
    ::Common::SparseJitUsage(&jit_reserved, &jit_committed);
    // Native GPU/guest/CPU use the SAME kernel-exposed direct pool (12 GiB
    // observed on FW 13.60, not the full installed 16 GiB GDDR6). Report
    // physical owners, not uncommitted virtual VA reservations. The Sony
    // system's own reservation must never be counted as an Encore heap.
    unsigned large_blocks = 0;
    const std::size_t heap_large = eden_heap_large_held ? eden_heap_large_held(&large_blocks) : 0;
    const bool heap_large_physical_known = eden_heap_large_physical_held != nullptr;
    const std::size_t heap_large_physical =
        heap_large_physical_known ? eden_heap_large_physical_held() : 0;
    std::printf("EDEN_HEAP_LIFETIME phase=%s pieces=%zu large=%zu large_blocks=%u tcache=%zu large_physical=%zu physical_known=%u\n",
                phase, eden_heap_committed ? eden_heap_committed() : std::size_t{0},
                heap_large, large_blocks, eden_heap_tcache_held ? eden_heap_tcache_held() : std::size_t{0},
                heap_large_physical, unsigned(heap_large_physical_known));
    std::printf("EDEN_JIT_SPARSE_MEMORY phase=%s reserved=%zu committed=%zu\n",
                phase, jit_reserved, jit_committed);
    const std::size_t jit_dense = ::Common::DenseJitDirectBytes();
    std::printf("EDEN_JIT_DENSE_MEMORY phase=%s physically_owned=%zu\n",
                phase, jit_dense);
    std::size_t sparse_virtual = 0, sparse_physical = 0;
    ::Common::SparseUsage(&sparse_virtual, &sparse_physical);
    const std::size_t heap_roots = eden_heap_committed ? eden_heap_committed() : std::size_t{0};
    const auto account = ::Eden::DirectPool::Summarize(total, {
        .heap_roots = heap_roots,
        .heap_large = heap_large_physical,
        .sparse_tables = sparse_physical,
        .jit_sparse = jit_committed,
        .jit_dense = jit_dense,
    });
    // These five tracked owner sets are distinct direct-memory allocations.
    // Untracked is NOT free: the renderer, guest and firmware may own it.
    // The largest contiguous free block comes exclusively from a kernel query.
    // Lifecycle checkpoints only: no per-frame region enumeration or logging.
    std::printf("EDEN_DIRECT_POOL_OWNERS phase=%s extent=%llu heap_roots=%zu "
                "heap_large=%zu heap_large_requested=%zu large_owner_known=%u sparse_tables=%zu jit_sparse=%zu jit_dense=%zu "
                "tracked=%llu unclassified=%llu tracked_within_extent=%u "
                "largest_free_known=%u largest_free=%zu\n",
                phase, static_cast<unsigned long long>(account.extent_bytes),
                heap_roots, heap_large_physical, heap_large, unsigned(heap_large_physical_known),
                sparse_physical, jit_committed, jit_dense,
                static_cast<unsigned long long>(account.tracked_bytes),
                static_cast<unsigned long long>(account.not_tracked_bytes),
                unsigned(account.within_extent), unsigned(largest_valid),
                largest_valid ? largest : std::size_t{0});
    std::printf("EDEN_MEMORY_LAYOUT phase=%s largest_rc=%d total=%lld largest=%zu "
                "largest_start=%lld largest_valid=%u free_upper=%lld scanned_regions=%u scan_valid=%d "
                "scan_reached_extent=%u scan_stop=%u query_rc=%d short=%d\n",
                phase, largest_rc, static_cast<long long>(total),
                largest_valid ? largest : size_t{0},
                static_cast<long long>(largest_start), unsigned(largest_valid), free_upper,
                scan.regions, int(scan_requested && scan.valid),
                unsigned(scan_requested && scan.ReachedExtent()),
                scan_stop, query_rc, int(graphics_memory_short.load(std::memory_order_relaxed)));
#else
    (void)phase;
#endif
}

void ReportGpuThread(unsigned frame) {
    // Heap growth and arena use over the run (allocation failures abort the title).
    ps5_opengl_heap_snapshot("vulkan_report", frame);
    {
        // What the heap and Eden's large tables hold of the memory the CPU shares with the GPU:
        // the heap's pieces (they only grow), its large blocks alive now, and the tables' own
        // slots out of the address range they span.
        unsigned large_blocks = 0;
        const std::size_t large = eden_heap_large_held ? eden_heap_large_held(&large_blocks) : 0;
        std::size_t table_span = 0, table_held = 0;
        ::Common::SparseUsage(&table_span, &table_held);
        std::printf("EDEN_PERF_HEAP arenas=%u pieces=%zu large=%zu large_blocks=%u tables=%zu table_span=%zu tcache_bytes=%zu\n",
                    eden_heap_arenas_created ? eden_heap_arenas_created() : 1u,
                    eden_heap_committed ? eden_heap_committed() : std::size_t{0}, large, large_blocks, table_held,
                    table_span, eden_heap_tcache_held ? eden_heap_tcache_held() : std::size_t{0});
    }
#ifdef EDEN_DEV_PROFILE
    // A game can use thousands of distinct service/command pairs.
    // Printing one line per pair ON THE GPU THREAD every five seconds
    // creates another avoidable frame hitch even without an HLE mutex.
    // Scan the atomic counters without locks or allocations, but report
    // only the 32 heaviest cumulative service commands plus totals.
    struct HleReportRow {
        const char* name{};
        unsigned command{};
        std::uint64_t calls{};
        std::uint64_t ns{};
    };
    std::array<HleReportRow, 32> heaviest{};
    std::size_t heavy_count = 0;
    std::size_t active_keys = 0;
    std::uint64_t total_hle_calls = 0, total_hle_ns = 0;
    hle_calls.ForEach([&](const char* name, unsigned command, std::uint64_t calls,
                          std::uint64_t ns) {
        ++active_keys;
        total_hle_calls += calls;
        total_hle_ns += ns;
        if (ns < 1'000'000) return;
        const HleReportRow candidate{name, command, calls, ns};
        if (heavy_count < heaviest.size()) {
            heaviest[heavy_count++] = candidate;
            return;
        }
        std::size_t least = 0;
        for (std::size_t i = 1; i < heaviest.size(); ++i)
            if (heaviest[i].ns < heaviest[least].ns) least = i;
        if (candidate.ns > heaviest[least].ns)
            heaviest[least] = candidate;
    });
    std::sort(heaviest.begin(), heaviest.begin() + heavy_count,
              [](const HleReportRow& a, const HleReportRow& b) {
                  return a.ns > b.ns;
              });
    for (std::size_t i = 0; i < heavy_count; ++i) {
        const auto& row = heaviest[i];
        std::printf("EDEN_DEV_HLE service=%s cmd=%u calls=%llu ns=%llu\n",
                    row.name, row.command,
                    static_cast<unsigned long long>(row.calls),
                    static_cast<unsigned long long>(row.ns));
    }
    std::printf("EDEN_DEV_HLE_SUMMARY keys=%zu reported=%zu calls=%llu ns=%llu\n",
                active_keys, heavy_count,
                static_cast<unsigned long long>(total_hle_calls),
                static_cast<unsigned long long>(total_hle_ns));
    if (const auto overflow = hle_calls.OverflowCalls(); overflow)
        std::printf("EDEN_DEV_HLE_OVERFLOW calls=%llu ns=%llu\n",
                    static_cast<unsigned long long>(overflow),
                    static_cast<unsigned long long>(hle_calls.OverflowNs()));
#endif
#ifdef PS5_NATIVE
    // Called on the native GPU thread inside its periodic frame-report branch.
    // A complete sceKernelDirectMemoryQuery region walk (up to 8192 syscalls)
    // here stalls actual rendering and can imitate the FC27 hitch under study.
    // The 100ms collector already publishes the confirmed largest free block;
    // reading its atomics requires ZERO additional firmware syscalls.
    std::printf("EDEN_MEMORY_LIVE frame=%u largest_last_confirmed=%llu short=%u\n",
                frame, largest_free_block.load(std::memory_order_relaxed),
                unsigned(graphics_memory_short.load(std::memory_order_relaxed)));
    // Preserve the existing five-second parser's sparse JIT time series
    // without reintroducing a contended mutex or 8192 kernel queries.
    std::size_t jit_reserved_live = 0, jit_committed_live = 0;
    ::Common::SparseJitUsageFast(&jit_reserved_live, &jit_committed_live);
    std::printf("EDEN_JIT_SPARSE_MEMORY phase=dev-profile reserved=%zu committed=%zu\n",
                jit_reserved_live, jit_committed_live);
#endif
    const auto load = [](const Totals& totals, bool calls) {
        return (calls ? totals.calls : totals.nanoseconds).load(std::memory_order_relaxed);
    };
    // Cumulative totals; the analyzer takes deltas between consecutive reports.
    std::printf("EDEN_DEV_GPU frame=%u mono_ns=%lld cpu_ns=%lld idle_ns=%llu dispatch_calls=%llu "
                "dispatch_ns=%llu drain_calls=%llu drain_ns=%llu present_calls=%llu present_ns=%llu "
                "full_calls=%llu full_ns=%llu draws=%llu\n", frame, ClockNs(CLOCK_MONOTONIC),
                owner_cpu_clock_valid ? ClockNs(CLOCK_THREAD_CPUTIME_ID) : -static_cast<long long>(ENOTSUP),
                load(gpu_queue_wait, false), load(gpu_dispatch, true), load(gpu_dispatch, false),
                load(gpu_fence_drain, true), load(gpu_fence_drain, false),
                load(gpu_present_wait, true), load(gpu_present_wait, false),
                load(gpu_queue_full, true), load(gpu_queue_full, false), load(rasterizer_draw, true));
    // Breakdown of GPU-owner command types. In the R293 quiet path no clocks
    // or counters are touched; this runs only inside ReportGpuThread's opt-in
    // five-second diagnostic callback. Values are cumulative, not FPS.
    std::printf("EDEN_GPU_COMMANDS frame=%u tick_calls=%llu tick_ns=%llu "
                "flush_calls=%llu flush_ns=%llu invalidate_calls=%llu invalidate_ns=%llu\n",
                frame, load(gpu_command_tick, true), load(gpu_command_tick, false),
                load(gpu_command_flush, true), load(gpu_command_flush, false),
                load(gpu_command_invalidate, true), load(gpu_command_invalidate, false));
    std::printf("EDEN_DEV_GUEST cpu_write_calls=%llu cpu_write_ns=%llu cpu_read_calls=%llu cpu_read_ns=%llu "
                "sync_calls=%llu sync_ns=%llu dequeue_calls=%llu dequeue_ns=%llu",
                load(guest_cpu_write, true), load(guest_cpu_write, false),
                load(guest_cpu_read, true), load(guest_cpu_read, false),
                load(guest_sync_wait, true), load(guest_sync_wait, false),
                load(guest_dequeue_wait, true), load(guest_dequeue_wait, false));
    std::printf(" ipc_calls=%llu ipc_ns=%llu", load(guest_ipc_wait, true), load(guest_ipc_wait, false));
    std::printf(" cache_lock_contended=%llu cache_lock_blocked=%llu cache_lock_wait_ns=%llu",
                cache_lock_contended.load(std::memory_order_relaxed),
                cache_lock_blocked.load(std::memory_order_relaxed),
                cache_lock_wait_ns.load(std::memory_order_relaxed));
    std::printf(" fs_file_calls=%llu fs_file_ns=%llu fs_file_bytes=%llu fs_storage_calls=%llu fs_storage_ns=%llu "
                "fs_storage_bytes=%llu",
                load(guest_fs_file, true), load(guest_fs_file, false),
                guest_fs_file_bytes.load(std::memory_order_relaxed),
                load(guest_fs_storage, true), load(guest_fs_storage, false),
                guest_fs_storage_bytes.load(std::memory_order_relaxed));
    for (unsigned core = 0; core < jit_callbacks.size(); ++core)
        std::printf(" r%u=%llu w%u=%llu x%u=%llu", core, jit_callbacks[core].reads.load(std::memory_order_relaxed),
                    core, jit_callbacks[core].writes.load(std::memory_order_relaxed),
                    core, jit_callbacks[core].exclusive_writes.load(std::memory_order_relaxed));
    for (unsigned core = 0; core < 3; ++core)
        std::printf(" idle%u=%llu/%llu/%llu", core, guest_idle[core].calls.load(std::memory_order_relaxed),
                    guest_idle[core].nanoseconds.load(std::memory_order_relaxed) / 1000000,
                    guest_idle[core].sleeps.load(std::memory_order_relaxed));
    std::printf(" cond=");
    for (unsigned slot = 0; slot < render_conditions.size(); ++slot)
        std::printf("%s%llu", slot ? "," : "", render_conditions[slot].load(std::memory_order_relaxed));
    std::printf("\n");
    const auto window = Eden::Fastmem::WindowStats();
    std::printf("EDEN_FASTMEM window=%llx pages=%llu chunks=%llu direct_reads=%llu direct_writes=%llu "
                "faults=%llu maps=%llu unmaps=%llu protects=%llu kernel_calls=%llu kernel_ns=%llu failures=%llu\n",
                static_cast<unsigned long long>(window.window), static_cast<unsigned long long>(window.mapped_pages),
                static_cast<unsigned long long>(window.aliased_chunks),
                static_cast<unsigned long long>(window.direct_reads),
                static_cast<unsigned long long>(window.direct_writes),
                static_cast<unsigned long long>(Eden::Fastmem::Faults()),
                static_cast<unsigned long long>(window.map_calls), static_cast<unsigned long long>(window.unmap_calls),
                static_cast<unsigned long long>(window.protect_calls),
                static_cast<unsigned long long>(window.kernel_calls), static_cast<unsigned long long>(window.kernel_ns),
                static_cast<unsigned long long>(window.failures));
    // Guest cores record their owner CPU clocks at their next JIT exit.
    Snapshot();
}

void RegisterWorker(const char* name) {
    Eden::Crash::NameThread(name);  // a crash report names the thread
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
    Eden::Stall::NoteThread(name);
#endif
    for (unsigned i = 0; i < names.size(); ++i) {
        if (std::strcmp(name, names[i])) continue;
        Worker worker;
        worker.thread = pthread_self();
        // Construct this thread's exit hook BEFORE pc_core_registration and
        // the optional jthread sampler. TLS destruction will therefore stop
        // the producer, clear core readiness, and finally unregister worker.
        owned_worker.owner = worker.thread;
#ifdef EDEN_DEV_PROFILE
        if (pc_sampling && (i == 4 || i == pc_sample_core.load())) {
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGUSR2);
            if (pthread_sigmask(SIG_UNBLOCK, &mask, nullptr))
                throw std::runtime_error("Cannot enable development PC sampler");
            if (i == pc_sample_core.load()) {
                core_sample_thread = pthread_self();
                pc_core_registration.active = true;
                core_sample_ready.store(true, std::memory_order_release);
                // dev-settings pc_fast=on: 500 Hz from 45s after registration.
                // Unlike detached threads, a thread-local jthread requests
                // stop and joins when its owning guest CPU worker exits. Bound
                // the boot delay in 200ms stop-aware steps, so game teardown
                // never waits another 45 seconds for an abandoned sampler.
                if (pc_fast.load()) {
                    static thread_local std::jthread fast_sampler;
                    const pthread_t target = pthread_self();
                    fast_sampler = std::jthread([target](std::stop_token stop) {
#ifdef PS5_NATIVE
                        PlaceSecondary("pc_sampler");
#endif
                        for (unsigned tick = 0; tick < 225 && !stop.stop_requested(); ++tick)
                            std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        while (!stop.stop_requested() && pthread_kill(target, SIGUSR2) == 0)
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    });
                }
            }
        }
#endif
        worker.clock_error = CurrentCpuClock(&worker.clock);
        worker.wall_ns = Common::g_wall_clock.GetTimeNS().count();
        worker.mono_ns = ClockNs(CLOCK_MONOTONIC);
#ifdef PS5_NATIVE
        cpuset_t affinity{};
        const int result = cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
                                             sizeof(unsigned long long), &affinity);
#else
        cpu_set_t affinity{};
        const int result = sched_getaffinity(0, sizeof(affinity), &affinity);
#endif
#ifdef PS5_NATIVE
        if (result == 0) PinWorker(i, affinity);
#endif
        worker.affinity_error = result == 0 ? 0 : errno;
        if (!worker.affinity_error) {
            for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
                if (!CPU_ISSET(cpu, &affinity)) continue;
                ++worker.allowed;
                if (cpu < 64) worker.mask |= 1ULL << cpu;
            }
        }
        worker.registered = true;
        {
            const std::lock_guard lock(workers_mutex);
            workers[i] = worker;
        }
        owned_worker.index = i;
        return;
    }
#ifdef PS5_NATIVE
    PlaceSecondary(name);
#endif
}

void SampleCpu(unsigned core, unsigned long long thread, unsigned long long pc, unsigned svc, unsigned fpcr) {
    const auto epoch = sample_epoch.load(std::memory_order_relaxed);
    if (sampled_epoch.at(core) == epoch) return;
    sampled_epoch[core] = epoch;
    const CpuSample sample{epoch, ClockNs(CLOCK_MONOTONIC),
        owner_cpu_clock_valid ? ClockNs(CLOCK_THREAD_CPUTIME_ID) : -ENOTSUP, thread, pc, svc, fpcr,
        compilation[core].calls.load(std::memory_order_relaxed),
        compilation[core].nanoseconds.load(std::memory_order_relaxed)};
    std::lock_guard lock(workers_mutex);
    cpu_samples[core] = sample;
}

void Snapshot() {
    // PC sample cursors and one-shot block dumps require serialized consumers,
    // but MUST NOT hold workers_mutex while formatting a large snapshot.
    const std::lock_guard snapshot_guard(snapshot_mutex);
    // Request a fresh owner-written sample; report the last completed sample with
    // its own timestamp. An idle core may remain stale; never infer zero CPU use.
    sample_epoch.fetch_add(1, std::memory_order_relaxed);
    const auto mono = ClockNs(CLOCK_MONOTONIC);
    std::printf("EDEN_PERF_SAMPLE mono_ns=%lld process_cpu_ns=%lld wall_ns=%lld\n",
                mono, cpu_clocks_valid ? ClockNs(process_clock) : -static_cast<long long>(ENOTSUP),
                static_cast<long long>(Common::g_wall_clock.GetTimeNS().count()));
#ifdef EDEN_DEV_PROFILE
    std::map<uintptr_t, unsigned> counts;
    const unsigned end = pc_count.load(std::memory_order_acquire);
    if (end < pc_reported) pc_reported = end; // wrap/reset safety
    if (end - pc_reported > sampled_pcs.size()) {
        const unsigned lost = end - pc_reported - static_cast<unsigned>(sampled_pcs.size());
        pc_reported = end - sampled_pcs.size();
        std::printf("EDEN_PERF_PC_SAMPLES_LOST source=gpu count=%u\n", lost);
    }
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
    std::map<std::array<uintptr_t, 9>, unsigned> caller_counts;
#endif
    while (pc_reported < end) {
        const unsigned index = pc_reported++;
        ++counts[sampled_pcs[index % sampled_pcs.size()].load(std::memory_order_relaxed)];
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
        std::array<uintptr_t, 9> key{
            sampled_pcs[index % sampled_pcs.size()].load(std::memory_order_relaxed)};
        std::copy(sampled_callers[index % sampled_callers.size()].begin(),
                  sampled_callers[index % sampled_callers.size()].end(), key.begin() + 1);
        ++caller_counts[key];
#endif
    }
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
    for (const auto& [chain, count] : caller_counts) {
        std::printf("EDEN_PERF_NATIVE_CALLERS mono_ns=%lld pc=%llx count=%u callers=", mono,
                    static_cast<unsigned long long>(chain[0]), count);
        for (unsigned i = 1; i < chain.size(); ++i)
            std::printf("%s%llx", i == 1 ? "" : ",", static_cast<unsigned long long>(chain[i]));
        std::printf("\n");
    }
#endif
    for (const auto& [pc, count] : counts)
        std::printf("EDEN_PERF_NATIVE_PC mono_ns=%lld pc=%llx count=%u\n", mono,
                    static_cast<unsigned long long>(pc), count);
    std::map<uintptr_t, unsigned> core_counts;
    const unsigned core_end = core_pc_count.load(std::memory_order_acquire);
    if (core_end < core_pc_reported) core_pc_reported = core_end;
    if (core_end - core_pc_reported > sampled_core_pcs.size()) {
        const unsigned lost = core_end - core_pc_reported - static_cast<unsigned>(sampled_core_pcs.size());
        core_pc_reported = core_end - sampled_core_pcs.size();
        std::printf("EDEN_PERF_PC_SAMPLES_LOST source=guest count=%u\n", lost);
    }
    while (core_pc_reported < core_end) {
        const auto index = core_pc_reported++;
        ++core_counts[sampled_core_pcs[index % sampled_core_pcs.size()].load(std::memory_order_relaxed)];
    }
    for (const auto& [pc, count] : core_counts)
        std::printf("EDEN_PERF_CORE_PC mono_ns=%lld pc=%llx count=%u\n", mono,
                    static_cast<unsigned long long>(pc), count);
    // The same samples by guest block: location (A32 PC in the low word), block offset, count.
    if (const std::size_t blocks = core0_blocks ? core0_block_count.load(std::memory_order_acquire) : 0) {
        std::map<std::pair<unsigned long long, unsigned long long>, unsigned> guest_counts;
        for (const auto& [pc, count] : core_counts) {
            const JitBlock* begin = core0_blocks;
            const JitBlock* it = std::upper_bound(begin, begin + blocks, pc,
                                                  [](uintptr_t value, const JitBlock& block) { return value < block.entry; });
            if (it == begin) continue;
            --it;
            if (pc - it->entry < it->size) guest_counts[{it->location, pc - it->entry}] += count;
        }
        for (const auto& [key, count] : guest_counts)
            std::printf("EDEN_PERF_GUEST_PC mono_ns=%lld location=%llx host_offset=%llx count=%u\n", mono,
                        key.first, key.second, count);
        // P1: guest and host code of this window's six hottest blocks not dumped before.
        std::map<unsigned long long, unsigned> block_counts;
        for (const auto& [key, count] : guest_counts) block_counts[key.first] += count;
        std::vector<std::pair<unsigned, unsigned long long>> ranked;
        for (const auto& [location, count] : block_counts) ranked.emplace_back(count, location);
        std::sort(ranked.rbegin(), ranked.rend());
        static std::set<unsigned long long> dumped;
        unsigned printed = 0;
        for (const auto& [count, location] : ranked) {
            if (printed == 6) break;
            if (!dumped.insert(location).second) continue;
            ++printed;
            const JitBlock* block = nullptr;
            for (std::size_t i = blocks; i-- > 0;)
                if (core0_blocks[i].location == location) { block = &core0_blocks[i]; break; }
            // A report runs on the native GPU render thread. Formatting
            // each 4 KiB code block with 4096 separate snprintf calls and
            // growing std::strings could visibly stall FC27 every five
            // seconds. Use bounded stack buffers and direct hex digits:
            // byte-for-byte identical guest/host diagnostic log format.
            constexpr char hex[] = "0123456789abcdef";
            std::array<char, 64 * 8 + 1> guest{};
            std::size_t guest_written = 0;
            const unsigned long long pc = location & 0xffffffffull;
            for (unsigned long long at = pc; at < pc + 256 && guest_read32; at += 4) {
                unsigned word = 0;
                if (!guest_read32(at, word)) break;
                for (int nibble = 7; nibble >= 0; --nibble)
                    guest[guest_written++] = hex[(word >> (nibble * 4)) & 0xfu];
            }
            guest[guest_written] = '\0';
            std::printf("EDEN_PERF_BLOCK_GUEST location=%llx count=%u words=%s\n",
                        location, count, guest.data());
            if (block) {
                std::array<char, 4096 * 2 + 1> host{};
                const auto* bytes = reinterpret_cast<const unsigned char*>(block->entry);
                const auto byte_count = std::min<unsigned long long>(block->size, 4096);
                for (unsigned long long i = 0; i < byte_count; ++i) {
                    const unsigned char value = bytes[i];
                    host[2 * i] = hex[value >> 4];
                    host[2 * i + 1] = hex[value & 0xfu];
                }
                host[2 * byte_count] = '\0';
                std::printf("EDEN_PERF_BLOCK_HOST location=%llx entry=%llx size=%llu bytes=%s\n",
                            location, static_cast<unsigned long long>(block->entry),
                            block->size, host.data());
            }
        }
    }
#endif
    std::printf("EDEN_GPU_UNMAPPED_COUNTERS read=%llu write=%llu guest_map_zero=%llu guest_null_mapped=%llu guest_alias_mapped=%llu guest_alias_access=%llu remap_replaced=%llu remap_mismatch=%llu bad_map_range=%llu bad_unmap_range=%llu bad_physical=%llu remap_cache_evictions=%llu\n",
                static_cast<unsigned long long>(::Eden::GpuFault::reads.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(::Eden::GpuFault::writes.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(::Eden::GpuFault::guest_map_zero.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(::Eden::GpuFault::guest_null_mapped.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(::Eden::GpuFault::guest_alias_mapped.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(::Eden::GpuFault::guest_alias_access.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(::Eden::GpuFault::remap_replaced.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(::Eden::GpuFault::remap_mismatch.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(::Eden::GpuFault::bad_map_range.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(::Eden::GpuFault::bad_unmap_range.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(::Eden::GpuFault::bad_physical.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(::Eden::GpuFault::remap_cache_evictions.load(std::memory_order_relaxed)));
    // Take an immutable owner-written copy; priority/CPU-clock syscalls,
    // formatting, and stdout flushes must NOT hold workers_mutex. Otherwise
    // all guest CPU workers can block in SampleCpu() on this GPU report.
    std::array<Worker, names.size()> worker_snapshot{};
    std::array<CpuSample, 4> cpu_snapshot{};
    {
        const std::lock_guard worker_guard(workers_mutex);
        worker_snapshot = workers;
        cpu_snapshot = cpu_samples;
    }
    for (unsigned i = 0; i < worker_snapshot.size(); ++i) {
        const auto& worker = worker_snapshot[i];
        sched_param priority{};
        int policy = -1;
        const int priority_error = worker.registered ?
            pthread_getschedparam(worker.thread, &policy, &priority) : ENOENT;
        const auto cpu = !cpu_clocks_valid ? -static_cast<long long>(ENOTSUP) :
                        worker.clock_error ? -static_cast<long long>(worker.clock_error) :
                                             ClockNs(worker.clock);
        std::printf("EDEN_PERF_WORKER mono_ns=%lld name=%s cpu_ns=%lld clock_id=%d affinity_error=%d "
                    "allowed=%d mask_low64=%llx priority_error=%d policy=%d priority=%d "
                    "registered_wall_ns=%lld registered_mono_ns=%lld\n",
                    mono, names[i], cpu, int(worker.clock), worker.affinity_error, worker.allowed, worker.mask,
                    priority_error, policy, priority.sched_priority, worker.wall_ns, worker.mono_ns);
        if (i < compilation.size()) {
            const auto& sample = cpu_snapshot[i];
            std::printf("EDEN_PERF_CPU_POINT core=%u phase=%u epoch=%u mono_ns=%lld cpu_ns=%lld thread=%llu pc=%llx svc=%x fpcr=%u compilations=%llu compile_ns=%llu\n",
                        i, unsigned(cpu_state[i].phase.load(std::memory_order_relaxed)), sample.epoch,
                        sample.mono_ns, sample.cpu_ns, sample.thread, sample.pc, sample.svc, sample.fpcr,
                        sample.compilations, sample.compile_ns);
#ifdef EDEN_DEV_PROFILE
            const auto& dups = jit_duplicates[i];
            std::printf("EDEN_PERF_DUPLICATES mono_ns=%lld core=%u first=%llu again=%llu lead_1ms=%llu lead_10ms=%llu "
                        "lead_100ms=%llu lead_1s=%llu lead_10s=%llu lead_more=%llu\n", mono, i,
                        dups[0].load(std::memory_order_relaxed), dups[1].load(std::memory_order_relaxed),
                        dups[2].load(std::memory_order_relaxed), dups[3].load(std::memory_order_relaxed),
                        dups[4].load(std::memory_order_relaxed), dups[5].load(std::memory_order_relaxed),
                        dups[6].load(std::memory_order_relaxed), dups[7].load(std::memory_order_relaxed));
            if (eden_jit_path_counters) {
                unsigned long long path[8]{};
                eden_jit_path_counters(i, path);
                std::printf("EDEN_PERF_JITPATH mono_ns=%lld core=%u runs=%llu thunks=%llu lookups=%llu hits=%llu "
                            "contended=%llu flush_all=%llu flush_locations=%llu far_links=%llu\n", mono, i,
                            path[0], path[1], path[2], path[3], path[4], path[5], path[6], path[7]);
            }
            std::printf("EDEN_PERF_PROGRESS mono_ns=%lld core=%u compilations=%llu compile_ns=%llu evacuations=%llu "
                        "translate_ns=%llu optimize_ns=%llu emit_ns=%llu ranges_ns=%llu\n",
                        mono, i, compilation[i].calls.load(std::memory_order_relaxed),
                        compilation[i].nanoseconds.load(std::memory_order_relaxed),
                        evacuations[i].load(std::memory_order_relaxed),
                        jit_phase_ns[i][0].load(std::memory_order_relaxed), jit_phase_ns[i][1].load(std::memory_order_relaxed),
                        jit_phase_ns[i][2].load(std::memory_order_relaxed), jit_phase_ns[i][3].load(std::memory_order_relaxed));
#else
            std::printf("EDEN_PERF_PROGRESS mono_ns=%lld core=%u compilations=%llu compile_ns=%llu evacuations=%llu\n",
                        mono, i, compilation[i].calls.load(std::memory_order_relaxed),
                        compilation[i].nanoseconds.load(std::memory_order_relaxed),
                        evacuations[i].load(std::memory_order_relaxed));
#endif
        }
    }
    std::fflush(stdout);
#ifdef EDEN_DEV_PROFILE
    // A late diagnostic cost measurement is not included in the reported
    // game's five-second frame interval; make this observer effect explicit.
    const long long snapshot_end = ClockNs(CLOCK_MONOTONIC);
    if (mono > 0 && snapshot_end >= mono)
        std::printf("EDEN_DEV_SNAPSHOT_COST mono_ns=%lld elapsed_ns=%lld\n",
                    mono, snapshot_end - mono);
#endif
}

#ifndef EDEN_DEV_PROFILE
// Release builds: the JIT's block hook (derived a32_interface.cpp) resolves here and does nothing,
// as does the shared JIT's compile-phase hook (headless/dynarmic/jit_impl.inc). Native packages
// cannot import an undefined weak symbol.
extern "C" void eden_jit_block(unsigned, unsigned long long, const void*, unsigned long long) {}
extern "C" void eden_jit_phases(unsigned, unsigned long long, unsigned long long, unsigned long long,
                                unsigned long long) {}
#endif

#ifdef EDEN_DEV_PROFILE
extern "C" void eden_jit_block(unsigned core, unsigned long long location, const void* entry,
                               unsigned long long size) {
    if (core != 0) return;
    if (!core0_blocks) core0_blocks = new JitBlock[jit_block_capacity];
    std::size_t count = core0_block_count.load(std::memory_order_relaxed);
    const auto address = reinterpret_cast<uintptr_t>(entry);
    if (count && address < core0_blocks[count - 1].entry) count = 0; // the cache was cleared
    if (count >= jit_block_capacity) return;
    core0_blocks[count] = JitBlock{address, size, location};
    core0_block_count.store(count + 1, std::memory_order_release);
}

void BeginPcSampling() {
    struct sigaction previous{}, action{};
    if (sigaction(SIGUSR2, nullptr, &previous) || previous.sa_handler != SIG_DFL)
        throw std::runtime_error("Development sampler signal already in use");
    action.sa_sigaction = PcSignal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR2, &action, nullptr))
        throw std::runtime_error("Cannot install development PC sampler");
    // Development process lifetime; polling stops before guest-worker shutdown.
    pc_sampling = true;
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
    std::printf("EDEN_PERF_CALLER_MODE enabled=1 firmware=6.02 depth=8 page=4096\n");
#endif
    std::printf("EDEN_PERF_PC_ANCHOR address=%llx capacity=%zu\n",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&PcSignal)), sampled_pcs.size());
    std::printf("EDEN_PERF_LIBC memcpy=%p memset=%p memmove=%p memcmp=%p\n",
                reinterpret_cast<void*>(&std::memcpy), reinterpret_cast<void*>(&std::memset),
                reinterpret_cast<void*>(&std::memmove), reinterpret_cast<void*>(&std::memcmp));
}

void PollGpuPc() {
    if (!pc_sampling) return;
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
    const bool gpu_capacity = pc_count.load(std::memory_order_acquire) < sampled_pcs.size();
#else
    const bool gpu_capacity = true; // bounded ring; retain recent samples
#endif
    // SampleCpu() needs workers_mutex to publish guest progress. Do not hold
    // it across a possibly blocking pthread_kill kernel call at 20 Hz.
    // The already-existing worker registration is process-lifetime; the
    // snapshot lock only protects copying its pthread_t, not OS liveness.
    pthread_t gpu_target{};
    bool gpu_ready = false;
    if (gpu_capacity) {
        const std::lock_guard lock(workers_mutex);
        if (workers[4].registered) {
            gpu_target = workers[4].thread;
            gpu_ready = true;
        }
    }
    if (gpu_ready && pthread_kill(gpu_target, SIGUSR2))
        throw std::runtime_error("Cannot sample development render thread");
    if (core_sample_ready.load(std::memory_order_acquire))
        pthread_kill(core_sample_thread, SIGUSR2);
}
#endif

extern "C" unsigned eden_heap_arenas_created(void) __attribute__((weak));

void AllocatorCheck() {
    std::printf("EDEN_PERF_HEAP arenas=%u\n", eden_heap_arenas_created ? eden_heap_arenas_created() : 1u);
    constexpr unsigned ops = 50000, live_slots = 1024;
    const auto run = [](unsigned seed) {
        std::vector<void*> live(live_slots, nullptr);
        unsigned x = seed;
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < ops; ++i) {
            x = x * 1664525u + 1013904223u;
            void*& slot = live[x % live_slots];
            std::free(slot);
            slot = std::malloc(16 + ((x >> 16) & 511));
        }
        const auto end = std::chrono::steady_clock::now();
        for (void* pointer : live) std::free(pointer);
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
    };
    std::printf("EDEN_PERF_ALLOC threads=1 pairs=%u ns_per_pair=%.1f\n", ops, run(1) / double(ops));
    for (unsigned threads : {2u, 4u, 8u}) {
        std::vector<long long> elapsed(threads);
        std::latch start{static_cast<std::ptrdiff_t>(threads)};
        std::vector<std::thread> pool;
        for (unsigned index = 0; index < threads; ++index)
            pool.emplace_back([&, index] { start.arrive_and_wait(); elapsed[index] = run(index + 7); });
        for (auto& thread : pool) thread.join();
        long long worst = 0, sum = 0;
        for (auto value : elapsed) { worst = std::max(worst, value); sum += value; }
        std::printf("EDEN_PERF_ALLOC threads=%u pairs=%u ns_per_pair_mean=%.1f ns_per_pair_worst=%.1f\n",
                    threads, ops, sum / double(threads) / ops, worst / double(ops));
    }
}

void PlatformChecks() {
#ifdef PS5_NATIVE
    CheckWorkerTopology();
#endif
    CheckCpuClocks();
#ifdef EDEN_DEV_PROFILE
    AllocatorCheck();
#endif
    using clock = std::chrono::steady_clock;
    const auto ns = [](auto start) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
    };
    std::printf("EDEN_PERF_CLOCK leaf_max=%x invariant=%d tsc_hz=%llu native=%d\n",
                __get_cpuid_max(0, nullptr), Common::g_cpu_caps.invariant_tsc,
                static_cast<unsigned long long>(Common::g_cpu_caps.tsc_frequency),
                Common::g_wall_clock.IsNative());
    unsigned long long sum = 0;
    auto start = clock::now();
    for (unsigned i = 0; i < 100'000; ++i) sum += Common::g_wall_clock.GetCNTPCT();
    std::printf("EDEN_PERF_CLOCK_READ calls=100000 elapsed_ns=%lld checksum=%llu\n",
                static_cast<long long>(ns(start)), sum);
    // Bound the diagnostic overhead comparison; per-block timers use the
    // already-qualified fenced counter, with the upstream steady-clock fallback.
    start = clock::now();
    for (unsigned i = 0; i < 1000; ++i) sum += clock::now().time_since_epoch().count();
    const auto system_clock_ns = ns(start);
    start = clock::now();
    for (unsigned i = 0; i < 1000; ++i) sum += Common::g_wall_clock.GetTimeNS().count();
    std::printf("EDEN_PERF_DIAGNOSTIC_CLOCK calls=1000 system_ns=%lld counter_ns=%lld checksum=%llu\n",
                static_cast<long long>(system_clock_ns), static_cast<long long>(ns(start)), sum);
    for (const unsigned requested_us : {1000u, 16667u, 50000u}) {
        const auto wall = Common::g_wall_clock.GetTimeNS();
        start = clock::now();
        std::this_thread::sleep_for(std::chrono::microseconds(requested_us));
        const auto mono_delta = ns(start);
        const auto wall_delta = (Common::g_wall_clock.GetTimeNS() - wall).count();
        if (mono_delta <= 0 || wall_delta <= 0) throw std::runtime_error("Nonmonotonic clock");
        if (std::abs(wall_delta - mono_delta) > std::max<decltype(mono_delta)>(2'000'000, mono_delta / 20))
            throw std::runtime_error("Guest clock disagrees with monotonic time");
        std::printf("EDEN_PERF_SLEEP requested_us=%u mono_ns=%lld wall_ns=%lld\n",
                    requested_us, static_cast<long long>(mono_delta), static_cast<long long>(wall_delta));
    }
    constexpr std::size_t bytes = 16 * 1024 * 1024;
    auto* memory = static_cast<unsigned long long*>(Common::AllocateMemoryPages(bytes));
    if (!memory) throw std::bad_alloc{};
    struct Release {
        void* memory;
        ~Release() { Common::FreeMemoryPages(memory, bytes); }
    } release{memory};
    constexpr std::size_t words = bytes / sizeof(*memory);
    for (std::size_t i = 0; i < words; ++i) memory[i] = i;
    const volatile auto* reads = memory;
    sum = 0;
    start = clock::now();
    for (unsigned repeat = 0; repeat < 4; ++repeat)
        for (std::size_t i = 0; i < words; ++i) sum += reads[i];
    if (sum != 4ULL * words * (words - 1) / 2) throw std::runtime_error("Memory read checksum");
    std::printf("EDEN_PERF_MEMORY pattern=stream bytes=%zu elapsed_ns=%lld checksum=%llu\n",
                bytes * 4, static_cast<long long>(ns(start)), sum);
    // Full-cycle LCG, one node per cache line. Compare a small warm working set
    // with the same dependent-load loop across the larger owned mapping.
    for (const std::size_t nodes : {std::size_t{512}, bytes / 64}) {
        for (std::size_t i = 0; i < nodes; ++i) memory[i * 8] = (i * 5 + 1) & (nodes - 1);
        std::size_t index = 0;
        constexpr unsigned steps = 1'048'576;
        start = clock::now();
        for (unsigned i = 0; i < steps; ++i) index = reads[index * 8];
        if (index != 0) throw std::runtime_error("Memory chain checksum");
        std::printf("EDEN_PERF_MEMORY pattern=dependent working_bytes=%zu steps=%u elapsed_ns=%lld checksum=%zu\n",
                    nodes * 64, steps, static_cast<long long>(ns(start)), index);
    }
    std::fflush(stdout);
}
} // namespace Eden::Performance

// dynarmic A64 hooks (weak there): whole-cache evacuations and block compilation time.
extern "C" void eden_jit_evacuation(unsigned core) {
    if (core < Eden::Performance::evacuations.size())
        Eden::Performance::evacuations[core].fetch_add(1, std::memory_order_relaxed);
}
#ifdef EDEN_DEV_PROFILE
// Development builds only (the per-phase and duplicate counters exist there).
extern "C" void eden_jit_phases(unsigned core, unsigned long long translate, unsigned long long optimize,
                                unsigned long long emit, unsigned long long location) {
    if (core >= Eden::Performance::jit_phase_ns.size()) return;
    auto& phases = Eden::Performance::jit_phase_ns[core];
    phases[0].fetch_add(translate, std::memory_order_relaxed);
    phases[1].fetch_add(optimize, std::memory_order_relaxed);
    phases[2].fetch_add(emit, std::memory_order_relaxed);
    if (!Eden::Performance::jit_duplicate_tracking.load(std::memory_order_relaxed)) return;
    // Which cores compiled each location, and when the first one did.
    struct First { unsigned cores; long long ns; };
    static std::mutex mutex;
    static auto* compiled = [] {
        auto* map = new std::unordered_map<unsigned long long, First>;
        map->reserve(1u << 21);
        return map;
    }();
    const long long now = Common::g_wall_clock.GetTimeNS().count();
    unsigned index = 0;
    {
        std::lock_guard lock{mutex};
        auto [it, inserted] = compiled->try_emplace(location, First{1u << core, now});
        if (!inserted) {
            if (it->second.cores & (1u << core)) {
                index = 1;
            } else {
                it->second.cores |= 1u << core;
                const long long lead = now - it->second.ns;
                index = 2;
                for (long long bound = 1'000'000; index < 7 && lead >= bound; bound *= 10) ++index;
            }
        }
    }
    Eden::Performance::jit_duplicates[core][index].fetch_add(1, std::memory_order_relaxed);
}
extern "C" void eden_jit_ranges(unsigned core, unsigned long long ns) {
    if (core < Eden::Performance::jit_phase_ns.size())
        Eden::Performance::jit_phase_ns[core][3].fetch_add(ns, std::memory_order_relaxed);
}
#endif
// The shared JIT's compile timers (headless/dynarmic/jit_impl.inc): the qualified invariant-TSC
// clock, about 26 ns per read against about 0.9 µs for the steady clock's system call.
extern "C" unsigned long long eden_jit_clock_ns() {
    return static_cast<unsigned long long>(Common::g_wall_clock.GetTimeNS().count());
}
extern "C" void eden_jit_compile(unsigned core, unsigned long long ns) {
    if (core >= Eden::Performance::compilation.size()) return;
    Eden::Performance::compilation[core].calls.fetch_add(1, std::memory_order_relaxed);
    Eden::Performance::compilation[core].nanoseconds.fetch_add(ns, std::memory_order_relaxed);
}
