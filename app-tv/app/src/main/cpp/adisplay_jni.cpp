// ADisplay —— Kotlin 调 castcore 的 JNI 桥
//
// 这一层刻意做得很薄：只做三件事 —— 类型转换、字符串转换、把 C 回调转成
// Java 回调。协议、会话、播放状态全在 castcore 里，这里一行判断都不做，
// 免得同一套逻辑在 C++ 和 Kotlin 各维护一份。
//
// 两个 JNI 上必须小心的地方：
//   1. 回调来自 castcore 的工作线程，那些线程对 JVM 是「未附着」的。直接拿
//      JNIEnv 用会崩，必须先 AttachCurrentThread、用完 Detach。附着/解附着
//      有开销，所以只在真正要回调时才做，不做常驻附着。
//   2. 传给 C 的 jobject 必须转成全局引用（NewGlobalRef），否则 Java 侧一
//      GC 就悬空 —— 表现同样是随机闪退，极难复现。
#include <jni.h>

#include <adisplay/adisplay.h>

#include <android/log.h>

#include <cstring>
#include <mutex>
#include <string>

namespace {

constexpr const char* kLogTag = "ADisplayJNI";

JavaVM* g_vm = nullptr;

// 回调对象与它那几个方法。全部用 g_mutex 保护 —— setCallbacks 可能在任意
// 时刻被调用，而回调随时可能在别的线程上进来。
std::mutex g_mutex;
jobject g_callback = nullptr;
jmethodID g_on_state_changed = nullptr;
jmethodID g_on_log = nullptr;
jmethodID g_on_media_url = nullptr;
jmethodID g_on_playback_command = nullptr;
jmethodID g_on_session_closed = nullptr;
jmethodID g_on_session_opened = nullptr;
jmethodID g_on_mirror_frame = nullptr;
jmethodID g_on_mirror_audio_frame = nullptr;

// 取出 JNIEnv。需要附着时顺带告诉调用方，用完要解附着。
JNIEnv* acquire_env(bool* did_attach) {
    *did_attach = false;
    if (g_vm == nullptr) {
        return nullptr;
    }
    JNIEnv* env = nullptr;
    const jint status = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_OK) {
        return env;
    }
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        return nullptr;
    }
    *did_attach = true;
    return env;
}

void release_env(bool did_attach) {
    if (did_attach && g_vm != nullptr) {
        g_vm->DetachCurrentThread();
    }
}

// 回调里抛出的 Java 异常必须就地清掉：跨回 C 代码继续跑的话，下一次 JNI
// 调用会莫名其妙地失败，排查起来极其绕。这里统一打印后清除。
void swallow_exception(JNIEnv* env) {
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

void call_state_changed(void* /*user_data*/, int state) {
    jobject target = nullptr;
    jmethodID method = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        target = g_callback;
        method = g_on_state_changed;
    }
    if (target == nullptr || method == nullptr) {
        return;
    }
    bool attached = false;
    JNIEnv* env = acquire_env(&attached);
    if (env != nullptr) {
        env->CallVoidMethod(target, method, static_cast<jint>(state));
        swallow_exception(env);
    }
    release_env(attached);
}

void call_log(void* /*user_data*/, int level, const char* message) {
    jobject target = nullptr;
    jmethodID method = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        target = g_callback;
        method = g_on_log;
    }
    if (target == nullptr || method == nullptr) {
        return;
    }
    bool attached = false;
    JNIEnv* env = acquire_env(&attached);
    if (env != nullptr) {
        jstring text = env->NewStringUTF(message != nullptr ? message : "");
        env->CallVoidMethod(target, method, static_cast<jint>(level), text);
        swallow_exception(env);
        if (text != nullptr) {
            env->DeleteLocalRef(text);
        }
    }
    release_env(attached);
}

