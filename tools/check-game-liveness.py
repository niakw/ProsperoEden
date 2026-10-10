#!/usr/bin/env python3
"""Compile portable developer in-game stall observer, guard PS5 wiring.

No game or SDK build. Detection is evidence-only: GPU counters can remain
constant while a game is legitimately paused. Never auto-reboot a title.
"""
from pathlib import Path
import platform
import sys
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "headless" / "stall_watchdog.h").read_text()
main = (ROOT / "headless" / "main.cpp").read_text()
assert "EDEN_GAME_GPU_STALL_SUSPECT" in source
assert "Crash::gpu_completed_commands.load" in source
assert "Crash::gpu_stall_suspicions.fetch_add(1, std::memory_order_relaxed);" in source
assert "if (!Performance::detailed_gpu_profile.load(std::memory_order_relaxed)) return;" in source
assert "Performance::rasterizer_draw.calls.load" in source
assert 'Crash::gpu_stall_suspicions.store(0, std::memory_order_relaxed);' in source
assert "game_probe.Observe(" in source
assert "game_armed.load(" in source
assert "game_session_epoch.fetch_add(1" in source
assert "epoch != previous_game_epoch" in source
assert "Eden::Stall::ArmGame();" in main
assert "Eden::Stall::DisarmGame();" in main
assert main.find("Eden::Stall::ArmGame();") > main.find("system.Run();")
assert "SCOPE_EXIT { Eden::Stall::DisarmGame(); }" in main

cxx = next((x for x in ("clang++-18", "clang++", "g++") if shutil.which(x)), None)
if not cxx:
    raise SystemExit("C++20 compiler required for developer GPU liveness gate")

fixture = r"""
#include "game_liveness.h"
#include <cassert>
#include <cstdint>
using namespace Eden::GameLiveness;
int main() {
    Probe no_gpu_counters;
    for (std::uint64_t seconds = 0; seconds < 600; ++seconds) {
        const auto observation = no_gpu_counters.Observe({0, 0}, seconds);
        assert(!observation.suspected);
        assert(!observation.established_gpu_progress);
    }
    // Must observe changes in the *current* game: prior GPU counters
    // may be nonzero due to the launcher or shader compilation.
    Probe from_launcher;
    for (std::uint64_t sec = 0; sec <= 60; ++sec)
        assert(!from_launcher.Observe({100, 200}, sec).suspected);

    Probe frozen;
    assert(!frozen.Observe({0, 0}, 0).suspected);
    assert(!frozen.Observe({1, 1}, 1).suspected);
    for (unsigned i = 1; i <= 180; ++i) {
        const auto info = frozen.Observe({1, 1}, i + 1);
        if (i == 30 || i == 60 || i == 90 || i == 120) {
            assert(info.suspected);
            assert(info.seconds_without_progress == i);
        } else {
            assert(!info.suspected);
        }
    }
    assert(!frozen.Observe({2, 2}, 200).suspected);
    // Reporting cap is four TOTAL for the session, even after recovery.
    for (unsigned sec = 201; sec < 350; ++sec)
        assert(!frozen.Observe({2, 2}, sec).suspected);

    // A new stall after a recovery gets its own 30-second clock,
    // *without* discarding the global report cap.
    Probe recovers;
    (void)recovers.Observe({1, 1}, 0);
    (void)recovers.Observe({2, 2}, 1);
    assert(recovers.Observe({2, 2}, 31).suspected);
    assert(!recovers.Observe({3, 3}, 32).suspected);
    assert(!recovers.Observe({3, 3}, 61).suspected);
    assert(recovers.Observe({3, 3}, 62).suspected);

    // Clock anomalies do not report a false freeze.
    Probe reset;
    (void)reset.Observe({0, 0}, 100);
    (void)reset.Observe({1, 1}, 101);
    assert(!reset.Observe({1, 1}, 0).suspected);
    assert(!reset.Observe({1, 1}, 1000).suspected);
    reset.Reset();
    assert(!reset.Observe({0, 0}, 999999).suspected);
}
"""
with tempfile.TemporaryDirectory(prefix="eden-game-liveness-host-") as folder:
    root = Path(folder)
    src, exe = root / "probe.cpp", root / "probe"
    src.write_text(fixture)
    subprocess.run([cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "headless"), str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    # Compile the actual PS5/DEV watchdog branch (without linking or running
    # proprietary SDK calls) against the REAL performance.h and diagnostics.h.
    # Only the pinned CPU-clock header absent from this repo is stubbed.
    clock = root / "common" / "cpu_features.h"
    clock.parent.mkdir()
    clock.write_text(
        "#pragma once\n#include <chrono>\n"
        "namespace Common {\n"
        "struct HostTestClock { std::chrono::nanoseconds GetTimeNS() const { return {}; } };\n"
        "inline HostTestClock g_wall_clock{};\n"
        "}\n"
    )
    native_src = root / "watchdog_native_header.cpp"
    native_obj = root / "watchdog_native_test"
    native_src.write_text(
        '#include "stall_watchdog.h"\n'
        'extern "C" unsigned long eden_heap_create_lock_state(unsigned* waiters) '
        '{ *waiters = 0; return 0; }\n'
        'int main() { '
        'Eden::Stall::ArmGame(); '
        'const unsigned first = Eden::Stall::game_session_epoch.load(); '
        'Eden::Stall::DisarmGame(); Eden::Stall::ArmGame(); '
        'if (Eden::Stall::game_session_epoch.load() != first + 1) return 2; '
        'Eden::Stall::DisarmGame(); return 0; }\n'
    )
    native_cmd = [cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                  "-pthread", "-DPS5_NATIVE=1", "-DEDEN_DEV_PROFILE=1",
                  "-I", str(root), "-I", str(ROOT / "headless")]
    # PS5 executes x86_64 instructions such as __builtin_ia32_pause.
    # macOS ARM64 can run the portable C++ policy but must cross-*compile*
    # this DEV/PS5 header instead of attempting to execute x86 guest code.
    mac_arm = sys.platform == "darwin" and platform.machine() in ("arm64", "aarch64")
    if mac_arm:
        subprocess.run([*native_cmd, "-target", "x86_64-apple-macos13.0",
                        "-c", str(native_src), "-o", str(root / "watchdog_native_x86.o")],
                       check=True)
    else:
        subprocess.run([*native_cmd, str(native_src), "-o", str(native_obj)], check=True)
        subprocess.run([str(native_obj)], check=True)

print("PASS developer-only in-game GPU liveness: 30s threshold, four reports, no counters -> no false alert")
if mac_arm:
    print("PASS real PS5/DEV watchdog header cross-compiles for x86_64 from ARM64 Mac (not executable here)")
else:
    print("PASS real PS5/DEV watchdog header host-links and runs session-epoch reset check")
print("PS5 native SDK/gameplay observation remains UNTESTED")
