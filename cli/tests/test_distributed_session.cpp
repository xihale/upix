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
#include <optional>
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
                      const std::vector<uint8_t>& png_bytes, const char* model_name)
    -> std::optional<std::pair<ClientTaskResponse, std::vector<uint8_t>>> {
    ClientTaskSubmit task{};
    std::strncpy(task.model_name, model_name, sizeof(task.model_name) - 1);
    std::strncpy(task.input_format, "png", sizeof(task.input_format) - 1);
    std::strncpy(task.output_format, "png", sizeof(task.output_format) - 1);
    task.tile_size = 64;
    task.tile_overlap = 8;
    task.output_quality = 90;
    task.input_data_length = static_cast<uint32_t>(png_bytes.size());

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
    result.erase(result.begin(), result.begin() + sizeof(ClientTaskResponse));
    return std::make_pair(resp, std::move(result));
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
        }

        auto real_first = submit_and_await(rsock, *rsession, png_bytes, model_name_env);
        CHECK(real_first.has_value());
        CHECK(real_first->first.success == 1);
        CHECK(real_first->second.size() > 1000);
        auto real_second = submit_and_await(rsock, *rsession, png_bytes, model_name_env);
        CHECK(real_second.has_value());
        CHECK(real_second->first.success == 1);
        CHECK(real_first->second == real_second->second);
        std::printf("OK real-model path (%s): two submits on one session, %zu-byte "
                    "identical results\n", model_name_env, real_first->second.size());

        rsock.close();
        real.stop();
    }

    std::filesystem::remove_all(dir);
    std::printf("ALL DISTRIBUTED SESSION TESTS PASSED\n");
    return 0;
}
