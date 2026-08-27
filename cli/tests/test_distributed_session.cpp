// Loopback test for the persistent phone↔PC session loop in Coordinator.
//
// One TCP connection runs: ECDH+Ed25519 handshake → CONNECT_REQUEST/RESPONSE
// (auto-approved via a pre-seeded TrustStore) → CLIENT_SUBMIT → response →
// CLIENT_SUBMIT → response. Receiving the SECOND response on the same
// connection is the regression guard: the old one-shot server closed after
// the first CLIENT_RESPONSE, so submit #2 would hit a dead socket.
//
// The submit uses a deliberately missing model name, so both tasks fail
// fast inside process_locally (engine/model not ready) with success=0 —
// exactly the per-task-failure path that must keep the session usable.
// No GPU/model files are required for this test.
//
// Built via the xmake `test_distributed_session` target; aborts on failure.
#include "core/distributed/coordinator.hpp"
#include "core/distributed/protocol.hpp"
#include "core/distributed/crypto_session.hpp"
#include "core/distributed/identity.hpp"
#include "utils/crypto_sodium.hpp"
#include "utils/image.hpp"
#include "utils/network.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
using namespace upix;  // NOLINT

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

namespace {

// Client side of the ECDH+signature handshake, strict-MITM against the
// server's known identity key (mirrors jni_bridge.cpp run_client_handshake).
auto client_handshake(TCPSocket& sock, const IdentityKey& server_identity,
                      const IdentityKey& client_identity)
    -> std::optional<EncryptedSession> {
    // (1) Send client KEY_HELLO (plaintext, no signature).
    auto eph = sodium::x25519_keygen();
    KeyHelloMessage c_hello{};
    std::memcpy(c_hello.eph_pub, eph.public_key.data(), 32);
    std::memcpy(c_hello.id_pub, client_identity.public_key().data(), 32);
    {
        MessageHeader hh{};
        hh.type = MessageType::MSG_KEY_HELLO;
        hh.payload_length = sizeof(c_hello);
        auto wire = Protocol::serialize(hh, &c_hello);
        if (!sock.send_all(wire.data(), wire.size()).has_value()) return std::nullopt;
    }

    // (2) Receive server KEY_HELLO (eph + id + transcript signature).
    KeyHelloMessage s_hello{};
    {
        std::vector<uint8_t> hdr(Protocol::HEADER_SIZE);
        if (!sock.receive_all(hdr.data(), hdr.size()).has_value()) return std::nullopt;
        auto parsed = Protocol::parse_header(hdr);
        if (!parsed || parsed->type != MessageType::MSG_KEY_HELLO ||
            parsed->payload_length != sizeof(s_hello)) {
            return std::nullopt;
        }
        std::vector<uint8_t> payload(parsed->payload_length);
        if (!sock.receive_all(payload.data(), payload.size()).has_value()) return std::nullopt;
        std::memcpy(&s_hello, payload.data(), sizeof(s_hello));
    }

    // (3) Transcript c_eph||c_id||s_eph||s_id, verify server signature.
    std::array<uint8_t, 128> transcript{};
    std::memcpy(transcript.data() + 0,  c_hello.eph_pub, 32);
    std::memcpy(transcript.data() + 32, c_hello.id_pub,  32);
    std::memcpy(transcript.data() + 64, s_hello.eph_pub, 32);
    std::memcpy(transcript.data() + 96, s_hello.id_pub,  32);
    std::array<uint8_t, 32> s_id_pub{};
    std::memcpy(s_id_pub.data(), s_hello.id_pub, 32);
    if (s_id_pub != server_identity.public_key()) {
        std::fprintf(stderr, "server identity mismatch (MITM check failed)\n");
        return std::nullopt;
    }
    std::array<uint8_t, 64> s_sig{};
    std::memcpy(s_sig.data(), s_hello.sig, 64);
    if (!sodium::ed25519_verify(s_id_pub, s_sig, transcript.data(), transcript.size())) {
        return std::nullopt;
    }

    // (4) Derive session key, send AUTH_PROOF with our transcript signature.
    EncryptedSession session;
    {
        std::array<uint8_t, 32> s_eph_pub{};
        std::memcpy(s_eph_pub.data(), s_hello.eph_pub, 32);
        session.activate(sodium::x25519_shared(eph.private_key, s_eph_pub));
    }
    {
        auto c_sig = client_identity.sign(transcript.data(), transcript.size());
        AuthProofMessage proof{};
        std::memcpy(proof.sig, c_sig.data(), 64);
        MessageHeader ph{};
        ph.type = MessageType::MSG_AUTH_PROOF;
        ph.payload_length = sizeof(proof);
        auto wire = session.seal(ph, &proof);
        if (!sock.send_all(wire.data(), wire.size()).has_value()) return std::nullopt;
    }

    // (5) Receive encrypted AUTH_RESPONSE.
    {
        std::vector<uint8_t> hdr(Protocol::HEADER_SIZE);
        if (!sock.receive_all(hdr.data(), hdr.size()).has_value()) return std::nullopt;
        auto parsed = Protocol::parse_header(hdr);
        if (!parsed || parsed->payload_length > MAX_NETWORK_PAYLOAD) return std::nullopt;
        std::vector<uint8_t> wire(Protocol::HEADER_SIZE + parsed->payload_length);
        std::memcpy(wire.data(), hdr.data(), Protocol::HEADER_SIZE);
        if (parsed->payload_length > 0 &&
            !sock.receive_all(wire.data() + Protocol::HEADER_SIZE,
                              parsed->payload_length).has_value()) {
            return std::nullopt;
        }
        auto opened = session.open(wire.data(), wire.size());
        if (!opened || opened->first.type != MessageType::MSG_AUTH_RESPONSE ||
            opened->second.size() < sizeof(AuthResponse)) {
            return std::nullopt;
        }
        const auto* resp = reinterpret_cast<const AuthResponse*>(opened->second.data());
        if (resp->authenticated == 0) return std::nullopt;
    }
    return session;
}

auto send_sealed(TCPSocket& sock, EncryptedSession& session, MessageType type,
                 const void* payload, size_t len) -> bool {
    MessageHeader h{};
    h.type = type;
    h.payload_length = static_cast<uint32_t>(len);
    auto wire = session.seal(h, payload);
    return sock.send_all(wire.data(), wire.size()).has_value();
}

auto make_task(const std::vector<uint8_t>& image_bytes, const char* model_name,
               const char* input_format, uint32_t tile_size) -> ClientTaskSubmit {
    ClientTaskSubmit task{};
    std::strncpy(task.model_name, model_name, sizeof(task.model_name) - 1);
    std::strncpy(task.input_format, input_format, sizeof(task.input_format) - 1);
    std::strncpy(task.output_format, "jpg", sizeof(task.output_format) - 1);
    task.tile_size = tile_size;
    task.tile_overlap = 8;
    task.output_quality = 90;
    task.input_data_length = static_cast<uint32_t>(image_bytes.size());
    return task;
}

auto recv_sealed(TCPSocket& sock, EncryptedSession& session)
    -> std::optional<std::pair<MessageHeader, std::vector<uint8_t>>> {
    std::vector<uint8_t> hdr(Protocol::HEADER_SIZE);
    if (!sock.receive_all(hdr.data(), hdr.size()).has_value()) return std::nullopt;
    auto parsed = Protocol::parse_header(hdr);
    if (!parsed || parsed->payload_length > MAX_NETWORK_PAYLOAD) return std::nullopt;
    std::vector<uint8_t> wire(Protocol::HEADER_SIZE + parsed->payload_length);
    std::memcpy(wire.data(), hdr.data(), Protocol::HEADER_SIZE);
    if (parsed->payload_length > 0 &&
        !sock.receive_all(wire.data() + Protocol::HEADER_SIZE,
                          parsed->payload_length).has_value()) {
        return std::nullopt;
    }
    return session.open(wire.data(), wire.size());
}

// Send one CLIENT_SUBMIT (struct + encoded file bytes) and drain frames until
// the terminal MSG_CLIENT_RESPONSE. Returns the parsed response + assembled
// result payload, or nullopt on transport/protocol failure.
auto submit_and_await(TCPSocket& sock, EncryptedSession& session,
                      const std::vector<uint8_t>& png_bytes, const char* model_name,
                      uint32_t tile_size = 64, const char* input_format = "png")
    -> std::optional<std::pair<ClientTaskResponse, std::vector<uint8_t>>> {
    ClientTaskSubmit task = make_task(png_bytes, model_name, input_format, tile_size);

    std::vector<uint8_t> payload(sizeof(task) + png_bytes.size());
    std::memcpy(payload.data(), &task, sizeof(task));
    std::memcpy(payload.data() + sizeof(task), png_bytes.data(), png_bytes.size());
    if (!send_sealed(sock, session, MessageType::MSG_CLIENT_SUBMIT,
                     payload.data(), payload.size())) {
        return std::nullopt;
    }

    std::vector<uint8_t> result;
    bool have_response = false;
    size_t expected_total = 0;
    while (true) {
        auto frame = recv_sealed(sock, session);
        if (!frame) return std::nullopt;
        if (frame->first.type == MessageType::MSG_TASK_PROGRESS) {
            continue;  // streamed progress; not asserted here
        }
        if (frame->first.type == MessageType::MSG_CLIENT_DATA) {
            if (!have_response) return std::nullopt;
            result.insert(result.end(), frame->second.begin(), frame->second.end());
            if (result.size() >= expected_total) break;
            continue;
        }
        if (frame->first.type != MessageType::MSG_CLIENT_RESPONSE) return std::nullopt;
        have_response = true;
        result = std::move(frame->second);
        if (result.size() < sizeof(ClientTaskResponse)) return std::nullopt;
        const auto* hdr = reinterpret_cast<const ClientTaskResponse*>(result.data());
        expected_total = sizeof(ClientTaskResponse) + hdr->result_data_length;
        if (result.size() >= expected_total) break;
    }
    ClientTaskResponse resp{};
    std::memcpy(&resp, result.data(), sizeof(resp));
    result.erase(result.begin(), result.begin() + sizeof(resp));
    return std::make_pair(resp, std::move(result));
}

// Drain frames after a submit until the terminal MSG_CLIENT_RESPONSE (+
// CLIENT_DATA continuations) assembles the full result. `on_progress` runs
// for every streamed progress frame — the cancel tests use it to fire
// MSG_TASK_CANCEL mid-task. Returns like submit_and_await.
auto drain_until_response(
    TCPSocket& sock, EncryptedSession& session,
    const std::function<void(const TaskProgress&)>& on_progress)
    -> std::optional<std::pair<ClientTaskResponse, std::vector<uint8_t>>> {
    std::vector<uint8_t> result;
    bool have_response = false;
    size_t expected_total = 0;
    while (true) {
        auto frame = recv_sealed(sock, session);
        if (!frame) return std::nullopt;
        if (frame->first.type == MessageType::MSG_TASK_PROGRESS) {
            if (frame->second.size() >= sizeof(TaskProgress) && on_progress) {
                TaskProgress prog{};
                std::memcpy(&prog, frame->second.data(), sizeof(prog));
                on_progress(prog);
            }
            continue;
        }
        if (frame->first.type == MessageType::MSG_CLIENT_DATA) {
            if (!have_response) return std::nullopt;
            result.insert(result.end(), frame->second.begin(), frame->second.end());
            if (result.size() >= expected_total) break;
            continue;
        }
        if (frame->first.type != MessageType::MSG_CLIENT_RESPONSE) return std::nullopt;
        have_response = true;
        result = std::move(frame->second);
        if (result.size() < sizeof(ClientTaskResponse)) return std::nullopt;
        const auto* hdr = reinterpret_cast<const ClientTaskResponse*>(result.data());
        expected_total = sizeof(ClientTaskResponse) + hdr->result_data_length;
        if (result.size() >= expected_total) break;
    }
    ClientTaskResponse resp{};
    std::memcpy(&resp, result.data(), sizeof(resp));
    result.erase(result.begin(), result.begin() + sizeof(resp));
    return std::make_pair(resp, std::move(result));
}

// Deterministic noise image (shared by the big-input tests). Seeded, so the
// JPEG bytes — and therefore the inference inputs — are identical on every
// run and across machines.
auto make_noise_jpeg(int width, int height, int jpeg_quality)
    -> std::vector<uint8_t> {
    std::mt19937 rng(0x53534358U);
    Image img(width, height, 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* px = img.data() + (static_cast<size_t>(y) * width + x) * 3;
            px[0] = static_cast<uint8_t>(rng());
            px[1] = static_cast<uint8_t>(rng());
            px[2] = static_cast<uint8_t>(rng());
        }
    }
    auto saved = img.save_to_memory("jpg", jpeg_quality, "", {});
    CHECK(saved.has_value());
    return std::move(*saved);
}

