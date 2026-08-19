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
    // D3D9-hook draw count between the two most recent appMainDrawend hook
    // fires (see FrameEnd()'s own comment for real, confirmed problems with
    // treating this as "one real frame" -- appMainDrawStart never fires at
    // all, and even the one working hook only covers ~62% of the SDK's own
    // real frame count in testing). Kept as a rough diagnostic only --
    // PREFER session_total_draws vs trace_stats::GetLatest()'s
    // "session_draws_total" (both session-cumulative) for the actual Phase 0
    // coverage gate, since that comparison needs no frame-alignment
    // assumption at all.
    uint64_t last_frame_draws = 0;

    // Raw diagnostic counters (2026-08-19): confirmed via
    // `grep appMainDrawStart generated/*.cpp` (zero matches) that rexglue's
    // codegen silently drops one of two midasm_hook entries sharing an
    // address -- appMainDrawStart never fires, only appMainDrawend does, and
    // even that one only fired 8765 times against the SDK's own 14218 real
    // frames in one session (~62%). session_total_draws is the reliable
    // session-cumulative D3D9-hook draw count -- compare it against
    // trace_stats' own session-cumulative "session_draws_total".
    uint64_t session_frame_starts = 0;
    uint64_t session_frame_ends = 0;
    uint64_t session_total_draws = 0;

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
