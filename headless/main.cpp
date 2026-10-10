#ifdef EDEN_DEV_ROM_ID
#include "development_input.h"
#include "dev_replay_policy.h"
#include "dev_launch_policy.h"
#endif
#include <utility>
#include <vector>
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <algorithm>
#include <exception>
#include "assets_dir.h"
#include "gpu_failure.h"
#include "guest_fault.h"
#include "jit_list.h"
#ifdef PS5_NATIVE
#include "elevation/elevation.hpp"
#include "boot_trace.h"
#include "filesystem_probe.h"
#include <sys/stat.h>
#endif
#ifdef EDEN_DEV_VULKAN
#include "sdk_audit.h"
#endif
#include <filesystem>
#include <system_error>
#include <condition_variable>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <new>
#include <string_view>
#include <stdexcept>
#include <fstream>
#include <nlohmann/json.hpp>
#include "devices.h"
#include "encore_overrides_generated.h"
#include "encore_overrides_runtime.h"
#include "encore_performance_policy.h"
#include "experimental_performance.h"
#include "diagnostics.h"
#include "display_refresh.h"
#include "log_pipe.h"
#include "mods.h"
#include "glyph_overrides_runtime.h"
#include "controller_applet.h"
#include "error_applet.h"
#include "preferences.h"
#include "metadata_bridge.h"
#ifdef EDEN_PS5_OPENGL
#include "graphics.h"
#endif
#if defined(EDEN_PS5_OPENGL) || defined(EDEN_PS5_VULKAN)
#include "video_core/renderer_base.h"
#include "video_core/rasterizer_interface.h"
#endif
#ifdef PS5_NATIVE
// ResetTitleCounters is defined here, not in gpu_failure.h. Keep its
// declaration explicit so the next native build does not depend on an
// accidental transitive include from another frontend header.
#include "gpu_fault_rate_limit.h"
#include "native_directory.h"
#include "cache_budget.h"
#include "performance.h"
#include "stall_watchdog.h"
#include "stop_limit.h"
#include "dev_vulkan.h"
#include "../src/fastmem.h"
#include "crash_report.h"
#include "prosperoeden/frontend.h"
#include "prosperoeden/version.h"
extern "C" void ps5_opengl_heap_snapshot(const char*, unsigned);
extern "C" std::size_t eden_heap_release_current_tcache(void);
extern "C" void eden_heap_report_roots(const char* phase);
extern "C" std::int64_t sceKernelGetDirectMemorySize();
extern "C" int sceNetInit();
#else
#include <malloc.h>
#include "mock_devices.h"
#endif
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/file_sys/registered_cache.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/set/settings_types.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-private-field"
#include "core/hle/kernel/k_process.h"  // every build: the game's code address (jit_list.h)
#ifdef PS5_NATIVE
#include "core/hle/kernel/k_thread.h"
#endif
#pragma clang diagnostic pop
#if EDEN_SHARED_JIT_AVAILABLE
extern "C" bool eden_jit_shared;  // headless/dynarmic/jit_group_support.inc
#endif
#ifdef EDEN_DEV_PROFILE
#include "crash_trigger.h"
#include "watch.h"
#include "core/arm/debug.h"
#include "core/memory.h"
#endif
#include "video_core/gpu.h"
namespace Common {
bool SparseTablesAvailable() noexcept; // src/memory_pages.cpp
#ifdef PS5_NATIVE
bool ProbeSparseJitAlias() noexcept; // development-only validation of fixed RX/RW alias mapping
#endif
}

class HeadlessWindow final : public Core::Frontend::EmuWindow {
public:
    HeadlessWindow() { UpdateCurrentFramebufferLayout(1280, 720); }
    ~HeadlessWindow() override = default;
    bool IsShown() const override { return false; }
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<Core::Frontend::GraphicsContext>();
    }
};

#ifdef PS5_NATIVE
// First start with filesystem access: copy what the sandbox kept (settings, covers, Eden's saves
// and caches) into /data/prosperoeden. Only folders that do not exist yet are filled, and the
// sandbox copy stays, so an older ProsperoEden still finds its data.
static bool CopySandboxTree(const std::filesystem::path& from,
                            const std::filesystem::path& to,
                            std::error_code& error) {
    const auto root = std::filesystem::symlink_status(from, error);
    if (error) return false;
    if (std::filesystem::is_symlink(root) || !std::filesystem::is_directory(root)) {
        error = std::make_error_code(std::errc::operation_not_permitted);
        return false;
    }

    // Validate the whole source tree before copying anything with elevated filesystem access.
    for (std::filesystem::recursive_directory_iterator it{from, error}, end;
         !error && it != end; it.increment(error)) {
        const auto status = it->symlink_status(error);
        if (error) return false;
        if (std::filesystem::is_symlink(status) ||
            (!std::filesystem::is_directory(status) && !std::filesystem::is_regular_file(status))) {
            error = std::make_error_code(std::errc::operation_not_permitted);
            return false;
        }
    }
    if (error) return false;

    std::filesystem::create_directories(to, error);
    if (error) return false;
    for (std::filesystem::recursive_directory_iterator it{from, error}, end;
         !error && it != end; it.increment(error)) {
        const auto relative = it->path().lexically_relative(from);
        const auto destination = to / relative;
        const auto status = it->symlink_status(error);
        if (error) break;
        if (std::filesystem::is_directory(status)) {
            std::filesystem::create_directories(destination, error);
        } else {
            std::filesystem::create_directories(destination.parent_path(), error);
            if (!error)
                std::filesystem::copy_file(it->path(), destination,
                                           std::filesystem::copy_options::skip_existing, error);
        }
    }
    return !error;
}

static void MigrateLegacyInstallAssets() {
    const std::string saved = Eden::LoadStoredAssetsDirForMigration();
    const std::filesystem::path target{Eden::kDefaultAssetsDir};

    // A user who already selected another game-files folder has intentionally left the old
    // app-local assets behind. Do not move historical/duplicate data they are no longer using.
    if (!saved.empty() && !Eden::LegacyAppAssetsPath(saved))
        return;

    // Prefer the exact legacy alias recorded by ProsperoEden when it is still mounted. Otherwise
    // use the normal folder-install location. This also covers upgrades from ShadowMountPlus
    // mounts without assuming that /data/homebrew is the active backing source.
    std::filesystem::path legacy{Eden::kLegacyInstallAssetsDir};
    if (!saved.empty() && Eden::LegacyAppAssetsPath(saved) && Eden::DirectoryExists(saved))
        legacy = saved;

    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(legacy, error);
    if (error || !std::filesystem::exists(root_status)) {
        // v1.000.020+ already used /data/prosperoeden, but an older saved game-files choice can
        // still name the app-local assets folder. If the app was removed first, that path is gone:
        // repair the stale selection rather than leaving Encore pointed at nowhere.
        if (Eden::LegacyAppAssetsPath(saved)) {
            if (Eden::SaveAssetsDir(Eden::kDefaultAssetsDir))
                Eden::Report("data migration", "Legacy app assets path is gone; game files now use /data/prosperoeden");
            else
                Eden::Report("data migration", "Legacy app assets path is gone; could not update game-files setting");
        }
        return;
    }
    if (std::filesystem::is_symlink(root_status) || !std::filesystem::is_directory(root_status)) {
        Eden::Report("data migration", "Legacy app assets rejected: source is not a real directory");
        return;
    }

    using Move = std::pair<std::filesystem::path, std::filesystem::path>;
    std::vector<Move> plan;
    std::vector<std::filesystem::path> empty_destinations;

    // Preflight the complete migration before moving a single directory. Any conflict or unsafe
    // file type leaves the old layout untouched.
    for (const char* name : {"keys", "firmware", "roms", "updates", "mods",
                             "save-import", "ryujinx", "save-export"}) {
        const auto from = legacy / name;
        const auto to = target / name;

        error.clear();
        const auto status = std::filesystem::symlink_status(from, error);
        if (error || !std::filesystem::exists(status))
            continue;
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
            Eden::Report("data migration", (std::string{"Legacy "} + name + " rejected: unsafe file type").c_str());
            return;
        }

        error.clear();
        const auto destination = std::filesystem::symlink_status(to, error);
        if (!error && std::filesystem::exists(destination)) {
            if (std::filesystem::is_symlink(destination) || !std::filesystem::is_directory(destination)) {
                Eden::Report("data migration",
                             (std::string{"Legacy "} + name + " kept in place: destination is not a directory").c_str());
                return;
            }
            std::error_code empty_error;
            if (!std::filesystem::is_empty(to, empty_error) || empty_error) {
                Eden::Report("data migration",
                             (std::string{"Legacy "} + name + " kept in place: destination already has data").c_str());
                return;
            }
            empty_destinations.push_back(to);
        }
        plan.emplace_back(from, to);
    }

    if (plan.empty()) {
        if (saved.empty() || Eden::LegacyAppAssetsPath(saved))
            (void)Eden::SaveAssetsDir(Eden::kDefaultAssetsDir);
        return;
    }

    // rename() cannot create the destination parent. A clean 13.60 install may not have
    // /data/prosperoeden yet, so create only the persistent root after the whole migration plan
    // has passed preflight. If anything later fails, remove it again when it is still empty.
    bool created_target = false;
    error.clear();
    const auto target_status = std::filesystem::symlink_status(target, error);
    if (!error && std::filesystem::exists(target_status)) {
        if (std::filesystem::is_symlink(target_status) || !std::filesystem::is_directory(target_status)) {
            Eden::Report("data migration", "Legacy move aborted: persistent target is not a real directory");
            return;
        }
    } else {
        error.clear();
        if (!std::filesystem::create_directories(target, error) && error) {
            Eden::Report("data migration",
                         (std::string{"Legacy move aborted: could not create persistent target: "} +
                          error.message()).c_str());
            return;
        }
        created_target = true;
    }

    std::vector<Move> moved;
    auto rollback = [&] {
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::error_code rollback_error;
            std::filesystem::rename(it->second, it->first, rollback_error);
            if (rollback_error)
                Eden::Report("data migration",
                             (std::string{"Rollback failed for "} + it->first.string() + ": " +
                              rollback_error.message()).c_str());
        }
        for (const auto& directory : empty_destinations) {
            std::error_code restore_error;
            if (!std::filesystem::exists(directory, restore_error))
                std::filesystem::create_directories(directory, restore_error);
        }
        if (created_target) {
            std::error_code cleanup_error;
            if (std::filesystem::is_empty(target, cleanup_error) && !cleanup_error)
                std::filesystem::remove(target, cleanup_error);
        }
    };

    for (const auto& [from, to] : plan) {
        error.clear();
        if (std::filesystem::exists(to, error) && !error) {
            std::filesystem::remove(to, error); // preflight proved it is an empty directory
            if (error) {
                rollback();
                Eden::Report("data migration",
                             (std::string{"Legacy move aborted: could not clear empty destination "} +
                              to.string()).c_str());
                return;
            }
        }
        std::filesystem::rename(from, to, error);
        if (error) {
            rollback();
            Eden::Report("data migration",
                         (std::string{"Legacy move aborted: "} + from.string() + " -> " +
                          to.string() + ": " + error.message()).c_str());
            return;
        }
        moved.emplace_back(from, to);
    }

    if ((saved.empty() || Eden::LegacyAppAssetsPath(saved)) &&
        !Eden::SaveAssetsDir(Eden::kDefaultAssetsDir)) {
        rollback();
        Eden::Report("data migration", "Legacy move rolled back because the game-files setting could not be updated");
        return;
    }

    error.clear();
    if (std::filesystem::is_empty(legacy, error) && !error)
        std::filesystem::remove(legacy, error);
    Eden::Report("data migration", "Legacy ProsperoEden game files moved to /data/prosperoeden");
}

static void MigrateSandboxData() {
    const std::filesystem::path sandbox{"/mnt/sandbox/PPSA99008_000/download0"};
    const std::pair<std::filesystem::path, std::string> moves[] = {
        {sandbox / "eden-headless-g7/user", Eden::UserDir()},
        {sandbox / "prosperoeden/covers", Eden::CoversDir()},
    };
    for (const auto& [from, to] : moves) {
        std::error_code error;
        if (Eden::DirectoryExists(to) || !Eden::DirectoryExists(from.string())) continue;
        const bool copied = CopySandboxTree(from, to, error);
        const std::string detail = from.string() + " -> " + to +
            (!copied ? " rejected/failed: " + error.message() : "");
        Eden::Report("data migration", detail.c_str());
    }

    // Settings files (not the covers folder) go to config/. Never follow a sandbox symlink after
    // elevation: a pre-elevation file tree must not be able to redirect privileged reads.
    if (!Eden::DirectoryExists(Eden::ConfigDir()) && Eden::DirectoryExists((sandbox / "prosperoeden").string())) {
        std::error_code error;
        std::filesystem::create_directories(Eden::ConfigDir(), error);
        for (std::filesystem::directory_iterator it{sandbox / "prosperoeden", error}, end;
             !error && it != end; it.increment(error)) {
            const auto status = it->symlink_status(error);
            if (error) break;
            if (std::filesystem::is_symlink(status)) {
                error = std::make_error_code(std::errc::operation_not_permitted);
                break;
            }
            if (std::filesystem::is_regular_file(status))
                std::filesystem::copy_file(it->path(), std::filesystem::path{Eden::ConfigDir()} / it->path().filename(),
                                           std::filesystem::copy_options::skip_existing, error);
        }
        Eden::Report("data migration", error ? ("settings rejected/failed: " + error.message()).c_str() : "settings copied");
    }
}

