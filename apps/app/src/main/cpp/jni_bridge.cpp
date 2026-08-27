#include <jni.h>
#include <string>
#include <android/log.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <vector>

#include "core/config.hpp"
#include "core/engine.hpp"
#include "core/distributed/identity.hpp"
#include "core/distributed/crypto_session.hpp"
#include "protocol.hpp"
#include "utils/crypto_sodium.hpp"
#include "utils/image.hpp"
#include "utils/image_avif.hpp"
#include "utils/network.hpp"
#include "utils/sr_marker.hpp"

#include "glaze/glaze.hpp"

#define LOG_TAG "Upix"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Security fix: Maximum allowed payload size (100 MB) to prevent malicious
// servers from causing excessive memory allocation via crafted payload_length values.
static constexpr uint32_t MAX_PAYLOAD_LENGTH = 100 * 1024 * 1024;

// Hard cap on the assembled distributed result (512 MB). expected_total is
// declared by the server; without a bound a malicious peer could promise an
// unbounded MSG_CLIENT_DATA stream and grow result_payload without limit.
static constexpr size_t MAX_TOTAL_RESULT_BYTES = 512 * 1024 * 1024;

// Bounded connect and I/O timeouts for all JNI network paths. The I/O timeout
// is per send/recv call (SO_RCVTIMEO/SO_SNDTIMEO semantics), so an active
// transfer keeps renewing it; only a fully stalled peer trips it.
constexpr int CONNECT_TIMEOUT_MS = 10 * 1000;
constexpr int IO_TIMEOUT_MS = 60 * 1000;

using namespace upix;

namespace {

// Per-thread JNI error slot: concurrent calls from different Java threads no
// longer clobber each other's message. Plain short-string assignments; does
// not throw for the literals used in the catch handlers below.
thread_local std::string g_jni_last_error;

void set_jni_last_error(const std::string& error_message) {
    g_jni_last_error = error_message;
}

auto get_jni_last_error() -> std::string {
    return g_jni_last_error;
}

// Peer capabilities learned from the most recent CONNECT_RESPONSE
// (perform_connect_request below). Cleared whenever a pairing attempt starts,
// so a stale PC's flags never outlive its connection attempt.
std::atomic<bool> g_peer_avif_decode{false};
std::atomic<bool> g_peer_avif_encode{false};
// Submit-ahead depth the PC advertised (CONNECT_RESPONSE.pipeline_depth;
// 0 = old PC without the field). The phone may keep at most this many tasks
// submitted-but-unreceived: upload task N+1 while task N still computes.
std::atomic<int> g_peer_pipeline_depth{0};

// Parses the Kotlin-side encoder settings JSON (field names mirror
// upix::EncoderOptions). Null/empty/malformed input falls back to defaults —
// advanced settings are best-effort, never fatal. Values are clamped/normalized
// the same way ProcessingConfig::validate() would.
auto parse_encoder_options(JNIEnv* env, jstring json) -> EncoderOptions {
    EncoderOptions opts;
    if (json == nullptr) {
        return opts;
    }
    const char* chars = env->GetStringUTFChars(json, nullptr);
    if (chars == nullptr) {
        return opts;
    }
    const std::string text(chars);
    env->ReleaseStringUTFChars(json, chars);
    if (!text.empty()) {
        // Reflect over EncoderOptions directly; unknown keys are ignored so
        // the Kotlin side can carry extra fields without breaking older
        // natives.
        (void)glz::read<glz::opts{.error_on_unknown_keys = false}>(opts, text);
    }
    opts.png_level = std::clamp(opts.png_level, -1, 9);
    if (opts.jpeg_subsampling != 422 && opts.jpeg_subsampling != 444) {
        opts.jpeg_subsampling = 420;
    }
    opts.webp_method = std::clamp(opts.webp_method, -1, 6);
    opts.avif_speed = std::clamp(opts.avif_speed, 0, 10);
    if (opts.avif_subsampling != 444) {
        opts.avif_subsampling = 420;
    }
    opts.avif_threads = std::clamp(opts.avif_threads, 0, 64);
    return opts;
}

// Process-wide long-term Ed25519 identity. Loaded once via nativeLoadIdentity;
// used by every worker/client connection to sign the ECDH handshake.
// Guarded by g_identity_mutex because nativeLoadIdentity may run while other
// threads are mid-call; readers take a cheap shared_ptr snapshot instead of
// holding the lock across network I/O. shared_ptr<const> because every read
// path only needs the const public_key()/sign() surface.
std::mutex g_identity_mutex;
std::shared_ptr<const upix::IdentityKey> g_identity;

auto identity_snapshot() -> std::shared_ptr<const upix::IdentityKey> {
    std::lock_guard<std::mutex> lock(g_identity_mutex);
    return g_identity;
}

// Client side of the 3-message ECDH+signature handshake. Mirrors
// Coordinator::run_handshake() in share/core/distributed/coordinator.cpp
// byte-for-byte. On success returns an activated EncryptedSession; on
// failure returns nullopt and sets error_msg.
auto run_client_handshake(
    upix::TCPSocket& socket,
    const std::array<uint8_t, 32>& peer_identity_pubkey,
    const upix::IdentityKey& local_identity,
    std::string& error_msg,
    bool tofu_mode = false,
    std::array<uint8_t, 32>* learned_peer_pub = nullptr
) -> std::optional<upix::EncryptedSession> {
    error_msg.clear();
    upix::sodium::init();

    // (1) Generate ephemeral X25519 keypair, send KEY_HELLO (plaintext).
    auto eph = upix::sodium::x25519_keygen();
    const auto c_eph_pub = eph.public_key;
    const auto c_id_pub = local_identity.public_key();

    upix::KeyHelloMessage c_hello{};
    std::memcpy(c_hello.eph_pub, c_eph_pub.data(), 32);
    std::memcpy(c_hello.id_pub, c_id_pub.data(), 32);
    std::memset(c_hello.sig, 0, 64);  // client's hello carries no signature

    {
        upix::MessageHeader hh{};
        hh.type = upix::MessageType::MSG_KEY_HELLO;
        hh.payload_length = sizeof(c_hello);
        auto wire = upix::Protocol::serialize(hh, &c_hello);
        if (!socket.send_all(wire.data(), wire.size()).has_value()) {
            error_msg = "KEY_HELLO send failed";
            return std::nullopt;
        }
    }

    // (2) Receive server KEY_HELLO with its signature.
    upix::KeyHelloMessage s_hello{};
    {
        std::vector<uint8_t> hdr_buf(upix::Protocol::HEADER_SIZE);
        if (!socket.receive_all(hdr_buf.data(), hdr_buf.size()).has_value()) {
            error_msg = "KEY_HELLO header recv failed";
            return std::nullopt;
        }
        auto parsed_hdr = upix::Protocol::parse_header(hdr_buf);
        if (!parsed_hdr || parsed_hdr->type != upix::MessageType::MSG_KEY_HELLO ||
            parsed_hdr->payload_length != sizeof(s_hello)) {
            error_msg = "unexpected message during handshake";
            return std::nullopt;
        }
        std::vector<uint8_t> payload(parsed_hdr->payload_length);
        if (!socket.receive_all(payload.data(), payload.size()).has_value()) {
            error_msg = "KEY_HELLO payload recv failed";
            return std::nullopt;
        }
        std::memcpy(&s_hello, payload.data(), sizeof(s_hello));
    }

    // (3) Build transcript c_eph||c_id||s_eph||s_id (128 bytes). Both sides
    // MUST construct this identical byte sequence.
    std::array<uint8_t, 128> transcript{};
    std::memcpy(transcript.data() + 0,  c_hello.eph_pub, 32);
    std::memcpy(transcript.data() + 32, c_hello.id_pub,  32);
    std::memcpy(transcript.data() + 64, s_hello.eph_pub, 32);
    std::memcpy(transcript.data() + 96, s_hello.id_pub,  32);

    // (4) MITM check: server's identity pubkey must match the one we expect.
    //     In TOFU mode (manual connect by IP+port, no prior key), we skip the
    //     equality check and instead learn the server's identity for the caller
    //     to surface as a fingerprint for user verification.
    std::array<uint8_t, 32> s_id_pub_arr{};
    std::memcpy(s_id_pub_arr.data(), s_hello.id_pub, 32);
    if (tofu_mode) {
        if (learned_peer_pub != nullptr) {
            *learned_peer_pub = s_id_pub_arr;
        }
        LOGI("TOFU: learned server identity, fingerprint=%s",
             upix::fingerprint(s_id_pub_arr).c_str());
    } else if (s_id_pub_arr != peer_identity_pubkey) {
        error_msg = "server identity mismatch (possible MITM)";
        return std::nullopt;
    }

    // (5) Verify server signature over the transcript.
    std::array<uint8_t, 64> s_sig_arr{};
    std::memcpy(s_sig_arr.data(), s_hello.sig, 64);
    if (!upix::sodium::ed25519_verify(s_id_pub_arr, s_sig_arr,
                                            transcript.data(), transcript.size())) {
        error_msg = "server identity signature invalid";
        return std::nullopt;
    }

    // (6) Derive session key and activate EncryptedSession.
    std::array<uint8_t, 32> s_eph_pub_arr{};
    std::memcpy(s_eph_pub_arr.data(), s_hello.eph_pub, 32);
    auto session_key = upix::sodium::x25519_shared(eph.private_key, s_eph_pub_arr);

    upix::EncryptedSession session;
    session.activate(session_key);

    // (7) Sign the same transcript with our identity, send AUTH_PROOF (encrypted).
    auto c_sig = local_identity.sign(transcript.data(), transcript.size());
    upix::AuthProofMessage proof{};
    std::memcpy(proof.sig, c_sig.data(), 64);
    {
        upix::MessageHeader ph{};
        ph.type = upix::MessageType::MSG_AUTH_PROOF;
        ph.payload_length = sizeof(proof);
        auto pwire = session.seal(ph, &proof);
        if (!socket.send_all(pwire.data(), pwire.size()).has_value()) {
            error_msg = "AUTH_PROOF send failed";
            return std::nullopt;
        }
    }

    // (8) Receive AUTH_RESPONSE (encrypted).
    {
        std::vector<uint8_t> hdr_buf(upix::Protocol::HEADER_SIZE);
        if (!socket.receive_all(hdr_buf.data(), hdr_buf.size()).has_value()) {
            error_msg = "AUTH_RESPONSE header recv failed";
            return std::nullopt;
        }
        auto parsed = upix::Protocol::parse_header(hdr_buf);
        if (!parsed) {
            error_msg = "AUTH_RESPONSE header invalid";
            return std::nullopt;
        }
        if (parsed->payload_length > MAX_PAYLOAD_LENGTH) {
            error_msg = "AUTH_RESPONSE payload too large";
            return std::nullopt;
        }
        std::vector<uint8_t> wire(upix::Protocol::HEADER_SIZE + parsed->payload_length);
        std::memcpy(wire.data(), hdr_buf.data(), upix::Protocol::HEADER_SIZE);
        if (parsed->payload_length > 0) {
            if (!socket.receive_all(wire.data() + upix::Protocol::HEADER_SIZE,
                                    parsed->payload_length).has_value()) {
                error_msg = "AUTH_RESPONSE payload recv failed";
                return std::nullopt;
            }
        }
        auto opened = session.open(wire.data(), wire.size());
        if (!opened) {
            error_msg = "AUTH_RESPONSE MAC failed";
            return std::nullopt;
        }
        auto& [resp_hdr, resp_payload] = *opened;
        if (resp_hdr.type != upix::MessageType::MSG_AUTH_RESPONSE ||
            resp_payload.size() < sizeof(upix::AuthResponse)) {
            error_msg = "authentication rejected by server";
            return std::nullopt;
        }
        const auto* auth_resp = reinterpret_cast<const upix::AuthResponse*>(resp_payload.data());
        if (auth_resp->authenticated == 0) {
            error_msg = "authentication rejected by server";
            return std::nullopt;
        }
    }

    return session;
}

// ── 新连接配对流程: CONNECT_REQUEST/RESPONSE(握手后立即调用) ──────────────────
// PC 协调器在 ECDH 握手成功后读取下一个加密帧并预期它是 CONNECT_REQUEST。
// 可信 peer 自动批准, 首次 peer 阻塞在 PC 侧 stdin [y/n] 提示。返回 true 表示
// approved=1。失败返回 false 并设置 error_msg(approved=0 / 网络 / MAC / 协议错误)。
auto perform_connect_request(
    upix::TCPSocket& socket,
    upix::EncryptedSession& session,
    const std::string& device_name,
    const std::string& own_fingerprint,
    std::string& error_msg
) -> bool {
    error_msg.clear();
    g_peer_avif_decode.store(false);
    g_peer_avif_encode.store(false);
    g_peer_pipeline_depth.store(0);

    // (1) 构造 ClientConnectRequest: 设备名 + 自己的指纹。
    upix::ClientConnectRequest req{};
    std::memset(req.device_name, 0, sizeof(req.device_name));
    std::strncpy(req.device_name, device_name.c_str(), sizeof(req.device_name) - 1);
    std::memset(req.fingerprint, 0, sizeof(req.fingerprint));
    std::strncpy(req.fingerprint, own_fingerprint.c_str(), sizeof(req.fingerprint) - 1);

    upix::MessageHeader req_hdr{};
    req_hdr.type = upix::MessageType::MSG_CLIENT_CONNECT_REQUEST;
    req_hdr.payload_length = sizeof(req);
    auto req_wire = session.seal(req_hdr, &req);
    if (!socket.send_all(req_wire.data(), req_wire.size()).has_value()) {
        error_msg = "CONNECT_REQUEST send failed";
        return false;
    }

    // (2) 接收加密的 CONNECT_RESPONSE 并解析。
    std::vector<uint8_t> hdr_buf(upix::Protocol::HEADER_SIZE);
    if (!socket.receive_all(hdr_buf.data(), hdr_buf.size()).has_value()) {
        error_msg = "CONNECT_RESPONSE header recv failed";
        return false;
    }
    auto parsed = upix::Protocol::parse_header(hdr_buf);
    if (!parsed) {
        error_msg = "CONNECT_RESPONSE header invalid";
        return false;
    }
    if (parsed->payload_length > MAX_PAYLOAD_LENGTH) {
        error_msg = "CONNECT_RESPONSE payload too large";
        return false;
    }
    std::vector<uint8_t> wire(upix::Protocol::HEADER_SIZE + parsed->payload_length);
    std::memcpy(wire.data(), hdr_buf.data(), upix::Protocol::HEADER_SIZE);
    if (parsed->payload_length > 0) {
        if (!socket.receive_all(wire.data() + upix::Protocol::HEADER_SIZE,
                                parsed->payload_length).has_value()) {
            error_msg = "CONNECT_RESPONSE payload recv failed";
            return false;
        }
    }
    auto opened = session.open(wire.data(), wire.size());
    if (!opened) {
        error_msg = "CONNECT_RESPONSE MAC failed";
        return false;
    }
    auto& [resp_hdr, resp_payload] = *opened;
    // ClientConnectResponse only ever grows at the tail: accept anything from
    // the 4-byte legacy floor (approved/trusted/avif_*) upward. Older PCs
    // without pipeline_depth simply report depth 0 (= serial submits).
    constexpr size_t kLegacyConnectResponseSize = 4;
    if (resp_hdr.type != upix::MessageType::MSG_CLIENT_CONNECT_RESPONSE ||
        resp_payload.size() < kLegacyConnectResponseSize) {
        error_msg = "unexpected response during pairing";
        return false;
    }
    const auto* resp = reinterpret_cast<const upix::ClientConnectResponse*>(resp_payload.data());
    if (resp->approved == 0) {
        error_msg = "PC denied the connection";
        return false;
    }
    // Approved ⇒ this response carries the PC's codec/pipelining flags;
    // publish them for nativePeerCapabilities()/Kotlin gating. Bytes past the
    // legacy tail stay unread when the peer is old (payload shorter than the
    // current struct).
    g_peer_avif_decode.store(resp->avif_decode != 0);
    g_peer_avif_encode.store(resp->avif_encode != 0);
    if (resp_payload.size() >= sizeof(upix::ClientConnectResponse)) {
        g_peer_pipeline_depth.store(
            static_cast<int>(resp->pipeline_depth));
    }
    return true;
}

// Read + decrypt one sealed frame from the server. Used in a loop after submit
// to drain streamed MSG_TASK_PROGRESS frames until the terminal MSG_CLIENT_RESPONSE.
auto recv_encrypted_frame(
    upix::TCPSocket& socket,
    upix::EncryptedSession& session,
    std::string& error_msg
) -> std::optional<std::pair<upix::MessageHeader, std::vector<uint8_t>>> {
    error_msg.clear();
    std::vector<uint8_t> hdr_buf(upix::Protocol::HEADER_SIZE);
    if (!socket.receive_all(hdr_buf.data(), hdr_buf.size()).has_value()) {
        error_msg = "response header receive failed";
        return std::nullopt;
    }
    auto parsed = upix::Protocol::parse_header(hdr_buf);
    if (!parsed) {
        error_msg = "response header is invalid";
        return std::nullopt;
    }
    if (parsed->payload_length > MAX_PAYLOAD_LENGTH) {
        error_msg = "response payload too large: " +
                     std::to_string(parsed->payload_length) + " bytes";
        return std::nullopt;
    }
    std::vector<uint8_t> wire(upix::Protocol::HEADER_SIZE + parsed->payload_length);
    std::memcpy(wire.data(), hdr_buf.data(), upix::Protocol::HEADER_SIZE);
    if (parsed->payload_length > 0) {
        if (!socket.receive_all(wire.data() + upix::Protocol::HEADER_SIZE,
                                parsed->payload_length).has_value()) {
            error_msg = "response payload receive failed";
            return std::nullopt;
        }
    }
    auto opened = session.open(wire.data(), wire.size());
    if (!opened) {
        error_msg = "response MAC failed";
        return std::nullopt;
    }
    return std::move(*opened);
}

// Polled variant of recv_encrypted_frame for the distributed receive loop:
// waits at most poll_ms for the FIRST byte of a frame (timed_out=true, no
// bytes consumed), then finishes the frame with the full I/O timeout so a
// poll tick can never abandon a partially received frame while the PC is
// merely slow. Uses single-shot receive() per call so the socket's io lock
// is released between calls (room for the loop's own CANCEL send).
auto recv_encrypted_frame_poll(
    upix::TCPSocket& socket,
    upix::EncryptedSession& session,
    int poll_ms,
    std::string& error_msg,
    bool& timed_out
) -> std::optional<std::pair<upix::MessageHeader, std::vector<uint8_t>>> {
    error_msg.clear();
    timed_out = false;
    socket.set_recv_timeout(poll_ms);
    std::vector<uint8_t> hdr_buf(upix::Protocol::HEADER_SIZE);
    size_t got = 0;
    while (got < upix::Protocol::HEADER_SIZE) {
        auto chunk = socket.receive(hdr_buf.data() + got,
                                    upix::Protocol::HEADER_SIZE - got);
        if (!chunk.has_value()) {
            const int code = chunk.error().code;
            if (got == 0 && (code == EAGAIN || code == EWOULDBLOCK)) {
                timed_out = true;
                return std::nullopt;
            }
            error_msg = "response header receive failed";
            return std::nullopt;
        }
        got += chunk.value();
        if (got > 0) {
            // Bytes are in: finish this frame patiently, not on the poll
            // clock.
            socket.set_recv_timeout(IO_TIMEOUT_MS);
        }
    }
    auto parsed = upix::Protocol::parse_header(hdr_buf);
    if (!parsed) {
        error_msg = "response header is invalid";
        return std::nullopt;
    }
    if (parsed->payload_length > MAX_PAYLOAD_LENGTH) {
        error_msg = "response payload too large: " +
                    std::to_string(parsed->payload_length) + " bytes";
        return std::nullopt;
    }
    std::vector<uint8_t> wire(upix::Protocol::HEADER_SIZE + parsed->payload_length);
    std::memcpy(wire.data(), hdr_buf.data(), upix::Protocol::HEADER_SIZE);
    if (parsed->payload_length > 0) {
        if (!socket.receive_all(wire.data() + upix::Protocol::HEADER_SIZE,
                                parsed->payload_length).has_value()) {
            error_msg = "response payload receive failed";
            return std::nullopt;
        }
    }
    auto opened = session.open(wire.data(), wire.size());
    if (!opened) {
        error_msg = "response MAC failed";
        return std::nullopt;
    }
    return std::move(*opened);
}

}  // namespace

