// Godot Engine contributors. SPDX-License-Identifier: MIT
// Captures actual LineEdit/TextEdit submissions without attaching a system IME.
// clang-format off
#include <native_window/external_window.h>
// clang-format on
#include "main/main.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/text_edit.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/display/display_server_headless.h"
#include "servers/rendering/dummy/rasterizer_dummy.h"

#include "platform/openharmony/os_openharmony.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>

namespace {
class ImeCaptureDisplay : public DisplayServerHeadless {
public:
	Point2i position;
	bool active = false;
	int submissions = 0;
	Size2i size = Size2i(1600, 1200);
	bool has_feature(DisplayServerEnums::Feature feature) const override {
		return feature == DisplayServerEnums::FEATURE_IME;
	}
	void window_set_ime_active(bool enabled, DisplayServerEnums::WindowID) override { active = enabled; }
	void window_set_ime_position(const Point2i &point, DisplayServerEnums::WindowID) override {
		position = point;
		++submissions;
	}
	void show_emoji_and_symbol_picker() const override {}
	Point2i window_get_position(DisplayServerEnums::WindowID) const override { return Point2i(511, 363); }
	void window_set_size(const Size2i value, DisplayServerEnums::WindowID) override { size = value; }
	Size2i window_get_size(DisplayServerEnums::WindowID) const override { return size; }
	DisplayServerEnums::WindowMode window_get_mode(DisplayServerEnums::WindowID) const override { return DisplayServerEnums::WINDOW_MODE_WINDOWED; }
	bool can_any_window_draw() const override { return true; }
	bool window_can_draw(DisplayServerEnums::WindowID) const override { return true; }
	int get_screen_count() const override { return 1; }
	Size2i screen_get_size(int) const override { return Size2i(4000, 3000); }
	Rect2i screen_get_usable_rect(int) const override { return Rect2i(0, 0, 4000, 3000); }
	float screen_get_scale(int) const override { return 1.9f; }
	static Vector<String> drivers() {
		Vector<String> values;
		values.push_back("dummy");
		return values;
	}
	static DisplayServer *create(const String &, DisplayServerEnums::WindowMode, DisplayServerEnums::VSyncMode,
			uint32_t, const Point2i *, const Size2i &, int, DisplayServerEnums::Context, int64_t, Error &error) {
		error = OK;
		RasterizerDummy::make_current();
		return memnew(ImeCaptureDisplay);
	}
};
void frame(OS_OpenHarmony &os) {
	os.delay_usec(20000);
	os.main_loop_iterate();
}
struct Probe {
	LineEdit *line;
	TextEdit *text;
};
Probe controls(Window *window) {
	Probe probe;
	probe.line = memnew(LineEdit);
	window->add_child(probe.line);
	probe.line->set_position(Vector2(37, 51));
	probe.line->set_size(Vector2(240, 44));
	probe.line->set_text("Caret test");
	probe.line->set_caret_column(3);
	probe.line->set_virtual_keyboard_enabled(false);
	probe.text = memnew(TextEdit);
	window->add_child(probe.text);
	probe.text->set_position(Vector2(43, 125));
	probe.text->set_size(Vector2(250, 100));
	probe.text->set_text("Caret test\nsecond line");
	probe.text->set_caret_column(3);
	probe.text->set_virtual_keyboard_enabled(false);
	return probe;
}
Point2 submit(LineEdit *line, ImeCaptureDisplay *display) {
	display->active = false;
	const int before = display->submissions;
	line->show_emoji_and_symbol_picker();
	if (!display->active || display->submissions == before) {
		std::fprintf(stderr, "LineEdit did not submit an IME anchor\n");
		std::exit(EXIT_FAILURE);
	}
	return display->position;
}
Point2 submit(TextEdit *text, ImeCaptureDisplay *display) {
	display->active = false;
	const int before = display->submissions;
	text->show_emoji_and_symbol_picker();
	if (!display->active || display->submissions == before) {
		std::fprintf(stderr, "TextEdit did not submit an IME anchor\n");
		std::exit(EXIT_FAILURE);
	}
	return display->position;
}
// Independent rendering chain: each embedded texture is drawn at its window's
// position in the parent canvas, with that viewport's final transform. Native
// screen origin is added once outside this chain, just as in the host adapter.
Transform2D render_transform(Window *window) {
	Transform2D result = window->get_final_transform();
	if (window->get_embedder()) {
		Transform2D placement;
		placement.set_origin(window->get_position());
		Window *parent = Object::cast_to<Window>(window->get_embedder());
		if (!parent) {
			std::fprintf(stderr, "Fixture requires Window embedders\n");
			std::exit(EXIT_FAILURE);
		}
		result = render_transform(parent) * placement * result;
	}
	return result;
}
bool check(const char *name, Window *window, Probe probe, const Point2 &line_caret,
		ImeCaptureDisplay *display, bool expect_bug, bool root = false) {
	const Point2 actual_line = submit(probe.line, display);
	const Point2 actual_text = submit(probe.text, display);
	const Point2 expected_line = render_transform(window).xform(probe.line->get_global_transform_with_canvas().xform(line_caret));
	const Point2 expected_text = render_transform(window).xform(probe.text->get_global_transform_with_canvas().xform(probe.text->get_caret_draw_pos()));
	// DisplayServer accepts integer coordinates; subpixel rounding is permitted.
	const bool line_ok = actual_line.distance_to(expected_line) < 1.5;
	const bool text_ok = actual_text.distance_to(expected_text) < 1.5;
	std::printf("%s LineEdit actual=(%.2f,%.2f) rendered=(%.2f,%.2f) delta=(%.2f,%.2f) %s\n", name,
			actual_line.x, actual_line.y, expected_line.x, expected_line.y,
			actual_line.x - expected_line.x, actual_line.y - expected_line.y, line_ok ? "OK" : "OFFSET");
	std::printf("%s TextEdit actual=(%.2f,%.2f) rendered=(%.2f,%.2f) delta=(%.2f,%.2f) %s\n", name,
			actual_text.x, actual_text.y, expected_text.x, expected_text.y,
			actual_text.x - expected_text.x, actual_text.y - expected_text.y, text_ok ? "OK" : "OFFSET");
	return root || !expect_bug ? line_ok && text_ok : !line_ok && !text_ok;
}
Window *dialog(Window *parent, const Point2i &position, bool embed_children) {
	Window *window = memnew(Window);
	window->set_title("Decorated IME dialog");
	window->set_flag(Window::FLAG_BORDERLESS, false);
	window->set_visible(false);
	window->set_initial_position(Window::WINDOW_INITIAL_POSITION_ABSOLUTE);
	window->set_size(Size2i(500, 420));
	window->set_position(position);
	parent->add_child(window);
	window->show();
	// Window's enter-tree handler initializes this from force_native. Set it
	// afterwards so the fixture really has two embedded rendering levels.
	window->set_embedding_subwindows(embed_children);
	return window;
}
} // namespace
int main(int argc, char **argv) {
	OS_OpenHarmony::EXEC_PATH = argv[0];
	OS_OpenHarmony os(false);
	setlocale(LC_CTYPE, "");
	DisplayServer::register_create_function("ime-capture", ImeCaptureDisplay::create, ImeCaptureDisplay::drivers);
	if (Main::setup(argv[0], argc - 1, argv + 1) != OK) {
		return EXIT_FAILURE;
	}
	if (Main::start() != EXIT_SUCCESS) {
		Main::cleanup();
		return EXIT_FAILURE;
	}
	os.main_loop_begin();
	auto *display = static_cast<ImeCaptureDisplay *>(DisplayServer::get_singleton());
	Window *root = SceneTree::get_singleton()->get_root();
	root->set_embedding_subwindows(true);
	root->set_size(Size2i(1600, 1200));
	Probe root_probe = controls(root);
	frame(os);
	const Point2 root_line = submit(root_probe.line, display);
	const Point2 line_caret = root_probe.line->get_global_transform_with_canvas().affine_inverse().xform(render_transform(root).affine_inverse().xform(root_line));
	const bool expect_bug = std::getenv("GODOT_EXPECT_ORIGINAL_IME_BUG") != nullptr;
	bool passed = check("root/display-1.9", root, root_probe, line_caret, display, false, true);
	Window *popup = dialog(root, Point2i(170, 130), false);
	Probe popup_probe = controls(popup);
	frame(os);
	passed &= check("decorated popup", popup, popup_probe, line_caret, display, expect_bug);
	popup->hide();
	Window *outer = dialog(root, Point2i(145, 97), true);
	Window *nested = dialog(outer, Point2i(72, 65), false);
	Probe nested_probe = controls(nested);
	frame(os);
	const bool valid_chain = popup->get_embedder() == root && outer->get_embedder() == root && nested->get_embedder() == outer;
	std::printf("Native origin=(%d,%d), outer=(%d,%d), nested=(%d,%d); parent chain=%d, nested/root=%d, outer embedding=%d\n", root->get_position().x, root->get_position().y, outer->get_position().x, outer->get_position().y, nested->get_position().x, nested->get_position().y, valid_chain, nested->get_embedder() == root, outer->is_embedding_subwindows());
	passed &= valid_chain && root->get_position() == Point2i(511, 363);
	passed &= check("nested decorated popup", nested, nested_probe, line_caret, display, expect_bug);
	nested->hide();
	outer->hide();
	// Physical viewport 1600x1200, logical canvas 800x600: genuine 2x stretch.
	root->set_content_scale_size(Size2i(800, 600));
	root->set_content_scale_mode(Window::CONTENT_SCALE_MODE_CANVAS_ITEMS);
	frame(os);
	passed &= check("root/2x stretch", root, root_probe, line_caret, display, false, true);
	popup->show();
	frame(os);
	passed &= check("popup/2x stretch", popup, popup_probe, line_caret, display, expect_bug);
	popup->hide();
	outer->show();
	nested->show();
	frame(os);
	passed &= check("nested popup/2x stretch", nested, nested_probe, line_caret, display, expect_bug);
	nested->hide();
	outer->hide();
	root->remove_child(popup);
	memdelete(popup);
	root->remove_child(outer);
	memdelete(outer);
	root->remove_child(root_probe.line);
	memdelete(root_probe.line);
	root->remove_child(root_probe.text);
	memdelete(root_probe.text);
	os.main_loop_end();
	Main::cleanup();
	if (passed) {
		std::printf("PASS %s real control IME mapping regression\n", expect_bug ? "reproduced original" : "fixed");
	}
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