static void PrepareStorageLayout() {
    if (!Eden::FilesystemAccess())
        return;
    const std::string storage_root = Eden::AssetsDir();
    if (storage_root != Eden::kDefaultAssetsDir && !Eden::DirectoryExists(storage_root)) {
        Eden::Report("storage", "Selected external storage root is unavailable; keeping it unchanged");
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(storage_root, error);
    if (error) {
        Eden::Report("storage", ("Cannot create storage root: " + error.message()).c_str());
        return;
    }
    for (const char* folder : {"keys", "firmware", "roms", "updates", "mods",
                               "save-import", "save-export", "ryujinx"}) {
        error.clear();
        std::filesystem::create_directories(std::filesystem::path{storage_root} / folder, error);
        if (error)
            Eden::Report("storage", (std::string{"Cannot create "} + folder + ": " + error.message()).c_str());
    }
}
#endif

// The pinned native Eden logger queries the same live preference as the
// stdout/stderr sinks. Crash::Install writes its own report regardless.
extern "C" bool eden_native_detailed_logging() noexcept {
    return Eden::NativeLogs::Detailed();
}
extern "C" unsigned eden_native_logging_generation() noexcept {
    return Eden::NativeLogs::Generation();
}

int main(int argc, char** argv) {
    try {
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
        Eden::Stall::Start();
#endif
        std::FILE* report = stdout;
        SCOPE_EXIT { if (report != stdout) std::fclose(report); };
        std::setvbuf(report, nullptr, _IONBF, 0);
#ifdef PS5_NATIVE
        Eden::BootTrace::Begin(Eden::kAppVersion, __DATE__ " " __TIME__);

        // One hardened one-shot filesystem request covers both the internal
        // /data/prosperoeden root and an optional external storage root.
        Eden::BootTrace::Line("requesting filesystem access");
        const uid_t uid_before = getuid();
        const uid_t euid_before = geteuid();
        const gid_t gid_before = getgid();
        const gid_t egid_before = getegid();
        const auto elevation_status = elevation::request(elevation::Capability::filesystem);
        const uid_t uid_after = getuid();
        const uid_t euid_after = geteuid();
        const gid_t gid_after = getgid();
        const gid_t egid_after = getegid();

        const bool identity_changed_on_failure =
            elevation_status != elevation::Status::ok &&
            (uid_after != uid_before || euid_after != euid_before ||
             gid_after != gid_before || egid_after != egid_before);
        if (elevation_status == elevation::Status::rollback_failed ||
            identity_changed_on_failure) {
            Eden::BootTrace::Line(
                "unsafe failed elevation state status=%d uid=%d/%d gid=%d/%d; terminating before filesystem use",
                static_cast<int>(elevation_status), static_cast<int>(uid_after),
                static_cast<int>(euid_after), static_cast<int>(gid_after),
                static_cast<int>(egid_after));
            std::_Exit(125);
        }

        // A successful protocol reply is not enough: prove the actual capability Encore uses.
        // This also creates /data/prosperoeden on a clean installation. Do not reject a valid
        // grant merely because a firmware reports credentials differently through get*id().
        bool filesystem_proven = false;
        int filesystem_status = static_cast<int>(elevation_status);
        if (elevation_status == elevation::Status::ok) {
            Eden::BootTrace::Line("filesystem protocol ok; proving writable persistent root");
            filesystem_proven = Eden::ProbeWritableRoot(Eden::kDataDir);
            if (!filesystem_proven)
                filesystem_status = static_cast<int>(elevation::Status::apply_failed);
        }
        Eden::FilesystemAccessStatus() = filesystem_status;
        Eden::BootTrace::Line("filesystem status=%d proof=%s uid=%d/%d gid=%d/%d",
                              Eden::FilesystemAccessStatus(), filesystem_proven ? "ok" : "no",
                              static_cast<int>(uid_after), static_cast<int>(euid_after),
                              static_cast<int>(gid_after), static_cast<int>(egid_after));
        if (Eden::FilesystemAccess()) MigrateSandboxData();
        for (const auto& folder : {Eden::UserDir(), Eden::ConfigDir(), Eden::CoversDir(), Eden::LogsDir()}) {
            std::error_code folder_error;
            std::filesystem::create_directories(folder, folder_error);
        }
        if (Eden::FilesystemAccess()) MigrateLegacyInstallAssets();
        PrepareStorageLayout();
        Eden::BootTrace::Line("persistent storage layout prepared");
        // Resolve the persisted setting AFTER the storage layout and before opening any
        // normal runtime log. A crash report is written independently of this toggle.
        const bool persist_detailed_logs = Eden::LoadPreferences().detailed_logging;
        Eden::NativeLogs::SetDetailed(persist_detailed_logs);
        // Keep the previous session's logs: a freeze is diagnosed after the app is reopened.
        for (const char* base : {"stderr", "heap"}) {
            const std::string current = Eden::LogFile(std::string{base} + ".log");
            const std::string first = Eden::LogFile(std::string{base} + ".first.log");
            const std::string previous = Eden::LogFile(std::string{base} + ".prev.log");
            const std::string previous_first = Eden::LogFile(std::string{base} + ".prev.first.log");
            (void)std::remove(previous.c_str());
            (void)std::remove(previous_first.c_str());
            (void)std::rename(current.c_str(), previous.c_str());
            (void)std::rename(first.c_str(), previous_first.c_str());
        }
        // A crash report the previous run left: that run's logs move beside it, and the launcher
        // says where it is (crash_report.h).
        const Eden::Crash::Last last_crash = Eden::Crash::TakeLast(Eden::LogsDir(), Eden::UserDir() + "/log/eden_log.txt");
        if (!persist_detailed_logs) {
            // TakeLast above has already archived any real crash evidence.
            // Discard leftovers from clean/older runs; never remove crash-*.
            for (const char* name : {
                    "stderr.log", "stderr.first.log", "stderr.prev.log", "stderr.prev.first.log",
                    "heap.log", "heap.first.log", "heap.prev.log", "heap.prev.first.log",
                    "result.tsv", "boot-trace.txt", "boot-trace.prev.txt", "boot-trace.sandbox-prev.txt"})
                (void)std::remove(Eden::LogFile(name).c_str());
            for (const char* name : {
                    "eden_log.txt", "eden_log.txt.first.txt", "eden_log.txt.old.txt"})
                (void)std::remove((Eden::UserDir() + "/log/" + name).c_str());
        }
        // Older Eden releases could leave a 100 MiB .old.txt behind. The current
        // first/recent segments are capped separately, but a stale legacy file can
        // survive upgrades and needlessly occupy storage. Give crash collection
        // first access, then discard *only* oversized obsolete backups; never touch
        // live logs, saves, shader caches, or the current game's diagnostics.
        {
            constexpr std::uintmax_t kLegacyBackupLimit = 32u * 1024u * 1024u;
            const std::string old_log = Eden::UserDir() + "/log/eden_log.txt.old.txt";
            // Direct stat/remove are already used by the validated PS5-native
            // storage layer. Avoid std::filesystem::file_size here: on firmware
            // 13.60, some C++ filesystem probes failed at launcher startup.
            struct stat info {};
            if (::stat(old_log.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
                info.st_size > 0 &&
                static_cast<std::uintmax_t>(info.st_size) > kLegacyBackupLimit) {
                if (std::remove(old_log.c_str()) == 0)
                    Eden::BootTrace::Line("legacy Eden log backup removed bytes=%llu",
                        static_cast<unsigned long long>(info.st_size));
                else
                    Eden::BootTrace::Line("legacy Eden log pruning failed");
            }
        }
#ifdef EDEN_DEV_PROFILE
        // UI inspection captures are disposable; keep saves, settings and shader caches.
        for (const char* name : {"ui-preview.bmp", "ui-main.bmp", "ui-nav.bmp",
                                 "ui-library.bmp", "ui-about.bmp"})
            std::filesystem::remove(std::filesystem::path{Eden::ConfigDir()} / name);
        for (const char* name : {"eden_log.txt", "eden_log.txt.old.txt"})
            std::filesystem::remove(std::filesystem::path{Eden::UserDir()} / "log" / name);
#endif
        // Quiet by default: stream to a pipe whose reader discards messages.
        // Crucially, do not even CREATE the normal log files in this mode.
        const std::string error_sink = persist_detailed_logs ? Eden::LogFile("stderr.log") : "/dev/null";
        const std::string output_sink = persist_detailed_logs ? Eden::LogFile("heap.log") : "/dev/null";
        if (!std::freopen(error_sink.c_str(), "w", stderr) ||
            !std::freopen(output_sink.c_str(), "w", stdout)) return 2;
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        // Batch SDK success traces; phase receipts still flush explicitly.
        static char stdout_buffer[64 * 1024];
        if (std::setvbuf(stdout, stdout_buffer, _IOFBF, sizeof(stdout_buffer)) != 0) return 2;
        // Console storage writes take ~25 ms each; background threads copy both streams to disk.
        static Eden::LogPipe stderr_pipe, stdout_pipe;
        const std::string stderr_path = Eden::LogFile("stderr.log");
        const std::string heap_path = Eden::LogFile("heap.log");
        static constexpr std::size_t kReleaseLogSegmentBytes = 8u * 1024u * 1024u;
        const bool stderr_attached = stderr_pipe.Attach(
            stderr, stderr_path, Eden::LogFile("stderr.first.log"),
            kReleaseLogSegmentBytes, persist_detailed_logs);
        const bool stdout_attached = stdout_pipe.Attach(
            stdout, heap_path, Eden::LogFile("heap.first.log"),
            kReleaseLogSegmentBytes, persist_detailed_logs);
        Eden::NativeLogs::Register(stderr_attached ? &stderr_pipe : nullptr,
                                   stdout_attached ? &stdout_pipe : nullptr);
        if (!stderr_attached || !stdout_attached)
            Eden::Report("logs", "Asynchronous log sink unavailable; crash reports remain active");
        Eden::Crash::Install(Eden::LogsDir(), Eden::kAppVersion, last_crash.restarted);
        if (persist_detailed_logs)
            Eden::BootTrace::Ready(Eden::LogsDir(), Eden::FilesystemAccess());
        else
            Eden::BootTrace::Quiet(Eden::LogsDir());
        Eden::BootTrace::Line("logs ready; app=%s data=%s", Eden::AppDir().c_str(), Eden::UserDir().c_str());
        // Host-side HTTPS (Nlib, remote override manifests) uses BSD sockets and the
        // Payload SDK resolver. Native titles must initialise libSceNet before those calls.
        // Network failure is non-fatal: the launcher remains fully usable from local caches.
        const int host_net_result = sceNetInit();
        Eden::BootTrace::Line("host network init result=%d", host_net_result);
        if (host_net_result != 0)
            Eden::Report("network", "Native host network initialization failed; online metadata is unavailable");
        const std::string stop_note = Eden::LogFile("stop-limit.txt");
        if (std::remove(stop_note.c_str()) == 0)
            Eden::Report("exit", "The previous game did not stop within ten seconds; Eden restarted safely");
        Eden::StopLimit::Start(stop_note);
        for (const char* candidate : {"/app0", Eden::kMountedAppDir, Eden::kInstallDir,
                                      "/mnt/sandbox/PPSA99008_000/app0"})
            Eden::BootTrace::Line("app candidate %s: %s", candidate,
                                  Eden::FileExists(std::string{candidate} + "/eboot.bin") ? "yes" : "no");
        std::set_new_handler([] {
            ps5_opengl_heap_snapshot("allocation_failure", 0);
            std::fflush(stdout);
            Eden::Report("allocation failure", "operator new: heap exhausted");
            throw std::bad_alloc{};
        });
        // Name the exception in klog, then the crash report (which starts the app again).
        std::set_terminate([] {
            const char* detail = "no active exception";
            if (const auto current = std::current_exception()) {
                try {
                    std::rethrow_exception(current);
                } catch (const std::exception& error) {
                    detail = error.what();
                } catch (...) {
                    detail = "non-standard exception";
                }
            }
            Eden::Report("terminate", detail);
            Eden::Crash::Fail(detail);
        });
        {
            const std::string access = "status=" + std::to_string(Eden::FilesystemAccessStatus()) +
                " app=" + Eden::AppDir() + " data=" + Eden::UserDir() + " game_files=" + Eden::AssetsDir();
            Eden::Report("filesystem access", access.c_str());
            // Keep RADV's shader cache in writable data, never under /app0. Elevated installs use
            // /data/prosperoeden; self-contained/sandboxed installs use /download0.
            const std::string cache_root = Eden::FilesystemAccess() ?
                std::string{Eden::kDataDir} + "/cache" : Eden::UserDir() + "/cache";
            const std::string cache = cache_root + "/radv";
            (void)mkdir(cache_root.c_str(), 0777);
            if (Eden::FilesystemAccess()) {
                const std::string before = Eden::AppFile("radv-shader-cache");
                if (!Eden::DirectoryExists(cache) && Eden::DirectoryExists(before) &&
                    std::rename(before.c_str(), cache.c_str()) != 0)
                    Eden::Report("cache", "The driver's shader cache could not move to the data folder; it starts empty");
            }
            (void)mkdir(cache.c_str(), 0777);
            setenv("MESA_SHADER_CACHE_DIR", cache.c_str(), 1);
            // The PS5 has writable game-data storage, not a desktop temp dir.
            // Keep compiled Vulkan/RADV shaders while space exists. The cache
            // ceiling follows *disk* headroom (not CPU/GPU RAM), so a large
            // installed library need not repeatedly rebuild evicted pipelines.
            // Query the cache's actual filesystem, not an optional external
            // game-assets drive. Do not use raw PS5 libc statfs("/user").
            std::error_code cache_space_error;
            const auto cache_space = std::filesystem::space(cache, cache_space_error);
            if (!cache_space_error && cache_space.available != std::uintmax_t(-1)) {
                constexpr std::uintmax_t mib = 1024ull * 1024ull;
                const auto max_cache_mib =
                    std::max<std::uintmax_t>(64, cache_space.available / (8 * mib));
                const auto cache_limit = std::to_string(max_cache_mib) + "M";
                (void)setenv("MESA_SHADER_CACHE_MAX_SIZE", cache_limit.c_str(), 1);
                std::printf("EDEN_RADV_DISK_CACHE limit_mib=%llu free_mib=%llu\n",
                            static_cast<unsigned long long>(max_cache_mib),
                            static_cast<unsigned long long>(cache_space.available / mib));
            } else {
                // Unknown disk availability: retain the already qualified 256 MiB limit.
                (void)setenv("MESA_SHADER_CACHE_MAX_SIZE", "256M", 1);
            }
        }
        // Whether Eden's large tables can be sparse on this console (src/memory_pages.cpp),
        // decided now: every session's log says it, with or without a game.
        (void)Common::SparseTablesAvailable();
        // The development TSV is routine telemetry, not a crash report.
        // Never create it when Detailed Logging is disabled.
        if (persist_detailed_logs) {
            report = std::fopen(Eden::LogFile("result.tsv").c_str(), "w");
            if (!report) { report = stdout; return 2; }
        }
        std::puts("[headless-startup] directories_ready");
#ifdef EDEN_DEV_VULKAN
        if (std::filesystem::exists(Eden::AppFile("sdk-audit.txt"))) {
            Eden::AuditSdk();
            std::puts("EDEN_SDK_AUDIT_COMPLETE");
            std::fflush(stdout);
            // Keep the sandbox mounted for the existing bounded FTP collector.
            std::this_thread::sleep_for(std::chrono::seconds(30));
            return 0;
        }
#endif
        const std::string user_dir = Eden::UserDir();
#ifdef EDEN_PS5_OPENGL
        const auto native_shader_cache = std::filesystem::path{user_dir} / "cache/native-opengl";
        if (setenv("PS5_GLTHREAD", "1", 1) != 0)
            throw std::runtime_error("Cannot configure GL worker");
#if defined(EDEN_DEV_PROFILE) && !defined(EDEN_DEV_VULKAN)
        Eden::Performance::BeginPcSampling();
        // A driver rebuild already invalidates these compiler records. Drop its
        // obsolete slots so repeated development builds fit the 256 MiB sandbox.
        std::string cached_runtime;
        std::ifstream(native_shader_cache / "development-runtime") >> cached_runtime;
        if (cached_runtime != EDEN_DEV_GL_VERSION) {
            std::filesystem::create_directories(native_shader_cache);
            std::error_code directory_error;
            size_t removed = 0;
            for (const auto& entry : Eden::ReadNativeDirectory(native_shader_cache, directory_error)) {
                const auto name = entry.path().filename().string();
                const auto dot = name.find(".bin");
                if ((dot == 3 || dot == 4) &&
                    name.find_first_not_of("0123456789abcdef", 0) == dot)
                    removed += std::filesystem::remove(entry.path());
            }
            if (directory_error) throw std::system_error(directory_error, "Read native compiler cache");
            std::ofstream(native_shader_cache / "development-runtime") << EDEN_DEV_GL_VERSION;
            std::printf("EDEN_DEV_CACHE removed_obsolete_files=%zu\n", size_t(removed));
        }
        for (unsigned i = 0; i < 8; ++i)
            std::filesystem::remove(std::filesystem::path{Eden::LogsDir()} /
                                    ("pass-" + std::to_string(i) + ".ppm"));
#endif
        std::error_code cache_error;
        std::filesystem::create_directories(native_shader_cache, cache_error);
        if (!cache_error && setenv("PS5_SHADER_CACHE_DIR", native_shader_cache.c_str(), 1) != 0)
            throw std::runtime_error("Cannot configure native shader cache");
#endif
        (void)argc;
        (void)argv;
        std::string launch_error;
        // The previous run crashed: the launcher says where its report is.
        if (!last_crash.report.empty()) launch_error = std::string{Eden::Crash::kNotice} + last_crash.report;
        // Guest faults are never silently retried: preserve the failure and return to the launcher.
#ifdef EDEN_DEV_VULKAN
        std::string recovery_mode;
        std::ifstream(Eden::AppFile("backend-recovery.txt")) >> recovery_mode;
        const bool check_backend_recovery = !recovery_mode.empty();
        bool recovery_opengl = false;
#endif
#if defined(EDEN_DEV_PROFILE) || defined(EDEN_DEV_ROM_ID)
        bool autoboot_pending = false;
        // Interactive launcher is always the startup default, including FC27
        // all-on diagnostic builds. A build's dev title is not permission to
        // bypass the library after the user closes or restarts the application.
#ifdef EDEN_DEV_ROM_ID
        std::string development_id = EDEN_DEV_ROM_ID;
#else
        std::string development_id = EDEN_DEV_PROFILE_TITLE;
#endif
        {
            Eden::DevLaunch::BootIntent boot;
            std::ifstream dev_settings(Eden::AppFile("dev-settings.txt"));
            for (std::string entry; dev_settings >> entry;) {
                if (entry.starts_with("rom=") && entry.size() == 20) development_id = entry.substr(4);
                boot.Observe(entry);
            }
            // Crash notices and an explicit launcher override always open
            // the library; only autoboot=on enables one unattended title boot.
            autoboot_pending = boot.ShouldAutoboot(!last_crash.report.empty());
            std::fprintf(stderr, "EDEN_DEV_BOOT mode=%s title=%s\n",
                         autoboot_pending ? "autoboot" : "launcher", development_id.c_str());
        }
#ifdef EDEN_DEV_ROM_ID
        {
            // A scripted input left by an earlier run is not for this process.
            std::ifstream left_behind(Eden::AppFile("compat-input.txt"));
            Eden::DevelopmentInput::IgnoreExisting(left_behind);
        }
#endif
#endif
        for (;;) {
#ifdef EDEN_PS5_OPENGL
        std::error_code trim_error;
        const auto cache_entries = Eden::ReadNativeDirectory(native_shader_cache, trim_error);
        if (!trim_error) {
            std::error_code free_space_error;
            const auto disk = std::filesystem::space(native_shader_cache, free_space_error);
            if (!free_space_error) {
                const auto removed = Eden::TrimShaderCache(
                    cache_entries, disk.available, disk.capacity);
                if (removed)
                    Eden::Report("cache", "Reclaimed old shader records because storage was low");
            }
        }
#endif
        std::string selected_game;
        Eden::Crash::SetSession("launcher", false);
#ifdef PS5_NATIVE
        // A Vulkan session at 120 Hz handed the output back at 60 Hz as it closed; the launcher
        // opens it once the display has had time to follow (display_refresh.h).
        if (Eden::Display::settle.exchange(false)) {
            Eden::Report("display", "Back to 60 Hz after a 120 Hz session");
            std::this_thread::sleep_for(std::chrono::seconds(Eden::Display::kSettleSeconds));
        }
#endif
#ifdef EDEN_DEV_VULKAN
        if (autoboot_pending && check_backend_recovery && !recovery_opengl && !launch_error.empty()) {
            recovery_opengl = true;
            Eden::Report("recovery check", "Starting OpenGL after Vulkan session failure");
        }
        const bool automatic_launch = autoboot_pending;
#endif
#if defined(EDEN_DEV_PROFILE) || defined(EDEN_DEV_ROM_ID)
        if (std::exchange(autoboot_pending, false)) {
        // Match the title ID in the file name, else in the ROM's own metadata; the game files
        // folder first, then the pre-1.000.020 assets folder that still holds other test titles.
        const auto development_title = std::strtoull(development_id.c_str(), nullptr, 16);
        for (const std::string& folder : {Eden::AssetsPath("roms"), std::string{"/data/assets/roms"}}) {
            std::error_code rom_error;
            for (const auto& entry : Eden::ReadNativeDirectory(folder, rom_error)) {
                const auto filename = entry.path().filename().string();
                const auto extension = entry.path().extension();
                if (extension != ".nsp" && extension != ".xci") continue;
                const auto path = folder + "/" + filename;
                if (filename.find(development_id) != std::string::npos ||
                    eden_game_title_id(path.c_str()) == development_title) {
                    selected_game = path;
                    break;
                }
            }
            if (!selected_game.empty()) break;
        }
        if (selected_game.empty())
            throw std::runtime_error("Development ROM not found");
        } else {
            Eden::BootTrace::Line("opening launcher");
            selected_game = SelectProsperoEdenGame(launch_error);
            Eden::BootTrace::Line("launcher closed: %s", selected_game.empty() ? "quit" : "game selected");
        }
#else
        Eden::BootTrace::Line("opening launcher");
        selected_game = SelectProsperoEdenGame(launch_error);
        Eden::BootTrace::Line("launcher closed: %s", selected_game.empty() ? "quit" : "game selected");
#endif
        if (selected_game.empty()) {
            Eden::Report("exit", "Launcher closed");
#ifdef EDEN_DEV_ROM_ID
            // Development runs end here (EDEN_DEV_QUIT): flush the logs and let the log pipes copy
            // them out. Returning asks the system to end the title (src/lifecycle.c); the C library's
            // own exit here made every development run end as an app crash.
            std::fflush(nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
#endif
            return 0;
        }
        try {
        launch_error.clear();
        const bool safe_launch = std::getenv("EDEN_SAFE_LAUNCH") != nullptr;
        if (safe_launch) unsetenv("EDEN_SAFE_LAUNCH");
        const std::uint64_t launch_title_id = eden_game_title_id(selected_game.c_str());
        const auto game_video = Eden::LoadGameSettings(launch_title_id);
        const auto launch_preferences = Eden::LoadPreferences();
        // PS5-wide code-memory policy, not title-specific or tiered A/B/C.
        // Query before constructing the emulated CPU. Only the active guest ISA
        // instantiates its planned code arenas; neither plan owns both at once.
        std::size_t jit_largest_free = 0;
        bool jit_memory_known = false;
#ifdef PS5_NATIVE
        jit_memory_known = Eden::Performance::QueryLargestDirectMemoryBlock(&jit_largest_free);
#endif
        auto jit_plan = Eden::Experimental::ChooseJitMemoryPlan(
            safe_launch, jit_memory_known, jit_largest_free);
        bool experimental_sparse_jit = false;
        bool experimental_logical_cpu = false;
        bool experimental_frame_probe = false;
#if defined(PS5_NATIVE) && defined(EDEN_DEV_PROFILE)
        // One test profile for all titles. These are automatically enabled by
        // the dedicated native DEV build, not by a settings-menu toggle or
        // stale experiments.json from an earlier A/B/C/D campaign.
        // Safe Launch is the only runtime opt-out. Sparse still requires
        // a successful real native RW/RX alias probe below.
        if (!safe_launch) {
#ifdef EDEN_SPARSE_JIT_DEV
            experimental_sparse_jit = true;
#endif
            experimental_logical_cpu = true;
            experimental_frame_probe = true;
        }
#endif
#if defined(PS5_NATIVE) && defined(EDEN_SPARSE_JIT_DEV)
        if (experimental_sparse_jit && !Common::ProbeSparseJitAlias()) {
            std::puts("EDEN_JIT_SPARSE disabled: native alias preflight failed; using dense JIT");
            experimental_sparse_jit = false;
        }
#endif
#if defined(PS5_NATIVE) && defined(EDEN_SPARSE_JIT_DEV) && defined(EDEN_DEV_PROFILE)
        // Native evidence: four consecutive starts went from 11,824 MiB to
        // 4,128 MiB *largest contiguous block*. The dense allocator must use
        // CURRENT headroom. Sparse only reserves virtual address space, so
        // preserve the greatest VERIFIED earlier headroom for virtual arena
        // sizing; actual JIT memory still commits in 2 MiB pages on demand.
        // This avoids shrinking from ~2.13 GiB VA to ~656 MiB VA after the
        // first return to library. No denser physical allocation is enabled.
        static std::size_t verified_sparse_peak_extent = 0;
        if (experimental_sparse_jit && jit_memory_known && !safe_launch) {
            verified_sparse_peak_extent = std::max(verified_sparse_peak_extent, jit_largest_free);
            jit_plan = Eden::Experimental::ChooseSparseVirtualJitPlan(
                safe_launch, jit_memory_known, jit_largest_free, verified_sparse_peak_extent);
            std::printf("EDEN_JIT_SPARSE_VA_PLAN current_largest_mib=%zu peak_seen_mib=%zu "
                        "a64_virtual_mib=%zu physical_policy=demand_pages\n",
                        jit_largest_free / Eden::Experimental::kMiB,
                        verified_sparse_peak_extent / Eden::Experimental::kMiB,
                        (std::size_t{jit_plan.a64[0]} + jit_plan.a64[1] +
                         jit_plan.a64[2] + jit_plan.a64[3]) / Eden::Experimental::kMiB);
        }
#endif
        Eden::Experimental::ApplyJitMemoryPlan(jit_plan);
        Eden::Experimental::sparse_jit_cache.store(experimental_sparse_jit, std::memory_order_relaxed);
        Eden::Experimental::vulkan_frame_probe.store(experimental_frame_probe, std::memory_order_relaxed);
#ifdef PS5_NATIVE
        std::printf("EDEN_PS5_JIT_POLICY scope=all_titles memory_known=%u free_mib=%zu "
                    "admission_budget_mib=%zu a64=%u,%u,%u a32=%u,%u,%u safe=%u sparse=%u\n",
                    unsigned(jit_memory_known), jit_largest_free / Eden::Experimental::kMiB,
                    jit_plan.admission_budget_bytes / Eden::Experimental::kMiB,
                    jit_plan.a64[0] / Eden::Experimental::kMiB, jit_plan.a64[1] / Eden::Experimental::kMiB,
                    jit_plan.a64[2] / Eden::Experimental::kMiB,
                    jit_plan.a32[0] / Eden::Experimental::kMiB, jit_plan.a32[1] / Eden::Experimental::kMiB,
                    jit_plan.a32[2] / Eden::Experimental::kMiB,
                    unsigned(safe_launch), unsigned(experimental_sparse_jit));
#endif
        const int global_authored_tier =
            launch_preferences.performance_profile >= 0 &&
            launch_preferences.performance_profile < Eden::EncoreOverrides::kAuthoredProfileCount ?
                launch_preferences.performance_profile : -1;
        const int game_authored_tier =
            game_video.performance_profile >= 0 &&
            game_video.performance_profile < Eden::EncoreOverrides::kAuthoredProfileCount ?
                game_video.performance_profile : -1;
        const int effective_authored_tier = game_authored_tier >= 0 ? game_authored_tier : global_authored_tier;
        const auto title_profile = Eden::EncoreOverridesRuntime::ProfileForTitle(
            launch_title_id, effective_authored_tier >= 0 ? effective_authored_tier : 1);
        const int profile_renderer = effective_authored_tier >= 0 ? title_profile.renderer :
            (launch_preferences.backend == Eden::GraphicsBackend::Vulkan ? 1 : 0);
        const int profile_output = effective_authored_tier >= 0 ? title_profile.output : launch_preferences.output;
        const int profile_resolution = effective_authored_tier >= 0 ? title_profile.resolution : launch_preferences.resolution;
        const int profile_filter = effective_authored_tier >= 0 ? title_profile.filter : launch_preferences.upscaling_filter;
        const int profile_fsr = effective_authored_tier >= 0 ? title_profile.fsr_sharpness : launch_preferences.fsr_sharpness;
        const int profile_aa = effective_authored_tier >= 0 ? title_profile.anti_aliasing : launch_preferences.anti_aliasing;
        const int profile_refresh = effective_authored_tier >= 0 ? title_profile.refresh : launch_preferences.refresh;
#ifdef EDEN_DEV_VULKAN
        // New game session: old Vulkan timestamp query pools are tied to a
        // destroyed VkDevice. Reset even when the dev probe is off now.
        Eden::DevVulkan::ResetForTitle();  // all developer Vulkan A/B switches
        Eden::DevVulkan::gpu_time_session.fetch_add(1, std::memory_order_release);
        Eden::Performance::vulkan_cost_enabled = launch_preferences.detailed_logging &&
            std::filesystem::exists(Eden::AppFile("cost-run.txt"));
        const bool performance_run = std::filesystem::exists(Eden::AppFile("performance-run.txt"));
#ifdef EDEN_DEV_PROFILE
        // R291: the full profiling snapshot scans HLE tables, captures guest
        // CPU state and writes many lines on the compositor/GPU thread.
        // Quiet gameplay must not do that work just because this is a DEV PKG.
        // The user setting is authoritative: development sentinel files
        // cannot re-enable deep captures while Detailed Logging is OFF.
        const bool deep_frame_profile = !performance_run && launch_preferences.detailed_logging;
        Eden::Performance::detailed_gpu_profile.store(deep_frame_profile, std::memory_order_relaxed);
        Eden::Performance::texture_budget_log.store(deep_frame_profile, std::memory_order_relaxed);
        Eden::Performance::capture_passes.store(0, std::memory_order_relaxed);
        std::printf("EDEN_FRAME_PROFILE detailed=%u mode=%s\n",
                    unsigned(deep_frame_profile), performance_run ? "performance_run" :
                    deep_frame_profile ? "diagnostic" : "quiet");
        // Optional 20 Hz GPU-thread PC samples. The handler must exist before the
        // GPU thread registers, which unblocks SIGUSR2 only while sampling is on.
        const bool pc_sample_run = launch_preferences.detailed_logging &&
            std::filesystem::exists(Eden::AppFile("pc-sample.txt"));
        static bool pc_sampler_installed = false;
        if (pc_sample_run && !pc_sampler_installed) {
            Eden::Performance::BeginPcSampling();
            pc_sampler_installed = true;
        }
#endif
        // A normal game session does not need synchronous driver debug
        // output, even in a native DEV build. The user may opt back in
        // with Detailed Logging; performance-run always remains quiet.
        const bool quiet_driver = performance_run || !launch_preferences.detailed_logging;
        setenv("PS5VK_QUIET_LOG", quiet_driver ? "1" : "0", 1);
        // Bounded development cost breakdowns; never enable per-draw tracing.
        setenv("PS5VK_COST_LOG", (launch_preferences.detailed_logging &&
            std::filesystem::exists(Eden::AppFile("cost-run.txt"))) ? "1" : "0", 1);
        if (performance_run) unsetenv("PS5VK_CAPTURE_SCANOUT");
        else setenv("PS5VK_CAPTURE_SCANOUT", "1", 1);
        std::printf("EDEN_VULKAN_MEASUREMENT quiet=%d captures=%d\n",
                    quiet_driver, !performance_run);
        const auto backend = safe_launch ? Eden::GraphicsBackend::OpenGL : automatic_launch ?
            (recovery_opengl ? Eden::GraphicsBackend::OpenGL : Eden::GraphicsBackend::Vulkan) :
            game_video.renderer >= 0 ?
            (game_video.renderer == 0 ? Eden::GraphicsBackend::OpenGL : Eden::GraphicsBackend::Vulkan) :
            (profile_renderer == 0 ? Eden::GraphicsBackend::OpenGL : Eden::GraphicsBackend::Vulkan);
#else
        // Manual game settings win over title-specific Encore profile, then global settings.
        const auto backend = safe_launch ? Eden::GraphicsBackend::OpenGL :
            game_video.renderer >= 0 ?
            (game_video.renderer == 0 ? Eden::GraphicsBackend::OpenGL : Eden::GraphicsBackend::Vulkan) :
            (profile_renderer == 0 ? Eden::GraphicsBackend::OpenGL : Eden::GraphicsBackend::Vulkan);
#ifdef EDEN_PS5_VULKAN
        // The user-facing diagnostics option controls Eden logs, not synchronous
        // driver traces for every draw. Keep those in explicit development probes.
        setenv("PS5VK_QUIET_LOG", "1", 1);
#endif
#endif
        // Safe Launch lowers rendering cost but keeps conservative accuracy. It is a recovery
        // path, not the aggressive Minimum performance tier.
        const int effective_performance_profile = safe_launch ? 1 :
            game_video.performance_profile >= 0 ? game_video.performance_profile :
                                                  launch_preferences.performance_profile;
        const int effective_resolution_for_tuning = safe_launch ? Eden::kNativeResolution :
            (game_video.resolution >= 0 ? game_video.resolution : profile_resolution);
        const int effective_output_for_tuning = safe_launch ? 0 :
            (game_video.output >= 0 ? game_video.output : profile_output);
        int runtime_performance_profile = effective_performance_profile;
        if (runtime_performance_profile < 0 ||
            runtime_performance_profile >= Eden::EncoreOverrides::kAuthoredProfileCount) {
            // Custom means one or more visible knobs differ from an authored preset. Do not silently
            // turn a 1x/1080p custom setup back into the heavy High runtime policy. Derive the hidden
            // GPU policy from the effective render/output cost instead.
            if (effective_resolution_for_tuning <= Eden::kNativeResolution &&
                effective_output_for_tuning <= 0)
                runtime_performance_profile = 0;
            else if (effective_resolution_for_tuning <= Eden::kNativeResolution + 1 &&
                     effective_output_for_tuning <= 1)
                runtime_performance_profile = 1;
            else if (effective_resolution_for_tuning <= Eden::kNativeResolution + 2)
                runtime_performance_profile = 2;
            else
                runtime_performance_profile = 3;
        }
#ifndef EDEN_PS5_VULKAN
        if (backend == Eden::GraphicsBackend::Vulkan)
            throw std::runtime_error("Vulkan is not available in this build yet. Select OpenGL in Settings > Video to play.");
#endif
        Eden::Report("launch", Eden::BackendName(backend));
#ifdef PS5_NATIVE
        // The full diagnostic enumerates up to 8192 Sony regions at EACH
        // lifecycle checkpoint. Opt in once per title with this marker;
        // leaving it absent keeps the largest-free and owner accounting but
        // eliminates the slow, unused synchronous kernel walk.
        Eden::Performance::SetDirectMemoryRegionScanEnabled(
            std::filesystem::exists(Eden::AppFile("memory-region-scan.txt")));
#endif
        if (safe_launch) Eden::Report("launch", "Safe launch active: OpenGL, Handheld, 1x, Bilinear, 60 Hz, 1080p, mods off");
        const bool game = std::filesystem::is_regular_file(selected_game);
        if (!game) throw std::runtime_error("Selected ROM is no longer available");
        const char* guest = selected_game.c_str();
        const unsigned cycles = game ? 1 : 3;
        const bool devices = EDEN_DEVICE_FRONTEND;
        if (game) {
            for (const char* capture : {"game-frame.ppm", "scanout.ppm", "game-hud.ppm", "game-hud-steady.ppm",
                                        "game-hud-late.ppm"})
                std::filesystem::remove(std::filesystem::path{Eden::LogsDir()} / capture);
            // Firmware, keys, games, updates and mods all resolve from the selected storage root.
            std::puts("EDEN_GAME_ASSETS_READY");
        }
        constexpr bool cpu_pressure = false;
        constexpr bool shutdown_sweep = false;
#else
        const char* guest = argc >= 2 ? argv[1] : nullptr;
        unsigned cycles = 1;
        bool devices = false;
        bool game = false;
        bool cpu_pressure = false;
        bool shutdown_sweep = false;
        for (int i = 2; i < argc; ++i) {
            if (std::string_view{argv[i]} == "--repeat" && cycles == 1) cycles = 3;
            else if (std::string_view{argv[i]} == "--soak" && cycles == 1) cycles = 20;
            else if (std::string_view{argv[i]} == "--devices" && !devices) devices = true;
            else if (std::string_view{argv[i]} == "--game" && !game) game = true;
            else if (std::string_view{argv[i]} == "--cpu-pressure" && !cpu_pressure) cpu_pressure = true;
            else if (std::string_view{argv[i]} == "--shutdown-sweep" && !shutdown_sweep) shutdown_sweep = true;
            else return 2;
        }
        // Device mock assertions describe the homebrew fixture, not arbitrary games.
        if (game && devices) return 2;
        if (cpu_pressure && (game || devices || cycles == 20)) return 2;
        if (shutdown_sweep) {
            if (game || devices || cpu_pressure || cycles != 1) return 2;
            game = true;
            cycles = 6;
        }
        const char* user_dir = "user";
#endif
        const auto passed = [&](const char* name) {
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
            Eden::Stall::Progress(name);
#endif
            std::fprintf(report, "%s\tPASS\n", name);
#ifdef PS5_NATIVE
            const auto mono_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            std::printf("EDEN_STAGE backend=%s phase=%s mono_ns=%lld\n",
                        Eden::BackendName(backend), name, static_cast<long long>(mono_ns));
            ps5_opengl_heap_snapshot(name, 0);
            // These are lifecycle points, not render-frame callbacks.
            if (std::string_view{name} == "core_initialized" ||
                std::string_view{name} == "core_shutdown" ||
                std::string_view{name} == "core_destroyed")
                Eden::Performance::ReportDirectMemoryState(name);
            // core_initialized occurs BEFORE actual guest JIT construction.
            // Attribute code ownership to loaded/CPU-ready stages instead of
            // mistaking its expected zero counts for missing emulator RAM.
            if (std::string_view{name} == "game_loaded" ||
                std::string_view{name} == "nro_loaded" ||
                std::string_view{name} == "cpu_manager_ready" ||
                std::string_view{name} == "core_shutdown")
                Eden::Performance::ReportJitCodeState(name);
            std::fflush(stdout);
#else
            const auto heap = mallinfo2();
            std::fprintf(stderr, "host_heap phase=%s allocated_bytes=%zu arena_bytes=%zu mapped_bytes=%zu free_bytes=%zu\n",
                         name, heap.uordblks + heap.hblkhd, heap.arena, heap.hblkhd, heap.fordblks);
#endif
        };
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
        Eden::Stall::Arm();
#endif
        passed("session_start");
        if (!std::filesystem::is_directory(user_dir)) {
            std::fputs("Run from the isolated directory containing user/.\n", stderr);
            return 2;
        }
        Common::FS::CreateEdenPaths();
#ifdef PS5_NATIVE
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, Eden::AssetsPath("keys"));
        // Mods come from the game files folder's mods/ (mods.h). Without that folder Eden's own
        // load folder stays, where nothing is expected.
        if (const std::string mods = Eden::AssetsPath("mods"); Eden::DirectoryExists(mods))
            Common::FS::SetEdenPath(Common::FS::EdenPath::LoadDir, mods);
#endif
#ifdef PS5_NATIVE
        if (Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) != user_dir) return 2;
        std::puts("[headless-startup] absolute_paths_ready");
#endif
#ifdef PS5_NATIVE
        if (game) {
            Eden::Performance::PlatformChecks();
            if (experimental_logical_cpu)
                Eden::Performance::EnableExperimentalLogicalPlacement();
        }
#endif
        Common::Log::Initialize();
        if (game) {
            Common::Log::Filter filter;
            filter.SetClassLevel(Common::Log::Class::Service_FS, Common::Log::Level::Info);
#if defined(PS5_NATIVE) && !defined(EDEN_DEV_PROFILE) && !defined(EDEN_DEV_ROM_ID)
            // Never enable global Debug logging in a shipping game. FC27 measured roughly
            // 1,600 lines/s and two rotating 100 MiB logs with this preference enabled.
            if (Eden::LoadPreferences().detailed_logging)
                std::fputs("[ProsperoEden] detailed gameplay logging requires a diagnostic build; release logging unchanged\n", stderr);
#endif
            Common::Log::SetGlobalFilter(filter);
        }
        Common::Log::Start();
        SCOPE_EXIT { Common::Log::Stop(); };
        // Reset every mutable speed/accuracy control for each session. One app process can launch
        // many games; no game's hidden runtime policy may leak into the next one.
        Settings::values.cpu_accuracy = Settings::CpuAccuracy::Auto;
        Settings::values.dma_accuracy.SetValue(Settings::DmaAccuracy::Default);
        Settings::values.use_reactive_flushing.SetValue(true);
        Settings::values.skip_cpu_inner_invalidation.SetValue(false);
#if defined(PS5_NATIVE) && defined(EDEN_DEV_PROFILE)
        // R237 recovered ~20% of contended cache locks with 8 PAUSE retries,
        // but its matched 7-minute FC27 frame trace was no faster than R236
        // (28.04 vs 28.13 FPS, more >50ms late frames). Do not burn guest CPU
        // cycles speculatively by default. Keep a bounded cache_spin=N A/B
        // override (max 32) for future test runs and reset it each title.
        Eden::Performance::cache_lock_spins.store(0, std::memory_order_relaxed);
#endif
#ifdef PS5_NATIVE
        // Host HTTPS (launcher Nlib/catalog) remains available, but emulated
        // Switch games are offline-only. The pinned HLE backport separately
        // rejects guest DNS/socket calls even if a guest toggles NIFM Wi-Fi.
        Settings::values.airplane_mode.SetValue(true);
        const auto performance_policy = Eden::EncorePerformance::ForTier(runtime_performance_profile);
#endif
        #ifdef EDEN_PS5_OPENGL
        Settings::values.renderer_backend = backend == Eden::GraphicsBackend::Vulkan ?
            Settings::RendererBackend::Vulkan : Settings::RendererBackend::OpenGL_GLSL;
        // Keep emulator threads off guest cores 0-2 and their SMT siblings (Vulkan):
        // +17% in the heaviest measured window of a racing game.
        Eden::Performance::SetSecondaryPlacement(backend == Eden::GraphicsBackend::Vulkan);
        // A32 fastmem window (direct JIT accesses to aliased guest memory): opt-in with dev-settings
        // fastmem=on; on the console it is slower than the page-table path.
        Eden::Fastmem::Request(false);
#ifdef EDEN_DEV_VULKAN
        Settings::values.async_presentation = backend == Eden::GraphicsBackend::Vulkan;
        // Exercise upstream's checked initialization error, never an invalid pointer.
        Settings::values.vulkan_device = recovery_mode == "init-failure" && !recovery_opengl ?
            0xffffffffu : 0u;
#endif
        Settings::values.use_asynchronous_shaders =
            performance_policy.async_shaders && backend == Eden::GraphicsBackend::Vulkan;
        Settings::values.renderer_debug = false;
        const auto profile_gpu_accuracy = performance_policy.fast_gpu ?
            Settings::GpuAccuracy::Low : Settings::GpuAccuracy::High;
        Settings::values.gpu_accuracy.SetValue(profile_gpu_accuracy);
        Settings::values.current_gpu_accuracy = profile_gpu_accuracy;
        if (performance_policy.unsafe_cpu)
            Settings::values.cpu_accuracy = Settings::CpuAccuracy::Unsafe;
        if (performance_policy.unsafe_dma)
            Settings::values.dma_accuracy.SetValue(Settings::DmaAccuracy::Unsafe);
        Settings::values.use_reactive_flushing.SetValue(performance_policy.reactive_flushing);
        Settings::values.skip_cpu_inner_invalidation.SetValue(performance_policy.skip_invalidation);
#if EDEN_SHARED_JIT_AVAILABLE
        Eden::JitList::enabled = performance_policy.compile_ahead;
#else
        // Shipping stability architecture: the saved-block worker depends on the experimental
        // shared JIT, so compile-ahead stays structurally unavailable rather than pretending to
        // honour a switch that cannot run.
        Eden::JitList::enabled = false;
#endif
#if !EDEN_SHARED_JIT_AVAILABLE && !EDEN_JIT_COMPILE_BATCH_AVAILABLE
        Eden::Report("performance",
            "CPU JIT: Dynarmic per-core; Encore cross-core sharing, successor batching and saved-block compile-ahead disabled");
#endif
        const int profile_label_index = std::clamp(
            effective_performance_profile, 0, Eden::EncoreOverrides::kCustomProfile);
        const auto on = [](bool value) { return value ? "on" : "off"; };
        Eden::Report("performance",
            (std::string("Profile ") + Eden::kPerformanceProfileLabels[profile_label_index] +
             (profile_label_index == Eden::EncoreOverrides::kCustomProfile ?
                 std::string(" (runtime ") + Eden::kPerformanceProfileLabels[runtime_performance_profile] + ")" :
                 std::string{}) +
             ": compile-ahead " + on(Eden::JitList::enabled.load(std::memory_order_relaxed)) +
             ", async shaders " + on(Settings::values.use_asynchronous_shaders.GetValue()) +
             ", fast GPU " + on(performance_policy.fast_gpu) +
             ", unsafe CPU " + on(performance_policy.unsafe_cpu) +
             ", unsafe DMA " + on(performance_policy.unsafe_dma) +
             ", reactive flushing " + on(performance_policy.reactive_flushing) +
             ", skip invalidation " + on(performance_policy.skip_invalidation)).c_str());
        // RADV presents the console's 12 GiB of direct memory as an integrated GPU, for which
        // Eden budgets 4 GiB: a game using ~4.4 GB of Vulkan memory then ran the texture GC
        // every frame (20-25 FPS). Eden's larger integrated budget (6 GiB) holds it at 30 FPS
        // with ~0.9 GB of direct memory to spare.
        Settings::values.vram_usage_mode.SetValue(Settings::VramUsageMode::Aggressive);
#else
        Settings::values.renderer_backend = Settings::RendererBackend::Null;
#endif
        Settings::values.memory_layout_mode = Settings::MemoryLayout::Memory_4Gb;
#ifdef EDEN_DEV_PROFILE
        // A one-game A/B override must NEVER silently become the next game's
        // effective performance policy. Reset the other development-only
        // globals before reading the current game's dev-settings.txt.
        Eden::Performance::graphics_usage_from_pool.store(true, std::memory_order_relaxed);
        Eden::Performance::gc_keep_dirty.store(true, std::memory_order_relaxed);
        Eden::Performance::dispatch_mask.store(7, std::memory_order_relaxed);
        Eden::Performance::idle_spin_iterations.store(5000, std::memory_order_relaxed);
        Eden::Performance::jit_duplicate_tracking.store(false, std::memory_order_relaxed);
        Eden::Performance::pc_sample_core.store(0, std::memory_order_relaxed);
        Eden::Performance::pc_fast.store(false, std::memory_order_relaxed);
        Eden::Performance::capture_early.store(false, std::memory_order_relaxed);
        Eden::Performance::firmware_applets.store(false, std::memory_order_relaxed);
        Eden::Performance::trace_fs_callers.store(false, std::memory_order_relaxed);
        // One-run A/B switches written by the development runner; absent = defaults.
        {
            std::ifstream dev_settings(Eden::AppFile("dev-settings.txt"));
            for (std::string entry; dev_settings >> entry;) {
                if (entry == "dma_accuracy=unsafe") {
                    Settings::values.dma_accuracy.SetValue(Settings::DmaAccuracy::Unsafe);
                } else if (entry == "placement=off") {
                    Eden::Performance::SetSecondaryPlacement(false);
                } else if (entry == "fastmem=off" || entry == "fastmem=on") {
                    Eden::Fastmem::Request(entry.ends_with("on"));
                } else if (entry.starts_with("cache_spin=")) {
                    // try_lock retries before a guest core sleeps on a GPU cache lock (0 = upstream).
                    const auto requested = std::strtoul(entry.c_str() + 11, nullptr, 10);
                    // Keep test overrides bounded: unlimited spinning can starve
                    // the native GPU worker under the three busy guest cores.
                    Eden::Performance::cache_lock_spins.store(
                        static_cast<unsigned>(std::min(requested, 32ul)), std::memory_order_relaxed);
                } else if (entry == "gc_dirty=upstream") {
                    // Eden's rule: evict GPU-written textures from the "expected" memory mark on.
                    Eden::Performance::gc_keep_dirty = false;
                } else if (entry.starts_with("dispatch_draws=")) {
                    // Draws between Vulkan worker hand-offs: a power of two from 8 (upstream) to 512.
                    const unsigned long draws = std::strtoul(entry.c_str() + 15, nullptr, 10);
                    if (draws >= 8 && draws <= 512 && (draws & (draws - 1)) == 0)
                        Eden::Performance::dispatch_mask = static_cast<unsigned>(draws - 1);
                } else if (entry.starts_with("idle_spin_us=")) {
                    // Guest cores spin this long on their interrupt flag before sleeping (W1).
                    Eden::Performance::idle_spin_iterations = static_cast<unsigned>(std::stoul(entry.substr(13)) * 50);
                } else if (entry.starts_with("pc_core=") && entry.size() == 9 && entry[8] >= '0' && entry[8] <= '3') {
                    // Host PC samples from this guest core instead of core 0 (with --pc-sample).
                    Eden::Performance::pc_sample_core = static_cast<unsigned>(entry[8] - '0');
                } else if (entry == "pc_fast=on") {
                    Eden::Performance::pc_fast = true;
                } else if (entry == "capture=early") {
                    Eden::Performance::capture_early = true;
                } else if (entry == "jit_dups=on") {
                    // Which cores compiled each A64 block, and when (EDEN_PERF_DUPLICATES).
                    Eden::Performance::jit_duplicate_tracking = true;
                } else if (entry == "applets=firmware") {
                    // Eden's default library applets: several run from the firmware as guest programs.
                    Eden::Performance::firmware_applets = true;
                } else if (entry == "jit_list=on" || entry == "jit_list=off") {
                    // The saved block list (jit_list.h): off unless asked for.
                    Eden::JitList::enabled = entry.ends_with("on");
                } else if (entry == "jit_shared=off") {
                    // Development A/B only. Production does not compile the cross-core JitGroup.
#if EDEN_SHARED_JIT_AVAILABLE
                    eden_jit_shared = false;
#endif
                } else if (entry == "cpu_accuracy=unsafe") {
                    // Dynarmic's unsafe FP shortcuts on top of Auto (reduced-error estimates, inaccurate NaN).
                    Settings::values.cpu_accuracy = Settings::CpuAccuracy::Unsafe;
                } else if (entry == "replay=off") {
                    // The profile title takes controller and runner input instead of the timed replay.
                } else if (entry == "large_pages=off" || entry == "sparse_tables=off" || entry == "heap=whole") {
                    // Read directly by the page allocator (src/memory_pages.cpp) before this parse:
                    // 16 KiB pages only; Eden's large tables dense; the heap's 3 GiB taken at start.
                } else if (entry == "graphics_usage=driver") {
                    // The caches' "memory in use" as the driver counts it (performance.h).
                    Eden::Performance::graphics_usage_from_pool = false;
                } else if (entry == "gpu_accuracy=low") {
                    // Nothing calls UpdateGPUAccuracy() here; set the live value too.
                    Settings::values.gpu_accuracy.SetValue(Settings::GpuAccuracy::Low);
                    Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::Low;
                } else if (entry == "null_descriptor=off") {
                    Eden::DevVulkan::disable_null_descriptor = true;
                } else if (entry == "descriptor_buffer=off") {
                    Eden::DevVulkan::disable_descriptor_buffer = true;
                } else if (entry == "robustness2=on") {
                    Eden::DevVulkan::robustness2 = true;
                } else if (entry == "vertex_input_dynamic=off") {
                    Settings::values.vertex_input_dynamic_state.SetValue(false);
                } else if (entry == "pipeline_trace=on") {
                    Eden::DevVulkan::trace_pipelines = true;
                } else if (entry.starts_with("radv_debug=")) {
                    // Read by RADV at instance creation, which happens after this parse.
                    setenv("RADV_DEBUG", entry.c_str() + 11, 1);
                } else if (entry.starts_with("env=") && entry.find('=', 4) != std::string::npos) {
                    // Other driver switches read at device creation, e.g. env=RADV_PS5_BORDER_REBIND=1.
                    const auto split = entry.find('=', 4);
                    setenv(entry.substr(4, split - 4).c_str(), entry.c_str() + split + 1, 1);
                } else if (entry == "sparse=off") {
                    Eden::DevVulkan::disable_sparse = true;
                } else if (entry == "multirange=off") {
                    Eden::DevVulkan::disable_multi_range = true;
                } else if (entry == "custom_border=off") {
                    Eden::DevVulkan::disable_custom_border = true;
                } else if (entry == "custom_border=on") {
                    Eden::DevVulkan::disable_custom_border = false;
                } else if (entry == "vram=aggressive") {
                    // Eden's own larger integrated-GPU budget (6 GiB instead of 4 GiB), the default.
                    Settings::values.vram_usage_mode.SetValue(Settings::VramUsageMode::Aggressive);
                } else if (entry == "vram=conservative") {
                    Settings::values.vram_usage_mode.SetValue(Settings::VramUsageMode::Conservative);
                } else if (entry == "conditional_rendering=off") {
                    Eden::DevVulkan::disable_conditional_rendering = true;
                } else if (entry == "conditional_rendering=on") {
                    Eden::DevVulkan::disable_conditional_rendering = false;
                } else if (entry == "gpu_time=on") {
                    Eden::DevVulkan::gpu_time = true;
                } else if (entry == "submit_sync=on") {
                    Eden::DevVulkan::sync_submissions = true;
                } else if (entry == "astc=cpu") {
                    Settings::values.accelerate_astc.SetValue(Settings::AstcDecodeMode::Cpu);
                } else if (entry == "log=render") {
                    // Renderer debug logging for crash probes (fetched every few seconds).
                    Common::Log::Filter filter;
                    filter.ParseFilterString("*:Info Render:Debug Render.Vulkan:Debug HW.GPU:Debug Shader:Debug");
                    Common::Log::SetGlobalFilter(filter);
                } else if (entry.starts_with("boot_trace=") && entry.find(':') != std::string::npos) {
                    // Individual guest GPU submissions/dispatches from START to END ms after now.
                    const auto colon = entry.find(':');
                    const long long now = Eden::Performance::NowNs();
                    Eden::Performance::boot_trace_begin = now + std::stoll(entry.substr(11, colon - 11)) * 1000000;
                    Eden::Performance::boot_trace_end = now + std::stoll(entry.substr(colon + 1)) * 1000000;
                } else if (entry == "hcr=eden" || entry == "hcr=exact" || entry == "hcr=cpu") {
                    Eden::DevVulkan::hcr_mode = entry == "hcr=eden" ? 0 : entry == "hcr=exact" ? 1 : 2;
                } else if (entry == "compute_barriers=off") {
                    Eden::DevVulkan::compute_barriers = false;
                } else if (entry == "gpu_clock=normal" || entry == "gpu_clock=boost" || entry == "gpu_clock=overclock") {
                    // Guest-visible GPU timestamp scale (Eden's fast_gpu_time; Boost is the default).
                    Settings::values.gpu_clock.SetValue(entry.ends_with("normal") ? Settings::GpuClock::Normal :
                        entry.ends_with("boost") ? Settings::GpuClock::Boost : Settings::GpuClock::Overclock);
                } else if (entry.starts_with("rom=")) {
                    // Development title override, applied when the title is selected.
                } else if (entry == "fs_callers=on") {
                    Eden::Performance::trace_fs_callers = true;
                } else if (entry.starts_with("log_filter=")) {
                    // Any Eden log filter for crash probes, '+' for spaces,
                    // e.g. log_filter=*:Info+Service:Debug+Service.HID:Info.
                    std::string text = entry.substr(11);
                    std::replace(text.begin(), text.end(), '+', ' ');
                    Common::Log::Filter filter;
                    filter.ParseFilterString(text);
                    Common::Log::SetGlobalFilter(filter);
                } else if (Eden::Watch::Parse(entry)) {
                    // Write watch / code dump on the main module, armed after load (watch.h).
                } else if (entry.starts_with("dyna_state=") && entry.size() == 12 &&
                           entry[11] >= '0' && entry[11] <= '3') {
                    Settings::values.dyna_state.SetValue(
                        static_cast<Settings::ExtendedDynamicState>(entry[11] - '0'));
                } else {
                    std::printf("EDEN_DEV_SETTINGS unknown=%s\n", entry.c_str());
                }
            }
            std::printf("EDEN_DEV_SETTINGS dma_accuracy=%u gpu_accuracy=%u null_descriptor=%u "
                        "descriptor_buffer=%u robustness2=%u vertex_input_dynamic=%u dyna_state=%u "
                        "sparse=%u multirange=%u custom_border=%u submit_sync=%u fastmem=%u\n",
                        unsigned(Settings::values.dma_accuracy.GetValue()),
                        unsigned(Settings::values.current_gpu_accuracy),
                        unsigned(!Eden::DevVulkan::disable_null_descriptor),
                        unsigned(!Eden::DevVulkan::disable_descriptor_buffer),
                        unsigned(Eden::DevVulkan::robustness2),
                        unsigned(Settings::values.vertex_input_dynamic_state.GetValue()),
                        unsigned(Settings::values.dyna_state.GetValue()),
                        unsigned(!Eden::DevVulkan::disable_sparse),
                        unsigned(!Eden::DevVulkan::disable_multi_range),
                        unsigned(!Eden::DevVulkan::disable_custom_border),
                        unsigned(Eden::DevVulkan::sync_submissions),
                        unsigned(Eden::Fastmem::Requested()));
            std::printf("EDEN_DEV_MEMORY direct_memory=%lld vram_mode=%u\n",
                        static_cast<long long>(sceKernelGetDirectMemorySize()),
                        unsigned(Settings::values.vram_usage_mode.GetValue()));
        }
        Settings::values.use_docked_mode.SetValue(Settings::ConsoleMode::Docked);
        std::puts("[headless-startup] console_mode=docked");
#else
        Settings::values.use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
#if defined(PS5_NATIVE) && defined(EDEN_PS5_OPENGL)
        const bool docked = safe_launch ? false :
            game_video.console_mode >= 0 ? game_video.console_mode == 1 :
            effective_authored_tier >= 0 ? title_profile.docked : true;
        Settings::values.use_docked_mode.SetValue(docked ? Settings::ConsoleMode::Docked
                                                       : Settings::ConsoleMode::Handheld);
        Eden::Report("launch", docked ? "Console mode: Docked" : "Console mode: Handheld");
#endif
#endif
#if defined(PS5_NATIVE) && defined(EDEN_PS5_OPENGL)
        {
            // Settings > Video: internal resolution and the filter scaling it to the output.
            static constexpr Settings::ResolutionSetup resolutions[] = {
                Settings::ResolutionSetup::Res1_4X, Settings::ResolutionSetup::Res1_2X,
                Settings::ResolutionSetup::Res3_4X, Settings::ResolutionSetup::Res1X,
                Settings::ResolutionSetup::Res5_4X, Settings::ResolutionSetup::Res3_2X,
                Settings::ResolutionSetup::Res2X, Settings::ResolutionSetup::Res3X,
                Settings::ResolutionSetup::Res4X};
            static_assert(std::size(resolutions) == std::size(Eden::kResolutionKeys));
            static constexpr Settings::ScalingFilter filters[] = {
                Settings::ScalingFilter::Bilinear, Settings::ScalingFilter::Fsr, Settings::ScalingFilter::Bicubic,
                Settings::ScalingFilter::NearestNeighbor};
            const int resolution = safe_launch ? Eden::kNativeResolution :
                (game_video.resolution >= 0 ? game_video.resolution : profile_resolution);
            const int filter = safe_launch ? 0 :
                (game_video.upscaling_filter >= 0 ? game_video.upscaling_filter : profile_filter);
            Settings::values.resolution_setup.SetValue(resolutions[resolution]);
            Settings::values.scaling_filter.SetValue(filters[filter]);
            const int fsr_sharpness = safe_launch ? 50 :
                (game_video.fsr_sharpness >= 0 ? game_video.fsr_sharpness : profile_fsr);
            const int anti_aliasing = safe_launch ? 0 :
                (game_video.anti_aliasing >= 0 ? game_video.anti_aliasing : profile_aa);
            // Eden's raw FSR control is reversed (0 = sharpest, 200 = softest). Encore presents
            // a normal 0-100 sharpness percentage.
            Settings::values.fsr_sharpening_slider.SetValue(200 - std::clamp(fsr_sharpness, 0, 100) * 2);
            static constexpr Settings::AntiAliasing aa[] = {
                Settings::AntiAliasing::None, Settings::AntiAliasing::Fxaa, Settings::AntiAliasing::Smaa};
            Settings::values.anti_aliasing.SetValue(aa[std::clamp(anti_aliasing, 0, 2)]);
            Settings::UpdateRescalingInfo();
            // The output's refresh rate while the game runs (display_refresh.h): the renderer asks
            // for it as it opens the output.
            const int refresh = safe_launch ? 0 :
                (game_video.refresh >= 0 ? game_video.refresh : profile_refresh);
            Eden::Display::requested_hz.store(Eden::kRefreshHz[refresh]);
            Eden::Display::output_millihertz.store(0);
            setenv(Eden::Display::kVulkanSwitch, refresh ? "1" : "0", 1);
            Eden::Display::game_millihertz.store(60000);
            // No 120/240-FPS skip timestamp may survive the previous game.
            // Called after the prior renderer has shut down, before this
            // session starts producing frames.
            Eden::Display::ResetSkipFrameTracking();
            // The previous game's Vulkan pressure snapshot belongs to its
            // destroyed renderer, not to this title's direct-memory budget.
            // Prior GPU workers have joined before this launcher path.
            Eden::Performance::ResetDirectMemoryProbeForTitle();
            // The size of the picture the session puts out (Settings > Video > Output resolution).
            const int output = safe_launch ? 0 :
                (game_video.output >= 0 ? game_video.output : profile_output);
            Eden::Display::output_width.store(Eden::kOutputWidth[output]);
            Eden::Display::output_height.store(Eden::kOutputHeight[output]);
            Eden::Report("launch", (std::string("Resolution ") + Eden::kResolutionKeys[resolution] + ", " +
                                    Eden::kUpscalingFilterLabels[filter] + ", FSR sharpness " +
                                    std::to_string(fsr_sharpness) + "%, AA " +
                                    Eden::kAntiAliasingLabels[std::clamp(anti_aliasing, 0, 2)] + ", output " +
                                    Eden::kOutputKeys[output] + ", " + Eden::kRefreshKeys[refresh] +
                                    " Hz").c_str());
            // What a crash report says was running.
            char title_id[20];
            std::snprintf(title_id, sizeof(title_id), "%016llx",
                          static_cast<unsigned long long>(eden_game_title_id(guest)));
            // The game's mods, minus those switched off in the launcher (Library > Game settings >
            // Mods): Eden's patch manager skips the names in this list. With the Library's Mods
            // switch off for the game, every one of them is in it.
            const u64 title = eden_game_title_id(guest);
            const auto all_mods = Eden::Mods::List(Eden::AssetsPath("mods"), title);
            auto mods_off = Eden::LoadDisabledMods(title);
            const bool all_mods_enabled = Eden::LoadModsEnabled(title);
            if (safe_launch) {
                for (const auto& mod : all_mods)
                    if (std::find(mods_off.begin(), mods_off.end(), mod.name) == mods_off.end())
                        mods_off.push_back(mod.name);
            } else if (!all_mods_enabled) {
                for (const auto& mod : all_mods)
                    if (std::find(mods_off.begin(), mods_off.end(), mod.name) == mods_off.end())
                        mods_off.push_back(mod.name);
            }
            // encore-overrides-style visual in-game glyph rules. This ONLY
            // enables/disables verified ROMFS *graphic assets*, not pad actions.
            // Unknown/updated titles, missing manifests and Safe Launch keep
            // original Nintendo artwork. Once per launch; no GPU hot-path scan.
            const auto glyph_catalogue = Eden::GlyphOverrides::LoadCatalogue(
                Eden::ConfigFile("encore-glyph-overrides.json"));
            // sys-con-like separation of physical pad, normalized guest HID,
            // and the game's art: a PlayStation texture pack is valid only
            // for the actual fixed Cross=A input profile. Custom/physical-
            // Switch mapping can require different game-native artwork.
            // Do not reread controller preferences for unsupported titles.
            const auto playstation_glyph_mapping = [&] {
                const auto controls = Eden::LoadPreferences();
                return Eden::CompatibleWithPlayStationActionGlyphs(
                    Eden::ResolveSessionButtonMapping(
                        controls.controller_layout, controls.mapping,
                        game_video.controller_layout, game_video.own_mapping,
                        game_video.mapping));
            };
            const bool glyph_requested = !safe_launch && all_mods_enabled &&
                Eden::LoadInGamePlayStationGlyphs(title) &&
                std::any_of(glyph_catalogue.rules.begin(), glyph_catalogue.rules.end(),
                    [title](const Eden::GlyphOverrides::Rule& rule) {
                        return rule.title == title;
                    }) &&
                playstation_glyph_mapping() &&
                std::none_of(mods_off.begin(), mods_off.end(), [](const std::string& name) {
                    return Eden::Mods::Lower(name) == Eden::Mods::Lower(Eden::GlyphOverrides::kModName);
                });
            const auto glyph_style = glyph_requested ?
                Eden::GlyphOverrides::Style::PlayStation :
                Eden::GlyphOverrides::Style::Nintendo;
            char glyph_update_version[96]{};
            // Opening a game's control RomFS can require additional reads
            // or decryption. Skip it if no compatible art rule/asset exists:
            // most titles have no rule, so there is no extra boot-time I/O.
            if (Eden::GlyphOverrides::NeedsGameVersion(
                    glyph_catalogue, title, glyph_style, all_mods)) {
                (void)eden_game_glyph_display_version(
                    guest, Eden::AssetsPath("keys").c_str(), title,
                    glyph_update_version, sizeof(glyph_update_version));
            }
            const auto glyph_state = Eden::GlyphOverrides::Select(
                glyph_catalogue, title, glyph_update_version, glyph_style,
                Eden::AssetsPath("mods"), all_mods);
            for (const auto& mod : all_mods) {
                if (Eden::Mods::Lower(mod.name) !=
                    Eden::Mods::Lower(Eden::GlyphOverrides::kModName)) continue;
                if (safe_launch || glyph_state != Eden::GlyphOverrides::State::Enabled) {
                    if (std::find(mods_off.begin(), mods_off.end(), mod.name) == mods_off.end())
                        mods_off.push_back(mod.name);
                }
            }
            const bool glyph_applied = glyph_state == Eden::GlyphOverrides::State::Enabled &&
                !safe_launch && std::none_of(mods_off.begin(), mods_off.end(), [](const std::string& name) {
                    return Eden::Mods::Lower(name) == Eden::Mods::Lower(Eden::GlyphOverrides::kModName);
                });
            Eden::Report("glyphs", (std::string("In-game button art: ") +
                (glyph_applied ? "PlayStation RomFS pack" : "Nintendo original") +
                " (rule=" + Eden::GlyphOverrides::StateName(glyph_state) + ")").c_str());
            const std::string mods = Eden::Mods::Summary(all_mods, mods_off);
            Settings::values.disabled_addons[title] = mods_off;
            Eden::Report("launch", ("Mods: " + mods).c_str());
            Eden::Crash::SetSession("game " + std::filesystem::path(guest).filename().string() + " (" + title_id +
                                    "), " + Eden::BackendName(backend) + ", resolution " +
                                    Eden::kResolutionKeys[resolution] + ", " + Eden::kUpscalingFilterLabels[filter] +
                                    ", output " + Eden::kOutputKeys[output] + ", " +
                                    Eden::kRefreshKeys[refresh] + " Hz, mods: " + mods,
                                    true);
        }
#endif
        Settings::values.sink_id = Settings::AudioEngine::Null;
        Settings::values.use_multi_core = true;
        // A game's library applets (its error dialog, profile picker, amiibo screen, Mii editor,
        // manual, album...) are Eden's built-in ones. Eden's defaults start several of them from
        // the firmware as a second guest program. Nothing can operate those here, and the error
        // applet started that way ran a game's session out of graphics memory on the console
        // (CreateBuffer: VK_ERROR_OUT_OF_DEVICE_MEMORY as the applet started). The built-in ones
        // answer at once: the error is logged (error_applet.h), the first profile is chosen, the
        // others close.
        // (dev-settings applets=firmware keeps Eden's defaults.)
#ifdef EDEN_DEV_PROFILE
        if (!Eden::Performance::firmware_applets)
#endif
        {
            Settings::values.cabinet_applet_mode = Settings::AppletMode::HLE;
            Settings::values.error_applet_mode = Settings::AppletMode::HLE;
            Settings::values.net_connect_applet_mode = Settings::AppletMode::HLE;
            Settings::values.player_select_applet_mode = Settings::AppletMode::HLE;
            Settings::values.mii_edit_applet_mode = Settings::AppletMode::HLE;
            Settings::values.photo_viewer_applet_mode = Settings::AppletMode::HLE;
            Settings::values.offline_web_applet_mode = Settings::AppletMode::HLE;
            Settings::values.my_page_applet_mode = Settings::AppletMode::HLE;
        }
        // The PS5 build bounds Eden's GPU command queue below, so the CPU and renderer can run in
        // parallel without accumulating the multi-frame controller lag of the upstream queue.
        Settings::values.use_asynchronous_gpu_emulation = true;
#if defined(EDEN_PS5_OPENGL) || defined(EDEN_PS5_VULKAN)
        // Persist compiled shaders in the foreground game title cache.
        Settings::values.use_disk_shader_cache = game;
#else
        Settings::values.use_disk_shader_cache = false;
#endif
#ifdef EDEN_PS5_VULKAN
        // Vulkan driver cache complements (but does not replace) guest shader
        // pipeline metadata persisted by the rasterizer.
        Settings::values.use_vulkan_driver_pipeline_cache = game;
#endif
        // The PS5 port has software FFmpeg decoders but no FFmpeg hardware-device adapter.
        Settings::values.nvdec_emulation = game ? Settings::NvdecEmulation::Cpu
                                                : Settings::NvdecEmulation::Off;
        Settings::values.use_gdbstub = false;
        // Settings > Language: the system language games see, with its console region.
        static_assert(static_cast<int>(Settings::Language::EnglishBritish) == 12 &&
                      static_cast<int>(Settings::Language::ChineseTraditional) == 16 &&
                      static_cast<int>(Settings::Language::Thai) == 19 &&
                      static_cast<int>(Settings::Region::Taiwan) == 6, "Eden's language or region order changed");
        {
            // Eden only finds a game's supported languages when an update supplies its control data.
            // Without them it hands the game the chosen language as it is, and a game that does not
            // know it (an older one given pt-BR) falls back to Japanese. So the game gets its own
            // closest language, the one the launcher shows for it (eden_game_language).
            const int choice = Eden::LoadPreferences().language;
            int language = Eden::kLanguageSettings[choice];
#ifdef PS5_NATIVE
            if (game && guest)
                language = eden_game_language(guest, Eden::AssetsPath("keys").c_str(), eden_game_title_id(guest), language);
#endif
            Settings::values.language_index.SetValue(static_cast<Settings::Language>(language));
            Settings::values.region_index.SetValue(static_cast<Settings::Region>(Eden::kLanguageRegions[choice]));
            const std::string code(reinterpret_cast<const char*>(
                &Service::Set::available_language_codes[static_cast<std::size_t>(language)]), 8);
            Eden::Report("launch", ("Language: " + std::string(code.c_str()) + " (chosen " +
                                    Eden::kLanguageKeys[choice] + ")").c_str());
        }
        std::unique_ptr<Eden::Pad> pad;
        bool return_to_menu = false;
        if (devices || game) {
            const auto controls = Eden::LoadPreferences();
            const auto session_controls = Eden::ResolveSessionButtonMapping(
                controls.controller_layout, controls.mapping,
                game_video.controller_layout, game_video.own_mapping, game_video.mapping);
            const int effective_layout = session_controls.layout;
            const auto mapping = session_controls.buttons;
            const bool custom_mapping = Eden::MappingIsCustom(mapping, effective_layout);
            pad = std::make_unique<Eden::Pad>(
                static_cast<float>(controls.stick_deadzone) / 100.0f, 0.5f);
            // Emulate desktop controller profiles: resolve the selected
            // global/game-specific mapping ONCE before creating guest HID.
            // No automatic UI/gameplay A/B/X/Y swaps and no mid-title toggle.
            pad->SetMapping(mapping);
            std::fprintf(stderr,
                "EDEN_PAD_MAPPING_FIXED title=%016llx layout=%d a=%d b=%d x=%d y=%d title_override=%u\n",
                static_cast<unsigned long long>(launch_title_id),
                effective_layout, mapping[Eden::game_a], mapping[Eden::game_b],
                mapping[Eden::game_x], mapping[Eden::game_y],
                static_cast<unsigned>(game_video.controller_layout >= 0 || game_video.own_mapping));
            if (!pad->Open()) throw std::runtime_error("PS5 controller initialization failed");
            const std::string mapping_profile =
                custom_mapping ? (effective_layout == 1 ? "Custom Switch" : "Custom PS5") :
                                 (effective_layout == 1 ? "Switch" : "PlayStation");
            Eden::Report("controls", "EDEN_PAD_MAPPING_LOCKED scope=session mode=static");
            Eden::Report("controls", (std::string("Controller profile ") + mapping_profile +
                ", map " + Eden::Settings::MappingJson(mapping, Eden::BaseMappingForLayout(effective_layout)).dump() +
                ", deadzone " + std::to_string(controls.stick_deadzone) + "%, vibration " +
                (controls.vibration ? std::to_string(controls.vibration_strength) + "%" : "off")).c_str());
            Settings::values.audio_output_device_id = "ps5";
            Settings::values.vibration_enabled.SetValue(controls.vibration);
            // One Pro Controller per signed-in user's DualSense; later changes apply mid-game.
            const unsigned connected = pad->ConnectedPlayers();
            (void)pad->TakeConnectionChanges();
            for (std::size_t index = 0; index < Eden::Pad::kMaxPlayers; ++index) {
                auto& player = Settings::values.players.GetValue()[index];
                player.connected = index == 0 || (connected & (1u << index)) != 0;
                player.controller_type = Settings::ControllerType::ProController;
                player.vibration_enabled = controls.vibration;
                player.vibration_strength = controls.vibration_strength;
            }
        }
        {
#ifdef EDEN_PS5_OPENGL
            Eden::GraphicsWindow window(backend == Eden::GraphicsBackend::Vulkan);
#ifdef EDEN_GPU_PROBE
            window.RunGpuProbe();
            passed("GPU_PROBE_COMPLETE");
            return 0;
#endif
#else
            HeadlessWindow window;
#endif
            // The running game's own contents, filled for each session below. The system keeps this
            // object's address, so it is declared first; its files belong to the system's file
            // system, so it is emptied before the system goes (a second launch crashed when it
            // released the first session's files after their file system).
            FileSys::ManualContentProvider game_contents;
            Core::System system;
            SCOPE_EXIT { game_contents.ClearAllEntries(); };
            passed("core_constructed");
#ifdef EDEN_PS5_OPENGL
            window.SetSystem(system);
#endif
            system.Initialize();
            if (system.IsPoweredOn()) return 1;
            passed("core_initialized");
            for (unsigned cycle = 0; guest && cycle < cycles; ++cycle) {
#ifdef PS5_NATIVE
                // The previous title's GPU owners have already finished
                // teardown. Keep the next title's faults independently counted.
                ::Eden::GpuFault::ResetTitleCounters();
#endif
                if (game) LOG_INFO(Frontend, "EDEN_GAME_SESSION_BEGIN {}", cycle + 1);
                struct Completion {
                    std::mutex mutex;
                    std::condition_variable wake;
                    bool exited = false;
                    bool captured = false;
                    bool return_to_menu = false;
                    std::exception_ptr failure;
                    std::string guest_fault;
                };
                auto completion = std::make_shared<Completion>();
                system.RegisterExitCallback([completion] {
                    std::lock_guard lock(completion->mutex);
                    completion->exited = true;
                    completion->wake.notify_one();
                });
                SCOPE_EXIT {
                    if (system.IsPoweredOn()) {
                        Eden::StopLimit::Begin();
                        system.ShutdownMainProcess();
                        Eden::StopLimit::End();
                    }
                    system.RegisterExitCallback({});
                };
                // Like Eden's Qt/Android frontends, reset shutdown state for each load.
                system.SetShuttingDown(false);
                // Update and DLC files (NSP or XCI) in the game files' updates folder apply to their
                // base game: Eden's ExternalContentProvider, which the patch manager checks first.
                Settings::values.external_content_dirs = {Eden::AssetsPath("updates")};
                system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
                // The game's own contents, registered as Eden's desktop frontends do before booting a
                // game (ConfigureFilesystemProvider). An update's data is a patch on the base game's:
                // the patch manager looks the base up here, and without it applied only the update's
                // code, which then ran against the old data. It also gives Eden the game's control data.
                game_contents.ClearAllEntries();
                if (game && guest && !game_contents.AddEntriesFromContainer(
                        system.GetFilesystem()->OpenFile(guest, FileSys::OpenMode::Read)))
                    Eden::Report("loader", "Game contents not registered; an update's data will not apply");
                system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual, &game_contents);
                {
                    Service::AM::Frontend::FrontendAppletSet applets;
                    // A game's "connect controllers" screen: one player per PS5 controller in use.
                    if (pad) applets.controller = std::make_unique<Eden::PadControllerApplet>(system.HIDCore(), *pad);
                    // A game's error dialog: logged and closed, so the game carries on.
                    applets.error = std::make_unique<Eden::LoggedErrorApplet>();
                    system.GetFrontendAppletHolder().SetFrontendAppletSet(std::move(applets));
                }
                Service::AM::FrontendAppletParameters params{
                    .applet_id = Service::AM::AppletId::Application,
                };
                std::atomic<bool> left_while_loading{false};
                std::jthread load_input;
                if (pad && game) {
                    load_input = std::jthread([&](std::stop_token stop) {
                        while (!stop.stop_requested()) {
                            pad->Poll();
                            (void)pad->TakeHudToggle();
                            if (pad->TakeReturnToMenu()) {
                                Eden::Report("exit", "Touchpad + L1 while loading; requesting bounded return");
                                left_while_loading = true;
                                Eden::StopLimit::Begin();
                                break;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(8));
                        }
                    });
                }
                Core::SystemResultStatus loaded;
                try {
                    loaded = system.Load(window, guest, params);
                } catch (const std::exception& error) {
                    load_input.request_stop();
                    if (load_input.joinable()) load_input.join();
                    if (left_while_loading) Eden::StopLimit::End();
                    // Preserve the original error before partial core teardown.
                    std::fprintf(stderr, "Game load failed: %s\n", error.what());
                    std::fflush(stdout);
                    throw;
                }
                load_input.request_stop();
                if (load_input.joinable()) load_input.join();
                if (loaded != Core::SystemResultStatus::Success) {
                    if (left_while_loading) Eden::StopLimit::End();
                    std::fprintf(stderr, "Core load failed: %u\n", static_cast<unsigned>(loaded));
                    if (loaded == Core::SystemResultStatus::ErrorVideoCore) {
                        throw std::runtime_error("Graphics backend initialization failed. Try another backend in Settings; see stderr.log and eden_log.txt for driver details.");
                    }
                    throw std::runtime_error("Loader status " + std::to_string(static_cast<unsigned>(loaded)) +
                        ". Check this ROM, its keys and firmware; see stderr.log.");
                }
#ifdef PS5_NATIVE
                if (game) {
                    const auto filename = std::filesystem::path(guest).filename().string();
                    const bool saved_last = Eden::SaveLastGame(filename);
                    const bool saved_recent = Eden::SaveRecentGame(filename);
                    if (!saved_last || !saved_recent)
                        Eden::Report("history", "Could not save recent game");
                }
#endif
                passed(game ? "game_loaded" : "nro_loaded");
                Eden::Report("loader", "Game loaded; initializing renderer");
#ifdef EDEN_PS5_OPENGL
                // Retain the failure, then release CPU readiness and complete normal
                // shutdown before reporting it. Unwinding before OnGpuReady can hang.
                std::exception_ptr graphics_error;
#ifndef PS5_NATIVE
                std::shared_future<bool> game_capture;
                try { window.CheckPresentation(system.GPU().Renderer().Context()); }
                catch (...) { graphics_error = std::current_exception(); }
                if (game && !graphics_error)
                    game_capture = window.CaptureNextFrame(system.GPU().Renderer(), [completion] {
                        std::lock_guard lock(completion->mutex);
                        completion->captured = true;
                        completion->wake.notify_one();
                    });
#endif
#endif
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
                Eden::Stall::Trace("main gpu_start");
#endif
                system.GPU().Start();
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
                Eden::Stall::Trace("main gpu_started");
#endif
                system.GetCpuManager().OnGpuReady();
                passed("cpu_manager_ready");
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
                Eden::Stall::Trace("main shader_cache");
#endif
#ifdef EDEN_PS5_OPENGL
                // Match yuzu_cmd's ordering: GPU/context ready, cache load, guest Run.
                if (Settings::values.use_disk_shader_cache.GetValue() && !graphics_error) {
                    // Every cached pipeline is built before the game starts; report progress
                    // every five seconds so a slow build can be told apart from a stalled one.
                    std::atomic<size_t> built{0}, total{0};
                    std::atomic<bool> counted{false};
                    const auto load_start = std::chrono::steady_clock::now();
                    std::jthread reporter([&](std::stop_token stop) {
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
                        Eden::Stall::Trace("reporter start");
#endif
                        for (unsigned tick = 1; !stop.stop_requested(); ++tick) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            if (tick % 50 != 0 || stop.stop_requested()) continue;
                            const std::string line = counted
                                ? fmt::format("Shader cache: {} of {} pipelines built ({} s)",
                                              built.load(), total.load(), tick / 10)
                                : fmt::format("Shader cache: reading ({} s)", tick / 10);
                            Eden::Report("loader", line.c_str());
                        }
                    });
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
                    Eden::Stall::Trace("main cache_load");
#endif
                    system.Renderer().ReadRasterizer()->LoadDiskResources(
                        system.GetApplicationProcessProgramID(), std::stop_token{},
                        [&](VideoCore::LoadCallbackStage stage, size_t value, size_t count) {
                            if (stage != VideoCore::LoadCallbackStage::Build) return;
                            built = value;
                            total = count;
                            counted = true;
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
                            Eden::Stall::Tick();
#endif
                        });
                    reporter.request_stop();
                    reporter.join();
                    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - load_start).count();
                    Eden::Report("loader", fmt::format("Shader cache ready: {} pipelines in {} ms",
                                                       total.load(), load_ms).c_str());
                    std::puts("EDEN_SHADER_CACHE_LOADED");
                    std::fflush(stdout);
                }
#endif
#ifdef EDEN_PS5_VULKAN
                if (game && Settings::values.use_disk_shader_cache.GetValue()) {
                    // Vulkan's per-title pipeline metadata was never configured:
                    // previous launches could serialize nothing because the
                    // filename stayed empty. Request initialization only, NOT
                    // Worker's destructive stop-token cancellation path.
                    std::stop_source lazy_cache;
                    lazy_cache.request_stop();
                    const auto title_id = system.GetApplicationProcessProgramID();
                    if (title_id) {
                        system.Renderer().ReadRasterizer()->LoadDiskResources(
                            title_id, lazy_cache.get_token(),
                            [](VideoCore::LoadCallbackStage, size_t, size_t) {});
                        std::printf("EDEN_VULKAN_SHADER_CACHE title=%016llX mode=lazy-persistent\n",
                                    static_cast<unsigned long long>(title_id));
                    }
                }
#endif
#ifndef PS5_NATIVE
                if (devices) Eden::Mock::BeginGuestCycle();
#endif
#ifdef EDEN_DEV_PROFILE
                if (auto* process = system.ApplicationProcess()) {
                    // P1: guest code reads for the core-0 profile's block dumps.
                    static Core::Memory::Memory* dev_guest_memory = nullptr;
                    dev_guest_memory = &process->GetMemory();
                    Eden::Performance::guest_read32 = [](unsigned long long address, unsigned& value) {
                        if (!dev_guest_memory || !dev_guest_memory->IsValidVirtualAddressRange(address, 4)) return false;
                        value = dev_guest_memory->Read32(address);
                        return true;
                    };
                    std::printf("EDEN_MAIN_BASE main=%llx\n",
                                static_cast<unsigned long long>(GetInteger(Core::FindMainModuleEntrypoint(process))));
                }
                if (auto* process = system.ApplicationProcess();
                    process && (Eden::Watch::watch_range.size || Eden::Watch::dump_range.size)) {
                    // Resolve dev-settings watch=/dump= against this boot's main module.
                    const u64 main_base = GetInteger(Core::FindMainModuleEntrypoint(process));
                    auto& memory = process->GetMemory();
                    if (const auto range = Eden::Watch::dump_range; range.size) {
                        for (u64 at = main_base + range.offset; at < main_base + range.offset + range.size; at += 0x40) {
                            std::string line = fmt::format("EDEN_CRASH_CODE dump ret={:016X} from={:016X}:", at, at);
                            for (u64 word = at; word < at + 0x40; word += 4)
                                line += fmt::format(" {:08X}", memory.IsValidVirtualAddressRange(word, 4) ? memory.Read32(word) : 0u);
                            LOG_CRITICAL(Core_ARM, "{}", line);
                        }
                    }
                    if (const auto range = Eden::Watch::watch_range; range.size) {
                        const u64 first = main_base + range.offset;
                        Eden::Watch::end = first + range.size;
                        Eden::Watch::begin = first;
                        memory.MarkRegionDebug(first, range.size, true);
                    }
                    LOG_CRITICAL(Core_ARM, "EDEN_WATCH main={:016X} watch=+{:#x}:{:#x} dump=+{:#x}:{:#x}", main_base,
                                 Eden::Watch::watch_range.offset, Eden::Watch::watch_range.size,
                                 Eden::Watch::dump_range.offset, Eden::Watch::dump_range.size);
                }
#endif
                // The blocks this game compiled in earlier sessions, compiled ahead on a spare CPU
                // according to the selected performance profile.
#if defined(EDEN_DEV_PROFILE) || defined(EDEN_DEV_ROM_ID)
                if (std::filesystem::exists(Eden::AppFile("block-list.txt")))
                    Eden::JitList::enabled = true;
#endif
                Eden::JitList::Session jit_list;
                if (auto* process = system.ApplicationProcess()) {
                    Eden::JitList::BuildId build{};
                    const auto& id = system.GetApplicationProcessBuildID();
                    std::memcpy(build.data(), id.data(), std::min(build.size(), id.size()));
                    // The game's own modules: the run of code mappings from its entry point.
                    const u64 code_start = GetInteger(process->GetEntryPoint());
                    u64 code_end = code_start;
                    for (;;) {
                        Kernel::KMemoryInfo info{};
                        Kernel::Svc::PageInfo page{};
                        if (process->GetPageTable().QueryInfo(&info, &page, code_end).IsError() || info.m_size == 0 ||
                            (info.m_state != Kernel::KMemoryState::Code && info.m_state != Kernel::KMemoryState::CodeData))
                            break;
                        code_end = info.m_address + info.m_size;
                    }
                    jit_list.Start(system.GetApplicationProcessProgramID(), build, code_start, code_end - code_start);
                }
                Eden::TakeGuestFault(); // Nothing from an earlier session belongs to this one.
                system.Run();
                if (left_while_loading) {
                    std::lock_guard lock(completion->mutex);
                    completion->return_to_menu = true;
                    completion->wake.notify_one();
                }
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
                Eden::Stall::Trace("main running");
                Eden::Stall::Disarm(); // boot supervision ends
                if (game) Eden::Stall::ArmGame(); // only an actual game, not device-only probes
                SCOPE_EXIT { Eden::Stall::DisarmGame(); }; // even when an exception unwinds
#endif
                const auto session_start = std::chrono::steady_clock::now();
                [[maybe_unused]] double session_seconds = 0;  // guest-fault relaunch (PS5 only)
                std::jthread input_worker;
                if (pad) input_worker = std::jthread([&](std::stop_token stop) {
#ifdef EDEN_DEV_PROFILE
                    // Live DualSense is ALWAYS the default, including the all-on
                    // FC27 development build. A title ID is only a boot target;
                    // it must never silently switch off physical button polling.
                    // Automated replay is opt-in for unattended bench runs only.
                    bool replay_requested = false;
                    {
                        std::ifstream dev_settings(Eden::AppFile("dev-settings.txt"));
                        for (std::string entry; dev_settings >> entry;)
                            replay_requested = Eden::DevInput::ReplayRequestedAfter(replay_requested, entry);
                    }
                    const bool timed_replay = Eden::DevInput::ScriptedReplayEnabled(
                        replay_requested, development_id, EDEN_DEV_PROFILE_TITLE);
                    std::fprintf(stderr, "EDEN_PAD_INPUT mode=%s replay_requested=%d title=%s\n",
                                 timed_replay ? "scripted" : "dualsense", int(replay_requested),
                                 development_id.c_str());
                    const auto replay_start = std::chrono::steady_clock::now();
#endif
#ifdef EDEN_DEV_ROM_ID
                    Eden::DevelopmentInput development_input;
                    unsigned command_poll = 0;
#endif
                    while (!stop.stop_requested()) {
                        if (auto error = Eden::TakeGpuFailure()) {
                            std::lock_guard lock(completion->mutex);
                            completion->failure = std::move(error);
                            completion->wake.notify_one();
                            break;
                        }
                        if (auto fault = Eden::TakeGuestFault(); !fault.empty()) {
                            std::lock_guard lock(completion->mutex);
                            completion->guest_fault = std::move(fault);
                            completion->wake.notify_one();
                            break;
                        }
#ifdef EDEN_DEV_PROFILE
                        if (!timed_replay) {
#endif
#ifdef EDEN_DEV_ROM_ID
                        if (!development_input.active) pad->Poll();
#else
                        pad->Poll();
#endif
#ifdef EDEN_DEV_ROM_ID
                        const auto command_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        if (++command_poll >= 25) {
                            command_poll = 0;
                            std::ifstream command(Eden::AppFile("compat-input.txt"));
                            if (development_input.Read(command, command_now))
                                std::printf("EDEN_DEV_INPUT sequence=%llu buttons=%x\n",
                                    static_cast<unsigned long long>(development_input.sequence), development_input.buttons);
                        }
                        if (const auto sample = development_input.Sample(command_now))
                            pad->Consume({&*sample, 1});
                        // The guest's Minus going down and up, with how long the state before it
                        // lasted: the touchpad's tap and long press (headless/pad.cpp) on real timing.
                        {
                            static bool minus_down = false;
                            static auto minus_since = std::chrono::steady_clock::now();
                            const bool down = pad->Engine().GetButton({}, static_cast<int>(
                                InputCommon::VirtualGamepad::VirtualButton::ButtonMinus));
                            if (down != minus_down) {
                                const auto changed = std::chrono::steady_clock::now();
                                std::printf("EDEN_DEV_MINUS state=%d after_ms=%lld\n", down,
                                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        changed - minus_since).count()));
                                minus_down = down;
                                minus_since = changed;
                            }
                        }
#endif
#ifdef EDEN_DEV_PROFILE
                        } else {
                        using Button = InputCommon::VirtualGamepad::VirtualButton;
                        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - replay_start).count();
                        // Development replay: enter Single Player/Grand Prix using defaults.
                        pad->Engine().SetButtonState(0, Button::TriggerL, seconds == 45);
                        pad->Engine().SetButtonState(0, Button::TriggerR, seconds == 45);
                        pad->Engine().SetButtonState(0, Button::ButtonA,
                            (seconds >= 50 && seconds < 200 && seconds % 5 == 0) || seconds >= 200);
                        }
#endif
                        if (const unsigned changed = pad->TakeConnectionChanges()) {
                            // A controller that came or went connects or disconnects its player.
                            const unsigned connected = pad->ConnectedPlayers();
                            for (std::size_t index = 1; index < Eden::Pad::kMaxPlayers; ++index) {
                                if (!(changed & (1u << index))) continue;
                                const bool present = (connected & (1u << index)) != 0;
                                Settings::values.players.GetValue()[index].connected = present;
                                auto* controller = system.HIDCore().GetEmulatedControllerByIndex(index);
                                if (present) {
                                    controller->SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
                                    controller->Connect();
                                } else {
                                    controller->Disconnect();
                                }
                                LOG_INFO(Input, "EDEN_PLAYER player={} connected={}", index + 1, present);
                            }
                        }
#ifdef EDEN_PS5_OPENGL
                        if (pad->TakeHudToggle()) Eden::ToggleHud();
#else
                        (void)pad->TakeHudToggle();
#endif
                        if (pad->TakeReturnToMenu()) {
                            std::lock_guard lock(completion->mutex);
                            completion->return_to_menu = true;
                            completion->wake.notify_one();
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(4));
                    }
                });
                {
                    std::unique_lock lock(completion->mutex);
                    constexpr unsigned stop_delays_ms[] = {0, 1, 10, 100, 1000, 30000};
                    const auto wait_ms = shutdown_sweep ? stop_delays_ms[cycle] :
                        1000u * (cpu_pressure ? 180u : game ? 600u : devices ? 20u : 5u);
                    if (shutdown_sweep) LOG_INFO(Frontend, "EDEN_SHUTDOWN_DELAY_MS {}", wait_ms);
                    const auto completed = [&] {
                        return completion->failure || completion->exited || completion->captured ||
                            completion->return_to_menu || !completion->guest_fault.empty();
                    };
#ifdef EDEN_DEV_PROFILE
                    // The 30-second segments a development session samples before it only waits.
                    // On Vulkan a picture can be asked for (capture-once.txt) for as long as the
                    // game runs.
#ifdef EDEN_DEV_VULKAN
                    [[maybe_unused]] constexpr unsigned segments = ~0u;
#else
                    [[maybe_unused]] constexpr unsigned segments = 12;
#endif
#endif
#ifdef PS5_NATIVE
                    if (game) {
#ifdef EDEN_DEV_PROFILE
                        for (unsigned segment = 0; segment < segments; ++segment) {
                            bool finished = false;
                            for (unsigned poll = 0; poll < 600; ++poll) {
                                if (completion->wake.wait_for(lock, std::chrono::milliseconds(50), completed)) {
                                    finished = true;
                                    break;
                                }
                                // The runner's crash request, to test the crash report in a game.
                                if (poll % 20 == 0) {
                                    lock.unlock();
                                    Eden::Crash::DevelopmentRequest(Eden::AppFile("crash-app.txt"));
                                    lock.lock();
                                }
#ifndef EDEN_DEV_VULKAN
                                if (segment >= 5) {
                                    lock.unlock();
                                    Eden::Performance::PollGpuPc();
                                    lock.lock();
                                }
#else
                                // Cover the heavy phase (wall ~90-180 s) when requested.
                                if (pc_sample_run && segment >= 3) {
                                    lock.unlock();
                                    Eden::Performance::PollGpuPc();
                                    lock.lock();
                                }
#endif
                            }
                            if (finished) break;
                            if (segment == segments - 1) {
                                completion->wake.wait(lock, completed);
                                break;
                            }
                            lock.unlock();
#ifndef EDEN_DEV_VULKAN
                            Eden::Performance::Snapshot();
                            window.CaptureNextFrame(system.GPU().Renderer(), [] {});
#else
                            std::error_code capture_error;
                            if ((segment >= 4 || Eden::Performance::capture_early) &&
                                std::filesystem::remove(Eden::AppFile("capture-once.txt"), capture_error))
                                window.CaptureNextFrame(system.GPU().Renderer(), [] {});
#endif
                            lock.lock();
                        }
#elif defined(EDEN_DEV_ROM_ID)
                        // Bounded compatibility probe (10 minutes; the runner usually closes
                        // the title first); use the ordinary teardown path.
                        constexpr unsigned observation_samples = 120;
                        for (unsigned sample = 0; sample < observation_samples; ++sample) {
                            if (completion->wake.wait_for(lock, std::chrono::seconds(5), completed)) break;
                            if (sample == observation_samples - 1) {
                                completion->return_to_menu = true;
                                break;
                            }
                            lock.unlock();
#ifdef EDEN_DEV_VULKAN
                            std::error_code capture_error;
                            const bool capture_requested = performance_run && sample >= 11 &&
                                std::filesystem::remove(Eden::AppFile("capture-once.txt"), capture_error);
                            if (!performance_run || capture_requested)
#endif
                            window.CaptureNextFrame(system.GPU().Renderer(), [] {});
                            lock.lock();
                        }
#else
                        // Shipping path: no periodic profiler/watchdog work while a game runs.
                        // Diagnostic builds own guest-PC sampling and performance snapshots.
                        completion->wake.wait(lock, completed);
#endif
                    } else
#endif
                    completion->wake.wait_for(lock, std::chrono::milliseconds(wait_ms), completed);
                    const bool guest_exited = completion->exited;
                    return_to_menu = completion->return_to_menu;
                    session_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - session_start).count();
                    if (!guest_exited && !game) {
                        std::fputs("Guest exit timed out\n", stderr);
                        return 1;
                    }
                    passed(return_to_menu ? "shortcut_return_to_menu" :
                        guest_exited ? "guest_exit_callback" : !completion->guest_fault.empty() ? "guest_fault" :
                        completion->captured ? "game_capture_shutdown" : "game_observation_complete");
                }
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
                Eden::Stall::DisarmGame(); // game wait ended; no false alerts during teardown
