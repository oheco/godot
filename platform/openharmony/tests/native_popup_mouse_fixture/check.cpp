// Godot Engine contributors. SPDX-License-Identifier: MIT
// Links the real engine's Input, Viewport, Button and embedded PopupMenu.
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

#include "platform/openharmony/bridge_openharmony.h"
#include "platform/openharmony/os_openharmony.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>

namespace {
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

void frame(OS_OpenHarmony &p_os) {
	p_os.delay_usec(20000);
	p_os.main_loop_iterate();
}

void move(OS_OpenHarmony &p_os, float p_x, float p_y, bool p_raw = false, float p_dx = 0, float p_dy = 0) {
	GodotMouseEvent event{};
	event.type = 2;
	event.x = p_x;
	event.y = p_y;
	event.has_relative = p_raw;
	event.relative_x = p_dx;
	event.relative_y = p_dy;
	godot_mouse(&event);
	frame(p_os);
}

void click(OS_OpenHarmony &p_os, float p_x, float p_y) {
	GodotMouseEvent event{};
	event.type = 0;
	event.button = 1;
	event.mask = 1;
	event.x = p_x;
	event.y = p_y;
	event.factor = 1;
	godot_mouse(&event);
	frame(p_os);
	event.type = 1;
	event.mask = 0;
	godot_mouse(&event);
	frame(p_os);
}
} // namespace

int main(int argc, char **argv) {
	OS_OpenHarmony::EXEC_PATH = argv[0];
	OS_OpenHarmony os(false);
	setlocale(LC_CTYPE, "");
	if (Main::setup(argv[0], argc - 1, argv + 1) != OK) {
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

	for (int i = 0; i < 12; ++i) {
		move(os, 30 + i, 35);
	}
	const bool root_hover = button->is_hovered();
	// Captured pointers can report movement while absolute coordinates stay
	// fixed. Preserve that genuine raw motion, and keep stationary events zero.
	move(os, 41, 35, true, 5, -3);
	const bool captured_raw = button->last_relative == Vector2(5, -3);
	move(os, 41, 35, true, 0, 0);
	const bool stationary = button->last_relative == Vector2();
	std::printf("Captured raw motion=%d; stationary motion=%d\n", captured_raw, stationary);
	popup->popup(Rect2i(200, 150, 240, 140));
	frame(os);
	for (int i = 0; i < 12; ++i) {
		move(os, 220 + i, 172);
	}
	const bool popup_hover = popup->get_focused_item() >= 0;
	std::printf("Root button hover=%d; embedded PopupMenu hover=%d\n", root_hover, popup_hover);
	click(os, 225, 172);
	const bool popup_click = !popup->is_visible();

	popup->popup(Rect2i(200, 150, 240, 140));
	frame(os);
	popup->set_focused_item(-1);
	// Noncaptured ArkUI pointer moves can contain explicitly available zero raw
	// deltas. Absolute movement must still update relative motion/velocity.
	for (int i = 0; i < 12; ++i) {
		move(os, 240 + i, 195, true, 0, 0);
	}
	const bool zero_raw_hover = popup->get_focused_item() >= 0;
	std::printf("Popup click=%d; zero-raw-delta Popup hover=%d\n", popup_click, zero_raw_hover);

	const bool expect_original_bug = std::getenv("GODOT_EXPECT_ORIGINAL_HOVER_BUG") != nullptr;
	const bool passed = root_hover && popup_click && captured_raw && stationary &&
			(expect_original_bug ? !popup_hover && !zero_raw_hover : popup_hover && zero_raw_hover);
	popup->hide();
	root->remove_child(popup);
	memdelete(popup);
	root->remove_child(button);
	memdelete(button);
	os.main_loop_end();
	Main::cleanup();
	if (passed) {
		std::printf("PASS %s real Input/Viewport/embedded PopupMenu regression\n", expect_original_bug ? "reproduced original" : "fixed");
	}
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
