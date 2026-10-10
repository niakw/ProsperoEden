// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <EGL/egl.h>
#include <atomic>
#include <array>
#include <future>
#include <functional>
#include "core/frontend/emu_window.h"

namespace VideoCore { class RendererBase; }
namespace Core { class System; }
namespace Eden {
void ToggleHud();
class GraphicsWindow final : public Core::Frontend::EmuWindow {
public:
    explicit GraphicsWindow(bool use_vulkan = false);
    ~GraphicsWindow() override;
#ifdef EDEN_GPU_PROBE
    void RunGpuProbe();
#endif
    void CheckPresentation(Core::Frontend::GraphicsContext& context);
    void SetSystem(Core::System& value) { system = &value; }
    std::shared_future<bool> CaptureNextFrame(VideoCore::RendererBase& renderer,
                                            std::function<void()> on_complete);
    bool IsShown() const override { return true; }
    void OnFrameDisplayed() override;
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override;
private:
    bool vulkan{};
    bool splash_hidden{};
    bool splash_hide_attempted{}; // PS5 system service must never be called per frame
    bool first_frame_reported{};
    double frame_sample_start{-1}, frame_sample_last{}, frame_sample_worst{};
    unsigned frame_sample_count{}, frame_total{};
    // Constant-cost five-second present-jitter counts. 30 FPS is ~33.33 ms:
    // 38 ms catches a visible 4–6 FPS dip, without per-frame file writes.
    unsigned frame_late_38{}, frame_late_50{}, frame_late_100{};
    unsigned frame_late_200{}, frame_late_500{};
    unsigned frame_slow_streak{}, frame_max_slow_streak{};
    std::array<unsigned, 6> experimental_pacing_bins{};
    // Five-second delta baselines belong to this GraphicsWindow, NOT a static
    // shared across titles. Read 11 cheap counters at the report boundary.
    std::array<unsigned long long, 12> frame_pressure_previous{};
    EGLDisplay display{EGL_NO_DISPLAY};
    EGLConfig config{};
    EGLContext root{EGL_NO_CONTEXT};
    mutable std::atomic<bool> window_context{};
    EGLSurface surface{EGL_NO_SURFACE};
    Core::System* system{};
};
}
