// Golden vector dumper — TEMPORARY tool, NOT part of the workspace build.
//
// Compiles against the original C++ implementation (share/ + libsodium) and
// dumps deterministic wire/crypto vectors as JSON into tests/golden/. The
// Rust test-suite (tests/golden.rs) asserts byte-exact equality against these
// files, proving wire compatibility with old C++/Android clients.
//
// Build & run (from repo root, one line):
//   g++ -std=c++23 -O0 -Wall -I share -I crates/upix-engine-ffi/csrc/upix
//       -I crates/upix-engine-ffi/csrc/upix/utils
//       crates/upix-net/vector_gen/dump.cpp
//       legacy/share/core/distributed/protocol.cpp
//       legacy/share/core/distributed/crypto_session.cpp
//       legacy/share/core/distributed/identity.cpp
//       legacy/share/utils/crypto_sodium.cpp legacy/share/utils/crypto.cpp
//       -lsodium -o /tmp/upix-vgen
//   /tmp/upix-vgen crates/upix-net/tests/golden
//
// All inputs are FIXED byte patterns, so every output below is deterministic
// except the two explicit "random nonce" roundtrip records (which are
// self-consistency checks: open(seal(x)) == x, not cross-impl byte equality).

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/distributed/crypto_session.hpp"
#include "core/distributed/identity.hpp"
#include "core/distributed/protocol.hpp"
#include "utils/crypto.hpp"
#include "utils/crypto_sodium.hpp"

using namespace upix;

// ── fixed test material ─────────────────────────────────────────────────────

static std::array<uint8_t, 32> fixed_pattern(uint8_t base) {
    std::array<uint8_t, 32> out{};
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(base + i);
    return out;
}

// Ephemeral X25519 private keys (clamped by libsodium during scalarmult).
static const auto C_EPH_PRIV = fixed_pattern(0xA0);
static const auto S_EPH_PRIV = fixed_pattern(0xC0);
// Long-term Ed25519 seeds (libsodium secret key = seed || pk).
static const auto C_ID_SEED = fixed_pattern(0x10);
static const auto S_ID_SEED = fixed_pattern(0x30);

static std::array<uint8_t, 24> fixed_nonce() {
    std::array<uint8_t, 24> n{};
    for (size_t i = 0; i < n.size(); ++i) n[i] = static_cast<uint8_t>(0x20 + i);
    return n;
}
// Distinct deterministic nonces for the chunked-continuation vectors.
static std::array<uint8_t, 24> fixed_nonce_variant(uint8_t last) {
    auto n = fixed_nonce();
    n[n.size() - 1] = last;
    return n;
}

static std::vector<uint8_t> pattern_bytes(size_t len) {
    std::vector<uint8_t> out(len);
    for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    return out;
}

// ── tiny JSON writer ────────────────────────────────────────────────────────

static std::string jhex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s = "\"";
    s.reserve(n * 2 + 2);
    for (size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 0xF];
    }
    s += "\"";
    return s;
}
template <size_t N>
static std::string jhex(const std::array<uint8_t, N>& a) {
    return jhex(a.data(), a.size());
}
static std::string jhex(const std::vector<uint8_t>& v) { return jhex(v.data(), v.size()); }

static std::string jstr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += "\"";
    return out;
}

struct JsonFile {
    std::filesystem::path dir;
    std::string name;
    std::string body;

    void write() const {
        std::filesystem::create_directories(dir);
        const auto path = dir / (name + ".json");
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << body;
        if (!out) {
            std::fprintf(stderr, "failed to write %s\n", path.string().c_str());
            std::exit(1);
        }
    }
};

// ── helpers ─────────────────────────────────────────────────────────────────

struct HdrJson {
    uint32_t magic;
    uint16_t type;
    uint32_t payload_length;
    uint32_t checksum;
    bool type_known;

    std::string str() const {
        return "{\"magic\":" + std::to_string(magic) + ",\"type\":" + std::to_string(type) +
               ",\"payload_length\":" + std::to_string(payload_length) +
               ",\"checksum\":" + std::to_string(checksum) +
               ",\"type_known\":" + (type_known ? "true" : "false") + "}";
    }
};

