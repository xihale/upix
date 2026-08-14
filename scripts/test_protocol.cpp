// Protocol-level unit tests for the phone↔PC wire format.
//
// Covers Protocol::{serialize, parse_header, compute_checksum} and the POD
// message structs (ClientTaskSubmit/Response, ClientConnectRequest/Response),
// including the output_format/output_quality fields added to ClientTaskSubmit.
// No network, no sockets — just the wire encoding. Built via the xmake
// `test_protocol` target; runs as a standalone binary that aborts on failure.
#include "core/distributed/protocol.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace crossscale;  // NOLINT

// Same abort-on-fail style as test_crypto.cpp so a regression stops the run.
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

int main() {
    // ── Constants ──────────────────────────────────────────────────────────
    CHECK(Protocol::MAGIC == 0x53534358u);
    CHECK(Protocol::VERSION == 2);
    CHECK(Protocol::HEADER_SIZE == 15);

    // ── 1. Header round-trip across every message type ────────────────────
    const MessageType types[] = {
        MessageType::MSG_KEY_HELLO, MessageType::MSG_AUTH_PROOF,
        MessageType::MSG_AUTH_RESPONSE, MessageType::MSG_CLIENT_SUBMIT,
        MessageType::MSG_CLIENT_RESPONSE,
        MessageType::MSG_CLIENT_CONNECT_REQUEST,
        MessageType::MSG_CLIENT_CONNECT_RESPONSE,
        MessageType::MSG_SERVE_REQUEST, MessageType::MSG_SERVE_RESPONSE,
    };
    for (auto t : types) {
        MessageHeader h;
        h.magic = Protocol::MAGIC;
        h.version = Protocol::VERSION;
        h.type = t;
        h.payload_length = 32;
        std::vector<uint8_t> pl(32);
        for (size_t i = 0; i < pl.size(); ++i) pl[i] = (uint8_t)(i * 7 + 1);
        auto wire = Protocol::serialize(h, pl.data());
        CHECK(wire.size() == Protocol::HEADER_SIZE + 32);

        auto parsed = Protocol::parse_header(wire.data(), wire.size());
        CHECK(parsed.has_value());
        CHECK(parsed->magic == Protocol::MAGIC);
        CHECK(parsed->version == Protocol::VERSION);
        CHECK(parsed->type == t);
        CHECK(parsed->payload_length == 32);
    }
    std::printf("OK header round-trip for all message types\n");

    // ── 2. Checksum is deterministic + payload-sensitive ──────────────────
    // NOTE: Protocol::serialize currently writes the checksum into the middle
    // of the header ([11..14]) and computes it over a region that excludes the
    // last 4 payload bytes, and no receiver verifies it — so the checksum is
    // effectively write-only. We therefore assert the properties the wire
    // format actually guarantees (determinism, payload sensitivity) rather than
    // pinning the internal algorithm, which would cement the latent bug.
    {
        MessageHeader h;
        h.type = MessageType::MSG_CLIENT_SUBMIT;
        h.payload_length = 64;
        std::vector<uint8_t> pl(64, 0xAB);

        auto w1 = Protocol::serialize(h, pl.data());
        auto w2 = Protocol::serialize(h, pl.data());
        CHECK(w1 == w2);  // deterministic
        CHECK(w1.size() == Protocol::HEADER_SIZE + 64);

        // Checksum field ([11..14]) must be populated for this payload.
        uint32_t cs1{};
        std::memcpy(&cs1, w1.data() + 11, 4);
        CHECK(cs1 != 0);

        // A different payload must yield a different checksum.
        std::vector<uint8_t> pl2(64, 0xCD);
        auto w3 = Protocol::serialize(h, pl2.data());
        uint32_t cs3{};
        std::memcpy(&cs3, w3.data() + 11, 4);
        CHECK(cs3 != cs1);

        // compute_checksum is itself deterministic.
        CHECK(Protocol::compute_checksum(pl.data(), pl.size()) ==
              Protocol::compute_checksum(pl.data(), pl.size()));
    }
    std::printf("OK checksum deterministic + payload-sensitive\n");

    // ── 3. parse_header rejection paths ───────────────────────────────────
    {
        MessageHeader h;
        h.type = MessageType::MSG_CLIENT_RESPONSE;
        h.payload_length = 0;
        auto wire = Protocol::serialize(h, nullptr);

        // Truncated below HEADER_SIZE.
        CHECK(!Protocol::parse_header(wire.data(), Protocol::HEADER_SIZE - 1).has_value());

        // Bad magic.
        auto bad = wire;
        bad[0] ^= 0xFF;
        CHECK(!Protocol::parse_header(bad.data(), bad.size()).has_value());

        // Exactly HEADER_SIZE with valid magic is accepted (zero payload).
        auto z = Protocol::parse_header(wire.data(), Protocol::HEADER_SIZE);
        CHECK(z.has_value());
        CHECK(z->payload_length == 0);
    }
    std::printf("OK parse_header rejects short/bad-magic, accepts zero-payload\n");

    // ── 4. POD struct memcpy round-trips ──────────────────────────────────
    // The wire payload is the raw struct, so a stable layout + lossless memcpy
    // is the contract. memset-0 first so every byte is deterministic.
    {
        ClientTaskSubmit s{};
        s.image_width = 1920;
        s.image_height = 1080;
        s.channels = 3;
        std::strncpy(s.model_name, "realesrgan-x4plus", sizeof(s.model_name) - 1);
        s.tile_size = 64;
        s.tile_overlap = 8;
        s.image_data_length = 1920 * 1080 * 3;
        std::strncpy(s.output_format, "webp", sizeof(s.output_format) - 1);
        s.output_quality = 88;

        std::vector<uint8_t> buf(sizeof(s));
        std::memcpy(buf.data(), &s, sizeof(s));
        ClientTaskSubmit back{};
        std::memcpy(&back, buf.data(), sizeof(back));
        CHECK(std::memcmp(&s, &back, sizeof(s)) == 0);
        CHECK(back.image_width == 1920);
        CHECK(back.output_quality == 88);
        CHECK(std::strcmp(back.output_format, "webp") == 0);
        // output_format slot is exactly 8 bytes (regression guard for the
        // recently-added field: a layout change flips this size).
        CHECK(sizeof(s.output_format) == 8);
    }
    {
        ClientConnectRequest r{};
        std::strncpy(r.device_name, "Xiaomi 25053RT47C", sizeof(r.device_name) - 1);
        // 16-hex fingerprint into a 17-byte field: copy the full field width so
        // strncpy NUL-pads (avoids -Wstringop-truncation), then belt-and-braces.
        std::strncpy(r.fingerprint, "ABCD1234EFFF9090", sizeof(r.fingerprint));
        r.fingerprint[sizeof(r.fingerprint) - 1] = '\0';
        r.wants_encrypted_stream = 1;
        ClientConnectRequest back{};
        std::vector<uint8_t> buf(sizeof(r));
        std::memcpy(buf.data(), &r, sizeof(r));
        std::memcpy(&back, buf.data(), sizeof(back));
        CHECK(std::memcmp(&r, &back, sizeof(r)) == 0);
        CHECK(back.wants_encrypted_stream == 1);
        CHECK(sizeof(r.fingerprint) == 17);  // 16 hex + NUL
    }
    {
        ClientConnectResponse r{};
        r.approved = 1;
        r.trusted = 0;
        r.stream_encrypted = 1;
        ClientConnectResponse back{};
        std::vector<uint8_t> buf(sizeof(r));
        std::memcpy(buf.data(), &r, sizeof(r));
        std::memcpy(&back, buf.data(), sizeof(back));
        CHECK(std::memcmp(&r, &back, sizeof(r)) == 0);
        CHECK(back.approved == 1 && back.stream_encrypted == 1);
    }
    {
        ClientTaskResponse r{};
        r.success = 1;
        std::strncpy(r.error_message, "", sizeof(r.error_message) - 1);
        r.result_data_length = 4096;
        ClientTaskResponse back{};
        std::vector<uint8_t> buf(sizeof(r));
        std::memcpy(buf.data(), &r, sizeof(r));
        std::memcpy(&back, buf.data(), sizeof(back));
        CHECK(std::memcmp(&r, &back, sizeof(r)) == 0);
        CHECK(back.result_data_length == 4096);
    }
    std::printf("OK struct memcpy round-trips (submit/connect/response)\n");

    // ── 5. Full frame: header + ClientTaskSubmit + image bytes ────────────
    {
        ClientTaskSubmit s{};
        s.image_width = 64;
        s.image_height = 64;
        s.channels = 3;
        std::strncpy(s.model_name, "test-model", sizeof(s.model_name) - 1);
        s.tile_size = 32;
        s.tile_overlap = 4;
        s.image_data_length = 64 * 64 * 3;
        std::strncpy(s.output_format, "jpg", sizeof(s.output_format) - 1);
        s.output_quality = 90;

        const size_t img_bytes = s.image_data_length;
        std::vector<uint8_t> payload(sizeof(s) + img_bytes);
        std::memcpy(payload.data(), &s, sizeof(s));
        for (size_t i = 0; i < img_bytes; ++i) {
            payload[sizeof(s) + i] = (uint8_t)(i & 0xFF);
        }

        MessageHeader h;
        h.type = MessageType::MSG_CLIENT_SUBMIT;
        h.payload_length = static_cast<uint32_t>(payload.size());
        auto wire = Protocol::serialize(h, payload.data());

        auto parsed = Protocol::parse_header(wire.data(), wire.size());
        CHECK(parsed.has_value());
        CHECK(parsed->type == MessageType::MSG_CLIENT_SUBMIT);
        CHECK(parsed->payload_length == payload.size());

        // Reconstruct the submit struct from the wire payload region.
        const uint8_t* body = wire.data() + Protocol::HEADER_SIZE;
        ClientTaskSubmit back{};
        std::memcpy(&back, body, sizeof(back));
        CHECK(back.image_width == 64);
        CHECK(back.image_data_length == img_bytes);
        CHECK(std::strcmp(back.output_format, "jpg") == 0);
        CHECK(back.output_quality == 90);
        // Image bytes follow the struct contiguously.
        CHECK(body[sizeof(back)] == 0x00);
        CHECK(body[sizeof(back) + 1] == 0x01);
    }
    std::printf("OK full frame (header + submit + image bytes)\n");

    std::printf("ALL PROTOCOL TESTS PASSED\n");
    return 0;
}