// Equivalence of two encoded outputs: identical pixels (via full decode) and
// identical encoded size. Raw byte-equality is not usable across submits —
// each output embeds the SR-marker XMP cs:ts wall-clock timestamp, so two
// submits a second apart differ in those bytes alone.
auto outputs_equivalent(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
    -> bool {
    if (a.size() != b.size()) {
        return false;
    }
    if (a == b) {
        return true;  // fast path (same second or non-image formats)
    }
    auto da = Image::load_from_memory("jpg", a);
    if (!da.has_value()) {
        da = Image::load_from_memory("png", a);
    }
    auto db = Image::load_from_memory("jpg", b);
    if (!db.has_value()) {
        db = Image::load_from_memory("png", b);
    }
    if (!da.has_value() || !db.has_value()) {
        return false;
    }
    if (da->width() != db->width() || da->height() != db->height() ||
        da->channels() != db->channels() || da->size() != db->size()) {
        return false;
    }
    return std::memcmp(da->data(), db->data(), da->size()) == 0;
}

// New-format chunked submit: struct-only CLIENT_SUBMIT frame followed by the
// input bytes as MSG_CLIENT_DATA continuation frames (mirrors the phone's
// send_distributed_submit for >4MB inputs). `chunk_bytes` is adjustable so
// the test can force multi-chunk reassembly on small inputs.
auto submit_chunked_and_await(
    TCPSocket& sock, EncryptedSession& session,
    const std::vector<uint8_t>& image_bytes, const char* model_name,
    const char* input_format, uint32_t tile_size,
    size_t chunk_bytes = CLIENT_DATA_CHUNK_BYTES)
    -> std::optional<std::pair<ClientTaskResponse, std::vector<uint8_t>>> {
    ClientTaskSubmit task = make_task(image_bytes, model_name, input_format, tile_size);
    if (!send_sealed(sock, session, MessageType::MSG_CLIENT_SUBMIT,
                     &task, sizeof(task))) {
        return std::nullopt;
    }
    for (size_t off = 0; off < image_bytes.size(); off += chunk_bytes) {
        const size_t n = std::min(chunk_bytes, image_bytes.size() - off);
        if (!send_sealed(sock, session, MessageType::MSG_CLIENT_DATA,
                         image_bytes.data() + off, n)) {
            return std::nullopt;
        }
    }
    return drain_until_response(sock, session, nullptr);
}

// ── Peak-RSS probes (/proc/self/status VmHWM) for the memory regression
//    check of chunked vs single-frame submits. clear_refs(5) resets the
//    high-water mark to the current RSS so each path gets an independent
//    measurement within one process.
auto peak_rss_kb() -> long {
    std::ifstream status("/proc/self/status");
    std::string key;
    long value = 0;
    std::string unit;
    while (status >> key) {
        if (key == "VmHWM:") {
            status >> value >> unit;
            return value;
        }
        status.ignore(4096, '\n');
    }
    return -1;
}

auto reset_peak_rss() -> bool {
    std::ofstream clear_refs("/proc/self/clear_refs", std::ios::trunc);
    clear_refs << "5\n";
    return clear_refs.good();
}

// Connect + handshake + pairing gate on a fresh socket (several tests open a
// second connection to the same coordinator).
auto connect_and_pair(TCPSocket& sock, uint16_t port,
                      const IdentityKey& server_identity,
                      const IdentityKey& client_identity)
    -> std::optional<EncryptedSession> {
    if (!sock.connect_with_timeout("127.0.0.1", port, 5000)) {
        return std::nullopt;
    }
    sock.set_timeout(120000);
    auto session = client_handshake(sock, server_identity, client_identity);
    if (!session.has_value()) {
        return std::nullopt;
    }
    ClientConnectRequest creq{};
    std::strncpy(creq.device_name, "loopback-test", sizeof(creq.device_name) - 1);
    const auto fp = fingerprint(client_identity.public_key());
    std::strncpy(creq.fingerprint, fp.c_str(), sizeof(creq.fingerprint) - 1);
    creq.fingerprint[sizeof(creq.fingerprint) - 1] = '\0';
    if (!send_sealed(sock, *session, MessageType::MSG_CLIENT_CONNECT_REQUEST,
                     &creq, sizeof(creq))) {
        return std::nullopt;
    }
    auto frame = recv_sealed(sock, *session);
    if (!frame || frame->first.type != MessageType::MSG_CLIENT_CONNECT_RESPONSE ||
        frame->second.size() < sizeof(ClientConnectResponse)) {
        return std::nullopt;
    }
    const auto* cresp = reinterpret_cast<const ClientConnectResponse*>(
        frame->second.data());
    if (cresp->approved != 1) {
        return std::nullopt;
    }
    if (cresp->pipeline_depth != 2) {
        return std::nullopt;
    }
    return session;
}

}  // namespace

