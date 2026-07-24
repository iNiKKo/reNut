// =============================================================================
// fsr_menu_hint.cpp
//
// Shows the game's own frontend message box the first time the player reaches
// the main menu (the Single Player / Multiplayer chooser), telling them they can
// enable FSR 3 in the F4 menu if their GPU supports it.
//
// How it works (all addresses from IDA):
//   * sub_823E9AB0 - the create-by-index scene factory. Indexes the descriptor
//       table at unk_82E56A90 (288-byte stride) by a2 = scene index, then calls
//       XuiSceneCreate. Every navigation path funnels through here (the
//       navigate-first wrapper sub_823E9BA8 is only one caller), so it is the
//       reliable place to detect a screen appearing. Screen index also selects
//       the parallel scene-name table at 0x82E5E038 (confirmed: MsgBox = 94
//       matches the literal in sub_82577510; name[68] = the MainMenu string).
//       A midasm hook on its entry (fsrHint_sceneDetect, r4 = a2 = scene index)
//       just arms a flag when it sees the main-menu screen -- we do NOT show the
//       box here, to avoid re-entering scene creation mid-build.
//   * sub_82577510(mgr, ctx2, wideBody, buttonCfg, cb, cbArg) - the frontend
//       message-box helper (the same one the game uses for "not connected", "no
//       storage", ...). It copies the body text immediately (via sub_829C87A0)
//       and builds the button set from buttonCfg, so a stack-scratch string is
//       safe. mgr = dword_82FAD9F0[0], ctx2 = dword_82FAC7AC.
//
// The armed flag is consumed once per launch from the per-frame pre-draw tick
// (renutFsrHintTick, called from appMainTickPreDrawStart), where we have a valid
// guest stack to carve scratch from and the frontend is ready to accept a new
// screen.
// =============================================================================

#include <rex/hook.h>
#include "rex_macros.h"
#include "renut_logging.h"

#include <cstdint>

#if defined(_MSC_VER)
#include <stdlib.h>
#define RENUT_BSWAP32(x) _byteswap_ulong(x)
#else
#define RENUT_BSWAP32(x) __builtin_bswap32(x)
#endif

// -----------------------------------------------------------------------------
// Guest globals / constants (guest memory is big-endian)
// -----------------------------------------------------------------------------
// Scene index for the main menu -- the screen where you pick Single Player /
// Multiplayer. Confirmed by logging every created scene id on a real run:
// XuiScene_BanjoX_Frontend_StartMenu = 70 (created right before the game's own
// sign-in MsgBox over the same screen). NOT 68 (Frontend_MainMenu), which never
// loads. If the box ever pops on the wrong screen, this is the only value to
// change (71 = StartScreen "Press Start", 95 = FullButtonMenu).
static constexpr uint32_t kMainMenuSceneId = 70;

static constexpr uint32_t kFrontendMgrArray = 0x82FAD9F0;  // dword_82FAD9F0[]; [0] = manager
static constexpr uint32_t kFrontendMsgCtx   = 0x82FAC7AC;  // dword_82FAC7AC (msgbox context arg)

static const char* kFsrHintText =
    "Tip: if your graphics card supports it, you can enable FSR 3 in the "
    "F4 menu by enabling present fsr3 frame generation, and changing present effect " 
    "to fsr3 for a big performance boost.";

// The frontend message-box helper: sub_82577510(mgr, ctx2, wideBody, buttonCfg,
// callback, callbackArg). Wrapper is called as (frame, base, <guest args...>).
REX_IMPORT(sub_82577510, RenutShowFrontendMsgBox,
           uint32_t(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t));

// -----------------------------------------------------------------------------
// Guest memory helpers
// -----------------------------------------------------------------------------
static inline uint32_t ReadGuestBE32(uint8_t* base, uint32_t gaddr) {
  return RENUT_BSWAP32(*reinterpret_cast<uint32_t*>(base + gaddr));
}

// Write an ASCII string as a UTF-16BE, NUL-terminated wide string into guest
// memory (the body text the message box expects).
static void WriteWideBE(uint8_t* base, uint32_t gaddr, const char* s) {
  uint8_t* d = base + gaddr;
  size_t i = 0;
  for (; s[i]; ++i) {
    d[i * 2 + 0] = 0x00;
    d[i * 2 + 1] = static_cast<uint8_t>(s[i]);
  }
  d[i * 2 + 0] = 0x00;
  d[i * 2 + 1] = 0x00;
}

// Write an ASCII string as a narrow, NUL-terminated string into guest memory
// (the button-config id, e.g. "ok").
static void WriteNarrow(uint8_t* base, uint32_t gaddr, const char* s) {
  uint8_t* d = base + gaddr;
  size_t i = 0;
  for (; s[i]; ++i) d[i] = static_cast<uint8_t>(s[i]);
  d[i] = 0x00;
}

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------
static bool g_hintPending = false;  // armed when the main-menu screen is created
static bool g_hintShown   = false;  // once per launch

// -----------------------------------------------------------------------------
// Midasm hook: frontend screen factory entry (sub_823E9BA8), r4 = scene id.
// Arms the hint the first time the main-menu screen is created.
// -----------------------------------------------------------------------------
void fsrHint_sceneDetect(PPCRegister& r4) {
  if (!g_hintShown && r4.u32 == kMainMenuSceneId) {
    g_hintPending = true;
    RNUT_INFO("fsrHint: main-menu (StartMenu, id={}) created -- arming hint", r4.u32);
  }
}

// -----------------------------------------------------------------------------
// Per-frame (pre-draw) tick: show the message box once the frontend is ready.
// Called from appMainTickPreDrawStart().
// -----------------------------------------------------------------------------
void renutFsrHintTick() {
  if (!g_hintPending || g_hintShown)
    return;

  REX_PPC_MEMBASE_PTR(base);

  const uint32_t mgr = ReadGuestBE32(base, kFrontendMgrArray);  // dword_82FAD9F0[0]
  const uint32_t ctx2 = ReadGuestBE32(base, kFrontendMsgCtx);

  // DIAGNOSTIC: report the context we resolved (remove once verified).
  static bool loggedOnce = false;
  if (!loggedOnce) {
    loggedOnce = true;
    RNUT_INFO("fsrHint: pending fire, mgr={:#x} ctx2={:#x}", mgr, ctx2);
  }

  if (!mgr)
    return;  // frontend manager not up yet -- keep the flag and retry next frame

  REX_PPC_CONTEXT_REF(ctx);

  // Carve a small scratch buffer off the guest stack for the strings. Both the
  // body (copied by sub_829C87A0) and the button id (consumed by sub_82571CF0)
  // are used before this call returns, so the scratch only needs to live for the
  // duration of the call.
  const uint32_t savedR1 = ctx.r1.u32;
  ctx.r1.u32 = (ctx.r1.u32 - 0x200) & ~15u;
  const uint32_t bodyAddr = ctx.r1.u32;
  const uint32_t btnAddr  = bodyAddr + 0x180;  // narrow "ok", well clear of the body

  WriteWideBE(base, bodyAddr, kFsrHintText);
  WriteNarrow(base, btnAddr, "ok");

  {
    rex::CallFrame frame{ctx};
    RenutShowFrontendMsgBox(frame, base, mgr, ctx2, bodyAddr, btnAddr, 0, 0);
  }

  ctx.r1.u32 = savedR1;

  g_hintShown = true;
  g_hintPending = false;
}
