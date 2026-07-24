/**
 * @file  throw_trace.cpp
 * @brief Identify the guest C++ throw that kills the process mid-session.
 *
 * The session now connects, runs for ~20s, then the process dies with no
 * [CRASH] line -- because it is not a guest access violation. Windows records:
 *
 *     Faulting module: rexruntimerd.dll
 *     Exception code:  0x80000003        (STATUS_BREAKPOINT)
 *     Fault offset:    0xaa370
 *
 * ReXGlue breaks unconditionally on a guest-raised exception, since unwinding
 * is unimplemented:
 *
 *     void RtlRaiseException_entry(...) {
 *       case 0xE06D7363: { HandleCppException(record); return; }   // and that
 *       ...                                                        // Breaks too
 *       rex::debug::Break();
 *     }
 *
 * So any C++ throw from the title is fatal here. _CxxThrowException has 87 call
 * sites, and the peer map's "invalid map/set<T> iterator" checks are among
 * them -- the tw/td traps seen earlier are the same family of STL assertion.
 *
 * This logs the throwing call site (guest LR) so the responsible code can be
 * found in IDA, then calls through unchanged. It does not attempt to swallow
 * the throw: without unwinding the title would continue with a half-destroyed
 * object graph, which is worse than the break.
 */

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>

REX_EXTERN(__imp__sub_82BB9B78);

// _CxxThrowException(void* object, _ThrowInfo* info)
REX_HOOK_RAW(sub_82BB9B78) {
  REXLOG_ERROR("[renut/throw] guest C++ throw from {:08X} (object={:08X} info={:08X}) -- ReXGlue "
               "will break on this",
               rex::ppc::GetGuestCallerAddress(), ctx.r3.u32, ctx.r4.u32);
  __imp__sub_82BB9B78(ctx, base);
}
