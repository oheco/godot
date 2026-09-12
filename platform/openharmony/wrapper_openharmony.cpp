/**************************************************************************/
/*  wrapper_openharmony.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "wrapper_openharmony.h"

#include <window_manager/oh_display_manager.h>
#include <window_manager/oh_window.h>

int ohos_wrapper_get_display_dpi() {
	int32_t dpi = 0;
	OH_NativeDisplayManager_GetDefaultDisplayDensityDpi(&dpi);
	return dpi;
}

float ohos_wrapper_get_display_scaled_density() {
	float scaled_density = 0;
	OH_NativeDisplayManager_GetDefaultDisplayScaledDensity(&scaled_density);
	return scaled_density;
}

float ohos_wrapper_get_display_refresh_rate() {
	uint32_t refresh_rate = 0;
	OH_NativeDisplayManager_GetDefaultDisplayRefreshRate(&refresh_rate);
	return refresh_rate;
}

WrapperScreenOrientation ohos_wrapper_get_display_orientation() {
	NativeDisplayManager_Orientation orientation = DISPLAY_MANAGER_PORTRAIT;
	OH_NativeDisplayManager_GetDefaultDisplayOrientation(&orientation);
	switch (orientation) {
		case DISPLAY_MANAGER_PORTRAIT:
			return WrapperScreenOrientation::WRAPPER_SCREEN_PORTRAIT;
		case DISPLAY_MANAGER_LANDSCAPE:
			return WrapperScreenOrientation::WRAPPER_SCREEN_LANDSCAPE;
		case DISPLAY_MANAGER_PORTRAIT_INVERTED:
			return WrapperScreenOrientation::WRAPPER_SCREEN_REVERSE_PORTRAIT;
		case DISPLAY_MANAGER_LANDSCAPE_INVERTED:
			return WrapperScreenOrientation::WRAPPER_SCREEN_REVERSE_LANDSCAPE;
		default:
			return WrapperScreenOrientation::WRAPPER_SCREEN_PORTRAIT;
	}
}

void ohos_wrapper_screen_set_keep_on(int32_t window_id, bool p_enable) {
	OH_WindowManager_SetWindowKeepScreenOn(window_id, p_enable);
}

bool ohos_wrapper_screen_is_kept_on(int32_t window_id) {
	WindowManager_WindowProperties properties = {};
	OH_WindowManager_GetWindowProperties(window_id, &properties);
	return properties.isKeepScreenOn;
}

int ohos_wrapper_get_keyboard_avoid_area(int32_t window_id) {
	WindowManager_AvoidArea area = {};
	OH_WindowManager_GetWindowAvoidArea(window_id, WINDOW_MANAGER_AVOID_AREA_TYPE_KEYBOARD, &area);
	return area.bottomRect.height;
}

// Keep SDK enums (including global OK) out of Godot translation units.
#include <multimodalinput/oh_input_manager.h>
#include <multimodalinput/oh_pointer_style.h>

int ohos_wrapper_set_mouse_mode(int32_t window_id, bool locked, bool follow, bool visible) {
	int result = locked ? OH_WindowManager_LockCursor(window_id, follow) : OH_WindowManager_UnlockCursor(window_id);
	if (result != 0) {
		return result;
	}
	return OH_Input_SetPointerVisible(visible);
}
int ohos_wrapper_set_cursor_shape(int32_t window_id, int shape) {
	static const int styles[] = {
		Input_PointerStyle::DEFAULT,
		Input_PointerStyle::TEXT_CURSOR,
		Input_PointerStyle::HAND_POINTING,
		Input_PointerStyle::CROSS,
		Input_PointerStyle::LOADING,
		Input_PointerStyle::RUNNING,
		Input_PointerStyle::HAND_GRABBING,
		Input_PointerStyle::CURSOR_COPY,
		Input_PointerStyle::CURSOR_FORBID,
		Input_PointerStyle::NORTH_SOUTH,
		Input_PointerStyle::WEST_EAST,
		Input_PointerStyle::NORTH_EAST_SOUTH_WEST,
		Input_PointerStyle::NORTH_WEST_SOUTH_EAST,
		Input_PointerStyle::MOVE,
		Input_PointerStyle::RESIZE_UP_DOWN,
		Input_PointerStyle::RESIZE_LEFT_RIGHT,
		Input_PointerStyle::HELP,
	};
	if (shape < 0 || shape >= int(sizeof(styles) / sizeof(styles[0]))) {
		return -1;
	}
	return OH_Input_SetPointerStyle(window_id, styles[shape]);
}
