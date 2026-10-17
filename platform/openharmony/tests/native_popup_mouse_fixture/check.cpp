// Godot Engine contributors. SPDX-License-Identifier: MIT
// Real Input/Viewport/Button/PopupMenu with a test-only hybrid headless display.
// Mouse modes change only this display's in-memory state, never an OS cursor.
// The SDK window type must be declared before os_openharmony.h.
// clang-format off
#include <native_window/external_window.h>
// clang-format on

#include "core/input/input.h"
#include "main/main.h"
#include "scene/gui/button.h"
#include "scene/gui/popup_menu.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/display/display_server_headless.h"
#include "servers/rendering/dummy/rasterizer_dummy.h"

#include "platform/openharmony/bridge_openharmony.h"
#include "platform/openharmony/os_openharmony.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {
class PointerDisplay : public DisplayServerHeadless {
	DisplayServerEnums::MouseMode mode = DisplayServerEnums::MOUSE_MODE_VISIBLE;
	DisplayServerEnums::MouseMode override_mode = DisplayServerEnums::MOUSE_MODE_VISIBLE;
	bool override_enabled = false;

public:
	bool is_touchscreen_available() const override { return true; }
	void mouse_set_mode(DisplayServerEnums::MouseMode p_mode) override { mode = p_mode; }
	DisplayServerEnums::MouseMode mouse_get_mode() const override { return mode; }
	void mouse_set_mode_override(DisplayServerEnums::MouseMode p_mode) override { override_mode = p_mode; }
	DisplayServerEnums::MouseMode mouse_get_mode_override() const override { return override_mode; }
	void mouse_set_mode_override_enabled(bool p_enabled) override { override_enabled = p_enabled; }
	bool mouse_is_mode_override_enabled() const override { return override_enabled; }
	static Vector<String> drivers() {
		Vector<String> result;
		result.push_back("dummy");
		return result;
	}
	static DisplayServer *create(const String &, DisplayServerEnums::WindowMode, DisplayServerEnums::VSyncMode, uint32_t, const Vector2i *, const Vector2i &, int, DisplayServerEnums::Context, int64_t, Error &r_error) {
		r_error = OK;
		RasterizerDummy::make_current();
		return memnew(PointerDisplay);
	}
};

class EventRecorder : public Node {
protected:
	void input(const Ref<InputEvent> &p_event) override { events.push_back(p_event); }

public:
	Vector<Ref<InputEvent>> events;
};

class MotionButton : public Button {
protected:
	void gui_input(const Ref<InputEvent> &p_event) override {
		Ref<InputEventMouseMotion> motion = p_event;
		if (motion.is_valid()) {
			last_relative = motion->get_relative();
		}
		Button::gui_input(p_event);
	}

public:
	Vector2 last_relative;
};

int failures = 0;
void check(bool p_ok, const char *p_message) {
	std::printf("%s %s\n", p_ok ? "PASS" : "FAIL", p_message);
	if (!p_ok) {
		failures++;
	}
}

void frame(OS_OpenHarmony &p_os) {
	p_os.delay_usec(20000);
	p_os.main_loop_iterate();
}

void dispatch(OS_OpenHarmony &p_os, GodotMouseEvent p_event) {
	godot_mouse(&p_event);
	frame(p_os);
}

void move(OS_OpenHarmony &p_os, float p_x, float p_y, bool p_raw = false, float p_dx = 0, float p_dy = 0) {
	GodotMouseEvent event{};
	event.type = 2;
	event.x = p_x;
	event.y = p_y;
	event.has_relative = p_raw;
	event.relative_x = p_dx;
	event.relative_y = p_dy;
	dispatch(p_os, event);
}

void click(OS_OpenHarmony &p_os, float p_x, float p_y) {
	GodotMouseEvent event{};
	event.type = 0;
	event.button = 1;
	event.mask = 1;
	event.x = p_x;
	event.y = p_y;
	event.factor = 1;
	dispatch(p_os, event);
	event.type = 1;
	event.mask = 0;
	dispatch(p_os, event);
}

bool modifiers(const Ref<InputEventWithModifiers> &p_event) {
	return p_event.is_valid() && p_event->is_alt_pressed() && p_event->is_ctrl_pressed() && p_event->is_shift_pressed() && p_event->is_meta_pressed();
}

