/**************************************************************************/
/*  display_server_openharmony.cpp                                        */
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

#include "display_server_openharmony.h"

#include "engine_host_openharmony.h"
#include "os_openharmony.h"
#include "rendering_context_driver_vulkan_openharmony.h"
#include "wrapper_openharmony.h"

#include "core/input/input.h"
#include "core/input/input_event.h"
#include "servers/display/native_menu.h"
#include "servers/rendering/renderer_rd/renderer_compositor_rd.h"
#include "servers/rendering/rendering_device.h"

#ifdef GLES3_ENABLED
#include "gl_manager_openharmony.h"

#include "drivers/gles3/rasterizer_gles3.h"
#endif

#include <database/pasteboard/oh_pasteboard.h>
#include <database/udmf/udmf.h>
#include <database/udmf/uds.h>

#include <memory>

void DisplayServerOpenHarmony::_dispatch_input_events(const Ref<InputEvent> &p_event) {
	get_singleton()->send_input_event(p_event);
}

DisplayServerOpenHarmony *DisplayServerOpenHarmony::get_singleton() {
	return static_cast<DisplayServerOpenHarmony *>(DisplayServer::get_singleton());
}

Vector<String> DisplayServerOpenHarmony::get_rendering_drivers_func() {
	Vector<String> drivers;
	drivers.push_back("vulkan");
#ifdef GLES3_ENABLED
	drivers.push_back("opengl3");
#endif
	return drivers;
}

DisplayServer *DisplayServerOpenHarmony::create_func(const String &p_rendering_driver, DisplayServerEnums::WindowMode p_mode, DisplayServerEnums::VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i *p_position, const Vector2i &p_resolution, int p_screen, DisplayServerEnums::Context p_context, int64_t p_parent_window, Error &r_error) {
	DisplayServer *ds = memnew(DisplayServerOpenHarmony(p_rendering_driver, p_mode, p_vsync_mode, p_flags, p_position, p_resolution, p_screen, p_context, p_parent_window, r_error));
	if (r_error != OK) {
		const String driver_name = p_rendering_driver == "opengl3" ? "OpenGL ES 3" : p_rendering_driver;
		OS::get_singleton()->alert(vformat("Unable to initialize the %s video driver. See the engine log for details.", driver_name));
	}
	return ds;
}

void DisplayServerOpenHarmony::register_openharmony_driver() {
	register_create_function("openharmony", create_func, get_rendering_drivers_func);
}

DisplayServerOpenHarmony::DisplayServerOpenHarmony(const String &p_rendering_driver, DisplayServerEnums::WindowMode p_mode, DisplayServerEnums::VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i *p_position, const Vector2i &p_resolution, int p_screen, DisplayServerEnums::Context p_context, int64_t p_parent_window, Error &r_error) {
	rendering_driver = p_rendering_driver;

	rendering_context = nullptr;
	rendering_device = nullptr;

	// The editor calls NativeMenu::get_singleton() without checking it, so a menu
	// object must always exist; the plain implementation reports that this platform
	// supports no global menu features.
	native_menu = memnew(NativeMenu);

	OHNativeWindow *native_window = OS_OpenHarmony::get_singleton()->get_native_window();
	if (!native_window) {
		ERR_PRINT("Cannot initialize a rendering driver without an OpenHarmony native window.");
		r_error = ERR_UNAVAILABLE;
		return;
	}

	if (rendering_driver == "vulkan") {
		rendering_context = memnew(RenderingContextDriverVulkanOpenHarmony);
		if (rendering_context->initialize() != OK) {
			memdelete(rendering_context);
			rendering_context = nullptr;
			ERR_PRINT("Failed to initialize Vulkan context.");
			r_error = ERR_UNAVAILABLE;
			return;
		}
		RenderingContextDriverVulkanOpenHarmony::WindowPlatformData vulkan;
		vulkan.window = native_window;

		if (rendering_context->window_create(DisplayServerEnums::MAIN_WINDOW_ID, &vulkan) != OK) {
			ERR_PRINT("Failed to create Vulkan window.");
			memdelete(rendering_context);
			rendering_context = nullptr;
			r_error = ERR_UNAVAILABLE;
			return;
		}

		Size2i display_size = OS_OpenHarmony::get_singleton()->get_display_size();
		rendering_context->window_set_size(DisplayServerEnums::MAIN_WINDOW_ID, display_size.width, display_size.height);
		rendering_context->window_set_vsync_mode(DisplayServerEnums::MAIN_WINDOW_ID, p_vsync_mode);

		rendering_device = memnew(RenderingDevice);
		if (rendering_device->initialize(rendering_context, DisplayServerEnums::MAIN_WINDOW_ID) != OK) {
			memdelete(rendering_device);
			rendering_device = nullptr;
			memdelete(rendering_context);
			rendering_context = nullptr;
			r_error = ERR_UNAVAILABLE;
			return;
		}
		rendering_device->screen_create(DisplayServerEnums::MAIN_WINDOW_ID);
		RendererCompositorRD::make_current();
#ifdef GLES3_ENABLED
	} else if (rendering_driver == "opengl3") {
		gl_manager = memnew(GLManagerOpenHarmony);
		r_error = gl_manager->initialize(native_window, p_vsync_mode);
		if (r_error != OK) {
			return;
		}
		RasterizerGLES3::make_current(false);
#endif
	} else {
		ERR_PRINT(vformat("Unsupported OpenHarmony rendering driver: %s.", rendering_driver));
		r_error = ERR_UNAVAILABLE;
		return;
	}

	Input::get_singleton()->set_event_dispatch_function(_dispatch_input_events);

	// ArkUI owns the existing main window; apply the project's initial mode
	// through the same asynchronous path used by later script requests.
	window_set_mode(p_mode);
	r_error = OK;
}

DisplayServerOpenHarmony::~DisplayServerOpenHarmony() {
	virtual_keyboard_hide();
	if (mouse_mode_managed && godot_host_is_window_active_for_cleanup()) {
		ohos_wrapper_set_mouse_mode(OS_OpenHarmony::get_singleton()->get_window_id(), false, true, true);
	}
	if (rendering_device) {
		memdelete(rendering_device);
	}
	if (rendering_context) {
		memdelete(rendering_context);
	}
#ifdef GLES3_ENABLED
	if (gl_manager) {
		memdelete(gl_manager);
	}
#endif
	if (native_menu) {
		memdelete(native_menu);
		native_menu = nullptr;
	}
}

void DisplayServerOpenHarmony::release_rendering_thread() {
#ifdef GLES3_ENABLED
	if (gl_manager) {
		gl_manager->release_current();
	}
#endif
}