void call_media_url(void* /*user_data*/, uint32_t session_id, const char* url,
                    const char* mime_type) {
    jobject target = nullptr;
    jmethodID method = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        target = g_callback;
        method = g_on_media_url;
    }
    if (target == nullptr || method == nullptr) {
        return;
    }
    bool attached = false;
    JNIEnv* env = acquire_env(&attached);
    if (env != nullptr) {
        jstring text = env->NewStringUTF(url != nullptr ? url : "");
        env->CallVoidMethod(target, method, static_cast<jint>(session_id), text);
        swallow_exception(env);
        if (text != nullptr) {
            env->DeleteLocalRef(text);
        }
    }
    release_env(attached);
}

void call_playback_command(void* /*user_data*/, uint32_t session_id, int command,
                           int64_t value) {
    jobject target = nullptr;
    jmethodID method = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        target = g_callback;
        method = g_on_playback_command;
    }
    if (target == nullptr || method == nullptr) {
        return;
    }
    bool attached = false;
    JNIEnv* env = acquire_env(&attached);
    if (env != nullptr) {
        env->CallVoidMethod(target, method, static_cast<jint>(session_id),
                            static_cast<jint>(command), static_cast<jlong>(value));
        swallow_exception(env);
    }
    release_env(attached);
}

void call_session_closed(void* /*user_data*/, uint32_t session_id, int reason) {
    jobject target = nullptr;
    jmethodID method = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        target = g_callback;
        method = g_on_session_closed;
    }
    if (target == nullptr || method == nullptr) {
        return;
    }
    bool attached = false;
    JNIEnv* env = acquire_env(&attached);
    if (env != nullptr) {
        env->CallVoidMethod(target, method, static_cast<jint>(session_id),
                            static_cast<jint>(reason));
        swallow_exception(env);
    }
    release_env(attached);
}

void call_session_opened(void* /*user_data*/, uint32_t session_id, const AdPeerInfo* /*peer*/,
                         int stream_kind) {
    jobject target = nullptr;
    jmethodID method = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        target = g_callback;
        method = g_on_session_opened;
    }
    if (target == nullptr || method == nullptr) {
        return;
    }
    bool attached = false;
    JNIEnv* env = acquire_env(&attached);
    if (env != nullptr) {
        env->CallVoidMethod(target, method, static_cast<jint>(session_id),
                            static_cast<jint>(stream_kind));
        swallow_exception(env);
    }
    release_env(attached);
}

void call_mirror_frame(void* /*user_data*/, const AdMirrorFrame* frame) {
    if (frame == nullptr || frame->data == nullptr || frame->size <= 0) {
        return;
    }
    jobject target = nullptr;
    jmethodID method = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        target = g_callback;
        method = g_on_mirror_frame;
    }
    if (target == nullptr || method == nullptr) {
        return;
    }

    bool attached = false;
    JNIEnv* env = acquire_env(&attached);
    if (env == nullptr) {
        release_env(attached);
        return;
    }

    // 每次都要新建一个 byte[] 并拷一份：核心那个指针只在回调期间有效，而
    // Kotlin 侧要留存到解码器真正用它的那一刻。每秒几十次，代价可接受。
    jbyteArray data = env->NewByteArray(static_cast<jsize>(frame->size));
    if (data != nullptr) {
        env->SetByteArrayRegion(data, 0, static_cast<jsize>(frame->size),
                                reinterpret_cast<const jbyte*>(frame->data));
        env->CallVoidMethod(target, method, data,
                            static_cast<jint>(frame->is_h265),
                            static_cast<jint>(frame->width),
                            static_cast<jint>(frame->height),
                            static_cast<jlong>(frame->pts_us));
        swallow_exception(env);
        env->DeleteLocalRef(data);
    }
    release_env(attached);
}

void call_mirror_audio_frame(void* /*user_data*/, const AdMirrorAudioFrame* frame) {
    if (frame == nullptr || frame->data == nullptr || frame->size <= 0) {
        return;
    }
    jobject target = nullptr;
    jmethodID method = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        target = g_callback;
        method = g_on_mirror_audio_frame;
    }
    if (target == nullptr || method == nullptr) {
        return;
    }

    bool attached = false;
    JNIEnv* env = acquire_env(&attached);
    if (env == nullptr) {
        release_env(attached);
        return;
    }

    // 与镜像视频同理：核心那个指针只在回调期间有效，Kotlin 侧要留到解码器
    // 真正用它的那一刻，所以这里拷一份。
    jbyteArray data = env->NewByteArray(static_cast<jsize>(frame->size));
    if (data != nullptr) {
        env->SetByteArrayRegion(data, 0, static_cast<jsize>(frame->size),
                                reinterpret_cast<const jbyte*>(frame->data));
        env->CallVoidMethod(target, method, data,
                            static_cast<jint>(frame->sample_rate),
                            static_cast<jint>(frame->channels),
                            static_cast<jlong>(frame->pts_us));
        swallow_exception(env);
        env->DeleteLocalRef(data);
    }
    release_env(attached);
}

