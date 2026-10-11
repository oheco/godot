// Godot Engine contributors. SPDX-License-Identifier: MIT
// Compile the real wrapper with only WindowProperties/display origins mocked.
// Cursor/TextConfig use the device's IME C API; never attach or show IME here.
#include "wrapper_openharmony.h"

#include <inputmethod/inputmethod_controller_capi.h>
#include <window_manager/oh_display_manager.h>
#include <window_manager/oh_window.h>

#include <cassert>
#include <cstdio>
#include <future>
#include <limits>
#include <thread>

namespace {
const auto ui_thread = std::this_thread::get_id();
int property_calls = 0;
int origin_calls = 0;
int property_result = 0;
NativeDisplayManager_ErrorCode origin_result = DISPLAY_MANAGER_OK;
uint32_t property_display_id = 0;

void display_origin(uint32_t id, int32_t &x, int32_t &y) {
	assert(id <= 2);
	x = id == 1 ? 2560 : id == 2 ? -1920 : 0;
	y = id == 2 ? -1080 : 0;
}

void assert_mapping(uint32_t id, double global_x, double global_y, double local_x, double local_y, double expected_x, double expected_y) {
	property_display_id = id;
	ohos_wrapper_set_surface_position(7, global_x, global_y);
	WrapperWindowGeometry geometry;
	assert(ohos_wrapper_get_window_geometry(7, geometry));
	int32_t origin_x, origin_y;
	display_origin(id, origin_x, origin_y);
	assert(geometry.window_position_valid && geometry.display_position_valid);
	assert(geometry.display_id == id && geometry.display_x == origin_x && geometry.display_y == origin_y);
	// Window/mouse readers keep global desktop coordinates, independent of IME.
	assert(geometry.surface_x == global_x && geometry.surface_y == global_y);
	assert(geometry.window_x == origin_x + 111 && geometry.window_y == origin_y + 222);
	double x = -999, y = -999;
	uint32_t mapped_id = std::numeric_limits<uint32_t>::max();
	assert(ohos_wrapper_map_surface_point(7, local_x, local_y, x, y, &mapped_id));
	assert(x == expected_x && y == expected_y && mapped_id == id);
	assert(ohos_wrapper_map_surface_point(7, local_x, local_y, x, y));
	assert(x == expected_x && y == expected_y); // Optional display ID preserves callers.
}

void assert_unmapped(int32_t window_id, double local_x = 120, double local_y = 60) {
	double x = 123.5, y = -456.25;
	uint32_t id = std::numeric_limits<uint32_t>::max();
	assert(!ohos_wrapper_map_surface_point(window_id, local_x, local_y, x, y, &id));
	assert(x == 123.5 && y == -456.25 && id == std::numeric_limits<uint32_t>::max());
}
} // namespace

extern "C" int32_t OH_WindowManager_GetWindowProperties(int32_t id, WindowManager_WindowProperties *properties) {
	assert(std::this_thread::get_id() == ui_thread);
	assert(id == 7 || id == 8);
	++property_calls;
	int32_t origin_x, origin_y;
	display_origin(property_display_id, origin_x, origin_y);
	properties->windowRect = { origin_x + 111, origin_y + 222, 2000, 1500 };
	// A stage paint rectangle is not the actual XComponent/surface origin.
	properties->drawableRect = { 17, 80, 1966, 1400 };
	properties->displayId = property_display_id;
	// Write fields even on failure: the caller must not adopt invalid metadata.
	return property_result;
}

extern "C" NativeDisplayManager_ErrorCode OH_NativeDisplayManager_GetDisplayPosition(uint64_t id, int32_t *x, int32_t *y) {
	assert(std::this_thread::get_id() == ui_thread);
	assert(id == property_display_id);
	++origin_calls;
	display_origin(static_cast<uint32_t>(id), *x, *y);
	if (origin_result != DISPLAY_MANAGER_OK) {
		*x = 9876; // Failed SDK calls may partially overwrite output arguments.
	}
	return origin_result;
}

