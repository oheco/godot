// Godot Engine contributors. SPDX-License-Identifier: MIT
// Real Input/Viewport/RichTextLabel/ScrollBar, with a touchscreen-capable
// headless display. This reproduces the hybrid mouse/touch editor Output panel.
// clang-format off
#include <native_window/external_window.h>
// clang-format on
#include "core/input/input.h"
#include "main/main.h"
#include "platform/openharmony/bridge_openharmony.h"
#include "platform/openharmony/os_openharmony.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/scroll_bar.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/display/display_server_headless.h"
#include "servers/rendering/dummy/rasterizer_dummy.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>

namespace {
class HybridDisplay : public DisplayServerHeadless {
public:
	bool is_touchscreen_available() const override { return true; }
	static Vector<String> drivers() {
		Vector<String> result;
		result.push_back("dummy");
		return result;
	}
	static DisplayServer *create(const String &, DisplayServerEnums::WindowMode, DisplayServerEnums::VSyncMode, uint32_t, const Vector2i *, const Vector2i &, int, DisplayServerEnums::Context, int64_t, Error &r_error) {
		r_error = OK;
		RasterizerDummy::make_current();
		return memnew(HybridDisplay);
	}
};
int failures = 0;
void check(bool p_ok, const char *p_message) {
	std::printf("%s %s\n", p_ok ? "PASS" : "FAIL", p_message);
	if (!p_ok) {
		failures++;
	}
}
void frame(OS_OpenHarmony &p_os, int p_count = 1) {
	for (int i = 0; i < p_count; i++) {
		p_os.delay_usec(20000);
		p_os.main_loop_iterate();
	}
}
void mouse(OS_OpenHarmony &p_os, int p_type, const Vector2 &p_pos, const Vector2 &p_relative = Vector2()) {
	GodotMouseEvent event{};
	event.type = p_type;
	event.button = 1;
	event.mask = p_type == 1 ? 0 : 1;
	event.x = p_pos.x;
	event.y = p_pos.y;
	event.factor = 1;
	event.has_relative = true;
	event.relative_x = p_relative.x;
	event.relative_y = p_relative.y;
	godot_mouse(&event);
	frame(p_os);
}
void touch(OS_OpenHarmony &p_os, int p_type, const Vector2 &p_pos) {
	GodotTouchEvent event{};
	event.type = p_type;
	event.id = 0;
	event.x = p_pos.x;
	event.y = p_pos.y;
	godot_touch(&event, 1);
	frame(p_os);
}
} // namespace

int main(int argc, char **argv) {
	OS_OpenHarmony::EXEC_PATH = argv[0];
	OS_OpenHarmony os(false);
	setlocale(LC_CTYPE, "");
	DisplayServer::register_create_function("hybrid-fixture", HybridDisplay::create, HybridDisplay::drivers);
	if (Main::setup(argv[0], argc - 1, argv + 1) != OK) {
		return EXIT_FAILURE;
	}
	if (Main::start() != EXIT_SUCCESS) {
		Main::cleanup();
		return EXIT_FAILURE;
	}
	os.main_loop_begin();
	Window *root = SceneTree::get_singleton()->get_root();
	root->set_size(Vector2i(800, 600));
	Input::get_singleton()->set_use_accumulated_input(false);
	Input::get_singleton()->set_emulate_mouse_from_touch(true);
	RichTextLabel *label = memnew(RichTextLabel);
	root->add_child(label);
	label->set_position(Vector2(60, 60));
	label->set_size(Vector2(400, 240));
	label->set_selection_enabled(true);
	label->set_scroll_follow(true); // Same as EditorLog.
	String content;
	for (int i = 0; i < 160; i++) {
		content += vformat("Output %03d with selectable text\n", i);
	}
	label->set_text(content);
	frame(os, 5);
	VScrollBar *bar = label->get_v_scroll_bar();
	check(bar->get_max() > bar->get_page() * 5, "log fixture overflows viewport on touchscreen-capable display");
	const bool original = std::getenv("GODOT_EXPECT_LOG_DRAG_BUG") != nullptr;
	for (int direction : { -1, 1 }) {
		label->deselect();
		bar->set_value(1200);
		frame(os, 2);
		Vector2 point(200, 180);
		mouse(os, 0, point);
		// Drag inside first: physical mouse must select, not pan content.
		point.y += direction * 40;
		mouse(os, 2, point, Vector2(0, direction * 40));
		const double inside_scroll = bar->get_value();
		check(original ? inside_scroll != 1200 : inside_scroll == 1200,
				original ? "reproduced physical mouse incorrectly panning log" : "physical mouse selection leaves in-bounds scroll unchanged");
		point.y = direction < 0 ? 30 : 330;
		mouse(os, 2, point, Vector2(0, direction * 150));
		bool monotonic = true;
		const double start = bar->get_value();
		double previous = start;
		int reversals = 0;
		for (int i = 0; i < 15; i++) {
			// Interleave real motion with selection-timer ticks, including zero raw
			// delta events. The old panning path restores its stale drag origin.
			frame(os, 3);
			const double before_motion = bar->get_value();
			mouse(os, 2, point, Vector2());
			const double after_motion = bar->get_value();
			if ((after_motion - before_motion) * direction < 0) {
				reversals++;
			}
			if ((after_motion - previous) * direction < 0) {
				monotonic = false;
			}
			previous = after_motion;
		}
		check(original ? reversals > 0 : reversals == 0 && monotonic,
				original ? "reproduced selection timer / touch panning oscillation" : "out-of-bounds selection scrolls without reversing");
		check(original || (bar->get_value() - start) * direction > 100,
				"selection continues beyond initially visible log lines");
		const int selection_extent = label->get_selection_to() - label->get_selection_from();
		frame(os, 12);
		std::printf("stationary selection extent %d -> %d; selected=%d; scroll=%.0f\n", selection_extent,
				label->get_selection_to() - label->get_selection_from(), label->get_selected_text().length(), bar->get_value());
		check(original || label->get_selection_to() - label->get_selection_from() > selection_extent,
				"stationary pointer continues extending selection outside viewport");
		mouse(os, 1, point);
		const double released_scroll = bar->get_value();
		const String released_text = label->get_selected_text();
		frame(os, 15);
		check(original || (bar->get_value() == released_scroll && label->get_selected_text() == released_text),
				"release stops scrolling and preserves selected text");
		std::printf("direction=%d start=%.0f end=%.0f reverse_count=%d\n", direction, start, released_scroll, reversals);
	}
	// Finger scrolling must still use the emulated mouse route.
	label->set_selection_enabled(false);
	bar->set_value(1200);
	frame(os, 2);
	touch(os, 0, Vector2(200, 180));
	touch(os, 2, Vector2(200, 140));
	check(bar->get_value() == 1240, "native finger drag retains touch scrolling");
	touch(os, 1, Vector2(200, 140));
	root->remove_child(label);
	memdelete(label);
	os.main_loop_end();
	Main::cleanup();
	std::printf("LOG_SELECTION_%s failures=%d\n", failures ? "FAIL" : "PASS", failures);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
