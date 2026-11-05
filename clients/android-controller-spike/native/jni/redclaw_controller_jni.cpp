// S0.1 spike JNI bridge: exposes redclaw::controller_core::ControllerSession to
// Kotlin. Not a production API; see clients/android-controller-spike/README.md.
#include <jni.h>

#include <android/log.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "redclaw/controller_core/controller_session.h"

namespace {

constexpr const char* kLogTag = "RedClawControllerCore";

JavaVM* g_vm = nullptr;

// Native callback threads (libdatachannel, session worker) attach lazily and
// detach when the thread exits.
class ScopedEnv final {
public:
    ScopedEnv() {
        if (g_vm == nullptr) return;
        if (g_vm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) == JNI_EDETACHED) {
            if (g_vm->AttachCurrentThread(&env_, nullptr) != JNI_OK) {
                env_ = nullptr;
                return;
            }
            thread_local struct Detach {
                ~Detach() { if (g_vm != nullptr) g_vm->DetachCurrentThread(); }
            } detach;
        }
    }
    [[nodiscard]] JNIEnv* get() const { return env_; }

private:
    JNIEnv* env_ = nullptr;
};

std::string to_string(JNIEnv* env, jstring value) {
    if (value == nullptr) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars != nullptr ? chars : "";
    if (chars != nullptr) env->ReleaseStringUTFChars(value, chars);
    return out;
}

std::vector<std::string> to_strings(JNIEnv* env, jobjectArray values) {
    std::vector<std::string> out;
    if (values == nullptr) return out;
    const jsize count = env->GetArrayLength(values);
    for (jsize i = 0; i < count; ++i) {
        auto item = static_cast<jstring>(env->GetObjectArrayElement(values, i));
        auto text = to_string(env, item);
        env->DeleteLocalRef(item);
        if (!text.empty()) out.push_back(std::move(text));
    }
    return out;
}

class JniObserver final : public redclaw::controller_core::IControllerSessionObserver {
public:
    JniObserver(JNIEnv* env, jobject listener) : listener_(env->NewGlobalRef(listener)) {
        jclass type = env->GetObjectClass(listener);
        on_phase_ = env->GetMethodID(type, "onPhase", "(Ljava/lang/String;Ljava/lang/String;)V");
        on_frame_ = env->GetMethodID(type, "onEncodedFrame", "(IIIZJJLjava/nio/ByteBuffer;)V");
        on_log_ = env->GetMethodID(type, "onLog", "(Ljava/lang/String;)V");
        env->DeleteLocalRef(type);
    }
    ~JniObserver() override {
        ScopedEnv env;
        if (env.get() != nullptr) env.get()->DeleteGlobalRef(listener_);
    }

    void on_phase(redclaw::controller_core::ControllerSessionPhase phase, std::string_view detail) override {
        ScopedEnv env;
        if (env.get() == nullptr) return;
        const auto name = redclaw::controller_core::controller_session_phase_to_string(phase);
        jstring jphase = env.get()->NewStringUTF(std::string(name).c_str());
        jstring jdetail = env.get()->NewStringUTF(std::string(detail).c_str());
        env.get()->CallVoidMethod(listener_, on_phase_, jphase, jdetail);
        clear_exception(env.get());
        env.get()->DeleteLocalRef(jphase);
        env.get()->DeleteLocalRef(jdetail);
    }

    // The ByteBuffer aliases native memory that is only valid for the duration
    // of this call; Kotlin copies it straight into a MediaCodec input buffer.
    void on_encoded_frame(redclaw::net::ReassembledEncodedVideoFrame&& frame) override {
        ScopedEnv env;
        if (env.get() == nullptr || frame.payload.empty()) return;
        jobject buffer = env.get()->NewDirectByteBuffer(
            frame.payload.data(), static_cast<jlong>(frame.payload.size()));
        if (buffer == nullptr) {
            clear_exception(env.get());
            return;
        }
        env.get()->CallVoidMethod(listener_, on_frame_,
            static_cast<jint>(frame.codec),
            static_cast<jint>(frame.width),
            static_cast<jint>(frame.height),
            static_cast<jboolean>(frame.keyframe ? JNI_TRUE : JNI_FALSE),
            static_cast<jlong>(frame.timestamp_ms),
            static_cast<jlong>(frame.frame_id),
            buffer);
        clear_exception(env.get());
        env.get()->DeleteLocalRef(buffer);
    }

    void on_log(std::string_view line) override {
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "%.*s", static_cast<int>(line.size()), line.data());
        ScopedEnv env;
        if (env.get() == nullptr) return;
        jstring jline = env.get()->NewStringUTF(std::string(line).c_str());
        env.get()->CallVoidMethod(listener_, on_log_, jline);
        clear_exception(env.get());
        env.get()->DeleteLocalRef(jline);
    }

private:
    static void clear_exception(JNIEnv* env) {
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    }

    jobject listener_ = nullptr;
    jmethodID on_phase_ = nullptr;
    jmethodID on_frame_ = nullptr;
    jmethodID on_log_ = nullptr;
};

