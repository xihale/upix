#include <jni.h>
#include <string>
#include <android/log.h>
#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <vector>

#include "core/config.hpp"
#include "core/engine.hpp"
#include "utils/image.hpp"

#define LOG_TAG "CrossScale"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace crossscale;

namespace {

class StubWorker {
public:
    auto connect(const std::string& host, uint16_t port, const std::string& auth_key) -> bool {
        if (host.empty() || port == 0 || auth_key.empty()) {
            return false;
        }
        connected_ = true;
        return true;
    }

    void disconnect() {
        connected_ = false;
    }

private:
    bool connected_ = false;
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
        LOGE("Failed to create engine");
        return 0;
    }

    DeviceType preferred = (device_type == 1) ? DeviceType::GPU_VULKAN : DeviceType::CPU;
    auto result = engine->initialize(preferred);
    
    if (!result.has_value()) {
        LOGE("Failed to initialize engine: %s", result.error().message.c_str());
        if (preferred == DeviceType::GPU_VULKAN) {
            LOGI("Falling back to CPU");
            result = engine->initialize(DeviceType::CPU);
            if (!result.has_value()) {
                LOGE("Failed to initialize CPU engine: %s", result.error().message.c_str());
                return 0;
            }
        } else {
            return 0;
        }
    }

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
    if (engine_ptr == 0) return JNI_FALSE;
    
    auto* engine = reinterpret_cast<Engine*>(engine_ptr);
    const char* path = env->GetStringUTFChars(model_path, nullptr);
    
    std::string param_path = std::string(path) + ".param";
    std::string bin_path = std::string(path) + ".bin";
    
    LOGI("Loading model: %s", path);
    
    auto result = engine->load_model(param_path, bin_path);
    
    env->ReleaseStringUTFChars(model_path, path);
    
    return result.has_value() ? JNI_TRUE : JNI_FALSE;
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
    auto* worker = new StubWorker();
    return reinterpret_cast<jlong>(worker);
}

JNIEXPORT jboolean JNICALL
Java_com_crossscale_native_NativeWorker_nativeConnect(
    JNIEnv* env,
    jobject thiz,
    jlong worker_ptr,
    jstring host,
    jint port,
    jstring auth_key
) {
    (void)thiz;
    if (worker_ptr == 0) return JNI_FALSE;
    
    auto* worker = reinterpret_cast<StubWorker*>(worker_ptr);
    
    const char* host_str = env->GetStringUTFChars(host, nullptr);
    const char* key_str = env->GetStringUTFChars(auth_key, nullptr);
    
    const bool result = worker->connect(host_str, static_cast<uint16_t>(port), key_str);
    
    env->ReleaseStringUTFChars(host, host_str);
    env->ReleaseStringUTFChars(auth_key, key_str);
    
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
    
    auto* worker = reinterpret_cast<StubWorker*>(worker_ptr);
    worker->disconnect();
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
    
    auto* worker = reinterpret_cast<StubWorker*>(worker_ptr);
    delete worker;
    LOGI("Worker released");
}

}
