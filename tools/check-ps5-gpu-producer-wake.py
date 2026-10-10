#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise PS5 stop-aware SPSC producer with full queue, cancellation and FIFO.

The template method is the REAL one injected by the native CMake generator.
The host shim reproduces the pinned SPSC queue's indexes and CV protocols.
Native SDK compilation remains a separate PS5 build gate.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
inc = (root / "headless/gpu_queue_stop_wait.inc").read_text()
cmake = (root / "headless/CMakeLists.txt").read_text()
assert "EmplaceWaitWithStopToken" in inc
assert "producer.cv.wait(lock, stop_token" in inc
assert "consumer.index.load(std::memory_order_acquire)" in inc
assert "std::construct_at" in inc
assert "consumer.cv.notify_one()" in inc
assert "file(READ \"${EDEN_PORT_DIR}/gpu_queue_stop_wait.inc\"" in cmake
assert "write_derived(\"${PORT_BUILD_DIR}/include/common/bounded_threadsafe_queue.h\"" in cmake
assert "state.queue.TryEmplace(std::move(command_data), fence, block)" in cmake
assert "state.queue.EmplaceWaitWithStopToken(stop_source.get_token()," in cmake
assert "std::this_thread::sleep_for(std::chrono::microseconds(100));" not in cmake

cpp = r"""
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

template <typename T, size_t Capacity> class Queue {
    static_assert((Capacity & (Capacity - 1)) == 0);
public:
    template <typename... Args> bool TryEmplace(Args&&... args) noexcept {
        const auto write_index=producer.index.load(std::memory_order_relaxed);
        if (write_index-consumer.index.load(std::memory_order_acquire)==Capacity)
            return false;
        std::construct_at(std::addressof(m_data[write_index%Capacity]),std::forward<Args>(args)...);
        ++producer.index;
        std::scoped_lock lock{consumer.cv_mutex}; consumer.cv.notify_one();
        return true;
    }
    bool TryPop(T& target) noexcept {
        const auto read_index=consumer.index.load(std::memory_order_relaxed);
        if (read_index==producer.index.load(std::memory_order_acquire)) return false;
        target=std::move(m_data[read_index%Capacity]);
        ++consumer.index;
        std::scoped_lock lock{producer.cv_mutex}; producer.cv.notify_one();
        return true;
    }
STOP_CODE
private:
    std::array<T,Capacity> m_data{};
    alignas(64) struct {
        std::atomic_size_t index{0};
        std::condition_variable_any cv;
        std::mutex cv_mutex;
    } producer;
    alignas(64) struct {
        std::atomic_size_t index{0};
        std::condition_variable_any cv;
        std::mutex cv_mutex;
    } consumer;
};
int main() {
    using namespace std::chrono_literals;
    {
        Queue<int,2> q;
        assert(q.TryEmplace(1) && q.TryEmplace(2) && !q.TryEmplace(3));
        std::stop_source src;
        std::promise<bool> complete;
        auto result=complete.get_future();
        std::thread waiter([&] {complete.set_value(q.EmplaceWaitWithStopToken(src.get_token(),3));});
        assert(result.wait_for(20ms)==std::future_status::timeout);
        src.request_stop();
        assert(result.wait_for(1s)==std::future_status::ready && !result.get());
        waiter.join();
        int v=0; assert(q.TryPop(v) && v==1); assert(q.TryPop(v) && v==2);
        assert(!q.TryPop(v));
    }
    {
        Queue<int,2> q;
        assert(q.TryEmplace(11) && q.TryEmplace(12));
        std::stop_source src;
        std::promise<bool> complete; auto result=complete.get_future();
        std::thread waiter([&] {complete.set_value(q.EmplaceWaitWithStopToken(src.get_token(),13));});
        assert(result.wait_for(20ms)==std::future_status::timeout);
        int v=0; assert(q.TryPop(v) && v==11);
        assert(result.wait_for(1s)==std::future_status::ready && result.get());
        waiter.join();
        assert(q.TryPop(v) && v==12); assert(q.TryPop(v) && v==13);
        assert(!q.TryPop(v));
    }
    {
        Queue<int,8> q;
        std::stop_source src;
        constexpr int n=4000;
        std::thread producer([&] {
            for (int i=0;i<n;++i) {
                if (!q.TryEmplace(i))
                    assert(q.EmplaceWaitWithStopToken(src.get_token(),i));
            }
        });
        std::thread consumer([&] {
            for (int i=0;i<n;++i) {
                int value;
                while (!q.TryPop(value)) std::this_thread::yield();
                assert(value==i);
            }
        });
        producer.join(); consumer.join();
    }
    std::puts("PASS: stop token cancels full-queue wait; consumer wake preserves order; 4K commands");
}
""".replace("STOP_CODE", inc)
with tempfile.TemporaryDirectory(prefix="eden-ps5-gpu-cv-wake-") as tmp:
    source = Path(tmp) / "queue.cpp"
    exe = Path(tmp) / "queue"
    source.write_text(cpp)
    compiler = shutil.which("clang++-18") or shutil.which("clang++") or shutil.which("g++")
    assert compiler, "C++20 compiler missing"
    subprocess.run([compiler,"-std=c++20","-O1","-g","-pthread",
                    "-Wall","-Wextra","-Werror",
                    str(source),"-o",str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
print("HOST ONLY: console compiler/CMake generation/PS5 frame pacing not yet qualified")
