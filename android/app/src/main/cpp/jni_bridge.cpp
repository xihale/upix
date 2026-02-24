#include <jni.h>
#include <string>
#include <android/log.h>
#include <memory>
#include <vector>

#include "core/engine.hpp"

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
    jint channels
) {
    (void)thiz;
    if (engine_ptr == 0 || input_image == nullptr) return nullptr;
    
    auto* engine = reinterpret_cast<Engine*>(engine_ptr);
    
    jsize input_size = env->GetArrayLength(input_image);
    jbyte* input_data = env->GetByteArrayElements(input_image, nullptr);
    
    int scale_factor = engine->get_scale_factor();
    int output_width = width * scale_factor;
    int output_height = height * scale_factor;
    jsize output_size = output_width * output_height * channels;
    
    std::vector<float> input_float(input_size);
    for (jsize i = 0; i < input_size; ++i) {
        input_float[i] = static_cast<unsigned char>(input_data[i]) / 255.0f;
    }
    
    std::vector<float> output_float(output_size);
    
    auto result = engine->process(
        input_float.data(),
        output_float.data(),
        width,
        height,
        channels
    );
    
    env->ReleaseByteArrayElements(input_image, input_data, JNI_ABORT);
    
    if (!result.has_value()) {
        LOGE("Engine processing failed: %s", result.error().message.c_str());
        return nullptr;
    }
    
    jbyteArray output = env->NewByteArray(output_size);
    std::vector<jbyte> output_bytes(output_size);
    for (jsize i = 0; i < output_size; ++i) {
        float val = output_float[i] * 255.0f;
        output_bytes[i] = static_cast<jbyte>(std::max(0.0f, std::min(255.0f, val)));
    }
    
    env->SetByteArrayRegion(output, 0, output_size, output_bytes.data());
    
    return output;
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
