#include "gamepad.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <map>
#include <mutex>

namespace {
enum Axis { X, Y, Z, RZ, RX, RY, HatX, HatY, LTrigger, RTrigger, Brake, Gas };
struct Range {
    float min, max, flat;
    bool available() const { return max > min; }
};
struct Controller {
    std::array<Range, 12> ranges{};
    bool queried = false;
    uint32_t keys = 0;
    PadState motion{};
};

std::mutex controllers_mutex;
std::map<int, Controller> controllers;
int active_device = -1;

constexpr std::array<int, 12> axes = {
    AMOTION_EVENT_AXIS_X, AMOTION_EVENT_AXIS_Y, AMOTION_EVENT_AXIS_Z,
    AMOTION_EVENT_AXIS_RZ, AMOTION_EVENT_AXIS_RX, AMOTION_EVENT_AXIS_RY,
    AMOTION_EVENT_AXIS_HAT_X, AMOTION_EVENT_AXIS_HAT_Y, AMOTION_EVENT_AXIS_LTRIGGER,
    AMOTION_EVENT_AXIS_RTRIGGER, AMOTION_EVENT_AXIS_BRAKE, AMOTION_EVENT_AXIS_GAS,
};

void query_ranges(ANativeActivity* activity, const AInputEvent* event, Controller& pad) {
    JNIEnv* env = nullptr;
    const bool attached = activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED;
    if (attached && activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    if (!env) return;
    jclass cls = env->GetObjectClass(activity->clazz);
    jmethodID method = cls ? env->GetStaticMethodID(cls, "gamepadRanges", "(II[I)[F") : nullptr;
    auto axis_ids = method ? env->NewIntArray(axes.size()) : nullptr;
    if (axis_ids) {
        env->SetIntArrayRegion(axis_ids, 0, axes.size(), axes.data());
        auto ranges = static_cast<jfloatArray>(env->CallStaticObjectMethod(
            cls, method, AInputEvent_getDeviceId(event), AInputEvent_getSource(event), axis_ids));
        if (ranges && !env->ExceptionCheck()) {
            std::array<jfloat, axes.size() * 3> values{};
            env->GetFloatArrayRegion(ranges, 0, values.size(), values.data());
            if (!env->ExceptionCheck()) {
                for (size_t i = 0; i < axes.size(); ++i)
                    pad.ranges[i] = {values[i * 3], values[i * 3 + 1], values[i * 3 + 2]};
                pad.queried = true;
            }
        }
        if (ranges) env->DeleteLocalRef(ranges);
        env->DeleteLocalRef(axis_ids);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (cls) env->DeleteLocalRef(cls);
    if (attached) activity->vm->DetachCurrentThread();
}

struct Button { int code; uint16_t mask; };
constexpr Button buttons[] = {
    {AKEYCODE_BUTTON_A, PAD_A}, {AKEYCODE_BUTTON_B, PAD_B},
    {AKEYCODE_BUTTON_X, PAD_X}, {AKEYCODE_BUTTON_Y, PAD_Y},
    {AKEYCODE_BUTTON_START, PAD_START},
    {AKEYCODE_BUTTON_L1, PAD_L}, {AKEYCODE_BUTTON_L2, PAD_L},
    {AKEYCODE_BUTTON_R1, PAD_Z}, {AKEYCODE_BUTTON_Z, PAD_Z},
    {AKEYCODE_BUTTON_R2, PAD_R},
    {AKEYCODE_DPAD_UP, PAD_UP}, {AKEYCODE_DPAD_DOWN, PAD_DOWN},
    {AKEYCODE_DPAD_LEFT, PAD_LEFT}, {AKEYCODE_DPAD_RIGHT, PAD_RIGHT},
    {AKEYCODE_DPAD_CENTER, PAD_A},
    {AKEYCODE_BUTTON_1, PAD_A}, {AKEYCODE_BUTTON_2, PAD_B},
    {AKEYCODE_BUTTON_3, PAD_X}, {AKEYCODE_BUTTON_4, PAD_Y},
    {AKEYCODE_BUTTON_5, PAD_L}, {AKEYCODE_BUTTON_6, PAD_Z},
    {AKEYCODE_BUTTON_7, PAD_L}, {AKEYCODE_BUTTON_8, PAD_R},
    {AKEYCODE_BUTTON_10, PAD_START},
};
static_assert(std::size(buttons) <= 32);

float value(const AInputEvent* event, Axis axis) {
    return AMotionEvent_getAxisValue(event, axes[axis], 0);
}

uint8_t stick(const AInputEvent* event, const Controller& pad, Axis axis, bool invert) {
    const Range& r = pad.ranges[axis];
    if (!r.available()) return 128;
    const float center = (r.min + r.max) * 0.5f;
    const float half = (r.max - r.min) * 0.5f;
    const float flat = std::min(std::max(r.flat, half * 0.12f), half * 0.95f);
    const float offset = value(event, axis) - center;
    if (std::abs(offset) <= flat) return 128;
    const float normalized = std::clamp((std::abs(offset) - flat) / (half - flat), 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(128 + 127 * std::copysign(normalized, invert ? -offset : offset)));
}

uint8_t trigger(const AInputEvent* event, const Controller& pad, Axis axis) {
    const Range& r = pad.ranges[axis];
    if (!r.available()) return 0;
    const float span = r.max - r.min;
    const float flat = std::clamp(r.flat, span * 0.02f, span * 0.95f);
    const float normalized = std::clamp((value(event, axis) - r.min - flat) / (span - flat), 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(255 * normalized));
}
} // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_ethannicholas_mkdd_TandemActivity_nativeGamepadChanged(JNIEnv*, jclass, jint id) {
    std::lock_guard<std::mutex> lock(controllers_mutex);
    controllers.erase(id);
    if (active_device == id) active_device = -1;
}

void android_gamepad_clear() {
    std::lock_guard<std::mutex> lock(controllers_mutex);
    for (auto& [id, pad] : controllers) {
        pad.keys = 0;
        pad.motion = {};
    }
    active_device = -1;
}

bool android_gamepad_event(ANativeActivity* activity, const AInputEvent* event) {
    const auto source = AInputEvent_getSource(event);
    const int id = AInputEvent_getDeviceId(event);
    const auto from = [source](int kind) { return (source & kind) == kind; };
    const bool joystick = from(AINPUT_SOURCE_JOYSTICK);
    const auto type = AInputEvent_getType(event);

    if (type == AINPUT_EVENT_TYPE_KEY &&
        (from(AINPUT_SOURCE_GAMEPAD) || from(AINPUT_SOURCE_DPAD) || joystick)) {
        for (size_t i = 0; i < std::size(buttons); ++i) {
            if (AKeyEvent_getKeyCode(event) != buttons[i].code) continue;
            std::lock_guard<std::mutex> lock(controllers_mutex);
            auto& pad = controllers[id];
            if (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN) {
                pad.keys |= uint32_t{1} << i;
                if (AKeyEvent_getRepeatCount(event) == 0) active_device = id;
            } else if (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_UP) {
                pad.keys &= ~(uint32_t{1} << i);
            }
            return true;
        }
    }

    if (type != AINPUT_EVENT_TYPE_MOTION || !joystick ||
        AMotionEvent_getAction(event) != AMOTION_EVENT_ACTION_MOVE) return false;

    std::lock_guard<std::mutex> lock(controllers_mutex);
    auto& pad = controllers[id];
    if (!pad.queried) query_ranges(activity, event, pad);
    PadState& p = pad.motion;
    const bool zr = pad.ranges[Z].available() && pad.ranges[RZ].available() &&
        pad.ranges[Z].min < 0 && pad.ranges[RZ].min < 0;
    p.stick_x = stick(event, pad, X, false);
    p.stick_y = stick(event, pad, Y, true);
    p.cstick_x = stick(event, pad, zr ? Z : RX, false);
    p.cstick_y = stick(event, pad, zr ? RZ : RY, true);
    p.trig_l = std::max(trigger(event, pad, LTrigger), trigger(event, pad, Brake));
    p.trig_r = std::max(trigger(event, pad, RTrigger), trigger(event, pad, Gas));
    p.buttons = 0;
    if (p.trig_l >= 128) p.buttons |= PAD_L;
    if (p.trig_r >= 128) p.buttons |= PAD_R;
    if (pad.ranges[HatX].available()) {
        const float hat = value(event, HatX);
        if (hat < -0.5f) p.buttons |= PAD_LEFT;
        if (hat > 0.5f) p.buttons |= PAD_RIGHT;
    }
    if (pad.ranges[HatY].available()) {
        const float hat = value(event, HatY);
        if (hat < -0.5f) p.buttons |= PAD_UP;
        if (hat > 0.5f) p.buttons |= PAD_DOWN;
    }
    if (p.buttons || p.trig_l > 32 || p.trig_r > 32 ||
        std::abs(int(p.stick_x) - 128) > 24 || std::abs(int(p.stick_y) - 128) > 24 ||
        std::abs(int(p.cstick_x) - 128) > 24 || std::abs(int(p.cstick_y) - 128) > 24 ||
        active_device == -1) active_device = id;
    return true;
}

void android_gamepad_merge(PadState& pad) {
    std::lock_guard<std::mutex> lock(controllers_mutex);
    auto it = controllers.find(active_device);
    if (it == controllers.end()) return;
    const Controller& controller = it->second;
    const PadState& p = controller.motion;
    pad.buttons |= p.buttons;
    for (size_t i = 0; i < std::size(buttons); ++i)
        if (controller.keys & (uint32_t{1} << i)) {
            pad.buttons |= buttons[i].mask;
            // Digital-only shoulder/trigger events still have a full press.
            if (buttons[i].mask == PAD_L && p.trig_l == 0) pad.trig_l = 255;
            if (buttons[i].mask == PAD_R && p.trig_r == 0) pad.trig_r = 255;
        }
    if (p.stick_x != 128 || p.stick_y != 128) {
        pad.stick_x = p.stick_x;
        pad.stick_y = p.stick_y;
    }
    pad.cstick_x = p.cstick_x;
    pad.cstick_y = p.cstick_y;
    pad.trig_l = std::max(pad.trig_l, p.trig_l);
    pad.trig_r = std::max(pad.trig_r, p.trig_r);
}