void expect_motion(EventRecorder *p_recorder, const Vector2 &p_position, const Vector2 &p_relative, const char *p_message) {
	Ref<InputEventMouseMotion> event = p_recorder->events.size() == 1 ? p_recorder->events[0] : Ref<InputEvent>();
	check(event.is_valid() && event->get_position() == p_position && event->get_global_position() == p_position &&
				event->get_relative() == p_relative && event->get_relative_screen_position() == p_relative,
			p_message);
	p_recorder->events.clear();
}

void pointer_modes(OS_OpenHarmony &p_os, EventRecorder *p_recorder) {
	Input *input = Input::get_singleton();
	input->set_mouse_mode(InputClassEnums::MOUSE_MODE_VISIBLE);
	check(input->get_mouse_mode() == InputClassEnums::MOUSE_MODE_VISIBLE, "visible mode is selected through real Input/DisplayServer getter");
	p_recorder->events.clear();
	// This is deliberately the first production bridge mouse event.
	move(p_os, 30, 35);
	expect_motion(p_recorder, Vector2(30, 35), Vector2(), "first absolute pointer position establishes a zero-relative baseline");
	move(p_os, 34, 37, true, 90, -70);
	expect_motion(p_recorder, Vector2(34, 37), Vector2(4, 2), "visible mode ignores nonzero raw hardware counts and uses absolute pixel movement");
	move(p_os, 34, 37, true, 5, -3);
	expect_motion(p_recorder, Vector2(34, 37), Vector2(), "visible stationary absolute position stays zero despite nonzero raw counts");
	input->set_mouse_mode(InputClassEnums::MOUSE_MODE_CONFINED);
	check(input->get_mouse_mode() == InputClassEnums::MOUSE_MODE_CONFINED, "confined mode is selected without an OS cursor call");
	move(p_os, 39, 40, true, 100, -100);
	expect_motion(p_recorder, Vector2(39, 40), Vector2(5, 3), "confined mode also uses absolute pixel movement");
	input->set_mouse_mode_override(InputClassEnums::MOUSE_MODE_CAPTURED);
	input->set_mouse_mode_override_enabled(true);
	check(input->get_mouse_mode() == InputClassEnums::MOUSE_MODE_CONFINED && input->is_mouse_mode_override_enabled() && input->get_mouse_mode_override() == InputClassEnums::MOUSE_MODE_CAPTURED,
			"base mode and captured override retain the production DisplayServer query contract");
	move(p_os, 39, 40, true, 5, -3);
	expect_motion(p_recorder, Vector2(39, 40), Vector2(5, -3), "captured mode preserves unscaled raw motion while absolute position stays fixed");
	move(p_os, 44, 43, true, 0, 0);
	expect_motion(p_recorder, Vector2(44, 43), Vector2(), "captured explicit zero raw delta stays zero even when absolute position changes");
	input->set_mouse_mode_override_enabled(false);
	input->set_mouse_mode(InputClassEnums::MOUSE_MODE_CAPTURED);
	move(p_os, 44, 43, true, -7, 9);
	expect_motion(p_recorder, Vector2(44, 43), Vector2(-7, 9), "captured base mode preserves raw movement without an override");
	input->set_mouse_mode_override(InputClassEnums::MOUSE_MODE_VISIBLE);
	input->set_mouse_mode_override_enabled(true);
	move(p_os, 47, 42, true, 90, -70);
	expect_motion(p_recorder, Vector2(47, 42), Vector2(3, -1), "visible override of captured base mode restores absolute pixel movement");
	input->set_mouse_mode_override_enabled(false);
	input->set_mouse_mode(InputClassEnums::MOUSE_MODE_VISIBLE);
	move(p_os, 41, 35, true, 0, 0);
	expect_motion(p_recorder, Vector2(41, 35), Vector2(-6, -7), "returning to visible mode restores absolute relative movement");
}

