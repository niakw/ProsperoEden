#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Quiet PS5 DEV gameplay must not do deep GPU/HLE work on its frame path.

Pure source regression check. A PS5 build and hardware A/B are still required.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
read = lambda path: (ROOT / path).read_text(encoding="utf-8")
main = read("headless/main.cpp")
graphics = read("headless/graphics.cpp")
header = read("headless/graphics.h")
perf = read("headless/performance.h")
cmake = read("headless/CMakeLists.txt")

# No settings option needed for the existing all-on JIT and CPU experiments:
# quiet/detailed controls only the expensive instrumentation.
assert "inline std::atomic<bool> detailed_gpu_profile{false};" in perf
assert "const bool deep_frame_profile = !performance_run &&" in main
assert "const bool deep_frame_profile = !performance_run && launch_preferences.detailed_logging;" in main
assert "const bool deep_frame_profile = !performance_run && launch_preferences.detailed_logging;" in main
assert 'std::filesystem::exists(Eden::AppFile("frame-profile.txt"))' not in main
assert "Eden::NativeLogs::Detailed()" in graphics
assert 'if (Eden::NativeLogs::Detailed()) {\n                // The game' in graphics
assert "detailed_gpu_profile.store(deep_frame_profile, std::memory_order_relaxed);" in main
assert "texture_budget_log.store(deep_frame_profile, std::memory_order_relaxed);" in main
assert "capture_passes.store(0, std::memory_order_relaxed);" in main
assert 'EDEN_FRAME_PROFILE detailed=%u mode=%s' in main
assert 'setenv("PS5VK_CAPTURE_SCANOUT", "1", 1);' in main  # not silently changed
assert "experimental_sparse_jit = true;" in main

# Full report traverses HLE counters and requests a CPU/guest snapshot.
# Preserve that diagnostic, but never call it from quiet GPU presentation.
present = graphics.split("void GraphicsWindow::OnFrameDisplayed()", 1)[1].split(
    "void GraphicsWindow::CheckPresentation(", 1)[0]
assert 'if (Eden::Performance::detailed_gpu_profile.load(std::memory_order_relaxed))\n                    Eden::Performance::ReportGpuThread(frame_total);' in present
assert present.index('EDEN_VULKAN_FRAME frames=') < present.index('EDEN_FRAME_PRESSURE frame=')
assert "const auto pressure_now = CaptureFramePressure();" in present
assert "frame_pressure_previous = CaptureFramePressure();" in present
assert "frame_pressure_previous = pressure_now;" in present
assert "if (pressure_now[i] < frame_pressure_previous[i])" in present
assert "counter" in present  # explicit reset handling; no bogus negative deltas
assert "std::this_thread::sleep" not in present
assert "frame_pressure_previous" in header
assert 'cache_wait_ms=%.3f jit_ms=%.3f' in graphics

# A fixed count of atomic loads at five-second boundaries; no new per-frame
# filesystem calls, mallocs, locks, kernel memory walks, or clock probes.
snapshot = graphics.split("FramePressureNumbers CaptureFramePressure()", 1)[1].split(
    "bool vulkan_loading{};", 1)[0]
for metric in ("gpu_queue_wait.nanoseconds", "gpu_dispatch.nanoseconds",
               "gpu_fence_drain.nanoseconds", "gpu_present_wait.nanoseconds",
               "gpu_queue_full.nanoseconds", "guest_dequeue_wait.nanoseconds",
               "guest_sync_wait.nanoseconds", "guest_ipc_wait.nanoseconds",
               "cache_lock_contended", "cache_lock_blocked", "cache_lock_wait_ns", "compilation"):
    assert metric in snapshot, metric
assert "std::memory_order_relaxed" in snapshot
assert "jit_ns += read(core.nanoseconds)" in snapshot
assert "std::printf(" not in snapshot
assert "NowNs()" not in snapshot
assert "mutex" not in snapshot and "malloc" not in snapshot
assert "std::array<unsigned long long, 12>" in header

# In the pinned CMake HLE-service injector, avoid TWO clock probes and the
# HleCounters atomic hash-table update for every quiet guest IPC request.
# Both normal and TIPC dispatches must still run exactly once per call.
injector = cmake.split('    foreach(hle_call "        InvokeRequest(ctx);"', 1)[1].split(
    "    write_derived(\"${PORT_BUILD_DIR}/hle_service.cpp\"", 1)[0]
assert 'const bool profile_hle = ::Eden::Performance::detailed_gpu_profile.load(std::memory_order_relaxed);' in injector
assert "const auto hle_start = profile_hle ? ::Eden::Performance::NowNs() : 0;" in injector
assert 'if (profile_hle)\\n                ::Eden::Performance::RecordHle(' in injector
assert '${hle_call}\\n            if (profile_hle)' in injector
assert 'foreach(hle_call "        InvokeRequest(ctx);" "            InvokeRequestTipc(ctx);")' in cmake
assert '::Eden::Performance::trace_fs_callers.load(std::memory_order_relaxed)' in injector
assert injector.count('::Eden::Performance::RecordHle(') == 3  # replacement + search + insertion

# OpenGL's one-off DEV pass dump must never trigger after 150 s by default,
# and its 150-second state belongs to each newly constructed context.
assert "double capture_start{-1};" in graphics
assert "bool captured_passes{};" in graphics
assert "static const double capture_start = now;" not in graphics
assert "detailed_gpu_profile.load(std::memory_order_relaxed) &&" in graphics
assert "!captured_passes) {" in graphics
assert "EDEN_DEV_PASS_REQUEST time=%.3f" in graphics

print("PASS: quiet GPU frame callback, HLE hot-path bypass, per-title counters")
print("SOURCE-ONLY: no native PS5 compilation or measured FPS improvement")
