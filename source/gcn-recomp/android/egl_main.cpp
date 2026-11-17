#include <cstdlib>
#include <jni.h>
// TANDEM_K043_LAYOUT
// TANDEM_K042_LAYOUT
// TANDEM_K041_LAYOUT
// TANDEM_K040_LAYOUT
// TANDEM_K039_LAYOUT
// TANDEM_K038_LAYOUT
// TANDEM_K037_LAYOUT
// TANDEM_K035_LAYOUT
bool audio_open();
void audio_pause();
void audio_resume();
void audio_close();
// NativeActivity EGL/OpenGL ES frontend for the original GX renderer.
#include <android/log.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <dlfcn.h>
#include <cinttypes>
#include <GLES3/gl32.h>
#include <string>
#include <cmath>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <string_view>
#include <iterator>
#include <algorithm>
#include <cstring>
#include <android/input.h>
#include <android/native_activity.h>
#include <android/window.h>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <cerrno>
#include <unistd.h>
#include <fstream>
#include <filesystem>
#ifdef GCN_ANDROID_EOS
#include "../../src/eos_config.h"
#endif

#include "runtime.h"
#include "platform.h"
#include "hw/pad.h"
#include "gamepad.h"
#include "input_script.h"
#include "gx/render.h"
#include "gx/render_gl.h"
#include "gx/gl.h"

uint32_t boot_load(const char*);
extern "C" void tandem_iso_selected(int fd, const char* display_name);
extern "C" void tandem_iso_cancelled();
namespace {
#define TANDEM_EGL_FUNCTIONS(X) \
    X(GetProcAddress) \
    X(MakeCurrent) \
    X(DestroySurface) \
    X(DestroyContext) \
    X(Terminate) \
    X(QuerySurface) \
    X(GetDisplay) \
    X(Initialize) \
    X(BindAPI) \
    X(ChooseConfig) \
    X(GetConfigAttrib) \
    X(CreateWindowSurface) \
    X(CreateContext) \
    X(GetError) \
    X(SwapBuffers)

struct EglApi {
#define EGL_MEMBER(Name) decltype(&egl##Name) Name = &egl##Name;
    TANDEM_EGL_FUNCTIONS(EGL_MEMBER)
#undef EGL_MEMBER

