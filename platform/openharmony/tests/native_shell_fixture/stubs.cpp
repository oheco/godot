// Godot Engine contributors. SPDX-License-Identifier: MIT
// Only native platform/engine boundaries are mocked. The test compiles the
// actual NAPI callbacks and runtime_paths.cpp against the target SDK headers.
#include "engine_host_openharmony.h"

#include <native_window/external_window.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
int state = 0;
bool stopped = false;
GodotOpenExternalCallback external_callback = nullptr;
const auto ui_thread = std::this_thread::get_id();
} // namespace

extern "C" {
NativeResourceManager *OH_ResourceManager_InitNativeResourceManager(napi_env, napi_value) {
	return reinterpret_cast<NativeResourceManager *>(1);
}
void OH_ResourceManager_ReleaseNativeResourceManager(NativeResourceManager *) {
	assert(stopped);
}
int32_t OH_NativeWindow_CreateNativeWindowFromSurfaceId(uint64_t id, OHNativeWindow **window) {
	assert(id == 23);
	*window = reinterpret_cast<OHNativeWindow *>(2);
	return 0;
}
void OH_NativeWindow_DestroyNativeWindow(OHNativeWindow *) {
	assert(stopped);
}
int godot_host_start(NativeResourceManager *resources, void *window, int32_t id, int32_t width, int32_t height,
		int argc, const char *const *argv, const char *permissions, bool packaged_game) {
	assert(state == 0 && !stopped && resources && window && id == 5 && width == 800 && height == 600);
	assert(argc == 2 && !strcmp(argv[0], "--path") && !strcmp(argv[1], "test project"));
	assert(!strcmp(permissions, getenv("GODOT_TEST_EXPECT_PERMISSIONS")));
	assert(packaged_game == (getenv("GODOT_TEST_PACKAGED") != nullptr));
	if (getenv("GODOT_TEST_FAIL_START")) {
		state = -7;
		return -7;
	}
	if (getenv("GODOT_TEST_EXTERNAL")) {
		assert(external_callback);
		// This worker must return while JS is still inside native startup.
		std::thread worker([]() {
			for (int kind = 0; kind < 3; ++kind) {
				assert(external_callback(kind, "/storage/Users/currentUser/test 中文 'folder'") == 0);
			}
		});
		worker.join();
	}
	state = 2;
	return 0;
}
void godot_host_stop() {
	stopped = true;
	if (state >= 0) {
		state = 3;
	}
}
int godot_host_state() {
	return state;
}
void godot_host_set_open_external_callback(GodotOpenExternalCallback callback) {
	external_callback = callback;
}
int32_t godot_host_open_terminal(const char *uri, char *diagnostic, uint32_t capacity) {
	assert(std::this_thread::get_id() != ui_thread);
	assert(capacity > 0);
	if (!strcmp(uri, "file://failure")) {
		strncpy(diagnostic, "fixture broker unavailable", capacity - 1);
		return -1;
	}
	return 0;
}
void godot_host_set_create_instance_callback(GodotCreateInstanceCallback) {}
int32_t godot_host_create_instance(int, const char *const *) {
	return -1;
}
void godot_host_touch(const GodotTouchEvent *events, int count) {
	assert(count >= 0 && count <= 65536);
	for (int i = 0; i < count; ++i) {
		assert(events[i].type <= 3 && events[i].id < 32);
	}
	if (const char *expected = getenv("GODOT_TEST_TOUCH_COUNT")) {
		assert(count == atoi(expected));
	}
}
void godot_host_mouse(const GodotMouseEvent *) {}
void godot_host_key(const GodotKeyEvent *) {}
void godot_host_set_surface_position(int32_t id, double x, double y) {
	assert(id == 5 && std::this_thread::get_id() == ui_thread);
	const char *expected = getenv("GODOT_TEST_SURFACE_POSITION");
	assert(expected);
	double expected_x, expected_y;
	assert(std::sscanf(expected, "%lf,%lf", &expected_x, &expected_y) == 2);
	assert(x == expected_x && y == expected_y);
}
void godot_host_resize(int32_t, int32_t) {}
void godot_host_window_event(int32_t) {}
}
