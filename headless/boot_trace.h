// SPDX-License-Identifier: GPL-3.0-or-later
// Early startup trace for hardware failures before the normal crash/log stack is ready.
#pragma once

#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <unistd.h>

extern "C" int sceKernelDebugOutText(int, const char*);
extern "C" int sysctlbyname(const char*, void*, size_t*, const void*, size_t);

namespace Eden::BootTrace {
namespace detail {
inline std::string& Memory() {
    static std::string text;
    return text;
}
inline int& File() {
    static int fd = -1;
    return fd;
}
inline bool& QuietMode() {
    static bool muted = false;
    return muted;
}
inline timespec& Start() {
    static timespec value = [] {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        return now;
    }();
    return value;
}
inline long Milliseconds() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const timespec& begin = Start();
    return (now.tv_sec - begin.tv_sec) * 1000L + (now.tv_nsec - begin.tv_nsec) / 1000000L;
}
inline void WriteAll(int out, const char* data, size_t size) {
    while (out >= 0 && size) {
        const ssize_t wrote = write(out, data, size);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) break;
        data += wrote;
        size -= static_cast<size_t>(wrote);
    }
}
inline void Emit(const char* line) {
    (void)sceKernelDebugOutText(0, line);
}
inline void OnFatal(int signal, siginfo_t* info, void*) {
    char line[192];
    const int length = std::snprintf(
        line, sizeof(line),
        "[Prospero.Eden Encore diag] +%ldms FATAL signal=%d address=%p before normal crash handler\n",
        Milliseconds(), signal, info ? info->si_addr : nullptr);
    Emit(line);
    if (length > 0 && File() >= 0) {
        const size_t n = static_cast<size_t>(length) < sizeof(line) ?
            static_cast<size_t>(length) : sizeof(line) - 1;
        WriteAll(File(), line, n);
        (void)fsync(File());
    }
    std::signal(signal, SIG_DFL);
    (void)raise(signal);
}
} // namespace detail

inline void Line(const char* format, ...) {
    if (detail::QuietMode()) return;
    char body[700];
    va_list args;
    va_start(args, format);
    std::vsnprintf(body, sizeof(body), format, args);
    va_end(args);

    char line[820];
    const int length = std::snprintf(line, sizeof(line), "[Prospero.Eden Encore diag] +%ldms %s\n",
                                     detail::Milliseconds(), body);
    if (length <= 0) return;
    const size_t size = static_cast<size_t>(length) < sizeof(line) ?
        static_cast<size_t>(length) : sizeof(line) - 1;
    detail::Emit(line);
    detail::Memory().append(line, size);
    if (detail::File() >= 0) {
        detail::WriteAll(detail::File(), line, size);
        (void)fsync(detail::File());
    }
}

inline void Begin(const char* version, const char* build) {
    (void)detail::Start();

    struct sigaction action{};
    action.sa_sigaction = detail::OnFatal;
    action.sa_flags = SA_SIGINFO;
    for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
        (void)sigaction(signal, &action, nullptr);

    (void)std::rename("/download0/boot-trace.txt", "/download0/boot-trace.prev.txt");
    detail::File() = open("/download0/boot-trace.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);

    unsigned int sdk = 0;
    size_t size = sizeof(sdk);
    const bool known = sysctlbyname("kern.sdk_version", &sdk, &size, nullptr, 0) == 0;
    Line("start version=%s build=%s firmware=%s0x%08x pid=%d uid=%d/%d gid=%d/%d trace_fd=%d",
         version, build, known ? "" : "unknown ", sdk, static_cast<int>(getpid()),
         static_cast<int>(getuid()), static_cast<int>(geteuid()),
         static_cast<int>(getgid()), static_cast<int>(getegid()), detail::File());
}

// The startup bootstrap can write a trace before preferences are readable.
// Once crash handling is installed, quiet sessions do not retain those files.
// Fatal events still use the independent Crash::WriteReport path.
inline void Quiet(const std::string& logs_dir) {
    detail::QuietMode() = true;
    if (detail::File() >= 0) {
        close(detail::File());
        detail::File() = -1;
    }
    detail::Memory().clear();
    (void)std::remove("/download0/boot-trace.txt");
    (void)std::remove("/download0/boot-trace.prev.txt");
    for (const char* name : {"/boot-trace.txt", "/boot-trace.prev.txt",
                              "/boot-trace.sandbox-prev.txt"})
        (void)std::remove((logs_dir + name).c_str());
}

inline void Ready(const std::string& logs_dir, bool full_filesystem) {
    if (!full_filesystem) {
        Line("filesystem unavailable; trace remains in /download0");
        return;
    }

    const std::string path = logs_dir + "/boot-trace.txt";
    (void)std::rename(path.c_str(), (logs_dir + "/boot-trace.prev.txt").c_str());

    // Preserve the previous sandbox-only crash trace after access becomes available.
    if (int previous = open("/mnt/sandbox/PPSA99008_000/download0/boot-trace.prev.txt", O_RDONLY);
        previous >= 0) {
        const std::string copy_path = logs_dir + "/boot-trace.sandbox-prev.txt";
        const int copy = open(copy_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        char buffer[4096];
        for (ssize_t got; (got = read(previous, buffer, sizeof(buffer))) > 0;)
            detail::WriteAll(copy, buffer, static_cast<size_t>(got));
        if (copy >= 0) close(copy);
        close(previous);
    }

    const int out = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) {
        Line("cannot open %s: %s", path.c_str(), std::strerror(errno));
        return;
    }
    detail::WriteAll(out, detail::Memory().data(), detail::Memory().size());
    if (detail::File() >= 0) close(detail::File());
    detail::File() = out;
    (void)fsync(out);
    Line("trace moved to %s", path.c_str());
}
} // namespace Eden::BootTrace
