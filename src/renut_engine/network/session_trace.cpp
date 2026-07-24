/**
 * @file  session_trace.cpp
 * @brief Non-invasive tracing of the title's session manager.
 *
 * Discovery demonstrably works -- a remote peer's XSessionSearchByID resolved a
 * host across the internet, wrote 1/1 results and pulled its QoS blob -- but no
 * XSessionJoinRemote ever followed, so the peer-link overrides in
 * xnet_peer_link.cpp are still unexercised. The kernel boundary cannot show why:
 * the decision happens inside the title's own session wrapper.
 *
 * These are trampolines, not replacements. DEFINE_REX_FUNC emits each
 * recompiled function as a weak alias of __imp__<name>, so a strong definition
 * here wins the link, logs, and then calls the original through __imp__. The
 * title's behaviour is unchanged.
 *
 * The session object's state word at +8 gates nearly every entry point:
 *
 *     sub_826E9B28 (create):      if ( !*(_DWORD *)(obj + 8) )
 *     sub_826EA678 (search):      if ( !*(_DWORD *)(obj + 8) )
 *     sub_826EA3D8 (searchById):  if ( !*(_DWORD *)(obj + 8) )
 *
 * 0 = idle (calls pass), 1 = pending, 2 = complete. Logging it on entry shows
 * whether a request was skipped outright because the object was busy.
 */

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

namespace {

// Reads the session object's state word (obj+8), big-endian in guest memory.
uint32_t SessionState(uint32_t obj, uint8_t* base) {
  if (!obj) return 0xFFFFFFFFu;
  return rex::memory::load_and_swap<uint32_t>(
      rex::memory::GuestPtr<uint8_t*>(base, obj + 8));
}

}  // namespace

REX_EXTERN(__imp__sub_826E9B28);
REX_EXTERN(__imp__sub_826EA020);
REX_EXTERN(__imp__sub_826EA090);
REX_EXTERN(__imp__sub_826EA3D8);
REX_EXTERN(__imp__sub_826EA678);
REX_EXTERN(__imp__sub_8227DB58);

// XSessionCreate wrapper. Gated on state == 0.
REX_HOOK_RAW(sub_826E9B28) {
  const uint32_t obj = ctx.r3.u32;
  REXLOG_INFO("[renut/sess] create      obj={:08X} state={} user={}", obj,
              SessionState(obj, base), ctx.r4.u32);
  __imp__sub_826E9B28(ctx, base);
}

// XSessionSearch / XSessionSearchEx wrapper. Gated on state == 0.
REX_HOOK_RAW(sub_826EA678) {
  const uint32_t obj = ctx.r3.u32;
  REXLOG_INFO("[renut/sess] search      obj={:08X} state={} user={} procs={}", obj,
              SessionState(obj, base), ctx.r4.u32, ctx.r7.u32);
  __imp__sub_826EA678(ctx, base);
  REXLOG_INFO("[renut/sess] search      -> {}", ctx.r3.u32);
}

// XSessionSearchByID wrapper. Gated on state == 0. Takes either a local-cache
// fast path (list at 0x82FAFE98) or the real XSessionSearchByID; the async arm
// only advances to state 1 when the second call returns 997 ERROR_IO_PENDING,
// otherwise the failure vtable slot (+88) runs instead.
REX_HOOK_RAW(sub_826EA3D8) {
  const uint32_t obj = ctx.r3.u32;
  REXLOG_INFO("[renut/sess] searchById  obj={:08X} state={} user={}", obj,
              SessionState(obj, base), ctx.r4.u32);
  __imp__sub_826EA3D8(ctx, base);
  REXLOG_INFO("[renut/sess] searchById  -> {} state={}", ctx.r3.u32,
              SessionState(obj, base));
}

// XSessionJoinLocal wrapper.
REX_HOOK_RAW(sub_826EA020) {
  REXLOG_INFO("[renut/sess] joinLocal   r3={:08X} r4={:08X}", ctx.r3.u32, ctx.r4.u32);
  __imp__sub_826EA020(ctx, base);
  REXLOG_INFO("[renut/sess] joinLocal   -> {}", ctx.r3.u32);
}

// XSessionJoinRemote wrapper -- the call that never fired in the paired logs.
// Returns 0 early without calling XSessionJoinRemote when the results buffer
// pointer is null, so a silent 0 here is itself a useful signal.
REX_HOOK_RAW(sub_826EA090) {
  REXLOG_INFO("[renut/sess] joinRemote  r3={:08X}:{:08X} r4={:08X} r5={:08X}",
              ctx.r3.u32, static_cast<uint32_t>(ctx.r3.u64 >> 32), ctx.r4.u32,
              ctx.r5.u32);
  __imp__sub_826EA090(ctx, base);
  REXLOG_INFO("[renut/sess] joinRemote  -> {}", ctx.r3.u32);
}

// Peer link setup: XNetXnAddrToInAddr followed by XNetConnect. Reaching this at
// all means the title committed to contacting a specific host.
REX_HOOK_RAW(sub_8227DB58) {
  REXLOG_INFO("[renut/sess] peerConnect r3={:08X} r4={:08X}", ctx.r3.u32, ctx.r4.u32);
  __imp__sub_8227DB58(ctx, base);
  REXLOG_INFO("[renut/sess] peerConnect -> {}", ctx.r3.u32);
}