void DisplayServerOpenHarmony::gl_window_make_current(DisplayServerEnums::WindowID p_window_id) {
	// Off-screen viewports use the context already bound to the rendering thread.
	if (p_window_id == DisplayServerEnums::INVALID_WINDOW_ID) {
		return;
	}
	ERR_FAIL_COND(p_window_id != DisplayServerEnums::MAIN_WINDOW_ID);
#ifdef GLES3_ENABLED
	if (gl_manager) {
		gl_manager->make_current();
	}
#endif
}

void DisplayServerOpenHarmony::swap_buffers() {
#ifdef GLES3_ENABLED
	if (gl_manager) {
		gl_manager->swap_buffers();
	}
#endif
}

int64_t DisplayServerOpenHarmony::window_get_native_handle(DisplayServerEnums::HandleType p_handle_type, DisplayServerEnums::WindowID p_window) const {
	ERR_FAIL_COND_V(p_window != DisplayServerEnums::MAIN_WINDOW_ID, 0);
	if (p_handle_type == DisplayServerEnums::WINDOW_HANDLE) {
		return reinterpret_cast<int64_t>(OS_OpenHarmony::get_singleton()->get_native_window());
	}
#ifdef GLES3_ENABLED
	if (gl_manager) {
		switch (p_handle_type) {
			case DisplayServerEnums::DISPLAY_HANDLE:
			case DisplayServerEnums::EGL_DISPLAY:
				return reinterpret_cast<int64_t>(gl_manager->get_display());
			case DisplayServerEnums::OPENGL_CONTEXT:
				return reinterpret_cast<int64_t>(gl_manager->get_context());
			case DisplayServerEnums::EGL_CONFIG:
				return reinterpret_cast<int64_t>(gl_manager->get_config());
			default:
				break;
		}
	}
#endif
	return 0;
}

void DisplayServerOpenHarmony::_window_callback(const Callable &p_callable, const Variant &p_arg, bool p_deferred) const {
	if (p_callable.is_valid()) {
		if (p_deferred) {
			p_callable.call_deferred(p_arg);
		} else {
			p_callable.call(p_arg);
		}
	}
}

void DisplayServerOpenHarmony::send_input_event(const Ref<InputEvent> &p_event) const {
	_window_callback(input_event_callback, p_event);
}

void DisplayServerOpenHarmony::resize_window(uint32_t p_width, uint32_t p_height) {
	Size2i size = Size2i(p_width, p_height);
	OS_OpenHarmony::get_singleton()->set_display_size(size);

#if defined(RD_ENABLED)
	if (rendering_context) {
		rendering_context->window_set_size(DisplayServerEnums::MAIN_WINDOW_ID, size.x, size.y);
	}
#endif

	reported_window_rect = Rect2i(window_get_position(), size);
	reported_window_rect_valid = window_resize_callback.is_valid();
	_window_callback(window_resize_callback, reported_window_rect);
}

void DisplayServerOpenHarmony::send_window_event(DisplayServerEnums::WindowEvent p_event) const {
	_window_callback(window_event_callback, int(p_event));
}

bool DisplayServerOpenHarmony::has_feature(DisplayServerEnums::Feature p_feature) const {
	switch (p_feature) {
		case DisplayServerEnums::FEATURE_MOUSE:
		case DisplayServerEnums::FEATURE_CURSOR_SHAPE:
		case DisplayServerEnums::FEATURE_TOUCHSCREEN:
		case DisplayServerEnums::FEATURE_CLIPBOARD:
		case DisplayServerEnums::FEATURE_VIRTUAL_KEYBOARD:
		case DisplayServerEnums::FEATURE_IME:
		case DisplayServerEnums::FEATURE_KEEP_SCREEN_ON:
			return true;
		default:
			return false;
	}
}

String DisplayServerOpenHarmony::get_name() const {
	return "OpenHarmony";
}

int DisplayServerOpenHarmony::get_screen_count() const {
	return 1;
}

int DisplayServerOpenHarmony::get_primary_screen() const {
	return 0;
}

Point2i DisplayServerOpenHarmony::screen_get_position(int p_screen) const {
	return Point2i(0, 0);
}

Size2i DisplayServerOpenHarmony::screen_get_size(int p_screen) const {
	return OS_OpenHarmony::get_singleton()->get_display_size();
}

Rect2i DisplayServerOpenHarmony::screen_get_usable_rect(int p_screen) const {
	Size2i display_size = OS_OpenHarmony::get_singleton()->get_display_size();
	return Rect2i(0, 0, display_size.width, display_size.height);
}

int DisplayServerOpenHarmony::screen_get_dpi(int p_screen) const {
	return ohos_wrapper_get_display_dpi();
}

float DisplayServerOpenHarmony::screen_get_scale(int p_screen) const {
	return ohos_wrapper_get_display_scale();
}

float DisplayServerOpenHarmony::screen_get_refresh_rate(int p_screen) const {
	return ohos_wrapper_get_display_refresh_rate();
}

bool DisplayServerOpenHarmony::is_touchscreen_available() const {
	return true;
}

void DisplayServerOpenHarmony::screen_set_orientation(DisplayServerEnums::ScreenOrientation p_orientation, int p_screen) {
	// Not supported on OpenHarmony.
}

DisplayServerEnums::ScreenOrientation DisplayServerOpenHarmony::screen_get_orientation(int p_screen) const {
	switch (ohos_wrapper_get_display_orientation()) {
		case WrapperScreenOrientation::WRAPPER_SCREEN_LANDSCAPE:
			return DisplayServerEnums::SCREEN_LANDSCAPE;
		case WrapperScreenOrientation::WRAPPER_SCREEN_PORTRAIT:
			return DisplayServerEnums::SCREEN_PORTRAIT;
		case WrapperScreenOrientation::WRAPPER_SCREEN_REVERSE_LANDSCAPE:
			return DisplayServerEnums::SCREEN_REVERSE_LANDSCAPE;
		case WrapperScreenOrientation::WRAPPER_SCREEN_REVERSE_PORTRAIT:
			return DisplayServerEnums::SCREEN_REVERSE_PORTRAIT;
		default:
			return DisplayServerEnums::SCREEN_PORTRAIT;
	}
}

