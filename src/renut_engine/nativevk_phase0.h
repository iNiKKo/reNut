#pragma once

#include <cstdint>

// Phase 0 of the SDK-independent native renderer rewrite (see
// docs/ai/archive/native-renderer-rewrite-plan.md): validates, read-only,
// whether hooking D3D9 draw/state calls directly at their guest addresses
// (the new architecture's whole foundation) actually sees close to 100% of
// real draws, and whether shader identity is resolvable for draws whose
// vertex shader bypassed CreateVertexShader via IM_LOAD. Counts/logs only --
// no rendering behavior changes.
namespace renut::nativevk_phase0 {

struct Snapshot {
    // D3D9-hook draw count for the last fully-completed frame (sum of
    // DrawVertices/DrawIndexedVertices/DrawVerticesUP/DrawIndexedVerticesUP
    // hook hits between one appMainDrawStart and the next). Compare against
    // trace_stats::GetLatest()'s "draws" value (the SDK's own real Vulkan
    // draw-submission count) -- close agreement is Phase 0's coverage gate.
    uint64_t last_frame_draws = 0;

    // Session-lifetime SetVertexShader (0x8222A0A8) call count, and how many
    // of those calls carried a handle TryResolveShaderUcodeHash could NOT
    // resolve (i.e. the handle's shader never went through CreateVertexShader
    // -- the known IM_LOAD-bypass risk this phase must measure, not assume).
    uint64_t session_set_vertex_shader_calls = 0;
    uint64_t session_unresolved_set_vertex_shader_calls = 0;

    // Distinct (deduplicated) handle counts for the same measurement, since
    // a single frequently-redrawn shader would otherwise dominate the raw
    // call counts above and hide how many DISTINCT shaders are affected.
    uint64_t session_distinct_handles_seen = 0;
    uint64_t session_distinct_handles_unresolved = 0;
};

Snapshot GetLatest();

// Called from FPS.cpp's appMainDrawStart/appMainDrawend (the guest's real
// per-frame draw-submission boundary, config/renut_hooks.toml address
// 0x82222250) to scope last_frame_draws to one frame.
void FrameStart();
void FrameEnd();

// Called from this file's own global-scope hook thunks (nativevk_phase0.cpp)
// -- declared here, not file-local, so their purpose is documented next to
// the rest of this module's API.
void CountDraw();
void RecordSetVertexShader(uint32_t handle);

}
