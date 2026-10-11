// Godot Engine contributors. SPDX-License-Identifier: MIT
// Includes byte-exact production lifecycle methods and state declarations.
// Real SDK objects/callback registration; only service-facing calls are mocked.
#include <inputmethod/inputmethod_controller_capi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

static bool input_focus = true;
extern "C" bool godot_host_is_input_focused() { return input_focus; }

using String = std::string;
using Mutex = std::recursive_mutex;
using MutexLock = std::lock_guard<Mutex>;
struct Point2 {
	double x = 0, y = 0;
	Point2() = default;
	Point2(double p_x, double p_y) : x(p_x), y(p_y) {}
};
using Point2i = Point2;
struct Rect2 {
	Point2 position, size;
};
namespace DisplayServerEnums {
enum VirtualKeyboardType {
	KEYBOARD_TYPE_DEFAULT, KEYBOARD_TYPE_MULTILINE, KEYBOARD_TYPE_NUMBER,
	KEYBOARD_TYPE_NUMBER_DECIMAL, KEYBOARD_TYPE_PHONE, KEYBOARD_TYPE_EMAIL_ADDRESS,
	KEYBOARD_TYPE_PASSWORD, KEYBOARD_TYPE_URL,
};
} // namespace DisplayServerEnums
class OS_OpenHarmony {
public:
	static OS_OpenHarmony *get_singleton() { static OS_OpenHarmony os; return &os; }
	int32_t get_window_id() const { return 42; }
};
static int ohos_wrapper_get_keyboard_avoid_area(int32_t p_id) { return p_id == 42 ? 240 : -1; }

namespace {
int failures = 0;
int text_events = 0;
void check(bool p_condition, const char *p_message) {
	if (!p_condition) {
		++failures;
		fprintf(stderr, "FAIL %s\n", p_message);
	}
}
struct Calls {
	int attach = 0, detach = 0, show = 0, hide = 0, errors = 0;
	int editor_create = 0, editor_destroy = 0, options_create = 0, options_destroy = 0;
	bool editor_allocation_failure = false, options_allocation_failure = false;
	bool poison_failed_output = false, null_success_output = false, send_attach_status = false;
	InputMethod_KeyboardStatus attach_status = IME_KEYBOARD_STATUS_SHOW;
	InputMethod_ErrorCode attach_result = IME_ERR_OK, detach_result = IME_ERR_OK;
	InputMethod_ErrorCode show_result = IME_ERR_OK, hide_result = IME_ERR_OK;
	InputMethod_TextInputType configured_type = IME_TEXT_INPUT_TYPE_TEXT;
	InputMethod_EnterKeyType configured_enter = IME_ENTER_KEY_DONE;
	std::set<InputMethod_TextEditorProxy *> editors;
	std::set<InputMethod_AttachOptions *> options;
	std::map<InputMethod_InputMethodProxy *, InputMethod_TextEditorProxy *> attached;
	std::map<InputMethod_InputMethodProxy *, InputMethod_AttachOptions *> attached_options;
	std::vector<std::string> diagnostics;
	InputMethod_TextEditorProxy *late_editor = nullptr;
	OH_TextEditorProxy_SendKeyboardStatusFunc late_status = nullptr;
} calls;
unsigned char proxy_cookies[128];
int next_proxy = 0;

void error_log(const std::string &p_message) {
	check(!p_message.empty(), "error message is reported");
	++calls.errors;
	calls.diagnostics.push_back(p_message);
}
std::string vformat(const char *p_format, InputMethod_ErrorCode p_code) {
	char text[160];
	snprintf(text, sizeof(text), p_format, static_cast<int>(p_code));
	return text;
}
} // namespace

