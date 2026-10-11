// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

namespace GodotOpenHarmony {
// Pure host snapshot normalization, shared by production and focused tests.
// The caller serializes updates/reads; queued events must not replay old focus.
class HostFocusState {
	// Compatibility for templates that report only surface focus (2/3). The
	// first actual window snapshot (8/9) replaces this initial fallback.
	bool window_active = true;
	bool surface_focused = false;
	bool visible = false;
	bool background = false;

public:
	constexpr void window_event(int p_event) {
		switch (p_event) {
			case 1: // SHOWN: visibility is independent of activation.
				visible = true;
				break;
			case 2: // XComponent surface focus in.
				surface_focused = true;
				break;
			case 3: // XComponent surface focus out.
				surface_focused = false;
				break;
			case 4: // HIDDEN: a hidden window cannot remain active.
				visible = false;
				window_active = false;
				break;
			case 5: // Foreground does not imply window activation.
				background = false;
				break;
			case 6: // Background also invalidates window activation.
				background = true;
				window_active = false;
				break;
			case 8: // Actual window ACTIVE / initial isFocused() == true.
				window_active = true;
				break;
			case 9: // Actual window INACTIVE / initial isFocused() == false.
				window_active = false;
				break;
		}
	}

	constexpr bool is_window_active() const { return window_active; }
	constexpr bool is_surface_focused() const { return surface_focused; }
	constexpr bool is_effectively_focused() const { return window_active && surface_focused; }
	constexpr bool is_visible() const { return visible; }
	constexpr bool is_background() const { return background; }
};
} // namespace GodotOpenHarmony