    bool load(void* library) {
#define EGL_LOAD(Name) \
        Name = reinterpret_cast<decltype(Name)>(dlsym(library, "egl" #Name)); \
        if (!Name) return false;
        TANDEM_EGL_FUNCTIONS(EGL_LOAD)
#undef EGL_LOAD
        return true;
    }
};
#undef TANDEM_EGL_FUNCTIONS

EglApi egl_api;
bool using_zink = false;
bool waiting_for_landscape = false;
unsigned zink_transform_updates = 0;

std::string native_library_directory(ANativeActivity* activity) {
    JNIEnv* env = nullptr;
    const bool attached = activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED;
    if (attached && activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return {};
    if (!env) return {};

    std::string path;
    jclass cls = env->GetObjectClass(activity->clazz);
    jmethodID method = cls ? env->GetMethodID(cls, "nativeLibraryDirectory", "()Ljava/lang/String;") : nullptr;
    auto value = method ? static_cast<jstring>(env->CallObjectMethod(activity->clazz, method)) : nullptr;
    if (value && !env->ExceptionCheck()) {
        if (const char* bytes = env->GetStringUTFChars(value, nullptr)) {
            path = bytes;
            env->ReleaseStringUTFChars(value, bytes);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (value) env->DeleteLocalRef(value);
    if (cls) env->DeleteLocalRef(cls);
    if (attached) activity->vm->DetachCurrentThread();
    return path;
}

void select_zink(ANativeActivity* activity) {
    const std::string libdir = native_library_directory(activity);
    if (libdir.empty()) return;
    const std::string root = libdir + "/";
    const std::string mesa = root + "libEGL_mesa.so";
    if (access(mesa.c_str(), R_OK) != 0) return;

    void* tools = dlopen((root + "libadrenotools.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    using OpenVulkan = void* (*)(int, int, const char*, const char*, const char*, const char*, const char*, void**);
    const auto open_vulkan = tools ? reinterpret_cast<OpenVulkan>(dlsym(tools, "adrenotools_open_libvulkan")) : nullptr;
    if (!open_vulkan) {
        __android_log_print(ANDROID_LOG_WARN, "TANDEM-DRIVER", "AdrenoTools unavailable, using Android EGL");
        return;
    }
    constexpr int custom_driver = 1;  // ADRENOTOOLS_DRIVER_CUSTOM
    void* vulkan = open_vulkan(RTLD_NOW | RTLD_LOCAL, custom_driver, activity->internalDataPath,
                              root.c_str(), root.c_str(), "libvulkan_freedreno.so", nullptr, nullptr);
    if (!vulkan) {
        __android_log_print(ANDROID_LOG_WARN, "TANDEM-DRIVER", "Turnip loader rejected, using Android EGL");
        return;
    }

    // Kopper's Android build reads the loader handle before its first Vulkan call.
    char handle[sizeof(uintptr_t) * 2 + 1];
    std::snprintf(handle, sizeof(handle), "%" PRIxPTR, reinterpret_cast<uintptr_t>(vulkan));
    setenv("VULKAN_PTR", handle, 1);
    setenv("EGL_PLATFORM", "android", 1);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
    setenv("GALLIUM_DRIVER", "zink", 1);
    setenv("MESA_ANDROID_NO_KMS_SWRAST", "1", 1);
    EglApi candidate;
    void* library = dlopen(mesa.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library || !candidate.load(library)) {
        __android_log_print(ANDROID_LOG_WARN, "TANDEM-DRIVER", "Mesa EGL unavailable: %s", library ? "missing EGL entries" : dlerror());
        return;
    }
    egl_api = candidate;
    using_zink = true;
}

EGLDisplay display = EGL_NO_DISPLAY;
EGLSurface surface = EGL_NO_SURFACE;
EGLContext context = EGL_NO_CONTEXT;
EGLConfig config = nullptr;
ANativeWindow* egl_window = nullptr;
bool ready = false;
bool renderer_initialized = false;
bool lifecycle_paused = false;
bool focus_lost = false;
bool game_paused = false;
EGLint surface_width = 0, surface_height = 0;
using FrameClock = std::chrono::steady_clock;
struct FrameRate {
    FrameClock::time_point start{};
    unsigned frames = 0;
    int value = -1;

    void presented(FrameClock::time_point now) {
        if (start == FrameClock::time_point{}) {
            start = now;
            return;
        }
        ++frames;
        const auto elapsed = now - start;
        if (elapsed < std::chrono::seconds(1)) return;
        value = static_cast<int>(std::lround(frames / std::chrono::duration<double>(elapsed).count()));
        start = now;
        frames = 0;
    }
} frame_rate;
struct FrameStats {
    FrameClock::time_point last{};
    double render_max = 0, hud_max = 0, swap_max = 0, interval_max = 0, interval_sum = 0;
    unsigned count = 0, intervals = 0, slow = 0;

    void presented(FrameClock::time_point begin, FrameClock::time_point rendered,
                   FrameClock::time_point hud_ready, FrameClock::time_point swapped) {
        const auto ms = [](auto duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        render_max = std::max(render_max, ms(rendered - begin));
        hud_max = std::max(hud_max, ms(hud_ready - rendered));
        swap_max = std::max(swap_max, ms(swapped - hud_ready));
        if (last != FrameClock::time_point{}) {
            const double interval = ms(swapped - last);
            interval_sum += interval;
            interval_max = std::max(interval_max, interval);
            ++intervals;
            if (interval > 25.0) ++slow;
        }
        last = swapped;
        if (++count == 120) {
            __android_log_print(ANDROID_LOG_INFO, "TANDEM-GL",
                "fps=%.2f frames>25ms=%u interval_max_ms=%.2f gx_max_ms=%.2f hud_max_ms=%.2f swap_max_ms=%.2f",
                interval_sum > 0 ? intervals * 1000.0 / interval_sum : 0.0, slow, interval_max, render_max, hud_max, swap_max);
            *this = {};
            last = swapped;
        }
    }
} frame_stats;

void reset_presentation_stats() {
    frame_stats = {};
    frame_rate = {};
}

std::mutex iso_mutex;
std::condition_variable iso_cv;
int iso_fd = -1;
std::string iso_display_name;
bool iso_result_ready = false;
#ifdef GCN_ANDROID_EOS
std::mutex eos_config_mutex;
std::condition_variable eos_config_cv;
int eos_config_fd = -1;
bool eos_config_result_ready = false;
#endif

void* gl_proc(const char* name) { return reinterpret_cast<void*>(egl_api.GetProcAddress(name)); }
// A NativeActivity can destroy its window surface during rotation, focus changes,
// or the SAF picker while the EGL context is still perfectly usable.  Destroying
// the context here invalidates every GL object owned by render_gl.cpp, while the
// old renderer_initialized flag used to make those stale names look live on the
// next surface.  Keep the context/display until the Activity itself is destroyed.
void stop(bool destroy_context = false) {
    ready = false;
    surface_width = surface_height = 0;
    zink_transform_updates = 0;
    reset_presentation_stats();
    if (display != EGL_NO_DISPLAY && (surface != EGL_NO_SURFACE || destroy_context)) {
        egl_api.MakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE) egl_api.DestroySurface(display,surface);
        surface = EGL_NO_SURFACE;
        egl_window = nullptr;
        if (destroy_context) {
            if (context != EGL_NO_CONTEXT) egl_api.DestroyContext(display,context);
            egl_api.Terminate(display);
            display=EGL_NO_DISPLAY;
            context=EGL_NO_CONTEXT;
            config=nullptr;
            renderer_initialized=false;
        }
    }
}

void update_lifecycle_pause() {
    const bool should_pause = lifecycle_paused || focus_lost;
    if (should_pause == game_paused) return;
    game_paused = should_pause;
    if (game_paused) {
        audio_pause();
        __android_log_print(ANDROID_LOG_INFO, "TANDEM-LIFECYCLE",
                            "Jogo pausado: audio e render suspensos");
    } else {
        audio_resume();
        __android_log_print(ANDROID_LOG_INFO, "TANDEM-LIFECYCLE",
                            "Jogo retomado: audio e render restaurados");
    }
}

static bool start_iso_picker(ANativeActivity* activity) {
    if (!activity || !activity->vm || !activity->clazz) return false;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
        attached = true;
    }
    jclass cls = env->GetObjectClass(activity->clazz);
    jmethodID open = env->GetMethodID(cls, "openIsoPicker", "()V");
    bool ok = open != nullptr && !env->ExceptionCheck();
    if (ok) env->CallVoidMethod(activity->clazz, open);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); ok = false; }
    env->DeleteLocalRef(cls);
    if (attached) activity->vm->DetachCurrentThread();
    return ok;
}

static bool start_persisted_iso(ANativeActivity* activity) {
    if (!activity || !activity->vm || !activity->clazz) return false;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
        attached = true;
    }
    jclass cls = env->GetObjectClass(activity->clazz);
    jmethodID open = cls ? env->GetMethodID(cls, "openPersistedIso", "()Z") : nullptr;
    bool ok = open != nullptr && !env->ExceptionCheck();
    if (ok) ok = env->CallBooleanMethod(activity->clazz, open) == JNI_TRUE;
    if (env->ExceptionCheck()) { env->ExceptionClear(); ok = false; }
    if (cls) env->DeleteLocalRef(cls);
    if (attached) activity->vm->DetachCurrentThread();
    return ok;
}

#ifdef GCN_ANDROID_EOS
static bool start_eos_config_picker(ANativeActivity* activity) {
    if (!activity || !activity->vm || !activity->clazz) return false;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
        attached = true;
    }
    jclass cls = env->GetObjectClass(activity->clazz);
    jmethodID open = env->GetMethodID(cls, "openEosConfigPicker", "()V");
    bool ok = open != nullptr && !env->ExceptionCheck();
    if (ok) env->CallVoidMethod(activity->clazz, open);
    if (env->ExceptionCheck()) { env->ExceptionClear(); ok = false; }
    env->DeleteLocalRef(cls);
    if (attached) activity->vm->DetachCurrentThread();
    return ok;
}
#endif

extern "C" void tandem_iso_selected(int fd, const char* display_name) {
    std::lock_guard<std::mutex> lock(iso_mutex);
    if (iso_fd >= 0) close(iso_fd);
    iso_fd = fd;
    iso_display_name = display_name ? display_name : "selected.iso";
    iso_result_ready = true;
    iso_cv.notify_one();
}

extern "C" void tandem_iso_cancelled() {
    std::lock_guard<std::mutex> lock(iso_mutex);
    iso_fd = -1;
    iso_display_name.clear();
    iso_result_ready = true;
    iso_cv.notify_one();
}

extern "C" JNIEXPORT void JNICALL
Java_com_ethannicholas_mkdd_TandemActivity_nativeIsoSelected(
    JNIEnv* env, jclass, jint fd, jstring display_name) {
    const char* name = display_name ? env->GetStringUTFChars(display_name, nullptr) : nullptr;
    tandem_iso_selected(fd, name);
    if (name) env->ReleaseStringUTFChars(display_name, name);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ethannicholas_mkdd_TandemActivity_nativeIsoCancelled(JNIEnv*, jclass) {
    tandem_iso_cancelled();
}

#ifdef GCN_ANDROID_EOS
extern "C" JNIEXPORT void JNICALL
Java_com_ethannicholas_mkdd_TandemActivity_nativeEosConfigSelected(
    JNIEnv*, jclass, jint fd) {
    std::lock_guard<std::mutex> lock(eos_config_mutex);
    if (eos_config_fd >= 0) close(eos_config_fd);
    eos_config_fd = fd;
    eos_config_result_ready = true;
    eos_config_cv.notify_one();
}

extern "C" JNIEXPORT void JNICALL
Java_com_ethannicholas_mkdd_TandemActivity_nativeEosConfigCancelled(JNIEnv*) {
    std::lock_guard<std::mutex> lock(eos_config_mutex);
    if (eos_config_fd >= 0) close(eos_config_fd);
    eos_config_fd = -1;
    eos_config_result_ready = true;
    eos_config_cv.notify_one();
}

static bool store_eos_config(int fd, const char* root) {
    if (fd < 0 || !root) return false;
    const std::string path = std::string(root) + "/eos.json";
    const std::string temporary = path + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) { close(fd); return false; }
    char block[4096]; size_t total = 0;
    for (;;) {
        const ssize_t count = read(fd, block, sizeof(block));
        if (count < 0) { output.close(); unlink(temporary.c_str()); close(fd); return false; }
        if (!count) break;
        total += static_cast<size_t>(count);
        if (total > 16384) { output.close(); unlink(temporary.c_str()); close(fd); return false; }
        output.write(block, count);
        if (!output) { output.close(); unlink(temporary.c_str()); close(fd); return false; }
    }
    close(fd); output.close();
    if (!output || total == 0) { unlink(temporary.c_str()); return false; }
    try {
        mkdd::eos::Config::load(temporary);
        if (rename(temporary.c_str(), path.c_str()) != 0) { unlink(temporary.c_str()); return false; }
        return true;
    } catch (...) {
        unlink(temporary.c_str());
        return false;
    }
}
#endif

static bool iso_fd_has_gamecube_header(int fd) {
    char header[6]{};
    if (fd < 0 || pread(fd, header, sizeof(header), 0) != sizeof(header))
        return false;
    // This recompilation targets Mario Kart: Double Dash!! GM4E01.
    return memcmp(header, "GM4E01", sizeof(header)) == 0;
}

void update_surface_size() {
    EGLint w = 0, h = 0;
    if (!egl_api.QuerySurface(display, surface, EGL_WIDTH, &w) ||
        !egl_api.QuerySurface(display, surface, EGL_HEIGHT, &h) || w <= 0 || h <= 0) return;
    if (w == surface_width && h == surface_height) return;
    surface_width = w;
    surface_height = h;
    gx::render_set_window_size(w, h);
    if (using_zink)
        __android_log_print(ANDROID_LOG_INFO, "TANDEM-DRIVER", "EGL output buffer: %dx%d (requested 1386x640)", w, h);
}

int reset_zink_buffer_transform(ANativeWindow* window) {
    using SetTransform = int (*)(ANativeWindow*, int32_t);
    static const auto set_transform = [] {
        void* library = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
        return library ? reinterpret_cast<SetTransform>(dlsym(library, "ANativeWindow_setBuffersTransform")) : nullptr;
    }();
    return set_transform ? set_transform(window, ANATIVEWINDOW_TRANSFORM_IDENTITY) : -ENOSYS;
}

void clear_input();

bool start(ANativeWindow* window) {
    if (!window || ready) return false;
    if (display == EGL_NO_DISPLAY) {
        display=egl_api.GetDisplay(EGL_DEFAULT_DISPLAY);
        if(display==EGL_NO_DISPLAY || !egl_api.Initialize(display,nullptr,nullptr)) return false;
        if(!egl_api.BindAPI(EGL_OPENGL_ES_API)) return false;
        const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,
            EGL_DEPTH_SIZE,24,EGL_NONE};
        EGLint count=0;
        if(!egl_api.ChooseConfig(display,attrs,&config,1,&count) || count<1) return false;
    }
    EGLint visual=0; egl_api.GetConfigAttrib(display,config,EGL_NATIVE_VISUAL_ID,&visual);
    // TANDEM_K050_LOW_RES
    ANativeWindow_setBuffersGeometry(window,1386,640,visual);
    if (surface != EGL_NO_SURFACE && egl_window != window) {
        // The old native window really went away.  The EGL context remains
        // alive, so deleting only this surface preserves all K192 GL objects.
        egl_api.MakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        egl_api.DestroySurface(display,surface);
        surface = EGL_NO_SURFACE;
        egl_window = nullptr;
    }
    if (surface == EGL_NO_SURFACE) {
        surface=egl_api.CreateWindowSurface(display,config,window,nullptr);
        egl_window = window;
    }
    if (context == EGL_NO_CONTEXT) {
        const EGLint ctx[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
        context=egl_api.CreateContext(display,config,EGL_NO_CONTEXT,ctx);
    }
    if(surface==EGL_NO_SURFACE || context==EGL_NO_CONTEXT ||
       !egl_api.MakeCurrent(display,surface,surface,context)) return false;
    const int version=gl_load_with(gl_proc);
    __android_log_print(ANDROID_LOG_INFO,"TANDEM-GL","GLAD version=%d EGL=%p",version,display);
    if(version<32) return false;
    if (using_zink && !renderer_initialized) {
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const std::string_view name = renderer ? renderer : "";
        __android_log_print(ANDROID_LOG_INFO, "TANDEM-DRIVER", "Experimental GL renderer: %s", renderer ? renderer : "(unknown)");
        if (name.find("zink") == name.npos ||
            (name.find("Turnip") == name.npos && name.find("turnip") == name.npos &&
             name.find("MESA_TURNIP") == name.npos))
            return false;
    }
    update_surface_size();
    if (!renderer_initialized) {
        gx::render_init(1);
        renderer_initialized=true;
    }
    zink_transform_updates = using_zink ? 2 : 0;
    ready=true;
    return true;
}
void start_window(android_app* app) {
    if (!app->window || ready) return;
    if (using_zink) {
        const int width = ANativeWindow_getWidth(app->window);
        const int height = ANativeWindow_getHeight(app->window);
        if (width <= height || height <= 0) {
            if (!waiting_for_landscape)
                __android_log_print(ANDROID_LOG_INFO, "TANDEM-DRIVER", "Waiting for landscape window (now %dx%d)", width, height);
            waiting_for_landscape = true;
            return;
        }
        __android_log_print(ANDROID_LOG_INFO, "TANDEM-DRIVER", "Starting Zink in landscape (%dx%d)", width, height);
    }
    waiting_for_landscape = false;

    bool started = start(app->window);
    if (!started && using_zink && !renderer_initialized) {
        __android_log_print(ANDROID_LOG_WARN, "TANDEM-DRIVER", "Zink or Turnip initialization failed, using Android EGL");
        stop(true);
        using_zink = false;
        egl_api = {};
        started = start(app->window);
    }
    if (!started)
        __android_log_print(ANDROID_LOG_ERROR, "TANDEM-GL", "EGL init failed: 0x%x", egl_api.GetError());
}

void command(android_app* app,int32_t cmd) {
    if (cmd == APP_CMD_INIT_WINDOW) {
        start_window(app);
    } else if(cmd==APP_CMD_TERM_WINDOW) {
        waiting_for_landscape = false;
        clear_input();
        stop(false);
    }
    else if(cmd==APP_CMD_DESTROY) stop(true);
    else if(cmd==APP_CMD_PAUSE) {
        clear_input();
        reset_presentation_stats();
        lifecycle_paused = true;
        update_lifecycle_pause();
    } else if(cmd==APP_CMD_RESUME) {
        lifecycle_paused = false;
        update_lifecycle_pause();
    } else if(cmd==APP_CMD_LOST_FOCUS) {
        clear_input();
        reset_presentation_stats();
        focus_lost = true;
        update_lifecycle_pause();
    } else if(cmd==APP_CMD_GAINED_FOCUS) {
        focus_lost = false;
        update_lifecycle_pause();
    }
    else if (cmd == APP_CMD_WINDOW_RESIZED || cmd == APP_CMD_CONFIG_CHANGED ||
             cmd == APP_CMD_WINDOW_REDRAW_NEEDED) {
        if (ready) {
            update_surface_size();
            if (using_zink) zink_transform_updates = 2;
        } else if (waiting_for_landscape) {
            start_window(app);
        }
    }
}

// TANDEM_K020_TOUCH
struct TouchPoint {
    int id = -1;
    float x = 0;
    float y = 0;
};

TouchPoint touches[16];
std::atomic<bool> physical_controller_connected{false};
bool detected_controller = false;
bool hide_touch_controls = false;
bool revealing_touch_controls = false;

void clear_touches() {
    for (auto& touch : touches) touch.id = -1;
    revealing_touch_controls = false;
}

void update_touch_visibility() {
    const bool connected = physical_controller_connected.load(std::memory_order_relaxed);
    if (connected == detected_controller) return;
    clear_touches();
    detected_controller = connected;
    hide_touch_controls = connected;
}

void clear_input() {
    clear_touches();
    android_gamepad_clear();
}

void update_touch(AInputEvent* event) {
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return;

    int action = AMotionEvent_getAction(event);
    int masked = action & AMOTION_EVENT_ACTION_MASK;
    int index = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

    if (masked == AMOTION_EVENT_ACTION_CANCEL) {
        for (auto& t : touches) t.id = -1;
        return;
    }

    if (masked == AMOTION_EVENT_ACTION_UP ||
        masked == AMOTION_EVENT_ACTION_POINTER_UP) {
        if (index < (int)AMotionEvent_getPointerCount(event)) {
            int id = AMotionEvent_getPointerId(event, index);
            for (auto& t : touches)
                if (t.id == id) t.id = -1;
        }
    }

    if (masked == AMOTION_EVENT_ACTION_DOWN ||
        masked == AMOTION_EVENT_ACTION_POINTER_DOWN ||
        masked == AMOTION_EVENT_ACTION_MOVE) {
        size_t count = AMotionEvent_getPointerCount(event);

        for (size_t i = 0; i < count; ++i) {
            if ((masked == AMOTION_EVENT_ACTION_DOWN ||
                 masked == AMOTION_EVENT_ACTION_POINTER_DOWN) &&
                (int)i != index) continue;

            int id = AMotionEvent_getPointerId(event, i);
            TouchPoint* slot = nullptr;

            for (auto& t : touches)
                if (t.id == id) { slot = &t; break; }

            if (!slot)
                for (auto& t : touches)
                    if (t.id == -1) { slot = &t; break; }

            if (slot) {
                slot->id = id;
                slot->x = AMotionEvent_getX(event, i);
                slot->y = AMotionEvent_getY(event, i);
            }
        }
    }
}

int32_t input(android_app* app, AInputEvent* event) {
    if (game_paused) return 0;
    update_touch_visibility();
    const auto type = AInputEvent_getType(event);
    if (android_gamepad_event(app->activity, event)) {
        if (detected_controller && !hide_touch_controls && type == AINPUT_EVENT_TYPE_KEY &&
            AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN &&
            AKeyEvent_getRepeatCount(event) == 0) {
            clear_touches();
            hide_touch_controls = true;
        }
        return 1;
    }
    if (type != AINPUT_EVENT_TYPE_MOTION ||
        (AInputEvent_getSource(event) & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN)
        return 0;

    const int action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    if (action == AMOTION_EVENT_ACTION_DOWN) {
        revealing_touch_controls = hide_touch_controls;
        hide_touch_controls = false;
    }
    if (revealing_touch_controls) {
        if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL)
            revealing_touch_controls = false;
        return 1;
    }
    if (!hide_touch_controls) update_touch(event);
    return 1;
}

// TANDEM_K023_COMPLETE
struct HudButton {
    float x,y,r;
    uint16_t mask;
    const char* label;
    float touch_scale = 1.5f;
};

static constexpr HudButton hud_buttons[] = {
    {0.8600f,0.7600f,0.0845f,PAD_A,"A"},
    {0.7827f,0.7600f,0.0650f,PAD_B,"B"},
    {0.8550f,0.4500f,0.065f,PAD_X,"X"},
    {0.82f,0.12f,0.035f,PAD_Z,"Z"},
    {0.09f,0.12f,0.045f,PAD_L,"L"},
    {0.20f,0.12f,0.040f,PAD_Y,"Y",1.0f},
    {0.91f,0.12f,0.045f,PAD_R,"R"},
    {0.50f,0.87f,0.040f,PAD_START,"START"}
};


static GLuint hud_program=0,hud_vao=0,hud_vbo=0;
static GLint hud_color=-1;
static bool hud_failed=false;

static GLuint hud_shader(GLenum type,const char* src) {
    GLuint sh=glCreateShader(type);
    glShaderSource(sh,1,&src,nullptr);
    glCompileShader(sh);
    GLint ok=0;
    glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);
    if(!ok) {
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static bool hud_init() {
    const char* vs=R"(#version 300 es
        layout(location=0) in vec2 pos;
        void main(){gl_Position=vec4(pos,0.0,1.0);}
    )";
    const char* fs=R"(#version 300 es
        precision mediump float;
        uniform vec4 color;
        out vec4 frag;
        void main(){frag=color;}
    )";

    GLuint v=hud_shader(GL_VERTEX_SHADER,vs);
    GLuint f=hud_shader(GL_FRAGMENT_SHADER,fs);
    if(!v || !f) {
        if(v) glDeleteShader(v);
        if(f) glDeleteShader(f);
        return false;
    }

    hud_program=glCreateProgram();
    glAttachShader(hud_program,v);
    glAttachShader(hud_program,f);
    glLinkProgram(hud_program);
    glDeleteShader(v);
    glDeleteShader(f);

    GLint ok=0;
    glGetProgramiv(hud_program,GL_LINK_STATUS,&ok);
    if(!ok) {
        glDeleteProgram(hud_program);
        hud_program=0;
        return false;
    }

    hud_color=glGetUniformLocation(hud_program,"color");

    glGenVertexArrays(1,&hud_vao);
    glGenBuffers(1,&hud_vbo);
    glBindVertexArray(hud_vao);
    glBindBuffer(GL_ARRAY_BUFFER,hud_vbo);
    glBufferData(GL_ARRAY_BUFFER,16384,nullptr,GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr);
    glBindVertexArray(0);
    return true;
}



static void hud_emit(const float* v,int count,GLenum mode) {
    glBufferSubData(GL_ARRAY_BUFFER,0,count*2*sizeof(float),v);
    glDrawArrays(mode,0,count);
}

static void hud_shape(float x, float y, float r, float w, float h,
                      uint16_t button = 0, bool octagon = false) {
    constexpr int segments = 32;
    const float scale = std::min(w, h);
    const float scale_x = 2 * r * scale / w;
    const float scale_y = 2 * r * scale / h;
    float vertices[(segments + 2) * 2]{};
    vertices[0] = 2 * x - 1;
    vertices[1] = 1 - 2 * y;
    const int sides = octagon ? 8 : segments;
    const bool shoulder = button == PAD_L || button == PAD_R;
    const float angle_range = shoulder ? 3.14159265f : 6.28318531f;
    for (int i = 0; i <= sides; ++i) {
        const float angle = (shoulder ? 3.14159265f : 0.39269908f * octagon) +
            angle_range * (i % (shoulder ? sides + 1 : sides)) / sides;
        float px = std::cos(angle);
        float py = std::sin(angle);
        switch (button) {
        case PAD_L:
        case PAD_R: {
            px *= 1.36f;
            py = 0.36f + py * 1.26f;
            const float tilt = button == PAD_L ? -0.39f : 0.39f;
            const float rotated_x = px * std::cos(tilt) - py * std::sin(tilt);
            py = px * std::sin(tilt) + py * std::cos(tilt);
            px = rotated_x;
            break;
        }
        case PAD_Z:
            px = px * 0.68f + std::copysign(0.62f, px);
            py *= 0.68f;
            break;
        case PAD_X:
            px = px * 0.63f + 0.29f * (0.5f - py * py);
            py *= 1.13f;
            break;
        case PAD_Y:
            py = py * 0.61f + 0.26f * (px * px - 0.5f);
            break;
        }
        vertices[(i + 1) * 2] = vertices[0] + px * scale_x;
        vertices[(i + 1) * 2 + 1] = vertices[1] - py * scale_y;
    }
    if (shoulder) {
        // The straight segment between the ends of the arc closes the shoulder.
        vertices[0] = vertices[2];
        vertices[1] = vertices[3];
    }
    hud_emit(vertices, sides + 2, GL_TRIANGLE_FAN);
}

static std::string_view hud_glyph(char c) {
    switch(c) {
        case 'A': return "011101000110001111111000110001";
        case 'B': return "111101000110001111101000111110";
        case 'X': return "10001100010101000100010101000110001";
        case 'Z': return "111110000100010001000100011111";
        case 'L': return "100001000010000100001000011111";
        case 'Y': return "10001100010101000100001000010000100";
        case 'R': return "111101000110001111101010010001";
        case 'S': return "011111000010000011100000111110";
        case 'T': return "111110010000100001000010000100";
        case '0': return "01110100011001110101110011000101110";
        case '1': return "00100011000010000100001000010001110";
        case '2': return "01110100010000100010001000100011111";
        case '3': return "11110000010000101110000010000111110";
        case '4': return "00010001100101010010111110001000010";
        case '5': return "11111100001000011110000010000111110";
        case '6': return "01110100001000011110100011000101110";
        case '7': return "11111000010001000100010000100001000";
        case '8': return "01110100011000101110100011000101110";
        case '9': return "01110100011000101111000010000101110";
        case 'F': return "11111100001000011110100001000010000";
        case 'P': return "11110100011000111110100001000010000";
        case '-': return "00000000000000011111000000000000000";
        default: return {};
    }
}

static void hud_text_vertices(std::vector<float>& vertices, std::string_view text,
                              float center_x, float center_y, float unit, float w, float h) {
    const size_t len = text.size();
    const float x0 = center_x * w - (len * 6 - 1) * unit * 0.5f;
    const float y0 = center_y * h - 3.5f * unit;

    vertices.clear();
    vertices.reserve(len * 35 * 12);
    for (size_t k = 0; k < len; ++k) {
        const auto glyph = hud_glyph(text[k]);

        for(int row=0;row<7;row++)
            for(int col=0;col<5;col++)
                if (size_t(row * 5 + col) < glyph.size() && glyph[row * 5 + col] == '1') {
                    float x=x0+(k*6+col)*unit;
                    float y=y0+row*unit;
                    float v[]={
                        x/w*2-1,1-y/h*2,
                        (x+unit)/w*2-1,1-y/h*2,
                        x/w*2-1,1-(y+unit)/h*2,
                        (x+unit)/w*2-1,1-y/h*2,
                        (x+unit)/w*2-1,1-(y+unit)/h*2,
                        x/w*2-1,1-(y+unit)/h*2
                    };
                    vertices.insert(vertices.end(), std::begin(v), std::end(v));
                }
    }
}

static void hud_draw_fps(EGLint w, EGLint h) {
    constexpr float right = 0.98f;
    constexpr float center_y = 0.045f;
    static EGLint width = 0, height = 0;
    static int displayed = -2;
    static std::vector<float> text;
    static std::array<float, 8> background{};
    if (displayed != frame_rate.value || width != w || height != h) {
        displayed = frame_rate.value;
        width = w;
        height = h;
        char label[24] = "-- FPS";
        if (displayed >= 0) std::snprintf(label, sizeof(label), "%d FPS", displayed);
        const float unit = 0.004f * std::min(w, h);
        const float width_fraction = (std::string_view(label).size() * 6 + 4) * unit / w;
        hud_text_vertices(text, label, right - width_fraction / 2, center_y, unit, w, h);
        const float x_right = 2 * right - 1;
        const float x_left = 2 * (right - width_fraction) - 1;
        const float half_height = 11 * unit / h;
        const float center = 1 - 2 * center_y;
        background = {x_left, center + half_height, x_right, center + half_height,
                      x_left, center - half_height, x_right, center - half_height};
    }

    glUniform4f(hud_color, 0, 0, 0, 0.68f);
    hud_emit(background.data(), 4, GL_TRIANGLE_STRIP);
    glUniform4f(hud_color, 1, 1, 1, 1);
    hud_emit(text.data(), text.size() / 2, GL_TRIANGLES);
}

// TANDEM_K033_LAYOUT
// TANDEM_K031_MOVING_STICK
static float hud_stick_x = 0.18f;
static float hud_stick_y = 0.76f;

static void hud_draw() {
    if(!ready || hud_failed) return;

    const EGLint w = surface_width, h = surface_height;
    if(w<=0 || h<=0) return;

    GLint program=0,vao=0,array_buffer=0;
    GLint previous_fbo=0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&previous_fbo);
    GLint viewport[4]{};
    GLint blend_src_rgb=0,blend_dst_rgb=0;
    GLint blend_src_alpha=0,blend_dst_alpha=0;
    GLint blend_eq_rgb=0,blend_eq_alpha=0;
    GLboolean color_mask[4]{};
    GLboolean depth_mask=GL_TRUE;
    GLboolean blend=glIsEnabled(GL_BLEND);
    GLboolean depth=glIsEnabled(GL_DEPTH_TEST);
    GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST);
    GLboolean cull=glIsEnabled(GL_CULL_FACE);
    GLboolean stencil=glIsEnabled(GL_STENCIL_TEST);

