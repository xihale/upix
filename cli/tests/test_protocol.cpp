// Protocol-level unit tests for the phone↔PC wire format.
//
// Covers Protocol::{serialize, parse_header, compute_checksum} and the POD
// message structs (ClientTaskSubmit/Response, ClientConnectRequest/Response).
// No network, no sockets — just the wire encoding. Built via the xmake
// `test_protocol` target; runs as a standalone binary that aborts on failure.
#include "core/distributed/protocol.hpp"
#include <cstring>
#include <span>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace upix;  // NOLINT

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
    CHECK(Protocol::HEADER_SIZE == 14);

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
        h.type = t;
        h.payload_length = 32;
        std::vector<uint8_t> pl(32);
        for (size_t i = 0; i < pl.size(); ++i) pl[i] = (uint8_t)(i * 7 + 1);
        auto wire = Protocol::serialize(h, pl.data());
        CHECK(wire.size() == Protocol::HEADER_SIZE + 32);

        auto parsed = Protocol::parse_header(wire);
        CHECK(parsed.has_value());
        CHECK(parsed->magic == Protocol::MAGIC);
        CHECK(parsed->type == t);
        CHECK(parsed->payload_length == 32);
    }
    std::printf("OK header round-trip for all message types\n");

    // ── 2. Checksum covers the full frame ─────────────────────────────────
    // Contract: crc32 over the ENTIRE frame ([0, total_size)) computed while
    // the checksum slot [10..13] is zero, then stored into [10..13]. A
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
        //    frame, compare against the stored value (read from offset 10).
        uint32_t stored{};
        std::memcpy(&stored, w1.data() + 10, 4);
        CHECK(stored != 0);  // populated for this payload
        auto recompute = w1;
        std::memset(recompute.data() + 10, 0, 4);
        CHECK(Protocol::compute_checksum(recompute.data(), recompute.size()) == stored);

        // 2. Tampering the FIRST payload byte must change the checksum that
        //    serialize stores.
        auto pl_first = pl;
        pl_first[0] ^= 0xFF;
        auto w_first = Protocol::serialize(h, pl_first.data());
        uint32_t cs_first{};
        std::memcpy(&cs_first, w_first.data() + 10, 4);
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
        std::memcpy(&cs_last, w_last.data() + 10, 4);
        CHECK(cs_last != stored);

        // A different payload must yield a different checksum.
        std::vector<uint8_t> pl2(64, 0xCD);
        auto w3 = Protocol::serialize(h, pl2.data());
        uint32_t cs3{};
        std::memcpy(&cs3, w3.data() + 10, 4);
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
        CHECK(!Protocol::parse_header(std::span(wire.data(), Protocol::HEADER_SIZE - 1)).has_value());

        // Bad magic.
        auto bad = wire;
        bad[0] ^= 0xFF;
        CHECK(!Protocol::parse_header(bad).has_value());

        // Exactly HEADER_SIZE with valid magic is accepted (zero payload).
        auto z = Protocol::parse_header(wire);
        CHECK(z.has_value());
        CHECK(z->payload_length == 0);
    }
    std::printf("OK parse_header rejects short/bad-magic, accepts zero-payload\n");

    // ── 4. POD struct memcpy round-trips ──────────────────────────────────
    // The wire payload is the raw struct, so a stable layout + lossless memcpy
    // is the contract. memset-0 first so every byte is deterministic.
    {
        ClientTaskSubmit s{};
        std::strncpy(s.model_name, "realesrgan-x4plus", sizeof(s.model_name) - 1);
        std::strncpy(s.input_format, "jpg", sizeof(s.input_format) - 1);
        s.tile_size = 64;
        s.tile_overlap = 8;
        s.output_quality = 88;
        s.input_data_length = 1920 * 1080;

        std::vector<uint8_t> buf(sizeof(s));
        std::memcpy(buf.data(), &s, sizeof(s));
        ClientTaskSubmit back{};
        std::memcpy(&back, buf.data(), sizeof(back));
        CHECK(std::memcmp(&s, &back, sizeof(s)) == 0);
        CHECK(back.tile_size == 64);
        CHECK(back.output_quality == 88);
        CHECK(std::strcmp(back.input_format, "jpg") == 0);
        // Fixed-width char fields keep their layout contract.
        CHECK(sizeof(s.model_name) == 64);
        CHECK(sizeof(s.input_format) == 16);
        // Encoder knob block: appended-at-tail fields + wire conversion round-trip.
        s.encoder = to_wire(EncoderOptions{
            .png_level = 7,
            .jpeg_subsampling = 444,
            .webp_method = 5,
            .webp_lossless = true,
            .avif_speed = 9,
            .avif_subsampling = 444,
            .avif_threads = 12,
        });
        std::memcpy(buf.data(), &s, sizeof(s));
        std::memcpy(&back, buf.data(), sizeof(back));
        CHECK(back.encoder.avif_speed == 9);
        CHECK(back.encoder.webp_lossless == 1);
        const EncoderOptions decoded = from_wire(back.encoder);
        CHECK(decoded.png_level == 7);
        CHECK(decoded.jpeg_subsampling == 444);
        CHECK(decoded.webp_method == 5);
        CHECK(decoded.webp_lossless);
        CHECK(decoded.avif_subsampling == 444);
        CHECK(decoded.avif_threads == 12);
        // Appended layouts keep natural 4-byte alignment on arm64 and x86_64.
        CHECK(sizeof(EncoderOptionsWire) == 28);
        CHECK(sizeof(ClientTaskSubmit) == 140);
        CHECK(sizeof(ServeRequest) == 128);
    }
    {
        ClientConnectRequest r{};
        std::strncpy(r.device_name, "Xiaomi 25053RT47C", sizeof(r.device_name) - 1);
        // 16-hex fingerprint into a 17-byte field: copy the full field width so
        // strncpy NUL-pads (avoids -Wstringop-truncation), then belt-and-braces.
        std::strncpy(r.fingerprint, "ABCD1234EFFF9090", sizeof(r.fingerprint));
        r.fingerprint[sizeof(r.fingerprint) - 1] = '\0';
        ClientConnectRequest back{};
        std::vector<uint8_t> buf(sizeof(r));
        std::memcpy(buf.data(), &r, sizeof(r));
        std::memcpy(&back, buf.data(), sizeof(back));
        CHECK(std::memcmp(&r, &back, sizeof(r)) == 0);
        CHECK(sizeof(r.fingerprint) == 17);  // 16 hex + NUL
    }
    {
        ClientConnectResponse r{};
        r.approved = 1;
        r.trusted = 0;
        r.avif_decode = 1;
        r.avif_encode = 0;  // lean host without a system encoder-capable libavif
        r.pipeline_depth = 2;
        ClientConnectResponse back{};
        std::vector<uint8_t> buf(sizeof(r));
        std::memcpy(buf.data(), &r, sizeof(r));
        std::memcpy(&back, buf.data(), sizeof(back));
        CHECK(std::memcmp(&r, &back, sizeof(r)) == 0);
        CHECK(back.approved == 1 && back.trusted == 0);
        // Capability bits + pipeline depth ride the pairing frame: 5×uint8,
        // no padding. Receivers keep a legacy 4-byte floor (older PCs omit
        // the depth byte), so growing this struct must stay append-only.
        CHECK(sizeof(ClientConnectResponse) == 5);
        CHECK(back.avif_decode == 1 && back.avif_encode == 0);
        CHECK(back.pipeline_depth == 2);
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

    // ── 5. Full frame: header + ClientTaskSubmit + input file bytes ────────
    {
        ClientTaskSubmit s{};
        std::strncpy(s.model_name, "test-model", sizeof(s.model_name) - 1);
        std::strncpy(s.input_format, "png", sizeof(s.input_format) - 1);
        s.tile_size = 32;
        s.tile_overlap = 4;
        s.output_quality = 90;
        s.input_data_length = 4096;

        const size_t img_bytes = s.input_data_length;
        std::vector<uint8_t> payload(sizeof(s) + img_bytes);
        std::memcpy(payload.data(), &s, sizeof(s));
        for (size_t i = 0; i < img_bytes; ++i) {
            payload[sizeof(s) + i] = (uint8_t)(i & 0xFF);
        }

        MessageHeader h;
        h.type = MessageType::MSG_CLIENT_SUBMIT;
        h.payload_length = static_cast<uint32_t>(payload.size());
        auto wire = Protocol::serialize(h, payload.data());

        auto parsed = Protocol::parse_header(wire);
        CHECK(parsed.has_value());
        CHECK(parsed->type == MessageType::MSG_CLIENT_SUBMIT);
        CHECK(parsed->payload_length == payload.size());

        // Reconstruct the submit struct from the wire payload region.
        const uint8_t* body = wire.data() + Protocol::HEADER_SIZE;
        ClientTaskSubmit back{};
        std::memcpy(&back, body, sizeof(back));
        CHECK(back.tile_size == 32);
        CHECK(back.input_data_length == img_bytes);
        CHECK(std::strcmp(back.input_format, "png") == 0);
        CHECK(back.output_quality == 90);
        // Input bytes follow the struct contiguously.
        CHECK(body[sizeof(back)] == 0x00);
        CHECK(body[sizeof(back) + 1] == 0x01);
    }
    std::printf("OK full frame (header + submit + input bytes)\n");

    std::printf("ALL PROTOCOL TESTS PASSED\n");
    return 0;
}
