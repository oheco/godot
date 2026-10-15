// Godot Engine contributors. SPDX-License-Identifier: MIT
// The runner supplies byte-exact production methods, members and mouse enum.
// Only the OS clock and cursor SDK boundary are mocked; no real input is touched.
#include "host_focus_state.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <mutex>

namespace DisplayServerEnums {
#include "mouse_capture_enum.inc"
} // namespace DisplayServerEnums

namespace {
GodotOpenHarmony::HostFocusState focus_state;
auto &focus = focus_state;
std::mutex event_mutex;
bool stopping = false;
uint64_t now_usec = 1000000;
int checks = 0;
int failures = 0;
std::vector<std::string> diagnostics;

void check(bool p_condition, const char *p_message) {
	++checks;
	if (!p_condition) {
		++failures;
		std::fprintf(stderr, "FAIL %s\n", p_message);
	}
}

struct CursorSDK {
	int attempts = 0;
	int errors = 0;
	int focus_errors = 0;
	int automatic_unlocks = 0;
	int failures_remaining = 0;
	int failure_result = 71;
	bool deactivate_during_call = false;
	bool locked = false;
	bool follow = true;
	bool visible = true;
	bool captured() const { return locked && !follow; }
} sdk;

void host_event(int p_event) {
	const bool was_active = focus.is_window_active();
	focus.window_event(p_event);
	// Model the system independently of Godot's request/applied bookkeeping.
	// INACTIVE/HIDDEN/background release LockCursor before any engine pump.
	if (was_active && !focus.is_window_active()) {
		sdk.locked = false;
		++sdk.automatic_unlocks;
	}
}

void begin(bool p_active = true, bool p_surface_focused = true) {
	focus = GodotOpenHarmony::HostFocusState{};
	sdk = CursorSDK{};
	stopping = false;
	diagnostics.clear();
	now_usec = 1000000;
	host_event(p_active ? 8 : 9);
	if (p_surface_focused) {
		host_event(2);
	}
}

std::string vformat(const char *p_format, int p_code) {
	char text[160];
	std::snprintf(text, sizeof(text), p_format, p_code);
	return text;
}
void error_log(const std::string &p_message) { diagnostics.push_back(p_message); }
} // namespace

#include "mouse_capture_host_queries.inc"

class OS {
public:
	static OS *get_singleton() { static OS os; return &os; }
	uint64_t get_ticks_usec() const { return now_usec; }
};
class OS_OpenHarmony {
public:
	static OS_OpenHarmony *get_singleton() { static OS_OpenHarmony os; return &os; }
	int32_t get_window_id() const { return 42; }
};

int ohos_wrapper_set_mouse_mode(int32_t p_id, bool p_locked, bool p_follow, bool p_visible) {
	++sdk.attempts;
	if (sdk.deactivate_during_call) {
		sdk.deactivate_during_call = false;
		host_event(9); // Activation can race between the gate and the SDK call.
	}
	if (p_id != 42 || !focus.is_window_active()) {
		++sdk.errors;
		++sdk.focus_errors;
		return 72;
	}
	if (sdk.failures_remaining > 0) {
		--sdk.failures_remaining;
		++sdk.errors;
		return sdk.failure_result;
	}
	sdk.locked = p_locked;
	sdk.follow = p_follow;
	sdk.visible = p_visible;
	return 0;
}

struct RenderingDevice {};
struct RenderingContextDriver {};
struct NativeMenu {};
class DisplayServerOpenHarmony {
public:
	RenderingDevice *rendering_device = nullptr;
	RenderingContextDriver *rendering_context = nullptr;
	NativeMenu *native_menu = nullptr;
	void virtual_keyboard_hide() {}
	~DisplayServerOpenHarmony();
#include "mouse_capture_members.inc"
	void update_mouse_focus(bool p_focused);
	void _mouse_update_mode();
	void mouse_set_mode(DisplayServerEnums::MouseMode p_mode);
	void mouse_set_mode_override(DisplayServerEnums::MouseMode p_mode);
	void mouse_set_mode_override_enabled(bool p_enabled);
};

#define ERR_FAIL_MSG(message) do { error_log(message); return; } while (0)
#define ERR_FAIL_INDEX(index, size) do { if (static_cast<uint64_t>(index) >= static_cast<uint64_t>(size)) { error_log("Invalid mouse mode"); return; } } while (0)
#define memdelete(pointer) delete (pointer)
#include "mouse_capture_methods.inc"
#undef memdelete
#undef ERR_FAIL_INDEX
#undef ERR_FAIL_MSG

