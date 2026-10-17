/**************************************************************************/
/*  bridge_openharmony.cpp                                                */
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

#include "bridge_openharmony.h"

#include "core/input/input.h"
#include "core/input/input_event.h"

void godot_touch(GodotTouchEvent *p_event, int count) {
	static Vector<GodotTouchEvent> last_touch_events;
	for (int i = 0; i < count; i++) {
		GodotTouchEvent &event = p_event[i];
		if (event.id >= last_touch_events.size()) {
			last_touch_events.resize(event.id + 1);
		}
		switch (event.type) {
			case 0: { // Touch begin
				Ref<InputEventScreenTouch> ev;
				ev.instantiate();
				ev->set_index(event.id);
				ev->set_pressed(true);
				ev->set_position(Vector2(event.x, event.y));
				Input::get_singleton()->parse_input_event(ev);
			} break;
			case 1: { // Touch up
				Ref<InputEventScreenTouch> ev;
				ev.instantiate();
				ev->set_index(event.id);
				ev->set_pressed(false);
				ev->set_position(Vector2(event.x, event.y));
				Input::get_singleton()->parse_input_event(ev);
			} break;
			case 2: { // Touch move
				Ref<InputEventScreenDrag> ev;
				ev.instantiate();
				ev->set_index(event.id);
				ev->set_position(Vector2(event.x, event.y));
				ev->set_relative(Vector2(event.x - last_touch_events[event.id].x, event.y - last_touch_events[event.id].y));
				ev->set_relative_screen_position(ev->get_relative());
				Input::get_singleton()->parse_input_event(ev);
			} break;
			case 3: { // Touch cancel
				Ref<InputEventScreenTouch> ev;
				ev.instantiate();
				ev->set_index(event.id);
				ev->set_canceled(true);
				ev->set_position(Vector2(event.x, event.y));
				Input::get_singleton()->parse_input_event(ev);
			} break;
		}
		last_touch_events.set(event.id, event);
	}
}

void godot_mouse(GodotMouseEvent *p_event) {
	static GodotMouseEvent last_mouse_event;
	static bool has_mouse_position = false;
	GodotMouseEvent &event = *p_event;
	const MouseButton button = MouseButton(event.button);
	const bool wheel = button >= MouseButton::WHEEL_UP && button <= MouseButton::WHEEL_RIGHT;
	if ((event.type == 0 || event.type == 1) && (button == MouseButton::NONE || (wheel && !(event.factor > 0)))) {
		return;
	}
	switch (event.type) {
		case 0: { // Mouse down
			Ref<InputEventMouseButton> ev;
			ev.instantiate();
			ev->set_pressed(true);
			ev->set_position(Vector2(event.x, event.y));
			ev->set_global_position(ev->get_position());
			ev->set_button_index(button);
			BitField<MouseButtonMask> mask(event.mask);
			mask.set_flag(mouse_button_to_mask(button));
			ev->set_button_mask(mask);
			ev->set_double_click(event.double_click);
			ev->set_factor(event.factor > 0 ? event.factor : 1.0);
			ev->set_alt_pressed(event.alt);
			ev->set_ctrl_pressed(event.ctrl);
			ev->set_shift_pressed(event.shift);
			ev->set_meta_pressed(event.meta);
			Input::get_singleton()->parse_input_event(ev);
		} break;
		case 1: { // Mouse up
			Ref<InputEventMouseButton> ev;
			ev.instantiate();
			ev->set_pressed(false);
			ev->set_position(Vector2(event.x, event.y));
			ev->set_global_position(ev->get_position());
			ev->set_button_index(button);
			BitField<MouseButtonMask> mask(event.mask);
			mask.clear_flag(mouse_button_to_mask(button));
			ev->set_button_mask(mask);
			ev->set_factor(event.factor > 0 ? event.factor : 1.0);
			ev->set_alt_pressed(event.alt);
			ev->set_ctrl_pressed(event.ctrl);
			ev->set_shift_pressed(event.shift);
			ev->set_meta_pressed(event.meta);
			Input::get_singleton()->parse_input_event(ev);
		} break;
		case 2: { // Mouse move
			Ref<InputEventMouseMotion> ev;
			ev.instantiate();
			ev->set_position(Vector2(event.x, event.y));
			ev->set_global_position(ev->get_position());
			Vector2 relative = has_mouse_position ? Vector2(event.x - last_mouse_event.x, event.y - last_mouse_event.y) : Vector2();
			// API 26 rawDelta values are hardware counts, not screen pixels. Use
			// them only for captured input; visible/confined GUI motion follows
			// the accelerated, density-converted absolute pointer coordinates.
			Input *input = Input::get_singleton();
			const InputClassEnums::MouseMode mode = input->is_mouse_mode_override_enabled() ? input->get_mouse_mode_override() : input->get_mouse_mode();
			if (event.has_relative && mode == InputClassEnums::MOUSE_MODE_CAPTURED) {
				relative = Vector2(event.relative_x, event.relative_y);
			}
			ev->set_relative(relative);
			ev->set_relative_screen_position(relative);
			// Input tracks mouse velocity but does not fill the event. PopupMenu
			// rejects stationary events, unlike ordinary Control hover handling.
			ev->set_velocity(Input::get_singleton()->get_last_mouse_velocity());
			ev->set_screen_velocity(Input::get_singleton()->get_last_mouse_screen_velocity());
			ev->set_button_mask(BitField<MouseButtonMask>(event.mask));
			ev->set_alt_pressed(event.alt);
			ev->set_ctrl_pressed(event.ctrl);
			ev->set_shift_pressed(event.shift);
			ev->set_meta_pressed(event.meta);
			Input::get_singleton()->parse_input_event(ev);
		} break;
		case 3: { // Continuous two-finger touchpad scrolling.
			Ref<InputEventPanGesture> ev;
			ev.instantiate();
			ev->set_position(Vector2(event.x, event.y));
			ev->set_delta(Vector2(event.relative_x, event.relative_y));
			ev->set_alt_pressed(event.alt);
			ev->set_ctrl_pressed(event.ctrl);
			ev->set_shift_pressed(event.shift);
			ev->set_meta_pressed(event.meta);
			Input::get_singleton()->parse_input_event(ev);
		} break;
		case 4: { // Incremental touchpad pinch ratio.
			Ref<InputEventMagnifyGesture> ev;
			ev.instantiate();
			ev->set_position(Vector2(event.x, event.y));
			ev->set_factor(event.factor);
			ev->set_alt_pressed(event.alt);
			ev->set_ctrl_pressed(event.ctrl);
			ev->set_shift_pressed(event.shift);
			ev->set_meta_pressed(event.meta);
			Input::get_singleton()->parse_input_event(ev);
		} break;
	}
	last_mouse_event = event;
	has_mouse_position = true;
}

void godot_key(GodotKeyEvent *p_event) {
	GodotKeyEvent &event = *p_event;
	Ref<InputEventKey> ev;
	ev.instantiate();
	ev->set_pressed(event.pressed);
	ev->set_echo(event.echo);
	ev->set_keycode(Key(event.code));
	ev->set_physical_keycode(Key(event.code));
	ev->set_key_label(Key(event.code));
	ev->set_unicode(event.unicode);
	ev->set_location(KeyLocation::UNSPECIFIED);
	ev->set_alt_pressed(event.alt);
	ev->set_ctrl_pressed(event.ctrl);
	ev->set_shift_pressed(event.shift);
	ev->set_meta_pressed(event.meta);
	Input::get_singleton()->parse_input_event(ev);
}
