#pragma once

#include <android/input.h>
#include <android/native_activity.h>

#include "hw/pad.h"

bool android_gamepad_event(ANativeActivity* activity, const AInputEvent* event);
void android_gamepad_merge(PadState& pad);
void android_gamepad_clear();
