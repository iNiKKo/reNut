/**
 * @file  transport_trace.cpp
 * @brief Trace the VDP transport, which is silently refusing to send.
 *
 * Both peers now resolve each other correctly -- client join path, host
 * inaOnline correct, XNetXnAddrToInAddr -> AB000002 -- and then sit in exactly
 * five seconds of total silence before tearing the session down. No socket, no
 * bind, and (proven by the remap hook never running) no WSASendTo at all. The
 * transport is bailing before it ever transmits.
 *
 * sub_82691550 has three silent early-outs, none of which log anything:
 *
 *     if ( XNetInAddrToXnAddr(...) )        -> fail
 *     v3 = sub_8268FC60(...);
 *     if ( v3 >= 0 ) return v3;             // already have a peer object
 *     if ( !byte_82FA390D ) return v3;      // transport never started
 *     v4 = sub_826933C8();
 *     if ( v4 >= 0 ) { ...socket/bind/send... }   // no free slot -> nothing
 *
 * byte_82FA390D is set to 1 only by sub_8268EAD0, which opens the VDP socket
 * (AF_INET, SOCK_DGRAM, IPPROTO_VDP=254) and binds port 1000. dword_82D93674
 * holds that socket, -1 when unopened. Logging both at entry says immediately
 * which branch is taken.
 */

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

namespace {

constexpr uint32_t kTransportReady = 0x82FA390D;  // byte: set by sub_8268EAD0
constexpr uint32_t kVdpSocket = 0x82D93674;       // dword: VDP socket, -1 = closed

uint32_t TransportReady(uint8_t* base) {
  return *rex::memory::GuestPtr<uint8_t*>(base, kTransportReady);
}

uint32_t VdpSocket(uint8_t* base) {
  return rex::memory::load_and_swap<uint32_t>(
      rex::memory::GuestPtr<uint8_t*>(base, kVdpSocket));
}

}  // namespace

REX_EXTERN(__imp__sub_8268EA60);
REX_EXTERN(__imp__sub_8268EAD0);
REX_EXTERN(__imp__sub_8268F118);
REX_EXTERN(__imp__sub_82691550);
REX_EXTERN(__imp__sub_826933C8);
REX_EXTERN(__imp__sub_8268FC60);

// XNetStartup(cfgFlags=1) followed by WSAStartup(2.2). Returns 0 on success.
REX_HOOK_RAW(sub_8268EA60) {
  __imp__sub_8268EA60(ctx, base);
  REXLOG_INFO("[renut/xport] netStartup -> {}", ctx.r3.u32);
}

// Opens the VDP socket, sets it non-blocking, binds port 1000, then sets
// byte_82FA390D = 1. Every send path depends on this having run.
REX_HOOK_RAW(sub_8268EAD0) {
  REXLOG_INFO("[renut/xport] openSocket  entry ready={} sock={:08X}", TransportReady(base),
              VdpSocket(base));
  __imp__sub_8268EAD0(ctx, base);
  REXLOG_INFO("[renut/xport] openSocket  exit  ready={} sock={:08X}", TransportReady(base),
              VdpSocket(base));
}

// Allocates a peer/connection slot. A negative return means the send is skipped
// entirely, with no other trace.
REX_HOOK_RAW(sub_826933C8) {
  __imp__sub_826933C8(ctx, base);
  REXLOG_INFO("[renut/xport] allocSlot   -> {}", static_cast<int32_t>(ctx.r3.u32));
}

// Looks up an existing peer object for this address. A non-negative return
// short-circuits the connect, so a stale entry would strand the join.
REX_HOOK_RAW(sub_8268FC60) {
  __imp__sub_8268FC60(ctx, base);
  REXLOG_INFO("[renut/xport] findPeer    -> {}", static_cast<int32_t>(ctx.r3.u32));
}

// The two peer-connect entry points. Logging the gate state at entry shows
// which early-out fired.
REX_HOOK_RAW(sub_82691550) {
  REXLOG_INFO("[renut/xport] connect(A)  entry ready={} sock={:08X} r3={:08X}",
              TransportReady(base), VdpSocket(base), ctx.r3.u32);
  __imp__sub_82691550(ctx, base);
  REXLOG_INFO("[renut/xport] connect(A)  -> {}", static_cast<int32_t>(ctx.r3.u32));
}

REX_HOOK_RAW(sub_8268F118) {
  REXLOG_INFO("[renut/xport] connect(B)  entry ready={} sock={:08X} r3={:08X}",
              TransportReady(base), VdpSocket(base), ctx.r3.u32);
  __imp__sub_8268F118(ctx, base);
  REXLOG_INFO("[renut/xport] connect(B)  -> {}", static_cast<int32_t>(ctx.r3.u32));
}
