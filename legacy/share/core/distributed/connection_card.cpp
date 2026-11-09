#include "core/distributed/connection_card.hpp"

#include <format>

#include "core/distributed/identity.hpp"
#include "utils/network.hpp"

namespace upix {

auto build_connection_card(uint16_t port,
                           const std::array<uint8_t, 32>& identity_pubkey)
    -> ConnectionCard {
    ConnectionCard card;
    card.addresses = network_utils::get_local_addresses();
    if (card.addresses.empty()) {
        card.addresses.push_back("127.0.0.1");
    }
    card.port = port;
    card.fingerprint = fingerprint(identity_pubkey);
    card.pubkey_b64 = base64_encode(identity_pubkey.data(), 32);

    // v2 JSON: carries fingerprint + identity pubkey. Android rejects v1.
    // ("name" removed: Android's JoinInviteParser never reads it — 13 bytes
    // of payload cost QR modules for nothing.)
    card.qr_payload = std::format(
        "{{\"v\":2,\"ip\":\"{}\",\"port\":{},\"fp\":\"{}\""
        ",\"pk\":\"{}\"}}",
        card.addresses[0], port, card.fingerprint, card.pubkey_b64);
    return card;
}

}  // namespace upix
