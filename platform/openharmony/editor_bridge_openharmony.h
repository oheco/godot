// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once
#include "bridge_openharmony.h"

extern "C" {
typedef int32_t (*GodotCreateInstanceCallback)(int argc, const char *const *argv);
void godot_editor_set_create_instance_callback(GodotCreateInstanceCallback callback);
int32_t godot_editor_create_instance(int argc, const char *const *argv);
// One Godot instance per UIAbility process. Arguments are copied before returning.
int godot_editor_start(NativeResourceManager *resources, void *window, int32_t window_id,
		int32_t width, int32_t height, int argc, const char *const *argv);
void godot_editor_stop();
// 0: not started, 1: loading, 2: running, 3: exited; negative: startup error.
int godot_editor_state();
void godot_editor_touch(const GodotTouchEvent *events, int count);
void godot_editor_mouse(const GodotMouseEvent *event);
void godot_editor_key(const GodotKeyEvent *event);
void godot_editor_resize(int32_t width, int32_t height);
void godot_editor_window_event(int32_t event);
}