class DisplayServerOpenHarmony {
public:
#include "ime_lifecycle_members.inc"
	static DisplayServerOpenHarmony *singleton;
	int cursor_updates = 0;
	bool metadata_known = true;
	uint32_t current_display_id = 11;
	Point2 current_screen_position = Point2(17.5, 91.25);
	DisplayServerOpenHarmony() { singleton = this; ime_active = true; }
	~DisplayServerOpenHarmony() { virtual_keyboard_hide(); singleton = nullptr; }
	static DisplayServerOpenHarmony *get_singleton() { return singleton; }
	static void _get_text_config(InputMethod_TextEditorProxy *, InputMethod_TextConfig *);
	static void _send_keyboard_status(InputMethod_TextEditorProxy *, InputMethod_KeyboardStatus);
	static void _insert_text(InputMethod_TextEditorProxy *, const char16_t *, size_t) { ++text_events; }
	static void _delete_forward(InputMethod_TextEditorProxy *, int32_t) {}
	static void _delete_backward(InputMethod_TextEditorProxy *, int32_t) {}
	static void _send_enter_key(InputMethod_TextEditorProxy *, InputMethod_EnterKeyType) {}
	static void _move_cursor(InputMethod_TextEditorProxy *, InputMethod_Direction) {}
	static void _handle_set_selection(InputMethod_TextEditorProxy *, int32_t, int32_t) {}
	static void _handle_extend_action(InputMethod_TextEditorProxy *, InputMethod_ExtendAction) {}
	static void _get_left_text_of_cursor(InputMethod_TextEditorProxy *, int32_t, char16_t *, size_t *) {}
	static void _get_right_text_of_cursor(InputMethod_TextEditorProxy *, int32_t, char16_t *, size_t *) {}
	static int32_t _get_text_index_at_cursor(InputMethod_TextEditorProxy *) { return 0; }
	static int32_t _receive_private_command(InputMethod_TextEditorProxy *, InputMethod_PrivateCommand *[], size_t) { return 0; }
	static int32_t _set_preview_text(InputMethod_TextEditorProxy *, const char16_t *, size_t, int32_t, int32_t) { return 0; }
	static void _finish_text_preview(InputMethod_TextEditorProxy *) {}
	void virtual_keyboard_show(const String &, const Rect2 & = Rect2(), DisplayServerEnums::VirtualKeyboardType = DisplayServerEnums::KEYBOARD_TYPE_DEFAULT, int = -1, int = -1, int = -1);
	void virtual_keyboard_hide();
	void update_ime_focus(bool);
	int virtual_keyboard_get_height() const;
};
DisplayServerOpenHarmony *DisplayServerOpenHarmony::singleton = nullptr;
bool DisplayServerOpenHarmony::_get_ime_screen_position(Point2 &r_position, uint32_t *r_display_id) const {
	MutexLock lock(ime_geometry_mutex);
	if (!metadata_known) {
		return false;
	}
	r_position = current_screen_position;
	if (r_display_id) {
		*r_display_id = current_display_id;
	}
	return true;
}
void DisplayServerOpenHarmony::_update_ime_cursor() { ++cursor_updates; }

#define ERR_PRINT(message) error_log(message)
#define ERR_FAIL_MSG(message) do { error_log(message); return; } while (0)
#include "ime_lifecycle_methods.inc"
#undef ERR_FAIL_MSG
#undef ERR_PRINT

namespace {
// Probe on another thread: recursive mutexes must still be released before an
// SDK call that can invoke GetTextConfig/status synchronously or on an IPC thread.
bool sdk_locks_free() {
	auto *display = DisplayServerOpenHarmony::get_singleton();
	bool free = true;
	std::thread probe([&]() {
		for (auto *mutex : { &display->ime_status_mutex, &display->ime_geometry_mutex }) {
			if (mutex->try_lock()) {
				mutex->unlock();
			} else {
				free = false;
			}
		}
	});
	probe.join();
	check(free, "no status/geometry mutex is held across reentrant SDK calls");
	return free;
}
OH_TextEditorProxy_SendKeyboardStatusFunc status_func(InputMethod_TextEditorProxy *p_editor) {
	OH_TextEditorProxy_SendKeyboardStatusFunc func = nullptr;
	check(OH_TextEditorProxy_GetSendKeyboardStatusFunc(p_editor, &func) == IME_ERR_OK && func,
			"real SDK stores the production status callback");
	return func;
}
void status(InputMethod_TextEditorProxy *p_editor, InputMethod_KeyboardStatus p_status) {
	auto func = status_func(p_editor);
	if (func && sdk_locks_free()) {
		std::thread ipc([=]() { func(p_editor, p_status); });
		ipc.join();
	}
}
void late_status() {
	if (calls.late_status) {
		calls.late_status(calls.late_editor, IME_KEYBOARD_STATUS_SHOW);
	}
}
} // namespace

