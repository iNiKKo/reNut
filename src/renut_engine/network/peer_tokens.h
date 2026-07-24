/**
 * @file  peer_tokens.h
 * @brief Canonical peer-token lookup shared by the netplay hooks.
 */

#pragma once

#include <cstdint>

namespace renut::net {

/// The one token the title is handed for the console reachable at
/// @p public_host_order (host byte order, first octet most significant), or 0
/// if that console has not been resolved yet.
///
/// Prefer this over scanning XNetAddrCache. ReXGlue allocates a fresh token
/// whenever a console re-registers with a different `ina` -- which happens on
/// every session migration -- and xnet_peer_link.cpp collapses those onto one
/// token without being able to remove the surplus cache entries. A scan of the
/// cache therefore still sees them, and iterates unordered, so it can return a
/// token the title never addresses.
uint32_t CanonicalTokenForPublicIp(uint32_t public_host_order);

/// Record that a packet just arrived from @p token, clearing any LOST state
/// XNetUnregisterInAddr left on it.
///
/// Necessary because one token now serves every session on a console: a single
/// unregister marks the whole console LOST, and nothing clears it, since
/// XNetConnect only runs on a retry path that needs a live peer. Measured
/// consequence -- the title stops sending while still receiving, tears down and
/// reallocates a peer every ~200ms, and the far end sees a player who is still
/// present until its own timeout expires.
///
/// A console we are actively receiving from is alive by definition, and that
/// outranks a stale unregister.
void NoteTokenAlive(uint32_t token);

}  // namespace renut::net
