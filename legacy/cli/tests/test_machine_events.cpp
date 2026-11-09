// Machine-output helpers conformance. These are the primitives the desktop
// GUI parses: json_escape feeds every event/document we emit, and the v2
// connection-card payload is the string the Android JoinInviteParser and the
// desktop image://qr provider both consume. A regression here corrupts every
// consumer at once, so the shapes are pinned exactly.

#include "common.hpp"
#include "core/distributed/identity.hpp"  // upix::fingerprint

#include <iostream>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "CHECK failed at " << __LINE__ << ": " #cond "\n";  \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

void test_json_escape() {
    CHECK(json_escape("") == "");
    CHECK(json_escape("plain") == "plain");
    // UTF-8 bytes >= 0x20 pass through untouched (scene tags stay readable).
    CHECK(json_escape("动漫照片") == "动漫照片");
    CHECK(json_escape("a\"b") == "a\\\"b");
    CHECK(json_escape("a\\b") == "a\\\\b");
    CHECK(json_escape("a\nb") == "a\\nb");
    CHECK(json_escape("a\tb") == "a\\tb");
    CHECK(json_escape("a\rb") == "a\\rb");
    CHECK(json_escape(std::string("a\x01" "b")) == "a\\u0001b");
    // Unix paths (the bulk of what we escape) survive verbatim.
    const std::string path = "/home/u/.local/share/upix/models/Real-CUGAN-x2.bin";
    CHECK(json_escape(path) == path);
}

void test_connection_card_payload() {
    // Arbitrary 32-byte identity; only the shape matters here.
    std::array<uint8_t, 32> pubkey{};
    for (size_t i = 0; i < pubkey.size(); ++i) pubkey[i] = static_cast<uint8_t>(i);

    const ConnectionCard card = build_connection_card(9527, pubkey);
    CHECK(card.port == 9527);
    CHECK(!card.addresses.empty());
    CHECK(card.fingerprint == upix::fingerprint(pubkey));
    CHECK(card.fingerprint.size() == 16);  // sha256[:8] uppercase hex

    // v2 invite contract: the fields Android's JoinInviteParser reads, no
    // extras (every field costs QR modules).
    const std::string& p = card.qr_payload;
    CHECK(p.find(R"("v":2)") != std::string::npos);
    CHECK(p.find(R"("ip":")" + card.addresses[0] + R"(")") != std::string::npos);
    CHECK(p.find(R"("port":9527)") != std::string::npos);
    CHECK(p.find(R"("fp":")" + card.fingerprint + R"(")") != std::string::npos);
    CHECK(p.find(R"("pk":")" + card.pubkey_b64 + R"(")") != std::string::npos);
    CHECK(p.front() == '{' && p.back() == '}');
}

}  // namespace

int main() {
    test_json_escape();
    test_connection_card_payload();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_machine_events: all checks passed\n";
    return 0;
}