void DisplayServerOpenHarmony::clipboard_set(const String &p_text) {
	std::unique_ptr<OH_Pasteboard, decltype(&OH_Pasteboard_Destroy)> pasteboard(OH_Pasteboard_Create(), OH_Pasteboard_Destroy);
	std::unique_ptr<OH_UdsPlainText, decltype(&OH_UdsPlainText_Destroy)> text(OH_UdsPlainText_Create(), OH_UdsPlainText_Destroy);
	std::unique_ptr<OH_UdmfRecord, decltype(&OH_UdmfRecord_Destroy)> record(OH_UdmfRecord_Create(), OH_UdmfRecord_Destroy);
	std::unique_ptr<OH_UdmfData, decltype(&OH_UdmfData_Destroy)> data(OH_UdmfData_Create(), OH_UdmfData_Destroy);
	ERR_FAIL_COND_MSG(!pasteboard || !text || !record || !data, "Could not allocate clipboard data.");
	int status = OH_UdsPlainText_SetContent(text.get(), p_text.utf8().get_data());
	if (status == 0) {
		status = OH_UdmfRecord_AddPlainText(record.get(), text.get());
	}
	if (status == 0) {
		status = OH_UdmfData_AddRecord(data.get(), record.get());
	}
	ERR_FAIL_COND_MSG(status != 0, "Could not create clipboard text (UDMF error " + itos(status) + ").");
	status = OH_Pasteboard_SetData(pasteboard.get(), data.get());
	ERR_FAIL_COND_MSG(status != 0, "Could not copy clipboard text (pasteboard error " + itos(status) + ").");
}

String DisplayServerOpenHarmony::clipboard_get() const {
	std::unique_ptr<OH_Pasteboard, decltype(&OH_Pasteboard_Destroy)> pasteboard(OH_Pasteboard_Create(), OH_Pasteboard_Destroy);
	ERR_FAIL_COND_V_MSG(!pasteboard, String(), "Could not allocate clipboard reader.");
	if (!OH_Pasteboard_HasData(pasteboard.get())) {
		return String();
	}
	int status = 0;
	std::unique_ptr<OH_UdmfData, decltype(&OH_UdmfData_Destroy)> data(OH_Pasteboard_GetData(pasteboard.get(), &status), OH_UdmfData_Destroy);
	ERR_FAIL_COND_V_MSG(status != 0 || !data, String(), "Could not paste clipboard text (pasteboard error " + itos(status) + "). Check READ_PASTEBOARD in the signing profile and application permissions.");
	std::unique_ptr<OH_UdsPlainText, decltype(&OH_UdsPlainText_Destroy)> text(OH_UdsPlainText_Create(), OH_UdsPlainText_Destroy);
	ERR_FAIL_COND_V_MSG(!text, String(), "Could not allocate clipboard text reader.");
	// Primary getters search the full data set, including when the first record
	// is an image or HTML. Text copied from browsers may have only HTML data.
	if (OH_UdmfData_GetPrimaryPlainText(data.get(), text.get()) == 0) {
		const char *content = OH_UdsPlainText_GetContent(text.get());
		if (content) {
			return String::utf8(content);
		}
	}
	std::unique_ptr<OH_UdsHtml, decltype(&OH_UdsHtml_Destroy)> html(OH_UdsHtml_Create(), OH_UdsHtml_Destroy);
	if (html && OH_UdmfData_GetPrimaryHtml(data.get(), html.get()) == 0) {
		const char *content = OH_UdsHtml_GetPlainContent(html.get());
		if (content) {
			return String::utf8(content);
		}
	}
	return String();
}

void DisplayServerOpenHarmony::screen_set_keep_on(bool p_enable) {
	ohos_wrapper_screen_set_keep_on(OS_OpenHarmony::get_singleton()->get_window_id(), p_enable);
}

bool DisplayServerOpenHarmony::screen_is_kept_on() const {
	return ohos_wrapper_screen_is_kept_on(OS_OpenHarmony::get_singleton()->get_window_id());
}

void DisplayServerOpenHarmony::_get_text_config(InputMethod_TextEditorProxy *p_text_editor_proxy, InputMethod_TextConfig *p_text_config) {
	InputMethod_TextInputType input_type = IME_TEXT_INPUT_TYPE_TEXT;
	InputMethod_EnterKeyType enter_key_type = IME_ENTER_KEY_DONE;
	DisplayServerOpenHarmony *display = get_singleton();
	DisplayServerEnums::VirtualKeyboardType type;
	{
		MutexLock lock(display->ime_geometry_mutex);
		type = display->keyboard_type;
	}
	switch (type) {
		case DisplayServerEnums::KEYBOARD_TYPE_DEFAULT:
			input_type = IME_TEXT_INPUT_TYPE_TEXT;
			break;
		case DisplayServerEnums::KEYBOARD_TYPE_MULTILINE:
			input_type = IME_TEXT_INPUT_TYPE_MULTILINE;
			enter_key_type = IME_ENTER_KEY_NEWLINE;
			break;
		case DisplayServerEnums::KEYBOARD_TYPE_NUMBER:
			input_type = IME_TEXT_INPUT_TYPE_NUMBER;
			break;
		case DisplayServerEnums::KEYBOARD_TYPE_NUMBER_DECIMAL:
			input_type = IME_TEXT_INPUT_TYPE_NUMBER_DECIMAL;
			break;
		case DisplayServerEnums::KEYBOARD_TYPE_PHONE:
			input_type = IME_TEXT_INPUT_TYPE_PHONE;
			break;
		case DisplayServerEnums::KEYBOARD_TYPE_EMAIL_ADDRESS:
			input_type = IME_TEXT_INPUT_TYPE_EMAIL_ADDRESS;
			break;
		case DisplayServerEnums::KEYBOARD_TYPE_PASSWORD:
			input_type = IME_TEXT_INPUT_TYPE_VISIBLE_PASSWORD;
			break;
		case DisplayServerEnums::KEYBOARD_TYPE_URL:
			input_type = IME_TEXT_INPUT_TYPE_URL;
			break;
		default:
			break;
	}
	OH_TextConfig_SetInputType(p_text_config, input_type);
	OH_TextConfig_SetPreviewTextSupport(p_text_config, false);
	OH_TextConfig_SetEnterKeyType(p_text_config, enter_key_type);
	// Bind to the actual ArkUI window, not Godot's MAIN_WINDOW_ID (zero).
	InputMethod_ErrorCode code = OH_TextConfig_SetWindowId(p_text_config, OS_OpenHarmony::get_singleton()->get_window_id());
	if (code != IME_ERR_OK) {
		ERR_PRINT(vformat("Failed to configure input method window: %d.", code));
	}
	Point2 screen_position;
	const bool have_screen_position = display->_get_ime_screen_position(screen_position);
	if (have_screen_position) {
		// GetCursorInfo returns TextConfig's borrowed, mutable cursor object.
		InputMethod_CursorInfo *cursor = nullptr;
		code = OH_TextConfig_GetCursorInfo(p_text_config, &cursor);
		if (code == IME_ERR_OK && cursor) {
			code = OH_CursorInfo_SetRect(cursor, screen_position.x, screen_position.y, 0, 0);
		}
		if (code != IME_ERR_OK || !cursor) {
			ERR_PRINT(vformat("Failed to configure input method cursor: %d.", code));
		}
	}
	if (have_screen_position) {
		InputMethod_TextAvoidInfo *avoid = nullptr;
		code = OH_TextConfig_GetTextAvoidInfo(p_text_config, &avoid);
		if (code == IME_ERR_OK && avoid) {
			// Core controls pass get_global_rect() to virtual_keyboard_show,
			// without the embedded-window/stretch transform. Avoid the known
			// physical caret anchor instead of misinterpreting that canvas rect.
			code = OH_TextAvoidInfo_SetPositionY(avoid, screen_position.y);
			if (code == IME_ERR_OK) {
				code = OH_TextAvoidInfo_SetHeight(avoid, 0);
			}
		}
		if (code != IME_ERR_OK || !avoid) {
			ERR_PRINT(vformat("Failed to configure input method text area: %d.", code));
		}
	}
}