namespace {
using namespace DisplayServerEnums;

void untouched_visible_without_permission() {
	begin();
	sdk.failures_remaining = 100;
	sdk.failure_result = 201;
	DisplayServerOpenHarmony display;
	display._mouse_update_mode();
	display.mouse_set_mode(MOUSE_MODE_VISIBLE);
	for (const int event : { 9, 8, 3, 2, 6, 5, 8 }) {
		host_event(event);
		display.update_mouse_focus(focus.is_effectively_focused());
		display._mouse_update_mode();
	}
	check(sdk.attempts == 0 && diagnostics.empty() && !display.mouse_mode_managed,
			"untouched visible mode needs no privileged SDK calls across focus changes");
}

void release_visible_after_surface_blur() {
	begin();
	DisplayServerOpenHarmony display;
	display.mouse_set_mode(MOUSE_MODE_CAPTURED);
	host_event(3);
	display.update_mouse_focus(false);
	check(focus.is_window_active() && sdk.captured(), "surface-only blur does not cause the system to release capture");
	display.mouse_set_mode(MOUSE_MODE_VISIBLE);
	check(sdk.attempts == 2 && !sdk.locked && sdk.visible && !display.mouse_mode_managed,
			"visible request releases capture despite surface blur");
	display.mouse_set_mode(MOUSE_MODE_CAPTURED);
	check(sdk.attempts == 2, "surface blur still defers acquisition");
	host_event(2);
	display.update_mouse_focus(true);
	display.mouse_set_mode_override(MOUSE_MODE_VISIBLE);
	host_event(3);
	display.update_mouse_focus(false);
	display.mouse_set_mode_override_enabled(true);
	check(sdk.attempts == 4 && !sdk.locked && sdk.visible, "visible override releases capture during surface-only blur");
	check(sdk.focus_errors == 0 && diagnostics.empty(), "release uses actual window activation without invalid requests");
}

void active_stop_releases_cursor() {
	begin();
	{
		DisplayServerOpenHarmony display;
		display.mouse_set_mode(MOUSE_MODE_CAPTURED);
		stopping = true;
		check(!godot_host_is_window_active() && godot_host_is_window_active_for_cleanup(),
				"stop blocks new application but retains actual activation for teardown");
		display._mouse_update_mode();
		check(sdk.attempts == 1 && sdk.captured(), "stopping does not reacquire or alter capture before teardown");
	}
	check(sdk.attempts == 2 && !sdk.locked && sdk.visible, "production destructor unlocks and shows active cursor after stop");
	begin();
	{
		DisplayServerOpenHarmony display;
		display.mouse_set_mode(MOUSE_MODE_CAPTURED);
		host_event(9);
		stopping = true;
	}
	check(sdk.attempts == 1 && !sdk.locked, "inactive cleanup does not change another focused window's cursor");
}

void capture_restored_after_app_switch() {
	begin();
	DisplayServerOpenHarmony display;
	display.mouse_set_mode(MOUSE_MODE_CAPTURED);
	check(sdk.attempts == 1 && sdk.captured() && display.mouse_mode_applied, "focused capture reaches the OS");
	host_event(9);
	check(!sdk.captured() && sdk.automatic_unlocks == 1, "deactivation independently clears the OS lock");
	check(display.mouse_mode == MOUSE_MODE_CAPTURED && display.mouse_mode_base == MOUSE_MODE_CAPTURED,
			"system unlock preserves Godot's requested capture");
	display.update_mouse_focus(false);
	display._mouse_update_mode();
	check(!display.mouse_mode_applied && sdk.attempts == 1 && diagnostics.empty(), "inactive pump makes no cursor call or error");
	host_event(8);
	display.update_mouse_focus(true);
	check(sdk.attempts == 2 && sdk.captured() && display.mouse_mode_applied, "activation restores the requested OS capture");
	check(display.mouse_focus_generation == focus.get_window_focus_generation(), "restoration consumes the latest focus generation");
	for (int i = 0; i < 5; ++i) {
		display._mouse_update_mode();
		display.mouse_set_mode(MOUSE_MODE_CAPTURED);
		display.update_mouse_focus(true);
	}
	check(sdk.attempts == 2 && diagnostics.empty(), "successful capture and repeated focus/mode requests are deduplicated");
}

void request_and_change_while_unfocused() {
	begin(false);
	DisplayServerOpenHarmony display;
	display.mouse_set_mode(MOUSE_MODE_CAPTURED);
	check(display.mouse_mode == MOUSE_MODE_CAPTURED && !display.mouse_mode_applied && sdk.attempts == 0,
			"unfocused capture remains a logical pending request");
	display.mouse_set_mode(MOUSE_MODE_CONFINED);
	display.mouse_set_mode(MOUSE_MODE_HIDDEN);
	display._mouse_update_mode();
	check(display.mouse_mode_base == MOUSE_MODE_HIDDEN && display.mouse_mode == MOUSE_MODE_HIDDEN && sdk.attempts == 0,
			"unfocused changes retain only the latest requested mode without SDK calls");
	host_event(8);
	display.update_mouse_focus(true);
	check(sdk.attempts == 1 && !sdk.locked && sdk.follow && !sdk.visible && display.mouse_mode_applied,
			"activation applies the latest hidden request instead of stale capture");
}

void all_mouse_mode_flags() {
	begin();
	DisplayServerOpenHarmony display;
	const struct {
		MouseMode mode;
		bool locked, follow, visible;
	} modes[] = {
		{ MOUSE_MODE_VISIBLE, false, true, true },
		{ MOUSE_MODE_HIDDEN, false, true, false },
		{ MOUSE_MODE_CAPTURED, true, false, false },
		{ MOUSE_MODE_CONFINED, true, true, true },
		{ MOUSE_MODE_CONFINED_HIDDEN, true, true, false },
	};
	int expected = 0;
	for (const auto &mode : modes) {
		display.mouse_set_mode(mode.mode);
		if (mode.mode != MOUSE_MODE_VISIBLE) {
			++expected;
		}
		check(sdk.attempts == expected && display.mouse_mode_applied && display.mouse_mode == mode.mode,
				"each changed mouse mode is applied once");
		check(sdk.locked == mode.locked && sdk.follow == mode.follow && sdk.visible == mode.visible,
				"visible, hidden, captured and confined modes use their exact SDK flags");
		check(sdk.captured() == (mode.mode == MOUSE_MODE_CAPTURED), "OS capture is distinct from cursor confinement");
		display._mouse_update_mode();
		check(sdk.attempts == expected, "each successful mode is deduplicated on the next pump");
		host_event(9);
		display.update_mouse_focus(false);
		check(!sdk.locked && display.mouse_mode == mode.mode, "system deactivation releases each mode's lock and preserves its request");
		host_event(8);
		display.update_mouse_focus(true);
		if (mode.mode != MOUSE_MODE_VISIBLE) {
			++expected;
		}
		check(sdk.attempts == expected && display.mouse_mode_applied && sdk.locked == mode.locked &&
				sdk.follow == mode.follow && sdk.visible == mode.visible, "activation restores the exact flags for every capture/visibility mode");
	}
}

void override_and_base_selection() {
	begin();
	DisplayServerOpenHarmony display;
	display.mouse_set_mode(MOUSE_MODE_CAPTURED);
	display.mouse_set_mode_override(MOUSE_MODE_HIDDEN);
	check(sdk.attempts == 1 && sdk.captured() && !display.mouse_mode_override_enabled, "disabled override preserves base capture");
	display.mouse_set_mode_override_enabled(true);
	check(sdk.attempts == 2 && display.mouse_mode == MOUSE_MODE_HIDDEN && !sdk.locked && !sdk.visible,
			"enabling an override selects its requested mode");
	display.mouse_set_mode(MOUSE_MODE_CONFINED);
	check(sdk.attempts == 2 && display.mouse_mode_base == MOUSE_MODE_CONFINED && display.mouse_mode == MOUSE_MODE_HIDDEN,
			"base changes under an override are retained without changing the OS");
	display.mouse_set_mode_override(MOUSE_MODE_CAPTURED);
	check(sdk.attempts == 3 && sdk.captured(), "changing an enabled override applies it");
	display.mouse_set_mode_override_enabled(true);
	check(sdk.attempts == 3, "repeated override enable is deduplicated");
	display.mouse_set_mode_override_enabled(false);
	check(sdk.attempts == 4 && display.mouse_mode == MOUSE_MODE_CONFINED && sdk.locked && sdk.follow && sdk.visible,
			"disabling the override restores the latest base mode");
	display.mouse_set_mode_override(MOUSE_MODE_VISIBLE);
	check(sdk.attempts == 4 && display.mouse_mode == MOUSE_MODE_CONFINED, "disabled override edits do not replace the base mode");
}

void rapid_focus_switch_between_pumps() {
	begin();
	DisplayServerOpenHarmony display;
	display.mouse_set_mode(MOUSE_MODE_CAPTURED);
	const uint64_t generation = focus.get_window_focus_generation();
	host_event(9);
	host_event(8);
	check(focus.is_effectively_focused() && !sdk.captured() && display.mouse_mode_applied,
			"latest active snapshot can hide an OS unlock from the engine's applied flag");
	check(focus.get_window_focus_generation() == generation + 1, "actual helper retains deactivation between engine pumps");
	// No update_mouse_focus(false) notification: exercise generation recovery.
	display._mouse_update_mode();
	check(sdk.attempts == 2 && sdk.captured() && display.mouse_focus_generation == generation + 1,
			"generation restores capture even without an observed focus-out callback");
	host_event(9);
	host_event(9);
	host_event(8);
	host_event(8);
	display._mouse_update_mode();
	check(focus.get_window_focus_generation() == generation + 2 && sdk.attempts == 3 && sdk.captured(),
			"duplicate snapshots retain exactly one generation per real deactivation");
	display._mouse_update_mode();
	check(sdk.attempts == 3, "a consumed focus generation does not repeatedly reapply capture");
}

void effective_surface_and_window_focus() {
	begin(false, false);
	DisplayServerOpenHarmony display;
	display.mouse_set_mode(MOUSE_MODE_CAPTURED);
	host_event(8);
	display.update_mouse_focus(true);
	check(focus.is_window_active() && !focus.is_surface_focused() && sdk.attempts == 0,
			"window activation alone cannot apply capture without surface focus");
	host_event(2);
	display.update_mouse_focus(true);
	check(sdk.attempts == 1 && sdk.captured(), "surface and window focus together permit capture");
	const uint64_t generation = focus.get_window_focus_generation();
	host_event(3);
	display.update_mouse_focus(false);
	display._mouse_update_mode();
	check(sdk.attempts == 1 && focus.get_window_focus_generation() == generation,
			"surface blur blocks cursor calls without inventing window deactivation");
	host_event(2);
	display.update_mouse_focus(true);
	check(sdk.attempts == 2 && sdk.captured(), "surface refocus reapplies the retained request");
	host_event(9);
	host_event(1);
	host_event(5);
	display.update_mouse_focus(true);
	check(focus.is_surface_focused() && !focus.is_window_active() && sdk.attempts == 2 && !sdk.captured(),
			"surface focus, shown and foreground cannot substitute for actual window activation");
	host_event(8);
	display._mouse_update_mode();
	check(sdk.attempts == 3 && sdk.captured(), "actual activation restores capture after focus gating");
	for (const int event : { 4, 6 }) {
		host_event(event);
		display._mouse_update_mode();
		const int attempts = sdk.attempts;
		host_event(1);
		host_event(5);
		display._mouse_update_mode();
		check(!sdk.captured() && sdk.attempts == attempts, "hidden/background invalidation remains pending through shown/foreground");
		host_event(8);
		display._mouse_update_mode();
		check(sdk.attempts == attempts + 1 && sdk.captured(), "activation restores capture after hidden/background invalidation");
	}
	check(sdk.focus_errors == 0 && diagnostics.empty(), "effective-focus gating avoids invalid SDK calls and error logs");
}

void transient_failure_and_retry_throttle() {
	begin();
	DisplayServerOpenHarmony display;
	sdk.failures_remaining = 2;
	display.mouse_set_mode(MOUSE_MODE_CAPTURED);
	check(sdk.attempts == 1 && sdk.errors == 1 && !sdk.captured() && !display.mouse_mode_applied,
			"failed cursor API leaves the OS unlocked and the request pending");
	check(display.mouse_mode == MOUSE_MODE_CAPTURED && display.mouse_mode_retry_at_usec == 1500000,
			"failure retains capture and schedules a 500000us retry");
	for (const uint64_t time : { uint64_t(1000000), uint64_t(1016666), uint64_t(1499999) }) {
		now_usec = time;
		display._mouse_update_mode();
		display.mouse_set_mode(MOUSE_MODE_CAPTURED);
		display.update_mouse_focus(true);
	}
	check(sdk.attempts == 1 && diagnostics.size() == 1, "frames and repeated requests do not retry or log during backoff");
	now_usec = 1500000;
	display._mouse_update_mode();
	check(sdk.attempts == 2 && sdk.errors == 2 && !display.mouse_mode_applied && display.mouse_mode_retry_at_usec == 2000000,
			"retry at the exact boundary reports one failure and schedules another 500000us");
	now_usec = 1999999;
	display._mouse_update_mode();
	check(sdk.attempts == 2 && diagnostics.size() == 1, "same numeric error is suppressed while the second retry remains throttled");
	now_usec = 2000000;
	display._mouse_update_mode();
	check(sdk.attempts == 3 && sdk.captured() && display.mouse_mode_applied && diagnostics.size() == 1,
			"successful retry applies the retained capture without an additional error");
	check(diagnostics.size() == 1 && diagnostics[0] == "Cannot change window cursor mode: 71",
			"stable repeated failures retain their numeric result without repeated diagnostics");
	now_usec = 3000000;
	display._mouse_update_mode();
	check(sdk.attempts == 3, "successful retry returns to normal deduplication");
}

void mode_change_bypasses_backoff() {
	begin();
	DisplayServerOpenHarmony display;
	sdk.failures_remaining = 1;
	display.mouse_set_mode(MOUSE_MODE_CAPTURED);
	++now_usec;
	display.mouse_set_mode(MOUSE_MODE_CONFINED);
	check(sdk.attempts == 2 && display.mouse_mode_applied && sdk.locked && sdk.follow && sdk.visible,
			"a changed base mode bypasses the prior failed capture's backoff");
	sdk.failures_remaining = 1;
	display.mouse_set_mode(MOUSE_MODE_HIDDEN);
	display.mouse_set_mode_override(MOUSE_MODE_CAPTURED);
	check(sdk.attempts == 3 && !display.mouse_mode_applied, "disabled override edit preserves the current failed mode's backoff");
	++now_usec;
	display.mouse_set_mode_override_enabled(true);
	check(sdk.attempts == 4 && sdk.captured() && display.mouse_mode_applied && display.mouse_mode_retry_at_usec == 0,
			"an effective override mode change also bypasses backoff");
}

void activation_race_remains_pending() {
	begin();
	DisplayServerOpenHarmony display;
	sdk.deactivate_during_call = true;
	display.mouse_set_mode(MOUSE_MODE_CAPTURED);
	check(sdk.attempts == 1 && sdk.focus_errors == 1 && sdk.errors == 1 && !sdk.captured() && !display.mouse_mode_applied,
			"deactivation at the SDK boundary leaves failed capture pending");
	check(display.mouse_mode_base == MOUSE_MODE_CAPTURED && diagnostics.size() == 1 && diagnostics[0] == "Cannot change window cursor mode: 72",
			"activation race preserves the request and its numeric diagnostic");
	host_event(8);
	++now_usec;
	display.update_mouse_focus(true);
	check(sdk.attempts == 2 && sdk.captured() && display.mouse_mode_applied && display.mouse_mode_retry_at_usec == 0,
			"a new activation generation permits immediate recovery from the focus race");
}
} // namespace