std::string from_java(JNIEnv* env, jstring text) {
    if (text == nullptr) {
        return std::string();
    }
    const char* chars = env->GetStringUTFChars(text, nullptr);
    if (chars == nullptr) {
        return std::string();
    }
    std::string result(chars);
    env->ReleaseStringUTFChars(text, chars);
    return result;
}

jstring to_java(JNIEnv* env, const std::string& text) {
    return env->NewStringUTF(text.c_str());
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* /*vm*/, void* /*reserved*/) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_vm = nullptr;
}

// ===========================================================================
// 引擎生命周期
// ===========================================================================

extern "C" JNIEXPORT jstring JNICALL
Java_com_adisplay_tv_AdDisplayNative_nativeVersion(JNIEnv* env, jobject /*thiz*/) {
    return to_java(env, ad_version_string());
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_adisplay_tv_AdDisplayNative_nativeCreate(JNIEnv* env, jobject /*thiz*/,
                                                  jstring device_name,
                                                  jstring log_file_path,
                                                  jstring config_file_path) {
    const std::string name = from_java(env, device_name);
    const std::string log_path = from_java(env, log_file_path);
    const std::string config_path = from_java(env, config_file_path);

    AdConfig config;
    if (ad_engine_get_default_config(&config) != AD_OK) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "读取默认配置失败");
        return 0;
    }

    // 空串表示「用核心的默认」。空字符串不能直接当 C 字符串传 —— 核心那边
    // 判的是指针是否为 NULL，给 "" 会变成一个长度为零的设备名。
    config.device_name = name.empty() ? nullptr : name.c_str();
    config.log_file_path = log_path.empty() ? nullptr : log_path.c_str();
    config.config_file_path = config_path.empty() ? nullptr : config_path.c_str();
    config.log_level = AD_LOG_INFO;
    // 电视端就在客厅，弹窗确认没人会去点，直接放行（文档 2.3）。
    config.require_confirmation = 0;

    AdEngine* engine = nullptr;
    const AdResult result = ad_engine_create(&config, &engine);
    if (result != AD_OK || engine == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "创建引擎失败：%d",
                            static_cast<int>(result));
        return 0;
    }
    return reinterpret_cast<jlong>(engine);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_adisplay_tv_AdDisplayNative_nativeSetCallbacks(JNIEnv* env, jobject /*thiz*/,
                                                        jlong handle, jobject callback) {
    AdEngine* engine = reinterpret_cast<AdEngine*>(handle);
    if (engine == nullptr) {
        return AD_ERR_INVALID_ARG;
    }

    jobject global = nullptr;
    jmethodID on_state_changed = nullptr;
    jmethodID on_log = nullptr;
    jmethodID on_media_url = nullptr;
    jmethodID on_playback_command = nullptr;
    jmethodID on_session_closed = nullptr;
    jmethodID on_session_opened = nullptr;
    jmethodID on_mirror_frame = nullptr;
    jmethodID on_mirror_audio_frame = nullptr;

    if (callback != nullptr) {
        jclass cls = env->GetObjectClass(callback);
        if (cls == nullptr) {
            return AD_ERR_INTERNAL;
        }
        on_state_changed = env->GetMethodID(cls, "onStateChanged", "(I)V");
        on_log = env->GetMethodID(cls, "onLog", "(ILjava/lang/String;)V");
        on_media_url = env->GetMethodID(cls, "onMediaUrl", "(ILjava/lang/String;)V");
        on_playback_command = env->GetMethodID(cls, "onPlaybackCommand", "(IIJ)V");
        on_session_closed = env->GetMethodID(cls, "onSessionClosed", "(II)V");
        on_session_opened = env->GetMethodID(cls, "onSessionOpened", "(II)V");
        on_mirror_frame = env->GetMethodID(cls, "onMirrorFrame", "([BIIIJ)V");
        on_mirror_audio_frame = env->GetMethodID(cls, "onMirrorAudioFrame", "([BIIJ)V");
        env->DeleteLocalRef(cls);
        if (on_state_changed == nullptr || on_log == nullptr || on_media_url == nullptr ||
            on_playback_command == nullptr || on_session_closed == nullptr) {
            // 走到这里说明 Kotlin 侧的接口签名和这里对不上。异常已经挂在
            // env 上，清掉再返回错误码，让 Java 侧看到的是一个干净的失败。
            swallow_exception(env);
            __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                                "AdCallback 的方法签名与 JNI 期望的不一致");
            return AD_ERR_INVALID_ARG;
        }
        global = env->NewGlobalRef(callback);
        if (global == nullptr) {
            return AD_ERR_INTERNAL;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_callback != nullptr) {
            env->DeleteGlobalRef(g_callback);
        }
        g_callback = global;
        g_on_state_changed = on_state_changed;
        g_on_log = on_log;
        g_on_media_url = on_media_url;
        g_on_playback_command = on_playback_command;
        g_on_session_closed = on_session_closed;
        g_on_session_opened = on_session_opened;
        g_on_mirror_frame = on_mirror_frame;
        g_on_mirror_audio_frame = on_mirror_audio_frame;
    }

    if (callback == nullptr) {
        return ad_engine_set_callbacks(engine, nullptr, nullptr);
    }

    AdCallbacks callbacks;
    std::memset(&callbacks, 0, sizeof(callbacks));
    callbacks.struct_size = static_cast<uint32_t>(sizeof(AdCallbacks));
    callbacks.on_state_changed = &call_state_changed;
    callbacks.on_log = &call_log;
    callbacks.on_media_url = &call_media_url;
    callbacks.on_playback_command = &call_playback_command;
    callbacks.on_session_closed = &call_session_closed;
    callbacks.on_session_opened = &call_session_opened;
    callbacks.on_mirror_frame = &call_mirror_frame;
    // 注册它等于告诉核心「伴音我自己解」—— 核心因此只转发压缩帧、不做解码，
    // 也就不必把 FFmpeg 链进 APK。
    callbacks.on_mirror_audio_frame = &call_mirror_audio_frame;

    return ad_engine_set_callbacks(engine, &callbacks, nullptr);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_adisplay_tv_AdDisplayNative_nativeStart(JNIEnv* /*env*/, jobject /*thiz*/,
                                                 jlong handle) {
    AdEngine* engine = reinterpret_cast<AdEngine*>(handle);
    if (engine == nullptr) {
        return AD_ERR_NOT_INITIALIZED;
    }
    return ad_engine_start(engine);
}