#endif
                input_worker.request_stop();
                if (input_worker.joinable()) input_worker.join();
                if (pad) {
                    const auto neutral = ps5::pad::neutral_data();
                    pad->Consume({&neutral, 1});
                }
                jit_list.Finish();  // while the JITs still exist: their block list is saved
                // Shutdown requests cancellation before suspending cores; Pause can
                // block while a CPU producer is waiting on a full GPU queue.
                Eden::ReportStep("shutdown", "Stopping the game");
                Eden::StopLimit::Begin();
                system.ShutdownMainProcess();
                Eden::StopLimit::End();
                Eden::Report("shutdown", "Game stopped; releasing renderer");
#ifndef PS5_NATIVE
                if (devices) {
                    Eden::Mock::CheckGuestCycle();
                    LOG_INFO(Frontend, "EDEN_GUEST_AUDIO_OUTPUT_PASS");
                    LOG_INFO(Frontend, "EDEN_GUEST_RENDERER_PCM_PASS");
                }
#endif
                if (system.IsPoweredOn()) return 1;
                passed("core_shutdown");
                if (completion->failure) {
                    try { std::rethrow_exception(completion->failure); }
                    catch (const std::bad_alloc&) {
                        // Renderer/GPU worker OOM is not necessarily a Vulkan
                        // device allocation. Preserve its type so the native
                        // outer recovery handler gives actionable guidance.
                        throw;
                    }
                    catch (const std::exception& error) {
                        // Out of graphics memory: seen with the resolution above 1x in a game that
                        // runs at 1x. The launcher shows this sentence in the player's language
                        // (prosperoeden/eden_services.cpp, kLaunchErrors).
                        const std::string what = error.what();
                        if (what.find("OUT_OF_DEVICE_MEMORY") != std::string::npos ||
                            what.find("OUT_OF_HOST_MEMORY") != std::string::npos)
                            throw std::runtime_error("The game ran out of graphics memory. Lower the resolution in "
                                                     "Settings, Video (or in the game's own settings) and start it again.");
                        throw std::runtime_error("Rendering failed: " + what +
                            ". Try another graphics backend in Settings, then reopen the game.");
                    }
                }
                // Never retry a guest fault automatically. Retrying an unmapped guest
                // jump repeatedly masks the first failure and can leave players in a
                // renderer/JIT restart loop. An explicit PS quit still wins the race.
                if (!completion->guest_fault.empty() && !return_to_menu) {
                    Eden::Report("guest fault", completion->guest_fault.c_str());
                    throw std::runtime_error("The game stopped: " + completion->guest_fault +
                        ". Reopen it from the launcher.");
                }