int main() {
	const struct {
		const char *name;
		void (*run)();
	} cases[] = {
		{ "untouched_visible_without_permission", untouched_visible_without_permission },
		{ "release_visible_after_surface_blur", release_visible_after_surface_blur },
		{ "active_stop_releases_cursor", active_stop_releases_cursor },
		{ "capture_restored_after_app_switch_and_dedup", capture_restored_after_app_switch },
		{ "request_and_change_while_unfocused", request_and_change_while_unfocused },
		{ "all_mouse_mode_flags", all_mouse_mode_flags },
		{ "override_and_base_selection", override_and_base_selection },
		{ "rapid_focus_switch_between_pumps", rapid_focus_switch_between_pumps },
		{ "effective_surface_and_window_focus", effective_surface_and_window_focus },
		{ "transient_failure_and_retry_throttle", transient_failure_and_retry_throttle },
		{ "mode_change_bypasses_backoff", mode_change_bypasses_backoff },
		{ "activation_race_remains_pending", activation_race_remains_pending },
	};
	std::string results;
	for (const auto &scenario : cases) {
		const int before = failures;
		scenario.run();
		if (!results.empty()) {
			results += ',';
		}
		results += "{\"name\":\"" + std::string(scenario.name) + "\",\"passed\":" + (failures == before ? "true" : "false") + "}";
	}
	std::printf("{\"passed\":%s,\"checks\":%d,\"failures\":%d,\"cases\":[%s]}\n",
			failures == 0 ? "true" : "false", checks, failures, results.c_str());
	return failures == 0 ? 0 : 1;
}
