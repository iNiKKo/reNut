#include "renut_engine/game_activity_stats.h"

#include "rex_macros.h"
#include "globals.h"

// Thunks for the 8 gameActivity* midasm_hooks (config/renut_hooks.toml) --
// the Performance tab's "Game activity" section reads their Record*/Finish*
// counters via game_activity_stats::GetSnapshot(). Split out from a larger
// file that also had unrelated, unwired native-renderer-rewrite scaffolding
// (config/renut_hooks.toml has zero hooks pointing at any of that scaffold);
// only these load-bearing thunks belong to the debug panel.

void gameActivityAnimationStream(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::RecordAnimationStream();
}

void gameActivityAnimationStreamEnd(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::FinishAnimationStream();
}

void gameActivityBodyBlend(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::RecordBodyBlend();
}

void gameActivityBodyBlendEnd(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::FinishBodyBlend();
}

void gameActivityActorScript(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::RecordActorScript();
}

void gameActivityActorScriptEnd(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::FinishActorScript();
}

void gameActivityActorGeneration(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::RecordActorGeneration();
}

void gameActivityActorGenerationEnd(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::FinishActorGeneration();
}
