#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""PS5 GPU/guest hot paths must not pay DEV clocks/atomics in quiet gameplay.

Exercise the exact extracted lock/timer functions under host contention.
No PS5 SDK or actual FPS inference.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
read = lambda p: (root / p).read_text(encoding="utf-8")
perf = read("headless/performance.h")
cmake = read("headless/CMakeLists.txt")
generator = read("tools/prepare-vulkan-port.py")

assert "if (!detailed_gpu_profile.load(std::memory_order_relaxed)) {\n        mutex.lock();\n        return;\n    }" in perf
assert "class DiagnosticTimer" in perf
assert "if (totals) start = Common::g_wall_clock.GetTimeNS();" in perf
assert "if (!totals) return;" in perf
assert "std::chrono::nanoseconds start{};" in perf
for name in ("idle_timer", "dispatch_timer", "full_timer"):
    assert "::Eden::Performance::DiagnosticTimer " + name in cmake
    assert "::Eden::Performance::Timer " + name not in cmake
for name in ("guest_write_timer", "guest_read_timer", "drain_timer", "present_wait_timer"):
    assert "::Eden::Performance::DiagnosticTimer " + name in generator
    assert "::Eden::Performance::Timer " + name not in generator
assert "if (::Eden::Performance::detailed_gpu_profile.load(std::memory_order_relaxed))\\n" in generator
assert "::Eden::Performance::rasterizer_draw.calls.fetch_add(1, std::memory_order_relaxed)" in generator
assert "using CommandQueue = Common::SPSCQueue<CommandDataContainer, 8>;" in cmake
assert "gpu_command_tick, gpu_command_flush, gpu_command_invalidate" in perf
report = read("headless/performance.cpp")
assert 'EDEN_GPU_COMMANDS frame=%u tick_calls=%llu' in report
for kind in ("tick", "flush", "invalidate"):
    assert 'set(metric "gpu_command_' + kind + '")' in cmake
assert 'DiagnosticTimer category_timer(::Eden::Performance::${metric});' in cmake
assert 'if(gpu_call_at LESS 0)' in cmake
# No diagnostic timer may alter queue depth, the restart/stop semantics,
# Vulkan profile choices or GPU emulation results.
assert 'std::this_thread::sleep_for(std::chrono::microseconds(100));' not in cmake
assert 'EmplaceWaitWithStopToken(stop_source.get_token()' in cmake
# We have not changed the tested native queue algorithm or guest/cache locking order.
assert "state.queue.TryEmplace(std::move(command_data), fence, block)" in cmake
assert "std::lock_guard lock{texture_cache.mutex, std::adopt_lock};" in generator

lock = perf.split("template <typename Mutex>\ninline void GuestCacheLock(", 1)[1].split("// Guest waits:", 1)[0]
lock = "template <typename Mutex>\ninline void GuestCacheLock(" + lock
timer = perf.split("class DiagnosticTimer {", 1)[1].split("// Opt-in API wall times:", 1)[0]
timer = "class DiagnosticTimer {" + timer

fixture = r"""
#include <atomic>
#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdio>
struct Totals {
    std::atomic<unsigned long long> calls{}, nanoseconds{}, requested_bytes{};
};
namespace Common {
struct Clock {
    mutable std::atomic<unsigned long long> calls{};
    std::chrono::nanoseconds GetTimeNS() const {
        calls.fetch_add(1, std::memory_order_relaxed);
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch());
    }
};
inline Clock g_wall_clock{};
}
namespace Eden::Performance {
inline std::atomic<bool> detailed_gpu_profile{false};
inline std::atomic<unsigned> cache_lock_spins{0};
inline std::atomic<unsigned long long> cache_lock_contended{0}, cache_lock_blocked{0};
LOCK

TIMER
}

int main() {
    using namespace Eden::Performance;
    std::mutex mutex;
    unsigned long long value=0;
    // Verify the shipping quiet lock handles genuine contention, still owns
    // every critical section, and performs zero diagnostic cache-line updates.
    std::vector<std::thread> threads;
    for (int t=0;t<8;++t) {
        threads.emplace_back([&] {
            for (int j=0;j<20000;++j) {
                GuestCacheLock(mutex);
                std::lock_guard guard{mutex, std::adopt_lock};
                ++value;
            }
        });
    }
    for (auto& t:threads) t.join();
    assert(value==8*20000);
    assert(cache_lock_contended.load()==0 && cache_lock_blocked.load()==0);
    // No CPU clock reads or atomic Totals updates for all quiet timers.
    Totals total;
    for (int i=0;i<20000;++i) { DiagnosticTimer quiet{total}; }
    assert(total.calls.load()==0 && Common::g_wall_clock.calls.load()==0);
    // Deep diagnostic case preserves the existing accounting semantics.
    detailed_gpu_profile.store(true);
    { DiagnosticTimer active{total}; }
    assert(total.calls.load()==1 && Common::g_wall_clock.calls.load()==2);
    // Controlled, deterministic lock contention while diagnostics are enabled.
    mutex.lock();
    std::thread contended([&] {
        GuestCacheLock(mutex);
        std::lock_guard guard{mutex,std::adopt_lock};
        ++value;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    mutex.unlock();
    contended.join();
    assert(cache_lock_contended.load()>=1 && cache_lock_blocked.load()>=1);
    assert(value==8*20000+1);
    std::puts("PASS: quiet cache lock semantics and zero DEV timers; detailed counters preserved");
}
"""
fixture = fixture.replace("LOCK", lock).replace("TIMER", timer)
with tempfile.TemporaryDirectory(prefix="eden-ps5-quiet-gpu-hotpaths-") as folder:
    cpp=Path(folder)/"test.cpp"
    exe=Path(folder)/"test"
    cpp.write_text(fixture)
    cxx=shutil.which("clang++-18") or shutil.which("clang++")
    assert cxx, "clang++ required"
    subprocess.run([cxx,"-std=c++20","-O2","-pthread","-Wall","-Wextra","-Werror",
                    str(cpp),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=30)
print("SOURCE/HOST ONLY: real PS5 frame pacing still needs a compiled build and hardware test")
