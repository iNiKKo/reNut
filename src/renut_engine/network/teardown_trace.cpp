/**
 * @file  teardown_trace.cpp
 * @brief Find what ends a healthy session after ~20 seconds.
 *
 * The link is symmetric and busy right up to the moment it dies -- tx=67/rx=69
 * two seconds before, rx_unmapped=0 throughout -- so nothing starves. Then:
 *
 *     XNetUnregisterInAddr(AB000002)
 *     [renut/peer] detached stale peer id 8 (object destroyed)
 *     tw/td trap hit (type 22) x2
 *     XSessionLeave x2
 *     XGISessionDelete
 *
 * No peerConnect line accompanies that unregister, so it is not the reconnect
 * path in sub_8227DB58; it comes from sub_8268F640, the teardown routine. The
 * question is who calls it while the peer is still exchanging packets.
 *
 * sub_826B24F8 (the per-peer tick) is the prime suspect: it drives several
 * timeout comparisons through sub_826AFFD0 against thresholds in flt_82D933FC /
 * flt_82D9340C / flt_82D9341C / flt_82D9342C, and on expiry writes a reason
 * code and calls sub_826B3020 to drop the peer. Logging the guest caller of
 * each teardown entry point separates "the title timed out" from "the session
 * layer asked to leave".
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/xsession.h>

namespace {

// Proof of life for the session: when the receive path last dispatched a
// message. Written by the sub_82691F28 hook, read by the sub_826DE428 guard.
std::atomic<int64_t> g_last_rx_ms{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// How recently a message must have arrived for the session to count as alive.
// The shortest timeout driving the teardown path is 4.5s and connections hand
// off about every 4.2s, so this sits well inside the window that matters while
// still lapsing quickly for a peer that has actually gone.
constexpr int64_t kSessionAliveWindowMs = 3000;

}  // namespace

REX_EXTERN(__imp__sub_8268F640);
REX_EXTERN(__imp__sub_826B3020);

// ---------------------------------------------------------------------------
// What kid is each new peer built with?
// ---------------------------------------------------------------------------
// sub_82691550 builds a peer via sub_8268F078(peerbuf, req, xnaddr, kid, 0),
// then runs a secure-channel setup callback (dword_82FAD260); when that returns
// 0 it destroys the peer it just made (sub_8268F640 at 0x82691778) and returns
// -1. Once the original peer is gone this fails on every rebuild, so the send
// path dies while packets still arrive.
//
// The leading hypothesis is that a rebuilt peer carries a kid the current
// session no longer honours -- the console migrated lobby -> game and the old
// kid's key was unregistered. This logs the kid each peer is built with next to
// the session this console is actually in, to settle that without guessing
// again.
//
//   sub_8268F078: r3=peerbuf, r4=req, r5=xnaddr, r6=kid ptr, r7=0
REX_EXTERN(__imp__sub_8268F078);

REX_HOOK_RAW(sub_8268F078) {
  const uint32_t kid_ptr = ctx.r6.u32;

  // Both kids are 8 raw bytes; read the peer's the same way the session's is
  // held so the comparison and the hex display share one byte order.
  uint8_t built[8] = {};
  if (kid_ptr) {
    std::memcpy(built, rex::memory::GuestPtr<uint8_t*>(base, kid_ptr), sizeof(built));
  }
  const rex::system::XNKID active = rex::system::GetActiveSession().session_id();

  const auto as_hex = [](const uint8_t* b) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | b[i];
    return v;
  };
  const bool match = std::memcmp(built, active.ab, 8) == 0;
  REXLOG_INFO("[renut/peerbuild] new peer kid {:016X}  active session kid {:016X}  {}",
              as_hex(built), as_hex(active.ab), match ? "MATCH" : "STALE");

  __imp__sub_8268F078(ctx, base);
}
REX_EXTERN(__imp__sub_826AFFD0);

// ---------------------------------------------------------------------------
// Share a peer's freshest last-heard stamp across all its connection objects
// ---------------------------------------------------------------------------
// Every timeout in the tick runs through this one predicate:
//
//     BOOL sub_826AFFD0(int a1, double timeout) {
//       return (float)(*(float *)(a1 + 20) + timeout) < now();
//     }
//
// a1 is a connection object (peer id at +40, last-heard stamp at +20). The bug,
// measured in run 004: a peer ends up with several connection objects and only
// one is fed. Object 466BFEA0 (peer 7) froze at 155.955 while sibling 4669B1E0
// (peer 7) advanced to 161.753 -- same peer, both live at the link level, but
// the game feeds only the current one. The frozen sibling expires and the tick
// escalates to sub_826DE428, which ends the whole session while packets are
// still arriving (last inbound 85ms before teardown).
//
// A peer is reachable as long as ANY of its connection objects is being fed, so
// every object for a peer should time out against the freshest stamp among them,
// not its own stale one. Before the predicate runs, a lagging object's +20 is
// raised to its liveliest sibling's stamp -- but only while some sibling has
// actually advanced within kSiblingFreshMs of wall time. If the peer genuinely
// leaves, nothing advances, the shared stamp goes wall-stale, refresh stops, and
// every object expires together -- so a real departure still tears down and the
// far end does not keep a ghost. Raising only (never lowering) means the live
// object, whose stamp already equals the max, is never touched.
//
// This is the correct form of what the removed sub_826DE428 "liveness guard"
// tried to do: it keys on per-peer connection liveness, not on any stray packet.
namespace {

struct PeerFreshness {
  float stamp = 0.0f;   // highest last-heard seen across this peer's connections
  int64_t wall = 0;     // wall-clock ms when that max last advanced
};

std::mutex g_freshness_mtx;
std::unordered_map<int32_t, PeerFreshness> g_peer_freshness;

// A sibling counts as live only if its stamp advanced this recently. Comfortably
// longer than the 4.5-6s game timeouts, short enough that a departed peer lapses
// well before anything downstream notices.
constexpr int64_t kSiblingFreshMs = 8000;

}  // namespace

REX_HOOK_RAW(sub_826AFFD0) {
  const uint32_t self = ctx.r3.u32;
  const double timeout = ctx.f1.f64;

  const int32_t peer_id =
      self ? static_cast<int32_t>(rex::memory::load_and_swap<uint32_t>(
                 rex::memory::GuestPtr<uint8_t*>(base, self + 40)))
           : -1;

  // Raise this object's stamp to the peer's freshest, if a sibling is live.
  if (self && peer_id >= 0 && peer_id < 0x10000) {
    uint8_t* stamp_ptr = rex::memory::GuestPtr<uint8_t*>(base, self + 20);
    const float own = rex::memory::load_and_swap<float>(stamp_ptr);
    const int64_t now_wall = NowMs();

    std::lock_guard<std::mutex> lock(g_freshness_mtx);
    PeerFreshness& f = g_peer_freshness[peer_id];
    if (own > f.stamp) {
      f.stamp = own;
      f.wall = now_wall;
    } else if (f.stamp > own && (now_wall - f.wall) < kSiblingFreshMs) {
      rex::memory::store_and_swap<float>(stamp_ptr, f.stamp);
    }
  }

  __imp__sub_826AFFD0(ctx, base);
  const bool expired = ctx.r3.u32 != 0;

  if (!self) return;
  const float last_heard =
      rex::memory::load_and_swap<float>(rex::memory::GuestPtr<uint8_t*>(base, self + 20));

  static std::mutex mtx;
  struct Sample {
    uint64_t second;
    float stamp;
  };
  static std::unordered_map<uint32_t, Sample> last;

  std::lock_guard<std::mutex> lock(mtx);
  const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
  auto& seen = last[self];
  // Always report an expiry on the edge, however quiet the object otherwise is.
  if (seen.second == now && !expired) return;
  const float delta = seen.second == 0 ? 0.0f : last_heard - seen.stamp;
  seen = {now, last_heard};
  REXLOG_INFO("[renut/tmo] conn {:08X} peer={} last_heard={:.3f} (+{:.3f}) timeout={:.3f} "
              "expired={}",
              self, peer_id, last_heard, delta, timeout, expired);
}

// ---------------------------------------------------------------------------
// Which connection actually receives?
// ---------------------------------------------------------------------------
// sub_82691F28(conn, from_peer, to_peer, msg, now, chan) dispatches each
// received message, and its first argument is the connection the receive path
// resolved for the peer id -- looked up through the map at unk_82FAFE54:
//
//     v22 = *(_DWORD *)sub_821E8350((int)&unk_82FAFE54, &v20);
//     sub_82691F28(v22, a2, v20, j, v6, v12);
//
// Pairing this against the tmo samples above shows, without inference, whether
// the connection being fed is the same one the tick is timing out.
REX_EXTERN(__imp__sub_82691F28);

REX_HOOK_RAW(sub_82691F28) {
  const uint32_t conn = ctx.r3.u32;
  const uint32_t from_peer = ctx.r4.u32;
  const uint32_t to_peer = ctx.r5.u32;

  static std::mutex mtx;
  static std::unordered_map<uint32_t, uint64_t> last_report;
  bool report = false;
  {
    const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    std::lock_guard<std::mutex> lock(mtx);
    auto& seen = last_report[conn];
    if (now != seen) {
      seen = now;
      report = true;
    }
  }
  // Proof of life for the session as a whole, consumed by the sub_826DE428
  // guard below. Recorded on every message, not just the logged ones.
  g_last_rx_ms.store(NowMs(), std::memory_order_relaxed);

  if (report) {
    REXLOG_INFO("[renut/rx] dispatch -> conn {:08X} (from_peer={} to_peer={})", conn,
                static_cast<int32_t>(from_peer), static_cast<int32_t>(to_peer));
  }

  __imp__sub_82691F28(ctx, base);
}

// ---------------------------------------------------------------------------
// Session disconnect -- logged, NOT suppressed
// ---------------------------------------------------------------------------
// sub_826DE428(*(a1 + 80), 0, *(a1 + 12)) is the call that actually ends the
// session; sub_826B3020 only retries.
//
// An earlier version SUPPRESSED this whenever a packet had arrived in the last
// 3s, on the theory that a superseded connection was timing out a live session.
// That was wrong in the way that matters: a half-dead peer's keepalives kept the
// window open, so a genuine departure was blocked -- the leaver disconnected
// while the other console kept showing them present (the reported ghost). The
// call is now only logged; the game decides teardown, which is correct now that
// the kid fix stops peers being destroyed for a stale XNKID. `last inbound` is
// kept in the line to tell a real silence apart from a spurious drop.
REX_EXTERN(__imp__sub_826DE428);

REX_HOOK_RAW(sub_826DE428) {
  const int64_t last_rx = g_last_rx_ms.load(std::memory_order_relaxed);
  const int64_t age = last_rx == 0 ? -1 : NowMs() - last_rx;

  REXLOG_WARN("[renut/teardown] sub_826DE428(session={:08X}, {}, {:08X}) -- session disconnect, "
              "last inbound {}ms ago",
              ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, age);
  __imp__sub_826DE428(ctx, base);
}

// Peer/session teardown: destroys peer objects and unregisters their addresses.
REX_HOOK_RAW(sub_8268F640) {
  REXLOG_WARN("[renut/teardown] sub_8268F640(peer={}) called from {:08X}",
              static_cast<int32_t>(ctx.r3.u32), rex::ppc::GetGuestCallerAddress());
  __imp__sub_8268F640(ctx, base);
}

// Reconnect-with-backoff, NOT a peer drop. The tick enters this every frame for
// as long as XNetGetConnectStatus reports 3; only the backoff branch does any
// work, so logging every entry buries the log and overstates what is happening.
// Report the peer id and the token the tick is querying -- that token is the one
// whose status is stuck -- and do it at most once a second per object.
REX_IMPORT(sub_82693520, FindPeerObjectForDrop, uint32_t(uint32_t));

REX_HOOK_RAW(sub_826B3020) {
  const uint32_t self = ctx.r3.u32;

  static std::mutex mtx;
  static std::unordered_map<uint32_t, uint64_t> last_report;
  bool report = false;
  {
    const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    std::lock_guard<std::mutex> lock(mtx);
    auto& seen = last_report[self];
    if (now != seen) {
      seen = now;
      report = true;
    }
  }

  if (report && self) {
    const uint32_t peer_id =
        rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr<uint8_t*>(base, self + 40));
    uint32_t token = 0;
    if (peer_id != 0xFFFFFFFFu) {
      if (const uint32_t peer = FindPeerObjectForDrop(peer_id)) {
        token = rex::memory::load_and_swap<uint32_t>(
            rex::memory::GuestPtr<uint8_t*>(base, peer + 36));
      }
    }
    REXLOG_WARN("[renut/teardown] sub_826B3020(obj={:08X}) retrying: peer={} token={:08X}", self,
                static_cast<int32_t>(peer_id), token);
  }

  __imp__sub_826B3020(ctx, base);
}
