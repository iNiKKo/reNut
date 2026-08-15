#pragma once

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "imgui.h"

#include <dlfcn.h>

// Drives the GPU plugin's alternating A/B benchmark and shows its progress.
//
// The benchmark toggles the optimisation under test every renut_ab_interval_ms
// so both arms sample the same scene, weather and camera. This title's per-draw
// cost varies up to 85% between runs (the city has a day/night cycle), which is
// far larger than the effects being measured, so comparing two separate runs
// cannot resolve them.
//
// The plugin is loaded with dlopen, so its symbols are resolved at runtime
// rather than linked. When they are missing - a stock xenos build, or a plugin
// without the benchmark - the overlay reports that instead of failing.
class AbBenchmarkOverlayDialog : public rex::ui::ImGuiDialog {
public:
    explicit AbBenchmarkOverlayDialog(rex::ui::ImGuiDrawer* drawer)
        : rex::ui::ImGuiDialog(drawer) {
        // The plugin is already resident (the graphics system came from it), so
        // RTLD_NOLOAD just takes a handle to it rather than loading a second copy.
        void* self = dlopen("librexgpu-renutrd.so", RTLD_LAZY | RTLD_NOLOAD);
        if (!self) {
            self = dlopen("librexgpu-renut.so", RTLD_LAZY | RTLD_NOLOAD);
        }
        if (!self) {
            // Fall back to a global lookup for builds that link it directly.
            self = dlopen(nullptr, RTLD_LAZY);
        }
        if (self) {
            start_ = reinterpret_cast<void (*)()>(dlsym(self, "renut_ab_start"));
            stop_ = reinterpret_cast<void (*)()>(dlsym(self, "renut_ab_stop"));
            is_active_ = reinterpret_cast<int (*)()>(dlsym(self, "renut_ab_is_active"));
            status_ = reinterpret_cast<const char* (*)()>(dlsym(self, "renut_ab_status"));
            dlclose(self);
        }

        // F1/F2 are taken by other overlays, F6 by the mnk controls dialog, and
        // F7 by the game's achievements. Start/stop is a button in the panel so
        // it needs no key of its own.
        rex::ui::RegisterBind("bind_renut_ab_overlay", "F8",
            "Toggle A/B benchmark overlay", [this] {
                visible_ = !visible_;
            });
    }

    ~AbBenchmarkOverlayDialog() {
        rex::ui::UnregisterBind("bind_renut_ab_overlay");
    }

    void OnDraw(ImGuiIO& io) override {
        if (!visible_) return;

        ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Once);
        ImGui::SetNextWindowPos(ImVec2(10.0f, 320.0f), ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.85f);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_AlwaysAutoResize;

        if (!ImGui::Begin("A/B benchmark##renut_ab", &visible_, flags)) {
            ImGui::End();
            return;
        }

        if (!Available()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "Benchmark not available in this GPU plugin.");
            ImGui::TextWrapped("Run with gpu_plugin = \"renut\" in renut.toml.");
            ImGui::End();
            return;
        }

        const bool running = is_active_() != 0;
        if (ImGui::Button(running ? "Stop" : "Start", ImVec2(120.0f, 0.0f))) {
            Toggle();
        }
        ImGui::SameLine();
        if (running) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "running");
        } else {
            ImGui::TextDisabled("idle");
        }

        ImGui::Separator();
        ImGui::TextUnformatted(status_());
        ImGui::Separator();
        ImGui::TextDisabled("A = stock path, B = optimisation under test.");
        ImGui::TextDisabled("Stand somewhere busy and keep the camera still.");

        ImGui::End();
    }

private:
    bool Available() const { return start_ && stop_ && is_active_ && status_; }

    void Toggle() {
        if (!Available()) return;
        if (is_active_() != 0) {
            stop_();
        } else {
            start_();
        }
    }

    bool visible_ = false;
    void (*start_)() = nullptr;
    void (*stop_)() = nullptr;
    int (*is_active_)() = nullptr;
    const char* (*status_)() = nullptr;
};
