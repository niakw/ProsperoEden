// SPDX-License-Identifier: GPL-3.0-or-later
// Log streams without storage stalls. On the console a small write to a file under /data takes
// ~25 ms or more (20 unbuffered lines: 474 ms), and the frontend writes stderr lines during game
// shutdown and flushes stdout from the GPU thread. Attach() points the stream's descriptor at a
// pipe; a background thread copies the pipe into the log file, so printing only waits for the
// pipe. Data still in the pipe when the process is killed is lost (milliseconds' worth).
#pragma once
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <thread>
#include <mutex>
#include <string>
#include <cstddef>
#include <unistd.h>

namespace Eden {
class LogPipe {
public:
    LogPipe() = default;
    LogPipe(const LogPipe&) = delete;
    LogPipe& operator=(const LogPipe&) = delete;
    ~LogPipe() { Detach(); }

    // The stream must already write to its log file; on failure it keeps doing so.
    // At segment_limit the first segment is preserved as first_path and logging continues in path.
    // If the second segment fills too, it is recycled so disk use stays bounded while the earliest
    // context and the latest messages are both retained.
    bool Attach(std::FILE* target, std::string path, std::string first_path,
                std::size_t segment_limit = 8u * 1024u * 1024u, bool initially_enabled = true) {
        if (stream) return false;
        std::fflush(target);
        const int stream_fd = fileno(target);
        int ends[2];
        // fcntl is a libkernel import; the SDK's static dup() is a direct syscall, which native
        // titles may not make.
        if (stream_fd < 0 || (file_fd = fcntl(stream_fd, F_DUPFD, 3)) < 0) return false;
        if (pipe(ends) != 0) {
            close(file_fd);
            file_fd = -1;
            return false;
        }
        if (dup2(ends[1], stream_fd) < 0) {
            close(ends[0]);
            close(ends[1]);
            close(file_fd);
            file_fd = -1;
            return false;
        }
        close(ends[1]);
        read_fd = ends[0];
        stream = target;
        log_path = std::move(path);
        first_log_path = std::move(first_path);
        limit = segment_limit;
        bytes = 0;
        rotated = false;
        enabled = initially_enabled;
        try {
            worker = std::thread([this] { Drain(); });
        } catch (...) {
            // dup2 already points the user's FILE stream at this pipe.
            // A thread-creation failure without restoring that descriptor
            // would leave stderr/stdout with no reader and freeze producers.
            (void)dup2(file_fd, stream_fd);
            close(read_fd);
            close(file_fd);
            read_fd = file_fd = -1;
            stream = nullptr;
            log_path.clear();
            first_log_path.clear();
            return false;
        }
        return true;
    }

    // Called from the launcher when Detailed Logging changes. The async reader
    // remains alive to prevent a pipe writer from blocking during gameplay.
    // In quiet mode its backing descriptor is /dev/null: no persistent files.
    bool SetEnabled(bool want_enabled) {
        if (!stream) return false;
        std::fflush(stream);
        std::scoped_lock lock{file_mutex};
        if (enabled == want_enabled) return true;
        const int next = want_enabled ?
            open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666) :
            open("/dev/null", O_WRONLY);
        if (next < 0) return false;
        close(file_fd);
        file_fd = next;
        enabled = want_enabled;
        rotated = false;
        bytes = 0;
        if (!want_enabled) {
            (void)std::remove(log_path.c_str());
            (void)std::remove(first_log_path.c_str());
        } else {
            (void)std::remove(first_log_path.c_str());
        }
        return true;
    }

    // Writes go straight to the file again once the pipe has been copied out.
    void Detach() {
        if (!stream) return;
        std::fflush(stream);
        // Rotations can replace file_fd on the draining thread. Serialize descriptor
        // handoff, then join and point the stream at the *final* current segment.
        // Without the lock a concurrent close/open could make dup2 see EBADF.
        {
            std::scoped_lock lock{file_mutex};
            (void)dup2(file_fd, fileno(stream));
        }
        worker.join();
        (void)dup2(file_fd, fileno(stream));
        close(read_fd);
        close(file_fd);
        read_fd = file_fd = -1;
        stream = nullptr;
    }

private:
    bool Rotate() {
        if (!limit || log_path.empty()) return true;
        if (!rotated) {
            // The descriptor remains valid after rename; open a fresh current file before closing it.
            (void)std::remove(first_log_path.c_str());
            if (std::rename(log_path.c_str(), first_log_path.c_str()) != 0) return false;
            const int next = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (next < 0) {
                // Restore the current path if the new segment could not be opened.
                (void)std::rename(first_log_path.c_str(), log_path.c_str());
                return false;
            }
            close(file_fd);
            file_fd = next;
            rotated = true;
        } else {
            // Keep the first segment, recycle only the current tail.
            // On PS5 ftruncate/lseek wrappers have inconsistent support:
            // always reopen with O_TRUNC before closing the old descriptor.
            // That guarantees the next write starts at byte zero.
            const int next = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (next < 0) return false;
            close(file_fd);
            file_fd = next;
        }
        bytes = 0;
        static constexpr char marker[] =
            "[Eden Encore] log segment rotated to keep storage bounded\n";
        const ssize_t wrote = write(file_fd, marker, sizeof(marker) - 1);
        if (wrote > 0) bytes = static_cast<std::size_t>(wrote);
        return true;
    }

    void Drain() {
        char buffer[16384];
        for (;;) {
            const ssize_t count = read(read_fd, buffer, sizeof(buffer));
            if (count == 0) return;
            if (count < 0) {
                if (errno == EINTR) continue;
                return;
            }
            std::scoped_lock lock{file_mutex};
            if (!enabled) continue;
            if (limit && bytes + static_cast<std::size_t>(count) > limit && !Rotate()) {
                // Do not fill the console disk on a transient filesystem error.
                // Keep draining the pipe (avoiding producer stalls) and retry on
                // the next chunk instead of permanently disabling the logger.
                continue;
            }
            for (ssize_t written = 0; written < count;) {
                const ssize_t result = write(file_fd, buffer + written, static_cast<size_t>(count - written));
                if (result < 0 && errno == EINTR) continue;
                if (result <= 0) break;
                written += result;
                bytes += static_cast<std::size_t>(result);
            }
        }
    }

    std::FILE* stream = nullptr;
    int read_fd = -1;
    int file_fd = -1;
    std::thread worker;
    std::mutex file_mutex; // guards worker rotation/write vs Detach descriptor handoff
    std::string log_path;
    std::string first_log_path;
    std::size_t limit = 0;
    std::size_t bytes = 0;
    bool rotated = false;
    bool enabled = true;
};

// The menu owns the two pipes for the entire process. Preferences may change
// while the launcher runs; no persistent traces are generated while disabled.
namespace NativeLogs {
inline std::atomic<bool> detailed{false};
inline std::atomic<unsigned> generation{0};
inline unsigned Generation() noexcept { return generation.load(std::memory_order_relaxed); }
inline LogPipe* error_pipe = nullptr;
inline LogPipe* output_pipe = nullptr;
inline bool Detailed() noexcept { return detailed.load(std::memory_order_relaxed); }
inline void Register(LogPipe* err, LogPipe* out) noexcept {
    error_pipe = err;
    output_pipe = out;
}
inline void SetDetailed(bool value) noexcept {
    if (detailed.exchange(value, std::memory_order_relaxed) != value)
        generation.fetch_add(1, std::memory_order_relaxed);
    if (error_pipe) (void)error_pipe->SetEnabled(value);
    if (output_pipe) (void)output_pipe->SetEnabled(value);
}
} // namespace NativeLogs
}