int main() {
	WrapperWindowGeometry geometry;
	assert(!ohos_wrapper_get_window_geometry(7, geometry));
	assert_unmapped(7);
	assert_unmapped(8);

	// The same physical screen anchor has three different global desktop origins.
	// Fractional surface/caret pixels must survive without density/drawable offsets.
	assert_mapping(0, 1500.25, 420.5, 120, 60, 1620.25, 480.5);
	assert_mapping(1, 4060.25, 420.5, 120, 60, 1620.25, 480.5);
	assert_mapping(2, -419.75, -659.5, 120, 60, 1620.25, 480.5);
	assert_mapping(1, 2479.5, 35.25, 12.125, 100.5, -68.375, 135.75);
	assert_mapping(2, -2000.5, -1044.75, 12.125, 100.5, -68.375, 135.75);
	assert(property_calls == 5 && origin_calls == 5);
	assert_unmapped(8);
	assert_unmapped(7, std::numeric_limits<double>::infinity(), 0);
	assert_unmapped(7, 0, std::numeric_limits<double>::quiet_NaN());
	ohos_wrapper_set_surface_position(7, std::numeric_limits<double>::quiet_NaN(), 0);
	ohos_wrapper_set_surface_position(-1, 0, 0);
	assert(property_calls == 5 && origin_calls == 5);

	// Invalid properties replace a previous valid snapshot, without its old origin.
	property_result = WINDOW_MANAGER_ERRORCODE_STATE_ABNORMAL;
	property_display_id = 1;
	ohos_wrapper_set_surface_position(7, 600.5, 700.25);
	assert(ohos_wrapper_get_window_geometry(7, geometry));
	assert(geometry.surface_x == 600.5 && geometry.surface_y == 700.25);
	assert(!geometry.window_position_valid && !geometry.display_position_valid);
	assert(origin_calls == 5); // Invalid WindowProperties never starts an origin query.
	assert_unmapped(7);
	property_result = 0;

	// A valid display ID alone is insufficient; failed origin queries cannot use
	// either the preceding display's origin or a fabricated primary display 0.
	origin_result = static_cast<NativeDisplayManager_ErrorCode>(-1);
	for (uint32_t id : { 0u, 1u, 2u }) {
		property_display_id = id;
		ohos_wrapper_set_surface_position(7, 800.25 + id, -90.5);
		assert(ohos_wrapper_get_window_geometry(7, geometry));
		assert(geometry.window_position_valid && !geometry.display_position_valid);
		assert(geometry.display_id == id && geometry.surface_x == 800.25 + id && geometry.surface_y == -90.5);
		assert_unmapped(7);
	}
	origin_result = DISPLAY_MANAGER_OK;
	assert_mapping(0, 600.5, 700.25, 120, 60, 720.5, 760.25);

	// UI updates alternate complete valid/invalid snapshots. Engine/Binder reads
	// see screen coordinates and display ID from the same snapshot, never query UI.
	assert_mapping(0, 0.25, -0.25, 0, 0, 0.25, -0.25);
	std::promise<void> ready;
	auto started = ready.get_future();
	std::thread reader([&ready]() {
		double x = 0, y = 0;
		uint32_t id = 99;
		assert(ohos_wrapper_map_surface_point(7, 12.5, 25.75, x, y, &id));
		ready.set_value();
		for (int i = 0; i < 50000; ++i) {
			x = 123.5;
			y = -456.25;
			id = 99;
			if (ohos_wrapper_map_surface_point(7, 12.5, 25.75, x, y, &id)) {
				assert(id <= 2 && x + y == 38.25);
				const double sequence = x - 12.5 - 10000 * id;
				assert(sequence >= 0.25 && sequence <= 1999.25);
			} else {
				assert(x == 123.5 && y == -456.25 && id == 99);
			}
			WrapperWindowGeometry snapshot;
			assert(ohos_wrapper_get_window_geometry(7, snapshot));
			if (snapshot.display_position_valid) {
				int32_t origin_x, origin_y;
				display_origin(snapshot.display_id, origin_x, origin_y);
				assert(snapshot.window_position_valid);
				assert(snapshot.display_x == origin_x && snapshot.display_y == origin_y);
				assert(snapshot.window_x == origin_x + 111 && snapshot.window_y == origin_y + 222);
				assert(snapshot.surface_x + snapshot.surface_y == origin_x + origin_y);
			}
		}
	});
	started.wait();
	for (int i = 0; i < 2000; ++i) {
		property_display_id = i % 3;
		int32_t origin_x, origin_y;
		display_origin(property_display_id, origin_x, origin_y);
		property_result = i % 31 == 0 ? WINDOW_MANAGER_ERRORCODE_STATE_ABNORMAL : 0;
		origin_result = i % 37 == 0 ? static_cast<NativeDisplayManager_ErrorCode>(-1) : DISPLAY_MANAGER_OK;
		const double local = 10000 * property_display_id + i + 0.25;
		ohos_wrapper_set_surface_position(7, origin_x + local, origin_y - local);
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
	puts("PASS native IME geometry: primary/right/above-left display origins and IDs, unchanged global positions, fractional px, invalid metadata, coherent threads; real cursor/TextConfig C API");
}
