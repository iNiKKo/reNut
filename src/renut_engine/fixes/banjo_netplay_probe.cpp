#include <rex/hook.h>
#include <rex/system/xsession.h>
#include "generated/renut_init.h"
#include "../renut_logging.h"

// ---------------------------------------------------------------------------
// Banjo netplay — public->private session-migration fix (host side)
// ---------------------------------------------------------------------------
// When a peer joins, Banjo (like most Live titles) migrates from a public
// matchmaking session to a private game session, keeping BOTH live briefly then
// leaving the public one. The two instances behave asymmetrically:
//
//   * JOINER resolves the host's XNADDR once per session via XNetXnAddrToInAddr,
//     passing each session's XNKID. Different XNKID -> different synthetic
//     in_addr token -> a DISTINCT peer connection per session. Leaving the
//     public session correctly destroys only the public connection; the private
//     session keeps its own. The joiner is already fine (the retire override
//     below covers the transient dangling public tick object).
//
//   * HOST learns the joiner from INCOMING packets (XNetAddrCache::
//     TokenForIncomingPeer, keyed by IP with no XNKID) -> ONE token -> ONE
//     shared peer connection reused by BOTH sessions. When the public session's
//     per-peer object is destroyed during the migration, its destructor
//     (sub_826AFED0) unconditionally destroys that connection (sub_8268F640,
//     no refcount) — but the PRIVATE session's per-peer tick object still holds
//     the same handle, so its next tick derefs the freed connection and the
//     host crashes (~1s after the join, guest_addr 0x24).
//
// Fix: on the HOST only, skip the connection destroy when it is driven by the
// per-peer object destructor (sub_826AFED0, the call at 0x826AFF10 -> return
// 0x826AFF14). The shared connection stays live in the map so the private
// session keeps ticking it. On the joiner AnyHostSessionActive() is false, so
// the normal per-session teardown runs untouched.
//
// LIMITATION: this host gate keeps the shared connection alive for the whole
// time the host has any session (fine for the 2-player case — one peer). A
// >2-player host, or a peer that legitimately leaves mid-match, would leak /
// keep a stale connection. The principled fix is a per-handle refcount of live
// per-peer objects (destroy only when the last session releases the handle);
// this gate is the minimal, evidence-backed step first (renut_011 showed the
// host survives + keeps the connection when this destroy is skipped).

REX_EXTERN(__imp__sub_826B24F8);   // per-peer connection tick
REX_EXTERN(__imp__sub_82693520);   // connection-map find (returns value or 0)
REX_EXTERN(__imp__sub_8268F640);   // connection destroy (map[handle] -> free)
REX_EXTERN(__imp__sub_826B89C0);   // connect state-machine ABORT/FAIL transition

namespace {
constexpr uint32_t kHandleOff     = 0x28;         // per-peer object handle field
constexpr uint32_t kDeadHandle    = 0xFFFFFFFFu;  // -1: "no connection"
constexpr uint32_t kMemberDtorRet = 0x826AFF14;   // LR after sub_8268F640 in sub_826AFED0
}  // namespace

// Host-side: keep the shared peer connection alive across the public-session
// teardown so the private session (which reuses the same connection) survives.
REX_HOOK_RAW(sub_8268F640) {
  const uint32_t handle = ctx.r3.u32;

  if (ctx.lr == kMemberDtorRet && rex::system::AnyHostSessionActive()) {
    RNUT_INFO("banjo: host keeping shared peer connection (handle {:08X}) alive "
              "across public-session teardown", handle);
    ctx.r3.u32 = 0;  // sub_826AFED0 ignores the return; leave the map intact
    return;
  }

  __imp__sub_8268F640(ctx, base);
}

// DIAGNOSTIC (temporary): the connect state machine sub_822009B0(obj) drives a
// session's peer readiness; obj+0x12C (=+300) is the state (1..7). When a wait
// state (2/3/4) times out it calls this FAIL transition sub_826B89C0(obj, err),
// which tears down the members and makes the game delete the (private) session
// -> back to lobby. Logging every call names the exact abort: err 17/18/19 tells
// which readiness state timed out (send gap => fixable by giving the host the
// joiner address; a different/earlier abort => receive/attribution gap). a1=obj
// (r3), a2=errcode (r4). Remove once the A-vs-B question is answered.
REX_HOOK_RAW(sub_826B89C0) {
  const uint32_t obj     = ctx.r3.u32;
  const uint32_t errcode = ctx.r4.u32;
  const uint32_t state   = obj ? REX_LOAD_U32(obj + 0x12C) : 0xFFFFFFFFu;
  RNUT_INFO("banjo readiness ABORT: obj={:08X} state(+0x12C)={} errcode={} caller_lr={:08X}",
            obj, state, errcode, static_cast<uint32_t>(ctx.lr));
  __imp__sub_826B89C0(ctx, base);
}

// Safety net (joiner + any path we don't skip): if a per-peer tick object's
// connection handle no longer resolves in the map, retire the object (-1) so its
// own `handle == -1` guards no-op it instead of dereferencing freed memory.
REX_HOOK_RAW(sub_826B24F8) {
  const uint32_t self   = ctx.r3.u32;
  const uint32_t handle = self ? REX_LOAD_U32(self + kHandleOff) : kDeadHandle;

  if (handle != kDeadHandle) {
    const uint64_t saved_lr = ctx.lr;
    ctx.r3.u32 = handle;
    __imp__sub_82693520(ctx, base);
    const uint32_t found = ctx.r3.u32;
    ctx.r3.u32 = self;
    ctx.lr      = saved_lr;

    if (found == 0) {
      REX_STORE_U32(self + kHandleOff, kDeadHandle);
      RNUT_INFO("banjo: peer connection handle {:08X} gone from map — retiring stale "
                "tick object {:08X} (a1+0x28=-1)", handle, self);
    }
  }

  __imp__sub_826B24F8(ctx, base);
}
