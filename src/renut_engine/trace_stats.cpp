#include "renut_engine/trace_stats.h"

#include <mutex>

#include "renut_engine/renut_trace_hook.h"

namespace renut::trace_stats {

namespace {
std::mutex g_mutex;
Snapshot g_latest;
}

Snapshot GetLatest() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_latest;
}

// Strong definition of the weak hook declared in renut_trace_hook.h: the
// nativevk/xenos plugin (loaded into this same process) calls this once per
// frame with everything it measured. Must be exported to the plugin's
// dynamic symbol table (see CMakeLists.txt's --export-dynamic-symbol) so its
// PLT entry resolves here instead of staying null. extern "C" keeps the name
// unmangled to match renut_trace_hook.h's declaration exactly.
extern "C" void RenutEmitTraceRow(const RenutTraceRow* row) {
    if (!row) {
        return;
    }
    Snapshot snapshot;
    snapshot.available = true;
    snapshot.path = "(live)";
    auto& values = snapshot.values;
    values.reserve(32);
    values["frame"] = double(row->frame);
    values["draws"] = double(row->draws);
    values["frame_ms"] = row->frame_ms;
    values["issuedraw_ms"] = row->issuedraw_ms;
    values["ns_per_draw"] = row->ns_per_draw;
    values["prim_ms"] = row->prim_ms;
    values["samp_ms"] = row->samp_ms;
    values["tex_ms"] = row->tex_ms;
    values["rt_ms"] = row->rt_ms;
    values["pipe_ms"] = row->pipe_ms;
    values["shadertrans_ms"] = row->shadertrans_ms;
    values["sysconst_ms"] = row->sysconst_ms;
    values["vfetch_ms"] = row->vfetch_ms;
    values["dyn_ms"] = row->dyn_ms;
    values["bind_ms"] = row->bind_ms;
    values["memexport_ms"] = row->memexport_ms;
    values["renderpass_enter_ms"] = row->renderpass_enter_ms;
    values["drawcmd_ms"] = row->drawcmd_ms;
    values["gpuwait_ms"] = row->gpuwait_ms;
    values["gpuwait_n"] = double(row->gpuwait_n);
    values["swap_ms"] = row->swap_ms;
    values["issuecopy_ms"] = row->issuecopy_ms;
    values["issuecopy_n"] = double(row->issuecopy_n);
    values["depthonly"] = double(row->depthonly);
    values["nopixelshader"] = double(row->nopixelshader);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_latest = std::move(snapshot);
}

}
