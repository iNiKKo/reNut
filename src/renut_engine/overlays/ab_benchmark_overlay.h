#pragma once

#include "imgui.h"

#include <dlfcn.h>

// Content-only: no window/Begin/End, no hotkey. Hosted as a tab inside
// DebugHubOverlayDialog (see debug_hub_overlay.h), which owns the single F5
// toggle shared by all the renderer debug panels.
//
// Drives the GPU plugin's alternating A/B benchmark and shows its progress.
// The plugin is loaded with dlopen, so its symbols are resolved at runtime
// rather than linked. When they are missing - a stock xenos build, or a
// plugin without the benchmark - this reports that instead of failing.
namespace renut::overlays::ab_benchmark {

namespace detail {

struct Api {
    void (*start)() = nullptr;
    void (*stop)() = nullptr;
    int (*is_active)() = nullptr;
    const char* (*status)() = nullptr;

    bool Available() const { return start && stop && is_active && status; }
};

inline const Api& Get() {
    static const Api api = [] {
        Api result;
        // The plugin is already resident (the graphics system came from it), so
        // RTLD_NOLOAD just takes a handle to it rather than loading a second copy.
        void* self = dlopen("librexgpu-nativevkrd.so", RTLD_LAZY | RTLD_NOLOAD);
        if (!self) {
            self = dlopen("librexgpu-nativevk.so", RTLD_LAZY | RTLD_NOLOAD);
        }
        if (!self) {
            // Fall back to a global lookup for builds that link it directly.
            self = dlopen(nullptr, RTLD_LAZY);
        }
        if (self) {
            result.start = reinterpret_cast<void (*)()>(dlsym(self, "renut_ab_start"));
            result.stop = reinterpret_cast<void (*)()>(dlsym(self, "renut_ab_stop"));
            result.is_active = reinterpret_cast<int (*)()>(dlsym(self, "renut_ab_is_active"));
            result.status = reinterpret_cast<const char* (*)()>(dlsym(self, "renut_ab_status"));
            dlclose(self);
        }
        return result;
    }();
    return api;
}

inline void Toggle(const Api& api) {
    if (!api.Available()) {
        return;
    }
    if (api.is_active() != 0) {
        api.stop();
    } else {
        api.start();
    }
}

}  // namespace detail

inline void DrawContent() {
    const detail::Api& api = detail::Get();
    if (!api.Available()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Benchmark not available in this GPU plugin.");
        ImGui::TextWrapped(
            "This benchmark harness was part of the old renut plugin's diagnostic "
            "instrumentation, removed while shrinking the rexglue-sdk patch to a "
            "minimal, principled diff (see docs/ai/history.md). Not currently wired "
            "up in rexgpu-nativevk.");
        return;
    }

    const bool running = api.is_active() != 0;
    if (ImGui::Button(running ? "Stop" : "Start", ImVec2(120.0f, 0.0f))) {
        detail::Toggle(api);
    }
    ImGui::SameLine();
    if (running) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "running");
    } else {
        ImGui::TextDisabled("idle");
    }

    ImGui::Separator();
    ImGui::TextUnformatted(api.status());
    ImGui::Separator();
    ImGui::TextDisabled("A = stock path, B = optimisation under test.");
    ImGui::TextDisabled("Stand somewhere busy and keep the camera still.");
}

}  // namespace renut::overlays::ab_benchmark
