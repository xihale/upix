#include <jni.h>
#include <string>
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <thread>
#include <vector>
#include <filesystem>
#include <chrono>

#include "core/config.hpp"
#include "core/engine.hpp"
#include "core/distributed/worker.hpp"
#include "protocol.hpp"
#include "utils/image.hpp"
#include "utils/network.hpp"

#define LOG_TAG "CrossScale"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
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

void clear_jni_last_error() {
    set_jni_last_error("");
}

auto get_jni_last_error() -> std::string {
    std::lock_guard<std::mutex> lock(g_jni_error_mutex);
    return g_jni_last_error;
}

auto infer_scale_factor_from_model_name(const std::string& model_name) -> int {
    if (model_name.empty()) {
        return 2;
    }

    auto extract_scale = [&](size_t marker_pos) -> int {
        if (marker_pos == std::string::npos || marker_pos + 2 >= model_name.size()) {
            return -1;
        }
        const char next_char = model_name[marker_pos + 2];
        if (!std::isdigit(static_cast<unsigned char>(next_char))) {
            return -1;
        }
        return std::max(1, next_char - '0');
    };

    const auto underscore_marker = model_name.find("_x");
    const int underscore_scale = extract_scale(underscore_marker);
    if (underscore_scale > 0) {
        return underscore_scale;
    }

    const auto dash_marker = model_name.find("-x");
    const int dash_scale = extract_scale(dash_marker);
    if (dash_scale > 0) {
        return dash_scale;
    }

    return 2;
}

class AndroidWorkerSession {
public:
    ~AndroidWorkerSession() {
        disconnect();
    }

    auto connect(
        const std::string& host,
        uint16_t port,
        const std::string& auth_key,
        const std::string& models_directory
    ) -> bool {
        if (host.empty() || port == 0 || auth_key.empty() || models_directory.empty()) {
            set_last_error("Invalid worker connection parameters");
            return false;
        }

        disconnect();

        worker_ = std::make_unique<crossscale::Worker>();
        engine_ = crossscale::create_ncnn_engine();
        if (!worker_ || !engine_) {
            set_last_error("Failed to create worker or engine");
            LOGE("Failed to create worker or engine");
            return false;
        }

        const auto gpu_result = engine_->initialize(crossscale::DeviceType::GPU_VULKAN);
        if (!gpu_result.has_value()) {
            const auto cpu_result = engine_->initialize(crossscale::DeviceType::CPU);
            if (!cpu_result.has_value()) {
                set_last_error(
                    "Engine init failed: gpu=" + gpu_result.error().message +
                    " cpu=" + cpu_result.error().message
                );
                LOGE("Engine init failed: gpu=%s cpu=%s",
                     gpu_result.error().message.c_str(),
                     cpu_result.error().message.c_str());
                return false;
            }
        }

        const auto connect_result = worker_->connect(host, port, auth_key);
        if (!connect_result.has_value()) {
            set_last_error("Worker connect failed: " + connect_result.error().message);
            LOGE("Worker connect failed: %s", connect_result.error().message.c_str());
            return false;
        }

        worker_->set_models_directory(std::filesystem::path(models_directory));
        const auto handshake_result = worker_->handshake(*engine_);
        if (!handshake_result.has_value()) {
            set_last_error("Worker handshake failed: " + handshake_result.error().message);
            LOGE("Worker handshake failed: %s", handshake_result.error().message.c_str());
            worker_->disconnect();
            return false;
        }

        const auto sync_result = worker_->sync_models();
        if (!sync_result.has_value()) {
            set_last_error("Worker model sync failed: " + sync_result.error().message);
            LOGE("Worker model sync failed: %s", sync_result.error().message.c_str());
            worker_->disconnect();
            return false;
        }

        set_last_error("");
        running_.store(true);
        worker_->start_heartbeat(std::chrono::seconds(5));
        task_loop_thread_ = std::thread([this]() { task_loop(); });
        return true;
    }

    void disconnect() {
        running_.store(false);
        if (worker_) {
            worker_->disconnect();
        }
        if (task_loop_thread_.joinable()) {
            task_loop_thread_.join();
        }
        engine_.reset();
        worker_.reset();
    }

    auto is_connected() const -> bool {
        return running_.load() && worker_ != nullptr && worker_->is_connected();
    }

