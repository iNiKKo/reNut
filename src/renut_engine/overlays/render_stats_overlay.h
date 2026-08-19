#pragma once

#include "imgui.h"
#include "renut_engine/Fps.h"
#include "renut_engine/game_activity_stats.h"
#include "renut_engine/nativevk_phase0.h"
#include "renut_engine/trace_stats.h"

// Content-only: no window/Begin/End, no hotkey. Hosted as a tab inside
// DebugHubOverlayDialog (see debug_hub_overlay.h), which owns the single F5
// toggle shared by all the renderer debug panels.
namespace renut::overlays::render_stats {

namespace detail {

inline bool Has(const renut::trace_stats::Snapshot& trace, const char* name) {
    return trace.values.find(name) != trace.values.end();
}

inline double Value(const renut::trace_stats::Snapshot& trace, const char* name) {
    const auto found = trace.values.find(name);
    return found == trace.values.end() ? 0.0 : found->second;
}

// Every field below is only present in trace.values if this build's
// command_processor.cpp instrumentation actually measures it (see
// RenutEmitTraceRow in trace_stats.cpp) -- an absent key means "not
// wired up", not "measured as zero". Show that distinctly instead of a
// misleading 0.00, so a genuinely idle stat and an unimplemented one
// don't look identical.
inline void Unmeasured(const char* label) {
    ImGui::TextDisabled("%-28s     n/a  (not measured)", label);
}

inline void Time(const renut::trace_stats::Snapshot& trace, const char* label, const char* value) {
    if (!Has(trace, value)) {
        Unmeasured(label);
        return;
    }
    ImGui::Text("%-28s %7.2f ms", label, Value(trace, value));
}

inline void Count(const renut::trace_stats::Snapshot& trace, const char* label, const char* value) {
    if (!Has(trace, value)) {
        Unmeasured(label);
        return;
    }
    ImGui::Text("%-28s %7.0f", label, Value(trace, value));
}

inline void Rate(const renut::trace_stats::Snapshot& trace, const char* label, const char* value) {
    if (!Has(trace, value)) {
        Unmeasured(label);
        return;
    }
    ImGui::Text("%-28s %7.1f ns", label, Value(trace, value));
}

inline void DrawTrace(const renut::trace_stats::Snapshot& trace) {
    ImGui::Separator();
    ImGui::TextUnformatted("GPU trace: latest frame");
    ImGui::TextDisabled("%s", trace.path.c_str());
    Time(trace, "Frame", "frame_ms");
    Rate(trace, "Draw submission per draw", "ns_per_draw");
    ImGui::TextDisabled("Normalised cost: compare this, not raw ms, across runs (draw count varies a lot).");
    Time(trace, "CPU blocked on GPU", "gpuwait_ms");
    Time(trace, "Draw submission", "issuedraw_ms");
    Time(trace, "Texture requests", "tex_ms");
    Time(trace, "Descriptor bindings", "bind_ms");
    Time(trace, "Vertex residency", "vfetch_ms");
    Time(trace, "Samplers", "samp_ms");
    Time(trace, "Render targets", "rt_ms");
    Time(trace, "Pipeline setup", "pipe_ms");
    Time(trace, "Present", "swap_ms");
    Time(trace, "EDRAM resolve", "issuecopy_ms");
    ImGui::Separator();
    ImGui::TextUnformatted("GPU submission visibility");
    Count(trace, "Draws submitted", "draws");
    Count(trace, "Depth-only submissions", "depthonly");
    Count(trace, "No-pixel-shader submissions", "nopixelshader");
    Count(trace, "Empty-scissor submissions", "scissor_empty");
    Count(trace, "Tiny-viewport submissions", "viewport_tiny");
    Count(trace, "Off-target submissions", "offscreen");
    Count(trace, "Small-target submissions", "smallrt");
    ImGui::TextDisabled("The last five categories can overlap; they are not summed.");
    ImGui::TextDisabled("Objects culled before draw submission need a game visibility-manager hook.");
    ImGui::Separator();
    ImGui::TextUnformatted("Phase 0: D3D9-hook rewrite validation");
    ImGui::TextDisabled("docs/ai/archive/native-renderer-rewrite-plan.md -- is D3D9-hook coverage");
    ImGui::TextDisabled("close to the real draw count, and are shader handles resolvable?");
    {
        const auto phase0 = renut::nativevk_phase0::GetLatest();
        // Session-cumulative comparison, not a single-frame snapshot -- see
        // nativevk_phase0.h's own comment for why last_frame_draws isn't
        // trustworthy (appMainDrawStart never fires; even the one working
        // hook only covers ~62% of real frames). This needs no assumption
        // that the two measurement paths share a frame boundary at all.
        ImGui::Text("%-28s %7llu", "D3D9-hook draws (session)",
            static_cast<unsigned long long>(phase0.session_total_draws));
        if (Has(trace, "session_draws_total")) {
            const double sdkDraws = Value(trace, "session_draws_total");
            const double ratio = sdkDraws > 0.0 ? double(phase0.session_total_draws) / sdkDraws * 100.0 : 0.0;
            ImGui::Text("%-28s %6.1f %%  (%.0f real)", "...as %% of SDK draws", ratio, sdkDraws);
        } else {
            Unmeasured("...as % of SDK draws");
        }
        ImGui::Text("%-28s %7llu", "SetVertexShader calls (session)",
            static_cast<unsigned long long>(phase0.session_set_vertex_shader_calls));
        const double unresolvedPct = phase0.session_set_vertex_shader_calls > 0
            ? double(phase0.session_unresolved_set_vertex_shader_calls) / double(phase0.session_set_vertex_shader_calls) * 100.0
            : 0.0;
        ImGui::Text("%-28s %7llu  (%.1f %%)", "...unresolved calls",
            static_cast<unsigned long long>(phase0.session_unresolved_set_vertex_shader_calls), unresolvedPct);
        ImGui::Text("%-28s %7llu", "Distinct handles (session)",
            static_cast<unsigned long long>(phase0.session_distinct_handles_seen));
        ImGui::Text("%-28s %7llu", "...distinct unresolved",
            static_cast<unsigned long long>(phase0.session_distinct_handles_unresolved));
        ImGui::TextDisabled("Unresolved = SetVertexShader handle never seen at CreateVertexShader");
        ImGui::TextDisabled("(the known IM_LOAD-bypass risk) -- resolvable via ucode-hash correlation");
        if (ImGui::CollapsingHeader("Phase 0 raw diagnostics (frame-boundary sanity check)")) {
            ImGui::TextDisabled("Confirmed 2026-08-19: appMainDrawStart never fires (codegen drops one");
            ImGui::TextDisabled("of two hooks sharing an address), and even appMainDrawend only covers");
            ImGui::TextDisabled("~62%% of real frames -- this is why the coverage %% above uses session-");
            ImGui::TextDisabled("cumulative totals, not a last-frame snapshot. Kept for reference.");
            ImGui::Text("%-28s %7llu", "Frame-start hook fires", static_cast<unsigned long long>(phase0.session_frame_starts));
            ImGui::Text("%-28s %7llu", "Frame-end hook fires", static_cast<unsigned long long>(phase0.session_frame_ends));
            ImGui::Text("%-28s %7llu", "Total draws (session)", static_cast<unsigned long long>(phase0.session_total_draws));
            if (Has(trace, "frame")) {
                ImGui::Text("%-28s %7.0f", "SDK real frame count", Value(trace, "frame"));
            } else {
                Unmeasured("SDK real frame count");
            }
            ImGui::TextDisabled("If frame-start/end fires are far below SDK real frame count, the");
            ImGui::TextDisabled("appMainDrawStart/end hook (0x82222250) is NOT firing once per real");
            ImGui::TextDisabled("frame despite being appMainDraw's real entry -- last_frame_draws above");
            ImGui::TextDisabled("would then span many real frames, not one, and isn't trustworthy yet.");
        }
        ImGui::TextDisabled("only if the shader DID go through CreateVertexShader at some point.");
    }
    if (ImGui::CollapsingHeader("All trace timings")) {
        Time(trace, "Primary buffer execution", "execprimary_ms");
        Time(trace, "GPU thread idle", "stall_ms");
        Time(trace, "Cross-thread callbacks", "pendingfns_ms");
        Time(trace, "Primitive processing", "prim_ms");
        Time(trace, "Shader translation", "shadertrans_ms");
        Time(trace, "Pipeline/layout bind bookkeeping", "post_ms");
        Time(trace, "Memexport range sync", "memexport_ms");
        Time(trace, "Render pass enter/barriers", "renderpass_enter_ms");
        Time(trace, "Draw command submission", "drawcmd_ms");
        Time(trace, "System constants", "sysconst_ms");
        Time(trace, "Dynamic state", "dyn_ms");
        Time(trace, "Descriptor writes", "texwrite_ms");
        Time(trace, "vkUpdateDescriptorSets", "updsets_ms");
        Count(trace, "Frame-in-flight waits", "fencewait_n");
        Time(trace, "GPU frame-in-flight wait", "fencewait_ms");
    }
    if (ImGui::CollapsingHeader("Trace counts")) {
        Count(trace, "Draws", "draws");
        Count(trace, "Render passes", "rp_n");
        Count(trace, "...small (transfer-only)", "rp_transfer");
        Count(trace, "...small (other)", "rp_small");
        Count(trace, "Render target switches", "rt_switch");
        Count(trace, "Barrier submits", "barrier_submits");
        Count(trace, "Buffer barriers", "barrier_buf");
        Count(trace, "Image barriers", "barrier_img");
        Count(trace, "Texture cache calls", "texcalls");
        Count(trace, "No-transition textures", "tex_notrans");
        Count(trace, "GPU fence waits", "gpuwait_n");
        Count(trace, "EDRAM resolves", "issuecopy_n");
        Count(trace, "Primary buffers", "execprimary_n");
        Count(trace, "GPU thread stalls", "stall_n");
        Count(trace, "Shared read/write", "use_rw");
        Count(trace, "Shared conservative rw", "use_rw_cons");
        Count(trace, "Shared read-only", "use_read");
        Count(trace, "Shared memory upload calls", "upload_calls");
        Count(trace, "...upload ranges", "upload_ranges");
        Count(trace, "...upload pages", "upload_pages");
        ImGui::TextDisabled("Each upload call transitions shared memory to transfer-dest,");
        ImGui::TextDisabled("then back to read on the next draw: a likely barrier source.");
        if (Has(trace, "rp_mpixels")) {
            ImGui::Text("%-28s %7.1f Mpx", "Pass attachment pixels", Value(trace, "rp_mpixels"));
        } else {
            Unmeasured("Pass attachment pixels");
        }
        Count(trace, "Color draws (has pixel shader)", "color_draws");
        Count(trace, "...distinct (vs, ps) pairs", "color_draw_identities");
        if (Has(trace, "color_draws") && Has(trace, "color_draw_identities")) {
            const double color_draws = Value(trace, "color_draws");
            const double identities = Value(trace, "color_draw_identities");
            const double per_identity = identities > 0.0 ? color_draws / identities : 0.0;
            ImGui::Text("%-28s %7.1f", "...draws per distinct pair", per_identity);
        } else {
            Unmeasured("...draws per distinct pair");
        }
        ImGui::TextDisabled("Distinct pairs is a proxy object/material count; low pair count");
        ImGui::TextDisabled("with high draws per pair means few objects drawn many times each");
        ImGui::TextDisabled("(normal multi-pass materials). High pair count while little is");
        ImGui::TextDisabled("visible would suggest an off-screen culling gap.");
        Count(trace, "...pairs drawn <=2x (suspicious)", "identities_low_use");
        Count(trace, "...pairs drawn >=6x (normal)", "identities_high_use");
        ImGui::TextDisabled("A large low-use count is a long tail of single-use shader pairs -");
        ImGui::TextDisabled("consistent with many off-screen objects each contributing a draw.");
        ImGui::Separator();
        Count(trace, "Phase 1 native draws (see docs/ai/research.md)", "phase1_draws");
        Time(trace, "Phase 1 native draw cost (CPU wall time)", "phase1_ms");
        ImGui::TextDisabled("Substitutes one specific vertex shader's draws with a hand-built");
        ImGui::TextDisabled("native Vulkan pipeline. CPU wall time, not isolated GPU time -");
        ImGui::TextDisabled("compare against the Xenos-translated path's ns_per_draw above.");
        Count(trace, "Generalized native draws (batch shader cache)", "xenos_draws");
        Time(trace, "Generalized native draw cost (CPU wall time)", "xenos_ms");
        ImGui::TextDisabled("Any (vertex,pixel) shader pair that successfully batch-converted");
        ImGui::TextDisabled("via XenosRecomp (tools/xenos_batch_convert.py) gets a real native");
        ImGui::TextDisabled("pipeline here -- constant buffers are not yet wired (push constants");
        ImGui::TextDisabled("are zeroed), so shaders that read constants will render with wrong");
        ImGui::TextDisabled("uniform data even though the draw mechanism itself is real.");
    }
    if (ImGui::CollapsingHeader("Draw-state counts")) {
        Count(trace, "Descriptor-set updates", "updsets_calls");
        Count(trace, "Constant writes", "consts_writes");
        Count(trace, "Vertex texture writes", "texvert_writes");
        Count(trace, "Pixel texture writes", "texpix_writes");
        Count(trace, "Same pipeline", "pipe_same");
        Count(trace, "Same vertex layout", "layout_same");
        Count(trace, "Same texture mask", "texmask_same");
        Count(trace, "Valid descriptor sets", "sets_valid");
        Count(trace, "Mergeable draws", "mergeable");
        Count(trace, "Empty scissors", "scissor_empty");
        Count(trace, "Tiny viewports", "viewport_tiny");
        Count(trace, "Offscreen draws", "offscreen");
        Count(trace, "Depth-only draws", "depthonly");
        Count(trace, "No-pixel-shader draws", "nopixelshader");
        Count(trace, "Small render targets", "smallrt");
        ImGui::TextDisabled("Offscreen/empty means submitted but cannot affect its target.");
        ImGui::TextDisabled("Game frustum-culled objects never reach this GPU trace.");
    }
}

}  // namespace detail

inline void DrawContent() {
    const auto trace = renut::trace_stats::GetLatest();
    const auto activity = renut::game_activity_stats::GetSnapshot();
    ImGui::Text("Guest tick (game logic)      %6.2f ms", cpuMS);
    ImGui::Text("Guest draw submission (appMainDraw) %6.2f ms", gpuMS);
    ImGui::TextDisabled("Whole-frame guest CPU thread time; not GPU work despite the name.");
    ImGui::Separator();
    ImGui::TextUnformatted("Game activity: frame / session");
    ImGui::Text("Animation streams    %5llu / %-8llu %6.3f ms", static_cast<unsigned long long>(activity.animation_stream_frame), static_cast<unsigned long long>(activity.animation_stream_session), activity.animation_stream_frame_ns / 1000000.0);
    ImGui::Text("Body blends          %5llu / %-8llu %6.3f ms", static_cast<unsigned long long>(activity.body_blend_frame), static_cast<unsigned long long>(activity.body_blend_session), activity.body_blend_frame_ns / 1000000.0);
    ImGui::Text("Actor scripts        %5llu / %-8llu %6.3f ms", static_cast<unsigned long long>(activity.actor_script_frame), static_cast<unsigned long long>(activity.actor_script_session), activity.actor_script_frame_ns / 1000000.0);
    ImGui::Text("Actor generation     %5llu / %-8llu %6.3f ms", static_cast<unsigned long long>(activity.actor_generation_frame), static_cast<unsigned long long>(activity.actor_generation_session), activity.actor_generation_frame_ns / 1000000.0);
    ImGui::TextDisabled("frame / session / measured guest CPU time");
    if (trace.available) {
        detail::DrawTrace(trace);
    } else {
        ImGui::TextDisabled("GPU trace unavailable: waiting for the first frame from the GPU plugin.");
    }
}

}  // namespace renut::overlays::render_stats
