/**
 * @file  xnet_peer_link.cpp
 * @brief Peer-link overrides for Banjo-Kazooie: Nuts & Bolts multiplayer.
 *
 * The title reaches XNet through xam.xex imports. ReXGlue implements every
 * NetDll_* import the title uses except a handful that are bare REX_EXPORT_STUB
 * -- and REX_STUB never touches ctx.r3. The recompiled thunks load r3 with the
 * XNCALLER_TITLE value (1) immediately before branching to the import:
 *
 *     sub_829A80C8:  mr r4,r3 ; li r3,1 ; b __imp__NetDll_XNetGetConnectStatus
 *
 * so XNetGetConnectStatus() deterministically returns 1 == PENDING forever, and
 * XNetConnect() returns 1, an error. The per-peer tick at 0x826B24F8 keys the
 * entire connection state machine off that status, so peers never reach
 * CONNECTED and every join sits in the timeout path until it expires.
 *
 * These hooks override the thunks themselves, so the call never reaches the
 * stubbed import. ReXGlue is read-only; nothing here duplicates its routing.
 * The synthetic IN_ADDR tokens come from XNetAddrCache, which
 * NetDll_XNetXnAddrToInAddr populates and XSocket::SendTo resolves back to a
 * real peer address -- that path already works, only the status did not.
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/xsession.h>

namespace {

// XNET_CONNECT_STATUS, as consumed by the peer tick at 0x826B24F8:
//   != 3 -> link alive;  == 2 -> clear the pending flag;  == 3 -> drop peer.
constexpr uint32_t kXnetConnectStatusPending = 1;
constexpr uint32_t kXnetConnectStatusConnected = 2;
constexpr uint32_t kXnetConnectStatusLost = 3;

// XNetAddrCache hands out tokens from 0xAB000001 up.
bool IsSyntheticToken(uint32_t ina) {
  return (ina & 0xFF000000u) == 0xAB000000u;
}

std::mutex g_mutex;

// Tokens the title explicitly tore down. XNetAddrCache::Store dedupes by
// address, so re-resolving a peer yields the same token; XNetConnect clears the
// entry to let a peer reconnect on the address it used before.
std::unordered_set<uint32_t> g_unregistered;

// Why a token reports LOST, so a stuck link names its own cause. The tick calls
// this every frame, so only transitions are logged.
const char* g_status_reason = "";

uint32_t PeerStatus(uint32_t ina) {
  if (!ina) {
    g_status_reason = "null in_addr";
    return kXnetConnectStatusLost;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_unregistered.count(ina)) {
      g_status_reason = "explicitly unregistered";
      return kXnetConnectStatusLost;
    }
  }
  if (IsSyntheticToken(ina)) {
    rex::system::XNetAddrEntry entry;
    if (rex::system::XNetAddrCache::Get().Lookup(ina, entry)) {
      g_status_reason = "in addr cache";
      return kXnetConnectStatusConnected;
    }
    g_status_reason = "not in addr cache";
    return kXnetConnectStatusLost;
  }
  // A literal IPv4 address (pure LAN, no web bridge): directly routable, so
  // there is no secure-link handshake to wait on.
  g_status_reason = "literal address";
  return kXnetConnectStatusConnected;
}

// XNetConnect(IN_ADDR ina) -- the IN_ADDR is passed by value, so r3 holds the
// token itself. Returns 0 on success.
//
// The real XNetConnect is asynchronous: it starts a security association and
// the status walks PENDING -> CONNECTED. ReXGlue's data plane is plain UDP with
// no association to build, and traffic to this peer is already routable by the
// time the title calls us, so the link is reported live immediately. NAT/filter
// punching is handled by ReXGlue's discovery path, not here.
uint32_t XNetConnect_hook(uint32_t ina) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_unregistered.erase(ina);
  }
  REXLOG_INFO("[renut/xnet] XNetConnect({:08X}) -> 0 (link up)", ina);
  return 0;
}

uint32_t XNetGetConnectStatus_hook(uint32_t ina) {
  const uint32_t status = PeerStatus(ina);

  // The per-peer tick drops straight into the reconnect path whenever this
  // reports 3, and does so silently -- a token that goes LOST and stays LOST is
  // indistinguishable in the log from a timeout. Report the edge.
  {
    static std::mutex mtx;
    static std::unordered_map<uint32_t, uint32_t> last;
    std::lock_guard<std::mutex> lock(mtx);
    auto [it, inserted] = last.emplace(ina, status);
    if (inserted || it->second != status) {
      const uint32_t previous = inserted ? 0 : it->second;
      it->second = status;
      REXLOG_INFO("[renut/xnet] status {:08X}: {} -> {} ({})", ina, previous, status,
                  g_status_reason);
    }
  }

  REXLOG_NOISY_DEBUG("[renut/xnet] XNetGetConnectStatus({:08X}) -> {}", ina, status);
  return status;
}

// XNetUnregisterInAddr(IN_ADDR ina). Returns 0 on success.
uint32_t XNetUnregisterInAddr_hook(uint32_t ina) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_unregistered.insert(ina);
  }
  REXLOG_INFO("[renut/xnet] XNetUnregisterInAddr({:08X})", ina);
  return 0;
}

std::string FormatIpv4(uint32_t host_order) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (host_order >> 24) & 0xFF,
                (host_order >> 16) & 0xFF, (host_order >> 8) & 0xFF, host_order & 0xFF);
  return buf;
}

bool IsZero(const std::array<uint8_t, 20>& id) {
  for (uint8_t b : id) {
    if (b) return false;
  }
  return true;
}

// Identity of a peer console, independent of the session it registered under.
// inaOnline and wPortOnline are what survive a session migration; ina does not,
// which is exactly why ReXGlue's dedupe misses and issues a second token.
uint64_t PeerKey(uint32_t ina_online, uint32_t ina, uint16_t port) {
  return (static_cast<uint64_t>(ina_online ? ina_online : ina) << 16) | port;
}

std::mutex g_token_mutex;

// Peer identity -> the single token the title is ever handed for that console.
std::unordered_map<uint64_t, uint32_t> g_canonical_token;

// abOnline as first seen per peer. Two consoles behind one public IP would
// share an identity key, so a disagreement here vetoes the collapse.
std::unordered_map<uint64_t, std::array<uint8_t, 20>> g_peer_online_id;

}  // namespace

namespace renut::net {

void NoteTokenAlive(uint32_t token) {
  if (!token) return;
  bool cleared = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    cleared = g_unregistered.erase(token) != 0;
  }
  if (cleared) {
    REXLOG_INFO("[renut/xnet] {:08X} un-unregistered: inbound traffic proves the console is alive",
                token);
  }
}

uint32_t CanonicalTokenForPublicIp(uint32_t public_host_order) {
  std::lock_guard<std::mutex> lock(g_token_mutex);
  // Keys pack address and port together; the caller only knows the address, and
  // a console is reachable at exactly one port here.
  for (const auto& [key, token] : g_canonical_token) {
    if (static_cast<uint32_t>(key >> 16) == public_host_order) return token;
  }
  return 0;
}

}  // namespace renut::net

// ---------------------------------------------------------------------------
// One console, one token
// ---------------------------------------------------------------------------
// A session with one remote player produced two tokens (AB000002, AB000004) and
// three peer objects. The token diagnostic showed what they actually are:
//
//     token AB000002 = ina 97.69.49.88 / online 97.69.49.88 port 1001 kid 37DF..
//     token AB000004 = ina 0.0.0.0     / online 97.69.49.88 port 1001 kid 3594..
//
// One console, twice -- and the peer's own log shows a CreateSession for the
// second XNKID at that moment. The console migrates from the lobby session into
// a game session and re-registers with an online-only XNADDR, ina zeroed.
// XNetAddrCache::Store dedupes on all three address fields:
//
//     entry.inaOnline == addr.inaOnline && entry.ina == addr.ina &&
//     entry.wPortOnline == addr.wPortOnline
//
// so the zeroed ina misses the dedupe and a second token is allocated for a
// console that already had one. Real XNet issues one IN_ADDR per peer whatever
// session it is in; ReXGlue is read-only, so the contract is restored here.
//
// It matters because a token is the transport address. Two tokens for one
// console means two peer objects, while the receive path can only attribute an
// inbound packet to one of them -- the peer_ip_remap reverse map is keyed by
// address, so it holds a single token per peer. The unattributed peer's
// last-heard timestamp at obj+20 goes stale and sub_826AFFD0 expires it:
//
//     return (float)(*(float *)(a1 + 20) + timeout) < now();
//
// Thunk: r3=pxna, r4=pxnkid, r5=pina; the token is written to *pina.
REX_EXTERN(__imp__sub_829A8048);

REX_HOOK_RAW(sub_829A8048) {
  const uint32_t pxna = ctx.r3.u32;
  const uint32_t pxnkid = ctx.r4.u32;
  const uint32_t pina = ctx.r5.u32;

  // Delegate rather than answer directly: the entry applies a loopback fixup for
  // two instances sharing one public IP, and its output marshalling is correct.
  // Any surplus token it allocates is simply left unreferenced.
  __imp__sub_829A8048(ctx, base);

  if (ctx.r3.u32 != 0 || !pina || !pxna) return;

  const uint8_t* a = rex::memory::GuestPtr<uint8_t*>(base, pxna);
  const uint32_t ina = rex::memory::load_and_swap<uint32_t>(a);
  const uint32_t ina_online = rex::memory::load_and_swap<uint32_t>(a + 4);
  const uint16_t port = rex::memory::load_and_swap<uint16_t>(a + 8);

  std::array<uint8_t, 20> online_id{};
  std::memcpy(online_id.data(), a + 16, online_id.size());

  uint8_t* pina_ptr = rex::memory::GuestPtr<uint8_t*>(base, pina);
  const uint32_t issued = rex::memory::load_and_swap<uint32_t>(pina_ptr);

  uint64_t kid = 0;
  if (pxnkid) {
    kid = rex::memory::load_and_swap<uint64_t>(rex::memory::GuestPtr<uint8_t*>(base, pxnkid));
  }

  const uint64_t key = PeerKey(ina_online, ina, port);
  uint32_t canonical = issued;

  {
    std::lock_guard<std::mutex> lock(g_token_mutex);
    auto it = g_canonical_token.find(key);
    if (it == g_canonical_token.end()) {
      g_canonical_token.emplace(key, issued);
      g_peer_online_id.emplace(key, online_id);
      REXLOG_INFO(
          "[renut/xnet] token {:08X} = ina {} / online {} port {} kid {:016X}", issued,
          FormatIpv4(ina), FormatIpv4(ina_online), port, kid);
    } else if (const auto& seen = g_peer_online_id[key];
               !IsZero(seen) && !IsZero(online_id) && seen != online_id) {
      // Two genuinely different consoles reachable at one address and port --
      // the same-machine case. Their online identities disagree, so leave
      // ReXGlue's separate tokens in place; collapsing these would merge two
      // players into one peer.
      REXLOG_WARN("[renut/xnet] {}:{} has a second online identity; keeping token {:08X} distinct",
                  FormatIpv4(ina_online ? ina_online : ina), port, issued);
    } else {
      canonical = it->second;
    }
  }
  // The peer's XNKID is supplied on the way back out by the XNetInAddrToXnAddr
  // hook below, from this console's active session rather than from whatever the
  // peer first registered under -- see the note there.

  if (canonical != issued) {
    rex::memory::store_and_swap<uint32_t>(pina_ptr, canonical);
    REXLOG_INFO("[renut/xnet] {}:{} re-registered under kid {:016X}: token {:08X} -> {:08X}",
                FormatIpv4(ina_online ? ina_online : ina), port, kid, issued, canonical);
  }
}

// ---------------------------------------------------------------------------
// XNetInAddrToXnAddr - calling convention repair
// ---------------------------------------------------------------------------
// The XDK takes the IN_ADDR by value:
//
//     INT XNetInAddrToXnAddr(const IN_ADDR ina, XNADDR *pxna, XNKID *pxnkid);
//
// and the thunk marshals it that way -- r4 ends up holding the token itself:
//
//     829a8078  mr r6,r5 ; mr r5,r4 ; mr r4,r3 ; li r3,1 ; b NetDll_...
//
// ReXGlue's entry declares that argument as mapped_void and dereferences it,
// so it reads four bytes from guest address 0xAB000002 instead of using the
// token, the XNetAddrCache lookup misses, and it returns 1. sub_8268F118 and
// sub_82691550 both bail on the first branch when that happens:
//
//     if ( XNetInAddrToXnAddr(*a1, &v8, &v7) ) return -1;
//
// which is the five seconds of silence seen on both peers -- no findPeer, no
// socket, no send. XNetXnAddrToInAddr is unaffected: every one of its
// parameters really is a pointer.
//
// Rather than re-marshal the 36-byte XNADDR by hand, this stages the token in
// guest scratch and delegates, so ReXGlue's own (correct) output path runs.
REX_EXTERN(__imp__NetDll_XNetInAddrToXnAddr);

REX_HOOK_RAW(sub_829A8078) {
  const uint32_t ina = ctx.r3.u32;
  const uint32_t pxna = ctx.r4.u32;
  const uint32_t pxnkid = ctx.r5.u32;

  const uint32_t saved_r1 = ctx.r1.u32;
  ctx.r1.u32 -= 0x10;
  const uint32_t scratch = ctx.r1.u32 + 0x8;
  rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr<uint8_t*>(base, scratch), ina);

  ctx.r3.u64 = 1;  // XNCALLER_TITLE
  ctx.r4.u32 = scratch;
  ctx.r5.u32 = pxna;
  ctx.r6.u32 = pxnkid;
  __imp__NetDll_XNetInAddrToXnAddr(ctx, base);

  ctx.r1.u32 = saved_r1;

  // Answer with THIS console's current session id as the peer's XNKID.
  //
  // sub_82691550 feeds this kid to sub_8268F078 when it rebuilds a peer, and a
  // secure-channel callback then destroys the peer unless the kid matches the
  // active session (measured: peerbuild MATCH works, STALE is torn down). The
  // kid the cache stored is whatever the peer first registered under, which the
  // title never re-resolves after a lobby->game migration -- so it stays stale
  // and every rebuild is rejected, killing the send path while packets arrive.
  //
  // The active session is the source of truth: both consoles share one session
  // id, so ours is the one the callback validates against. findPeer still dedups
  // on the token (its second loop ignores the kid), so overwriting the kid
  // cannot strand an existing peer.
  if (ctx.r3.u32 == 0 && pxnkid) {
    const rex::system::XNKID active = rex::system::GetActiveSession().session_id();
    if (!active.IsZero()) {
      std::memcpy(rex::memory::GuestPtr<uint8_t*>(base, pxnkid), active.ab, sizeof(active.ab));
    }
  }

  REXLOG_INFO("[renut/xnet] XNetInAddrToXnAddr({:08X}) -> {}", ina, ctx.r3.u32);
}

// 0x829A80B8 XNetConnect            (callers: 0x8227DB58)
// 0x829A80C8 XNetGetConnectStatus   (callers: 0x822009B0 0x82279018 0x8227DB58
//                                             0x8268F640 0x826B24F8 0x826B3020)
// 0x829A80A8 XNetUnregisterInAddr   (callers: 0x8227DB58 0x8268F640 0x82BAC378)
REX_HOOK(sub_829A80B8, XNetConnect_hook)
REX_HOOK(sub_829A80C8, XNetGetConnectStatus_hook)
REX_HOOK(sub_829A80A8, XNetUnregisterInAddr_hook)