void DisplayServerOpenHarmony::_insert_text(InputMethod_TextEditorProxy *p_text_editor_proxy, const char16_t *p_text, size_t length) {
	String characters = String::utf16(p_text, length);

	for (int i = 0; i < characters.length(); i++) {
		int character = characters[i];
		Key key = Key::NONE;

		if (character == '\t') { // 0x09
			key = Key::TAB;
		} else if (character == '\n') { // 0x0A
			key = Key::ENTER;
		} else if (character == ' ') {
			key = Key::SPACE;
		}

		_input_text_key(key, character, key, Key::NONE, 0, true, KeyLocation::UNSPECIFIED);
		_input_text_key(key, character, key, Key::NONE, 0, false, KeyLocation::UNSPECIFIED);
	}
}

void DisplayServerOpenHarmony::_delete_forward(InputMethod_TextEditorProxy *p_text_editor_proxy, int32_t length) {
	for (int i = 0; i < length; i++) {
		_input_text_key(Key::KEY_DELETE, 0, Key::KEY_DELETE, Key::NONE, 0, true, KeyLocation::UNSPECIFIED);
		_input_text_key(Key::KEY_DELETE, 0, Key::KEY_DELETE, Key::NONE, 0, false, KeyLocation::UNSPECIFIED);
	}
}

void DisplayServerOpenHarmony::_delete_backward(InputMethod_TextEditorProxy *p_text_editor_proxy, int32_t length) {
	for (int i = 0; i < length; i++) {
		_input_text_key(Key::BACKSPACE, 0, Key::BACKSPACE, Key::NONE, 0, true, KeyLocation::UNSPECIFIED);
		_input_text_key(Key::BACKSPACE, 0, Key::BACKSPACE, Key::NONE, 0, false, KeyLocation::UNSPECIFIED);
	}
}

void DisplayServerOpenHarmony::_send_keyboard_status(InputMethod_TextEditorProxy *p_text_editor_proxy, InputMethod_KeyboardStatus status) {
	DisplayServerOpenHarmony *display = get_singleton();
	if (!display) {
		return;
	}
	MutexLock lock(display->ime_status_mutex);
	if (p_text_editor_proxy && p_text_editor_proxy == display->text_editor_proxy) {
		display->keyboard_status = status;
	}
}

void DisplayServerOpenHarmony::_send_enter_key(InputMethod_TextEditorProxy *p_text_editor_proxy, InputMethod_EnterKeyType enter_key_type) {
	_input_text_key(Key::ENTER, 0, Key::ENTER, Key::NONE, 0, true, KeyLocation::UNSPECIFIED);
	_input_text_key(Key::ENTER, 0, Key::ENTER, Key::NONE, 0, false, KeyLocation::UNSPECIFIED);
}

void DisplayServerOpenHarmony::_move_cursor(InputMethod_TextEditorProxy *p_text_editor_proxy, InputMethod_Direction direction) {
	switch (direction) {
		case IME_DIRECTION_LEFT:
			_input_text_key(Key::LEFT, 0, Key::LEFT, Key::NONE, 0, true, KeyLocation::UNSPECIFIED);
			_input_text_key(Key::LEFT, 0, Key::LEFT, Key::NONE, 0, false, KeyLocation::UNSPECIFIED);
			break;
		case IME_DIRECTION_RIGHT:
			_input_text_key(Key::RIGHT, 0, Key::RIGHT, Key::NONE, 0, true, KeyLocation::UNSPECIFIED);
			_input_text_key(Key::RIGHT, 0, Key::RIGHT, Key::NONE, 0, false, KeyLocation::UNSPECIFIED);
			break;
		case IME_DIRECTION_UP:
			_input_text_key(Key::UP, 0, Key::UP, Key::NONE, 0, true, KeyLocation::UNSPECIFIED);
			_input_text_key(Key::UP, 0, Key::UP, Key::NONE, 0, false, KeyLocation::UNSPECIFIED);
			break;
		case IME_DIRECTION_DOWN:
			_input_text_key(Key::DOWN, 0, Key::DOWN, Key::NONE, 0, true, KeyLocation::UNSPECIFIED);
			_input_text_key(Key::DOWN, 0, Key::DOWN, Key::NONE, 0, false, KeyLocation::UNSPECIFIED);
			break;
		default:
			break;
	}
}

void DisplayServerOpenHarmony::_handle_set_selection(InputMethod_TextEditorProxy *p_text_editor_proxy, int32_t start, int32_t end) {
	// Not supported by Godot.
}

void DisplayServerOpenHarmony::_handle_extend_action(InputMethod_TextEditorProxy *p_text_editor_proxy, InputMethod_ExtendAction action) {
	// Not supported by Godot.
}

void DisplayServerOpenHarmony::_get_left_text_of_cursor(InputMethod_TextEditorProxy *p_text_editor_proxy, int32_t number, char16_t *p_text, size_t *p_length) {
	// Not supported by Godot.
	*p_length = 0;
}

void DisplayServerOpenHarmony::_get_right_text_of_cursor(InputMethod_TextEditorProxy *p_text_editor_proxy, int32_t number, char16_t *p_text, size_t *p_length) {
	// Not supported by Godot.
	*p_length = 0;
}

int32_t DisplayServerOpenHarmony::_get_text_index_at_cursor(InputMethod_TextEditorProxy *p_text_editor_proxy) {
	// Not supported by Godot.
	return 0;
}

