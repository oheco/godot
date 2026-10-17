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
#include "scene/resources/style_box.h"
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
void mouse(OS_OpenHarmony &p_os, int p_type, const Vector2 &p_pos, const Vector2 &p_relative = Vector2(), bool p_double_click = false) {
	GodotMouseEvent event{};
	event.type = p_type;
	event.button = 1;
	event.mask = p_type == 1 ? 0 : 1;
	event.x = p_pos.x;
	event.y = p_pos.y;
	event.double_click = p_double_click;
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
Vector2i selected_range(RichTextLabel *p_label) {
	return Vector2i(p_label->get_selection_from(), p_label->get_selection_to());
}
Vector2i drag_selection(OS_OpenHarmony &p_os, RichTextLabel *p_label, const Vector2 &p_anchor, const Vector2 &p_target, int p_mode = 0) {
	p_label->deselect();
	mouse(p_os, 0, p_anchor, Vector2(), p_mode > 0);
	if (p_mode == 2) {
		mouse(p_os, 1, p_anchor);
		mouse(p_os, 0, p_anchor); // Third click selects a paragraph.
	}
	mouse(p_os, 2, p_target);
	frame(p_os, 4);
	const Vector2i result = selected_range(p_label);
	mouse(p_os, 1, p_target);
	return result;
}
void selection_boundaries(OS_OpenHarmony &p_os, RichTextLabel *p_label, bool p_wrapped) {
	p_label->set_selection_enabled(true);
	p_label->set_scroll_follow(false);
	p_label->set_drag_and_drop_selection_enabled(false);
	p_label->set_deselect_on_focus_loss_enabled(false);
	Ref<StyleBoxEmpty> style;
	style.instantiate();
	style->set_content_margin(SIDE_LEFT, 16);
	style->set_content_margin(SIDE_TOP, 24);
	style->set_content_margin(SIDE_RIGHT, 16);
	style->set_content_margin(SIDE_BOTTOM, 24);
	p_label->add_theme_style_override("normal", style);
	p_label->add_theme_constant_override("line_separation", 10);
	p_label->add_theme_constant_override("paragraph_separation", 14);
	p_label->set_autowrap_mode(p_wrapped ? TextServer::AUTOWRAP_WORD_SMART : TextServer::AUTOWRAP_OFF);
	String content;
	for (int i = 0; i < 20; i++) {
		if (i) {
			content += "\n";
		}
		content += p_wrapped ? "First selectable words stretch across several visual rows; last selectable words remain responsive." : "Output selectable words across the row";
	}
	p_label->set_text(content);
	frame(p_os, 5);
	VScrollBar *bar = p_label->get_v_scroll_bar();
	const Vector2 origin = p_label->get_global_position();
	const real_t height = p_label->get_theme_font("normal_font")->get_height(p_label->get_theme_font_size("normal_font_size"));
	for (int direction : { -1, 1 }) {
		bar->set_value(direction < 0 ? 0 : bar->get_max());
		frame(p_os, 2);
		const double scroll = bar->get_value();
		const int edge_line = direction < 0 ? 0 : p_label->get_line_count() - 1;
		const real_t row_offset = p_wrapped ? p_label->get_line_offset(edge_line) : p_label->get_paragraph_offset(edge_line);
		const real_t row_y = 24 + row_offset - scroll + height * 0.5;
		const int anchor_line = edge_line - direction;
		const real_t anchor_offset = p_wrapped ? p_label->get_line_offset(anchor_line) : p_label->get_paragraph_offset(anchor_line);
		const Vector2 anchor = origin + Vector2(140, 24 + anchor_offset - scroll + height * 0.5);
		Vector2i expected[2];
		for (int x = 0; x < 2; x++) {
			p_label->deselect();
			mouse(p_os, 0, anchor);
			mouse(p_os, 2, origin + Vector2(x ? 230 : 80, row_y));
			frame(p_os, 4);
			expected[x] = selected_range(p_label);
			mouse(p_os, 1, origin + Vector2(x ? 230 : 80, row_y));
		}
		check(expected[0] != expected[1], "boundary reference positions select distinct columns on the same visual row");
		for (real_t y : { direction < 0 ? -40.0f : p_label->get_size().y + 40.0f,
					 direction < 0 ? -120.0f : p_label->get_size().y + 400.0f,
					 direction < 0 ? 8.0f : p_label->get_size().y - 8.0f }) {
			p_label->deselect();
			mouse(p_os, 0, anchor);
			for (int x = 0; x < 2; x++) {
				const Vector2 target = origin + Vector2(x ? 230 : 80, y);
				mouse(p_os, 2, target);
				frame(p_os, 4);
				std::printf("boundary wrapped=%d direction=%d y=%.0f x=%.0f got=%d:%d expected=%d:%d\n", p_wrapped, direction, y, target.x, selected_range(p_label).x, selected_range(p_label).y, expected[x].x, expected[x].y);
				check(selected_range(p_label) == expected[x], "held horizontal drag outside/padding preserves nearest visual row and column");
				check(bar->get_value() == scroll, "boundary drag keeps scroll at its minimum/maximum");
			}
			mouse(p_os, 1, origin + Vector2(230, y));
		}
		// Simulate a host updating the live pointer without routing motion to the
		// label (for example outside the window). Only the timer can observe it.
		p_label->deselect();
		mouse(p_os, 0, anchor);
		mouse(p_os, 2, origin + Vector2(80, row_y));
		frame(p_os, 4);
		Input::get_singleton()->set_mouse_position(origin + Vector2(230, row_y));
		frame(p_os, 5);
		check(selected_range(p_label) == expected[1], "selection timer polls live pointer without label motion");
		mouse(p_os, 1, origin + Vector2(230, row_y));
		const Vector2i released = selected_range(p_label);
		Input::get_singleton()->set_mouse_position(origin + Vector2(80, direction < 0 ? -40 : p_label->get_size().y + 40));
		frame(p_os, 5);
		check(selected_range(p_label) == released && bar->get_value() == scroll, "release stops live-pointer timer and preserves selection");
		mouse(p_os, 1, anchor);
	}
	bar->set_step(0);
	bar->set_value(0);
	frame(p_os, 2);
	const real_t next_offset = p_wrapped ? p_label->get_line_offset(1) : p_label->get_paragraph_offset(1);
	Vector2i unclipped[2];
	for (int x = 0; x < 2; x++) {
		unclipped[x] = drag_selection(p_os, p_label, origin + Vector2(140, 24 + next_offset + height * 0.5), origin + Vector2(x ? 230 : 80, 24 + height * 0.5));
	}
	bar->set_value(height - 0.25);
	frame(p_os, 2);
	const double partial_scroll = bar->get_value();
	for (int x = 0; x < 2; x++) {
		check(drag_selection(p_os, p_label, origin + Vector2(140, 24 + next_offset - partial_scroll + height * 0.5), origin + Vector2(x ? 230 : 80, 8)) == unclipped[x],
				"fractional clipped top row preserves horizontal endpoints even without an interior visible integer pixel");
	}
	bar->set_step(1);
	bar->set_value(0);
	frame(p_os, 2);
	const real_t second_top = 24 + (p_wrapped ? p_label->get_line_offset(1) : p_label->get_paragraph_offset(1));
	const Vector2 anchor = origin + Vector2(140, 24 + (p_wrapped ? p_label->get_line_offset(2) : p_label->get_paragraph_offset(2)) + height * 0.5);
	for (real_t x : { 80.0f, 230.0f }) {
		const Vector2i above = drag_selection(p_os, p_label, anchor, origin + Vector2(x, 24 + height * 0.5));
		const Vector2i below = drag_selection(p_os, p_label, anchor, origin + Vector2(x, second_top + height * 0.5));
		check(above != below, "gap references identify neighboring visual rows");
		check(drag_selection(p_os, p_label, anchor, origin + Vector2(x, 24 + height + 2)) == above,
				"upper part of line/paragraph gap uses nearest preceding row");
		check(drag_selection(p_os, p_label, anchor, origin + Vector2(x, second_top - 2)) == below,
				"lower part of line/paragraph gap uses nearest following row");
	}
	check(bar->get_value() == 0, "in-bounds gap selection does not scroll");
}
void selection_blank_and_focus(OS_OpenHarmony &p_os, RichTextLabel *p_label) {
	p_label->set_autowrap_mode(TextServer::AUTOWRAP_OFF);
	p_label->set_text("First selectable words\n\nLast selectable words");
	frame(p_os, 5);
	const Vector2 origin = p_label->get_global_position();
	const real_t height = p_label->get_theme_font("normal_font")->get_height(p_label->get_theme_font_size("normal_font_size"));
	const Vector2 anchor = origin + Vector2(100, 24 + height * 0.5);
	const real_t last_y = 24 + p_label->get_paragraph_offset(2) + height * 0.5;
	const real_t empty_y = 24 + p_label->get_paragraph_offset(1) + height * 0.5;
	for (real_t x : { 80.0f, 180.0f }) {
		const Vector2i expected = drag_selection(p_os, p_label, anchor, origin + Vector2(x, last_y));
		check(expected.x >= 0 && drag_selection(p_os, p_label, anchor, origin + Vector2(x, 220)) == expected,
				"blank space below short text keeps last row's horizontal hit position");
	}
	for (real_t x : { -120.0f, 900.0f }) {
		const Vector2i expected = drag_selection(p_os, p_label, anchor, origin + Vector2(x < 0 ? 0 : 399, last_y));
		check(drag_selection(p_os, p_label, anchor, origin + Vector2(x, last_y)) == expected,
				"drag beyond left/right window edge keeps shaped line endpoint semantics");
	}
	const Vector2i empty = drag_selection(p_os, p_label, anchor, origin + Vector2(80, empty_y));
	check(empty.x >= 0 && drag_selection(p_os, p_label, anchor, origin + Vector2(230, empty_y)) == empty,
			"empty paragraph remains a valid selection endpoint");
	for (int mode : { 1, 2 }) {
		const Vector2 mode_anchor = origin + Vector2(100, last_y);
		const Vector2i expected = drag_selection(p_os, p_label, mode_anchor, origin + Vector2(80, 24 + height * 0.5), mode);
		check(expected.x >= 0 && drag_selection(p_os, p_label, mode_anchor, origin + Vector2(80, -40), mode) == expected,
				"word/paragraph drag retains expansion at nearest boundary row");
	}
	// Reset click mode after the intentional double/triple clicks.
	frame(p_os, 31);
	for (int notification : { int(Control::NOTIFICATION_WM_WINDOW_FOCUS_OUT), int(Control::NOTIFICATION_FOCUS_EXIT) }) {
		p_label->deselect();
		mouse(p_os, 0, anchor);
		mouse(p_os, 2, origin + Vector2(180, last_y));
		frame(p_os, 4);
		const Vector2i before = selected_range(p_label);
		p_label->notification(notification);
		Input::get_singleton()->set_mouse_position(origin + Vector2(80, -40));
		frame(p_os, 6);
		check(before.x >= 0 && selected_range(p_label) == before && p_label->get_v_scroll_bar()->get_value() == 0,
				"focus loss stops held selection without clearing selected text");
		mouse(p_os, 1, anchor);
	}
}
void selection_rtl_and_table(OS_OpenHarmony &p_os, RichTextLabel *p_label) {
	const Vector2 origin = p_label->get_global_position();
	const real_t height = p_label->get_theme_font("normal_font")->get_height(p_label->get_theme_font_size("normal_font_size"));
	p_label->set_text_direction(Control::TEXT_DIRECTION_RTL);
	p_label->set_text(String::utf8("בחירת טקסט עם מילים רבות לבדיקת גרירה\nבחירת שורה נוספת עם מילים רבות"));
	frame(p_os, 5);
	const Vector2 rtl_anchor = origin + Vector2(180, 24 + p_label->get_paragraph_offset(1) + height * 0.5);
	Vector2i rtl_ranges[2];
	for (int i = 0; i < 2; i++) {
		const real_t x = i ? 230 : 80;
		rtl_ranges[i] = drag_selection(p_os, p_label, rtl_anchor, origin + Vector2(x, 24 + height * 0.5));
		check(rtl_ranges[i].x >= 0 && drag_selection(p_os, p_label, rtl_anchor, origin + Vector2(x, -40)) == rtl_ranges[i],
				"RTL boundary drag uses the same shaped horizontal position as an in-row drag");
	}
	check(rtl_ranges[0] != rtl_ranges[1], "RTL boundary references identify distinct horizontal positions");
	p_label->set_text_direction(Control::TEXT_DIRECTION_AUTO);
	p_label->set_use_bbcode(true);
	p_label->set_text("[table=1][cell]First selectable table cell words\nLast selectable table cell words[/cell][/table]");
	frame(p_os, 5);
	const real_t first_y = 24 + Math::floor(p_label->get_theme_constant("table_v_separation") * 0.5) + height * 0.5;
	const real_t last_y = first_y + height + 10 + 14;
	for (int direction : { -1, 1 }) {
		const Vector2 anchor = origin + Vector2(140, direction < 0 ? last_y : first_y);
		Vector2i ranges[2];
		for (int i = 0; i < 2; i++) {
			const real_t x = i ? 230 : 80;
			ranges[i] = drag_selection(p_os, p_label, anchor, origin + Vector2(x, direction < 0 ? first_y : last_y));
			check(ranges[i].x >= 0 && drag_selection(p_os, p_label, anchor, origin + Vector2(x, direction < 0 ? -40 : 220)) == ranges[i],
					"table cell boundary/padding drag retains cell paragraph and horizontal position");
		}
		check(ranges[0] != ranges[1], "table cell references identify distinct horizontal positions");
	}
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
	if (!original) {
		// A pan has one event, unlike a wheel's press/release pair. Check the
		// page-based gain after core gesture dispatch is made exactly once.
		bar->set_step(0);
		bar->set_value(1200);
		const double pan_start = bar->get_value();
		GodotMouseEvent pan{};
		pan.type = 3;
		pan.x = label->get_global_position().x + 100;
		pan.y = label->get_global_position().y + 100;
		pan.has_relative = true;
		pan.relative_x = 0.125;
		pan.relative_y = 0.25;
		godot_mouse(&pan);
		frame(os, 2);
		check(Math::is_equal_approx(bar->get_value() - pan_start, bar->get_page() * 0.25 / 8),
				"one precise pan uses the same page-based gain as other scroll containers");
		bar->set_step(1);
	}
	// Finger scrolling must still use the emulated mouse route.
	label->set_selection_enabled(false);
	bar->set_value(1200);
	frame(os, 2);
	touch(os, 0, Vector2(200, 180));
	touch(os, 2, Vector2(200, 140));
	check(bar->get_value() == 1240, "native finger drag retains touch scrolling");
	touch(os, 1, Vector2(200, 140));
	if (!original) {
		selection_boundaries(os, label, false);
		selection_boundaries(os, label, true);
		selection_blank_and_focus(os, label);
		selection_rtl_and_table(os, label);
	}
	root->remove_child(label);
	memdelete(label);
	os.main_loop_end();
	Main::cleanup();
	std::printf("LOG_SELECTION_%s failures=%d\n", failures ? "FAIL" : "PASS", failures);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
