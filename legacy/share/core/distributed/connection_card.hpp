// Connection card data (IPs / port / fingerprint / QR invite payload) —
// formerly legacy/cli/src/common.cpp, now shared: the desktop host page builds the
// same card in-process (stage 2) and the CLI prints the identical bytes.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace upix {

struct ConnectionCard {
    // All local IPv4 addresses (first non-loopback one is used as the QR
    // default); falls back to 127.0.0.1 when enumeration finds nothing.
    std::vector<std::string> addresses;
    uint16_t port = 0;
    std::string fingerprint;  // SHA-256 of the identity pubkey, first 16 bytes, uppercase hex
    std::string pubkey_b64;   // base64 of the 32-byte Ed25519 pubkey
    // v2 invite payload — the exact string the Android app's
    // JoinInviteParser.parseJsonInvite() understands (v1 rejected).
    std::string qr_payload;
};

// Resolve local addresses + identity into a card. Pure data — no I/O beyond
// the interface enumeration, safe to call before any output mode is chosen.
auto build_connection_card(uint16_t port,
                           const std::array<uint8_t, 32>& identity_pubkey)
    -> ConnectionCard;

}  // namespace upix
