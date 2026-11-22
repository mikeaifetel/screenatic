// JNI glue between app.screenatic.airplay.AirPlayEngine and the native AirPlay server.

#include <jni.h>
#include <android/native_window_jni.h>

#include <memory>
#include <string>

#include "airplay_server.h"
#include "audio_player.h"
#include "video_decoder.h"

using namespace screenatic;

namespace {

JavaVM* gVm = nullptr;
jstring newUtf(JNIEnv* env, const std::string& s);

/** Attaches the current (protocol) thread to the JVM for the duration of a callback. */
class ScopedEnv {
public:
    ScopedEnv() {
        if (gVm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) == JNI_EDETACHED) {
            gVm->AttachCurrentThread(&env_, nullptr);
            attached_ = true;
        }
    }
    ~ScopedEnv() { if (attached_) gVm->DetachCurrentThread(); }
    JNIEnv* operator->() const { return env_; }
    JNIEnv* get() const { return env_; }
private:
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};

struct Engine {
    jobject javaEngine = nullptr;  // global ref to AirPlayEngine
    jmethodID onConnected = nullptr;
    jmethodID onPlayUrl = nullptr;
    jmethodID onDisconnected = nullptr;
    jmethodID onVideoSize = nullptr;
    jmethodID onPlaybackCommand = nullptr;
    jmethodID onTrackInfo = nullptr;
    jmethodID onCoverArt = nullptr;
    jmethodID onTrackProgress = nullptr;
    jmethodID onAudioPlaying = nullptr;
    jmethodID onVolume = nullptr;
    jmethodID onLog = nullptr;
    VideoDecoder decoder;
    AudioPlayer audio;
    std::unique_ptr<AirPlayServer> server;
};

Engine* fromHandle(jlong h) { return reinterpret_cast<Engine*>(h); }

jobjectArray toJavaArray(JNIEnv* env, const std::map<std::string, std::string>& kv) {
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray(static_cast<jsize>(kv.size()), stringClass, nullptr);
    jsize i = 0;
    for (const auto& [k, v] : kv) {
        std::string entry = k + "=" + v;
        jstring s = newUtf(env, entry);
        env->SetObjectArrayElement(arr, i++, s);
        env->DeleteLocalRef(s);
    }
    return arr;
}

/**
 * DMAP strings are UTF-8 from the sender, but JNI's NewStringUTF wants Modified UTF-8 and aborts
 * on 4-byte sequences (emoji in song titles). Build the String from bytes via the String(byte[], "UTF-8")
 * constructor instead.
 */
jstring newUtf(JNIEnv* env, const std::string& s) {
    jbyteArray bytes = env->NewByteArray(static_cast<jsize>(s.size()));
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(s.size()), reinterpret_cast<const jbyte*>(s.data()));
    jclass strClass = env->FindClass("java/lang/String");
    jmethodID ctor = env->GetMethodID(strClass, "<init>", "([BLjava/lang/String;)V");
    jstring charset = env->NewStringUTF("UTF-8");
    auto* str = static_cast<jstring>(env->NewObject(strClass, ctor, bytes, charset));
    env->DeleteLocalRef(charset);
    env->DeleteLocalRef(bytes);
    env->DeleteLocalRef(strClass);
    return str;
}

