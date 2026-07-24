/**
 * @file  peer_tick_null_guard.cpp
 * @brief Stop the per-peer tick dereferencing a destroyed peer.
 *
 * Once peers actually connect, tearing the session down crashes:
 *
 *     [CRASH] unhandled guest AV: READ guest_addr=0x00000024
 *
 * 0x24 is offset +36, the peer's IN_ADDR field. sub_826B24F8 looks the peer up
 * by the id cached at obj+40 and reads that field with no null check:
 *
 *     v11 = sub_82693520(*(_DWORD *)(a1 + 40));
 *     XNetGetConnectStatus(*(const IN_ADDR **)(v11 + 36))
 *
 * and sub_82693520 returns 0 for an id that is no longer in the peer map:
 *
 *     if ( v4 == dword_82FAFE58 ) return 0;    // iterator == end
 *
 * XSessionLeave destroys the peer object while obj+40 still holds its id, so
 * the next tick reads through null. There are three such derefs in that
 * function, all reachable only when obj+40 is not -1, so detaching the stale id
 * closes all three at once and puts the title on its own "no peer" path.
 *
 * Worth noting this is reachable partly because XNetGetConnectStatus reports
 * CONNECTED for as long as the token resolves (see xnet_peer_link.cpp). Real
 * XNet would eventually report LOST and the title would drop the peer through
 * sub_826B3020, clearing obj+40 itself. Detecting genuine peer loss needs
 * liveness the runtime does not track, so the stale handle is cleared here
 * instead of guessing at a status.
 */

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

namespace {
constexpr uint32_t kNoPeer = 0xFFFFFFFFu;
bool g_warned = false;
}  // namespace

// Peer lookup: returns 0 when the id has been removed from the map.
REX_IMPORT(sub_82693520, FindPeerObject, uint32_t(uint32_t));

REX_EXTERN(__imp__sub_826B24F8);

REX_HOOK_RAW(sub_826B24F8) {
  const uint32_t self = ctx.r3.u32;
  if (self) {
    uint8_t* slot = rex::memory::GuestPtr<uint8_t*>(base, self + 40);
    const uint32_t peer_id = rex::memory::load_and_swap<uint32_t>(slot);
    if (peer_id != kNoPeer && FindPeerObject(peer_id) == 0) {
      rex::memory::store_and_swap<uint32_t>(slot, kNoPeer);
      if (!g_warned) {
        g_warned = true;
        REXLOG_WARN("[renut/peer] detached stale peer id {} from {:08X} (object destroyed)",
                    peer_id, self);
      }
    }
  }

  __imp__sub_826B24F8(ctx, base);
}