int main() {
    sodium::init();

    // ── Temp workspace: identities + pre-seeded TrustStore (auto-approve). ──
    const auto dir = std::filesystem::temp_directory_path() /
        ("upix-test-session-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto server_id_path = dir / "server_identity";
    const auto client_id_path = dir / "client_identity";
    const auto trust_path = dir / "trusted_clients";
    const auto models_dir = dir / "models";
    std::filesystem::create_directories(models_dir);

    auto server_identity = IdentityKey::load_or_create(server_id_path);
    auto client_identity = IdentityKey::load_or_create(client_id_path);
    {
        auto store = TrustStore::load(trust_path);
        store.add(client_identity.public_key());
        store.save();
    }

    // ── Coordinator on a free port (empty models dir → per-task failures,
    //    which is exactly the path that must keep the session alive). ────────
    Coordinator coordinator;
    coordinator.set_trust_store_path(trust_path);
    coordinator.set_models_directory(models_dir);
    uint16_t port = 0;
    for (uint32_t candidate = 39527; candidate <= 39546; ++candidate) {
        if (coordinator.start(static_cast<uint16_t>(candidate), server_identity).has_value()) {
            port = static_cast<uint16_t>(candidate);
            break;
        }
    }
    CHECK(port != 0);

    // ── Connect, handshake, pairing gate. ───────────────────────────────────
    TCPSocket sock;
    CHECK(sock.connect_with_timeout("127.0.0.1", port, 5000));
    sock.set_timeout(30000);
    auto session_opt = client_handshake(sock, server_identity, client_identity);
    CHECK(session_opt.has_value());
    auto& session = *session_opt;

    {
        ClientConnectRequest creq{};
        std::strncpy(creq.device_name, "loopback-test", sizeof(creq.device_name) - 1);
        const auto fp = fingerprint(client_identity.public_key());
        std::strncpy(creq.fingerprint, fp.c_str(), sizeof(creq.fingerprint));
        creq.fingerprint[sizeof(creq.fingerprint) - 1] = '\0';
        CHECK(send_sealed(sock, session, MessageType::MSG_CLIENT_CONNECT_REQUEST,
                          &creq, sizeof(creq)));
        auto frame = recv_sealed(sock, session);
        CHECK(frame.has_value());
        CHECK(frame->first.type == MessageType::MSG_CLIENT_CONNECT_RESPONSE);
        CHECK(frame->second.size() >= sizeof(ClientConnectResponse));
        const auto* cresp = reinterpret_cast<const ClientConnectResponse*>(
            frame->second.data());
        CHECK(cresp->approved == 1);
        CHECK(cresp->trusted == 1);
        // This PC advertises submit-ahead: the phone may stage task N+1
        // (struct + DATA chunks) while task N still computes.
        CHECK(cresp->pipeline_depth == 2);
    }

    // ── A tiny valid PNG as input. ──────────────────────────────────────────
    std::vector<uint8_t> png_bytes;
    {
        Image img(64, 48, 3);
        for (int y = 0; y < 48; ++y) {
            for (int x = 0; x < 64; ++x) {
                uint8_t* px = img.data() + (static_cast<size_t>(y) * 64 + x) * 3;
                px[0] = static_cast<uint8_t>(x * 4);
                px[1] = static_cast<uint8_t>(y * 5);
                px[2] = 0x80;
            }
        }
        auto saved = img.save_to_memory("png", 90, "", {});
        CHECK(saved.has_value());
        png_bytes = std::move(*saved);
        CHECK(png_bytes.size() > 100);
    }

    // ── Task #1 on this connection. ─────────────────────────────────────────
    auto first = submit_and_await(sock, session, png_bytes, "no-such-model");
    CHECK(first.has_value());
    CHECK(first->first.success == 0);  // engine/model not ready (empty models dir)
    CHECK(first->first.result_data_length == 0);
    CHECK(first->second.empty());
    std::printf("OK submit #1 answered on-session (failure: %s)\n",
                first->first.error_message);

    // Idle gap between submits (the phone writes output N + checkpoints here).
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ── Task #2 on the SAME connection — the actual regression guard. ───────
    auto second = submit_and_await(sock, session, png_bytes, "no-such-model");
    CHECK(second.has_value());
    CHECK(second->first.success == 0);
    CHECK(second->first.result_data_length == 0);
    std::printf("OK submit #2 answered on the same connection\n");

    // Identical failure text for identical requests.
    CHECK(std::strcmp(first->first.error_message, second->first.error_message) == 0);

    // ── Task #3 with a broken (undecodable) input: per-task error must also
    //    keep the session alive (decode failure path). ────────────────────────
    std::vector<uint8_t> garbage(1024, 0x5A);
    auto third = submit_and_await(sock, session, garbage, "no-such-model");
    CHECK(third.has_value());
    CHECK(third->first.success == 0);
    std::printf("OK undecodable input answered without killing the session\n");

    // ── Task #4 after the decode failure: session still usable. ─────────────
    auto fourth = submit_and_await(sock, session, png_bytes, "no-such-model");
    CHECK(fourth.has_value());
    CHECK(fourth->first.success == 0);
    std::printf("OK submit #4 after decode failure still answered\n");

    // ── Stray MSG_TASK_CANCEL with no task in flight: must be ignored, the
    //    session stays usable (a cancel can race the response on the wire —
    //    the PC already answered, the phone already moved on). ─────────────
    {
        const uint8_t empty = 0;
        CHECK(send_sealed(sock, session, MessageType::MSG_TASK_CANCEL,
                          &empty, 0));
        auto fifth = submit_and_await(sock, session, png_bytes, "no-such-model");
        CHECK(fifth.has_value());
        CHECK(fifth->first.success == 0);
        std::printf("OK stray CANCEL (no task running) ignored; session usable\n");
    }

    // ── Chunked submit (new format): struct-only frame + MSG_CLIENT_DATA
    //    continuations. Without a model the task fails per-task (engine not
    //    ready) — which is exactly the path that must keep the session alive. ──
    {
        ClientTaskSubmit task = make_task(png_bytes, "no-such-model", "png", 64);
        CHECK(send_sealed(sock, session, MessageType::MSG_CLIENT_SUBMIT,
                          &task, sizeof(task)));
        const size_t chunk = png_bytes.size() / 3 + 1;
        for (size_t off = 0; off < png_bytes.size(); off += chunk) {
            const size_t n = std::min(chunk, png_bytes.size() - off);
            CHECK(send_sealed(sock, session, MessageType::MSG_CLIENT_DATA,
                              png_bytes.data() + off, n));
        }
        auto chunked = drain_until_response(sock, session, nullptr);
        CHECK(chunked.has_value());
        CHECK(chunked->first.success == 0);
        CHECK(chunked->first.result_data_length == 0);
        std::printf("OK chunked submit answered on-session (failure: %s)\n",
                    chunked->first.error_message);

        // Mixed formats on one session: legacy single frame still works after
        // a chunked one.
        auto legacy_after = submit_and_await(sock, session, png_bytes, "no-such-model");
        CHECK(legacy_after.has_value());
        CHECK(legacy_after->first.success == 0);
        std::printf("OK legacy single-frame submit after a chunked one\n");
    }

    // ── Malformed chunked submit: struct promises N bytes but a non-DATA
    //    frame arrives instead. Must be a per-task error (session alive), so
    //    a version-mismatched phone can fall back to the legacy format. ────
    {
        ClientTaskSubmit task{};
        std::strncpy(task.model_name, "no-such-model", sizeof(task.model_name) - 1);
        std::strncpy(task.input_format, "png", sizeof(task.input_format) - 1);
        task.tile_size = 64;
        task.tile_overlap = 8;
        task.output_quality = 90;
        task.input_data_length = 4096;  // promise bytes we never send
        CHECK(send_sealed(sock, session, MessageType::MSG_CLIENT_SUBMIT,
                          &task, sizeof(task)));
        // A submit frame where the PC expects a DATA chunk.
        CHECK(send_sealed(sock, session, MessageType::MSG_CLIENT_SUBMIT,
                          &task, sizeof(task)));
        auto broken = drain_until_response(sock, session, nullptr);
        CHECK(broken.has_value());
        CHECK(broken->first.success == 0);
        const std::string err = broken->first.error_message;
        CHECK(err.find("interrupted") != std::string::npos ||
              err.find("CLIENT_DATA") != std::string::npos);
        std::printf("OK malformed chunked submit rejected per-task: %s\n", err.c_str());

        auto recovered = submit_and_await(sock, session, png_bytes, "no-such-model");
        CHECK(recovered.has_value());
        CHECK(recovered->first.success == 0);
        std::printf("OK session usable after a malformed chunked submit\n");
    }

    sock.close();
    coordinator.stop();

    // ── Optional real-model success path (needs UPIX_TEST_MODEL_DIR + _NAME):
    //    two identical submits on one session must both succeed and return
    //    byte-identical results (deterministic GPU pipeline regression guard).
    const char* model_dir_env = std::getenv("UPIX_TEST_MODEL_DIR");
    const char* model_name_env = std::getenv("UPIX_TEST_MODEL_NAME");
    if (model_dir_env != nullptr && model_name_env != nullptr &&
        *model_name_env != '\0' && std::filesystem::is_directory(model_dir_env)) {
        Coordinator real;
        real.set_trust_store_path(trust_path);
        real.set_models_directory(model_dir_env);
        uint16_t real_port = 0;
        for (uint32_t candidate = 39627; candidate <= 39646; ++candidate) {
            if (real.start(static_cast<uint16_t>(candidate), server_identity).has_value()) {
                real_port = static_cast<uint16_t>(candidate);
                break;
            }
        }
        CHECK(real_port != 0);

        TCPSocket rsock;
        CHECK(rsock.connect_with_timeout("127.0.0.1", real_port, 5000));
        // First submit pays engine init + model load; later iterations can be
        // long on big models — give the socket a generous budget.
        rsock.set_timeout(180000);
        auto rsession = client_handshake(rsock, server_identity, client_identity);
        CHECK(rsession.has_value());
        {
            ClientConnectRequest creq{};
            std::strncpy(creq.device_name, "loopback-test", sizeof(creq.device_name) - 1);
            const auto rfp = fingerprint(client_identity.public_key());
            std::strncpy(creq.fingerprint, rfp.c_str(), sizeof(creq.fingerprint));
            creq.fingerprint[sizeof(creq.fingerprint) - 1] = '\0';
            CHECK(send_sealed(rsock, *rsession, MessageType::MSG_CLIENT_CONNECT_REQUEST,
                              &creq, sizeof(creq)));
            auto frame = recv_sealed(rsock, *rsession);
            CHECK(frame.has_value());
            CHECK(frame->first.type == MessageType::MSG_CLIENT_CONNECT_RESPONSE);
            const auto* cresp = reinterpret_cast<const ClientConnectResponse*>(
                frame->second.data());
            CHECK(cresp->approved == 1);
            CHECK(cresp->pipeline_depth == 2);
        }

        auto real_first = submit_and_await(rsock, *rsession, png_bytes, model_name_env);
        CHECK(real_first.has_value());
        CHECK(real_first->first.success == 1);
        CHECK(real_first->second.size() > 1000);
        auto real_second = submit_and_await(rsock, *rsession, png_bytes, model_name_env);
        CHECK(real_second.has_value());
        CHECK(real_second->first.success == 1);
        // Byte-equality of two submits is NOT a sound invariant: every output
        // embeds the SR-marker XMP with a wall-clock cs:ts (second
        // granularity), so submits landing in different seconds differ in
        // exactly those bytes. Compare the decoded pixels (and the encoded
        // size) instead — deterministic for a deterministic pipeline.
        CHECK(outputs_equivalent(real_first->second, real_second->second));
        std::printf("OK real-model path (%s): two submits on one session, %zu-byte "
                    "pixel-identical results\n", model_name_env, real_first->second.size());

        // ── tile_size=0 (remote auto): the PC must resolve it itself. The
        //    PC log line "Auto-selected tile size ... for remote task" goes
        //    to this process's stdout (the coordinator runs in-process), so
        //    capture stdout around the submit and assert the log appeared.
        //    Two 0-submits must also be byte-identical: the probe cache
        //    guarantees the same tile is picked for the same model+buckets.
        const auto cap_path = dir / "tile0_capture.log";
        std::fflush(stdout);
        const int saved_stdout = ::dup(STDOUT_FILENO);
        CHECK(saved_stdout >= 0);
        CHECK(std::freopen(cap_path.c_str(), "w", stdout) != nullptr);
        auto auto_first = submit_and_await(rsock, *rsession, png_bytes, model_name_env, 0);
        std::fflush(stdout);
        ::dup2(saved_stdout, STDOUT_FILENO);
        ::close(saved_stdout);
        CHECK(auto_first.has_value());
        CHECK(auto_first->first.success == 1);
        CHECK(auto_first->second.size() > 1000);
        std::string captured;
        {
            std::ifstream in(cap_path);
            std::string line;
            while (std::getline(in, line)) {
                captured += line + '\n';
            }
        }
        CHECK(captured.find("Auto-selected tile size") != std::string::npos);
        CHECK(captured.find("for remote task") != std::string::npos);
        std::printf("OK tile_size=0 auto path: PC logged auto-selection\n");
        auto auto_second = submit_and_await(rsock, *rsession, png_bytes, model_name_env, 0);
        CHECK(auto_second.has_value());
        CHECK(auto_second->first.success == 1);
        CHECK(outputs_equivalent(auto_first->second, auto_second->second));
        std::printf("OK tile_size=0 auto path: two 0-submits pixel-identical "
                    "(probe cache picked the same tile)\n");

        // ── Protocol-level task cancel. Submit a multi-second task, shoot
        //    MSG_TASK_CANCEL at the first progress tick, and require:
        //      (a) a prompt success=0 / "canceled" answer (not a timeout),
        //      (b) the session stays usable afterwards. ────────────────────
        {
            const auto big = make_noise_jpeg(2600, 2000, 98);
            ClientTaskSubmit task = make_task(big, model_name_env, "jpg", 0);
            std::vector<uint8_t> payload(sizeof(task) + big.size());
            std::memcpy(payload.data(), &task, sizeof(task));
            std::memcpy(payload.data() + sizeof(task), big.data(), big.size());
            CHECK(send_sealed(rsock, *rsession, MessageType::MSG_CLIENT_SUBMIT,
                              payload.data(), payload.size()));

            bool cancel_sent = false;
            auto t_cancel = std::chrono::steady_clock::now();
            auto canceled = drain_until_response(
                rsock, *rsession,
                [&](const TaskProgress& p) {
                    if (!cancel_sent && p.stage == 0 && p.percent > 0) {
                        const uint8_t empty = 0;
                        CHECK(send_sealed(rsock, *rsession,
                                          MessageType::MSG_TASK_CANCEL,
                                          &empty, 0));
                        cancel_sent = true;
                        t_cancel = std::chrono::steady_clock::now();
                    }
                });
            CHECK(canceled.has_value());
            CHECK(cancel_sent);
            CHECK(canceled->first.success == 0);
            CHECK(std::string(canceled->first.error_message).find("canceled") !=
                  std::string::npos);
            const double cancel_s = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t_cancel).count();
            CHECK(cancel_s < 60.0);
            std::printf("OK MSG_TASK_CANCEL answered '%s' after %.2f s\n",
                        canceled->first.error_message, cancel_s);

            // Session reuse after the cancel: the very next submit must
            // still succeed on the same connection.
            auto after = submit_and_await(rsock, *rsession, png_bytes,
                                          model_name_env);
            CHECK(after.has_value());
            CHECK(after->first.success == 1);
            std::printf("OK session still serves submits after a cancel\n");
        }

        // ── Dead connection mid-task: the PC must ABORT the task (not run
        //    it to completion). Open a second connection, submit the same
        //    slow input, drop the socket at the first progress tick, and
        //    watch the coordinator log for the abort line. ────────────────
        {
            TCPSocket dsock;
            auto dsession = connect_and_pair(dsock, real_port, server_identity,
                                             client_identity);
            CHECK(dsession.has_value());

            const auto big = make_noise_jpeg(2600, 2000, 98);
            ClientTaskSubmit task = make_task(big, model_name_env, "jpg", 0);
            std::vector<uint8_t> payload(sizeof(task) + big.size());
            std::memcpy(payload.data(), &task, sizeof(task));
            std::memcpy(payload.data() + sizeof(task), big.data(), big.size());
            CHECK(send_sealed(dsock, *dsession, MessageType::MSG_CLIENT_SUBMIT,
                              payload.data(), payload.size()));

            // Wait for the first real progress tick, then pull the plug.
            bool dropped = false;
            while (!dropped) {
                auto frame = recv_sealed(dsock, *dsession);
                CHECK(frame.has_value());
                CHECK(frame->first.type == MessageType::MSG_TASK_PROGRESS);
                if (frame->second.size() >= sizeof(TaskProgress)) {
                    TaskProgress prog{};
                    std::memcpy(&prog, frame->second.data(), sizeof(prog));
                    if (prog.stage == 0 && prog.percent > 0) {
                        dsock.close();
                        dropped = true;
                    }
                }
            }
            CHECK(dropped);

            // The coordinator runs in-process; its log goes to stdout.
            // Redirect stdout to a file, then poll the file for BOTH lines:
            // the handler noticing the dead peer, and — the one that matters
            // for "PC stops computing" — the worker actually exiting
            // inference after the cancel.
            const auto cap_path = dir / "disconnect_capture.log";
            std::fflush(stdout);
            const int saved_stdout = ::dup(STDOUT_FILENO);
            CHECK(saved_stdout >= 0);
            CHECK(std::freopen(cap_path.c_str(), "w", stdout) != nullptr);
            bool aborted = false;
            auto t_drop = std::chrono::steady_clock::now();
            double abort_s = 0.0;
            for (int i = 0; i < 480 && !aborted; ++i) {  // ≤ 120 s
                std::fflush(stdout);
                std::ifstream in(cap_path);
                std::string line;
                while (std::getline(in, line)) {
                    if (line.find("inference stopped after cancel") !=
                        std::string::npos) {
                        aborted = true;
                        abort_s = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - t_drop).count();
                        break;
                    }
                }
                if (!aborted) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
            }
            std::fflush(stdout);
            ::dup2(saved_stdout, STDOUT_FILENO);
            ::close(saved_stdout);
            CHECK(aborted);
            std::printf("OK dead-connection mid-task: PC inference stopped after "
                        "%.2f s (handler noticed at first poll tick)\n", abort_s);

            // The surviving session must still work (engine freed, no
            // cross-connection cancel bleed).
            auto after = submit_and_await(rsock, *rsession, png_bytes,
                                          model_name_env);
            CHECK(after.has_value());
            CHECK(after->first.success == 1);
            std::printf("OK other session unaffected by the dropped one\n");
        }

        // ── Submit-ahead staging (pipeline_depth >= 1): while a multi-second
        //    task still computes, stream in the NEXT chunked submit — its
        //    struct arms the mid-task capture and every MSG_CLIENT_DATA
        //    continuation lands across later poll ticks — then drain both
        //    responses in order. Regression for "a DATA frame arriving
        //    mid-task used to count as a protocol violation and dropped the
        //    whole session". The staged output must also be pixel-equal to a
        //    serial resubmit of the same input. ─────────────────────────────
        {
            const auto slow = make_noise_jpeg(2600, 2000, 98);
            const auto pipelined = make_noise_jpeg(2400, 2200, 97);

            // Task A fully uploaded first — reassembly is eager, so its
            // worker is computing while task B travels.
            ClientTaskSubmit ta = make_task(slow, model_name_env, "jpg", 0);
            CHECK(send_sealed(rsock, *rsession,
                              MessageType::MSG_CLIENT_SUBMIT, &ta, sizeof(ta)));
            for (size_t off = 0; off < slow.size(); off += CLIENT_DATA_CHUNK_BYTES) {
                const size_t n =
                    std::min<size_t>(CLIENT_DATA_CHUNK_BYTES, slow.size() - off);
                CHECK(send_sealed(rsock, *rsession,
                                  MessageType::MSG_CLIENT_DATA,
                                  slow.data() + off, n));
            }

            // Task B streams in while A computes: exactly what a depth-2
            // phone produces.
            ClientTaskSubmit tb = make_task(pipelined, model_name_env, "jpg", 0);
            CHECK(send_sealed(rsock, *rsession,
                              MessageType::MSG_CLIENT_SUBMIT, &tb, sizeof(tb)));
            for (size_t off = 0; off < pipelined.size(); off += CLIENT_DATA_CHUNK_BYTES) {
                const size_t n =
                    std::min<size_t>(CLIENT_DATA_CHUNK_BYTES, pipelined.size() - off);
                CHECK(send_sealed(rsock, *rsession,
                                  MessageType::MSG_CLIENT_DATA,
                                  pipelined.data() + off, n));
            }

            auto resp_a = drain_until_response(rsock, *rsession, nullptr);
            CHECK(resp_a.has_value());
            CHECK(resp_a->first.success == 1);
            auto resp_b = drain_until_response(rsock, *rsession, nullptr);
            CHECK(resp_b.has_value());
            CHECK(resp_b->first.success == 1);

            auto serial_b = submit_chunked_and_await(
                rsock, *rsession, pipelined, model_name_env, "jpg", 0);
            CHECK(serial_b.has_value());
            CHECK(serial_b->first.success == 1);
            CHECK(outputs_equivalent(resp_b->second, serial_b->second));
            std::printf("OK submit-ahead staging: %.2f MB+%.2f MB pipelined, "
                        "responses in order, pixels identical to serial\n",
                        static_cast<double>(slow.size()) / (1024.0 * 1024.0),
                        static_cast<double>(pipelined.size()) / (1024.0 * 1024.0));
        }

        // ── Submit-ahead prefetch DECODE failure: the head staged entry now
        //    decodes on a helper thread while task A computes. A corrupted
        //    task B must surface exactly the inline-path per-task error in
        //    its own response (success=0), never kill the session, and leave
        //    a following submit working. ────────────────────────────────────
        {
            const auto slow = make_noise_jpeg(2600, 2000, 98);
            auto broken = make_noise_jpeg(2400, 2200, 97);
            // Kill the JPEG signature so the decoder rejects it deterministically.
            broken[0] = 0xDE;
            broken[1] = 0xAD;

            ClientTaskSubmit ta = make_task(slow, model_name_env, "jpg", 0);
            CHECK(send_sealed(rsock, *rsession,
                              MessageType::MSG_CLIENT_SUBMIT, &ta, sizeof(ta)));
            for (size_t off = 0; off < slow.size(); off += CLIENT_DATA_CHUNK_BYTES) {
                const size_t n =
                    std::min<size_t>(CLIENT_DATA_CHUNK_BYTES, slow.size() - off);
                CHECK(send_sealed(rsock, *rsession,
                                  MessageType::MSG_CLIENT_DATA,
                                  slow.data() + off, n));
            }

            ClientTaskSubmit tb = make_task(broken, model_name_env, "jpg", 0);
            CHECK(send_sealed(rsock, *rsession,
                              MessageType::MSG_CLIENT_SUBMIT, &tb, sizeof(tb)));
            for (size_t off = 0; off < broken.size(); off += CLIENT_DATA_CHUNK_BYTES) {
                const size_t n =
                    std::min<size_t>(CLIENT_DATA_CHUNK_BYTES, broken.size() - off);
                CHECK(send_sealed(rsock, *rsession,
                                  MessageType::MSG_CLIENT_DATA,
                                  broken.data() + off, n));
            }

            auto resp_a = drain_until_response(rsock, *rsession, nullptr);
            CHECK(resp_a.has_value());
            CHECK(resp_a->first.success == 1);
            auto resp_b = drain_until_response(rsock, *rsession, nullptr);
            CHECK(resp_b.has_value());
            CHECK(resp_b->first.success == 0);
            const std::string err(resp_b->first.error_message);
            CHECK(err.find("input decode failed") != std::string::npos);

            auto after = submit_chunked_and_await(
                rsock, *rsession, slow, model_name_env, "jpg", 0);
            CHECK(after.has_value());
            CHECK(after->first.success == 1);
            std::printf("OK submit-ahead prefetch failure: '%s' propagated "
                        "per-task, session survived\n",
                        err.c_str());
        }

        // ── Big input, chunked vs single-frame upload. The same noise JPEG
        //    (8-20MB) is submitted through both wire formats; the real model
        //    must produce equivalent outputs (identical decoded pixels), and
        //    the process peak RSS must be LOWER on the chunked path — that
        //    is the whole point of chunking: neither side ever materializes
        //    full plaintext+wire copies of the input simultaneously. ───────
        {
            const auto big = make_noise_jpeg(2600, 2000, 100);
            constexpr uint64_t MB = 1024ull * 1024ull;
            CHECK(big.size() >= 8 * MB);
            CHECK(big.size() <= 20 * MB);
            std::printf("big-input test: %.2f MB noise JPEG\n",
                        static_cast<double>(big.size()) / MB);

            const bool can_measure = reset_peak_rss();
            auto via_chunks = submit_chunked_and_await(
                rsock, *rsession, big, model_name_env, "jpg", 0);
            CHECK(via_chunks.has_value());
            CHECK(via_chunks->first.success == 1);
            const long peak_chunked = can_measure ? peak_rss_kb() : -1;

            if (can_measure) {
                CHECK(reset_peak_rss());
            }
            auto via_single = submit_and_await(rsock, *rsession, big,
                                               model_name_env, 0, "jpg");
            CHECK(via_single.has_value());
            CHECK(via_single->first.success == 1);
            const long peak_single = can_measure ? peak_rss_kb() : -1;

            CHECK(outputs_equivalent(via_chunks->second, via_single->second));
            std::printf("OK big input: chunked and single-frame outputs equivalent "
                        "(%zu vs %zu bytes)\n",
                        via_chunks->second.size(), via_single->second.size());

            if (can_measure && peak_chunked > 0 && peak_single > 0) {
                const double c = static_cast<double>(peak_chunked) / 1024.0;
                const double s = static_cast<double>(peak_single) / 1024.0;
                std::printf("peak RSS: chunked %.1f MB vs single-frame %.1f MB "
                            "(saved %.1f MB)\n", c, s, s - c);
                CHECK(peak_chunked < peak_single);
            } else {
                std::printf("peak RSS probe unavailable; skipping the "
                            "chunked<singe assertion\n");
            }
        }

        // ── EXIF orientation=6 (portrait JPEG): the PC must bake the
        //    rotation in after decode, so the same stored 64x48 frame comes
        //    back as width/height swapped relative to the EXIF-free JPEG of
        //    identical pixels. Asserted on the decoded output dimensions.
        auto exif_jpeg_dims = [&]() -> std::pair<int, int> {
            // 64x48 landscape stored, tagged orientation 6 (rotate 90° CW).
            std::vector<uint8_t> jpeg_bytes;
            {
                Image img(64, 48, 3);
                for (int y = 0; y < 48; ++y) {
                    for (int x = 0; x < 64; ++x) {
                        uint8_t* px = img.data() + (static_cast<size_t>(y) * 64 + x) * 3;
                        px[0] = static_cast<uint8_t>(x * 4);
                        px[1] = static_cast<uint8_t>(y * 5);
                        px[2] = 0x80;
                    }
                }
                auto saved = img.save_to_memory("jpg", 90);
                CHECK(saved.has_value());
                jpeg_bytes = std::move(*saved);
            }
            // APP1 "Exif\0\0" with an IFD0 holding only Orientation=6,
            // spliced after the SOI (same shape as test_exif's builder).
            const uint8_t app1[] = {
                0xFF, 0xE1, 0x00, 0x22,
                'E', 'x', 'i', 'f', 0, 0,
                'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,
                0x01, 0x00,
                0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,
                0x06, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00,
            };
            std::vector<uint8_t> tagged;
            tagged.reserve(jpeg_bytes.size() + sizeof(app1));
            const auto* jb = jpeg_bytes.data();
            tagged.insert(tagged.end(), jb, jb + 2);
            tagged.insert(tagged.end(), app1, app1 + sizeof(app1));
            tagged.insert(tagged.end(), jb + 2, jb + jpeg_bytes.size());

            auto out = submit_and_await(rsock, *rsession, tagged, model_name_env,
                                        64, "jpg");
            CHECK(out.has_value());
            CHECK(out->first.success == 1);
            auto decoded = Image::load_from_memory("jpg", out->second);
            CHECK(decoded.has_value());
            return {decoded->width(), decoded->height()};
        }();
        // Baseline: the same JPEG pixels without the EXIF tag.
        {
            Image img(64, 48, 3);
            for (int y = 0; y < 48; ++y) {
                for (int x = 0; x < 64; ++x) {
                    uint8_t* px = img.data() + (static_cast<size_t>(y) * 64 + x) * 3;
                    px[0] = static_cast<uint8_t>(x * 4);
                    px[1] = static_cast<uint8_t>(y * 5);
                    px[2] = 0x80;
                }
            }
            auto saved = img.save_to_memory("jpg", 90);
            CHECK(saved.has_value());
            auto out = submit_and_await(rsock, *rsession, *saved, model_name_env,
                                        64, "jpg");
            CHECK(out.has_value());
            CHECK(out->first.success == 1);
            auto decoded = Image::load_from_memory("jpg", out->second);
            CHECK(decoded.has_value());
            CHECK(decoded->width() > decoded->height());  // landscape base
            CHECK(exif_jpeg_dims.first == decoded->height());
            CHECK(exif_jpeg_dims.second == decoded->width());
            std::printf("OK orientation=6 remote path: output %dx%d vs baseline "
                        "%dx%d (width/height swapped)\n",
                        exif_jpeg_dims.first, exif_jpeg_dims.second,
                        decoded->width(), decoded->height());
        }

        rsock.close();
        real.stop();
    }

    std::filesystem::remove_all(dir);
    std::printf("ALL DISTRIBUTED SESSION TESTS PASSED\n");
    return 0;
}
