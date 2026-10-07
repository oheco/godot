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

#include "os_openharmony.h"
#include "rendering_context_driver_vulkan_openharmony.h"
#include "wrapper_openharmony.h"

#include "core/input/input.h"
#include "core/input/input_event.h"
#include "servers/display/native_menu.h"
#include "servers/rendering/renderer_rd/renderer_compositor_rd.h"
#include "servers/rendering/rendering_device.h"

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
	return drivers;
}

DisplayServer *DisplayServerOpenHarmony::create_func(const String &p_rendering_driver, DisplayServerEnums::WindowMode p_mode, DisplayServerEnums::VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i *p_position, const Vector2i &p_resolution, int p_screen, DisplayServerEnums::Context p_context, int64_t p_parent_window, Error &r_error) {
	DisplayServer *ds = memnew(DisplayServerOpenHarmony(p_rendering_driver, p_mode, p_vsync_mode, p_flags, p_position, p_resolution, p_screen, p_context, p_parent_window, r_error));
	if (r_error != OK) {
		OS::get_singleton()->alert(
				"Your device seems not to support the required Vulkan version.\n\n"
				"Unable to initialize Vulkan video driver.");
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

	if (rendering_driver != "vulkan") {
		ERR_PRINT(vformat("Failed to create %s context.", rendering_driver));
		r_error = ERR_UNAVAILABLE;
		return;
	}

	rendering_context = memnew(RenderingContextDriverVulkanOpenHarmony);

	if (rendering_context->initialize() != OK) {
		memdelete(rendering_context);
		rendering_context = nullptr;
		ERR_PRINT(vformat("Failed to initialize %s context.", rendering_driver));
		r_error = ERR_UNAVAILABLE;
		return;
	}
	RenderingContextDriverVulkanOpenHarmony::WindowPlatformData vulkan;
	OHNativeWindow *native_window = OS_OpenHarmony::get_singleton()->get_native_window();
	if (!native_window) {
		r_error = ERR_UNAVAILABLE;
		return;
	}
	vulkan.window = native_window;

	if (rendering_context->window_create(DisplayServerEnums::MAIN_WINDOW_ID, &vulkan) != OK) {
		ERR_PRINT(vformat("Failed to create %s window.", rendering_driver));
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

	Input::get_singleton()->set_event_dispatch_function(_dispatch_input_events);

	r_error = OK;
}

DisplayServerOpenHarmony::~DisplayServerOpenHarmony() {
	virtual_keyboard_hide();
	if (mouse_mode != DisplayServerEnums::MOUSE_MODE_VISIBLE) {
		ohos_wrapper_set_mouse_mode(OS_OpenHarmony::get_singleton()->get_window_id(), false, true, true);
	}
	if (rendering_device) {
		memdelete(rendering_device);
	}
	if (rendering_context) {
		memdelete(rendering_context);
	}
	if (native_menu) {
		memdelete(native_menu);
		native_menu = nullptr;
	}
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
	get_singleton()->keyboard_status = status;
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
	{
		MutexLock lock(ime_geometry_mutex);
		ime_text_rect = p_screen_rect;
	}
	if (keyboard_status == IME_KEYBOARD_STATUS_SHOW && keyboard_type == p_type) {
		_update_ime_cursor();
		return;
	}
	if (keyboard_status != IME_KEYBOARD_STATUS_NONE) {
		virtual_keyboard_hide();
	}

	{
		MutexLock lock(ime_geometry_mutex);
		keyboard_type = p_type;
	}
	text_editor_proxy = OH_TextEditorProxy_Create();
	attach_options = OH_AttachOptions_Create(true);

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

	InputMethod_ErrorCode code = OH_InputMethodController_Attach(text_editor_proxy, attach_options, &input_method_proxy);
	if (code != IME_ERR_OK) {
		virtual_keyboard_hide();
		ERR_FAIL_MSG(vformat("Failed to attach input method controller: %d.", code));
	}
	ime_screen_position_valid = false;
	_update_ime_cursor();
}

void DisplayServerOpenHarmony::virtual_keyboard_hide() {
	if (keyboard_status == IME_KEYBOARD_STATUS_SHOW) {
		if (OH_InputMethodProxy_HideKeyboard(input_method_proxy) != IME_ERR_OK) {
			ERR_PRINT("Failed to hide keyboard.");
		}
	}
	if (input_method_proxy) {
		if (OH_InputMethodController_Detach(input_method_proxy) != IME_ERR_OK) {
			ERR_PRINT("Failed to detach input method controller.");
		}
		input_method_proxy = nullptr;
	}
	if (attach_options) {
		OH_AttachOptions_Destroy(attach_options);
		attach_options = nullptr;
	}
	if (text_editor_proxy) {
		OH_TextEditorProxy_Destroy(text_editor_proxy);
		text_editor_proxy = nullptr;
	}
	keyboard_status = IME_KEYBOARD_STATUS_NONE;
}

int DisplayServerOpenHarmony::virtual_keyboard_get_height() const {
	if (keyboard_status == IME_KEYBOARD_STATUS_SHOW) {
		int height = ohos_wrapper_get_keyboard_avoid_area(OS_OpenHarmony::get_singleton()->get_window_id());
		return height;
	}
	return 0;
}

bool DisplayServerOpenHarmony::_get_ime_screen_position(Point2 &r_position) const {
	Point2 local_position;
	{
		MutexLock lock(ime_geometry_mutex);
		// LineEdit/TextEdit submit the transformed caret before attaching.
		// A direct virtual_keyboard_show caller can use its supplied text area.
		local_position = ime_position_valid ? Point2(ime_position) : ime_text_rect.position + Point2(0, ime_text_rect.size.y);
	}
	double x, y;
	if (!ohos_wrapper_map_surface_point(OS_OpenHarmony::get_singleton()->get_window_id(), local_position.x, local_position.y, x, y)) {
		return false;
	}
	r_position = Point2(x, y);
	return true;
}

void DisplayServerOpenHarmony::_update_ime_cursor() {
	if (!ime_active || !input_method_proxy) {
		return;
	}
	Point2 screen_position;
	if (!_get_ime_screen_position(screen_position) || (ime_screen_position_valid && screen_position == last_ime_screen_position)) {
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
	// Not supported on OpenHarmony.
}

DisplayServerEnums::WindowMode DisplayServerOpenHarmony::window_get_mode(DisplayServerEnums::WindowID p_window) const {
	return DisplayServerEnums::WINDOW_MODE_WINDOWED;
}

void DisplayServerOpenHarmony::window_set_vsync_mode(DisplayServerEnums::VSyncMode p_vsync_mode, DisplayServerEnums::WindowID p_window) {
	if (rendering_context) {
		rendering_context->window_set_vsync_mode(DisplayServerEnums::MAIN_WINDOW_ID, p_vsync_mode);
	}
}

DisplayServerEnums::VSyncMode DisplayServerOpenHarmony::window_get_vsync_mode(DisplayServerEnums::WindowID p_window) const {
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
	_update_ime_cursor();
}

void DisplayServerOpenHarmony::_mouse_update_mode() {
	using namespace DisplayServerEnums;
	MouseMode wanted = mouse_mode_override_enabled ? mouse_mode_override : mouse_mode_base;
	if (wanted == mouse_mode) {
		return;
	}
	int32_t id = OS_OpenHarmony::get_singleton()->get_window_id();
	bool locked = wanted == MOUSE_MODE_CAPTURED || wanted == MOUSE_MODE_CONFINED || wanted == MOUSE_MODE_CONFINED_HIDDEN;
	bool visible = wanted == MOUSE_MODE_VISIBLE || wanted == MOUSE_MODE_CONFINED;
	int result = ohos_wrapper_set_mouse_mode(id, locked, wanted != MOUSE_MODE_CAPTURED, visible);
	ERR_FAIL_COND_MSG(result != 0, vformat("Cannot change window cursor mode: %d", result));
	mouse_mode = wanted;
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