struct NativeSession {
    std::unique_ptr<JniObserver> observer;
    std::unique_ptr<redclaw::controller_core::ControllerSession> session;
};

NativeSession* from_handle(jlong handle) {
    return reinterpret_cast<NativeSession*>(handle);
}

}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL
Java_com_redclaw_controllerspike_NativeControllerCore_apiVersion(JNIEnv*, jclass) {
    return static_cast<jint>(redclaw::controller_core::kControllerCoreApiVersion);
}

JNIEXPORT jlong JNICALL
Java_com_redclaw_controllerspike_NativeControllerCore_create(JNIEnv* env, jclass, jobject listener) {
    auto native = std::make_unique<NativeSession>();
    native->observer = std::make_unique<JniObserver>(env, listener);
    native->session = std::make_unique<redclaw::controller_core::ControllerSession>(*native->observer);
    return reinterpret_cast<jlong>(native.release());
}

// Returns null on success or a redacted error string.
JNIEXPORT jstring JNICALL
Java_com_redclaw_controllerspike_NativeControllerCore_start(
    JNIEnv* env, jclass, jlong handle,
    jstring machine_code, jbyteArray password_utf8, jstring mailbox_directory,
    jobjectArray ice_servers, jobjectArray dht_bootstrap_nodes,
    jint ice_udp_port, jint viewport_width, jint viewport_height) {
    auto* native = from_handle(handle);
    if (native == nullptr) return env->NewStringUTF("invalid native handle");

    redclaw::controller_core::ControllerSessionConfig config;
    config.machine_code = to_string(env, machine_code);
    if (password_utf8 != nullptr) {
        const jsize size = env->GetArrayLength(password_utf8);
        config.password.resize(static_cast<std::size_t>(size));
        env->GetByteArrayRegion(password_utf8, 0, size, reinterpret_cast<jbyte*>(config.password.data()));
    }
    config.mailbox_directory = to_string(env, mailbox_directory);
    config.ice_servers = to_strings(env, ice_servers);
    config.dht_bootstrap_nodes = to_strings(env, dht_bootstrap_nodes);
    config.ice_udp_port = static_cast<std::uint16_t>(ice_udp_port);
    config.viewport_width = static_cast<std::uint32_t>(viewport_width);
    config.viewport_height = static_cast<std::uint32_t>(viewport_height);

    std::string error;
    if (!native->session->start(std::move(config), &error)) {
        return env->NewStringUTF(error.c_str());
    }
    return nullptr;
}

JNIEXPORT void JNICALL
Java_com_redclaw_controllerspike_NativeControllerCore_stop(JNIEnv*, jclass, jlong handle) {
    if (auto* native = from_handle(handle)) native->session->stop();
}

JNIEXPORT void JNICALL
Java_com_redclaw_controllerspike_NativeControllerCore_destroy(JNIEnv*, jclass, jlong handle) {
    std::unique_ptr<NativeSession> native(from_handle(handle));
    if (native) native->session->stop();
}

JNIEXPORT jboolean JNICALL
Java_com_redclaw_controllerspike_NativeControllerCore_setViewport(
    JNIEnv*, jclass, jlong handle, jint width, jint height) {
    auto* native = from_handle(handle);
    return native != nullptr
        && native->session->set_viewport(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
}

JNIEXPORT jboolean JNICALL
Java_com_redclaw_controllerspike_NativeControllerCore_requestKeyframe(
    JNIEnv* env, jclass, jlong handle, jstring reason) {
    auto* native = from_handle(handle);
    return native != nullptr && native->session->request_keyframe(to_string(env, reason));
}

JNIEXPORT jstring JNICALL
Java_com_redclaw_controllerspike_NativeControllerCore_stats(JNIEnv* env, jclass, jlong handle) {
    auto* native = from_handle(handle);
    if (native == nullptr) return env->NewStringUTF("");
    const auto s = native->session->stats();
    const auto phase = redclaw::controller_core::controller_session_phase_to_string(native->session->phase());
    const std::string text = "phase=" + std::string(phase)
        + " dht_publish=" + std::to_string(s.dht_publish_success)
        + " dht_fetch=" + std::to_string(s.dht_fetch_hits) + "/" + std::to_string(s.dht_fetch_attempts)
        + " remote_revision=" + std::to_string(s.dht_remote_revision)
        + " candidates=" + std::to_string(s.local_candidates) + "/" + std::to_string(s.remote_candidates_applied)
        + " fragments=" + std::to_string(s.media_fragments)
        + " frames=" + std::to_string(s.frames_reassembled)
        + " keyframes=" + std::to_string(s.keyframes_reassembled)
        + " dropped=" + std::to_string(s.frames_dropped)
        + " kf_requests=" + std::to_string(s.keyframe_requests_sent)
        + " codec=" + std::to_string(s.latest_codec)
        + " size=" + std::to_string(s.latest_width) + "x" + std::to_string(s.latest_height);
    return env->NewStringUTF(text.c_str());
}

}  // extern "C"
