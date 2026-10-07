// Godot Engine contributors. SPDX-License-Identifier: MIT
// Compile the real wrapper with only WindowProperties mocked. Cursor/TextConfig
// objects use the device's actual IME C API; this test never attaches or shows IME.
#include "wrapper_openharmony.h"

#include <inputmethod/inputmethod_controller_capi.h>
#include <window_manager/oh_window.h>

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <future>
#include <limits>
#include <thread>

namespace {
const auto ui_thread = std::this_thread::get_id();
int property_calls = 0;
int property_result = 0;
} // namespace

extern "C" int32_t OH_WindowManager_GetWindowProperties(int32_t id, WindowManager_WindowProperties *properties) {
	assert(std::this_thread::get_id() == ui_thread);
	assert(id == 7 || id == 8);
	++property_calls;
	if (property_result != 0) {
		return property_result;
	}
	properties->windowRect = { -100, 350, 2000, 1500 };
	// A stage paint rectangle is not the actual XComponent/surface origin.
	properties->drawableRect = { 17, 80, 1966, 1400 };
	return 0;
}

int main() {
	WrapperWindowGeometry geometry;
	double x = 0, y = 0;
	assert(!ohos_wrapper_get_window_geometry(7, geometry));
	assert(!ohos_wrapper_map_surface_point(7, 10, 20, x, y));
	ohos_wrapper_set_surface_position(7, 1500.25, 420.5);
	assert(ohos_wrapper_get_window_geometry(7, geometry));
	assert(geometry.window_position_valid && geometry.window_x == -100 && geometry.window_y == 350);
	assert(ohos_wrapper_map_surface_point(7, 120, 60, x, y));
	assert(x == 1620.25 && y == 480.5);
	assert(property_calls == 1); // Engine/Binder reads never query ArkUI properties.
	assert(!ohos_wrapper_map_surface_point(8, 120, 60, x, y));
	assert(!ohos_wrapper_map_surface_point(7, std::numeric_limits<double>::infinity(), 0, x, y));
	ohos_wrapper_set_surface_position(7, std::numeric_limits<double>::quiet_NaN(), 0);
	assert(property_calls == 1);

	// Moving/negative screen coordinates remain physical pixels. No density or
	// drawableRect offset is added a second time to the submitted surface origin.
	ohos_wrapper_set_surface_position(7, -80.5, 35.25);
	assert(ohos_wrapper_map_surface_point(7, 12, 100, x, y));
	assert(x == -68.5 && y == 135.25);
	property_result = WINDOW_MANAGER_ERRORCODE_STATE_ABNORMAL;
	ohos_wrapper_set_surface_position(7, 600.5, 700.25);
	assert(ohos_wrapper_get_window_geometry(7, geometry));
	assert(!geometry.window_position_valid);
	assert(ohos_wrapper_map_surface_point(7, 120, 60, x, y));
	assert(x == 720.5 && y == 760.25);
	property_result = 0;

	// Coherent x/y snapshots when the UI updates during native/Binder reads.
	ohos_wrapper_set_surface_position(7, 0, 0);
	std::promise<void> ready;
	auto started = ready.get_future();
	std::thread reader([&ready]() {
		double screen_x, screen_y;
		assert(ohos_wrapper_map_surface_point(7, 0, 0, screen_x, screen_y));
		ready.set_value();
		for (int i = 0; i < 20000; ++i) {
			assert(ohos_wrapper_map_surface_point(7, 0, 0, screen_x, screen_y));
			assert(screen_x + screen_y == 0);
		}
	});
	started.wait();
	for (int i = 0; i < 2000; ++i) {
		ohos_wrapper_set_surface_position(7, i + 0.25, -i - 0.25);
	}
	reader.join();

	// Native C API permits the zero-sized popup anchor and borrowed cursor/avoid
	// mutation in GetTextConfig, including fractional physical screen positions.
	auto *cursor = OH_CursorInfo_Create(720.5, 760.25, 0, 0);
	assert(cursor != nullptr);
	double left, top, width, height;
	assert(OH_CursorInfo_GetRect(cursor, &left, &top, &width, &height) == IME_ERR_OK);
	assert(left == 720.5 && top == 760.25 && width == 0 && height == 0);
	OH_CursorInfo_Destroy(cursor);
	auto *config = OH_TextConfig_Create();
	assert(config != nullptr);
	assert(OH_TextConfig_SetWindowId(config, 7) == IME_ERR_OK);
	int32_t window_id;
	assert(OH_TextConfig_GetWindowId(config, &window_id) == IME_ERR_OK && window_id == 7);
	cursor = nullptr;
	assert(OH_TextConfig_GetCursorInfo(config, &cursor) == IME_ERR_OK && cursor != nullptr);
	assert(OH_CursorInfo_SetRect(cursor, 720.5, 760.25, 0, 0) == IME_ERR_OK);
	assert(OH_CursorInfo_GetRect(cursor, &left, &top, &width, &height) == IME_ERR_OK);
	assert(left == 720.5 && top == 760.25 && width == 0 && height == 0);
	InputMethod_TextAvoidInfo *avoid = nullptr;
	assert(OH_TextConfig_GetTextAvoidInfo(config, &avoid) == IME_ERR_OK && avoid != nullptr);
	assert(OH_TextAvoidInfo_SetPositionY(avoid, 701.5) == IME_ERR_OK);
	assert(OH_TextAvoidInfo_SetHeight(avoid, 58.25) == IME_ERR_OK);
	assert(OH_TextAvoidInfo_GetPositionY(avoid, &top) == IME_ERR_OK && top == 701.5);
	assert(OH_TextAvoidInfo_GetHeight(avoid, &height) == IME_ERR_OK && height == 58.25);
	assert(OH_TextAvoidInfo_SetHeight(avoid, 0) == IME_ERR_OK);
	assert(OH_TextAvoidInfo_GetHeight(avoid, &height) == IME_ERR_OK && height == 0);
	OH_TextConfig_Destroy(config);
	puts("PASS native IME geometry: window/surface offsets, negative/fractional px, coherent threads, no UI getter dispatch; real cursor/TextConfig C API");
}
