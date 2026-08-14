#include <jni.h>
#include <string>
#include <android/log.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
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
#include "utils/network.hpp"

#define LOG_TAG "CrossScale"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Security fix: Maximum allowed payload size (100 MB) to prevent malicious
// servers from causing excessive memory allocation via crafted payload_length values.
static constexpr uint32_t MAX_PAYLOAD_LENGTH = 100 * 1024 * 1024;

using namespace crossscale;

namespace {

std::mutex g_jni_error_mutex;
std::string g_jni_last_error;

void set_jni_last_error(const std::string& error_message) {
    std::lock_guard<std::mutex> lock(g_jni_error_mutex);
    g_jni_last_error = error_message;
}

auto get_jni_last_error() -> std::string {
    std::lock_guard<std::mutex> lock(g_jni_error_mutex);
    return g_jni_last_error;
}

// Process-wide long-term Ed25519 identity. Loaded once via nativeLoadIdentity;
// used by every worker/client connection to sign the ECDH handshake.
// std::optional because IdentityKey is not default-constructible.
std::optional<crossscale::IdentityKey> g_identity;

// Client side of the 3-message ECDH+signature handshake. Mirrors
// Worker::handshake() in share/core/distributed/worker.cpp byte-for-byte.
// On success returns an activated EncryptedSession; on failure returns
// nullopt and sets error_msg.
auto run_client_handshake(
    crossscale::TCPSocket& socket,
    const std::array<uint8_t, 32>& peer_identity_pubkey,
    crossscale::IdentityKey& local_identity,
    std::string& error_msg,
    bool tofu_mode = false,
    std::array<uint8_t, 32>* learned_peer_pub = nullptr
) -> std::optional<crossscale::EncryptedSession> {
    error_msg.clear();
    crossscale::sodium::init();

    // (1) Generate ephemeral X25519 keypair, send KEY_HELLO (plaintext).
    auto eph = crossscale::sodium::x25519_keygen();
    const auto c_eph_pub = eph.public_key;
    const auto c_id_pub = local_identity.public_key();

    crossscale::KeyHelloMessage c_hello{};
    std::memcpy(c_hello.eph_pub, c_eph_pub.data(), 32);
    std::memcpy(c_hello.id_pub, c_id_pub.data(), 32);
    std::memset(c_hello.sig, 0, 64);  // client's hello carries no signature

    {
        crossscale::MessageHeader hh{};
        hh.type = crossscale::MessageType::MSG_KEY_HELLO;
        hh.payload_length = sizeof(c_hello);
        auto wire = crossscale::Protocol::serialize(hh, &c_hello);
        if (!socket.send_all(wire.data(), wire.size()).has_value()) {
            error_msg = "KEY_HELLO send failed";
            return std::nullopt;
        }
    }

    // (2) Receive server KEY_HELLO with its signature.
    crossscale::KeyHelloMessage s_hello{};
    {
        std::vector<uint8_t> hdr_buf(crossscale::Protocol::HEADER_SIZE);
        if (!socket.receive_all(hdr_buf.data(), hdr_buf.size()).has_value()) {
            error_msg = "KEY_HELLO header recv failed";
            return std::nullopt;
        }
        auto parsed_hdr = crossscale::Protocol::parse_header(hdr_buf.data(), hdr_buf.size());
        if (!parsed_hdr || parsed_hdr->type != crossscale::MessageType::MSG_KEY_HELLO ||
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
             crossscale::fingerprint(s_id_pub_arr).c_str());
    } else if (s_id_pub_arr != peer_identity_pubkey) {
        error_msg = "server identity mismatch (possible MITM)";
        return std::nullopt;
    }

    // (5) Verify server signature over the transcript.
    std::array<uint8_t, 64> s_sig_arr{};
    std::memcpy(s_sig_arr.data(), s_hello.sig, 64);
    if (!crossscale::sodium::ed25519_verify(s_id_pub_arr, s_sig_arr,
                                            transcript.data(), transcript.size())) {
        error_msg = "server identity signature invalid";
        return std::nullopt;
    }

    // (6) Derive session key and activate EncryptedSession.
    std::array<uint8_t, 32> s_eph_pub_arr{};
    std::memcpy(s_eph_pub_arr.data(), s_hello.eph_pub, 32);
    auto session_key = crossscale::sodium::x25519_shared(eph.private_key, s_eph_pub_arr);

    crossscale::EncryptedSession session;
    session.activate(session_key);

    // (7) Sign the same transcript with our identity, send AUTH_PROOF (encrypted).
    auto c_sig = local_identity.sign(transcript.data(), transcript.size());
    crossscale::AuthProofMessage proof{};
    std::memcpy(proof.sig, c_sig.data(), 64);
    {
        crossscale::MessageHeader ph{};
        ph.type = crossscale::MessageType::MSG_AUTH_PROOF;
        ph.payload_length = sizeof(proof);
        auto pwire = session.seal(ph, &proof);
        if (!socket.send_all(pwire.data(), pwire.size()).has_value()) {
            error_msg = "AUTH_PROOF send failed";
            return std::nullopt;
        }
    }

    // (8) Receive AUTH_RESPONSE (encrypted).
    {
        std::vector<uint8_t> hdr_buf(crossscale::Protocol::HEADER_SIZE);
        if (!socket.receive_all(hdr_buf.data(), hdr_buf.size()).has_value()) {
            error_msg = "AUTH_RESPONSE header recv failed";
            return std::nullopt;
        }
        auto parsed = crossscale::Protocol::parse_header(hdr_buf.data(), hdr_buf.size());
        if (!parsed) {
            error_msg = "AUTH_RESPONSE header invalid";
            return std::nullopt;
        }
        if (parsed->payload_length > MAX_PAYLOAD_LENGTH) {
            error_msg = "AUTH_RESPONSE payload too large";
            return std::nullopt;
        }
        std::vector<uint8_t> wire(crossscale::Protocol::HEADER_SIZE + parsed->payload_length);
        std::memcpy(wire.data(), hdr_buf.data(), crossscale::Protocol::HEADER_SIZE);
        if (parsed->payload_length > 0) {
            if (!socket.receive_all(wire.data() + crossscale::Protocol::HEADER_SIZE,
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
        if (resp_hdr.type != crossscale::MessageType::MSG_AUTH_RESPONSE ||
            resp_payload.size() < sizeof(crossscale::AuthResponse)) {
            error_msg = "authentication rejected by server";
            return std::nullopt;
        }
        const auto* auth_resp = reinterpret_cast<const crossscale::AuthResponse*>(resp_payload.data());
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
    crossscale::TCPSocket& socket,
    crossscale::EncryptedSession& session,
    const std::string& device_name,
    const std::string& own_fingerprint,
    bool wants_encrypted_stream,
    std::string& error_msg
) -> bool {
    error_msg.clear();

    // (1) 构造 ClientConnectRequest: 设备名 + 自己的指纹 + 加密意愿。
    crossscale::ClientConnectRequest req{};
    std::memset(req.device_name, 0, sizeof(req.device_name));
    std::strncpy(req.device_name, device_name.c_str(), sizeof(req.device_name) - 1);
    std::memset(req.fingerprint, 0, sizeof(req.fingerprint));
    std::strncpy(req.fingerprint, own_fingerprint.c_str(), sizeof(req.fingerprint) - 1);
    req.wants_encrypted_stream = wants_encrypted_stream ? 1 : 0;

    crossscale::MessageHeader req_hdr{};
    req_hdr.type = crossscale::MessageType::MSG_CLIENT_CONNECT_REQUEST;
    req_hdr.payload_length = sizeof(req);
    auto req_wire = session.seal(req_hdr, &req);
    if (!socket.send_all(req_wire.data(), req_wire.size()).has_value()) {
        error_msg = "CONNECT_REQUEST send failed";
        return false;
    }

    // (2) 接收加密的 CONNECT_RESPONSE 并解析。
    std::vector<uint8_t> hdr_buf(crossscale::Protocol::HEADER_SIZE);
    if (!socket.receive_all(hdr_buf.data(), hdr_buf.size()).has_value()) {
        error_msg = "CONNECT_RESPONSE header recv failed";
        return false;
    }
    auto parsed = crossscale::Protocol::parse_header(hdr_buf.data(), hdr_buf.size());
    if (!parsed) {
        error_msg = "CONNECT_RESPONSE header invalid";
        return false;
    }
    if (parsed->payload_length > MAX_PAYLOAD_LENGTH) {
        error_msg = "CONNECT_RESPONSE payload too large";
        return false;
    }
    std::vector<uint8_t> wire(crossscale::Protocol::HEADER_SIZE + parsed->payload_length);
    std::memcpy(wire.data(), hdr_buf.data(), crossscale::Protocol::HEADER_SIZE);
    if (parsed->payload_length > 0) {
        if (!socket.receive_all(wire.data() + crossscale::Protocol::HEADER_SIZE,
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
    if (resp_hdr.type != crossscale::MessageType::MSG_CLIENT_CONNECT_RESPONSE ||
        resp_payload.size() < sizeof(crossscale::ClientConnectResponse)) {
        error_msg = "unexpected response during pairing";
        return false;
    }
    const auto* resp = reinterpret_cast<const crossscale::ClientConnectResponse*>(resp_payload.data());
    if (resp->approved == 0) {
        error_msg = "PC denied the connection";
        return false;
    }
    // Stream encryption was removed (all frames are sealed); resp->stream_encrypted
    // is now always 0 and ignored. The session stays in its default sealed mode.
    return true;
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_crossscale_native_NCNNEngine_nativeInit(
    JNIEnv* env,
    jobject thiz,
    jint device_type
) {
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
}

JNIEXPORT jboolean JNICALL
Java_com_crossscale_native_NCNNEngine_nativeLoadModel(
    JNIEnv* env,
    jobject thiz,
    jlong engine_ptr,
    jstring model_path
) {
    (void)thiz;
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
}

JNIEXPORT jstring JNICALL
Java_com_crossscale_native_NCNNEngine_nativeGetLastError(
    JNIEnv* env,
    jobject thiz
) {
    (void)thiz;
    const std::string last_error = get_jni_last_error();
    return env->NewStringUTF(last_error.c_str());
}

JNIEXPORT jbyteArray JNICALL
Java_com_crossscale_native_NCNNEngine_nativeProcessImage(
    JNIEnv* env,
    jobject thiz,
    jlong engine_ptr,
    jbyteArray input_image,
    jint width,
    jint height,
    jint channels,
    jstring output_format,
    jint output_quality
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
        LOGE("Unsupported output format: %s", output_config.output_format.c_str());
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
            LOGE("Engine processing failed: %s", result.error().message.c_str());
            return nullptr;
        }

        Image output_image(output_width, output_height, channels);
        std::copy(output_u8.begin(), output_u8.end(), output_image.data());

        auto encoded_result = output_image.save_to_memory(
            output_config.output_format,
            output_config.output_quality
        );
        if (!encoded_result.has_value()) {
            LOGE("Failed to encode output image: %s", encoded_result.error().message.c_str());
            return nullptr;
        }

        const auto& encoded = encoded_result.value();
        jbyteArray output = env->NewByteArray(static_cast<jsize>(encoded.size()));
        if (output == nullptr) {
            LOGE("Failed to allocate output byte array");
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
        return nullptr;
    } catch (const std::exception& exception) {
        if (input_data != nullptr) {
            env->ReleaseByteArrayElements(input_image, input_data, JNI_ABORT);
        }
        LOGE("Unexpected native processing exception: %s", exception.what());
        return nullptr;
    }
}

JNIEXPORT jfloat JNICALL
Java_com_crossscale_native_NCNNEngine_nativeBenchmark(
    JNIEnv* env,
    jobject thiz,
    jlong engine_ptr
) {
    (void)env;
    (void)thiz;
    if (engine_ptr == 0) return 0.0f;
    
    auto* engine = reinterpret_cast<Engine*>(engine_ptr);
    return engine->benchmark(256);
}

JNIEXPORT jbyteArray JNICALL
Java_com_crossscale_native_NCNNEngine_nativeProcessImageDistributed(
    JNIEnv* env,
    jobject thiz,
    jstring host,
    jint port,
    jstring peer_pubkey_b64,
    jboolean wants_encrypted_stream,
    jstring model_name,
    jint tile_size,
    jint tile_overlap,
    jbyteArray input_image,
    jint width,
    jint height,
    jint channels,
    jstring output_format,
    jint output_quality
) {
    (void)thiz;
    if (host == nullptr || peer_pubkey_b64 == nullptr || model_name == nullptr || input_image == nullptr) {
        set_jni_last_error("Distributed processing parameters are incomplete");
        return nullptr;
    }
    if (port <= 0 || port > std::numeric_limits<uint16_t>::max()) {
        set_jni_last_error("Distributed processing port is invalid");
        return nullptr;
    }
    if (width <= 0 || height <= 0 || channels <= 0) {
        set_jni_last_error("Distributed processing image shape is invalid");
        return nullptr;
    }

    const char* host_chars = env->GetStringUTFChars(host, nullptr);
    const char* peer_pk_chars = env->GetStringUTFChars(peer_pubkey_b64, nullptr);
    const char* model_chars = env->GetStringUTFChars(model_name, nullptr);
    const char* format_chars = output_format == nullptr ? nullptr : env->GetStringUTFChars(output_format, nullptr);

    if (host_chars == nullptr || peer_pk_chars == nullptr || model_chars == nullptr) {
        if (host_chars != nullptr) {
            env->ReleaseStringUTFChars(host, host_chars);
        }
        if (peer_pk_chars != nullptr) {
            env->ReleaseStringUTFChars(peer_pubkey_b64, peer_pk_chars);
        }
        if (model_chars != nullptr) {
            env->ReleaseStringUTFChars(model_name, model_chars);
        }
        if (format_chars != nullptr) {
            env->ReleaseStringUTFChars(output_format, format_chars);
        }
        set_jni_last_error("Failed to read distributed processing strings from Java");
        return nullptr;
    }

    const std::string host_str(host_chars);
    const std::string peer_pubkey_b64_str(peer_pk_chars);
    const std::string model_str(model_chars);
    std::string output_format_str = "jpg";
    if (format_chars != nullptr) {
        output_format_str = normalize_output_format(format_chars);
    }
    if (!is_supported_output_format(output_format_str)) {
        output_format_str = "jpg";
    }
    const int normalized_quality = std::clamp(static_cast<int>(output_quality), 1, 100);

    env->ReleaseStringUTFChars(host, host_chars);
    env->ReleaseStringUTFChars(peer_pubkey_b64, peer_pk_chars);
    env->ReleaseStringUTFChars(model_name, model_chars);
    if (format_chars != nullptr) {
        env->ReleaseStringUTFChars(output_format, format_chars);
    }

    const jsize input_size = env->GetArrayLength(input_image);
    const int64_t expected_input_size =
        static_cast<int64_t>(width) * static_cast<int64_t>(height) * static_cast<int64_t>(channels);
    if (expected_input_size <= 0 || expected_input_size > std::numeric_limits<jsize>::max()) {
        set_jni_last_error("Distributed processing input size overflow");
        return nullptr;
    }
    if (input_size != static_cast<jsize>(expected_input_size)) {
        set_jni_last_error("Distributed processing input size mismatch");
        return nullptr;
    }

    std::vector<uint8_t> input_bytes(static_cast<size_t>(input_size));
    env->GetByteArrayRegion(
        input_image,
        0,
        input_size,
        reinterpret_cast<jbyte*>(input_bytes.data())
    );

    // Decode peer identity pubkey (base64 from mDNS TXT or QR).
    auto peer_pk_bytes = crossscale::base64_decode(peer_pubkey_b64_str);
    if (!peer_pk_bytes || peer_pk_bytes->size() != 32) {
        set_jni_last_error("invalid peer identity pubkey (expected 32-byte base64)");
        return nullptr;
    }
    std::array<uint8_t, 32> peer_pk{};
    std::memcpy(peer_pk.data(), peer_pk_bytes->data(), 32);
    if (!g_identity.has_value()) {
        set_jni_last_error("identity not loaded; call loadIdentity first");
        return nullptr;
    }

    crossscale::TCPSocket socket;
    const auto connect_result = socket.connect(host_str, static_cast<uint16_t>(port));
    if (!connect_result.has_value()) {
        set_jni_last_error("Distributed connect failed: " + connect_result.error().message);
        return nullptr;
    }

    std::string handshake_error;
    auto session = run_client_handshake(socket, peer_pk, *g_identity, handshake_error);
    if (!session.has_value()) {
        set_jni_last_error("Distributed handshake failed: " + handshake_error);
        return nullptr;
    }

    // 握手成功后立即进行 CONNECT_REQUEST/RESPONSE 配对。PC 协调器在握手后会读取
    // 下一个加密帧并预期它是 CONNECT_REQUEST(可信 peer 自动批准, 首次 peer 阻塞
    // 在 stdin [y/n])。批准后 PC 才接受角色帧(MSG_CLIENT_SUBMIT)。
    const std::string own_fp = crossscale::fingerprint(g_identity->public_key());
    std::string pairing_error;
    if (!perform_connect_request(
            socket, *session, "CrossScale-Android", own_fp,
            wants_encrypted_stream == JNI_TRUE, pairing_error)) {
        set_jni_last_error("Distributed pairing failed: " + pairing_error);
        return nullptr;
    }

    // Pack ClientTaskSubmit struct + image bytes contiguously into one
    // plaintext payload, then seal into a single encrypted frame. The
    // coordinator unpacks the same layout (see coordinator.cpp ~line 392).
    crossscale::ClientTaskSubmit submit_request{};
    submit_request.image_width = static_cast<uint32_t>(width);
    submit_request.image_height = static_cast<uint32_t>(height);
    submit_request.channels = static_cast<uint8_t>(channels);
    std::strncpy(submit_request.model_name, model_str.c_str(), sizeof(submit_request.model_name) - 1);
    submit_request.tile_size = static_cast<uint32_t>(std::max(tile_size, 16));
    submit_request.tile_overlap = static_cast<uint32_t>(std::max(tile_overlap, 0));
    submit_request.image_data_length = static_cast<uint32_t>(input_bytes.size());
    std::strncpy(submit_request.output_format, output_format_str.c_str(),
                 sizeof(submit_request.output_format) - 1);
    submit_request.output_quality = static_cast<uint8_t>(normalized_quality);

    std::vector<uint8_t> plaintext(sizeof(submit_request) + input_bytes.size());
    std::memcpy(plaintext.data(), &submit_request, sizeof(submit_request));
    std::memcpy(plaintext.data() + sizeof(submit_request), input_bytes.data(), input_bytes.size());

    crossscale::MessageHeader submit_header{};
    submit_header.type = crossscale::MessageType::MSG_CLIENT_SUBMIT;
    submit_header.payload_length = static_cast<uint32_t>(plaintext.size());
    auto submit_wire = session->seal(submit_header, plaintext.data());
    if (!socket.send_all(submit_wire.data(), submit_wire.size()).has_value()) {
        set_jni_last_error("Distributed submit send failed");
        return nullptr;
    }

    // Receive the encrypted ClientTaskResponse frame: header + nonce+ct, then
    // open() to recover the plaintext header + payload. The payload layout is
    // [ClientTaskResponse struct][result image bytes] contiguous (coordinator.cpp ~467).
    std::vector<uint8_t> result_hdr_buf(crossscale::Protocol::HEADER_SIZE);
    if (!socket.receive_all(result_hdr_buf.data(), result_hdr_buf.size()).has_value()) {
        set_jni_last_error("Distributed response header receive failed");
        return nullptr;
    }
    auto result_hdr = crossscale::Protocol::parse_header(result_hdr_buf.data(), result_hdr_buf.size());
    if (!result_hdr) {
        set_jni_last_error("Distributed response header type is invalid");
        return nullptr;
    }
    if (result_hdr->payload_length > MAX_PAYLOAD_LENGTH) {
        set_jni_last_error("Distributed response payload too large: " +
                           std::to_string(result_hdr->payload_length) + " bytes");
        return nullptr;
    }
    std::vector<uint8_t> result_wire(crossscale::Protocol::HEADER_SIZE + result_hdr->payload_length);
    std::memcpy(result_wire.data(), result_hdr_buf.data(), crossscale::Protocol::HEADER_SIZE);
    if (result_hdr->payload_length > 0) {
        if (!socket.receive_all(result_wire.data() + crossscale::Protocol::HEADER_SIZE,
                                result_hdr->payload_length).has_value()) {
            set_jni_last_error("Distributed response payload receive failed");
            return nullptr;
        }
    }

    auto opened = session->open(result_wire.data(), result_wire.size());
    if (!opened) {
        set_jni_last_error("Distributed response MAC failed");
        return nullptr;
    }
    if (opened->first.type != crossscale::MessageType::MSG_CLIENT_RESPONSE) {
        set_jni_last_error("Distributed response header type is invalid");
        return nullptr;
    }
    const auto& result_payload = opened->second;
    if (result_payload.size() < sizeof(crossscale::ClientTaskResponse)) {
        set_jni_last_error("Distributed response payload is truncated");
        return nullptr;
    }

    const auto* task_response = reinterpret_cast<const crossscale::ClientTaskResponse*>(result_payload.data());
    if (task_response->success == 0) {
        set_jni_last_error("Distributed task failed: " + std::string(task_response->error_message));
        return nullptr;
    }

    // Security fix: Validate result data length before use to prevent OOM.
    if (task_response->result_data_length > MAX_PAYLOAD_LENGTH) {
        set_jni_last_error("Distributed result data too large: " +
                           std::to_string(task_response->result_data_length) + " bytes");
        return nullptr;
    }
    // The result image bytes follow the ClientTaskResponse struct in result_payload.
    const size_t resp_hdr_size = sizeof(crossscale::ClientTaskResponse);
    if (resp_hdr_size + task_response->result_data_length > result_payload.size()) {
        set_jni_last_error("Distributed result payload truncated");
        return nullptr;
    }
    // The PC encodes the upscaled result in the requested format before
    // transport (raw RGB doesn't scale — a camera photo at x2 is hundreds of
    // MB). Forward the encoded bytes straight to Kotlin; its
    // decodeOutputBounds()/writeOutputBytes() handle them.
    const size_t result_len = task_response->result_data_length;
    jbyteArray encoded_array = env->NewByteArray(static_cast<jsize>(result_len));
    if (encoded_array == nullptr) {
        set_jni_last_error("Distributed output allocation failed");
        return nullptr;
    }
    env->SetByteArrayRegion(
        encoded_array, 0, static_cast<jsize>(result_len),
        reinterpret_cast<const jbyte*>(result_payload.data() + resp_hdr_size));

    set_jni_last_error("");
    return encoded_array;
}

JNIEXPORT jint JNICALL
Java_com_crossscale_native_NCNNEngine_nativeGetScaleFactor(
    JNIEnv* env,
    jobject thiz,
    jlong engine_ptr
) {
    (void)env;
    (void)thiz;
    if (engine_ptr == 0) return 2;
    
    auto* engine = reinterpret_cast<Engine*>(engine_ptr);
    return engine->get_scale_factor();
}

JNIEXPORT jobjectArray JNICALL
Java_com_crossscale_native_NCNNEngine_nativeGetDevices(
    JNIEnv* env,
    jclass clazz
) {
    (void)clazz;
    auto engine = create_ncnn_engine();
    if (!engine) {
        return nullptr;
    }
    
    auto devices = engine->list_devices();
    
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray result = env->NewObjectArray(static_cast<jsize>(devices.size()), stringClass, nullptr);
    
    for (size_t i = 0; i < devices.size(); ++i) {
        std::string device_str = devices[i].name + " (" + 
                                 (devices[i].type == DeviceType::GPU_VULKAN ? "GPU" : "CPU") + ")";
        env->SetObjectArrayElement(result, static_cast<jsize>(i), env->NewStringUTF(device_str.c_str()));
    }
    
    return result;
}

JNIEXPORT void JNICALL
Java_com_crossscale_native_NCNNEngine_nativeRelease(
    JNIEnv* env,
    jobject thiz,
    jlong engine_ptr
) {
    (void)env;
    (void)thiz;
    if (engine_ptr == 0) return;
    
    auto* engine = reinterpret_cast<Engine*>(engine_ptr);
    delete engine;
    LOGI("Engine released");
}

JNIEXPORT jstring JNICALL
Java_com_crossscale_native_NativeWorker_nativeRequestConnection(
    JNIEnv* env,
    jobject thiz,
    jstring host,
    jint port,
    jstring peer_pubkey_b64,
    jboolean wants_encrypted_stream
) {
    (void)thiz;
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
        auto peer_pk_bytes = crossscale::base64_decode(peer_pubkey_b64_str);
        if (!peer_pk_bytes || peer_pk_bytes->size() != 32) {
            set_jni_last_error("invalid peer identity pubkey (expected 32-byte base64)");
            return nullptr;
        }
        std::memcpy(peer_pk.data(), peer_pk_bytes->data(), 32);
    }
    if (!g_identity.has_value()) {
        set_jni_last_error("identity not loaded; call loadIdentity first");
        return nullptr;
    }

    // 计算手机自身的指纹(从长期身份公钥派生), 供 PC 在批准提示中展示。
    const std::string own_fingerprint = crossscale::fingerprint(g_identity->public_key());

    crossscale::TCPSocket socket;
    const auto connect_result = socket.connect(host_str, static_cast<uint16_t>(port));
    if (!connect_result.has_value()) {
        set_jni_last_error("requestConnection connect failed: " + connect_result.error().message);
        return nullptr;
    }

    std::string handshake_error;
    auto session = run_client_handshake(
        socket, peer_pk, *g_identity, handshake_error,
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
        "CrossScale-Android",
        own_fingerprint,
        wants_encrypted_stream == JNI_TRUE,
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
        ? crossscale::base64_encode(learned_peer_pub.data(), learned_peer_pub.size())
        : peer_pubkey_b64_str;
    const auto& result_pubkey_arr = tofu_mode ? learned_peer_pub : peer_pk;

    LOGI("Pairing approved for %s:%d (tofu=%d, fp=%s)",
         host_str.c_str(), port, tofu_mode ? 1 : 0,
         crossscale::fingerprint(result_pubkey_arr).c_str());
    return env->NewStringUTF(result_pubkey_b64.c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_crossscale_native_NativeWorker_nativeLoadIdentity(
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
        g_identity.emplace(crossscale::IdentityKey::load_or_create(path_str));
        LOGI("Identity loaded from %s, fingerprint will be derived on connect", path_str.c_str());
        set_jni_last_error("");
        return JNI_TRUE;
    } catch (const std::exception& e) {
        set_jni_last_error(std::string("failed to load/create identity: ") + e.what());
        LOGE("Identity load failed: %s", e.what());
        return JNI_FALSE;
    }
}

}