extern "C" InputMethod_TextEditorProxy *__real_OH_TextEditorProxy_Create();
extern "C" void __real_OH_TextEditorProxy_Destroy(InputMethod_TextEditorProxy *);
extern "C" InputMethod_AttachOptions *__real_OH_AttachOptions_Create(bool);
extern "C" void __real_OH_AttachOptions_Destroy(InputMethod_AttachOptions *);
extern "C" InputMethod_TextEditorProxy *__wrap_OH_TextEditorProxy_Create() {
	if (calls.editor_allocation_failure) {
		return nullptr;
	}
	auto *editor = __real_OH_TextEditorProxy_Create();
	check(editor && calls.editors.insert(editor).second, "real SDK editor allocation has unique live ownership");
	++calls.editor_create;
	return editor;
}
extern "C" void __wrap_OH_TextEditorProxy_Destroy(InputMethod_TextEditorProxy *p_editor) {
	for (const auto &entry : calls.attached) {
		check(entry.second != p_editor, "editor lives until its Detach call completes");
	}
	if (calls.editors.erase(p_editor) != 1) {
		check(false, "destroy exactly one owned editor");
		return;
	}
	++calls.editor_destroy;
	__real_OH_TextEditorProxy_Destroy(p_editor);
}
extern "C" InputMethod_AttachOptions *__wrap_OH_AttachOptions_Create(bool p_show) {
	check(p_show, "Attach preserves the soft-keyboard show intent");
	if (calls.options_allocation_failure) {
		return nullptr;
	}
	auto *options = __real_OH_AttachOptions_Create(p_show);
	check(options && calls.options.insert(options).second, "real SDK attach options have unique live ownership");
	++calls.options_create;
	return options;
}
extern "C" void __wrap_OH_AttachOptions_Destroy(InputMethod_AttachOptions *p_options) {
	for (const auto &entry : calls.attached_options) {
		check(entry.second != p_options, "attach options live until their Detach call completes");
	}
	if (calls.options.erase(p_options) != 1) {
		check(false, "destroy exactly one owned attach options object");
		return;
	}
	++calls.options_destroy;
	__real_OH_AttachOptions_Destroy(p_options);
}
extern "C" InputMethod_ErrorCode __wrap_OH_InputMethodController_Attach(InputMethod_TextEditorProxy *p_editor,
		InputMethod_AttachOptions *p_options, InputMethod_InputMethodProxy **r_proxy) {
	++calls.attach;
	check(calls.editors.count(p_editor) && calls.options.count(p_options), "Attach receives live real SDK objects");
	check(calls.attached.empty() && calls.attached_options.empty(), "replacement Attach follows deterministic Detach");
#ifndef IME_LIFECYCLE_BASELINE
	check(!DisplayServerOpenHarmony::get_singleton()->ime_display_id_valid, "display binding is published only after Attach succeeds");
#endif
	if (!sdk_locks_free()) {
		return IME_ERR_IMCLIENT;
	}
	late_status(); // Delayed notification from the preceding client during a new Attach.
	OH_TextEditorProxy_GetTextConfigFunc get_config = nullptr;
	check(OH_TextEditorProxy_GetGetTextConfigFunc(p_editor, &get_config) == IME_ERR_OK && get_config,
			"real SDK stores the production GetTextConfig callback");
	auto *config = OH_TextConfig_Create();
	check(config != nullptr, "real SDK creates reentrant TextConfig");
	if (config && get_config) {
		get_config(p_editor, config);
		check(OH_TextConfig_GetInputType(config, &calls.configured_type) == IME_ERR_OK, "read actual configured keyboard type");
		check(OH_TextConfig_GetEnterKeyType(config, &calls.configured_enter) == IME_ERR_OK, "read actual configured enter action");
		int32_t window = 0;
		check(OH_TextConfig_GetWindowId(config, &window) == IME_ERR_OK && window == 42, "preserve actual ArkUI window identity");
		OH_TextConfig_Destroy(config);
	}
	if (calls.send_attach_status) {
		status(p_editor, calls.attach_status);
	}
	if (calls.attach_result != IME_ERR_OK || calls.null_success_output) {
		*r_proxy = calls.poison_failed_output ? reinterpret_cast<InputMethod_InputMethodProxy *>(&proxy_cookies[127]) : nullptr;
		return calls.attach_result;
	}
	check(next_proxy < 127, "fixture proxy storage is bounded");
	*r_proxy = reinterpret_cast<InputMethod_InputMethodProxy *>(&proxy_cookies[next_proxy++]);
	calls.attached[*r_proxy] = p_editor;
	calls.attached_options[*r_proxy] = p_options;
	return IME_ERR_OK;
}
extern "C" InputMethod_ErrorCode __wrap_OH_InputMethodController_Detach(InputMethod_InputMethodProxy *p_proxy) {
	++calls.detach;
	const auto entry = calls.attached.find(p_proxy);
	if (entry == calls.attached.end()) {
		check(false, "Detach receives a successfully attached live proxy");
		return IME_ERR_IMCLIENT;
	}
	check(calls.editors.count(entry->second), "editor remains alive during Detach");
	const auto options = calls.attached_options.find(p_proxy);
	check(options != calls.attached_options.end() && calls.options.count(options->second), "attach options remain alive during Detach");
	auto *display = DisplayServerOpenHarmony::get_singleton();
	check(!display->text_editor_proxy && !display->input_method_proxy && !display->attach_options,
			"callback identity and ownership pointers are invalidated before Detach");
	check(display->keyboard_status == IME_KEYBOARD_STATUS_NONE, "visibility is invalidated before Detach");
	if (sdk_locks_free()) {
		status(entry->second, IME_KEYBOARD_STATUS_SHOW);
	}
	check(display->keyboard_status == IME_KEYBOARD_STATUS_NONE, "reentrant old callback cannot restore visibility during Detach");
	calls.late_editor = entry->second;
	calls.late_status = status_func(entry->second);
	calls.attached.erase(entry); // SDK proxy lifetime ends at this call, also on service failure.
	calls.attached_options.erase(p_proxy);
	return calls.detach_result;
}
extern "C" InputMethod_ErrorCode __wrap_OH_InputMethodProxy_ShowKeyboard(InputMethod_InputMethodProxy *p_proxy) {
	++calls.show;
	check(calls.attached.count(p_proxy), "ShowKeyboard reuses the live attached proxy");
	sdk_locks_free();
	return calls.show_result;
}
extern "C" InputMethod_ErrorCode __wrap_OH_InputMethodProxy_HideKeyboard(InputMethod_InputMethodProxy *p_proxy) {
	++calls.hide;
	const auto entry = calls.attached.find(p_proxy);
	check(entry != calls.attached.end(), "HideKeyboard receives a live attached proxy");
	if (entry != calls.attached.end() && sdk_locks_free()) {
		status(entry->second, IME_KEYBOARD_STATUS_HIDE);
	}
	return calls.hide_result;
}

