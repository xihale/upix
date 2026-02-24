#include <jni.h>
#include <string>
#include <android/log.h>
#include <algorithm>
#include <atomic>
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
#include "utils/image.hpp"

#define LOG_TAG "CrossScale"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

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
                break;
            }

            auto execution_result = worker_->execute_task(task_result.value(), *engine_);
            if (!execution_result.has_value()) {
                LOGE("Worker task execute failed: %s", execution_result.error().message.c_str());
                continue;
            }

            auto submit_result = worker_->submit_results(execution_result.value());
            if (!submit_result.has_value()) {
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
        set_jni_last_error("GPU initialization downgraded to CPU internally; aborting because CPU fallback is disabled");
        LOGE("GPU initialization downgraded to CPU internally; aborting because CPU fallback is disabled");
        return 0;
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