    auto last_error() const -> std::string {
        std::lock_guard<std::mutex> lock(error_mutex_);
        return last_error_;
    }

private:
    void set_last_error(std::string message) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = std::move(message);
    }

    void task_loop() {
        while (running_.load() && worker_ && engine_ && worker_->is_connected()) {
            auto task_result = worker_->receive_task();
            if (!task_result.has_value()) {
                set_last_error("Worker receive task failed: " + task_result.error().message);
                LOGW("Worker receive task failed: %s", task_result.error().message.c_str());
                break;
            }

            auto execution_result = worker_->execute_task(task_result.value(), *engine_);
            if (!execution_result.has_value()) {
                LOGE("Worker task execute failed: %s", execution_result.error().message.c_str());
                continue;
            }

            auto submit_result = worker_->submit_results(execution_result.value());
            if (!submit_result.has_value()) {
                set_last_error("Worker submit failed: " + submit_result.error().message);
                LOGE("Worker submit result failed: %s", submit_result.error().message.c_str());
                break;
            }
        }
        running_.store(false);
        if (worker_) {
            worker_->disconnect();
        }
    }

    std::unique_ptr<crossscale::Worker> worker_;
    std::unique_ptr<crossscale::Engine> engine_;
    std::atomic<bool> running_{false};
    std::thread task_loop_thread_;
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

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

    clear_jni_last_error();
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

    clear_jni_last_error();
    
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
    jstring auth_key,
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
    if (host == nullptr || auth_key == nullptr || model_name == nullptr || input_image == nullptr) {
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
    const char* key_chars = env->GetStringUTFChars(auth_key, nullptr);
    const char* model_chars = env->GetStringUTFChars(model_name, nullptr);
    const char* format_chars = output_format == nullptr ? nullptr : env->GetStringUTFChars(output_format, nullptr);

    if (host_chars == nullptr || key_chars == nullptr || model_chars == nullptr) {
        if (host_chars != nullptr) {
            env->ReleaseStringUTFChars(host, host_chars);
        }
        if (key_chars != nullptr) {
            env->ReleaseStringUTFChars(auth_key, key_chars);
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
    const std::string key_str(key_chars);
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
    env->ReleaseStringUTFChars(auth_key, key_chars);
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

    crossscale::TCPSocket socket;
    const auto connect_result = socket.connect(host_str, static_cast<uint16_t>(port));
    if (!connect_result.has_value()) {
        set_jni_last_error("Distributed connect failed: " + connect_result.error().message);
        return nullptr;
    }

    crossscale::AuthRequest auth_request{};
    std::strncpy(auth_request.auth_key, key_str.c_str(), sizeof(auth_request.auth_key) - 1);

    crossscale::MessageHeader auth_header{};
    auth_header.type = crossscale::MessageType::MSG_AUTH_REQUEST;
    auth_header.payload_length = sizeof(auth_request);
    auto auth_payload = crossscale::Protocol::serialize(auth_header, &auth_request);
    const auto auth_send_result = socket.send_all(auth_payload.data(), auth_payload.size());
    if (!auth_send_result.has_value()) {
        set_jni_last_error("Distributed auth send failed: " + auth_send_result.error().message);
        return nullptr;
    }

    std::vector<uint8_t> auth_response_header_buffer(crossscale::Protocol::HEADER_SIZE);
    auto auth_header_recv = socket.receive_all(auth_response_header_buffer.data(), auth_response_header_buffer.size());
    if (!auth_header_recv.has_value()) {
        set_jni_last_error("Distributed auth header receive failed: " + auth_header_recv.error().message);
        return nullptr;
    }

    const auto auth_response_header =
        crossscale::Protocol::parse_header(auth_response_header_buffer.data(), auth_response_header_buffer.size());
    if (!auth_response_header || auth_response_header->type != crossscale::MessageType::MSG_AUTH_RESPONSE) {
        set_jni_last_error("Distributed auth response type is invalid");
        return nullptr;
    }

    // Security fix: Validate payload length before allocation to prevent OOM attacks.
    if (auth_response_header->payload_length > MAX_PAYLOAD_LENGTH) {
        set_jni_last_error("Distributed auth response payload too large: " +
                           std::to_string(auth_response_header->payload_length) + " bytes");
        return nullptr;
    }
    std::vector<uint8_t> auth_response_payload(auth_response_header->payload_length);
    auto auth_payload_recv = socket.receive_all(auth_response_payload.data(), auth_response_payload.size());
    if (!auth_payload_recv.has_value()) {
        set_jni_last_error("Distributed auth payload receive failed: " + auth_payload_recv.error().message);
        return nullptr;
    }
    if (auth_response_payload.size() < sizeof(crossscale::AuthResponse)) {
        set_jni_last_error("Distributed auth payload is truncated");
        return nullptr;
    }

    const auto* auth_response = reinterpret_cast<const crossscale::AuthResponse*>(auth_response_payload.data());
    if (auth_response->authenticated == 0) {
        set_jni_last_error("Distributed authentication rejected by coordinator");
        return nullptr;
    }

    crossscale::ClientTaskSubmit submit_request{};
    submit_request.image_width = static_cast<uint32_t>(width);
    submit_request.image_height = static_cast<uint32_t>(height);
    submit_request.channels = static_cast<uint8_t>(channels);
    std::strncpy(submit_request.model_name, model_str.c_str(), sizeof(submit_request.model_name) - 1);
    submit_request.tile_size = static_cast<uint32_t>(std::max(tile_size, 16));
    submit_request.tile_overlap = static_cast<uint32_t>(std::max(tile_overlap, 0));
    submit_request.image_data_length = static_cast<uint32_t>(input_bytes.size());

    crossscale::MessageHeader submit_header{};
    submit_header.type = crossscale::MessageType::MSG_CLIENT_SUBMIT;
    submit_header.payload_length = sizeof(submit_request);
    auto submit_header_data = crossscale::Protocol::serialize(submit_header, &submit_request);
    auto submit_send_result = socket.send_all(submit_header_data.data(), submit_header_data.size());
    if (!submit_send_result.has_value()) {
        set_jni_last_error("Distributed submit header send failed: " + submit_send_result.error().message);
        return nullptr;
    }
    submit_send_result = socket.send_all(input_bytes.data(), input_bytes.size());
    if (!submit_send_result.has_value()) {
        set_jni_last_error("Distributed submit payload send failed: " + submit_send_result.error().message);
        return nullptr;
    }

    std::vector<uint8_t> result_header_buffer(crossscale::Protocol::HEADER_SIZE);
    auto result_header_recv = socket.receive_all(result_header_buffer.data(), result_header_buffer.size());
    if (!result_header_recv.has_value()) {
        set_jni_last_error("Distributed response header receive failed: " + result_header_recv.error().message);
        return nullptr;
    }

    const auto result_header =
        crossscale::Protocol::parse_header(result_header_buffer.data(), result_header_buffer.size());
    if (!result_header || result_header->type != crossscale::MessageType::MSG_CLIENT_RESPONSE) {
        set_jni_last_error("Distributed response header type is invalid");
        return nullptr;
    }

    // Security fix: Validate payload length before allocation to prevent OOM attacks.
    if (result_header->payload_length > MAX_PAYLOAD_LENGTH) {
        set_jni_last_error("Distributed response payload too large: " +
                           std::to_string(result_header->payload_length) + " bytes");
        return nullptr;
    }
    std::vector<uint8_t> result_payload(result_header->payload_length);
    auto result_payload_recv = socket.receive_all(result_payload.data(), result_payload.size());
    if (!result_payload_recv.has_value()) {
        set_jni_last_error("Distributed response payload receive failed: " + result_payload_recv.error().message);
        return nullptr;
    }
    if (result_payload.size() < sizeof(crossscale::ClientTaskResponse)) {
        set_jni_last_error("Distributed response payload is truncated");
        return nullptr;
    }

    const auto* task_response = reinterpret_cast<const crossscale::ClientTaskResponse*>(result_payload.data());
    if (task_response->success == 0) {
        set_jni_last_error("Distributed task failed: " + std::string(task_response->error_message));
        return nullptr;
    }

    // Security fix: Validate result data length before allocation to prevent OOM attacks.
    if (task_response->result_data_length > MAX_PAYLOAD_LENGTH) {
        set_jni_last_error("Distributed result data too large: " +
                           std::to_string(task_response->result_data_length) + " bytes");
        return nullptr;
    }
    std::vector<uint8_t> processed_raw(task_response->result_data_length);
    auto processed_raw_recv = socket.receive_all(processed_raw.data(), processed_raw.size());
    if (!processed_raw_recv.has_value()) {
        set_jni_last_error("Distributed result receive failed: " + processed_raw_recv.error().message);
        return nullptr;
    }

    const int scale_factor = infer_scale_factor_from_model_name(model_str);
    const int output_width = width * std::max(scale_factor, 1);
    const int output_height = height * std::max(scale_factor, 1);
    const int64_t expected_output_size =
        static_cast<int64_t>(output_width) * static_cast<int64_t>(output_height) * static_cast<int64_t>(channels);
    if (expected_output_size <= 0 ||
        expected_output_size != static_cast<int64_t>(processed_raw.size())) {
        set_jni_last_error("Distributed result size mismatch");
        return nullptr;
    }

    crossscale::Image output_image(output_width, output_height, channels);
    std::copy(processed_raw.begin(), processed_raw.end(), output_image.data());
    auto encoded_result = output_image.save_to_memory(output_format_str, normalized_quality);
    if (!encoded_result.has_value()) {
        set_jni_last_error("Distributed output encode failed: " + encoded_result.error().message);
        return nullptr;
    }

    const auto& encoded_bytes = encoded_result.value();
    jbyteArray encoded_array = env->NewByteArray(static_cast<jsize>(encoded_bytes.size()));
    if (encoded_array == nullptr) {
        set_jni_last_error("Distributed output allocation failed");
        return nullptr;
    }
    env->SetByteArrayRegion(
        encoded_array,
        0,
        static_cast<jsize>(encoded_bytes.size()),
        reinterpret_cast<const jbyte*>(encoded_bytes.data())
    );

    clear_jni_last_error();
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

JNIEXPORT jlong JNICALL
Java_com_crossscale_native_NativeWorker_nativeCreate(
    JNIEnv* env,
    jobject thiz
) {
    (void)env;
    (void)thiz;
    auto* worker = new AndroidWorkerSession();
    return reinterpret_cast<jlong>(worker);
}

JNIEXPORT jboolean JNICALL
Java_com_crossscale_native_NativeWorker_nativeConnect(
    JNIEnv* env,
    jobject thiz,
    jlong worker_ptr,
    jstring host,
    jint port,
    jstring auth_key,
    jstring models_directory
) {
    (void)thiz;
    if (worker_ptr == 0) return JNI_FALSE;

    if (host == nullptr || auth_key == nullptr || models_directory == nullptr) {
        return JNI_FALSE;
    }
    if (port <= 0 || port > std::numeric_limits<uint16_t>::max()) {
        return JNI_FALSE;
    }

    auto* worker = reinterpret_cast<AndroidWorkerSession*>(worker_ptr);

    const char* host_str = env->GetStringUTFChars(host, nullptr);
    const char* key_str = env->GetStringUTFChars(auth_key, nullptr);
    const char* models_dir_str = env->GetStringUTFChars(models_directory, nullptr);

    if (host_str == nullptr || key_str == nullptr || models_dir_str == nullptr) {
        if (host_str != nullptr) {
            env->ReleaseStringUTFChars(host, host_str);
        }
        if (key_str != nullptr) {
            env->ReleaseStringUTFChars(auth_key, key_str);
        }
        if (models_dir_str != nullptr) {
            env->ReleaseStringUTFChars(models_directory, models_dir_str);
        }
        return JNI_FALSE;
    }

    const bool result = worker->connect(
        host_str,
        static_cast<uint16_t>(port),
        key_str,
        models_dir_str
    );

    env->ReleaseStringUTFChars(host, host_str);
    env->ReleaseStringUTFChars(auth_key, key_str);
    env->ReleaseStringUTFChars(models_directory, models_dir_str);

    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_crossscale_native_NativeWorker_nativeDisconnect(
    JNIEnv* env,
    jobject thiz,
    jlong worker_ptr
) {
    (void)env;
    (void)thiz;
    if (worker_ptr == 0) return;
    
    auto* worker = reinterpret_cast<AndroidWorkerSession*>(worker_ptr);
    worker->disconnect();
}

JNIEXPORT jboolean JNICALL
Java_com_crossscale_native_NativeWorker_nativeIsConnected(
    JNIEnv* env,
    jobject thiz,
    jlong worker_ptr
) {
    (void)env;
    (void)thiz;
    if (worker_ptr == 0) {
        return JNI_FALSE;
    }

    auto* worker = reinterpret_cast<AndroidWorkerSession*>(worker_ptr);
    return worker->is_connected() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_crossscale_native_NativeWorker_nativeGetLastError(
    JNIEnv* env,
    jobject thiz,
    jlong worker_ptr
) {
    (void)thiz;
    if (worker_ptr == 0) {
        return env->NewStringUTF("");
    }

    auto* worker = reinterpret_cast<AndroidWorkerSession*>(worker_ptr);
    const std::string message = worker->last_error();
    return env->NewStringUTF(message.c_str());
}

JNIEXPORT void JNICALL
Java_com_crossscale_native_NativeWorker_nativeRelease(
    JNIEnv* env,
    jobject thiz,
    jlong worker_ptr
) {
    (void)env;
    (void)thiz;
    if (worker_ptr == 0) return;
    
    auto* worker = reinterpret_cast<AndroidWorkerSession*>(worker_ptr);
    delete worker;
    LOGI("Worker released");
}

}
