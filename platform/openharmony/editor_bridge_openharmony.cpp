// Godot Engine contributors. SPDX-License-Identifier: MIT
#include "editor_bridge_openharmony.h"

#include "crash_handler_openharmony.h"
#include "dir_access_openharmony.h"
#include "display_server_openharmony.h"
#include "file_access_openharmony.h"
#include "os_openharmony.h"

#include "main/main.h"

#include <native_vsync/native_vsync.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Event {
	enum Type { TOUCH,
		MOUSE,
		KEY,
		RESIZE,
		WINDOW } type;
	GodotTouchEvent touch{};
	GodotMouseEvent mouse{};
	GodotKeyEvent key{};
	int32_t first = 0;
	int32_t second = 0;
};
std::mutex event_mutex;
std::condition_variable frame_ready;
std::deque<Event> events;
std::thread engine_thread;
std::atomic<int> state{ 0 };
GodotCreateInstanceCallback create_instance_callback = nullptr;
bool stopping = false;
bool frame = false;

void enqueue(Event event) {
	std::lock_guard<std::mutex> lock(event_mutex);
	if (state.load() == 1 || state.load() == 2) {
		events.push_back(event);
	}
}

void on_vsync(long long timestamp, void *data) {
	std::lock_guard<std::mutex> lock(event_mutex);
	frame = true;
	frame_ready.notify_one();
}

void run(NativeResourceManager *resources, void *window, int32_t window_id,
		int32_t width, int32_t height, std::vector<std::string> arguments) {
	// ArkUI owns its UI thread. All engine initialization, input and teardown
	// happen on this single thread; VSync callbacks only wake it.
	OS_OpenHarmony os;
	OS_OpenHarmony::EXEC_PATH = "godot-editor";
	// A crash here kills the whole editor process, and the launcher that started
	// it stays alive: leave the backtrace where the application can read it.
	ohos_crash_handler_install((os.get_data_path() + "/godot-crash.log").utf8().get_data());
	FileAccessOpenHarmony::setup(resources);
	DirAccessOpenHarmony::setup(resources);
	os.set_native_window(static_cast<OHNativeWindow *>(window));
	os.set_window_id(window_id);
	os.set_display_size(Size2i(width, height));
	os.set_allowed_permissions("ohos.permission.INTERNET,ohos.permission.LOCK_WINDOW_CURSOR");
	std::vector<char *> argv;
	for (std::string &argument : arguments) {
		argv.push_back(argument.data());
	}
	Error error = Main::setup(OS_OpenHarmony::EXEC_PATH, argv.size(), argv.data());
	if (error != OK) {
		state = -int(error);
		return; // Main::setup unwinds its own failed initialization.
	}
	if (Main::start() != EXIT_SUCCESS || !os.get_main_loop()) {
		Main::cleanup();
		state = -int(FAILED);
		return;
	}
	OH_NativeVSync *vsync = OH_NativeVSync_Create_ForAssociatedWindow(window_id, "GodotEditor", 11);
	os.main_loop_begin();
	os.on_focus_in();
	state = 2;
	bool quit = false;
	while (!quit) {
		std::deque<Event> pending;
		{
			std::unique_lock<std::mutex> lock(event_mutex);
			if (stopping) {
				break;
			}
			frame = false;
			lock.unlock();
			int requested = vsync ? OH_NativeVSync_RequestFrame(vsync, on_vsync, nullptr) : -1;
			lock.lock();
			// Hidden windows may receive no VSync. Continue pumping lifecycle events.
			frame_ready.wait_for(lock, std::chrono::milliseconds(requested == 0 ? 100 : 16), [] { return frame || stopping; });
			if (stopping) {
				break;
			}
			pending.swap(events);
		}
		for (Event &event : pending) {
			switch (event.type) {
				case Event::TOUCH:
					godot_touch(&event.touch, 1);
					break;
				case Event::MOUSE:
					godot_mouse(&event.mouse);
					break;
				case Event::KEY:
					godot_key(&event.key);
					break;
				case Event::RESIZE:
					DisplayServerOpenHarmony::get_singleton()->resize_window(event.first, event.second);
					break;
				case Event::WINDOW:
					switch (event.first) {
						case 1:
						case 2:
							os.on_focus_in();
							break;
						case 3:
						case 4:
							os.on_focus_out();
							break;
						case 5:
							os.on_exit_background();
							break;
						case 6:
							os.on_enter_background();
							break;
						case 7:
							DisplayServerOpenHarmony::get_singleton()->send_window_event(DisplayServerEnums::WINDOW_EVENT_CLOSE_REQUEST);
							break;
					}
					break;
			}
		}
		quit = os.main_loop_iterate();
	}
	if (vsync) {
		OH_NativeVSync_Destroy(vsync);
	}
	os.main_loop_end();
	Main::cleanup();
	state = 3;
}
} // namespace

int godot_editor_start(NativeResourceManager *resources, void *window, int32_t window_id,
		int32_t width, int32_t height, int argc, const char *const *argv) {
	if (!resources || !window || width <= 0 || height <= 0 || argc < 0 || (argc && !argv)) {
		return -int(ERR_INVALID_PARAMETER);
	}
	int expected = 0;
	if (!state.compare_exchange_strong(expected, 1)) {
		return -int(ERR_ALREADY_IN_USE);
	}
	std::vector<std::string> arguments;
	for (int i = 0; i < argc; ++i) {
		arguments.emplace_back(argv[i]);
	}
	engine_thread = std::thread(run, resources, window, window_id, width, height, std::move(arguments));
	return 0;
}

void godot_editor_stop() {
	{
		std::lock_guard<std::mutex> lock(event_mutex);
		stopping = true;
		frame_ready.notify_one();
	}
	if (engine_thread.joinable()) {
		engine_thread.join();
	}
}
int godot_editor_state() {
	return state.load();
}
void godot_editor_set_create_instance_callback(GodotCreateInstanceCallback callback) {
	create_instance_callback = callback;
}
int32_t godot_editor_create_instance(int argc, const char *const *argv) {
	return create_instance_callback ? create_instance_callback(argc, argv) : -1;
}
void godot_editor_touch(const GodotTouchEvent *p_events, int count) {
	for (int i = 0; i < count; i++) {
		Event event{};
		event.type = Event::TOUCH;
		event.touch = p_events[i];
		enqueue(event);
	}
}
void godot_editor_mouse(const GodotMouseEvent *p_event) {
	Event event{};
	event.type = Event::MOUSE;
	event.mouse = *p_event;
	enqueue(event);
}
void godot_editor_key(const GodotKeyEvent *p_event) {
	Event event{};
	event.type = Event::KEY;
	event.key = *p_event;
	enqueue(event);
}
void godot_editor_resize(int32_t width, int32_t height) {
	if (width <= 0 || height <= 0) {
		return;
	}
	Event event{};
	event.type = Event::RESIZE;
	event.first = width;
	event.second = height;
	enqueue(event);
}
void godot_editor_window_event(int32_t p_event) {
	Event event{};
	event.type = Event::WINDOW;
	event.first = p_event;
	enqueue(event);
}
