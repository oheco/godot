// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

#include "bridge_openharmony.h"

#include <rawfile/raw_file_manager.h>

extern "C" {
typedef int32_t (*GodotCreateInstanceCallback)(int argc, const char *const *argv);
void godot_host_set_create_instance_callback(GodotCreateInstanceCallback callback);
int32_t godot_host_create_instance(int argc, const char *const *argv);

enum GodotExternalKind {
	GODOT_EXTERNAL_URI = 0,
	GODOT_EXTERNAL_FOLDER = 1,
	GODOT_EXTERNAL_TERMINAL = 2,
};
// Returns queue acceptance, never waits for the external application to start.
typedef int32_t (*GodotOpenExternalCallback)(int32_t kind, const char *target);
void godot_host_set_open_external_callback(GodotOpenExternalCallback callback);
int32_t godot_host_open_external(int32_t kind, const char *target);
// Synchronous broker operation: call only from a background worker, never the
// engine/ArkUI thread. Uses the installed shell service and system aa command.
int32_t godot_host_open_terminal(const char *uri, char *diagnostic, uint32_t capacity);

// Exactly one engine lifetime per process: exited/failed/stopped hosts cannot
// restart. Arguments and comma-separated granted permissions are copied.
// resources/window remain owned by the caller until godot_host_stop() joins.
// packaged_game reads rawfile _cl_ before argv and defaults to template.pck;
// otherwise argv alone selects the editor, project manager or project run.
int godot_host_start(NativeResourceManager *resources, void *window, int32_t window_id,
		int32_t width, int32_t height, int argc, const char *const *argv,
		const char *granted_permissions, bool packaged_game);
// Stop/join is idempotent; call from the UI/owner thread, not the engine thread.
void godot_host_stop();
// 0: not started, 1: loading, 2: running, 3: exited/stopped; negative: error.
int godot_host_state();
void godot_host_touch(const GodotTouchEvent *events, int count);
void godot_host_mouse(const GodotMouseEvent *event);
void godot_host_key(const GodotKeyEvent *event);
void godot_host_resize(int32_t width, int32_t height);
// UI-thread snapshot of the actual XComponent surface origin in screen px.
void godot_host_set_surface_position(int32_t window_id, double x, double y);
void godot_host_window_event(int32_t event);
}