static HdrJson hdr_json(const MessageHeader& h) {
    // MessageType covers the full enum in protocol.hpp; anything outside is a
    // cast artifact of parse_header's unchecked u16 conversion (see dump of
    // unknown_type in parse_header.json; the Rust port returns None there).
    uint16_t raw = static_cast<uint16_t>(h.type);
    bool known = true;
    switch (static_cast<MessageType>(raw)) {  // enumeration mirrors protocol.hpp
        case MessageType::MSG_KEY_HELLO:
        case MessageType::MSG_AUTH_PROOF:
        case MessageType::MSG_AUTH_RESPONSE:
        case MessageType::MSG_CLIENT_SUBMIT:
        case MessageType::MSG_CLIENT_RESPONSE:
        case MessageType::MSG_TASK_PROGRESS:
        case MessageType::MSG_CLIENT_DATA:
        case MessageType::MSG_TASK_CANCEL:
        case MessageType::MSG_CLIENT_CONNECT_REQUEST:
        case MessageType::MSG_CLIENT_CONNECT_RESPONSE:
        case MessageType::MSG_MODEL_LIST_REQUEST:
        case MessageType::MSG_MODEL_LIST_RESPONSE:
        case MessageType::MSG_MODEL_UPLOAD:
        case MessageType::MSG_MODEL_UPLOAD_RESPONSE:
        case MessageType::MSG_MODEL_DOWNLOAD_REQUEST:
        case MessageType::MSG_MODEL_DOWNLOAD_RESPONSE:
        case MessageType::MSG_SERVE_REQUEST:
        case MessageType::MSG_SERVE_RESPONSE:
        case MessageType::MSG_SERVE_DATA:
        case MessageType::MSG_SERVE_PROGRESS: break;
        default: known = false;
    }
    return {h.magic, raw, h.payload_length, h.checksum, known};
}

static std::string serialize_case(const std::string& name, MessageType type,
                                  const void* payload, size_t len) {
    MessageHeader h;
    h.type = type;
    h.payload_length = static_cast<uint32_t>(len);
    auto frame = Protocol::serialize(h, len > 0 ? payload : nullptr);
    auto hdr = Protocol::parse_header(frame);
    if (!hdr) {
        std::fprintf(stderr, "parse roundtrip failed for %s\n", name.c_str());
        std::exit(1);
    }
    return "{\"name\":" + jstr(name) + ",\"type\":" + std::to_string(static_cast<uint16_t>(type)) +
           ",\"header\":" + hdr_json(*hdr).str() + ",\"payload_hex\":" + jhex(static_cast<const uint8_t*>(payload), len) +
           ",\"frame_hex\":" + jhex(frame) + "}";
}

// Zero-copy char-array filler for the fixed-layout wire structs.
template <size_t N>
static void fill_chars(char (&dst)[N], const std::string& s) {
    std::memset(dst, 0, N);
    std::memcpy(dst, s.data(), std::min(s.size(), N - 1));
}