int32_t DisplayServerOpenHarmony::_receive_private_command(InputMethod_TextEditorProxy *p_text_editor_proxy, InputMethod_PrivateCommand *p_command[], size_t length) {
	// Not supported by Godot.
	return 0;
}

int32_t DisplayServerOpenHarmony::_set_preview_text(InputMethod_TextEditorProxy *p_text_editor_proxy, const char16_t *p_text, size_t length, int32_t start, int32_t end) {
	// Not supported by Godot.
	return 0;
}

void DisplayServerOpenHarmony::_finish_text_preview(InputMethod_TextEditorProxy *p_text_editor_proxy) {
	// Not supported by Godot.
}

void DisplayServerOpenHarmony::_input_text_key(Key p_key, char32_t p_char, Key p_unshifted, Key p_physical, int p_modifier, bool p_pressed, KeyLocation p_location) {
	Ref<InputEventKey> ev;
	ev.instantiate();
	ev->set_echo(false);
	ev->set_pressed(p_pressed);
	ev->set_keycode(fix_keycode(p_char, p_key));
	ev->set_key_label(p_unshifted);
	ev->set_physical_keycode(p_physical);
	ev->set_unicode(fix_unicode(p_char));
	ev->set_location(p_location);
	Input::get_singleton()->parse_input_event(ev);
}

void DisplayServerOpenHarmony::virtual_keyboard_show(const String &p_existing_text, const Rect2 &p_screen_rect, DisplayServerEnums::VirtualKeyboardType p_type, int p_max_length, int p_cursor_start, int p_cursor_end) {
	virtual_keyboard_requested = true;
	requested_keyboard_type = p_type;
	DisplayServerEnums::VirtualKeyboardType current_type;
	{
		MutexLock lock(ime_geometry_mutex);
		ime_text_rect = p_screen_rect;
		current_type = keyboard_type;
	}
	// ArkUI activation and surface focus can arrive after the first click.
	// Keep the latest request and service it on a real focus transition.
	if (!godot_host_is_input_focused()) {
		update_ime_focus(false);
		return;
	}
	Point2 current_screen_position;
	uint32_t current_display_id = 0;
	const bool have_display = _get_ime_screen_position(current_screen_position, &current_display_id);
	const bool display_changed = have_display && (!ime_display_id_valid || current_display_id != ime_display_id);
	// Attach can succeed without a SHOW callback (including hardware keyboards).
	// Reuse it independent of visibility, while a valid display transition
	// refreshes the service binding even when the keyboard type is unchanged.
	if (input_method_proxy && current_type == p_type && !display_changed) {
		InputMethod_KeyboardStatus status;
		{
			MutexLock lock(ime_status_mutex);
			status = keyboard_status;
		}
		if (status != IME_KEYBOARD_STATUS_SHOW) {
			InputMethod_ErrorCode code = OH_InputMethodProxy_ShowKeyboard(input_method_proxy);
			if (code != IME_ERR_OK) {
				ERR_PRINT(vformat("Failed to show keyboard: %d.", code));
			}
		}
		_update_ime_cursor();
		return;
	}

	// Allocate the replacement while the old editor still lives, keeping their
	// callback identities distinct during reconfiguration. Allocation failure
	// leaves an existing attachment intact.
	InputMethod_TextEditorProxy *new_text_editor_proxy = OH_TextEditorProxy_Create();
	InputMethod_AttachOptions *new_attach_options = OH_AttachOptions_Create(true);
	if (!new_text_editor_proxy || !new_attach_options) {
		if (new_attach_options) {
			OH_AttachOptions_Destroy(new_attach_options);
		}
		if (new_text_editor_proxy) {
			OH_TextEditorProxy_Destroy(new_text_editor_proxy);
		}
		ERR_FAIL_MSG("Failed to allocate input method client.");
	}
	virtual_keyboard_hide();
	virtual_keyboard_requested = true;
	requested_keyboard_type = p_type;
	{
		MutexLock lock(ime_geometry_mutex);
		keyboard_type = p_type;
	}
	{
		MutexLock lock(ime_status_mutex);
		text_editor_proxy = new_text_editor_proxy;
	}
	attach_options = new_attach_options;

	OH_TextEditorProxy_SetGetTextConfigFunc(text_editor_proxy, _get_text_config);
	OH_TextEditorProxy_SetInsertTextFunc(text_editor_proxy, _insert_text);
	OH_TextEditorProxy_SetDeleteForwardFunc(text_editor_proxy, _delete_forward);
	OH_TextEditorProxy_SetDeleteBackwardFunc(text_editor_proxy, _delete_backward);
	OH_TextEditorProxy_SetSendKeyboardStatusFunc(text_editor_proxy, _send_keyboard_status);
	OH_TextEditorProxy_SetSendEnterKeyFunc(text_editor_proxy, _send_enter_key);
	OH_TextEditorProxy_SetMoveCursorFunc(text_editor_proxy, _move_cursor);
	OH_TextEditorProxy_SetHandleSetSelectionFunc(text_editor_proxy, _handle_set_selection);
	OH_TextEditorProxy_SetHandleExtendActionFunc(text_editor_proxy, _handle_extend_action);
	OH_TextEditorProxy_SetGetLeftTextOfCursorFunc(text_editor_proxy, _get_left_text_of_cursor);
	OH_TextEditorProxy_SetGetRightTextOfCursorFunc(text_editor_proxy, _get_right_text_of_cursor);
	OH_TextEditorProxy_SetGetTextIndexAtCursorFunc(text_editor_proxy, _get_text_index_at_cursor);
	OH_TextEditorProxy_SetReceivePrivateCommandFunc(text_editor_proxy, _receive_private_command);
	OH_TextEditorProxy_SetSetPreviewTextFunc(text_editor_proxy, _set_preview_text);
	OH_TextEditorProxy_SetFinishTextPreviewFunc(text_editor_proxy, _finish_text_preview);

	// Publish attachment ownership only after success. A failed Attach does not
	// grant a usable output proxy, even if the SDK wrote its output parameter.
	InputMethod_InputMethodProxy *new_input_method_proxy = nullptr;
	InputMethod_ErrorCode code = OH_InputMethodController_Attach(text_editor_proxy, attach_options, &new_input_method_proxy);
	if (code != IME_ERR_OK || !new_input_method_proxy) {
		virtual_keyboard_hide();
		ERR_FAIL_MSG(vformat("Failed to attach input method controller: %d.", code != IME_ERR_OK ? code : IME_ERR_NULL_POINTER));
	}
	input_method_proxy = new_input_method_proxy;
	ime_display_id = current_display_id;
	ime_display_id_valid = have_display;
	ime_screen_position_valid = false;
	_update_ime_cursor();
}

