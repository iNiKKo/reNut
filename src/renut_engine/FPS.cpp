#include <rex/logging.h>
#include "Fps.h"
#include <renut_engine/hooks.h>
#include <renut_engine/Timer.h>
#include <renut_engine/game_activity_stats.h>
#include <renut_engine/nativevk_phase0.h>
#include <rex/hook.h>

//CPU Time
REX_EXTERN(__imp__appMainTickPreDraw);
REX_HOOK_RAW(appMainTickPreDraw){
    Timer timer;
    timer.start();
    __imp__appMainTickPreDraw(ctx, base);
    timer.stop();
    cpuMS = timer.elapsedMilliseconds();
    auto fpshook = fpsManager.GetCreateCounter("Tick");
    fpshook->Tick();
}

//GPU Time
void renutFrameLimit();             // frameHooks.cpp: our low-overhead frame cap ("vsync")
void renutApplyShaderCompileMode(); // frameHooks.cpp: sync-compile toggle (black-flash fix)

REX_EXTERN(__imp__appMainDraw);
REX_HOOK_RAW(appMainDraw){
    static uint32_t drawTicks = 0;
    if (++drawTicks <= 6 || (drawTicks % 300) == 0) {
        REXLOG_INFO("game: appMainDraw tick {}", drawTicks);
    }
    // Keep the engine's shader-compile mode in sync with our toggle (cheap; only
    // touches the engine cvar when the toggle actually changes).
    renutApplyShaderCompileMode();

    Timer timer;
    timer.start();
    __imp__appMainDraw(ctx, base);
    timer.stop();
    gpuMS = timer.elapsedMilliseconds();

    // Pace the frame after the draw is submitted so the GPU renders fewer frames
    // instead of running uncapped. No-op while frame_cap is "Off".
    renutFrameLimit();
}

void FPSCounter::Tick(){
    auto Time = std::chrono::steady_clock::now();
    std::chrono::duration<double, std::milli> delta = Time - lastTick;
    lastTick = Time;
    float ms = static_cast<float>(delta.count());
    frameTimes.push_back(ms);
    if (frameTimes.size() > AverageCount) {
        frameTimes.erase(frameTimes.begin());
    }
    float total = 0.0f;
    for (float f : frameTimes) {
        total += f;
    }
    averageMs = total / frameTimes.size();
    averageFps = 1000.0f / averageMs;
}

// Hooked from config/renut_hooks.toml (frame-timing instrumentation points
// used by the native-renderer trace panel, see trace_stats.cpp). Also the
// real per-frame boundary for nativevk_phase0's D3D9-hook draw-count
// validation (docs/ai/archive/native-renderer-rewrite-plan.md Phase 0) --
// this wraps the guest's ENTIRE draw submission (REX_HOOK_RAW(appMainDraw)
// above), matching exactly what the SDK's own real "draws" trace stat
// (trace_stats.cpp) is scoped to, so the two counts are directly comparable.
void appMainDrawStart() {
    renut::nativevk_phase0::FrameStart();
}

void appMainDrawend() {
    renut::nativevk_phase0::FrameEnd();

    // Real fix (2026-08-19): game_activity_stats::EndFrame() (the only thing
    // that copies the per-event Record*/Finish* counters into the snapshot
    // the Performance tab actually reads) was only ever called from
    // render_hooks_stub.cpp's nativeSwap() -- confirmed dead scaffolding,
    // zero midasm_hook wiring anywhere (see docs/ai/archive/
    // native-renderer-rewrite-plan.md). The counters themselves were
    // incrementing correctly the whole time; the displayed snapshot just
    // never refreshed from its initial all-zero state. appMainDrawend is a
    // real, working hook (~62% of real frames, see nativevk_phase0.cpp's own
    // comment) -- not perfectly 1:1 per frame, but far better than never.
    renut::game_activity_stats::EndFrame();
}

void appMainTickPreDrawStart() {
}

void appMainTickPreDrawend() {
}