static std::vector<uint8_t> as_bytes(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    return std::vector<uint8_t>(b, b + n);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
        return 2;
    }
    if (sodium_init() < 0) {
        std::fprintf(stderr, "sodium_init failed\n");
        return 1;
    }
    const std::filesystem::path dir = argv[1];

    // ── fixed key material ──────────────────────────────────────────────────
    std::array<uint8_t, 32> c_eph_pub{}, s_eph_pub{};
    crypto_scalarmult_base(c_eph_pub.data(), C_EPH_PRIV.data());
    crypto_scalarmult_base(s_eph_pub.data(), S_EPH_PRIV.data());

    std::array<uint8_t, 32> c_id_pk{}, s_id_pk{};
    std::array<uint8_t, 64> c_id_sk{}, s_id_sk{};
    crypto_sign_seed_keypair(c_id_pk.data(), c_id_sk.data(), C_ID_SEED.data());
    crypto_sign_seed_keypair(s_id_pk.data(), s_id_sk.data(), S_ID_SEED.data());

    // ── ④ session_key.json ──────────────────────────────────────────────────
    {
        std::array<uint8_t, 32> raw{};
        if (crypto_scalarmult_curve25519(raw.data(), C_EPH_PRIV.data(), s_eph_pub.data()) != 0) {
            std::fprintf(stderr, "scalarmult failed\n");
            return 1;
        }
        const auto client_key = sodium::x25519_shared(C_EPH_PRIV, s_eph_pub);
        const auto server_key = sodium::x25519_shared(S_EPH_PRIV, c_eph_pub);
        if (client_key != server_key) {
            std::fprintf(stderr, "session keys diverge\n");
            return 1;
        }
        JsonFile f{dir, "session_key",
                   "{\"c_eph_priv_hex\":" + jhex(C_EPH_PRIV) + ",\"s_eph_priv_hex\":" + jhex(S_EPH_PRIV) +
                   ",\"c_eph_pub_hex\":" + jhex(c_eph_pub) + ",\"s_eph_pub_hex\":" + jhex(s_eph_pub) +
                   ",\"raw_shared_hex\":" + jhex(raw) +
                   // BLAKE2b-256 keyed with "upix-ecdh-v1" (see crypto_sodium.cpp)
                   ",\"client_key_hex\":" + jhex(client_key) + ",\"server_key_hex\":" + jhex(server_key) +
                   ",\"keys_equal\":true}"};
        f.write();
    }

    // ── ⑤ ed25519.json — KeyHello transcript signatures ─────────────────────
    {
        // Transcript (see coordinator run_handshake): c_eph||c_id||s_eph||s_id.
        std::vector<uint8_t> transcript;
        transcript.insert(transcript.end(), c_eph_pub.begin(), c_eph_pub.end());
        transcript.insert(transcript.end(), c_id_pk.begin(), c_id_pk.end());
        transcript.insert(transcript.end(), s_eph_pub.begin(), s_eph_pub.end());
        transcript.insert(transcript.end(), s_id_pk.begin(), s_id_pk.end());

        const auto server_sig = sodium::ed25519_sign(s_id_sk, transcript.data(), transcript.size());
        const auto client_sig = sodium::ed25519_sign(c_id_sk, transcript.data(), transcript.size());

        auto corrupted = server_sig;
        corrupted[0] ^= 0x01;
        const auto tampered = sodium::ed25519_verify(s_id_pk, server_sig, transcript.data(), transcript.size() - 1);
        const auto bad = sodium::ed25519_verify(s_id_pk, corrupted, transcript.data(), transcript.size());
        const auto good = sodium::ed25519_verify(s_id_pk, server_sig, transcript.data(), transcript.size());

        JsonFile f{dir, "ed25519",
                   "{\"s_id_seed_hex\":" + jhex(S_ID_SEED) + ",\"s_id_sk_hex\":" + jhex(s_id_sk) +
                   ",\"s_id_pub_hex\":" + jhex(s_id_pk) +
                   ",\"c_id_seed_hex\":" + jhex(C_ID_SEED) + ",\"c_id_pub_hex\":" + jhex(c_id_pk) +
                   ",\"transcript_hex\":" + jhex(transcript) +
                   ",\"server_sig_hex\":" + jhex(server_sig) +
                   ",\"client_sig_hex\":" + jhex(client_sig) +
                   ",\"verify_good\":" + (good ? "true" : "false") +
                   ",\"verify_corrupted_sig\":" + (bad ? "true" : "false") +
                   ",\"verify_wrong_msg\":" + (tampered ? "true" : "false") + "}"};
        f.write();
    }

    // ── ① serialize.json ────────────────────────────────────────────────────
    {
        std::string body;
        auto push = [&](const std::string& entry) {
            if (!body.empty()) body += ",";
            body += entry;
        };

        // KEY_HELLO with the fixed handshake key material.
        KeyHelloMessage hello{};
        std::memcpy(hello.eph_pub, c_eph_pub.data(), 32);
        std::memcpy(hello.id_pub, c_id_pk.data(), 32);
        std::memset(hello.sig, 0, 64);  // client side: sig zeroed
        push(serialize_case("key_hello_client", MessageType::MSG_KEY_HELLO, &hello, sizeof(hello)));
        std::memcpy(hello.eph_pub, s_eph_pub.data(), 32);
        std::memcpy(hello.id_pub, s_id_pk.data(), 32);
        // server sig over the transcript
        {
            std::vector<uint8_t> transcript;
            transcript.insert(transcript.end(), c_eph_pub.begin(), c_eph_pub.end());
            transcript.insert(transcript.end(), c_id_pk.begin(), c_id_pk.end());
            transcript.insert(transcript.end(), s_eph_pub.begin(), s_eph_pub.end());
            transcript.insert(transcript.end(), s_id_pk.begin(), s_id_pk.end());
            auto sig = sodium::ed25519_sign(s_id_sk, transcript.data(), transcript.size());
            std::memcpy(hello.sig, sig.data(), 64);
        }
        push(serialize_case("key_hello_server", MessageType::MSG_KEY_HELLO, &hello, sizeof(hello)));

        AuthProofMessage proof{};
        {
            std::vector<uint8_t> transcript;
            transcript.insert(transcript.end(), c_eph_pub.begin(), c_eph_pub.end());
            transcript.insert(transcript.end(), c_id_pk.begin(), c_id_pk.end());
            transcript.insert(transcript.end(), s_eph_pub.begin(), s_eph_pub.end());
            transcript.insert(transcript.end(), s_id_pk.begin(), s_id_pk.end());
            auto sig = sodium::ed25519_sign(c_id_sk, transcript.data(), transcript.size());
            std::memcpy(proof.sig, sig.data(), 64);
        }
        push(serialize_case("auth_proof", MessageType::MSG_AUTH_PROOF, &proof, sizeof(proof)));

        AuthResponse auth{};
        auth.authenticated = 1;
        fill_chars(auth.device_id, "0123456789abcdef0123456789abcdef01234");  // 35 chars
        auth.trusted = 1;
        push(serialize_case("auth_response", MessageType::MSG_AUTH_RESPONSE, &auth, sizeof(auth)));

        // Legacy single-frame submit: struct + inline input bytes.
        ClientTaskSubmit submit{};
        fill_chars(submit.model_name, "NomosUni-SPAN-x2");
        fill_chars(submit.input_format, "jpg");
        fill_chars(submit.output_format, "png");
        submit.tile_size = 256;
        submit.tile_overlap = 16;
        submit.output_quality = 90;
        submit.encoder.png_level = -1;
        submit.encoder.jpeg_subsampling = 420;
        submit.encoder.webp_method = -1;
        submit.encoder.webp_lossless = 0;
        submit.encoder.avif_speed = 6;
        submit.encoder.avif_subsampling = 420;
        submit.encoder.avif_threads = 0;
        {
            auto inline_bytes = pattern_bytes(96);
            submit.input_data_length = static_cast<uint32_t>(inline_bytes.size());
            std::vector<uint8_t> payload = as_bytes(&submit, sizeof(submit));
            payload.insert(payload.end(), inline_bytes.begin(), inline_bytes.end());
            push(serialize_case("client_submit_legacy", MessageType::MSG_CLIENT_SUBMIT,
                                payload.data(), payload.size()));
        }
        // Chunked submit: struct-only frame, input rides DATA continuations.
        {
            submit.input_data_length = 12345678;
            push(serialize_case("client_submit_chunked", MessageType::MSG_CLIENT_SUBMIT,
                                &submit, sizeof(submit)));
        }

        TaskProgress progress{0, 42, 17};
        push(serialize_case("task_progress", MessageType::MSG_TASK_PROGRESS, &progress, sizeof(progress)));

        {
            auto chunk = pattern_bytes(32);
            push(serialize_case("client_data", MessageType::MSG_CLIENT_DATA, chunk.data(), chunk.size()));
        }
        push(serialize_case("task_cancel", MessageType::MSG_TASK_CANCEL, nullptr, 0));

        ClientConnectRequest creq{};
        fill_chars(creq.device_name, "Pixel 9 Pro");
        fill_chars(creq.fingerprint, "0123456789ABCDEF");
        push(serialize_case("connect_request", MessageType::MSG_CLIENT_CONNECT_REQUEST, &creq, sizeof(creq)));

        ClientConnectResponse cresp{1, 1, 1, 1, 2};
        push(serialize_case("connect_response", MessageType::MSG_CLIENT_CONNECT_RESPONSE, &cresp, sizeof(cresp)));

        push(serialize_case("model_list_request", MessageType::MSG_MODEL_LIST_REQUEST, nullptr, 0));
        {
            ModelListResponse list{1};
            ModelLibraryEntry entry{};
            fill_chars(entry.name, "NomosUni-SPAN-x2");
            entry.scale_factor = 2;
            entry.total_size = 66554433;
            std::vector<uint8_t> payload = as_bytes(&list, sizeof(list));
            const auto entry_bytes = as_bytes(&entry, sizeof(entry));
            payload.insert(payload.end(), entry_bytes.begin(), entry_bytes.end());
            push(serialize_case("model_list_response", MessageType::MSG_MODEL_LIST_RESPONSE,
                                payload.data(), payload.size()));
        }

        ServeRequest serve{};
        fill_chars(serve.model_name, "");
        fill_chars(serve.output_format, "webp");
        serve.tile_size = 0;
        serve.tile_overlap = 16;
        serve.output_quality = 90;
        {
            const std::string in_path = "/tmp/upix/in.jpg";
            serve.input_path_length = static_cast<uint32_t>(in_path.size());
            serve.output_path_length = 0;  // daemon derives
            std::vector<uint8_t> payload = as_bytes(&serve, sizeof(serve));
            payload.insert(payload.end(), in_path.begin(), in_path.end());
            push(serialize_case("serve_request", MessageType::MSG_SERVE_REQUEST, payload.data(), payload.size()));
        }

        ServeProgress sprog{7, 169, 0};
        push(serialize_case("serve_progress", MessageType::MSG_SERVE_PROGRESS, &sprog, sizeof(sprog)));

        ClientTaskResponse resp{};
        resp.success = 1;
        fill_chars(resp.error_message, "");
        resp.result_data_length = 4096;
        push(serialize_case("client_response", MessageType::MSG_CLIENT_RESPONSE, &resp, sizeof(resp)));

        JsonFile f{dir, "serialize", "[" + body + "]"};
        f.write();
    }

    // ── ② parse_header.json ─────────────────────────────────────────────────
    {
        std::string body;
        auto push = [&](const std::string& entry) {
            if (!body.empty()) body += ",";
            body += entry;
        };
        auto case_ok = [&](const std::string& name, const std::vector<uint8_t>& bytes) {
            auto h = Protocol::parse_header(bytes);
            if (!h) {
                std::fprintf(stderr, "parse case %s unexpectedly failed\n", name.c_str());
                std::exit(1);
            }
            push("{\"name\":" + jstr(name) + ",\"bytes_hex\":" + jhex(bytes) +
                 ",\"ok\":true,\"header\":" + hdr_json(*h).str() + "}");
        };
        auto case_err = [&](const std::string& name, const std::vector<uint8_t>& bytes) {
            auto h = Protocol::parse_header(bytes);
            if (h) {
                std::fprintf(stderr, "parse case %s unexpectedly succeeded\n", name.c_str());
                std::exit(1);
            }
            push("{\"name\":" + jstr(name) + ",\"bytes_hex\":" + jhex(bytes) + ",\"ok\":false}");
        };

        // Roundtrip of a real frame header (key_hello_client serialize case).
        KeyHelloMessage hello{};
        std::memcpy(hello.eph_pub, c_eph_pub.data(), 32);
        std::memcpy(hello.id_pub, c_id_pk.data(), 32);
        MessageHeader h;
        h.type = MessageType::MSG_KEY_HELLO;
        h.payload_length = sizeof(hello);
        auto frame = Protocol::serialize(h, &hello);
        case_ok("roundtrip_key_hello", std::vector<uint8_t>(frame.begin(), frame.begin() + Protocol::HEADER_SIZE));
        case_err("empty", {});
        case_err("truncated_8", std::vector<uint8_t>(frame.begin(), frame.begin() + 8));
        case_err("truncated_13", std::vector<uint8_t>(frame.begin(), frame.begin() + 13));

        {
            auto bad = std::vector<uint8_t>(frame.begin(), frame.begin() + Protocol::HEADER_SIZE);
            const uint32_t bad_magic = 0x12345678;
            std::memcpy(bad.data(), &bad_magic, 4);
            case_err("bad_magic", bad);
        }
        {
            // Unknown type u16: C++ parse_header casts unchecked and returns a
            // header (type_known=false); the Rust port returns None (its
            // MessageHeader carries a closed MessageType enum). Recorded here
            // so the divergence is pinned, not accidental.
            auto unknown = std::vector<uint8_t>(frame.begin(), frame.begin() + Protocol::HEADER_SIZE);
            const uint16_t raw_type = 0xFFFF;
            std::memcpy(unknown.data() + 4, &raw_type, 2);
            auto h = Protocol::parse_header(unknown);
            if (!h) {
                std::fprintf(stderr, "unknown-type case unexpectedly failed\n");
                std::exit(1);
            }
            push("{\"name\":\"unknown_type\",\"bytes_hex\":" + jhex(unknown) +
                 ",\"ok\":true,\"header\":" + hdr_json(*h).str() +
                 ",\"rust_parses\":false}");
        }
        JsonFile f{dir, "parse_header", "[" + body + "]"};
        f.write();
    }

    // ── ③ checksum.json ─────────────────────────────────────────────────────
    {
        std::string body;
        auto push = [&](const std::string& entry) {
            if (!body.empty()) body += ",";
            body += entry;
        };

        auto crc_case = [&](const std::string& name, const std::vector<uint8_t>& data) {
            push("{\"name\":" + jstr(name) + ",\"data_hex\":" + jhex(data) +
                 ",\"crc32\":" + std::to_string(Protocol::compute_checksum(data.data(), data.size())) + "}");
        };
        crc_case("crc_empty", {});
        crc_case("crc_ascii", std::vector<uint8_t>({'u', 'p', 'i', 'x'}));
        {
            std::vector<uint8_t> pattern(256);
            for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<uint8_t>(i);
            crc_case("crc_pattern_256", pattern);
        }

        // verify_frame samples built from a real serialize() frame.
        ClientTaskSubmit submit{};
        fill_chars(submit.model_name, "NomosUni-SPAN-x2");
        submit.tile_size = 256;
        submit.tile_overlap = 16;
        submit.output_quality = 90;
        submit.input_data_length = 96;
        auto payload = as_bytes(&submit, sizeof(submit));
        MessageHeader h;
        h.type = MessageType::MSG_CLIENT_SUBMIT;
        h.payload_length = static_cast<uint32_t>(payload.size());
        auto frame = Protocol::serialize(h, payload.data());
        std::array<uint8_t, Protocol::HEADER_SIZE> hdr{};
        std::memcpy(hdr.data(), frame.data(), Protocol::HEADER_SIZE);

        auto verify_case = [&](const std::string& name, const std::array<uint8_t, Protocol::HEADER_SIZE>& hh,
                               const std::vector<uint8_t>& pay) {
            push("{\"name\":" + jstr(name) + ",\"header_hex\":" + jhex(hh.data(), hh.size()) +
                 ",\"payload_hex\":" + jhex(pay) +
                 ",\"ok\":" + (Protocol::verify_frame(hh.data(), pay.data(), pay.size()) ? "true" : "false") + "}");
        };
        verify_case("verify_valid", hdr, payload);
        {
            auto corrupt = payload;
            corrupt[0] ^= 0x01;
            verify_case("verify_corrupt_payload", hdr, corrupt);
        }
        {
            auto bad_hdr = hdr;
            bad_hdr[10] ^= 0xFF;  // tamper checksum slot
            verify_case("verify_bad_checksum", bad_hdr, payload);
        }
        {
            // Encrypted frames carry checksum 0 and MUST skip verify_frame:
            // record the expected false so the skip rule is pinned.
            auto enc_hdr = hdr;
            std::memset(enc_hdr.data() + 10, 0, 4);
            verify_case("verify_encrypted_zero_checksum", enc_hdr, payload);
        }
        JsonFile f{dir, "checksum", "[" + body + "]"};
        f.write();
    }

    // ── ⑥ secretbox.json + session_seal.json ────────────────────────────────
    const auto session_key = sodium::x25519_shared(C_EPH_PRIV, s_eph_pub);
    {
        const auto nonce = fixed_nonce();
        const auto pt = pattern_bytes(100);
        std::vector<uint8_t> ct(pt.size() + crypto_secretbox_MACBYTES);
        crypto_secretbox_easy(ct.data(), pt.data(), pt.size(), nonce.data(), session_key.data());
        std::vector<uint8_t> nonce_ct;
        nonce_ct.reserve(nonce.size() + ct.size());
        nonce_ct.insert(nonce_ct.end(), nonce.begin(), nonce.end());
        nonce_ct.insert(nonce_ct.end(), ct.begin(), ct.end());

        // Random-nonce self-consistency via the same C++ code path the Rust
        // port must mirror (open(seal(x)) == x).
        auto sealed = sodium::secretbox_seal(session_key.data(), pt.data(), pt.size());
        auto opened = sodium::secretbox_open(session_key.data(), sealed.data(), sealed.size());
        if (!opened || *opened != pt) {
            std::fprintf(stderr, "secretbox open(seal(x)) failed\n");
            return 1;
        }

        JsonFile f{dir, "secretbox",
                   "{\"key_hex\":" + jhex(session_key) + ",\"nonce_hex\":" + jhex(nonce) +
                   ",\"plaintext_hex\":" + jhex(pt) +
                   // fixed key+nonce: sealed = ct||mac (crypto_secretbox_easy)
                   ",\"sealed_hex\":" + jhex(ct) +
                   // full wire form: nonce||ct||mac
                   ",\"nonce_sealed_hex\":" + jhex(nonce_ct) +
                   // random nonce roundtrip: cross-impl asserts opened==pt only
                   ",\"roundtrip_nonce_ct_hex\":" + jhex(sealed) +
                   ",\"roundtrip_plaintext_hex\":" + jhex(pt) + ",\"roundtrip_ok\":true}"};
        f.write();
    }
    {
        // EncryptedSession fixed key+nonce seal bytes: framing identical to
        // EncryptedSession::seal() but with deterministic nonces so the Rust
        // port can assert byte equality. Chunked MSG_CLIENT_DATA continuation
        // frames are covered by the same vectors.
        std::string body;
        auto push = [&](const std::string& entry) {
            if (!body.empty()) body += ",";
            body += entry;
        };
        auto seal_with_nonce = [&](MessageType type, const std::vector<uint8_t>& pt,
                                   const std::array<uint8_t, 24>& nonce) {
            // Mirror EncryptedSession::seal(): payload_length = 24 + pt + 16,
            // checksum forced to 0, then nonce||ct||mac appended.
            const size_t nonce_ct_len = nonce.size() + pt.size() + crypto_secretbox_MACBYTES;
            std::vector<uint8_t> wire(Protocol::HEADER_SIZE + nonce_ct_len);
            const uint32_t magic = Protocol::MAGIC;
            const uint16_t t = static_cast<uint16_t>(type);
            const uint32_t plen = static_cast<uint32_t>(nonce_ct_len);
            std::memcpy(wire.data() + 0, &magic, 4);
            std::memcpy(wire.data() + 4, &t, 2);
            std::memcpy(wire.data() + 6, &plen, 4);
            std::memset(wire.data() + 10, 0, 4);
            std::vector<uint8_t> nonce_ct(nonce_ct_len);
            std::memcpy(nonce_ct.data(), nonce.data(), nonce.size());
            crypto_secretbox_easy(nonce_ct.data() + nonce.size(), pt.data(), pt.size(),
                                  nonce.data(), session_key.data());
            std::memcpy(wire.data() + Protocol::HEADER_SIZE, nonce_ct.data(), nonce_ct.size());
            return wire;
        };
        auto seal_case = [&](const std::string& name, MessageType type, const std::vector<uint8_t>& pt,
                             const std::array<uint8_t, 24>& nonce) {
            auto wire = seal_with_nonce(type, pt, nonce);
            // Verify with the real EncryptedSession that open() restores it.
            EncryptedSession session;
            session.activate(session_key);
            auto opened = session.open(wire.data(), wire.size());
            if (!opened || opened->second != pt || opened->first.type != type ||
                opened->first.payload_length != pt.size()) {
                std::fprintf(stderr, "session open() mismatch for %s\n", name.c_str());
                return;
            }
            push("{\"name\":" + jstr(name) + ",\"msg_type\":" + std::to_string(static_cast<uint16_t>(type)) +
                 ",\"key_hex\":" + jhex(session_key) + ",\"nonce_hex\":" + jhex(nonce) +
                 ",\"plaintext_hex\":" + jhex(pt) + ",\"wire_hex\":" + jhex(wire) +
                 ",\"opened_header\":" + hdr_json(opened->first).str() +
                 ",\"opened_payload_hex\":" + jhex(opened->second) + "}");
        };

        ClientConnectRequest creq{};
        fill_chars(creq.device_name, "Pixel 9 Pro");
        fill_chars(creq.fingerprint, "0123456789ABCDEF");
        seal_case("seal_connect_request", MessageType::MSG_CLIENT_CONNECT_REQUEST,
                  as_bytes(&creq, sizeof(creq)), fixed_nonce());

        auto chunk0 = pattern_bytes(64);
        auto chunk1 = pattern_bytes(40);
        seal_case("seal_client_data_chunk0", MessageType::MSG_CLIENT_DATA, chunk0, fixed_nonce_variant(0x21));
        seal_case("seal_client_data_chunk1", MessageType::MSG_CLIENT_DATA, chunk1, fixed_nonce_variant(0x22));

        AuthProofMessage proof{};
        {
            std::vector<uint8_t> transcript;
            std::array<uint8_t, 32> c_eph_pub{}, s_eph_pub{};
            crypto_scalarmult_base(c_eph_pub.data(), C_EPH_PRIV.data());
            crypto_scalarmult_base(s_eph_pub.data(), S_EPH_PRIV.data());
            std::array<uint8_t, 32> c_id_pk{}, s_id_pk{};
            std::array<uint8_t, 64> c_id_sk{}, s_id_sk{};
            crypto_sign_seed_keypair(c_id_pk.data(), c_id_sk.data(), C_ID_SEED.data());
            crypto_sign_seed_keypair(s_id_pk.data(), s_id_sk.data(), S_ID_SEED.data());
            transcript.insert(transcript.end(), c_eph_pub.begin(), c_eph_pub.end());
            transcript.insert(transcript.end(), c_id_pk.begin(), c_id_pk.end());
            transcript.insert(transcript.end(), s_eph_pub.begin(), s_eph_pub.end());
            transcript.insert(transcript.end(), s_id_pk.begin(), s_id_pk.end());
            auto sig = sodium::ed25519_sign(c_id_sk, transcript.data(), transcript.size());
            std::memcpy(proof.sig, sig.data(), 64);
        }
        seal_case("seal_auth_proof", MessageType::MSG_AUTH_PROOF, as_bytes(&proof, sizeof(proof)),
                  fixed_nonce_variant(0x23));

        // Random-nonce seal/open roundtrip through the real EncryptedSession
        // (cross-impl asserts semantics, not bytes).
        {
            EncryptedSession session;
            session.activate(session_key);
            auto pt = pattern_bytes(300);
            MessageHeader h;
            h.type = MessageType::MSG_CLIENT_SUBMIT;
            h.payload_length = static_cast<uint32_t>(pt.size());
            auto wire = session.seal(h, pt.data());
            auto opened = session.open(wire.data(), wire.size());
            if (!opened || opened->second != pt || opened->first.type != MessageType::MSG_CLIENT_SUBMIT ||
                opened->first.payload_length != pt.size() ||
                opened->first.payload_length + 40 != wire.size() - Protocol::HEADER_SIZE) {
                std::fprintf(stderr, "session roundtrip failed\n");
                return 1;
            }
            // Encrypted frames must carry checksum 0 in the wire header.
            uint32_t wire_checksum = 0;
            std::memcpy(&wire_checksum, wire.data() + 10, 4);
            if (wire_checksum != 0) {
                std::fprintf(stderr, "sealed frame checksum not zeroed\n");
                return 1;
            }
        }

        JsonFile f{dir, "session_seal", "[" + body + "]"};
        f.write();
    }

    // ── ⑦ fingerprint.json ──────────────────────────────────────────────────
    {
        std::array<uint8_t, 32> s_id_pk{}, s_id_sk{};
        crypto_sign_seed_keypair(s_id_pk.data(), s_id_sk.data(), S_ID_SEED.data());
        const auto full = crypto::sha256_hex(
            std::string(reinterpret_cast<const char*>(s_id_pk.data()), 32));
        JsonFile f{dir, "fingerprint",
                   "{\"pubkey_hex\":" + jhex(s_id_pk) + ",\"sha256_full_hex\":" + jstr(full) +
                   ",\"fingerprint\":" + jstr(fingerprint(s_id_pk)) + "}"};
        f.write();
    }

    // ── ⑧ truststore.json ───────────────────────────────────────────────────
    {
        std::array<uint8_t, 32> k1 = fixed_pattern(0x60);
        std::array<uint8_t, 32> k2 = fixed_pattern(0x80);
        std::array<uint8_t, 32> kshort{};
        for (size_t i = 0; i < 16; ++i) kshort[i] = static_cast<uint8_t>(0xE0 + i);

        const std::string valid1 = base64_encode(k1.data(), 32);
        const std::string valid2 = base64_encode(k2.data(), 32);
        const std::string short_line = base64_encode(kshort.data(), 16);

        const std::string content =
            "# comment-style garbage is just an invalid line\n"  // decodes? fails → dropped
            + valid1 + "\n"
            + "\n"
            + "   \t \n"
            + "!!invalid!!\n"
            + short_line + "\n"
            + "  " + valid2 + "\t\n"
            + valid1 + "\n";  // duplicate: load() does NOT dedupe

        const auto tmp = dir / "vector_trusted_clients.tmp";
        {
            std::filesystem::create_directories(dir);
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << content;
        }
        const auto store = TrustStore::load(tmp);
        std::filesystem::remove(tmp);

        // TrustStore exposes only contains()/size(), so the entry list below
        // is computed with the exact same loop load() runs (getline → strip
        // whitespace → base64 decode → keep len==32), then cross-checked
        // against the real object.
        std::vector<std::array<uint8_t, 32>> entries;
        {
            std::istringstream in(content);
            std::string line;
            while (std::getline(in, line)) {
                line.erase(std::remove_if(line.begin(), line.end(),
                                          [](unsigned char c) { return std::isspace(c); }),
                           line.end());
                if (line.empty()) continue;
                auto bytes = base64_decode(line);
                if (bytes && bytes->size() == 32) {
                    std::array<uint8_t, 32> pk{};
                    std::memcpy(pk.data(), bytes->data(), 32);
                    entries.push_back(pk);
                }
            }
        }
        if (entries.size() != store.size()) {
            std::fprintf(stderr, "truststore mirrored parse disagrees with load()\n");
            return 1;
        }
        std::string entries_json;
        for (const auto& e : entries) {
            if (!entries_json.empty()) entries_json += ",";
            entries_json += jhex(e);
        }
        std::array<uint8_t, 32> unknown{};
        for (size_t i = 0; i < 32; ++i) unknown[i] = static_cast<uint8_t>(0xAA ^ i);
        JsonFile f{dir, "truststore",
                   "{\"content\":" + jstr(content) + ",\"entries_hex\":[" + entries_json +
                   "],\"size\":" + std::to_string(store.size()) +
                   ",\"contains_valid1\":" + (store.contains(k1) ? "true" : "false") +
                   ",\"contains_valid2\":" + (store.contains(k2) ? "true" : "false") +
                   ",\"contains_unknown\":" + (store.contains(unknown) ? "true" : "false") + "}"};
        f.write();
    }

    std::fprintf(stderr, "golden vectors written to %s\n", dir.string().c_str());
    return 0;
}
