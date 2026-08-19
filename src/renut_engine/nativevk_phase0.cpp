#include "renut_engine/nativevk_phase0.h"
#include "renut_engine/shader_dump.h"

#include <atomic>
#include <mutex>
#include <unordered_set>

namespace renut::nativevk_phase0 {

namespace {

std::atomic<uint64_t> g_frameDrawCount{0};
std::atomic<uint64_t> g_lastFrameDrawCount{0};
std::atomic<uint64_t> g_sessionFrameStarts{0};
std::atomic<uint64_t> g_sessionFrameEnds{0};
std::atomic<uint64_t> g_sessionTotalDraws{0};

std::atomic<uint64_t> g_sessionSetVertexShaderCalls{0};
std::atomic<uint64_t> g_sessionUnresolvedCalls{0};

std::mutex g_handleMutex;
std::unordered_set<uint32_t> g_handlesSeen;
std::unordered_set<uint32_t> g_handlesUnresolved;

}  // namespace

// Called from d3d9_draw_stats.cpp / shader_dump.cpp's own hook thunks (see
// this file's closing comment for why) -- kept internal-linkage-free (not
// in the anonymous namespace above) so those other TUs can reach them.
void CountDraw() {
    g_frameDrawCount.fetch_add(1, std::memory_order_relaxed);
    g_sessionTotalDraws.fetch_add(1, std::memory_order_relaxed);
}

void RecordSetVertexShader(uint32_t handle) {
    g_sessionSetVertexShaderCalls.fetch_add(1, std::memory_order_relaxed);

    uint64_t hash = 0;
    const bool resolved = renut::shader_dump::TryResolveShaderUcodeHash(handle, hash);
    if (!resolved) {
        g_sessionUnresolvedCalls.fetch_add(1, std::memory_order_relaxed);
    }

    std::lock_guard<std::mutex> lock(g_handleMutex);
    if (g_handlesSeen.insert(handle).second) {
        if (!resolved) g_handlesUnresolved.insert(handle);
    } else if (resolved) {
        g_handlesUnresolved.erase(handle);
    }
}

Snapshot GetLatest() {
    Snapshot snapshot;
    snapshot.last_frame_draws = g_lastFrameDrawCount.load(std::memory_order_relaxed);
    snapshot.session_frame_starts = g_sessionFrameStarts.load(std::memory_order_relaxed);
    snapshot.session_frame_ends = g_sessionFrameEnds.load(std::memory_order_relaxed);
    snapshot.session_total_draws = g_sessionTotalDraws.load(std::memory_order_relaxed);
    snapshot.session_set_vertex_shader_calls = g_sessionSetVertexShaderCalls.load(std::memory_order_relaxed);
    snapshot.session_unresolved_set_vertex_shader_calls = g_sessionUnresolvedCalls.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_handleMutex);
    snapshot.session_distinct_handles_seen = g_handlesSeen.size();
    snapshot.session_distinct_handles_unresolved = g_handlesUnresolved.size();
    return snapshot;
}

// Wired from FPS.cpp's appMainDrawStart/appMainDrawend (config/renut_hooks.toml,
// address 0x82222250, appMainDraw's real entry point per config/renut_funcs.toml).
//
// Real, confirmed bug (2026-08-19, playtest diagnostics): appMainDrawStart
// NEVER fires -- confirmed via `grep appMainDrawStart generated/*.cpp`
// finding zero matches, only appMainDrawend. rexglue's codegen silently
// drops one of two midasm_hook entries sharing the same address (same
// pattern confirmed on the sibling appMainTickPreDrawStart/end pair -- only
// "end" survives there too). So FrameStart() below is effectively dead code
// today (kept in case a future codegen fix makes the pair work again, which
// would just make these two calls redundant, not wrong). FrameEnd() alone
// is made self-contained (atomic exchange, not separate load+store) so it
// works correctly whether or not FrameStart ever actually runs alongside it.
//
// Second confirmed issue: even the one working hook only fired 8765 times
// against the SDK's own 14218 real frames in the same session (~62%) -- this
// address is NOT a reliable 1:1-with-real-frames marker (likely a guest
// tick-rate/frame-pacing mismatch, not another codegen bug). That makes any
// single "last frame" snapshot fundamentally unreliable here, not just
// broken by the dead-hook bug above -- see Snapshot::last_frame_draws and
// the diagnostics fields' own comments. GetLatest() callers should prefer
// comparing session_total_draws against trace_stats' session_draws_total
// (both cumulative, no frame-alignment assumption needed) over
// last_frame_draws for the actual Phase 0 go/no-go call.
void FrameStart() {
    g_sessionFrameStarts.fetch_add(1, std::memory_order_relaxed);
}

void FrameEnd() {
    g_sessionFrameEnds.fetch_add(1, std::memory_order_relaxed);
    g_lastFrameDrawCount.store(g_frameDrawCount.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
}

}  // namespace renut::nativevk_phase0

// Real fix (2026-08-19): this file used to register its OWN midasm_hook
// entries (phase0Draw*_hook at the four D3D9 draw addresses,
// phase0SetVertexShader_hook at SetVertexShader) alongside other hooks
// already present at those exact same addresses. Confirmed via a full
// config/renut_hooks.toml sweep that rexglue's codegen only keeps the
// LAST-defined midasm_hook when two entries share the same
// (address, after_instruction) pair -- silently dropping the earlier one
// entirely (dumpVertexShaderSet_hook and the four original draw-count
// paths were all dead as a result, not just this one). Fixed by removing
// those duplicate entries and having the single surviving hook at each
// address call into both subsystems instead:
//   - SetVertexShader (0x8222A0A8): dumpVertexShaderSet_hook
//     (shader_dump.cpp) now also calls RecordSetVertexShader() directly.
//   - The four draw addresses: d3d9DrawVertices_hook and friends
//     (d3d9_draw_stats.cpp) now also call CountDraw() directly.
// CountDraw()/RecordSetVertexShader() stay part of this namespace's public
// API for that reason -- no longer called from a hook thunk in this file.
