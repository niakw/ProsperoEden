#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Quiet console logging: disk is crash-only; normal logs are opt-in.

Host C++ pipe test plus source contracts. Does not validate PS5 firmware ABI.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
read = lambda name: (root / name).read_text(encoding="utf-8")
pipe = read("headless/log_pipe.h")
main = read("headless/main.cpp")
boot = read("headless/boot_trace.h")
service = read("headless/prosperoeden/eden_services.cpp")
patch = read("headless/backports/eden-ps5-crash-only-logging.patch")
crash = read("headless/crash_report.cpp")

# All three routine sources (stdio, early trace, upstream Eden logging) must
# honor the same saved preference, including a change while in the launcher.
assert "const bool persist_detailed_logs = Eden::LoadPreferences().detailed_logging;" in main
assert 'persist_detailed_logs ? Eden::LogFile("stderr.log") : "/dev/null"' in main
assert 'persist_detailed_logs ? Eden::LogFile("heap.log") : "/dev/null"' in main
assert 'Eden::NativeLogs::Register(stderr_attached ? &stderr_pipe : nullptr,' in main
assert 'Eden::NativeLogs::SetDetailed(persist_detailed_logs);' in main
assert 'Eden::BootTrace::Quiet(Eden::LogsDir());' in main
assert 'Eden::BootTrace::Ready(Eden::LogsDir(), Eden::FilesystemAccess());' in main
assert main.index('const Eden::Crash::Last last_crash =') < main.index('if (!persist_detailed_logs) {')
assert main.index('Eden::Crash::Install(') < main.index('Eden::BootTrace::Quiet(Eden::LogsDir());')
assert 'std::remove(Eden::LogFile(name).c_str());' in main
assert '"result.tsv", "boot-trace.txt"' in main
assert '''if (persist_detailed_logs) {
            report = std::fopen(Eden::LogFile("result.tsv").c_str(), "w");''' in main
assert 'std::remove((Eden::UserDir() + "/log/" + name).c_str());' in main

assert 'Eden::NativeLogs::SetDetailed(value.detailed_logging);' in service
assert 'if (saved) {' in service.split('bool EdenServices::set_preferences(', 1)[1]
assert 'Eden::BootTrace::Quiet(Eden::LogsDir());' in service
assert "inline bool& QuietMode()" in boot
graphics = read("headless/graphics.cpp")
assert "if (Eden::NativeLogs::Detailed()) {" in graphics
assert "frame_pressure_previous = CaptureFramePressure();" in graphics
assert 'std::filesystem::exists(Eden::AppFile("frame-profile.txt"))' not in main
assert 'const bool deep_frame_profile = !performance_run && launch_preferences.detailed_logging;' in main
assert 'Eden::Performance::vulkan_cost_enabled = launch_preferences.detailed_logging &&' in main
assert 'const bool pc_sample_run = launch_preferences.detailed_logging &&' in main
assert 'if (detail::QuietMode()) return;' in boot
assert 'std::remove("/download0/boot-trace.txt");' in boot
assert 'std::remove("/download0/boot-trace.prev.txt");' in boot
assert 'detail::File() = -1;' in boot

# The upstream Eden file backend must not create or format routine logs in
# quiet mode. It lazily opens on enabling and reopens after an on/off cycle.
assert '+        if (!eden_native_detailed_logging()) return;' in patch
assert 'file.emplace(filename, FS::FileAccessMode::Write, FS::FileType::TextFile)' in patch
assert '+            file.reset();' in patch
assert 'eden_native_logging_generation()' in patch
assert 'if (file && eden_native_detailed_logging()) file->Flush();' in patch
assert 'void FmtLogMessageImpl(' in patch
assert 'extern "C" bool eden_native_detailed_logging() noexcept {' in main
assert 'eden-ps5-crash-only-logging.patch' in read("tools/apply-eden-backports.sh")
assert '.encore-backport-ps5-crash-only-logging.sha256' in read("tools/apply-eden-backports.sh")