#ifdef EDEN_PS5_OPENGL
                if (graphics_error) std::rethrow_exception(graphics_error);
#ifndef PS5_NATIVE
                if (game) {
                    bool captured = false;
                    try {
                        captured = game_capture.wait_for(std::chrono::seconds(2)) ==
                            std::future_status::ready && game_capture.get();
                    } catch (const std::future_error& error) {
                        if (error.code() != std::make_error_code(std::future_errc::broken_promise)) throw;
                    }
                    if (!captured) throw std::runtime_error("No completed game frame capture");
                }
#endif
#endif
                if (game) LOG_INFO(Frontend, "EDEN_GAME_SESSION_END {}", cycle + 1);
            }
        }
        passed("core_destroyed");
#ifdef PS5_NATIVE
        // This thread is the only owner of its TLS allocation cache. Guest
        // workers have already joined and returned THEIR private cached blocks.
        // Drain solely the already-freed small blocks; do not unmap Sony mspaces.
        const std::size_t tcache_drained = eden_heap_release_current_tcache();
        std::printf("EDEN_HEAP_TITLE_CACHE_DRAIN bytes=%zu\n", tcache_drained);
        // Non-destructive per-mspace ownership snapshot; never unmap from a
        // merely low global live-byte count or while another thread allocates.
        eden_heap_report_roots("core_destroyed");
        // The previous core_destroyed snapshot preceded this thread's TLS
        // drain. This paired post-drain reading separates deferred allocator
        // accounting from physically retained mspace backing.
        ps5_opengl_heap_snapshot("post_cache_drain", 0);