void DisplayServerOpenHarmony::virtual_keyboard_hide() {
	virtual_keyboard_requested = false;
	ime_display_id_valid = false;
	ime_screen_position_valid = false;
	InputMethod_TextEditorProxy *old_text_editor_proxy;
	{
		MutexLock lock(ime_status_mutex);
		// Invalidate callback identity before reentrant Hide/Detach notifications.
		old_text_editor_proxy = text_editor_proxy;
		text_editor_proxy = nullptr;
		keyboard_status = IME_KEYBOARD_STATUS_NONE;
	}
	InputMethod_InputMethodProxy *old_input_method_proxy = input_method_proxy;
	input_method_proxy = nullptr;
	InputMethod_AttachOptions *old_attach_options = attach_options;
	attach_options = nullptr;
	if (old_input_method_proxy) {
		// This path releases the client, not only its soft keyboard. Detach
		// stops input and hides its panel; a preceding Hide requires a focused
		// client and can fail during a normal window-focus handoff.
		InputMethod_ErrorCode code = OH_InputMethodController_Detach(old_input_method_proxy);
		if (code != IME_ERR_OK) {
			ERR_PRINT(vformat("Failed to detach input method controller: %d.", code));
		}
	}
	// SDK ownership lasts through the next Attach/Detach call, regardless of
	// soft-keyboard visibility. Release the editor only after calling Detach.
	if (old_attach_options) {
		OH_AttachOptions_Destroy(old_attach_options);
	}
	if (old_text_editor_proxy) {
		OH_TextEditorProxy_Destroy(old_text_editor_proxy);
	}
}

void DisplayServerOpenHarmony::update_ime_focus(bool p_focused) {
	if (!p_focused) {
		const bool requested = virtual_keyboard_requested;
		virtual_keyboard_hide();
		virtual_keyboard_requested = requested;
		return;
	}
	if (virtual_keyboard_requested) {
		Rect2 screen_rect;
		{
			MutexLock lock(ime_geometry_mutex);
			screen_rect = ime_text_rect;
		}
		virtual_keyboard_show(String(), screen_rect, requested_keyboard_type, -1, -1, -1);
	}
}

int DisplayServerOpenHarmony::virtual_keyboard_get_height() const {
	InputMethod_KeyboardStatus status;
	{
		MutexLock lock(ime_status_mutex);
		status = keyboard_status;
	}
	if (status == IME_KEYBOARD_STATUS_SHOW) {
		int height = ohos_wrapper_get_keyboard_avoid_area(OS_OpenHarmony::get_singleton()->get_window_id());
		return height;
	}
	return 0;
}

bool DisplayServerOpenHarmony::_get_ime_screen_position(Point2 &r_position, uint32_t *r_display_id) const {
	Point2 local_position;
	{
		MutexLock lock(ime_geometry_mutex);
		// LineEdit/TextEdit submit the transformed caret before attaching.
		// A direct virtual_keyboard_show caller can use its supplied text area.
		local_position = ime_position_valid ? Point2(ime_position) : ime_text_rect.position + Point2(0, ime_text_rect.size.y);
	}
	double x, y;
	if (!ohos_wrapper_map_surface_point(OS_OpenHarmony::get_singleton()->get_window_id(), local_position.x, local_position.y, x, y, r_display_id)) {
		return false;
	}
	r_position = Point2(x, y);
	return true;
}

void DisplayServerOpenHarmony::_update_ime_display() {
	if (!input_method_proxy || !virtual_keyboard_requested || !godot_host_is_input_focused()) {
		return;
	}
	Point2 screen_position;
	uint32_t display_id = 0;
	if (!_get_ime_screen_position(screen_position, &display_id) || (ime_display_id_valid && display_id == ime_display_id)) {
		return;
	}
	Rect2 screen_rect;
	{
		MutexLock lock(ime_geometry_mutex);
		screen_rect = ime_text_rect;
	}
	// Reconfigure on the engine thread, after the UI publishes valid metadata.
	// The show path preserves the latest request and balances the old client.
	virtual_keyboard_show(String(), screen_rect, requested_keyboard_type, -1, -1, -1);
}

void DisplayServerOpenHarmony::_update_ime_cursor() {
	if (!ime_active || !input_method_proxy || !godot_host_is_input_focused()) {
		return;
	}
	Point2 screen_position;
	uint32_t display_id = 0;
	if (!_get_ime_screen_position(screen_position, &display_id) || (ime_screen_position_valid && screen_position == last_ime_screen_position && display_id == last_ime_display_id)) {
		return;
	}
	// Godot supplies the popup anchor at the bottom of the caret. A zero-sized
	// cursor places candidates there without an extra guessed 30-pixel offset.
	InputMethod_CursorInfo *info = OH_CursorInfo_Create(screen_position.x, screen_position.y, 0, 0);
	if (!info) {
		ERR_PRINT("Failed to allocate input method cursor information.");
		return;
	}
	InputMethod_ErrorCode code = OH_InputMethodProxy_NotifyCursorUpdate(input_method_proxy, info);
	OH_CursorInfo_Destroy(info);
	if (code != IME_ERR_OK) {
		ERR_PRINT(vformat("Failed to update input method cursor: %d.", code));
		return;
	}
	last_ime_screen_position = screen_position;
	last_ime_display_id = display_id;
	ime_screen_position_valid = true;
}

void DisplayServerOpenHarmony::window_set_ime_active(const bool p_active, DisplayServerEnums::WindowID p_window) {
	ime_active = p_active;
	if (!p_active) {
		MutexLock lock(ime_geometry_mutex);
		ime_position_valid = false;
		ime_screen_position_valid = false;
	}
}

void DisplayServerOpenHarmony::window_set_ime_position(const Point2i &p_pos, DisplayServerEnums::WindowID p_window) {
	{
		MutexLock lock(ime_geometry_mutex);
		ime_position = p_pos;
		ime_position_valid = true;
	}
	_update_ime_cursor();
}

Vector<DisplayServerEnums::WindowID> DisplayServerOpenHarmony::get_window_list() const {
	Vector<DisplayServerEnums::WindowID> ret;
	ret.push_back(DisplayServerEnums::MAIN_WINDOW_ID);
	return ret;
}

DisplayServerEnums::WindowID DisplayServerOpenHarmony::get_window_at_screen_position(const Point2i &p_position) const {
	return DisplayServerEnums::MAIN_WINDOW_ID;
}

