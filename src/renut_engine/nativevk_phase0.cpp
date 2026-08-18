#include "renut_engine/nativevk_phase0.h"
#include "renut_engine/shader_dump.h"

#include <atomic>
#include <mutex>
#include <unordered_set>

#include <rex/ppc.h>

namespace renut::nativevk_phase0 {

namespace {

std::atomic<uint64_t> g_frameDrawCount{0};
std::atomic<uint64_t> g_lastFrameDrawCount{0};

std::atomic<uint64_t> g_sessionSetVertexShaderCalls{0};
std::atomic<uint64_t> g_sessionUnresolvedCalls{0};

std::mutex g_handleMutex;
std::unordered_set<uint32_t> g_handlesSeen;
std::unordered_set<uint32_t> g_handlesUnresolved;

}  // namespace

// Called from the global-scope hook thunks at the bottom of this file --
// kept internal-linkage-free (not in the anonymous namespace above) so
// those thunks can reach them.
void CountDraw() {
    g_frameDrawCount.fetch_add(1, std::memory_order_relaxed);
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
    snapshot.session_set_vertex_shader_calls = g_sessionSetVertexShaderCalls.load(std::memory_order_relaxed);
    snapshot.session_unresolved_set_vertex_shader_calls = g_sessionUnresolvedCalls.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_handleMutex);
    snapshot.session_distinct_handles_seen = g_handlesSeen.size();
    snapshot.session_distinct_handles_unresolved = g_handlesUnresolved.size();
    return snapshot;
}

// Wired from FPS.cpp's appMainDrawStart/appMainDrawend (config/renut_hooks.toml,
// address 0x82222250 -- already wraps the guest's ENTIRE per-frame draw
// submission, see REX_HOOK_RAW(appMainDraw) in FPS.cpp). Using this existing
// frame boundary rather than Present/Swap keeps Phase 0's draw count scoped
// to exactly the same span the SDK's own real per-frame "draws" trace stat
// (trace_stats.cpp) covers, so the two numbers are directly comparable.
void FrameStart() {
    g_frameDrawCount.store(0, std::memory_order_relaxed);
}

void FrameEnd() {
    g_lastFrameDrawCount.store(g_frameDrawCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

}  // namespace renut::nativevk_phase0

// Hook thunks below are deliberately global-scope (not inside the namespace
// above) -- every other midasm_hook target in this codebase (shader_dump.cpp,
// FPS.cpp, hooks.cpp) is a plain global function, matched by name against
// config/renut_hooks.toml's [[midasm_hook]] "name" field.

// Read-only hooks on the four D3D9 draw entry points (config/renut_gpu_funcs.toml):
// DrawVertices (0x8222C7A0), DrawIndexedVertices (0x826567C8), DrawVerticesUP
// (0x8222E520), DrawIndexedVerticesUP (0x82656728). Just count -- no register
// values needed, so registers = [] in config/renut_hooks.toml and these take
// no PPCRegister arguments.
void phase0DrawVertices_hook() { renut::nativevk_phase0::CountDraw(); }
void phase0DrawIndexedVertices_hook() { renut::nativevk_phase0::CountDraw(); }
void phase0DrawVerticesUP_hook() { renut::nativevk_phase0::CountDraw(); }
void phase0DrawIndexedVerticesUP_hook() { renut::nativevk_phase0::CountDraw(); }

// A second, independent midasm_hook at the SAME address as
// dumpVertexShaderSet_hook (0x8222A0A8, shader_dump.cpp) -- confirmed
// supported by rexglue's hook codegen (config/renut_hooks.toml already has
// other duplicate-address pairs, e.g. 0x82222250's appMainDrawStart/end).
// Deliberately NOT reusing dumpVertexShaderSet_hook itself: that function
// (and everything it does -- dumpObjectHex, vertdecl-usage dumping) is
// gated behind the unrelated renut_dump_vertex_decl cvar, so Phase 0
// measurement must not depend on a user having that dump feature on.
void phase0SetVertexShader_hook(PPCRegister&, PPCRegister& r4) {
    renut::nativevk_phase0::RecordSetVertexShader(r4.u32);
}