#endif
        if (devices && pad) {
            pad.reset();
            LOG_INFO(Frontend, "EDEN_DEVICE_FRONTEND_PASS");
        }
#ifdef PS5_NATIVE
        // Undo any per-title logical-CPU trial before the next launcher/game.
        // Guest workers have stopped above; restore the original allowed mask.
        Eden::Performance::SetSecondaryPlacement(false);
        if (return_to_menu) continue;
#endif
        passed("HEADLESS_COMPLETE");
#ifdef EDEN_DEV_PROFILE
        // Keep the sandbox mounted long enough to collect final driver counters.
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(20));
#endif
        return 0;
#ifdef PS5_NATIVE
        } catch (const std::bad_alloc&) {
            // Unlike a native GPU OOM error, this can be physical RAM,
            // fragmented VA, JIT or a C++ heap failure. Do not guess.
            // Return to the launcher instead of propagating an opaque
            // std::bad_alloc; only a full app exit reclaims warm mspaces.
            Eden::Performance::SetSecondaryPlacement(false);
            // Snapshot a single firmware headroom query only on failure.
            // Largest contiguous free PA is NOT the system's total free RAM.
            std::size_t oom_largest_free = 0;
            const bool oom_query_ok = Eden::Performance::QueryLargestDirectMemoryBlock(&oom_largest_free);
            std::printf("EDEN_PS5_OOM largest_query_ok=%u largest_free_mib=%zu\n",
                        unsigned(oom_query_ok), oom_largest_free / Eden::Experimental::kMiB);
            launch_error = "The game could not allocate PS5 memory. Close Eden Encore completely before retrying to release memory retained between games.";
            Eden::Report("session failed", "std::bad_alloc: native memory allocation");
            std::fflush(stderr);
            std::fflush(stdout);
        } catch (const std::exception& error) {
            // An experimental run can fail before normal game teardown.
            Eden::Performance::SetSecondaryPlacement(false);
            launch_error = error.what();
            Eden::Report("session failed", error.what());
            std::fflush(stderr);
            std::fflush(stdout);
        }
        }
#endif
    } catch (const std::exception& error) {
        std::fflush(stdout);
        Eden::Report("fatal startup failure", error.what());
        return 1;
    }
}