void DisplayServerOpenHarmony::window_attach_instance_id(ObjectID p_instance, DisplayServerEnums::WindowID p_window) {
	window_attached_instance_id = p_instance;
}

ObjectID DisplayServerOpenHarmony::window_get_attached_instance_id(DisplayServerEnums::WindowID p_window) const {
	return window_attached_instance_id;
}

void DisplayServerOpenHarmony::window_set_window_event_callback(const Callable &p_callable, DisplayServerEnums::WindowID p_window) {
	window_event_callback = p_callable;
}

void DisplayServerOpenHarmony::window_set_input_event_callback(const Callable &p_callable, DisplayServerEnums::WindowID p_window) {
	input_event_callback = p_callable;
}

void DisplayServerOpenHarmony::window_set_input_text_callback(const Callable &p_callable, DisplayServerEnums::WindowID p_window) {
	input_text_callback = p_callable;
}

void DisplayServerOpenHarmony::window_set_rect_changed_callback(const Callable &p_callable, DisplayServerEnums::WindowID p_window) {
	window_resize_callback = p_callable;
}

void DisplayServerOpenHarmony::window_set_drop_files_callback(const Callable &p_callable, DisplayServerEnums::WindowID p_window) {
	// Not supported on OpenHarmony.
}

void DisplayServerOpenHarmony::window_set_title(const String &p_title, DisplayServerEnums::WindowID p_window) {
	// Not supported on OpenHarmony.
}

int DisplayServerOpenHarmony::window_get_current_screen(DisplayServerEnums::WindowID p_window) const {
	return DisplayServerEnums::SCREEN_OF_MAIN_WINDOW;
}

void DisplayServerOpenHarmony::window_set_current_screen(int p_screen, DisplayServerEnums::WindowID p_window) {
	// Not supported on OpenHarmony.
}

Point2i DisplayServerOpenHarmony::window_get_position(DisplayServerEnums::WindowID p_window) const {
	WrapperWindowGeometry geometry;
	if (ohos_wrapper_get_window_geometry(OS_OpenHarmony::get_singleton()->get_window_id(), geometry)) {
		return Point2i(Math::round(geometry.surface_x), Math::round(geometry.surface_y));
	}
	return Point2i();
}

Point2i DisplayServerOpenHarmony::window_get_position_with_decorations(DisplayServerEnums::WindowID p_window) const {
	WrapperWindowGeometry geometry;
	if (ohos_wrapper_get_window_geometry(OS_OpenHarmony::get_singleton()->get_window_id(), geometry) && geometry.window_position_valid) {
		return Point2i(geometry.window_x, geometry.window_y);
	}
	return Point2i();
}

void DisplayServerOpenHarmony::window_set_position(const Point2i &p_position, DisplayServerEnums::WindowID p_window) {
	// Not supported on OpenHarmony.
}

void DisplayServerOpenHarmony::window_set_transient(DisplayServerEnums::WindowID p_window, DisplayServerEnums::WindowID p_parent) {
	// Not supported on OpenHarmony.
}

void DisplayServerOpenHarmony::window_set_max_size(const Size2i p_size, DisplayServerEnums::WindowID p_window) {
	// Not supported on OpenHarmony.
}

Size2i DisplayServerOpenHarmony::window_get_max_size(DisplayServerEnums::WindowID p_window) const {
	return Size2i();
}

void DisplayServerOpenHarmony::window_set_min_size(const Size2i p_size, DisplayServerEnums::WindowID p_window) {
	// Not supported on OpenHarmony.
}

Size2i DisplayServerOpenHarmony::window_get_min_size(DisplayServerEnums::WindowID p_window) const {
	return Size2i();
}

void DisplayServerOpenHarmony::window_set_size(const Size2i p_size, DisplayServerEnums::WindowID p_window) {
	// Not supported on OpenHarmony.
}

Size2i DisplayServerOpenHarmony::window_get_size(DisplayServerEnums::WindowID p_window) const {
	return OS_OpenHarmony::get_singleton()->get_display_size();
}

Size2i DisplayServerOpenHarmony::window_get_size_with_decorations(DisplayServerEnums::WindowID p_window) const {
	return OS_OpenHarmony::get_singleton()->get_display_size();
}

void DisplayServerOpenHarmony::window_set_mode(DisplayServerEnums::WindowMode p_mode, DisplayServerEnums::WindowID p_window) {
	ERR_FAIL_COND(p_window != DisplayServerEnums::MAIN_WINDOW_ID);
	ERR_FAIL_COND(p_mode < DisplayServerEnums::WINDOW_MODE_WINDOWED || p_mode > DisplayServerEnums::WINDOW_MODE_EXCLUSIVE_FULLSCREEN);
	// HarmonyOS exposes ordinary fullscreen; never advertise or request an
	// exclusive display mode. The host also normalizes this at its ABI boundary.
	const DisplayServerEnums::WindowMode mode = p_mode == DisplayServerEnums::WINDOW_MODE_EXCLUSIVE_FULLSCREEN ? DisplayServerEnums::WINDOW_MODE_FULLSCREEN : p_mode;
	ERR_FAIL_COND_MSG(godot_host_set_window_mode(mode) != 0, "Cannot queue window mode change.");
}

DisplayServerEnums::WindowMode DisplayServerOpenHarmony::window_get_mode(DisplayServerEnums::WindowID p_window) const {
	ERR_FAIL_COND_V(p_window != DisplayServerEnums::MAIN_WINDOW_ID, DisplayServerEnums::WINDOW_MODE_WINDOWED);
	return static_cast<DisplayServerEnums::WindowMode>(godot_host_get_window_mode());
}

void DisplayServerOpenHarmony::window_set_vsync_mode(DisplayServerEnums::VSyncMode p_vsync_mode, DisplayServerEnums::WindowID p_window) {
	ERR_FAIL_COND(p_window != DisplayServerEnums::MAIN_WINDOW_ID);
	if (rendering_context) {
		rendering_context->window_set_vsync_mode(DisplayServerEnums::MAIN_WINDOW_ID, p_vsync_mode);
	}
#ifdef GLES3_ENABLED
	if (gl_manager) {
		gl_manager->set_vsync_mode(p_vsync_mode);
	}
#endif
}

DisplayServerEnums::VSyncMode DisplayServerOpenHarmony::window_get_vsync_mode(DisplayServerEnums::WindowID p_window) const {
	ERR_FAIL_COND_V(p_window != DisplayServerEnums::MAIN_WINDOW_ID, DisplayServerEnums::VSYNC_ENABLED);
#ifdef GLES3_ENABLED
	if (gl_manager) {
		return gl_manager->get_vsync_mode();
	}
#endif
	return rendering_context ? rendering_context->window_get_vsync_mode(DisplayServerEnums::MAIN_WINDOW_ID) : DisplayServerEnums::VSYNC_ENABLED;
}