// ── Shared model library ops (phone↔PC) ─────────────────────────────────────
// Each op opens a fresh one-shot connection, exactly like image submit:
// TCP connect → ECDH handshake → CONNECT_REQUEST/RESPONSE → op frame → reply.
namespace {

struct PairedLink {
    upix::TCPSocket socket;
    upix::EncryptedSession session;
};

auto open_paired_link(
    const std::string& host,
    uint16_t port,
    const std::array<uint8_t, 32>& peer_pk,
    std::string& error_msg
) -> std::optional<PairedLink> {
    error_msg.clear();
    auto identity = identity_snapshot();
    if (!identity) {
        error_msg = "identity not loaded; call loadIdentity first";
        return std::nullopt;
    }

    PairedLink link;
    if (!link.socket.connect_with_timeout(host, port, CONNECT_TIMEOUT_MS)) {
        error_msg = "connect failed (timeout or unreachable): " + host + ":" +
                    std::to_string(port);
        return std::nullopt;
    }
    link.socket.set_timeout(IO_TIMEOUT_MS);

    std::string handshake_error;
    auto session = run_client_handshake(link.socket, peer_pk, *identity, handshake_error);
    if (!session.has_value()) {
        error_msg = "handshake failed: " + handshake_error;
        return std::nullopt;
    }

    const std::string own_fp = upix::fingerprint(identity->public_key());
    std::string pairing_error;
    if (!perform_connect_request(
            link.socket, *session, "Upix-Android", own_fp, pairing_error)) {
        error_msg = "pairing failed: " + pairing_error;
        return std::nullopt;
    }

    link.session = std::move(*session);
    return link;
}

auto send_sealed(
    PairedLink& link,
    upix::MessageType type,
    const void* payload,
    size_t len,
    std::string& error_msg
) -> bool {
    error_msg.clear();
    upix::MessageHeader header{};
    header.type = type;
    header.payload_length = static_cast<uint32_t>(len);
    try {
        auto wire = link.session.seal(header, payload);
        if (!link.socket.send_all(wire.data(), wire.size()).has_value()) {
            error_msg = "frame send failed";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        error_msg = std::string("frame seal failed: ") + e.what();
        return false;
    }
}

// Parse "host:port" style params shared by the three model-library JNI calls.
// Returns false and sets jni last error on failure.
auto resolve_endpoint_params(
    JNIEnv* env,
    jstring host,
    jint port,
    jstring peer_pubkey_b64,
    std::string& host_out,
    uint16_t& port_out,
    std::array<uint8_t, 32>& peer_pk_out
) -> bool {
    if (host == nullptr) {
        set_jni_last_error("model library op: host is null");
        return false;
    }
    if (port <= 0 || port > std::numeric_limits<uint16_t>::max()) {
        set_jni_last_error("model library op: port is invalid");
        return false;
    }
    const char* host_chars = env->GetStringUTFChars(host, nullptr);
    if (host_chars == nullptr) {
        set_jni_last_error("model library op: failed to read host string");
        return false;
    }
    host_out.assign(host_chars);
    env->ReleaseStringUTFChars(host, host_chars);
    port_out = static_cast<uint16_t>(port);

    std::string peer_b64;
    if (peer_pubkey_b64 != nullptr) {
        const char* chars = env->GetStringUTFChars(peer_pubkey_b64, nullptr);
        if (chars != nullptr) {
            peer_b64.assign(chars);
            env->ReleaseStringUTFChars(peer_pubkey_b64, chars);
        }
    }
    auto pk_bytes = upix::base64_decode(peer_b64);
    if (!pk_bytes || pk_bytes->size() != 32) {
        set_jni_last_error("model library op: invalid peer pubkey (expected 32-byte base64)");
        return false;
    }
    std::memcpy(peer_pk_out.data(), pk_bytes->data(), 32);
    return true;
}

}  // namespace

// ── Persistent distributed session (phone → PC) ─────────────────────────────
// One ECDH handshake + pairing per batch; every image then travels over the
// same TCP connection (the PC coordinator serves repeated CLIENT_SUBMITs in a
// session loop). Splitting send/receive lets the caller upload image N+1
// while the PC still processes N, and persist output N in between.
namespace {

struct DistributedSession {
    upix::TCPSocket socket;
    std::optional<upix::EncryptedSession> session;
    // Cross-thread task cancel. cancel_requested is set from ANY thread
    // (nativeCancelDistributedTask, e.g. the main thread when the user hits
    // cancel); the blocking receive loop polls it between frames and sends
    // the MSG_TASK_CANCEL frame ITSELF from the receiving thread. That keeps
    // the send on the socket's io owner-of-the-moment: the socket lock is
    // only held during individual receive calls (each bounded by the short
    // poll timeout), so there is always a gap for the loop's own send — a
    // naive "another thread sends while receive is blocked" design would
    // deadlock on StreamSocket's single recursive io_mutex.
    std::atomic<bool> cancel_requested{false};
    // Set once the CANCEL frame went out for the current task; the loop then
    // keeps draining until the PC's terminal "canceled" response (or a
    // bounded deadline) instead of treating poll timeouts as PC-gone.
    std::atomic<bool> cancel_sent{false};
};

// Receive-side poll cadence while a task is in flight: how often the blocked
// nativeReceiveDistributedResult re-checks cancel_requested. Also the upper
// bound for how long a cancel takes to reach the wire.
constexpr int RECEIVE_POLL_MS = 200;
// After MSG_TASK_CANCEL was sent, keep waiting this long for the terminal
// "canceled" response: the PC aborts at the next tile boundary, which for
// huge tiles / contended GPUs can be several seconds, but never minutes.
constexpr int CANCEL_DRAIN_TIMEOUT_MS = 90 * 1000;

// Sealed submit payload = ClientTaskSubmit struct + input + 24B nonce + 16B
// MAC on top of the raw file bytes; the PC rejects frames whose payload
// exceeds MAX_NETWORK_PAYLOAD. Keep the input under the wire budget so a
// near-100MB image cannot slip past this side and die as a silent server
// disconnect (the old one-shot path admitted it and then failed opaquely).
constexpr size_t MAX_SUBMIT_INPUT_BYTES =
    static_cast<size_t>(upix::MAX_NETWORK_PAYLOAD) - sizeof(upix::ClientTaskSubmit) - 64;

// Seal + send one CLIENT_SUBMIT. Two formats (see ClientTaskSubmit in
// protocol.hpp): inputs ≤ SINGLE_FRAME_SUBMIT_BYTES ride the legacy
// struct+bytes single frame (one RTT, no chunk overhead); larger inputs are
// sent as a struct-only frame followed by ≤8MB MSG_CLIENT_DATA chunks. The
// chunked path never materializes a whole plaintext or whole wire copy of
// the input — peak extra memory is one chunk (~8MB wire) instead of ~2x
// input size. `allow_chunked` = false forces the legacy format (the
// version-fallback resend against an old PC that rejected a chunked submit).
constexpr size_t SINGLE_FRAME_SUBMIT_BYTES = 4u * 1024u * 1024u;

auto send_distributed_submit(
    upix::TCPSocket& socket,
    upix::EncryptedSession& session,
    const std::string& model_name,
    int tile_size,
    int tile_overlap,
    const std::string& input_format,
    const uint8_t* input_data,
    size_t input_len,
    const std::string& output_format,
    int output_quality,
    const upix::EncoderOptions& encoder_opts,
    bool allow_chunked
) -> bool {
    if (input_len == 0 || input_len > MAX_SUBMIT_INPUT_BYTES) {
        set_jni_last_error("Distributed processing input size is invalid");
        return false;
    }
    upix::ClientTaskSubmit submit_request{};
    std::strncpy(submit_request.model_name, model_name.c_str(),
                 sizeof(submit_request.model_name) - 1);
    std::strncpy(submit_request.input_format, input_format.c_str(),
                 sizeof(submit_request.input_format) - 1);
    std::strncpy(submit_request.output_format, output_format.c_str(),
                 sizeof(submit_request.output_format) - 1);
    // tile_size 0 = remote auto (PC-side TileSizeOptimizer); every explicit
    // size is still clamped to the 16px protocol minimum.
    submit_request.tile_size = static_cast<uint32_t>(
        tile_size == 0 ? 0 : std::max(tile_size, 16));
    submit_request.tile_overlap = static_cast<uint32_t>(std::max(tile_overlap, 0));
    submit_request.output_quality = static_cast<uint32_t>(std::clamp(output_quality, 1, 100));
    submit_request.encoder = to_wire(encoder_opts);
    submit_request.input_data_length = static_cast<uint32_t>(input_len);

    const bool chunked = allow_chunked && input_len > SINGLE_FRAME_SUBMIT_BYTES;
    if (!chunked) {
        std::vector<uint8_t> plaintext(sizeof(submit_request) + input_len);
        std::memcpy(plaintext.data(), &submit_request, sizeof(submit_request));
        std::memcpy(plaintext.data() + sizeof(submit_request), input_data, input_len);

        upix::MessageHeader submit_header{};
        submit_header.type = upix::MessageType::MSG_CLIENT_SUBMIT;
        submit_header.payload_length = static_cast<uint32_t>(plaintext.size());
        auto submit_wire = session.seal(submit_header, plaintext.data());
        if (!socket.send_all(submit_wire.data(), submit_wire.size()).has_value()) {
            set_jni_last_error("Distributed submit send failed");
            return false;
        }
        return true;
    }

    // Chunked: struct-only frame, then ≤8MB DATA chunks sealed one at a
    // time. input_data stays the caller's buffer; only the current chunk's
    // wire copy exists at any moment.
    {
        upix::MessageHeader submit_header{};
        submit_header.type = upix::MessageType::MSG_CLIENT_SUBMIT;
        submit_header.payload_length = static_cast<uint32_t>(sizeof(submit_request));
        auto submit_wire = session.seal(submit_header, &submit_request);
        if (!socket.send_all(submit_wire.data(), submit_wire.size()).has_value()) {
            set_jni_last_error("Distributed submit send failed");
            return false;
        }
    }
    size_t offset = 0;
    while (offset < input_len) {
        const size_t n = std::min<size_t>(
            upix::CLIENT_DATA_CHUNK_BYTES, input_len - offset);
        upix::MessageHeader chunk_header{};
        chunk_header.type = upix::MessageType::MSG_CLIENT_DATA;
        chunk_header.payload_length = static_cast<uint32_t>(n);
        auto chunk_wire = session.seal(chunk_header, input_data + offset);
        if (!socket.send_all(chunk_wire.data(), chunk_wire.size()).has_value()) {
            set_jni_last_error("Distributed submit chunk send failed");
            return false;
        }
        offset += n;
    }
    return true;
}

// Drain MSG_TASK_PROGRESS frames (each forwarded to progress_cb) until the
// terminal MSG_CLIENT_RESPONSE (+ MSG_CLIENT_DATA continuations) assembles
// the full encoded result. Runs as a poll loop (RECEIVE_POLL_MS) instead of
// a blocking receive so a cross-thread nativeCancelDistributedTask is
// honored within one tick: the loop then sends MSG_TASK_CANCEL itself and
// keeps draining for the PC's "canceled" response. Returns the result bytes
// without the ClientTaskResponse header, or nullopt (thread-local error
// set). Cancel-related failures report "Distributed task canceled..." so the
// Kotlin layer can distinguish them from both task errors and PC-gone.
auto receive_distributed_result(
    DistributedSession& session_state,
    const std::function<void(uint32_t stage, uint32_t percent, uint32_t eta)>& progress_cb
) -> std::optional<std::vector<uint8_t>> {
    upix::TCPSocket& socket = session_state.socket;
    upix::EncryptedSession& session = *session_state.session;

    // New wait, new cancel state. Clearing here (instead of at submit time)
    // keeps a user cancel racing a concurrent submit-ahead upload from being
    // erased before this loop can observe it: sends no longer touch the
    // flags at all. A stale flag from an aborted batch cannot leak — cancel
    // always ends that batch with closeSession(), and a fresh session object
    // starts zeroed.
    session_state.cancel_requested.store(false);
    session_state.cancel_sent.store(false);

    // Leave the receive timeout in its normal (long) state on every exit.
    struct RecvTimeoutGuard {
        upix::TCPSocket& socket;
        ~RecvTimeoutGuard() { socket.set_recv_timeout(IO_TIMEOUT_MS); }
    } timeout_guard{socket};

    std::vector<uint8_t> result_payload;
    size_t expected_total = 0;
    bool have_response = false;
    auto cancel_deadline = std::chrono::steady_clock::time_point::max();
    while (true) {
        // Honor a cross-thread cancel request: send the CANCEL frame from
        // THIS thread (the only thread that can safely interleave a send
        // with the polled receive), then keep waiting for the terminal
        // response so the session is left in a clean state.
        if (session_state.cancel_requested.load() &&
            !session_state.cancel_sent.exchange(true)) {
            upix::MessageHeader cancel_hdr{};
            cancel_hdr.type = upix::MessageType::MSG_TASK_CANCEL;
            cancel_hdr.payload_length = 0;
            const uint8_t empty = 0;
            auto wire = session.seal(cancel_hdr, &empty);
            if (!socket.send_all(wire.data(), wire.size()).has_value()) {
                set_jni_last_error("Distributed task canceled (connection lost)");
                return std::nullopt;
            }
            LOGI("Sent MSG_TASK_CANCEL; waiting for the PC to abort the task");
            cancel_deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(CANCEL_DRAIN_TIMEOUT_MS);
        }
        std::string frame_error;
        bool timed_out = false;
        auto frame = recv_encrypted_frame_poll(
            socket, session, RECEIVE_POLL_MS, frame_error, timed_out);
        if (!frame) {
            if (timed_out) {
                if (std::chrono::steady_clock::now() >= cancel_deadline) {
                    set_jni_last_error(
                        "Distributed task canceled (no response from PC)");
                    return std::nullopt;
                }
                continue;  // ordinary poll tick / still draining after cancel
            }
            if (session_state.cancel_sent.load()) {
                // Old PC (no CANCEL support) dropped the connection, or the
                // link died right after we canceled: the user asked for a
                // cancel, not a PC failure.
                set_jni_last_error("Distributed task canceled (connection lost)");
                return std::nullopt;
            }
            set_jni_last_error("Distributed " + frame_error);
            return std::nullopt;
        }
        if (frame->first.type == upix::MessageType::MSG_TASK_PROGRESS &&
            frame->second.size() >= sizeof(upix::TaskProgress)) {
            upix::TaskProgress prog{};
            std::memcpy(&prog, frame->second.data(), sizeof(prog));
            if (progress_cb) {
                progress_cb(prog.stage, prog.percent, prog.eta_seconds);
            } else if (prog.stage == 0) {
                LOGI("PC progress: %u%%, ETA %us", prog.percent, prog.eta_seconds);
            }
            continue;  // progress update; drain and wait for the result
        }
        if (frame->first.type == upix::MessageType::MSG_CLIENT_DATA) {
            if (!have_response) {
                set_jni_last_error("Distributed result data arrived before the response header");
                return std::nullopt;
            }
            // One reserve per result: the total is declared by the response
            // header, so growing chunk-by-chunk with reallocation is waste.
            if (expected_total > result_payload.capacity()) {
                result_payload.reserve(expected_total);
            }
            result_payload.insert(result_payload.end(),
                                  frame->second.begin(), frame->second.end());
            if (result_payload.size() > MAX_TOTAL_RESULT_BYTES) {
                set_jni_last_error("Distributed result exceeds the total size limit");
                return std::nullopt;
            }
            if (result_payload.size() >= expected_total) {
                break;
            }
            continue;
        }
        if (frame->first.type != upix::MessageType::MSG_CLIENT_RESPONSE) {
            set_jni_last_error("Distributed response frame type is invalid");
            return std::nullopt;
        }
        have_response = true;
        result_payload = std::move(frame->second);
        if (result_payload.size() < sizeof(upix::ClientTaskResponse)) {
            set_jni_last_error("Distributed response payload is truncated");
            return std::nullopt;
        }
        const auto* hdr = reinterpret_cast<const upix::ClientTaskResponse*>(
            result_payload.data());
        expected_total = sizeof(upix::ClientTaskResponse) + hdr->result_data_length;
        if (expected_total > MAX_TOTAL_RESULT_BYTES) {
            set_jni_last_error("Distributed result exceeds the total size limit");
            return std::nullopt;
        }
        if (result_payload.size() >= expected_total) {
            break;  // single-frame result (the common case)
        }
    }
    const auto* task_response = reinterpret_cast<const upix::ClientTaskResponse*>(
        result_payload.data());
    if (task_response->success == 0) {
        set_jni_last_error("Distributed task failed: " +
                           std::string(task_response->error_message));
        return std::nullopt;
    }
    // result_data_length is only trusted as a byte count to wait for; the
    // truncation check validates it against bytes actually received, so no
    // allocation ever happens from a claimed length.
    const size_t resp_hdr_size = sizeof(upix::ClientTaskResponse);
    if (resp_hdr_size + task_response->result_data_length > result_payload.size()) {
        set_jni_last_error("Distributed result payload truncated");
        return std::nullopt;
    }
    if (task_response->result_data_length > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
        set_jni_last_error("Distributed result is too large for a Java byte array");
        return std::nullopt;
    }
    result_payload.erase(result_payload.begin(),
                         result_payload.begin() + static_cast<long>(resp_hdr_size));
    return result_payload;
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_io_upix_nativelib_NCNNEngine_nativeInit(
    JNIEnv* env,
    jobject thiz,
    jint device_type
) {
    try {
        (void)env;
        (void)thiz;
        auto engine = create_ncnn_engine();
        if (!engine) {
            set_jni_last_error("Failed to create engine");
            LOGE("Failed to create engine");
            return 0;
        }

        DeviceType preferred = (device_type == 1) ? DeviceType::GPU_VULKAN : DeviceType::CPU;
        auto result = engine->initialize(preferred);

        if (!result.has_value()) {
            set_jni_last_error("Failed to initialize engine: " + result.error().message);
            LOGE("Failed to initialize engine: %s", result.error().message.c_str());
            return 0;
        }

        if (preferred == DeviceType::GPU_VULKAN && engine->current_device() != DeviceType::GPU_VULKAN) {
            // GPU unavailable on this device — gracefully fall back to CPU instead of failing.
            LOGW("GPU unavailable, falling back to CPU for engine processing");
        }

        set_jni_last_error("");
        LOGI("Engine initialized with device type: %d", device_type);
        return reinterpret_cast<jlong>(engine.release());
    } catch (const std::exception& exception) {
        set_jni_last_error(std::string("nativeInit failed: ") + exception.what());
        return 0;
    } catch (...) {
        set_jni_last_error("nativeInit failed: unknown native error");
        return 0;
    }
}

JNIEXPORT jboolean JNICALL
Java_io_upix_nativelib_NCNNEngine_nativeLoadModel(
    JNIEnv* env,
    jobject thiz,
    jlong engine_ptr,
    jstring model_path
) {
    (void)thiz;
    try {
        if (engine_ptr == 0) {
            set_jni_last_error("nativeLoadModel called with null engine pointer");
            return JNI_FALSE;
        }

        auto* engine = reinterpret_cast<Engine*>(engine_ptr);
        const char* path = env->GetStringUTFChars(model_path, nullptr);
        if (path == nullptr) {
            set_jni_last_error("Failed to read model path string from Java");
            return JNI_FALSE;
        }

        std::string param_path = std::string(path) + ".param";
        std::string bin_path = std::string(path) + ".bin";

        LOGI("Loading model: %s", path);

        auto result = engine->load_model(param_path, bin_path);

        env->ReleaseStringUTFChars(model_path, path);

        if (!result.has_value()) {
            std::ostringstream stream;
            stream << "nativeLoadModel failed code=" << result.error().code
                   << " message=" << result.error().message;
            const std::string error_message = stream.str();
            set_jni_last_error(error_message);
            LOGE("%s", error_message.c_str());
            return JNI_FALSE;
        }

        set_jni_last_error("");

        return JNI_TRUE;
    } catch (const std::exception& exception) {
        set_jni_last_error(std::string("nativeLoadModel failed: ") + exception.what());
        return JNI_FALSE;
    } catch (...) {
        set_jni_last_error("nativeLoadModel failed: unknown native error");
        return JNI_FALSE;
    }
}

JNIEXPORT jstring JNICALL
Java_io_upix_nativelib_NCNNEngine_nativeGetLastError(
    JNIEnv* env,
    jobject thiz
) {
    (void)thiz;
    try {
        const std::string last_error = get_jni_last_error();
        return env->NewStringUTF(last_error.c_str());
    } catch (const std::exception& exception) {
        set_jni_last_error(std::string("nativeGetLastError failed: ") + exception.what());
    } catch (...) {
        set_jni_last_error("nativeGetLastError failed: unknown native error");
    }
    return nullptr;
}

// SR-marker probe for the planner: scans the encoded input bytes for this
// tool's own XMP marker. Returns "model|scale|ts" when found, null when the
// input is not one of our outputs (or is unreadable — callers treat that as
// "process normally").
JNIEXPORT jstring JNICALL
Java_io_upix_nativelib_NCNNEngine_nativeProbeSrMarker(
    JNIEnv* env,
    jobject thiz,
    jbyteArray encoded_image
) {
    (void)thiz;
    try {
        if (encoded_image == nullptr) return nullptr;
        const jsize size = env->GetArrayLength(encoded_image);
        if (size <= 0) return nullptr;
        jbyte* bytes = env->GetByteArrayElements(encoded_image, nullptr);
        if (bytes == nullptr) return nullptr;
        auto marker = sr_marker::probe(reinterpret_cast<const uint8_t*>(bytes),
                                       static_cast<size_t>(size));
        env->ReleaseByteArrayElements(encoded_image, bytes, JNI_ABORT);
        if (!marker.has_value()) return nullptr;
        return env->NewStringUTF(
            (marker->model + "|" + std::to_string(marker->scale) + "|" +
             std::to_string(marker->ts)).c_str());
    } catch (const std::exception& exception) {
        set_jni_last_error(std::string("nativeProbeSrMarker failed: ") + exception.what());
        return nullptr;
    } catch (...) {
        set_jni_last_error("nativeProbeSrMarker failed: unknown native error");
        return nullptr;
    }
}

JNIEXPORT jstring JNICALL
Java_io_upix_nativelib_NCNNEngine_nativeCapabilities(JNIEnv* env, jobject thiz) {
    (void)thiz;
    // Comma-separated build capabilities; the Kotlin layer uses these to gate
    // AVIF output/input availability (compile-time optional codecs, root
    // xmake.lua). Empty = baseline build (no AVIF encode, platform-only decode).
    std::string caps;
#if defined(UPIX_WITH_AVIF_ENCODER)
    caps += "avif-encode";
#endif
#if defined(UPIX_WITH_AVIF_DECODER)
    if (!caps.empty()) {
        caps += ",";
    }
    caps += "avif-decode";
#endif
    return env->NewStringUTF(caps.c_str());
}

// AVIF capabilities of the paired PC, learned from the latest successful
// pairing handshake (CONNECT_RESPONSE). Same comma-token format as
// nativeCapabilities(); empty = unknown / nothing learned yet.
JNIEXPORT jstring JNICALL
Java_io_upix_nativelib_NCNNEngine_nativePeerCapabilities(JNIEnv* env, jobject thiz) {
    (void)thiz;
    std::string caps;
    if (g_peer_avif_decode.load()) {
        caps += "avif-decode";
    }
    if (g_peer_avif_encode.load()) {
        if (!caps.empty()) {
            caps += ",";
        }
        caps += "avif-encode";
    }
    if (g_peer_pipeline_depth.load() >= 1) {
        if (!caps.empty()) {
            caps += ",";
        }
        // Depth 2 = one task computing + one more fully staged on the wire.
        caps += "pipeline-ahead";
    }
    if (g_peer_pipeline_depth.load() >= 2) {
        // The PC keeps TWO workers alive (task N computes while N+1 is fully
        // staged and its worker queued), so the engine may ALSO upload N+2
        // during N's result wait — that keeps the phone's post-receive
        // persist/commit work overlapped with PC inference of the next image.
        caps += ",pipeline-deep";
    }
    return env->NewStringUTF(caps.c_str());
}

// Decode an encoded image buffer into raw pixels via the native Image
// pipeline. out_meta[0..2] = width, height, channels; returns the pixel bytes.
// The Kotlin input path only routes here when the platform decoder failed
// (AVIF below API 31) — the native AVIF decoder is a compile-time option.
JNIEXPORT jbyteArray JNICALL
Java_io_upix_nativelib_NCNNEngine_nativeDecodeImage(
    JNIEnv* env,
    jobject thiz,
    jbyteArray encoded,
    jlongArray out_meta
) {
    (void)thiz;
    try {
        if (encoded == nullptr || out_meta == nullptr ||
            env->GetArrayLength(out_meta) < 3) {
            set_jni_last_error("nativeDecodeImage: invalid arguments");
            return nullptr;
        }
        const jsize size = env->GetArrayLength(encoded);
        if (size <= 0) {
            set_jni_last_error("nativeDecodeImage: empty input");
            return nullptr;
        }
        std::vector<uint8_t> buffer(static_cast<size_t>(size));
        env->GetByteArrayRegion(
            encoded, 0, size, reinterpret_cast<jbyte*>(buffer.data()));
        auto image = Image::load_from_memory("avif", buffer);
        if (!image) {
            set_jni_last_error("nativeDecodeImage: " + image.error().message);
            return nullptr;
        }
        const jlong meta[3] = {
            image->width(), image->height(), static_cast<jlong>(image->channels())};
        env->SetLongArrayRegion(out_meta, 0, 3, meta);
        const jsize pixel_bytes = static_cast<jsize>(image->size());
        jbyteArray pixels = env->NewByteArray(pixel_bytes);
        if (pixels == nullptr) {
            return nullptr;
        }
        env->SetByteArrayRegion(
            pixels, 0, pixel_bytes,
            reinterpret_cast<const jbyte*>(image->data()));
        return pixels;
    } catch (const std::exception& e) {
        set_jni_last_error(std::string("nativeDecodeImage failed: ") + e.what());
        return nullptr;
    } catch (...) {
        set_jni_last_error("nativeDecodeImage failed: unknown native error");
        return nullptr;
    }
}

JNIEXPORT jbyteArray JNICALL
Java_io_upix_nativelib_NCNNEngine_nativeProcessImage(
    JNIEnv* env,
    jobject thiz,
    jlong engine_ptr,
    jbyteArray input_image,
    jint width,
    jint height,
    jint channels,
    jstring output_format,
    jint output_quality,
    jstring encoder_options_json
) {
    (void)thiz;
    if (engine_ptr == 0 || input_image == nullptr) return nullptr;
    if (width <= 0 || height <= 0 || channels <= 0) {
        LOGE("Invalid input shape: width=%d, height=%d, channels=%d", width, height, channels);
        return nullptr;
    }
    
    auto* engine = reinterpret_cast<Engine*>(engine_ptr);

    ProcessingConfig output_config;
    output_config.output_quality = std::clamp(static_cast<int>(output_quality), 1, 100);
    output_config.output_format = "png";
    if (output_format != nullptr) {
        const char* format_chars = env->GetStringUTFChars(output_format, nullptr);
        if (format_chars != nullptr) {
            output_config.output_format = normalize_output_format(format_chars);
            env->ReleaseStringUTFChars(output_format, format_chars);
        }
    }
    if (output_config.output_format.empty()) {
        output_config.output_format = "png";
    }
    if (!is_supported_output_format(output_config.output_format)) {
        const std::string message = "Unsupported output format: " + output_config.output_format;
        LOGE("%s", message.c_str());
        set_jni_last_error(message);
        return nullptr;
    }
    
    const jsize input_size = env->GetArrayLength(input_image);
    const int64_t expected_input_size =
        static_cast<int64_t>(width) * static_cast<int64_t>(height) * static_cast<int64_t>(channels);
    if (expected_input_size <= 0 || expected_input_size > std::numeric_limits<jsize>::max()) {
        LOGE("Input dimensions overflow: width=%d, height=%d, channels=%d", width, height, channels);
        return nullptr;
    }
    if (input_size != static_cast<jsize>(expected_input_size)) {
        LOGE(
            "Input byte array size mismatch: actual=%d expected=%lld",
            input_size,
            static_cast<long long>(expected_input_size)
        );
        return nullptr;
    }

    jbyte* input_data = env->GetByteArrayElements(input_image, nullptr);
    if (input_data == nullptr) {
        LOGE("Failed to access input byte array elements");
        return nullptr;
    }

    const int scale_factor = std::max(engine->get_scale_factor(), 1);
    const int64_t output_width64 = static_cast<int64_t>(width) * static_cast<int64_t>(scale_factor);
    const int64_t output_height64 = static_cast<int64_t>(height) * static_cast<int64_t>(scale_factor);
    const int64_t output_size64 = output_width64 * output_height64 * static_cast<int64_t>(channels);
    if (output_width64 <= 0 || output_height64 <= 0 || output_size64 <= 0 ||
        output_width64 > std::numeric_limits<int>::max() ||
        output_height64 > std::numeric_limits<int>::max() ||
        output_size64 > std::numeric_limits<jsize>::max()) {
        LOGE(
            "Output dimensions overflow: output_width=%lld output_height=%lld channels=%d",
            static_cast<long long>(output_width64),
            static_cast<long long>(output_height64),
            channels
        );
        env->ReleaseByteArrayElements(input_image, input_data, JNI_ABORT);
        return nullptr;
    }

    const int output_width = static_cast<int>(output_width64);
    const int output_height = static_cast<int>(output_height64);
    const jsize output_size = static_cast<jsize>(output_size64);

    // Clear stale errors so a successful call leaves nativeGetLastError()
    // empty; every nullptr return below sets a specific message instead.
    set_jni_last_error("");

    try {
        std::vector<uint8_t> output_u8(static_cast<size_t>(output_size));

        auto result = engine->process_u8(
            reinterpret_cast<const uint8_t*>(input_data),
            output_u8.data(),
            width,
            height,
            channels
        );

        env->ReleaseByteArrayElements(input_image, input_data, JNI_ABORT);
        input_data = nullptr;

        if (!result.has_value()) {
            const std::string message =
                "Engine processing failed: " + result.error().message;
            LOGE("%s", message.c_str());
            set_jni_last_error(message);
            return nullptr;
        }

        Image output_image(output_width, output_height, channels);
        std::copy(output_u8.begin(), output_u8.end(), output_image.data());

        // Same SR marker the CLI writes, so phone-side outputs are
        // recognized on re-input (planner skips them by default).
        const auto sr_xmp = sr_marker::build_xmp(
            engine->model_name(), scale_factor,
            static_cast<int64_t>(time(nullptr)));
        const EncoderOptions encoder_opts =
            parse_encoder_options(env, encoder_options_json);
        auto encoded_result = output_image.save_to_memory(
            output_config.output_format,
            output_config.output_quality,
            sr_xmp,
            encoder_opts
        );
        if (!encoded_result.has_value()) {
            const std::string message =
                "Failed to encode output image: " + encoded_result.error().message;
            LOGE("%s", message.c_str());
            set_jni_last_error(message);
            return nullptr;
        }

        const auto& encoded = encoded_result.value();
        jbyteArray output = env->NewByteArray(static_cast<jsize>(encoded.size()));
        if (output == nullptr) {
            LOGE("Failed to allocate output byte array");
            set_jni_last_error("Failed to allocate output byte array");
            return nullptr;
        }
        env->SetByteArrayRegion(
            output,
            0,
            static_cast<jsize>(encoded.size()),
            reinterpret_cast<const jbyte*>(encoded.data())
        );
        return output;
    } catch (const std::bad_alloc&) {
        if (input_data != nullptr) {
            env->ReleaseByteArrayElements(input_image, input_data, JNI_ABORT);
        }
        LOGE(
            "Out of memory during native processing buffers: input=%dx%dx%d output=%dx%dx%d",
            width,
            height,
            channels,
            output_width,
            output_height,
            channels
        );
        set_jni_last_error(
            "Out of memory during native processing buffers: input=" +
            std::to_string(width) + "x" + std::to_string(height) + "x" +
            std::to_string(channels) + " output=" +
            std::to_string(output_width) + "x" + std::to_string(output_height) + "x" +
            std::to_string(channels)
        );
        return nullptr;
    } catch (const std::exception& exception) {
        if (input_data != nullptr) {
            env->ReleaseByteArrayElements(input_image, input_data, JNI_ABORT);
        }
        LOGE("Unexpected native processing exception: %s", exception.what());
        set_jni_last_error(std::string("Unexpected native processing exception: ") + exception.what());
        return nullptr;
    } catch (...) {
        if (input_data != nullptr) {
            env->ReleaseByteArrayElements(input_image, input_data, JNI_ABORT);
        }
        LOGE("Unexpected native processing exception: unknown error");
        set_jni_last_error("Unexpected native processing exception: unknown native error");
        return nullptr;
    }
}

// Open a persistent encrypted session to the PC: TCP connect + ECDH+Ed25519
// handshake + CONNECT_REQUEST/RESPONSE pairing, once per batch. Returns an
// opaque handle (0 on failure; details via nativeGetLastError).
JNIEXPORT jlong JNICALL
Java_io_upix_nativelib_NCNNEngine_nativeOpenDistributedSession(
    JNIEnv* env,
    jobject thiz,
    jstring host,
    jint port,
    jstring peer_pubkey_b64
) {
    (void)thiz;
    try {
        if (host == nullptr || peer_pubkey_b64 == nullptr ||
            port <= 0 || port > std::numeric_limits<uint16_t>::max()) {
            set_jni_last_error("Distributed session parameters are invalid");
            return 0;
        }

        const char* host_chars = env->GetStringUTFChars(host, nullptr);
        const char* peer_pk_chars = env->GetStringUTFChars(peer_pubkey_b64, nullptr);
        if (host_chars == nullptr || peer_pk_chars == nullptr) {
            if (host_chars != nullptr) {
                env->ReleaseStringUTFChars(host, host_chars);
            }
            if (peer_pk_chars != nullptr) {
                env->ReleaseStringUTFChars(peer_pubkey_b64, peer_pk_chars);
            }
            set_jni_last_error("Failed to read distributed session strings from Java");
            return 0;
        }
        const std::string host_str(host_chars);
        const std::string peer_pubkey_b64_str(peer_pk_chars);
        env->ReleaseStringUTFChars(host, host_chars);
        env->ReleaseStringUTFChars(peer_pubkey_b64, peer_pk_chars);

        auto peer_pk_bytes = upix::base64_decode(peer_pubkey_b64_str);
        if (!peer_pk_bytes || peer_pk_bytes->size() != 32) {
            set_jni_last_error("invalid peer identity pubkey (expected 32-byte base64)");
            return 0;
        }
        std::array<uint8_t, 32> peer_pk{};
        std::memcpy(peer_pk.data(), peer_pk_bytes->data(), 32);

        auto identity = identity_snapshot();
        if (!identity) {
            set_jni_last_error("identity not loaded; call loadIdentity first");
            return 0;
        }

        auto* session_state = new DistributedSession();
        if (!session_state->socket.connect_with_timeout(
                host_str, static_cast<uint16_t>(port), CONNECT_TIMEOUT_MS)) {
            delete session_state;
            set_jni_last_error("Distributed connect failed (timeout or unreachable): " +
                               host_str + ":" + std::to_string(port));
            return 0;
        }
        session_state->socket.set_timeout(IO_TIMEOUT_MS);

        std::string handshake_error;
        auto session = run_client_handshake(
            session_state->socket, peer_pk, *identity, handshake_error);
        if (!session.has_value()) {
            session_state->socket.close();
            delete session_state;
            set_jni_last_error("Distributed handshake failed: " + handshake_error);
            return 0;
        }

        // Pairing gate right after the handshake, mirroring the one-shot flow:
        // trusted peers auto-approve; first-time peers block on the PC's
        // interactive approval. Only after approval may submits travel.
        const std::string own_fp = upix::fingerprint(identity->public_key());
        std::string pairing_error;
        if (!perform_connect_request(
                session_state->socket, *session, "Upix-Android", own_fp, pairing_error)) {
            session_state->socket.close();
            delete session_state;
            set_jni_last_error("Distributed pairing failed: " + pairing_error);
            return 0;
        }

        session_state->session = std::move(*session);
        return reinterpret_cast<jlong>(session_state);
    } catch (const std::exception& exception) {
        set_jni_last_error(std::string("nativeOpenDistributedSession failed: ") + exception.what());
        return 0;
    } catch (...) {
        set_jni_last_error("nativeOpenDistributedSession failed: unknown native error");
        return 0;
    }
}

// Upload one image on an open session (seal + send CLIENT_SUBMIT; >4MB
// inputs travel as a struct frame + ≤8MB DATA chunks unless allowChunked is
// false — the legacy resend path against old PCs). Split from receive so the
// caller can push image N+1 while the PC still works on N.
// Returns false on failure (details via nativeGetLastError).
JNIEXPORT jboolean JNICALL
Java_io_upix_nativelib_NCNNEngine_nativeSendDistributedTask(
    JNIEnv* env,
    jobject thiz,
    jlong handle,
    jstring model_name,
    jint tile_size,
    jint tile_overlap,
    jstring input_format,
    jbyteArray input_image,
    jstring output_format,
    jint output_quality,
    jstring encoder_options_json,
    jboolean allow_chunked
) {
    (void)thiz;
    try {
        if (handle == 0 || model_name == nullptr || input_format == nullptr ||
            input_image == nullptr) {
            set_jni_last_error("Distributed task parameters are incomplete");
            return JNI_FALSE;
        }
        auto* session_state = reinterpret_cast<DistributedSession*>(handle);
        if (!session_state->session.has_value()) {
            set_jni_last_error("Distributed session is closed");
            return JNI_FALSE;
        }

        const char* model_chars = env->GetStringUTFChars(model_name, nullptr);
        const char* in_format_chars = env->GetStringUTFChars(input_format, nullptr);
        const char* format_chars =
            output_format == nullptr ? nullptr : env->GetStringUTFChars(output_format, nullptr);
        if (model_chars == nullptr || in_format_chars == nullptr) {
            if (model_chars != nullptr) {
                env->ReleaseStringUTFChars(model_name, model_chars);
            }
            if (in_format_chars != nullptr) {
                env->ReleaseStringUTFChars(input_format, in_format_chars);
            }
            if (format_chars != nullptr) {
                env->ReleaseStringUTFChars(output_format, format_chars);
            }
            set_jni_last_error("Failed to read distributed task strings from Java");
            return JNI_FALSE;
        }
        const std::string model_str(model_chars);
        const std::string input_format_str(in_format_chars);
        std::string output_format_str = "jpg";
        if (format_chars != nullptr) {
            output_format_str = normalize_output_format(format_chars);
        }
        if (!is_supported_output_format(output_format_str)) {
            output_format_str = "jpg";
        }
        env->ReleaseStringUTFChars(model_name, model_chars);
        env->ReleaseStringUTFChars(input_format, in_format_chars);
        if (format_chars != nullptr) {
            env->ReleaseStringUTFChars(output_format, format_chars);
        }

        const jsize input_size = env->GetArrayLength(input_image);
        if (input_size <= 0) {
            set_jni_last_error("Distributed task input size is invalid");
            return JNI_FALSE;
        }
        std::vector<uint8_t> input_bytes(static_cast<size_t>(input_size));
        env->GetByteArrayRegion(
            input_image, 0, input_size,
            reinterpret_cast<jbyte*>(input_bytes.data()));

        // NOTE: the per-task cancel flags are deliberately NOT cleared here.
        // With submit-ahead (pipeline_depth >= 1) this send can run
        // concurrently with a blocked nativeReceiveDistributedResult; wiping
        // cancel_requested at send time raced a user cancel and could erase
        // it before the receive loop observed it. The receive side clears
        // both flags once, when a new wait begins.

        const bool sent = send_distributed_submit(
            session_state->socket,
            *session_state->session,
            model_str,
            tile_size,
            tile_overlap,
            input_format_str,
            input_bytes.data(),
            input_bytes.size(),
            output_format_str,
            static_cast<int>(output_quality),
            parse_encoder_options(env, encoder_options_json),
            allow_chunked == JNI_TRUE);
        return sent ? JNI_TRUE : JNI_FALSE;
    } catch (const std::exception& exception) {
        set_jni_last_error(std::string("nativeSendDistributedTask failed: ") + exception.what());
        return JNI_FALSE;
    } catch (...) {
        set_jni_last_error("nativeSendDistributedTask failed: unknown native error");
        return JNI_FALSE;
    }
}

// Block until the current task's result arrives, streaming TASK_PROGRESS
// updates to `progressListener` (io.upix.nativelib.DistributedProgressListener,
// nullable). Returns the encoded output bytes, or null on failure (details
// via nativeGetLastError; an error starting with "Distributed task failed:"
// marks a PC-side task error — the session itself stays usable).
JNIEXPORT jbyteArray JNICALL
Java_io_upix_nativelib_NCNNEngine_nativeReceiveDistributedResult(
    JNIEnv* env,
    jobject thiz,
    jlong handle,
    jobject progress_listener
) {
    (void)thiz;
    try {
        if (handle == 0) {
            set_jni_last_error("Distributed session handle is invalid");
            return nullptr;
        }
        auto* session_state = reinterpret_cast<DistributedSession*>(handle);
        if (!session_state->session.has_value()) {
            set_jni_last_error("Distributed session is closed");
            return nullptr;
        }

        std::function<void(uint32_t, uint32_t, uint32_t)> progress_cb;
        if (progress_listener != nullptr) {
            jclass listener_class = env->GetObjectClass(progress_listener);
            jmethodID on_progress = listener_class != nullptr
                ? env->GetMethodID(listener_class, "onProgress", "(III)V")
                : nullptr;
            if (on_progress != nullptr) {
                // The listener is only invoked during this call, on this
                // thread, so the local reference is safe without a global.
                progress_cb = [env, progress_listener, on_progress](
                                  uint32_t stage, uint32_t percent, uint32_t eta) {
                    env->CallVoidMethod(
                        progress_listener, on_progress,
                        static_cast<jint>(stage), static_cast<jint>(percent),
                        static_cast<jint>(eta));
                };
            }
        }

        auto result = receive_distributed_result(*session_state, progress_cb);
        if (!result.has_value()) {
            return nullptr;  // error already in the thread-local slot
        }
        jbyteArray encoded_array =
            env->NewByteArray(static_cast<jsize>(result->size()));
        if (encoded_array == nullptr) {
            set_jni_last_error("Distributed output allocation failed");
            return nullptr;
        }
        env->SetByteArrayRegion(
            encoded_array, 0, static_cast<jsize>(result->size()),
            reinterpret_cast<const jbyte*>(result->data()));
        set_jni_last_error("");
        return encoded_array;
    } catch (const std::exception& exception) {
        set_jni_last_error(std::string("nativeReceiveDistributedResult failed: ") + exception.what());
        return nullptr;
    } catch (...) {
        set_jni_last_error("nativeReceiveDistributedResult failed: unknown native error");
        return nullptr;
    }
}

// Request cancellation of the task currently in flight on this session.
// Thread-safe and non-blocking: it only flips the per-session cancel flag;
// the blocking nativeReceiveDistributedResult (on its IO thread) notices
// within one poll tick (RECEIVE_POLL_MS), sends MSG_TASK_CANCEL itself, and
// returns promptly with a "Distributed task canceled..." error once the PC's
// terminal response (or the drain deadline) arrives. No-op when no task is
// in flight — the flag is reset by the next nativeSendDistributedTask.
JNIEXPORT jboolean JNICALL
Java_io_upix_nativelib_NCNNEngine_nativeCancelDistributedTask(
    JNIEnv* env,
    jobject thiz,
    jlong handle
) {
    (void)env;
    (void)thiz;
    if (handle == 0) {
        return JNI_TRUE;  // no open session: nothing in flight
    }
    auto* session_state = reinterpret_cast<DistributedSession*>(handle);
    if (!session_state->session.has_value()) {
        return JNI_TRUE;
    }
    session_state->cancel_requested.store(true);
    return JNI_TRUE;
}

// Close + free a session handle. Safe on 0 and on already-closed handles.
JNIEXPORT void JNICALL
Java_io_upix_nativelib_NCNNEngine_nativeCloseDistributedSession(
    JNIEnv* env,
    jobject thiz,
    jlong handle
) {
    (void)env;
    (void)thiz;
    if (handle == 0) {
        return;
    }
    auto* session_state = reinterpret_cast<DistributedSession*>(handle);
    session_state->socket.close();
    delete session_state;
}


JNIEXPORT jint JNICALL
Java_io_upix_nativelib_NCNNEngine_nativeGetScaleFactor(
    JNIEnv* env,
    jobject thiz,
    jlong engine_ptr
) {
    (void)env;
    (void)thiz;
    try {
        if (engine_ptr == 0) return 2;

        auto* engine = reinterpret_cast<Engine*>(engine_ptr);
        return engine->get_scale_factor();
    } catch (const std::exception& exception) {
        set_jni_last_error(std::string("nativeGetScaleFactor failed: ") + exception.what());
        return 0;
    } catch (...) {
        set_jni_last_error("nativeGetScaleFactor failed: unknown native error");
        return 0;
    }
}

JNIEXPORT jstring JNICALL
Java_io_upix_nativelib_NCNNEngine_nativeDebugBlobScan(
    JNIEnv* env,
    jobject thiz,
    jlong engine_ptr,
    jbyteArray input_image,
    jint width,
    jint height,
    jint channels
) {
    (void)thiz;
    if (engine_ptr == 0 || input_image == nullptr) return nullptr;
    if (width <= 0 || height <= 0 || channels <= 0) {
        set_jni_last_error("nativeDebugBlobScan: invalid dimensions");
        return nullptr;
    }
    const jsize input_size = env->GetArrayLength(input_image);
    const int64_t expected =
        static_cast<int64_t>(width) * static_cast<int64_t>(height) * static_cast<int64_t>(channels);
    if (expected <= 0 || input_size != static_cast<jsize>(expected)) {
        set_jni_last_error("nativeDebugBlobScan: input byte array size mismatch");
        return nullptr;
    }
    jbyte* input_data = env->GetByteArrayElements(input_image, nullptr);
    if (input_data == nullptr) {
        set_jni_last_error("nativeDebugBlobScan: failed to access input bytes");
        return nullptr;
    }
    std::string report;
    try {
        auto* engine = reinterpret_cast<Engine*>(engine_ptr);
        report = engine->debug_blob_scan(
            reinterpret_cast<const uint8_t*>(input_data),
            width,
            height,
            channels
        );
    } catch (const std::exception& exception) {
        report = std::string("nativeDebugBlobScan failed: ") + exception.what();
    } catch (...) {
        report = "nativeDebugBlobScan failed: unknown native error";
    }
    env->ReleaseByteArrayElements(input_image, input_data, JNI_ABORT);
    return env->NewStringUTF(report.c_str());
}

// Diagnostic: ncnn logs failures to stderr and the engine logger writes
// stdout — Android discards both. Point fd 1 and fd 2 at one file so all of
// it becomes retrievable on-device.
JNIEXPORT jboolean JNICALL
Java_io_upix_nativelib_NCNNEngine_nativeRedirectStderr(
    JNIEnv* env,
    jobject thiz,
    jstring path
) {
    (void)thiz;
    if (path == nullptr) return JNI_FALSE;
    const char* path_c = env->GetStringUTFChars(path, nullptr);
    if (path_c == nullptr) return JNI_FALSE;
    fflush(stderr);
    fflush(stdout);
    const int fd = open(path_c, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
    env->ReleaseStringUTFChars(path, path_c);
    return fd >= 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_io_upix_nativelib_NCNNEngine_nativeRelease(
    JNIEnv* env,
    jobject thiz,
    jlong engine_ptr
) {
    (void)env;
    (void)thiz;
    try {
        if (engine_ptr == 0) return;

        auto* engine = reinterpret_cast<Engine*>(engine_ptr);
        delete engine;
        LOGI("Engine released");
    } catch (const std::exception& exception) {
        set_jni_last_error(std::string("nativeRelease failed: ") + exception.what());
    } catch (...) {
        set_jni_last_error("nativeRelease failed: unknown native error");
    }
}

JNIEXPORT jstring JNICALL
Java_io_upix_nativelib_NativeWorker_nativeRequestConnection(
    JNIEnv* env,
    jobject thiz,
    jstring host,
    jint port,
    jstring peer_pubkey_b64
) {
    (void)thiz;
    try {
        set_jni_last_error("");

        if (host == nullptr) {
            set_jni_last_error("requestConnection host is null");
            return nullptr;
        }
        if (port <= 0 || port > std::numeric_limits<uint16_t>::max()) {
            set_jni_last_error("requestConnection port is invalid");
            return nullptr;
        }

        const char* host_chars = env->GetStringUTFChars(host, nullptr);
        if (host_chars == nullptr) {
            set_jni_last_error("Failed to read requestConnection host from Java");
            return nullptr;
        }
        const std::string host_str(host_chars);
        env->ReleaseStringUTFChars(host, host_chars);

        // peer_pubkey_b64 may be null/empty (manual connect by IP+port → TOFU mode,
        // server identity learned during handshake) or a 32-byte base64 (mDNS/QR →
        // strict MITM check).
        std::string peer_pubkey_b64_str;
        if (peer_pubkey_b64 != nullptr) {
            const char* peer_pk_chars = env->GetStringUTFChars(peer_pubkey_b64, nullptr);
            if (peer_pk_chars != nullptr) {
                peer_pubkey_b64_str.assign(peer_pk_chars);
                env->ReleaseStringUTFChars(peer_pubkey_b64, peer_pk_chars);
            }
        }
        const bool tofu_mode = peer_pubkey_b64_str.empty();

        std::array<uint8_t, 32> peer_pk{};
        std::array<uint8_t, 32> learned_peer_pub{};
        if (!tofu_mode) {
            auto peer_pk_bytes = upix::base64_decode(peer_pubkey_b64_str);
            if (!peer_pk_bytes || peer_pk_bytes->size() != 32) {
                set_jni_last_error("invalid peer identity pubkey (expected 32-byte base64)");
                return nullptr;
            }
            std::memcpy(peer_pk.data(), peer_pk_bytes->data(), 32);
        }
        auto identity = identity_snapshot();
        if (!identity) {
            set_jni_last_error("identity not loaded; call loadIdentity first");
            return nullptr;
        }

        // 计算手机自身的指纹(从长期身份公钥派生), 供 PC 在批准提示中展示。
        const std::string own_fingerprint = upix::fingerprint(identity->public_key());

        upix::TCPSocket socket;
        if (!socket.connect_with_timeout(host_str, static_cast<uint16_t>(port), CONNECT_TIMEOUT_MS)) {
            set_jni_last_error("requestConnection connect failed (timeout or unreachable): " +
                               host_str + ":" + std::to_string(port));
            return nullptr;
        }
        socket.set_timeout(IO_TIMEOUT_MS);

        std::string handshake_error;
        auto session = run_client_handshake(
            socket, peer_pk, *identity, handshake_error,
            tofu_mode, &learned_peer_pub
        );
        if (!session.has_value()) {
            set_jni_last_error("requestConnection handshake failed: " + handshake_error);
            return nullptr;
        }

        std::string pairing_error;
        const bool approved = perform_connect_request(
            socket,
            *session,
            "Upix-Android",
            own_fingerprint,
            pairing_error
        );
        if (!approved) {
            set_jni_last_error("requestConnection pairing failed: " + pairing_error);
            LOGW("Pairing rejected for %s:%d — %s",
                 host_str.c_str(), port, pairing_error.c_str());
            return nullptr;
        }

        // Resolve the server identity to return to Kotlin. In strict mode it's the
        // pre-known key; in TOFU mode it's the key learned during this handshake.
        const std::string result_pubkey_b64 = tofu_mode
            ? upix::base64_encode(learned_peer_pub.data(), learned_peer_pub.size())
            : peer_pubkey_b64_str;
        const auto& result_pubkey_arr = tofu_mode ? learned_peer_pub : peer_pk;

        LOGI("Pairing approved for %s:%d (tofu=%d, fp=%s)",
             host_str.c_str(), port, tofu_mode ? 1 : 0,
             upix::fingerprint(result_pubkey_arr).c_str());
        return env->NewStringUTF(result_pubkey_b64.c_str());
    } catch (const std::exception& exception) {
        set_jni_last_error(std::string("nativeRequestConnection failed: ") + exception.what());
        return nullptr;
    } catch (...) {
        set_jni_last_error("nativeRequestConnection failed: unknown native error");
        return nullptr;
    }
}

JNIEXPORT jstring JNICALL
Java_io_upix_nativelib_NativeWorker_nativeListPCModels(
    JNIEnv* env,
    jobject thiz,
    jstring host,
    jint port,
    jstring peer_pubkey_b64
) {
    (void)thiz;
    try {
        set_jni_last_error("");

        std::string host_str;
        uint16_t port_u16 = 0;
        std::array<uint8_t, 32> peer_pk{};
        if (!resolve_endpoint_params(env, host, port, peer_pubkey_b64,
                                     host_str, port_u16, peer_pk)) {
            return nullptr;
        }

        std::string link_error;
        auto link = open_paired_link(host_str, port_u16, peer_pk, link_error);
        if (!link.has_value()) {
            set_jni_last_error("Model list: " + link_error);
            return nullptr;
        }

        std::string send_error;
        static const uint8_t kEmptyPayload = 0;  // seal reads len=0 bytes from it
        if (!send_sealed(*link, upix::MessageType::MSG_MODEL_LIST_REQUEST,
                         &kEmptyPayload, 0, send_error)) {
            set_jni_last_error("Model list: " + send_error);
            return nullptr;
        }

        std::string frame_error;
        auto frame = recv_encrypted_frame(link->socket, link->session, frame_error);
        if (!frame) {
            set_jni_last_error("Model list: " + frame_error);
            return nullptr;
        }
        if (frame->first.type != upix::MessageType::MSG_MODEL_LIST_RESPONSE ||
            frame->second.size() < sizeof(upix::ModelListResponse)) {
            set_jni_last_error("Model list: unexpected response frame");
            return nullptr;
        }

        upix::ModelListResponse list_resp{};
        std::memcpy(&list_resp, frame->second.data(), sizeof(list_resp));
        const size_t expected = sizeof(list_resp) +
            static_cast<size_t>(list_resp.count) * sizeof(upix::ModelLibraryEntry);
        if (list_resp.count > 1024 || frame->second.size() < expected) {
            set_jni_last_error("Model list: payload truncated");
            return nullptr;
        }

        // One record per line: "name\tscale\tsize". Model names are sanitized
        // (alnum/_/-/space) on both ends, so tabs are safe separators.
        std::ostringstream out;
        for (uint32_t i = 0; i < list_resp.count; ++i) {
            upix::ModelLibraryEntry entry{};
            const size_t offset = sizeof(list_resp) +
                static_cast<size_t>(i) * sizeof(entry);
            std::memcpy(&entry, frame->second.data() + offset, sizeof(entry));
            out << entry.name << '\t' << entry.scale_factor << '\t' << entry.total_size << '\n';
        }
        set_jni_last_error("");
        return env->NewStringUTF(out.str().c_str());
    } catch (const std::exception& exception) {
        set_jni_last_error(std::string("nativeListPCModels failed: ") + exception.what());
        return nullptr;
    } catch (...) {
        set_jni_last_error("nativeListPCModels failed: unknown native error");
        return nullptr;
    }
}

JNIEXPORT jboolean JNICALL
Java_io_upix_nativelib_NativeWorker_nativeUploadModelToPC(
    JNIEnv* env,
    jobject thiz,
    jstring host,
    jint port,
    jstring peer_pubkey_b64,
    jstring model_name,
    jint scale_factor,
    jstring param_path,
    jstring bin_path
) {
    (void)thiz;
    try {
        set_jni_last_error("");

        std::string host_str;
        uint16_t port_u16 = 0;
        std::array<uint8_t, 32> peer_pk{};
        if (!resolve_endpoint_params(env, host, port, peer_pubkey_b64,
                                     host_str, port_u16, peer_pk)) {
            return JNI_FALSE;
        }
        if (model_name == nullptr || param_path == nullptr || bin_path == nullptr) {
            set_jni_last_error("Model upload: name/paths are null");
            return JNI_FALSE;
        }

        // Check sizes via the file system BEFORE reading: two near-limit files
        // would otherwise both be fully buffered in memory before the combined
        // cap below runs. Opening in ate mode seeks to the end without reading.
        auto stat_file_size = [&](jstring path_jstr, uint64_t& size_out) -> bool {
            const char* chars = env->GetStringUTFChars(path_jstr, nullptr);
            if (chars == nullptr) {
                set_jni_last_error("Model upload: failed to read path string");
                return false;
            }
            const std::string path(chars);
            env->ReleaseStringUTFChars(path_jstr, chars);
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file) {
                set_jni_last_error("Model upload: cannot open " + path);
                return false;
            }
            const std::streamoff size = file.tellg();
            if (size <= 0) {
                set_jni_last_error("Model upload: invalid file size for " + path);
                return false;
            }
            size_out = static_cast<uint64_t>(size);
            return true;
        };
        uint64_t param_file_size = 0;
        uint64_t bin_file_size = 0;
        if (!stat_file_size(param_path, param_file_size) ||
            !stat_file_size(bin_path, bin_file_size)) {
            return JNI_FALSE;
        }
        if (param_file_size > MAX_PAYLOAD_LENGTH || bin_file_size > MAX_PAYLOAD_LENGTH ||
            param_file_size + bin_file_size > MAX_PAYLOAD_LENGTH) {
            set_jni_last_error("Model upload: model exceeds transfer limit");
            return JNI_FALSE;
        }

        auto read_all = [&](jstring path_jstr, std::vector<uint8_t>& out) -> bool {
            const char* chars = env->GetStringUTFChars(path_jstr, nullptr);
            if (chars == nullptr) {
                set_jni_last_error("Model upload: failed to read path string");
                return false;
            }
            const std::string path(chars);
            env->ReleaseStringUTFChars(path_jstr, chars);
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                set_jni_last_error("Model upload: cannot open " + path);
                return false;
            }
            file.seekg(0, std::ios::end);
            const std::streamoff size = file.tellg();
            file.seekg(0, std::ios::beg);
            if (size <= 0 || static_cast<uint64_t>(size) > MAX_PAYLOAD_LENGTH) {
                set_jni_last_error("Model upload: invalid file size for " + path);
                return false;
            }
            out.resize(static_cast<size_t>(size));
            return static_cast<bool>(
                file.read(reinterpret_cast<char*>(out.data()), size));
        };

        std::vector<uint8_t> param_bytes;
        std::vector<uint8_t> bin_bytes;
        if (!read_all(param_path, param_bytes) || !read_all(bin_path, bin_bytes)) {
            return JNI_FALSE;
        }
        if (param_bytes.size() + bin_bytes.size() > MAX_PAYLOAD_LENGTH) {
            set_jni_last_error("Model upload: model exceeds transfer limit");
            return JNI_FALSE;
        }

        const char* name_chars = env->GetStringUTFChars(model_name, nullptr);
        if (name_chars == nullptr) {
            set_jni_last_error("Model upload: failed to read model name");
            return JNI_FALSE;
        }
        const std::string name(name_chars);
        env->ReleaseStringUTFChars(model_name, name_chars);

        std::string link_error;
        auto link = open_paired_link(host_str, port_u16, peer_pk, link_error);
        if (!link.has_value()) {
            set_jni_last_error("Model upload: " + link_error);
            return JNI_FALSE;
        }

        upix::ModelUploadRequest req{};
        std::strncpy(req.name, name.c_str(), sizeof(req.name) - 1);
        req.scale_factor = static_cast<uint32_t>(std::max(scale_factor, 1));
        req.param_size = param_bytes.size();
        req.bin_size = bin_bytes.size();

        std::vector<uint8_t> plaintext(sizeof(req) + param_bytes.size() + bin_bytes.size());
        std::memcpy(plaintext.data(), &req, sizeof(req));
        std::memcpy(plaintext.data() + sizeof(req), param_bytes.data(), param_bytes.size());
        std::memcpy(plaintext.data() + sizeof(req) + param_bytes.size(),
                    bin_bytes.data(), bin_bytes.size());

        std::string send_error;
        if (!send_sealed(*link, upix::MessageType::MSG_MODEL_UPLOAD,
                         plaintext.data(), plaintext.size(), send_error)) {
            set_jni_last_error("Model upload: " + send_error);
            return JNI_FALSE;
        }

        std::string frame_error;
        auto frame = recv_encrypted_frame(link->socket, link->session, frame_error);
        if (!frame ||
            frame->first.type != upix::MessageType::MSG_MODEL_UPLOAD_RESPONSE ||
            frame->second.size() < sizeof(upix::ModelOpResponse)) {
            set_jni_last_error("Model upload: bad response (" + frame_error + ")");
            return JNI_FALSE;
        }
        upix::ModelOpResponse resp{};
        std::memcpy(&resp, frame->second.data(), sizeof(resp));
        if (resp.success == 0) {
            set_jni_last_error(std::string("Model upload rejected by PC: ") + resp.error);
            return JNI_FALSE;
        }
        set_jni_last_error("");
        LOGI("Model uploaded to PC: %s (%zu bytes)", name.c_str(),
             param_bytes.size() + bin_bytes.size());
        return JNI_TRUE;
    } catch (const std::exception& exception) {
        set_jni_last_error(std::string("nativeUploadModelToPC failed: ") + exception.what());
        return JNI_FALSE;
    } catch (...) {
        set_jni_last_error("nativeUploadModelToPC failed: unknown native error");
        return JNI_FALSE;
    }
}

JNIEXPORT jboolean JNICALL
Java_io_upix_nativelib_NativeWorker_nativeDownloadModelFromPC(
    JNIEnv* env,
    jobject thiz,
    jstring host,
    jint port,
    jstring peer_pubkey_b64,
    jstring model_name,
    jstring dest_param_path,
    jstring dest_bin_path
) {
    (void)thiz;
    try {
        set_jni_last_error("");

        std::string host_str;
        uint16_t port_u16 = 0;
        std::array<uint8_t, 32> peer_pk{};
        if (!resolve_endpoint_params(env, host, port, peer_pubkey_b64,
                                     host_str, port_u16, peer_pk)) {
            return JNI_FALSE;
        }
        if (model_name == nullptr || dest_param_path == nullptr || dest_bin_path == nullptr) {
            set_jni_last_error("Model download: name/paths are null");
            return JNI_FALSE;
        }

        const char* name_chars = env->GetStringUTFChars(model_name, nullptr);
        if (name_chars == nullptr) {
            set_jni_last_error("Model download: failed to read model name");
            return JNI_FALSE;
        }
        const std::string name(name_chars);
        env->ReleaseStringUTFChars(model_name, name_chars);

        std::string link_error;
        auto link = open_paired_link(host_str, port_u16, peer_pk, link_error);
        if (!link.has_value()) {
            set_jni_last_error("Model download: " + link_error);
            return JNI_FALSE;
        }

        upix::ModelDownloadRequest req{};
        std::strncpy(req.name, name.c_str(), sizeof(req.name) - 1);

        std::string send_error;
        if (!send_sealed(*link, upix::MessageType::MSG_MODEL_DOWNLOAD_REQUEST,
                         &req, sizeof(req), send_error)) {
            set_jni_last_error("Model download: " + send_error);
            return JNI_FALSE;
        }

        std::string frame_error;
        auto frame = recv_encrypted_frame(link->socket, link->session, frame_error);
        if (!frame ||
            frame->first.type != upix::MessageType::MSG_MODEL_DOWNLOAD_RESPONSE ||
            frame->second.size() < sizeof(upix::ModelDownloadResponse)) {
            set_jni_last_error("Model download: bad response (" + frame_error + ")");
            return JNI_FALSE;
        }
        upix::ModelDownloadResponse resp{};
        std::memcpy(&resp, frame->second.data(), sizeof(resp));
        if (resp.success == 0) {
            set_jni_last_error(std::string("Model download failed on PC: ") + resp.error);
            return JNI_FALSE;
        }
        // Security: validate declared sizes against the actual frame before use.
        if (resp.param_size == 0 || resp.bin_size == 0 ||
            resp.param_size > MAX_PAYLOAD_LENGTH || resp.bin_size > MAX_PAYLOAD_LENGTH ||
            resp.param_size + resp.bin_size > MAX_PAYLOAD_LENGTH ||
            sizeof(resp) + resp.param_size + resp.bin_size != frame->second.size()) {
            set_jni_last_error("Model download: payload truncated or oversized");
            return JNI_FALSE;
        }

        auto write_file = [&](jstring path_jstr, const uint8_t* data, uint64_t size) -> bool {
            const char* chars = env->GetStringUTFChars(path_jstr, nullptr);
            if (chars == nullptr) {
                set_jni_last_error("Model download: failed to read destination path");
                return false;
            }
            const std::string path(chars);
            env->ReleaseStringUTFChars(path_jstr, chars);
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file) {
                set_jni_last_error("Model download: cannot open " + path);
                return false;
            }
            file.write(reinterpret_cast<const char*>(data),
                       static_cast<std::streamsize>(size));
            file.close();
            if (!file) {
                set_jni_last_error("Model download: write failed for " + path);
                return false;
            }
            return true;
        };

        const uint8_t* param_begin = frame->second.data() + sizeof(resp);
        const uint8_t* bin_begin = param_begin + resp.param_size;
        if (!write_file(dest_param_path, param_begin, resp.param_size)) {
            return JNI_FALSE;
        }
        if (!write_file(dest_bin_path, bin_begin, resp.bin_size)) {
            return JNI_FALSE;
        }
        set_jni_last_error("");
        LOGI("Model downloaded from PC: %s (%llu bytes)",
             name.c_str(), static_cast<unsigned long long>(resp.total_size));
        return JNI_TRUE;
    } catch (const std::exception& exception) {
        set_jni_last_error(std::string("nativeDownloadModelFromPC failed: ") + exception.what());
        return JNI_FALSE;
    } catch (...) {
        set_jni_last_error("nativeDownloadModelFromPC failed: unknown native error");
        return JNI_FALSE;
    }
}

JNIEXPORT jboolean JNICALL
Java_io_upix_nativelib_NativeWorker_nativeLoadIdentity(
    JNIEnv* env,
    jobject thiz,
    jstring identity_path
) {
    (void)thiz;
    if (identity_path == nullptr) {
        set_jni_last_error("identity path is null");
        return JNI_FALSE;
    }
    const char* path_chars = env->GetStringUTFChars(identity_path, nullptr);
    if (path_chars == nullptr) {
        set_jni_last_error("failed to read identity path string");
        return JNI_FALSE;
    }
    std::string path_str(path_chars);
    env->ReleaseStringUTFChars(identity_path, path_chars);

    try {
        auto identity = std::make_shared<const upix::IdentityKey>(
            upix::IdentityKey::load_or_create(path_str));
        {
            std::lock_guard<std::mutex> lock(g_identity_mutex);
            g_identity = std::move(identity);
        }
        LOGI("Identity loaded from %s, fingerprint will be derived on connect", path_str.c_str());
        set_jni_last_error("");
        return JNI_TRUE;
    } catch (const std::exception& e) {
        set_jni_last_error(std::string("failed to load/create identity: ") + e.what());
        LOGE("Identity load failed: %s", e.what());
        return JNI_FALSE;
    } catch (...) {
        set_jni_last_error("failed to load/create identity: unknown native error");
        LOGE("Identity load failed: unknown error");
        return JNI_FALSE;
    }
}

}