namespace {
void begin() {
	input_focus = true;
	check(calls.editors.empty() && calls.options.empty() && calls.attached.empty() && calls.attached_options.empty(), "preceding scenario releases every resource");
	calls = Calls{};
	calls.hide_result = IME_ERR_IMMS;
	text_events = 0;
}
void clean(const DisplayServerOpenHarmony &p_display) {
	check(calls.hide == 0, "client cleanup never calls the configured-to-fail Hide service");
	check(!p_display.input_method_proxy && !p_display.text_editor_proxy && !p_display.attach_options,
			"cleanup resets all production ownership pointers");
	check(p_display.keyboard_status == IME_KEYBOARD_STATUS_NONE && p_display.virtual_keyboard_get_height() == 0,
			"cleanup invalidates status even after reentrant old callbacks");
	check(calls.editors.empty() && calls.options.empty() && calls.attached.empty() && calls.attached_options.empty(), "cleanup releases all SDK objects");
#ifndef IME_LIFECYCLE_BASELINE
	check(!p_display.ime_display_id_valid, "cleanup invalidates the attached display binding");
#endif
}
void report(const char *p_name, int p_before) {
	printf("%s %s: attach=%d detach=%d show=%d hide=%d editor=%d/%d options=%d/%d errors=%d\n",
			failures == p_before ? "PASS" : "FAIL", p_name, calls.attach, calls.detach, calls.show, calls.hide,
			calls.editor_create, calls.editor_destroy, calls.options_create, calls.options_destroy, calls.errors);
}
void double_show_without_status() {
	const int before = failures;
	begin();
	DisplayServerOpenHarmony display;
	display.virtual_keyboard_show("");
	auto *editor = display.text_editor_proxy;
	auto *proxy = display.input_method_proxy;
	check(proxy && display.keyboard_status == IME_KEYBOARD_STATUS_NONE, "Attach succeeds without a visibility callback");
	Rect2 moved;
	moved.position = Point2(53, 70);
	display.virtual_keyboard_show("", moved);
	check(calls.attach == 1 && calls.detach == 0 && calls.editor_create == 1,
			"focus-enter then mouse-release double show keeps one live attachment");
	check(display.text_editor_proxy == editor && display.input_method_proxy == proxy, "double show preserves both proxy identities");
	check(calls.show == 1 && display.ime_text_rect.position.x == 53 && display.cursor_updates == 2,
			"repeated show requests soft keyboard and still updates geometry");
	display.virtual_keyboard_hide();
	check(calls.hide == 0 && calls.detach == 1, "explicit hide only detaches without a SHOW callback");
	clean(display);
	report("Attach without callback -> double show -> explicit hide", before);
}
void hidden_hardware_and_reconfigure() {
	const int before = failures;
	begin();
	DisplayServerOpenHarmony display;
	calls.send_attach_status = true;
	display.virtual_keyboard_show("");
	check(display.virtual_keyboard_get_height() == 240, "current IPC SHOW status controls keyboard height");
	auto *old_editor = display.text_editor_proxy;
	auto old_callback = status_func(old_editor);
	status(old_editor, IME_KEYBOARD_STATUS_HIDE);
	check(display.input_method_proxy && display.virtual_keyboard_get_height() == 0, "HIDE is visibility only and retains attachment");
	OH_TextEditorProxy_InsertTextFunc insert = nullptr;
	check(OH_TextEditorProxy_GetInsertTextFunc(old_editor, &insert) == IME_ERR_OK && insert, "hidden client retains text callback");
	if (insert) {
		insert(old_editor, u"a", 1);
	}
	check(text_events == 1, "hardware text entry stays registered while keyboard is hidden");
	display.virtual_keyboard_show("");
	check(calls.attach == 1 && calls.show == 1 && display.text_editor_proxy == old_editor, "HIDE then show reuses client and requests ShowKeyboard");
	calls.send_attach_status = false;
	display.virtual_keyboard_show("", Rect2(), DisplayServerEnums::KEYBOARD_TYPE_MULTILINE);
	check(calls.attach == 2 && calls.detach == 1 && calls.hide == 0 && display.text_editor_proxy != old_editor,
			"keyboard type change detaches then attaches a distinct callback identity");
	check(calls.configured_type == IME_TEXT_INPUT_TYPE_MULTILINE && calls.configured_enter == IME_ENTER_KEY_NEWLINE,
			"reentrant production GetTextConfig configures multiline/newline");
	check(display.keyboard_status == IME_KEYBOARD_STATUS_NONE, "late old status during replacement Attach is ignored");
	if (old_callback) {
		old_callback(old_editor, IME_KEYBOARD_STATUS_SHOW);
	}
	check(display.virtual_keyboard_get_height() == 0, "late old status after replacement cannot change new visibility");
	// Read height on the engine thread while an IPC thread alternates current
	// visibility and stale-client notifications. Only current identity may win.
	auto *current_editor = display.text_editor_proxy;
	auto current_callback = status_func(current_editor);
	std::thread ipc([=]() {
		for (int i = 0; i < 1000; ++i) {
			current_callback(current_editor, i % 2 ? IME_KEYBOARD_STATUS_SHOW : IME_KEYBOARD_STATUS_HIDE);
			old_callback(old_editor, IME_KEYBOARD_STATUS_SHOW);
		}
		current_callback(current_editor, IME_KEYBOARD_STATUS_HIDE);
		old_callback(old_editor, IME_KEYBOARD_STATUS_SHOW);
	});
	for (int i = 0; i < 1000; ++i) {
		const int height = display.virtual_keyboard_get_height();
		check(height == 0 || height == 240, "concurrent height read uses a coherent visibility snapshot");
	}
	ipc.join();
	check(display.virtual_keyboard_get_height() == 0, "stale IPC status cannot overwrite the current client's final HIDE");
	status(display.text_editor_proxy, IME_KEYBOARD_STATUS_SHOW);
	display.virtual_keyboard_show("", Rect2(), DisplayServerEnums::KEYBOARD_TYPE_MULTILINE);
	check(calls.show == 1 && calls.attach == 2, "visible same-type client reuses attachment without redundant ShowKeyboard");
	display.virtual_keyboard_hide();
	late_status();
	clean(display);
	report("HIDE hardware text -> type switch -> late old status", before);
}
void failures_and_retry() {
	const int before = failures;
	begin();
	DisplayServerOpenHarmony display;
	calls.attach_result = IME_ERR_IMCLIENT;
	calls.poison_failed_output = true;
	calls.send_attach_status = true;
	display.virtual_keyboard_show("");
	check(calls.attach == 1 && calls.detach == 0 && calls.hide == 0 && calls.errors == 1,
			"Attach IMCLIENT reports once, ignores poisoned output, and cleans without blanket retry");
	clean(display);
	calls.attach_result = IME_ERR_OK;
	calls.poison_failed_output = false;
	display.virtual_keyboard_show("");
	check(calls.attach == 2 && display.input_method_proxy, "next caller request attaches successfully after failure");
	status(display.text_editor_proxy, IME_KEYBOARD_STATUS_HIDE);
	calls.show_result = IME_ERR_IMMS;
	auto *proxy = display.input_method_proxy;
	display.virtual_keyboard_show("");
	check(display.input_method_proxy == proxy && calls.attach == 2 && calls.detach == 0 && calls.errors == 2,
			"ShowKeyboard failure preserves attachment for hardware input without reattach");
	calls.show_result = IME_ERR_OK;
	display.virtual_keyboard_show("");
	check(calls.show == 2 && calls.attach == 2, "next show request retries only ShowKeyboard on the same live client");
	calls.hide_result = IME_ERR_IMMS;
	calls.detach_result = IME_ERR_IMCLIENT;
	display.virtual_keyboard_hide();
	check(calls.hide == 0 && calls.detach == 1 && calls.errors == 3, "configured Hide failure is never called; Detach failure is reported once");
	check(calls.diagnostics == std::vector<std::string>{
			vformat("Failed to attach input method controller: %d.", IME_ERR_IMCLIENT),
			vformat("Failed to show keyboard: %d.", IME_ERR_IMMS),
			vformat("Failed to detach input method controller: %d.", IME_ERR_IMCLIENT)},
			"each injected Attach/Show/Detach numeric error is diagnosed exactly once");
	clean(display);
	// The SDK ends proxy lifetime at the Detach call. No callback is delivered
	// from an already destroyed client on a later, unrelated attachment.
	calls.late_status = nullptr;
	calls.hide_result = calls.detach_result = IME_ERR_OK;
	display.virtual_keyboard_show("");
	check(calls.attach == 3 && display.input_method_proxy, "caller can attach again after a failing Detach call");
	display.virtual_keyboard_hide();
	clean(display);
	report("Attach/Show/Detach failures -> caller retries, no Hide", before);
}
void allocation_and_null_output() {
	const int before = failures;
	begin();
	DisplayServerOpenHarmony display;
	calls.editor_allocation_failure = true;
	display.virtual_keyboard_show("");
	clean(display);
	check(calls.attach == 0 && calls.errors == 1, "failed editor allocation cleans partial options without Attach");
	calls.editor_allocation_failure = false;
	calls.options_allocation_failure = true;
	display.virtual_keyboard_show("");
	clean(display);
	check(calls.attach == 0 && calls.errors == 2, "failed options allocation cleans partial editor without Attach");
	calls.options_allocation_failure = false;
	calls.null_success_output = true;
	display.virtual_keyboard_show("");
	clean(display);
	check(calls.attach == 1 && calls.detach == 0 && calls.errors == 3, "success with null proxy is rejected and cleaned");
	calls.null_success_output = false;
	display.virtual_keyboard_show("");
	auto *proxy = display.input_method_proxy;
	calls.options_allocation_failure = true;
	display.virtual_keyboard_show("", Rect2(), DisplayServerEnums::KEYBOARD_TYPE_NUMBER);
	check(display.input_method_proxy == proxy && calls.attach == 2 && calls.detach == 0,
			"replacement allocation failure leaves the previous client attached");
	calls.options_allocation_failure = false;
	display.virtual_keyboard_hide();
	clean(display);
	report("partial allocation/null output cleanup and recovery", before);
}
void detach_focus_status_matrix() {
	const InputMethod_KeyboardStatus statuses[] = { IME_KEYBOARD_STATUS_NONE, IME_KEYBOARD_STATUS_HIDE, IME_KEYBOARD_STATUS_SHOW };
	const char *status_names[] = { "NONE pending show", "HIDE hardware keyboard", "SHOW" };
	const InputMethod_ErrorCode results[] = { IME_ERR_OK, IME_ERR_IMCLIENT, IME_ERR_IMMS };
	for (const bool focused : { true, false }) {
		for (int index = 0; index < 3; ++index) {
			for (const auto code : results) {
				const int before = failures;
				begin();
				DisplayServerOpenHarmony display;
				calls.hide_result = IME_ERR_IMMS; // Any redundant Hide would fail.
				calls.detach_result = code;
				calls.send_attach_status = statuses[index] != IME_KEYBOARD_STATUS_NONE;
				display.virtual_keyboard_show("", Rect2(), DisplayServerEnums::KEYBOARD_TYPE_NUMBER);
				auto *editor = display.text_editor_proxy;
				if (statuses[index] == IME_KEYBOARD_STATUS_HIDE) {
					status(editor, IME_KEYBOARD_STATUS_HIDE);
				}
				check(display.keyboard_status == statuses[index] && display.input_method_proxy,
						"matrix starts with a live client in the requested NONE/HIDE/SHOW state");
				input_focus = focused;
				display.virtual_keyboard_hide();
				check(calls.hide == 0 && calls.detach == 1, "every focus/status combination uses exactly one Detach and no Hide");
				check(calls.editor_create == 1 && calls.editor_destroy == 1 && calls.options_create == 1 && calls.options_destroy == 1,
						"every Detach result balances real SDK editor/options ownership");
				clean(display);
				const std::vector<std::string> expected = code == IME_ERR_OK ? std::vector<std::string>{} :
						std::vector<std::string>{ vformat("Failed to detach input method controller: %d.", code) };
				check(calls.diagnostics == expected && calls.errors == static_cast<int>(expected.size()),
						"injected Detach code produces its exact numeric diagnostic once, without a Hide error");
				late_status();
				display.virtual_keyboard_hide();
				check(calls.detach == 1 && calls.hide == 0 && calls.diagnostics == expected,
						"late status and repeated hide never retry an ended attachment or repeat its diagnostic");
				clean(display);
				char name[160];
				snprintf(name, sizeof(name), "%s x %s x Detach=%d", focused ? "focused" : "unfocused", status_names[index], static_cast<int>(code));
				report(name, before);
			}
		}
	}
}
#ifndef IME_LIFECYCLE_BASELINE
void check_binding(const DisplayServerOpenHarmony &p_display, uint32_t p_display_id, const Rect2 &p_rect) {
	check(p_display.ime_display_id_valid && p_display.ime_display_id == p_display_id, "successful client is bound to the known current display");
	check(p_display.virtual_keyboard_requested && p_display.requested_keyboard_type == DisplayServerEnums::KEYBOARD_TYPE_MULTILINE &&
			p_display.keyboard_type == DisplayServerEnums::KEYBOARD_TYPE_MULTILINE,
			"display rebinding preserves the live request and multiline keyboard type");
	check(calls.configured_type == IME_TEXT_INPUT_TYPE_MULTILINE && calls.configured_enter == IME_ENTER_KEY_NEWLINE,
			"real SDK config preserves multiline/newline on each display attachment");
	check(p_display.ime_text_rect.position.x == p_rect.position.x && p_display.ime_text_rect.position.y == p_rect.position.y &&
			p_display.ime_text_rect.size.x == p_rect.size.x && p_display.ime_text_rect.size.y == p_rect.size.y,
			"display rebinding preserves the retained control rectangle");
	check(calls.editors.size() == 1 && calls.options.size() == 1 && calls.attached.size() == 1 && calls.attached_options.size() == 1,
			"display binding owns exactly one real editor/options pair and service proxy");
}
Rect2 retained_rect() {
	Rect2 rect;
	rect.position = Point2(37, 48);
	rect.size = Point2(210, 31);
	return rect;
}
void display_round_trip_and_same_display_move() {
	const int before = failures;
	begin();
	DisplayServerOpenHarmony display;
	calls.hide_result = IME_ERR_IMMS;
	const Rect2 rect = retained_rect();
	display.current_display_id = 11;
	display.virtual_keyboard_show("", rect, DisplayServerEnums::KEYBOARD_TYPE_MULTILINE);
	check_binding(display, 11, rect);
	auto *first_editor = display.text_editor_proxy;
	auto first_callback = status_func(first_editor);
	display._update_ime_display();
	check(calls.attach == 1 && calls.detach == 0 && calls.show == 0, "same display does not rebind or redundantly show a pending keyboard");
	// Global and local positions stay exactly equal across A -> B -> A.
	display.current_display_id = 22;
	display._update_ime_display();
	check(calls.attach == 2 && calls.detach == 1 && calls.hide == 0 && display.text_editor_proxy != first_editor,
			"equal coordinates on display B still allocate a distinct editor then Detach and Attach");
	check(calls.editor_create == 2 && calls.editor_destroy == 1 && calls.options_create == 2 && calls.options_destroy == 1,
			"A to B has balanced replacement allocation and one surviving owner");
	check_binding(display, 22, rect);
	if (first_callback) {
		first_callback(first_editor, IME_KEYBOARD_STATUS_SHOW);
	}
	check(display.keyboard_status == IME_KEYBOARD_STATUS_NONE && display.virtual_keyboard_get_height() == 0,
			"old display callback cannot change replacement visibility");
	auto *second_editor = display.text_editor_proxy;
	display.current_display_id = 11;
	display._update_ime_display();
	check(calls.attach == 3 && calls.detach == 2 && calls.hide == 0 && display.text_editor_proxy != second_editor,
			"return to display A at the same local coordinates rebinds exactly once");
	check_binding(display, 11, rect);
	display.current_screen_position = Point2(450, 600);
	display._update_ime_display();
	display._update_ime_display();
	check(calls.attach == 3 && calls.detach == 2 && calls.show == 0 && display.text_editor_proxy != second_editor,
			"movement within the same display keeps ownership and never rebinds");
	display.virtual_keyboard_hide();
	check(calls.attach == 3 && calls.detach == 3 && calls.hide == 0 && calls.errors == 0,
			"A to B to A and final hide have balanced Detach-only cleanup");
	clean(display);
	report("A -> B -> A equal coordinates; same-display move stays attached", before);
}
void unknown_display_metadata_recovery() {
	const int before = failures;
	begin();
	DisplayServerOpenHarmony display;
	calls.hide_result = IME_ERR_IMMS;
	const Rect2 rect = retained_rect();
	display.metadata_known = false;
	display.virtual_keyboard_show("", rect, DisplayServerEnums::KEYBOARD_TYPE_MULTILINE);
	check(calls.attach == 1 && display.input_method_proxy && !display.ime_display_id_valid,
			"initial Attach without geometry retains an explicitly unknown display binding");
	display.current_display_id = 22;
	display._update_ime_display();
	check(calls.attach == 1 && calls.detach == 0, "unknown metadata does not trigger speculative rebinding");
	display.metadata_known = true;
	display._update_ime_display();
	check(calls.attach == 2 && calls.detach == 1 && calls.hide == 0,
			"known metadata recovers an unknown live binding through one balanced replacement");
	check_binding(display, 22, rect);
	display._update_ime_display();
	check(calls.attach == 2 && calls.detach == 1, "recovered same display does not rebind again");
	display.metadata_known = false;
	display.current_display_id = 33;
	display._update_ime_display();
	check(calls.attach == 2 && calls.detach == 1 && display.ime_display_id == 22 && display.ime_display_id_valid,
			"temporary missing geometry preserves the last successful binding without a guessed replacement");
	display.metadata_known = true;
	display._update_ime_display();
	check_binding(display, 33, rect);
	check(calls.attach == 3 && calls.detach == 2 && calls.hide == 0, "recovered changed display rebinds exactly once");
	display.virtual_keyboard_hide();
	clean(display);
	report("unknown metadata attachment and known-display recovery", before);
}
void display_attach_failure_and_user_retry() {
	const int before = failures;
	begin();
	DisplayServerOpenHarmony display;
	calls.hide_result = IME_ERR_IMMS;
	const Rect2 rect = retained_rect();
	display.virtual_keyboard_show("", rect, DisplayServerEnums::KEYBOARD_TYPE_MULTILINE);
	check_binding(display, 11, rect);
	calls.attach_result = IME_ERR_IMCLIENT;
	calls.poison_failed_output = true;
	calls.send_attach_status = true;
	display.current_display_id = 22;
	display._update_ime_display();
	check(calls.attach == 2 && calls.detach == 1 && calls.hide == 0 && calls.errors == 1,
			"failed display replacement releases old client once and ignores poisoned Attach output");
	check(calls.editor_create == 2 && calls.editor_destroy == 2 && calls.options_create == 2 && calls.options_destroy == 2,
			"failed replacement cleans both real SDK allocation pairs");
	check(calls.diagnostics == std::vector<std::string>{ vformat("Failed to attach input method controller: %d.", IME_ERR_IMCLIENT) },
			"display Attach failure reports the exact injected numeric code once");
	clean(display);
	display._update_ime_display();
	check(calls.attach == 2 && calls.detach == 1 && calls.errors == 1,
			"engine display polling does not retry a failed attachment or publish a failed display");
	calls.attach_result = IME_ERR_OK;
	calls.poison_failed_output = false;
	calls.send_attach_status = false;
	calls.late_status = nullptr;
	display.virtual_keyboard_show("", rect, DisplayServerEnums::KEYBOARD_TYPE_MULTILINE);
	check(calls.attach == 3 && calls.detach == 1 && calls.errors == 1, "later user request retries Attach once on the actual current display");
	check_binding(display, 22, rect);
	display._update_ime_display();
	check(calls.attach == 3 && calls.detach == 1, "successful user retry does not cause another display rebind");
	display.virtual_keyboard_hide();
	clean(display);
	report("display Attach failure -> explicit user retry preserves type", before);
}
void display_move_focus_defer_and_hide_cancel() {
	const int before = failures;
	begin();
	DisplayServerOpenHarmony display;
	calls.hide_result = IME_ERR_IMMS;
	const Rect2 rect = retained_rect();
	display.virtual_keyboard_show("", rect, DisplayServerEnums::KEYBOARD_TYPE_MULTILINE);
	input_focus = false;
	display.current_display_id = 22;
	display._update_ime_display();
	check(calls.attach == 1 && calls.detach == 0 && display.ime_display_id == 11,
			"cross-display move without valid focus never rebinds a still-live client");
	display.update_ime_focus(false);
	check(calls.detach == 1 && calls.hide == 0 && display.virtual_keyboard_requested,
			"focus loss detaches once while retaining the request for the moved display");
	clean(display);
	display._update_ime_display();
	check(calls.attach == 1 && calls.detach == 1, "display polling while unfocused defers new allocation and Attach");
	input_focus = true;
	display.update_ime_focus(true);
	check(calls.attach == 2 && calls.detach == 1, "focus recovery attaches exactly once on display B");
	check_binding(display, 22, rect);
	display._update_ime_display();
	check(calls.attach == 2 && calls.detach == 1, "post-recovery display polling keeps the recovered client");
	input_focus = false;
	display.update_ime_focus(false);
	check(calls.detach == 2 && display.virtual_keyboard_requested, "second focus handoff retains a pending request");
	display.virtual_keyboard_hide();
	display.current_display_id = 11;
	input_focus = true;
	display._update_ime_display();
	display.update_ime_focus(true);
	check(calls.attach == 2 && calls.detach == 2 && calls.hide == 0 && !display.virtual_keyboard_requested && calls.errors == 0,
			"explicit hide cancels a deferred moved-display request; focus or display recovery cannot resurrect it");
	clean(display);
	report("focus-loss move defers binding; explicit hide cancels recovery", before);
}
void focus_handoff_and_cancelled_request() {
	const int before = failures;
	begin();
	DisplayServerOpenHarmony display;
	input_focus = false;
	display.virtual_keyboard_show("", Rect2(), DisplayServerEnums::KEYBOARD_TYPE_NUMBER);
	display.virtual_keyboard_show("", Rect2(), DisplayServerEnums::KEYBOARD_TYPE_EMAIL_ADDRESS);
	check(calls.attach == 0 && calls.editor_create == 0 && display.virtual_keyboard_requested,
			"inactive requests are retained without allocating or attaching");
	input_focus = true;
	display.update_ime_focus(true);
	check(calls.attach == 1 && calls.configured_type == IME_TEXT_INPUT_TYPE_EMAIL_ADDRESS,
			"activation attaches once using the latest requested type");
	input_focus = false;
	display.update_ime_focus(false);
	check(calls.detach == 1 && !display.input_method_proxy && display.virtual_keyboard_requested,
			"focus loss suspends attachment and keeps the control request");
	input_focus = true;
	display.update_ime_focus(true);
	check(calls.attach == 2 && display.input_method_proxy,
			"legitimate focus recovery services the retained request once");
	input_focus = false;
	display.update_ime_focus(false);
	display.virtual_keyboard_hide();
	input_focus = true;
	display.update_ime_focus(true);
	check(calls.attach == 2 && !display.virtual_keyboard_requested,
			"explicit hide cancels the suspended request before later activation");
	clean(display);
	input_focus = false;
	display.virtual_keyboard_show("");
	display.virtual_keyboard_hide();
	input_focus = true;
	display.update_ime_focus(true);
	check(calls.attach == 2 && !display.input_method_proxy,
			"cancelled deferred request is never resurrected");
	clean(display);
	report("focus handoff, latest deferred request and explicit cancellation", before);
}
#endif
} // namespace

int main() {
	double_show_without_status();
	hidden_hardware_and_reconfigure();
	failures_and_retry();
	allocation_and_null_output();
	detach_focus_status_matrix();
#ifndef IME_LIFECYCLE_BASELINE
	focus_handoff_and_cancelled_request();
	display_round_trip_and_same_display_move();
	unknown_display_metadata_recovery();
	display_attach_failure_and_user_retry();
	display_move_focus_defer_and_hide_cancel();
#endif
	printf("%s native production IME client lifecycle: %d assertion failures; real SDK editor/options/config; service calls mocked\n",
			failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