void callWithString(Engine* e, jmethodID m, const std::string& s) {
    ScopedEnv env;
    jstring js = newUtf(env.get(), s);
    env->CallVoidMethod(e->javaEngine, m, js);
    env->DeleteLocalRef(js);
}

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    gVm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jlong JNICALL
Java_app_screenatic_airplay_AirPlayEngine_nativeCreate(JNIEnv* env, jobject thiz, jstring jName, jstring jId,
                                                      jstring jKeyFile, jboolean h265, jfloat volumeDb) {
    auto* e = new Engine();
    e->javaEngine = env->NewGlobalRef(thiz);
    jclass cls = env->GetObjectClass(thiz);
    e->onConnected = env->GetMethodID(cls, "onClientConnected", "(Ljava/lang/String;I)V");
    e->onPlayUrl = env->GetMethodID(cls, "onPlayUrl", "(Ljava/lang/String;Ljava/lang/String;J)V");
    e->onDisconnected = env->GetMethodID(cls, "onClientDisconnected", "()V");
    e->onVideoSize = env->GetMethodID(cls, "onVideoSize", "(II)V");
    e->onPlaybackCommand = env->GetMethodID(cls, "onPlaybackCommand", "(ID)V");
    e->onTrackInfo = env->GetMethodID(cls, "onTrackInfo", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    e->onCoverArt = env->GetMethodID(cls, "onCoverArt", "([B)V");
    e->onTrackProgress = env->GetMethodID(cls, "onTrackProgress", "(DD)V");
    e->onAudioPlaying = env->GetMethodID(cls, "onAudioPlaying", "(Z)V");
    e->onVolume = env->GetMethodID(cls, "onVolume", "(F)V");
    e->onLog = env->GetMethodID(cls, "onNativeLog", "(Ljava/lang/String;)V");

    const char* name = env->GetStringUTFChars(jName, nullptr);
    const char* id = env->GetStringUTFChars(jId, nullptr);
    const char* keyFile = env->GetStringUTFChars(jKeyFile, nullptr);

    AirPlayCallbacks cb;
    cb.onConnected = [e](const std::string& client, ClientKind kind) {
        ScopedEnv env;
        jstring js = newUtf(env.get(), client);
        env->CallVoidMethod(e->javaEngine, e->onConnected, js, static_cast<jint>(kind));
        env->DeleteLocalRef(js);
    };
    cb.onPlayUrl = [e](const std::string& client, const std::string& url, int64_t startMs) {
        ScopedEnv env;
        jstring jc = newUtf(env.get(), client);
        jstring ju = newUtf(env.get(), url);
        env->CallVoidMethod(e->javaEngine, e->onPlayUrl, jc, ju, static_cast<jlong>(startMs));
        env->DeleteLocalRef(jc);
        env->DeleteLocalRef(ju);
    };
    cb.onDisconnected = [e]() {
        ScopedEnv env;
        env->CallVoidMethod(e->javaEngine, e->onDisconnected);
    };
    cb.onVideo = [e](const uint8_t* data, size_t size, int64_t ptsUs, bool h265) {
        e->decoder.submit(data, size, ptsUs, h265 ? VideoDecoder::Codec::H265 : VideoDecoder::Codec::H264);
    };
    cb.onVideoReset = [e]() { e->decoder.reset(); };
    // Picture size comes from the decoder's output format (crop applied); UxPlay's
    // video_report_size describes the sender's screen, not the stream, so it is only logged.
    e->decoder.setSizeListener([e](int w, int h) {
        ScopedEnv env;
        env->CallVoidMethod(e->javaEngine, e->onVideoSize, static_cast<jint>(w), static_cast<jint>(h));
    });
    cb.onAudioFormat = [e](int ct) { e->audio.start(ct); };
    cb.onAudio = [e](const uint8_t* data, size_t size) { e->audio.submit(data, size); };
    cb.onAudioVolume = [e](float db) {
        e->audio.setVolumeDb(db);
        ScopedEnv env;
        env->CallVoidMethod(e->javaEngine, e->onVolume, static_cast<jfloat>(db));  // persisted by Java
    };
    cb.onAudioFlush = [e]() { e->audio.flush(); };
    cb.onAudioStop = [e]() { e->audio.stop(); };
    cb.onPlaybackCommand = [e](int command, double value) {
        ScopedEnv env;
        env->CallVoidMethod(e->javaEngine, e->onPlaybackCommand, static_cast<jint>(command), static_cast<jdouble>(value));
    };
    cb.onTrackInfo = [e](const std::string& t, const std::string& a, const std::string& al) {
        ScopedEnv env;
        jstring jt = newUtf(env.get(), t), ja = newUtf(env.get(), a), jal = newUtf(env.get(), al);
        env->CallVoidMethod(e->javaEngine, e->onTrackInfo, jt, ja, jal);
        env->DeleteLocalRef(jt);
        env->DeleteLocalRef(ja);
        env->DeleteLocalRef(jal);
    };
    cb.onCoverArt = [e](const uint8_t* data, size_t size) {
        ScopedEnv env;
        jbyteArray arr = nullptr;
        if (data && size) {
            arr = env->NewByteArray(static_cast<jsize>(size));
            env->SetByteArrayRegion(arr, 0, static_cast<jsize>(size), reinterpret_cast<const jbyte*>(data));
        }
        env->CallVoidMethod(e->javaEngine, e->onCoverArt, arr);
        if (arr) env->DeleteLocalRef(arr);
    };
    cb.onTrackProgress = [e](double pos, double dur) {
        ScopedEnv env;
        env->CallVoidMethod(e->javaEngine, e->onTrackProgress, static_cast<jdouble>(pos), static_cast<jdouble>(dur));
    };
    cb.onAudioPlaying = [e](bool playing) {
        ScopedEnv env;
        env->CallVoidMethod(e->javaEngine, e->onAudioPlaying, static_cast<jboolean>(playing));
    };
    cb.onLog = [e](const std::string& msg) { callWithString(e, e->onLog, msg); };

    AirPlayOptions opt;
    opt.deviceName = name;
    opt.deviceId = id;
    opt.keyFile = keyFile;
    opt.h265 = h265 == JNI_TRUE;
    opt.initialVolumeDb = volumeDb;
    e->audio.setVolumeDb(volumeDb);
    e->server = std::make_unique<AirPlayServer>(std::move(opt), std::move(cb));
    env->ReleaseStringUTFChars(jName, name);
    env->ReleaseStringUTFChars(jId, id);
    env->ReleaseStringUTFChars(jKeyFile, keyFile);
    return reinterpret_cast<jlong>(e);
}

extern "C" JNIEXPORT jintArray JNICALL
Java_app_screenatic_airplay_AirPlayEngine_nativeStart(JNIEnv* env, jobject, jlong h) {
    int port = 0;
    if (!fromHandle(h)->server->start(port)) return nullptr;
    jint ports[2] = {port, port};  // AirPlay and RAOP share the port
    jintArray arr = env->NewIntArray(2);
    env->SetIntArrayRegion(arr, 0, 2, ports);
    return arr;
}

extern "C" JNIEXPORT void JNICALL
Java_app_screenatic_airplay_AirPlayEngine_nativeStop(JNIEnv*, jobject, jlong h) {
    fromHandle(h)->server->stop();
}

extern "C" JNIEXPORT void JNICALL
Java_app_screenatic_airplay_AirPlayEngine_nativeDestroy(JNIEnv* env, jobject, jlong h) {
    Engine* e = fromHandle(h);
    e->server.reset();
    env->DeleteGlobalRef(e->javaEngine);
    delete e;
}

extern "C" JNIEXPORT void JNICALL
Java_app_screenatic_airplay_AirPlayEngine_nativeDisconnectClients(JNIEnv*, jobject, jlong h) {
    fromHandle(h)->server->disconnectClients();
}

extern "C" JNIEXPORT void JNICALL
Java_app_screenatic_airplay_AirPlayEngine_nativeSetSurface(JNIEnv* env, jobject, jlong h, jobject surface) {
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    fromHandle(h)->decoder.setWindow(window);  // decoder takes its own reference
    if (window) ANativeWindow_release(window);
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_app_screenatic_airplay_AirPlayEngine_nativeTxtRecord(JNIEnv* env, jobject, jlong h, jint which) {
    AirPlayServer* s = fromHandle(h)->server.get();
    return toJavaArray(env, which == 0 ? s->airplayTxt() : s->raopTxt());
}

extern "C" JNIEXPORT void JNICALL
Java_app_screenatic_airplay_AirPlayEngine_nativeSetPlayback(JNIEnv*, jobject, jlong h, jdouble durationSec,
                                                          jdouble positionSec, jfloat rate, jboolean ready,
                                                          jboolean buffering, jboolean finished) {
    PlaybackState s;
    s.durationSec = durationSec;
    s.positionSec = positionSec;
    s.rate = rate;
    s.ready = ready == JNI_TRUE;
    s.buffering = buffering == JNI_TRUE;
    s.finished = finished == JNI_TRUE;
    fromHandle(h)->server->setPlaybackState(s);
}