    glGetIntegerv(GL_CURRENT_PROGRAM,&program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&array_buffer);
    glGetIntegerv(GL_VIEWPORT,viewport);
    glGetIntegerv(GL_BLEND_SRC_RGB,&blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB,&blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&blend_src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA,&blend_dst_alpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB,&blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA,&blend_eq_alpha);
    glGetBooleanv(GL_COLOR_WRITEMASK,color_mask);
    glGetBooleanv(GL_DEPTH_WRITEMASK,&depth_mask);

    if(!hud_program && !hud_init()) {
        hud_failed=true;
        glBindBuffer(GL_ARRAY_BUFFER,array_buffer);
        glBindVertexArray(vao);
        glUseProgram(program);
        return;
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
    glViewport(0,0,w,h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(hud_program);
    glBindVertexArray(hud_vao);
    glBindBuffer(GL_ARRAY_BUFFER,hud_vbo);

    if (!hide_touch_controls) {
        static std::array<std::vector<float>, std::size(hud_buttons)> label_vertices;
        static EGLint label_width = 0, label_height = 0;
        if (label_width != w || label_height != h) {
            for (size_t i = 0; i < std::size(hud_buttons); ++i) {
                const auto& button = hud_buttons[i];
                const float unit = (button.mask == PAD_START ? 0.0022f : 0.0038f) * std::min(w, h);
                hud_text_vertices(label_vertices[i], button.label, button.x, button.y, unit, w, h);
            }
            label_width = w;
            label_height = h;
        }

        for (size_t i = 0; i < std::size(hud_buttons); ++i) {
            const auto& b = hud_buttons[i];
            glUniform4f(hud_color, 0.72f, 0.72f, 0.72f, 0.80f);
            hud_shape(b.x, b.y, b.r, w, h, b.mask);
            glUniform4f(hud_color, 0.035f, 0.035f, 0.035f, 1);
            const auto& label = label_vertices[i];
            if (!label.empty()) hud_emit(label.data(), label.size() / 2, GL_TRIANGLES);
        }

        glUniform4f(hud_color, 0.39f, 0.39f, 0.39f, 0.81f);
        hud_shape(0.18f, 0.76f, 0.128f, w, h, 0, true);
        glUniform4f(hud_color, 0.12f, 0.12f, 0.12f, 0.86f);
        hud_shape(0.18f, 0.76f, 0.092f, w, h);
        glUniform4f(hud_color, 0.78f, 0.78f, 0.78f, 0.86f);
        hud_shape(hud_stick_x, hud_stick_y, 0.0525f, w, h);
    }
    hud_draw_fps(w, h);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,previous_fbo);
    glBindBuffer(GL_ARRAY_BUFFER,array_buffer);
    glBindVertexArray(vao);
    glUseProgram(program);
    glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
    glBlendFuncSeparate(blend_src_rgb,blend_dst_rgb,
                        blend_src_alpha,blend_dst_alpha);
    glBlendEquationSeparate(blend_eq_rgb,blend_eq_alpha);
    glDepthMask(depth_mask);
    glColorMask(color_mask[0],color_mask[1],
                color_mask[2],color_mask[3]);

    if(blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if(depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if(scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if(cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if(stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
}

PadState touch_pad() {
    PadState pad{};
    pad.connected = true;

    if (hide_touch_controls || surface == EGL_NO_SURFACE) return pad;

    EGLint width = 0, height = 0;
    if (egl_window) {
        width = ANativeWindow_getWidth(egl_window);
        height = ANativeWindow_getHeight(egl_window);
    }
    if (width <= 0 || height <= 0) return pad;

    float w = float(width);
    float h = float(height);

    hud_stick_x = 0.18f;
    hud_stick_y = 0.76f;

    for (const auto& t : touches) {
        if (t.id < 0) continue;

        float x = t.x / w;
        float y = t.y / h;

        // Controles alinhados ao layout K021.
        if (x < 0.40f && y > 0.50f) {
            const float visual_radius = 0.075f * std::min(w,h);
            float vx = t.x - 0.18f*w;
            float vy = t.y - 0.76f*h;
            float distance = std::sqrt(vx*vx + vy*vy);

            if (distance > visual_radius && distance > 0.0f) {
                vx *= visual_radius / distance;
                vy *= visual_radius / distance;
            }

            hud_stick_x = (0.18f*w + vx)/w;
            hud_stick_y = (0.76f*h + vy)/h;

            float dx = (x - 0.18f) / 0.085f;
            float dy = (0.76f - y) / 0.085f;

            dx = std::clamp(dx, -1.0f, 1.0f);
            dy = std::clamp(dy, -1.0f, 1.0f);

            if (std::fabs(dx) < 0.12f) dx = 0;
            if (std::fabs(dy) < 0.12f) dy = 0;

            pad.stick_x = uint8_t(128 + dx * 127);
            pad.stick_y = uint8_t(128 + dy * 127);
        }

        for (const auto& b : hud_buttons) {
            float dx = (x - b.x) * w;
            float dy = (y - b.y) * h;
            float r = b.r * std::min(w, h) * b.touch_scale;

            if (dx * dx + dy * dy <= r * r)
                pad.buttons |= b.mask;
        }

    }

    return pad;
}

}

// TANDEM_K044_IMMERSIVE
static void tandem_enable_immersive(ANativeActivity* activity) {
    if (!activity || !activity->vm || !activity->clazz)
        return;

    JNIEnv* env = nullptr;
    bool attached = false;

    if (activity->vm->GetEnv(
            reinterpret_cast<void**>(&env),
            JNI_VERSION_1_6) != JNI_OK) {
        if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
            return;
        attached = true;
    }

    jclass activityClass = env->GetObjectClass(activity->clazz);
    jmethodID getWindow = env->GetMethodID(
        activityClass,
        "getWindow",
        "()Landroid/view/Window;"
    );

    if (getWindow) {
        jobject window = env->CallObjectMethod(
            activity->clazz, getWindow
        );

        if (window) {
            jclass windowClass = env->GetObjectClass(window);
            jmethodID getDecorView = env->GetMethodID(
                windowClass,
                "getDecorView",
                "()Landroid/view/View;"
            );

            if (getDecorView) {
                jobject decor = env->CallObjectMethod(
                    window, getDecorView
                );

                if (decor) {
                    jclass viewClass = env->GetObjectClass(decor);
                    jmethodID setFlags = env->GetMethodID(
                        viewClass,
                        "setSystemUiVisibility",
                        "(I)V"
                    );

                    if (setFlags) {
                        // FULLSCREEN | HIDE_NAVIGATION |
                        // IMMERSIVE_STICKY | LAYOUT_FLAGS
                        env->CallVoidMethod(
                            decor, setFlags, 0x1706
                        );
                    }

                    env->DeleteLocalRef(viewClass);
                    env->DeleteLocalRef(decor);
                }
            }

            env->DeleteLocalRef(windowClass);
            env->DeleteLocalRef(window);
        }
    }

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }

    env->DeleteLocalRef(activityClass);

    if (attached)
        activity->vm->DetachCurrentThread();
}

#ifdef GCN_ANDROID_EOS
extern "C" bool tandem_eos_java_init(ANativeActivity* activity);
extern "C" bool tandem_eos_native_init(ANativeActivity* activity);
extern "C" void tandem_eos_service_start(ANativeActivity* activity);
#endif

extern "C" JNIEXPORT void JNICALL
Java_com_ethannicholas_mkdd_TandemActivity_nativeGamepadPresence(JNIEnv*, jclass, jboolean connected) {
    physical_controller_connected.store(connected == JNI_TRUE, std::memory_order_relaxed);
}

void android_main(android_app* app) {
#ifdef GCN_ANDROID_EOS
    // TANDEM_K057C_ONLINE
    setenv("MKDD_ONLINE_MENU", "1", 1);
#endif

    tandem_enable_immersive(app->activity);

    app_dummy();
    // TANDEM_FULLSCREEN
    ANativeActivity_setWindowFlags(
        app->activity,
        AWINDOW_FLAG_FULLSCREEN,
        0
    );

    const std::string shader_cache = std::string(app->activity->internalDataPath) + "/tandem-gx-shaders.bin";
    gx::render_set_shader_cache(shader_cache.c_str());
    clear_input();
    select_zink(app->activity);
    app->onAppCmd=command; app->onInputEvent=input;
    while ((!app->window || waiting_for_landscape) && !app->destroyRequested) {
        int events; android_poll_source* source=nullptr;
        int id=ALooper_pollOnce(100,nullptr,&events,(void**)&source);
        if(id>=0 && source) source->process(app,source);
        if (waiting_for_landscape && !app->destroyRequested) start_window(app);
    }
    if (app->destroyRequested) return;
    const char* root=app->activity->internalDataPath;
#ifdef GCN_ANDROID_EOS
    const std::string eos_config_path = std::string(root) + "/eos.json";
    if (std::filesystem::is_regular_file(eos_config_path)) {
        try {
            (void)mkdd::eos::Config::load(eos_config_path);
            __android_log_print(ANDROID_LOG_INFO, "TANDEM-EOS",
                                "eos.json privado valido; seletor EOS omitido");
        } catch (const std::exception& error) {
            __android_log_print(ANDROID_LOG_WARN, "TANDEM-EOS",
                                "eos.json privado invalido; multiplayer desativado (%s)",
                                error.what());
        }
    } else {
        __android_log_print(ANDROID_LOG_INFO, "TANDEM-EOS",
                            "eos.json privado ausente; multiplayer desativado");
    }
    // EOS configuration is optional for offline play.  The existing EOS picker
    // remains available as an explicit recovery/configuration entry point; it
    // is never mandatory during the normal boot path.
    // The official EOS Android SDK requires its Java bridge before native
    // EOS_Initialize.  Its dependencies are packaged in K198; no credential
    // is passed through this call.
    tandem_eos_java_init(app->activity);
    // Native EOS Connect/Lobby/P2P is initialized by the service.
    tandem_eos_service_start(app->activity);
#endif

    for (;;) {
        {
            std::lock_guard<std::mutex> lock(iso_mutex);
            if (iso_fd >= 0) close(iso_fd);
            iso_fd = -1;
            iso_display_name.clear();
            iso_result_ready = false;
        }
        bool persisted_started = start_persisted_iso(app->activity);
        if (persisted_started) {
            __android_log_print(ANDROID_LOG_INFO,"TANDEM-ISO",
                                "Tentando ISO persistida via permissao SAF");
        } else {
            __android_log_print(ANDROID_LOG_INFO,"TANDEM-ISO",
                                "Solicitando ISO pelo Android Storage Access Framework");
            if (!start_iso_picker(app->activity)) {
                __android_log_print(ANDROID_LOG_ERROR,"TANDEM-ISO","Falha ao iniciar ACTION_OPEN_DOCUMENT");
                return;
            }
        }
        // K084: manter o looper Android ativo enquanto o SAF responde.
        while (!app->destroyRequested) {
            {
                std::lock_guard<std::mutex> lock(iso_mutex);
                if (iso_result_ready) break;
            }
            int events = 0;
            android_poll_source* source = nullptr;
            int id = ALooper_pollOnce(16, nullptr, &events,
                                     (void**)&source);
            if (id >= 0 && source) source->process(app, source);
        }
        if (app->destroyRequested) {
            __android_log_print(ANDROID_LOG_WARN, "TANDEM-ISO",
                                "Activity encerrada durante selecao");
            return;
        }
        std::unique_lock<std::mutex> iso_lock(iso_mutex);
        if (iso_fd < 0) {
            __android_log_print(ANDROID_LOG_ERROR,"TANDEM-ISO","Selecao cancelada ou descritor invalido");
            return;
        }
        if (!iso_fd_has_gamecube_header(iso_fd)) {
            __android_log_print(ANDROID_LOG_ERROR,"TANDEM-ISO",
                                "Arquivo selecionado nao e uma ISO GM4E01; selecione a imagem do jogo");
            close(iso_fd);
            iso_fd = -1;
            iso_display_name.clear();
            iso_lock.unlock();
            continue;
        }
        const std::string iso = "/proc/self/fd/" + std::to_string(iso_fd);
        __android_log_print(ANDROID_LOG_INFO,"TANDEM-ISO","ISO SAF selecionada: %s fd=%d (sem copia)", iso_display_name.c_str(), iso_fd);
        iso_lock.unlock();
        g_save_dir=root;
        mem_init(); timing_init(); input_script_init(); configure_game_boot();
        uint32_t entry=boot_load(iso.c_str()); threads_start_boot(entry);
        // TANDEM_K004_AUDIO_START
        if (!audio_open()) __android_log_print(ANDROID_LOG_ERROR,"TANDEM-AUDIO","Falha ao iniciar audio");
        break;
    }
    while(!app->destroyRequested) {
        int events; android_poll_source* source=nullptr;
        const int timeout = ready && !game_paused ? 0 : 16;
        int id = ALooper_pollOnce(timeout, nullptr, &events, (void**)&source);
        if (id >= 0 && source) source->process(app, source);
        if (app->destroyRequested) break;
        if (waiting_for_landscape) start_window(app);

        update_touch_visibility();
        PadState pad = touch_pad();
        if (!game_paused) android_gamepad_merge(pad);
        pad_set_state(0, pad);
        if (game_paused) {
            // The guest threads keep their timing state while backgrounded.  Drain
            // batches produced during that interval instead of replaying stale or
            // partial frames when focus returns; the first resumed batch is then a
            // complete current frame while the audio callback remains gated.
            (void)gx::take_batch(0);
        } else if(ready) {
            if(auto batch=gx::take_batch(8)) {
                const auto begin = FrameClock::now();
                if (gx::render_execute(*batch)) {
                    const auto rendered = FrameClock::now();
                    hud_draw();
                    const auto hud_ready = FrameClock::now();
                    if (zink_transform_updates && egl_window) {
                        // Kopper's Vulkan swapchain can leave a rotation on
                        // the native window. Our EGL output is already landscape.
                        const int result = reset_zink_buffer_transform(egl_window);
                        --zink_transform_updates;
                        __android_log_print(result == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_WARN,
                            "TANDEM-DRIVER", "Landscape buffer transform identity: %d", result);
                    }
                    if (egl_api.SwapBuffers(display, surface)) {
                        const auto swapped = FrameClock::now();
                        frame_rate.presented(swapped);
                        frame_stats.presented(begin, rendered, hud_ready, swapped);
                    } else {
                        __android_log_print(ANDROID_LOG_ERROR,"TANDEM-GL","swap failed 0x%x",egl_api.GetError());
                        stop();
                    }
                }
            }
        }
    }
    audio_close(); stop(true); threads_request_quit();
}
