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

    // ── 2. Checksum covers the full frame ─────────────────────────────────
    // Contract: crc32 over the ENTIRE frame ([0, total_size)) computed while
    // the checksum slot [11..14] is zero, then stored into [11..14]. A
    // verifier zeros the slot, recomputes crc32 over the whole frame, and
    // compares against the stored value.
    {
        MessageHeader h;
        h.type = MessageType::MSG_CLIENT_SUBMIT;
        h.payload_length = 64;
        std::vector<uint8_t> pl(64, 0xAB);

        auto w1 = Protocol::serialize(h, pl.data());
        auto w2 = Protocol::serialize(h, pl.data());
        CHECK(w1 == w2);  // deterministic
        CHECK(w1.size() == Protocol::HEADER_SIZE + 64);

        // 1. Recompute: zero the checksum slot in a copy, crc32 the whole
        //    frame, compare against the stored value (read from offset 11).
        uint32_t stored{};
        std::memcpy(&stored, w1.data() + 11, 4);
        CHECK(stored != 0);  // populated for this payload
        auto recompute = w1;
        std::memset(recompute.data() + 11, 0, 4);
        CHECK(Protocol::compute_checksum(recompute.data(), recompute.size()) == stored);

        // 2. Tampering the FIRST payload byte must change the checksum that
        //    serialize stores.
        auto pl_first = pl;
        pl_first[0] ^= 0xFF;
        auto w_first = Protocol::serialize(h, pl_first.data());
        uint32_t cs_first{};
        std::memcpy(&cs_first, w_first.data() + 11, 4);
        CHECK(cs_first != stored);

        // 3. Regression guard for the original bug: flipping the LAST payload
        //    byte must also change the checksum. The old code computed over a
        //    total_size-4 prefix that excluded the last 4 payload bytes, so
        //    this flip left the checksum unchanged; the corrected full-frame
        //    crc32 must catch it.
        auto pl_last = pl;
        pl_last[pl_last.size() - 1] ^= 0xFF;
        auto w_last = Protocol::serialize(h, pl_last.data());
        uint32_t cs_last{};
        std::memcpy(&cs_last, w_last.data() + 11, 4);
        CHECK(cs_last != stored);

        // A different payload must yield a different checksum.
        std::vector<uint8_t> pl2(64, 0xCD);
        auto w3 = Protocol::serialize(h, pl2.data());
        uint32_t cs3{};
        std::memcpy(&cs3, w3.data() + 11, 4);
        CHECK(cs3 != stored);

        // compute_checksum is itself deterministic.
        CHECK(Protocol::compute_checksum(pl.data(), pl.size()) ==
              Protocol::compute_checksum(pl.data(), pl.size()));
    }
    std::printf("OK checksum = full-frame crc32; tamper first/last payload byte\n");

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
        std::strncpy(s.output_format, "avif", sizeof(s.output_format) - 1);
        CHECK(std::strcmp(s.output_format, "avif") == 0);
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