extern "C" JNIEXPORT void JNICALL
Java_com_adisplay_tv_AdDisplayNative_nativeStop(JNIEnv* /*env*/, jobject /*thiz*/,
                                                jlong handle) {
    AdEngine* engine = reinterpret_cast<AdEngine*>(handle);
    if (engine != nullptr) {
        ad_engine_stop(engine);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_adisplay_tv_AdDisplayNative_nativeDestroy(JNIEnv* env, jobject /*thiz*/,
                                                   jlong handle) {
    AdEngine* engine = reinterpret_cast<AdEngine*>(handle);
    if (engine != nullptr) {
        ad_engine_destroy(engine);
    }
    // 引擎没了，回调再进来就是野指针。顺手把全局引用放掉。
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_callback != nullptr) {
        env->DeleteGlobalRef(g_callback);
        g_callback = nullptr;
    }
}

// ===========================================================================
// 状态与信息
// ===========================================================================

extern "C" JNIEXPORT jint JNICALL
Java_com_adisplay_tv_AdDisplayNative_nativeGetState(JNIEnv* /*env*/, jobject /*thiz*/,
                                                    jlong handle) {
    AdEngine* engine = reinterpret_cast<AdEngine*>(handle);
    int state = AD_STATE_STOPPED;
    if (engine != nullptr && ad_engine_get_state(engine, &state) != AD_OK) {
        return AD_STATE_ERROR;
    }
    return state;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_adisplay_tv_AdDisplayNative_nativeLastError(JNIEnv* env, jobject /*thiz*/,
                                                     jlong handle) {
    AdEngine* engine = reinterpret_cast<AdEngine*>(handle);
    if (engine == nullptr) {
        return to_java(env, std::string());
    }
    char buffer[1024];
    std::size_t length = 0;
    const AdResult result =
        ad_engine_get_last_error(engine, buffer, sizeof(buffer), &length);
    if (result != AD_OK) {
        return to_java(env, std::string());
    }
    return to_java(env, std::string(buffer, length > 0 ? length - 1 : 0));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_adisplay_tv_AdDisplayNative_nativeDeviceId(JNIEnv* env, jobject /*thiz*/,
                                                    jlong handle) {
    AdEngine* engine = reinterpret_cast<AdEngine*>(handle);
    if (engine == nullptr) {
        return to_java(env, std::string());
    }
    char buffer[256];
    std::size_t length = 0;
    if (ad_engine_get_device_id(engine, buffer, sizeof(buffer), &length) != AD_OK) {
        return to_java(env, std::string());
    }
    return to_java(env, std::string(buffer));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_adisplay_tv_AdDisplayNative_nativeLocalAddresses(JNIEnv* env, jobject /*thiz*/,
                                                          jlong handle) {
    AdEngine* engine = reinterpret_cast<AdEngine*>(handle);
    if (engine == nullptr) {
        return to_java(env, std::string());
    }
    char buffer[512];
    std::size_t length = 0;
    if (ad_engine_get_local_addresses(engine, buffer, sizeof(buffer), &length) != AD_OK) {
        return to_java(env, std::string());
    }
    return to_java(env, std::string(buffer));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_adisplay_tv_AdDisplayNative_nativeGetDeviceName(JNIEnv* env, jobject /*thiz*/,
                                                         jlong handle) {
    AdEngine* engine = reinterpret_cast<AdEngine*>(handle);
    if (engine == nullptr) {
        return to_java(env, std::string());
    }
    char buffer[256];
    std::size_t length = 0;
    if (ad_engine_get_device_name(engine, buffer, sizeof(buffer), &length) != AD_OK) {
        return to_java(env, std::string());
    }
    return to_java(env, std::string(buffer));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_adisplay_tv_AdDisplayNative_nativeSetDeviceName(JNIEnv* env, jobject /*thiz*/,
                                                         jlong handle, jstring name) {
    AdEngine* engine = reinterpret_cast<AdEngine*>(handle);
    if (engine == nullptr) {
        return AD_ERR_NOT_INITIALIZED;
    }
    const std::string text = from_java(env, name);
    return ad_engine_set_device_name(engine, text.c_str());
}

// ===========================================================================
// 播放状态回报
// ===========================================================================

extern "C" JNIEXPORT jint JNICALL
Java_com_adisplay_tv_AdDisplayNative_nativeReportPlayback(
    JNIEnv* /*env*/, jobject /*thiz*/, jlong handle, jint session_id, jint transport_state,
    jlong position_ms, jlong duration_ms, jint volume, jint muted) {
    AdEngine* engine = reinterpret_cast<AdEngine*>(handle);
    if (engine == nullptr) {
        return AD_ERR_NOT_INITIALIZED;
    }
    AdPlaybackStatus status;
    std::memset(&status, 0, sizeof(status));
    status.struct_size = static_cast<uint32_t>(sizeof(AdPlaybackStatus));
    status.abi_version = AD_ABI_VERSION;
    status.session_id = static_cast<uint32_t>(session_id);
    status.transport_state = transport_state;
    status.position_ms = position_ms;
    status.duration_ms = duration_ms;
    status.volume = volume;
    status.muted = muted;
    return ad_engine_report_playback(engine, &status);
}