void wheel_and_gestures(OS_OpenHarmony &p_os, EventRecorder *p_recorder) {
	Input *input = Input::get_singleton();
	GodotMouseEvent event{};
	event.type = 0;
	event.button = 1;
	event.mask = 1;
	event.x = 60;
	event.y = 45;
	event.factor = 1;
	dispatch(p_os, event);
	event.button = 2;
	event.mask = 3;
	dispatch(p_os, event);
	check(input->is_mouse_button_pressed(MouseButton::LEFT) && input->is_mouse_button_pressed(MouseButton::RIGHT), "physical left and right buttons are held before wheel input");
	const float factors[] = { 0.125f, 0.5f, 1.75f, 2.25f };
	for (uint32_t button = 4; button <= 7; ++button) {
		const uint32_t wheel_bit = 1u << (button - 1);
		p_recorder->events.clear();
		event.type = 0;
		event.button = button;
		// Deliberately omit the wheel bit on press, include it on release: the
		// bridge must enforce both transitions while retaining physical bits.
		event.mask = 3;
		event.factor = factors[button - 4];
		event.alt = event.ctrl = event.shift = event.meta = true;
		dispatch(p_os, event);
		event.type = 1;
		event.mask = 3 | wheel_bit;
		dispatch(p_os, event);
		bool valid = p_recorder->events.size() == 2;
		for (int i = 0; i < p_recorder->events.size(); ++i) {
			Ref<InputEventMouseButton> wheel = p_recorder->events[i];
			valid = valid && wheel.is_valid() && wheel->get_button_index() == MouseButton(button) &&
					wheel->is_pressed() == (i == 0) && wheel->get_factor() == factors[button - 4] &&
					uint32_t(wheel->get_button_mask()) == (i == 0 ? 3 | wheel_bit : 3) &&
					wheel->get_position() == Vector2(60, 45) && wheel->get_global_position() == Vector2(60, 45) && modifiers(wheel);
		}
		std::printf("Wheel button=%u factor=%.3f events=%d\n", button, factors[button - 4], int(p_recorder->events.size()));
		check(valid, "vertical/horizontal wheel press and release preserve the same fractional factor, modifiers, position and corrected masks");
		check(input->is_mouse_button_pressed(MouseButton::LEFT) && input->is_mouse_button_pressed(MouseButton::RIGHT) &&
					!input->is_mouse_button_pressed(MouseButton(button)),
				"wheel pair leaves physical buttons held and no transient wheel bit stuck");
	}
	p_recorder->events.clear();
	for (float factor : { 0.0f, -0.25f, std::numeric_limits<float>::quiet_NaN() }) {
		event.factor = factor;
		for (uint32_t type : { 0u, 1u }) {
			event.type = type;
			dispatch(p_os, event);
		}
	}
	event.button = 0;
	event.factor = 1;
	dispatch(p_os, event);
	check(p_recorder->events.is_empty(), "zero/nonpositive/NaN wheel factors and NONE button create no native events");

	event.type = 3;
	event.button = 0;
	event.mask = 3;
	event.x = 73;
	event.y = 51;
	event.relative_x = 0.125f;
	event.relative_y = -0.75f;
	event.has_relative = true;
	p_recorder->events.clear();
	dispatch(p_os, event);
	std::printf("Pan events=%d\n", int(p_recorder->events.size()));
	check(p_recorder->events.size() == 1, "one diagonal pan reaches the real viewport exactly once");
	bool pan_payload = !p_recorder->events.is_empty();
	for (const Ref<InputEvent> &received : p_recorder->events) {
		Ref<InputEventPanGesture> pan = received;
		pan_payload = pan_payload && pan.is_valid() && pan->get_position() == Vector2(73, 51) &&
				pan->get_delta() == Vector2(0.125f, -0.75f) && modifiers(pan);
	}
	check(pan_payload, "pan retains both fractional components, physical position and all modifiers without wheel pulses");
	for (float factor : { 1.25f, 0.8f }) {
		p_recorder->events.clear();
		event.type = 4;
		event.factor = factor;
		dispatch(p_os, event);
		std::printf("Magnify factor=%.3f events=%d\n", factor, int(p_recorder->events.size()));
		check(p_recorder->events.size() == 1, "one magnify reaches the real viewport exactly once");
		bool magnify_payload = !p_recorder->events.is_empty();
		for (const Ref<InputEvent> &received : p_recorder->events) {
			Ref<InputEventMagnifyGesture> magnify = received;
			magnify_payload = magnify_payload && magnify.is_valid() && magnify->get_factor() == factor &&
					magnify->get_position() == Vector2(73, 51) && modifiers(magnify);
		}
		check(magnify_payload, "magnify retains its incremental factor, physical position and modifiers");
	}
	check(input->is_mouse_button_pressed(MouseButton::LEFT) && input->is_mouse_button_pressed(MouseButton::RIGHT), "pan and magnify preserve held physical buttons");
	// Release only resources and in-memory input state owned by this fixture.
	event = GodotMouseEvent{};
	event.type = 1;
	event.button = 1;
	event.mask = 2;
	event.x = 60;
	event.y = 45;
	event.factor = 1;
	dispatch(p_os, event);
	event.button = 2;
	event.mask = 0;
	dispatch(p_os, event);
	check(!input->is_mouse_button_pressed(MouseButton::LEFT) && !input->is_mouse_button_pressed(MouseButton::RIGHT), "fixture releases its held buttons");
	p_recorder->events.clear();
}
} // namespace

