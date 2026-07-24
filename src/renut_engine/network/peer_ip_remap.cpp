/**
 * @file  peer_ip_remap.cpp
 * @brief Redirect gameplay traffic to a VPN address (Radmin / ZeroTier).
 *
 * ReXGlue advertises a hosted session with its public IP and nothing else:
 *
 *     ws_info.host_address = wc.public_address();          // xgi_app.cpp
 *
 * and XSocket::SendTo resolves a peer token to inaOnline (that public IP),
 * falling back to the LAN address only when inaOnline is zero or when the peer
 * shares our public IP. Two players behind different NATs therefore always
 * address each other over the internet, where neither router forwards UDP 1000
 * -- confirmed in the paired logs: the joiner resolved the host to token
 * AB000002, sent, and timed out five seconds later having never been heard.
 *
 * A LAN-emulating VPN gives both machines a routable address, but nothing in
 * the discovery path can advertise it. This hook rewrites the destination just
 * before the send leaves the title, so ReXGlue passes the VPN address through
 * verbatim (its token translation only triggers on the 0xAB prefix).
 *
 * 0x829A87B0 is WSASendTo -- IDA names it WSARecvFrom_0, but it shuffles args
 * and tail-calls NetDll_WSASendTo. It is the gameplay transport's only send
 * path (callers 0x82690908, 0x82691828). Args: r3=s, r4=lpBuffers,
 * r5=dwBufferCount, r6=lpNumberOfBytesSent, r7=dwFlags, r8=lpTo, r9=iTolen.
 *
 * Configure with pairs of public=vpn addresses, identical on both machines:
 *
 *     renut_peer_ip_map = "216.79.141.3=26.1.2.3,154.7.124.156=26.4.5.6"
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/xsession.h>

#include "peer_tokens.h"

REXCVAR_DEFINE_STRING(renut_peer_ip_map, "", "Nuts&Bolts/Netplay",
                      "Comma-separated public=vpn IPv4 pairs used to route peer traffic "
                      "over a LAN-emulating VPN (Radmin, ZeroTier). Example: "
                      "216.79.141.3=26.1.2.3,154.7.124.156=26.4.5.6");

namespace {

std::once_flag g_parse_once;
// Keyed and valued in host order, matching what load_and_swap yields from the
// guest sockaddr. See ResolveTarget for why XNADDR fields differ.
std::unordered_map<uint32_t, uint32_t> g_map;

// Inverse of g_map, built at parse time. It must come from configuration rather
// than from observed sends: whichever console receives first has sent nothing
// yet, so a send-populated table is empty exactly when the unsolicited inbound
// packet arrives. That asymmetry made joins work in one direction only -- the
// responder dropped the initiator's packets, never replied, and the initiator
// timed out after five seconds.
std::unordered_map<uint32_t, uint32_t> g_vpn_to_public;

uint32_t ParseIpv4(const std::string& s) {
  unsigned a, b, c, d;
  if (std::sscanf(s.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
  if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
  // Host order: first octet in the most significant position.
  return (a << 24) | (b << 16) | (c << 8) | d;
}

std::string FormatIpv4(uint32_t net) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (net >> 24) & 0xFF, (net >> 16) & 0xFF,
                (net >> 8) & 0xFF, net & 0xFF);
  return buf;
}

void ParseMap() {
  const std::string& cfg = REXCVAR_GET(renut_peer_ip_map);
  size_t pos = 0;
  while (pos < cfg.size()) {
    size_t comma = cfg.find(',', pos);
    if (comma == std::string::npos) comma = cfg.size();
    std::string pair = cfg.substr(pos, comma - pos);
    pos = comma + 1;

    size_t eq = pair.find('=');
    if (eq == std::string::npos) continue;
    const uint32_t from = ParseIpv4(pair.substr(0, eq));
    const uint32_t to = ParseIpv4(pair.substr(eq + 1));
    if (!from || !to) {
      REXLOG_WARN("[renut/remap] ignoring malformed entry '{}'", pair);
      continue;
    }
    g_map[from] = to;
    g_vpn_to_public[to] = from;
    REXLOG_INFO("[renut/remap] {} -> {}", FormatIpv4(from), FormatIpv4(to));
  }
  if (g_map.empty() && !cfg.empty()) {
    REXLOG_WARN("[renut/remap] renut_peer_ip_map set but no usable entries");
  }
}

// Sends run at packet rate, so each distinct destination is reported once.
std::unordered_set<uint32_t> g_redirected;
std::unordered_set<uint32_t> g_unmapped;
std::unordered_set<uint32_t> g_restored;

// Reverse of a redirect: VPN address -> the token the title addressed. Recorded
// on send so the receive path can undo the substitution. Sends and receives can
// land on different threads, so this is guarded.
std::mutex g_reverse_mtx;
std::unordered_map<uint32_t, uint32_t> g_vpn_to_token;

void LogOnce(std::unordered_set<uint32_t>& seen, uint32_t key, const std::string& msg) {
  if (seen.insert(key).second) {
    REXLOG_INFO("{}", msg);
  }
}

// The once-per-peer logging above cannot show whether a link goes quiet before
// a disconnect, which is exactly what distinguishes "peer stopped talking" from
// "we stopped talking". Counters are reported on a timer instead.
std::atomic<uint64_t> g_tx{0};
std::atomic<uint64_t> g_rx{0};
std::atomic<uint64_t> g_rx_unmapped{0};

void ReportTraffic() {
  using clock = std::chrono::steady_clock;
  static std::mutex mtx;
  static clock::time_point last{};
  static uint64_t last_tx = 0, last_rx = 0;

  std::lock_guard<std::mutex> lock(mtx);
  const auto now = clock::now();
  if (last.time_since_epoch().count() != 0 && now - last < std::chrono::seconds(2)) return;
  last = now;

  const uint64_t tx = g_tx.load(), rx = g_rx.load();
  if (tx == last_tx && rx == last_rx) return;  // idle, nothing to say
  REXLOG_INFO("[renut/traffic] tx={} (+{}) rx={} (+{}) rx_unmapped={}", tx, tx - last_tx, rx,
              rx - last_rx, g_rx_unmapped.load());
  last_tx = tx;
  last_rx = rx;
}

constexpr uint32_t Bswap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}

// Resolves the address the title is really trying to reach. ReXGlue hands the
// title synthetic 0xAB....... tokens from XNetXnAddrToInAddr rather than real
// addresses, so a token has to be walked back through the cache to find the
// public IP the map is keyed on.
//
// Everything here works in host order. The guest sockaddr is read with
// load_and_swap, which already yields host order, and the token counter is a
// plain host-order value -- but XNADDR keeps ina/inaOnline in network order
// (XSocket::SendTo applies ntohl to these same fields for exactly this reason),
// so those need swapping or the key never matches and every peer logs as its
// own octets reversed.
uint32_t ResolveTarget(uint32_t dest_host) {
  if ((dest_host & 0xFF000000u) != 0xAB000000u) return dest_host;
  rex::system::XNetAddrEntry entry;
  if (!rex::system::XNetAddrCache::Get().Lookup(dest_host, entry)) return dest_host;
  const uint32_t net = entry.xn_addr.inaOnline ? entry.xn_addr.inaOnline : entry.xn_addr.ina;
  return Bswap32(net);
}

// Finds the synthetic token XNetXnAddrToInAddr issued for a peer, given the
// public address the session advertised. XNetAddrCache exposes no iteration and
// FindByPublicIp returns the entry rather than its key, so the token space is
// probed instead -- it is a counter from 0xAB000001, so this is a short scan,
// and the answer is cached per peer.
uint32_t TokenForPublicIp(uint32_t public_host_order) {
  // The token the XNetXnAddrToInAddr hook actually handed the title, if it has
  // resolved this console. Consulted first and never memoised: the cache scan
  // below can still see the surplus entries ReXGlue allocated for the same
  // console before they were collapsed, and memoising one of those would pin
  // the receive path to a token the title does not address.
  if (const uint32_t canonical = renut::net::CanonicalTokenForPublicIp(public_host_order)) {
    return canonical;
  }

  static std::mutex mtx;
  static std::unordered_map<uint32_t, uint32_t> cache;
  {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = cache.find(public_host_order);
    if (it != cache.end()) return it->second;
  }

  constexpr uint32_t kFirstToken = 0xAB000001u;
  constexpr uint32_t kMaxTokens = 256;
  for (uint32_t token = kFirstToken; token < kFirstToken + kMaxTokens; ++token) {
    rex::system::XNetAddrEntry entry;
    if (!rex::system::XNetAddrCache::Get().Lookup(token, entry)) continue;
    const uint32_t net = entry.xn_addr.inaOnline ? entry.xn_addr.inaOnline : entry.xn_addr.ina;
    if (Bswap32(net) == public_host_order) {
      std::lock_guard<std::mutex> lock(mtx);
      cache[public_host_order] = token;
      return token;
    }
  }
  return 0;
}

// First contact from a peer we have never resolved. The title identifies an
// inbound peer by calling XNetInAddrToXnAddr on the source address, which only
// succeeds for an address XNetAddrCache already knows -- and the cache is
// populated solely by XNetXnAddrToInAddr during a session search. A console
// sitting in its own lobby has therefore never issued a token for the peer
// contacting it, so every unsolicited packet is unidentifiable and dropped.
// That is why joins only ever worked in the direction where the receiver had
// already searched for the sender's session.
//
// Real XNet learns a peer from the inbound secure packet itself. Do the same:
// register an XNADDR on demand so a token exists to hand the title. Store()
// dedupes on address, so repeats return the same token.
uint32_t EnsureTokenForPublicIp(uint32_t public_host_order) {
  if (const uint32_t existing = TokenForPublicIp(public_host_order)) return existing;

  rex::system::XNADDR addr{};
  addr.ina = Bswap32(public_host_order);  // XNADDR keeps these in network order
  addr.inaOnline = addr.ina;
  // Leave wPortOnline zero: XSocket::SendTo only applies its advertised-port
  // shift when this is non-zero, and the guest's own port is already correct.
  addr.wPortOnline = 0;
  // A stable synthetic MAC per peer, so distinct peers stay distinguishable.
  addr.abEnet[0] = 0x02;
  addr.abEnet[1] = 0x00;
  addr.abEnet[2] = static_cast<uint8_t>(public_host_order >> 24);
  addr.abEnet[3] = static_cast<uint8_t>(public_host_order >> 16);
  addr.abEnet[4] = static_cast<uint8_t>(public_host_order >> 8);
  addr.abEnet[5] = static_cast<uint8_t>(public_host_order);

  // Key it to the session this console is actually in; the title matches the
  // peer's XNKID against its own session when it builds the peer record.
  const rex::system::XNKID kid = rex::system::GetActiveSession().session_id();

  const uint32_t token = rex::system::XNetAddrCache::Get().Store(addr, kid);
  REXLOG_INFO("[renut/remap] first contact from {} -> allocated token {:08X}",
              FormatIpv4(public_host_order), token);
  return token;
}

}  // namespace

REX_EXTERN(__imp__sub_829A87B0);

// WSASendTo. lpTo (r8) points at a guest sockaddr_in; sin_addr sits at +4 and
// is stored big-endian, which is also network byte order.
REX_HOOK_RAW(sub_829A87B0) {
  std::call_once(g_parse_once, ParseMap);

  uint32_t original = 0;
  uint8_t* sin_addr = nullptr;

  const uint32_t to_ptr = ctx.r8.u32;
  if (!g_map.empty() && to_ptr) {
    sin_addr = rex::memory::GuestPtr<uint8_t*>(base, to_ptr + 4);
    original = rex::memory::load_and_swap<uint32_t>(sin_addr);

    const uint32_t target = ResolveTarget(original);
    auto it = g_map.find(target);
    if (it != g_map.end()) {
      rex::memory::store_and_swap<uint32_t>(sin_addr, it->second);
      // Only a synthetic token needs restoring on the way back in; a literal
      // address the title already understands can be left alone.
      if ((original & 0xFF000000u) == 0xAB000000u) {
        std::lock_guard<std::mutex> lock(g_reverse_mtx);
        g_vpn_to_token[it->second] = original;
      }
      LogOnce(g_redirected, target,
              fmt::format("[renut/remap] redirecting {} -> {}", FormatIpv4(target),
                          FormatIpv4(it->second)));
    } else {
      // The common misconfiguration is holding only your own pair, which never
      // matches locally. Name the address that went unmapped so it is obvious
      // which entry is missing.
      LogOnce(g_unmapped, target,
              fmt::format("[renut/remap] NO MAPPING for {} (sending direct; add "
                          "\"{}=<peer vpn ip>\" to renut_peer_ip_map)",
                          FormatIpv4(target), FormatIpv4(target)));
      sin_addr = nullptr;  // nothing rewritten, nothing to restore
    }
  }

  g_tx.fetch_add(1, std::memory_order_relaxed);
  ReportTraffic();
  __imp__sub_829A87B0(ctx, base);

  // The sockaddr belongs to the title and is frequently reused across sends;
  // put it back so the guest never observes the substitution.
  if (sin_addr) {
    rex::memory::store_and_swap<uint32_t>(sin_addr, original);
  }
}

// ---------------------------------------------------------------------------
// Receive side
// ---------------------------------------------------------------------------
// XSocket::RecvFrom hands the title the raw source address:
//
//     from->sin_addr = ntohl(nfrom.sin_addr.s_addr);
//
// with no reverse mapping, so a redirected peer arrives as 26.x.x.x while the
// title only knows it as the token XNetXnAddrToInAddr issued. The transport
// cannot match that to a peer and drops it, which looks identical to the peer
// never having replied. Undo the substitution recorded on send.
//
// 0x821FB950 is WSARecvFrom (callers 0x821A38D0, 0x82692710, 0x82BAC100 -- the
// middle one is the gameplay transport). lpFrom is in r8.

// ReXGlue does not implement this at all -- NetDll_WSARecvFrom_entry is a
// hardcoded `return -1` ("we're not going to be receiving packets any time
// soon"). It never touches the socket, so the gameplay transport, which
// receives through this and nothing else, could never hear a peer. Both
// consoles sent correctly and both timed out at five seconds.
//
// NetDll_recvfrom IS implemented (XSocket::RecvFrom), and the title only ever
// asks for a single 1024-byte buffer:
//
//     WSARecvFrom(a1, &v84, 1u, &v73, v74, &v85, &v72, 0, ...)   // 0x82692710
//
// so scatter/gather is not needed -- forward buffer 0 to recvfrom and translate
// the result back to WSARecvFrom's convention (0 = success, count via
// lpNumberOfBytesRecvd, which the caller checks immediately afterwards).
REX_EXTERN(__imp__NetDll_recvfrom);

REX_HOOK_RAW(sub_821FB950) {
  // The map must be parsed here too, not just on send. A console contacted
  // before it has sent anything would otherwise consult an empty table and drop
  // every unsolicited packet -- visible as rx_unmapped climbing while tx is
  // still 0, with the first successful restore arriving only after the first
  // outbound packet happened to trigger the parse.
  std::call_once(g_parse_once, ParseMap);

  const uint32_t socket_handle = ctx.r3.u32;
  const uint32_t buffers_ptr = ctx.r4.u32;
  const uint32_t buffer_count = ctx.r5.u32;
  const uint32_t bytes_recv_ptr = ctx.r6.u32;
  const uint32_t from_ptr = ctx.r8.u32;
  const uint32_t fromlen_ptr = ctx.r9.u32;

  if (!buffers_ptr || !buffer_count) {
    ctx.r3.u64 = static_cast<uint32_t>(-1);
    return;
  }

  const uint8_t* wsabuf = rex::memory::GuestPtr<uint8_t*>(base, buffers_ptr);
  const uint32_t buf_len = rex::memory::load_and_swap<uint32_t>(wsabuf);
  const uint32_t buf_ptr = rex::memory::load_and_swap<uint32_t>(wsabuf + 4);

  ctx.r3.u64 = 1;  // XNCALLER_TITLE
  ctx.r4.u32 = socket_handle;
  ctx.r5.u32 = buf_ptr;
  ctx.r6.u32 = buf_len;
  ctx.r7.u64 = 0;  // flags
  ctx.r8.u32 = from_ptr;
  ctx.r9.u32 = fromlen_ptr;
  __imp__NetDll_recvfrom(ctx, base);

  const int32_t ret = static_cast<int32_t>(ctx.r3.u32);
  if (ret < 0) {
    ctx.r3.u64 = static_cast<uint32_t>(-1);  // SOCKET_ERROR
    return;
  }
  if (bytes_recv_ptr) {
    rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr<uint8_t*>(base, bytes_recv_ptr),
                                          static_cast<uint32_t>(ret));
  }
  ctx.r3.u64 = 0;  // success
  g_rx.fetch_add(1, std::memory_order_relaxed);
  ReportTraffic();

  if (!from_ptr) return;

  uint8_t* sin_addr = rex::memory::GuestPtr<uint8_t*>(base, from_ptr + 4);
  const uint32_t src = rex::memory::load_and_swap<uint32_t>(sin_addr);

  uint32_t token = 0;
  {
    std::lock_guard<std::mutex> lock(g_reverse_mtx);
    auto it = g_vpn_to_token.find(src);
    if (it != g_vpn_to_token.end()) token = it->second;
  }
  if (!token) {
    // Nothing sent to this peer yet -- the unsolicited-inbound case. Recover the
    // token from the configured mapping instead of waiting for an outbound
    // packet that the title will not send until this one is accepted.
    auto it = g_vpn_to_public.find(src);
    if (it != g_vpn_to_public.end()) {
      token = EnsureTokenForPublicIp(it->second);
      if (token) {
        std::lock_guard<std::mutex> lock(g_reverse_mtx);
        g_vpn_to_token[src] = token;
      }
    }
  }
  if (!token) {
    g_rx_unmapped.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  rex::memory::store_and_swap<uint32_t>(sin_addr, token);

  // A packet from this console outranks any stale XNetUnregisterInAddr. Without
  // this the token stays LOST forever after a single peer teardown, since one
  // token now serves every session, and the title stops sending while still
  // receiving -- churning a peer every ~200ms and looking "connected" to the
  // far end long after it has gone quiet.
  renut::net::NoteTokenAlive(token);

  LogOnce(g_restored, src,
          fmt::format("[renut/remap] inbound {} -> restored token {:08X}", FormatIpv4(src), token));
}