bool DisplayServerOpenHarmony::window_is_maximize_allowed(DisplayServerEnums::WindowID p_window) const {
	return false;
}

void DisplayServerOpenHarmony::window_set_flag(DisplayServerEnums::WindowFlags p_flag, bool p_enabled, DisplayServerEnums::WindowID p_window) {
	// Not supported on OpenHarmony.
}

bool DisplayServerOpenHarmony::window_get_flag(DisplayServerEnums::WindowFlags p_flag, DisplayServerEnums::WindowID p_window) const {
	return false;
}

void DisplayServerOpenHarmony::window_request_attention(DisplayServerEnums::WindowID p_window) {
	// Not supported on OpenHarmony.
}

void DisplayServerOpenHarmony::window_move_to_foreground(DisplayServerEnums::WindowID p_window) {
	// Not supported on OpenHarmony.
}

bool DisplayServerOpenHarmony::window_is_focused(DisplayServerEnums::WindowID p_window) const {
	return OS_OpenHarmony::get_singleton()->is_window_focused();
}

bool DisplayServerOpenHarmony::window_can_draw(DisplayServerEnums::WindowID p_window) const {
	return true;
}

bool DisplayServerOpenHarmony::can_any_window_draw() const {
	return true;
}

void DisplayServerOpenHarmony::process_events() {
	// Reapply locks invalidated by the system, including rapid focus switches
	// that were folded into one latest host snapshot. Failed requests retry here.
	_mouse_update_mode();
	// Window caches its screen position for absolute viewport transforms. Update
	// it on the engine thread when ArkUI moves the surface without resizing it.
	const Rect2i rect(window_get_position(), window_get_size());
	if (window_resize_callback.is_valid() && (!reported_window_rect_valid || rect != reported_window_rect)) {
		reported_window_rect = rect;
		reported_window_rect_valid = true;
		_window_callback(window_resize_callback, rect);
	}
	Input::get_singleton()->flush_buffered_events();
	// Window moves/layout changes also move candidates when the caret is still.
	_update_ime_display();
	_update_ime_cursor();
}

void DisplayServerOpenHarmony::update_mouse_focus(bool p_focused) {
	if (!p_focused) {
		mouse_mode_applied = false;
		mouse_mode_retry_at_usec = 0;
		mouse_mode_last_error = 0;
		return;
	}
	_mouse_update_mode();
}

void DisplayServerOpenHarmony::_mouse_update_mode() {
	using namespace DisplayServerEnums;
	const MouseMode wanted = mouse_mode_override_enabled ? mouse_mode_override : mouse_mode_base;
	const uint64_t focus_generation = godot_host_get_window_focus_generation();
	if (wanted != mouse_mode || focus_generation != mouse_focus_generation) {
		mouse_mode = wanted;
		mouse_focus_generation = focus_generation;
		mouse_mode_applied = false;
		mouse_mode_retry_at_usec = 0;
		mouse_mode_last_error = 0;
	}
	if (wanted != MOUSE_MODE_VISIBLE) {
		mouse_mode_managed = true;
	}
	if (!mouse_mode_managed) {
		// Default-visible games need neither LockCursor permission nor an SDK call.
		mouse_mode_applied = true;
		return;
	}
	// Acquire only with input focus, but allow release to VISIBLE after a
	// surface blur while the actual window still owns the system cursor.
	if (!godot_host_is_window_active() || (wanted != MOUSE_MODE_VISIBLE && !godot_host_is_input_focused())) {
		mouse_mode_applied = false;
		mouse_mode_retry_at_usec = 0;
		return;
	}
	if (mouse_mode_applied) {
		return;
	}
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	if (now < mouse_mode_retry_at_usec) {
		return;
	}
	const int32_t id = OS_OpenHarmony::get_singleton()->get_window_id();
	const bool locked = wanted == MOUSE_MODE_CAPTURED || wanted == MOUSE_MODE_CONFINED || wanted == MOUSE_MODE_CONFINED_HIDDEN;
	const bool visible = wanted == MOUSE_MODE_VISIBLE || wanted == MOUSE_MODE_CONFINED;
	const int result = ohos_wrapper_set_mouse_mode(id, locked, wanted != MOUSE_MODE_CAPTURED, visible);
	if (result != 0) {
		// Activation can race the SDK call. Retain the request and avoid retrying
		// or reporting the same transient error every rendered frame.
		mouse_mode_retry_at_usec = now + 500000;
		if (result != mouse_mode_last_error) {
			mouse_mode_last_error = result;
			ERR_FAIL_MSG(vformat("Cannot change window cursor mode: %d", result));
		}
		return;
	}
	mouse_mode_last_error = 0;
	mouse_mode_applied = true;
	mouse_mode_managed = wanted != MOUSE_MODE_VISIBLE;
}

void DisplayServerOpenHarmony::mouse_set_mode(DisplayServerEnums::MouseMode p_mode) {
	ERR_FAIL_INDEX(p_mode, DisplayServerEnums::MOUSE_MODE_MAX);
	mouse_mode_base = p_mode;
	_mouse_update_mode();
}
void DisplayServerOpenHarmony::mouse_set_mode_override(DisplayServerEnums::MouseMode p_mode) {
	ERR_FAIL_INDEX(p_mode, DisplayServerEnums::MOUSE_MODE_MAX);
	mouse_mode_override = p_mode;
	_mouse_update_mode();
}
void DisplayServerOpenHarmony::mouse_set_mode_override_enabled(bool p_enabled) {
	mouse_mode_override_enabled = p_enabled;
	_mouse_update_mode();
}
Point2i DisplayServerOpenHarmony::mouse_get_position() const {
	// ArkUI input positions are surface-local; DisplayServer reports screen
	// coordinates, which Viewport converts back using its absolute transform.
	return window_get_position() + Input::get_singleton()->get_mouse_position();
}
BitField<MouseButtonMask> DisplayServerOpenHarmony::mouse_get_button_state() const {
	return Input::get_singleton()->get_mouse_button_mask();
}
void DisplayServerOpenHarmony::cursor_set_shape(DisplayServerEnums::CursorShape p_shape) {
	ERR_FAIL_INDEX(p_shape, DisplayServerEnums::CURSOR_MAX);
	if (ohos_wrapper_set_cursor_shape(OS_OpenHarmony::get_singleton()->get_window_id(), p_shape) == 0) {
		cursor_shape = p_shape;
	}
}