int main(int argc, char **argv) {
	OS_OpenHarmony::EXEC_PATH = argv[0];
	OS_OpenHarmony os(false);
	setlocale(LC_CTYPE, "");
	DisplayServer::register_create_function("pointer-fixture", PointerDisplay::create, PointerDisplay::drivers);
	// Keep the existing runner's --headless/dummy renderer, then select the
	// hybrid display so the production Input getter can observe each mode.
	std::vector<char *> arguments(argv + 1, argv + argc);
	arguments.push_back(const_cast<char *>("--display-driver"));
	arguments.push_back(const_cast<char *>("pointer-fixture"));
	if (Main::setup(argv[0], arguments.size(), arguments.data()) != OK) {
		return EXIT_FAILURE;
	}
	if (Main::start() != EXIT_SUCCESS) {
		Main::cleanup();
		return EXIT_FAILURE;
	}
	os.main_loop_begin();
	Window *root = SceneTree::get_singleton()->get_root();
	root->set_embedding_subwindows(true);
	root->set_size(Vector2i(640, 480));
	Input::get_singleton()->set_use_accumulated_input(false);
	Input::get_singleton()->set_emulate_touch_from_mouse(false);
	EventRecorder *recorder = memnew(EventRecorder);
	root->add_child(recorder);
	recorder->set_process_input(true);
	MotionButton *button = memnew(MotionButton);
	root->add_child(button);
	button->set_text("Root button");
	button->set_position(Vector2(20, 20));
	button->set_size(Vector2(140, 50));
	PopupMenu *popup = memnew(PopupMenu);
	root->add_child(popup);
	popup->add_item("First item", 1);
	popup->add_item("Second item", 2);
	popup->add_item("Third item", 3);
	frame(os);

	const bool expect_original_bug = std::getenv("GODOT_EXPECT_ORIGINAL_HOVER_BUG") != nullptr;
	if (!expect_original_bug) {
		pointer_modes(os, recorder);
		wheel_and_gestures(os, recorder);
	}
	for (int i = 0; i < 12; ++i) {
		move(os, 30 + i, 35);
	}
	check(button->is_hovered(), "root button hovers through real Input/Viewport");
	popup->popup(Rect2i(200, 150, 240, 140));
	frame(os);
	for (int i = 0; i < 12; ++i) {
		move(os, 220 + i, 172);
	}
	const bool popup_hover = popup->get_focused_item() >= 0;
	check(expect_original_bug ? !popup_hover : popup_hover, expect_original_bug ? "reproduced original embedded popup hover bug" : "embedded PopupMenu hovers through real motion");
	click(os, 225, 172);
	check(!popup->is_visible(), "embedded PopupMenu responds to mouse click");

	popup->popup(Rect2i(200, 150, 240, 140));
	frame(os);
	popup->set_focused_item(-1);
	for (int i = 0; i < 12; ++i) {
		move(os, 240 + i, 195, true, 0, 0);
	}
	const bool zero_raw_hover = popup->get_focused_item() >= 0;
	check(expect_original_bug ? !zero_raw_hover : zero_raw_hover, expect_original_bug ? "reproduced original zero-raw popup hover bug" : "visible zero-raw moves still hover embedded PopupMenu");

	popup->hide();
	root->remove_child(popup);
	memdelete(popup);
	root->remove_child(button);
	memdelete(button);
	recorder->events.clear();
	root->remove_child(recorder);
	memdelete(recorder);
	os.main_loop_end();
	Main::cleanup();
	if (failures == 0) {
		std::printf("PASS %s real Input/Viewport/embedded PopupMenu regression\n", expect_original_bug ? "reproduced original" : "fixed");
	}
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
