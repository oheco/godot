// Godot Engine contributors. SPDX-License-Identifier: MIT
// Compile-time tests of the exact production helper; no engine/executable,
// signing, system IME, ArkUI operation, or copied state algorithm is needed.
// clang++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only \
//     platform/openharmony/tests/test_host_focus.cpp
#include "../host_focus_state.h"

using GodotOpenHarmony::HostFocusState;

constexpr bool initial_and_legacy_surface_focus() {
	HostFocusState focus;
	if (!focus.is_window_active() || focus.is_surface_focused() || focus.is_effectively_focused()) {
		return false;
	}
	focus.window_event(2);
	if (!focus.is_effectively_focused()) {
		return false;
	}
	focus.window_event(3);
	return focus.is_window_active() && !focus.is_effectively_focused();
}
static_assert(initial_and_legacy_surface_focus(), "Initial surface false; legacy 2/3 retain surface focus behavior");

constexpr bool active_before_setup() {
	HostFocusState focus;
	focus.window_event(8);
	if (!focus.is_window_active() || focus.is_effectively_focused()) {
		return false;
	}
	focus.window_event(2);
	return focus.is_effectively_focused();
}
static_assert(active_before_setup(), "ACTIVE snapshot before setup is retained, but requires surface focus");

constexpr bool inactive_before_setup() {
	HostFocusState focus;
	focus.window_event(9);
	focus.window_event(2);
	return !focus.is_window_active() && focus.is_surface_focused() && !focus.is_effectively_focused();
}
static_assert(inactive_before_setup(), "INACTIVE before setup replaces legacy fallback despite surface focus");

constexpr bool shown_while_inactive() {
	HostFocusState focus;
	focus.window_event(9);
	focus.window_event(2);
	focus.window_event(1);
	return focus.is_visible() && !focus.is_window_active() && !focus.is_effectively_focused();
}
static_assert(shown_while_inactive(), "SHOWN cannot grant inactive windows focus");

constexpr bool surface_focus_while_inactive() {
	HostFocusState focus;
	focus.window_event(8);
	focus.window_event(9);
	focus.window_event(2);
	focus.window_event(3);
	focus.window_event(2);
	return focus.is_surface_focused() && !focus.is_window_active() && !focus.is_effectively_focused();
}
static_assert(surface_focus_while_inactive(), "Surface callbacks cannot restore window activation or legacy fallback");

constexpr bool activation_with_focused_surface() {
	HostFocusState focus;
	focus.window_event(9);
	focus.window_event(2);
	focus.window_event(8);
	if (!focus.is_effectively_focused()) {
		return false;
	}
	focus.window_event(9);
	if (!focus.is_surface_focused() || focus.is_effectively_focused()) {
		return false;
	}
	focus.window_event(8);
	return focus.is_effectively_focused();
}
static_assert(activation_with_focused_surface(), "Stage activation restores focus only while the surface remains focused");

constexpr bool surface_blur_with_active_window() {
	HostFocusState focus;
	focus.window_event(8);
	focus.window_event(2);
	focus.window_event(3);
	return focus.is_window_active() && !focus.is_surface_focused() && !focus.is_effectively_focused();
}
static_assert(surface_blur_with_active_window(), "Surface blur clears effective focus without changing actual activation");

constexpr bool hidden_then_shown() {
	HostFocusState focus;
	focus.window_event(8);
	focus.window_event(2);
	focus.window_event(1);
	focus.window_event(4);
	if (focus.is_visible() || focus.is_window_active() || focus.is_effectively_focused()) {
		return false;
	}
	focus.window_event(1);
	focus.window_event(5);
	if (!focus.is_visible() || focus.is_window_active() || focus.is_effectively_focused()) {
		return false;
	}
	focus.window_event(8);
	return focus.is_effectively_focused();
}
static_assert(hidden_then_shown(), "HIDDEN clears activation; SHOWN/foreground cannot replace a fresh ACTIVE snapshot");

constexpr bool background_then_foreground() {
	HostFocusState focus;
	focus.window_event(8);
	focus.window_event(2);
	focus.window_event(6);
	if (!focus.is_background() || focus.is_window_active() || focus.is_effectively_focused()) {
		return false;
	}
	focus.window_event(5);
	if (focus.is_background() || focus.is_window_active() || focus.is_effectively_focused()) {
		return false;
	}
	focus.window_event(8);
	return focus.is_effectively_focused();
}
static_assert(background_then_foreground(), "Background clears activation; resume notification alone cannot assert focus");

constexpr bool pending_mouse_and_inactive_snapshot() {
	HostFocusState focus;
	focus.window_event(8);
	focus.window_event(2); // Surface focus and mouse are now pending on engine thread.
	const bool focus_when_mouse_queued = focus.is_effectively_focused();
	focus.window_event(9); // ArkUI updates snapshot before pending mouse is dispatched.
	// Dispatch reads the live state, never the snapshot captured by a queued event.
	const bool focus_when_mouse_dispatched = focus.is_effectively_focused();
	const bool ime_attach_gate_when_mouse_dispatched = focus.is_window_active();
	return focus_when_mouse_queued && !focus_when_mouse_dispatched && !ime_attach_gate_when_mouse_dispatched;
}
static_assert(pending_mouse_and_inactive_snapshot(), "Pending mouse and old surface-focus events must observe the latest INACTIVE snapshot");

constexpr bool close_request_preserves_focus() {
	HostFocusState focus;
	focus.window_event(8);
	focus.window_event(2);
	focus.window_event(7);
	return focus.is_window_active() && focus.is_effectively_focused();
}
static_assert(close_request_preserves_focus(), "Close request remains independent from focus");