# The fatal path is separately installed, not dependent on FILE stdout/stderr
# or the patched upstream log backend; normal crash reports are still retained.
assert 'const bool saved = WriteFile(report_path.data, report.data, report.size);' in crash
assert 'const std::string index_file = logs_folder + "/crash-index.txt";' in crash
assert 'Eden::Crash::Install(Eden::LogsDir()' in main
crash_header = read("headless/crash_report.h")
crash_source = read("headless/crash_report.cpp")
gpu_worker_generator = read("headless/CMakeLists.txt")
watchdog = read("headless/stall_watchdog.h")
assert 'inline std::atomic<std::uint64_t> gpu_completed_commands{0};' in crash_header
assert 'inline std::atomic<unsigned> gpu_stall_suspicions{0};' in crash_header
assert 'gpu_completed_commands.load(std::memory_order_relaxed)' in crash_source
assert 'gpu_stall_suspicions.load(std::memory_order_relaxed)' in crash_source
assert '(++completed_gpu_commands & 63u) == 0u' in gpu_worker_generator
assert 'gpu_completed_commands.fetch_add(64, std::memory_order_relaxed);' in gpu_worker_generator
assert 'Crash::gpu_completed_commands.load(std::memory_order_relaxed)' in watchdog
assert 'Crash::gpu_stall_suspicions.fetch_add(1, std::memory_order_relaxed)' in watchdog
assert 'if (!Performance::detailed_gpu_profile.load(std::memory_order_relaxed)) return;' in watchdog

source = r"""
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>
#include "log_pipe.h"

int main(int argc, char** argv) {
    assert(argc == 2);
    namespace fs = std::filesystem;
    const fs::path folder = argv[1];
    const auto recent = (folder / "heap.log").string();
    const auto first = (folder / "heap.first.log").string();
    std::FILE* out = std::fopen("/dev/null", "w");
    assert(out);
    std::setvbuf(out, nullptr, _IONBF, 0);
    {
        Eden::LogPipe stream;
        assert(stream.Attach(out, recent, first, 4096, false));
        for (int i=0;i<150;++i) std::fprintf(out, "QUIET-%d\n", i);
        std::fflush(out);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        assert(!fs::exists(recent) && !fs::exists(first));

        assert(stream.SetEnabled(true));
        for (int i=0;i<12;++i) std::fprintf(out, "DETAIL-%d\n", i);
        std::fflush(out);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        assert(fs::exists(recent));
        assert(fs::file_size(recent) > 0);

        assert(stream.SetEnabled(false));
        assert(!fs::exists(recent) && !fs::exists(first));
        for (int i=0;i<150;++i) std::fprintf(out, "QUIET-AGAIN-%d\n", i);
        std::fflush(out);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        assert(!fs::exists(recent) && !fs::exists(first));

        assert(stream.SetEnabled(true));
        std::fprintf(out, "AFTER-REENABLE\n");
        std::fflush(out);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        stream.Detach();
        assert(fs::exists(recent));
    }
    std::fclose(out);
    assert(!fs::exists(first));
    const auto size = fs::file_size(recent);
    assert(size > 0 && size < 4096);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="encore-crash-only-logging-") as tmp:
    work = Path(tmp)
    cpp = work / "quiet_logging.cpp"
    binary = work / "quiet_logging"
    cpp.write_text(source)
    cxx = shutil.which("clang++-18") or shutil.which("clang++")
    assert cxx, "C++ compiler required"
    subprocess.run([cxx, "-std=c++20", "-pthread", "-Wall", "-Wextra",
                    "-Werror", "-I", str(root / "headless"),
                    str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary), str(work)], check=True, timeout=15)

print("PASS: quiet stdout/stderr no files; live enable/disable/re-enable; crash reporter independent")
print("SOURCE/HOST ONLY: PS5 native binary, startup sequence and actual FPS still unverified")
