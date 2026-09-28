// Godot Engine contributors. SPDX-License-Identifier: MIT
#include "engine_host_openharmony.h"

#include "crash_handler_openharmony.h"
#include "dir_access_openharmony.h"
#include "display_server_openharmony.h"
#include "engine_arguments_openharmony.h"
#include "file_access_openharmony.h"
#include "os_openharmony.h"

#include "core/config/engine.h"
#include "main/main.h"

#include <native_vsync/native_vsync.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <string>
#include <utility>
#include <unistd.h>
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
// Serializes start/stop/join independently of the engine's event queue.
std::mutex lifetime_mutex;
std::mutex event_mutex;
std::condition_variable frame_ready;
std::deque<Event> events;
// The engine thread is created explicitly instead of through std::thread: musl
// gives a new thread about a megabyte of stack, and the .NET runtime needs more
// than that while it starts (it overran the stack inside libcoreclr otherwise).
pthread_t engine_thread = {};
bool engine_thread_started = false;
std::atomic<int> state{ 0 };
std::atomic<GodotCreateInstanceCallback> create_instance_callback{ nullptr };
bool stopping = false;
bool frame = false;
bool frame_requested = false;
constexpr size_t ENGINE_THREAD_STACK_SIZE = 32 * 1024 * 1024;

void enqueue(Event event) {
	std::lock_guard<std::mutex> lock(event_mutex);
	if (!stopping && (state.load() == 1 || state.load() == 2)) {
		events.push_back(event);
	}
}

void on_vsync(long long timestamp, void *data) {
	std::lock_guard<std::mutex> lock(event_mutex);
	frame_requested = false;
	if (!stopping) {
		frame = true;
		frame_ready.notify_one();
	}
}

Error packaged_arguments(NativeResourceManager *resources, std::vector<std::string> &arguments) {
	String content;
	Error error = FileAccessOpenHarmony::get_rawfile_content("_cl_", content);
	if (error != OK && error != ERR_FILE_NOT_FOUND) {
		return error;
	}
	auto rawfile_exists = [resources](const std::string &path) {
		RawFile64 *file = OH_ResourceManager_OpenRawFile64(resources, path.c_str());
		if (!file) {
			return false;
		}
		OH_ResourceManager_CloseRawFile64(file);
		return true;
	};
	using GodotOpenHarmony::ArgumentError;
	const ArgumentError result = GodotOpenHarmony::prepare_packaged_arguments(
			content.utf8().get_data(), arguments, OS_OpenHarmony::BUNDLE_RESOURCE_DIR, rawfile_exists, arguments);
	switch (result) {
		case ArgumentError::OK:
			return OK;
		case ArgumentError::MISSING_DEFAULT_PACK:
			ERR_PRINT("The packaged game is missing rawfile template.pck.");
			return ERR_FILE_NOT_FOUND;
		case ArgumentError::PATH_OVERRIDES_DISABLED:
			ERR_PRINT("An explicit --main-pack requires disable_path_overrides=no; hardened builds use the bundled template.pck.");
			return ERR_UNAVAILABLE;
		case ArgumentError::INVALID_PARAMETER:
			ERR_PRINT("Invalid packaged game command line: editor/project-manager modes and missing option values are not allowed.");
			return ERR_INVALID_PARAMETER;
	}
	return ERR_INVALID_PARAMETER;
}

void run(NativeResourceManager *resources, void *window, int32_t window_id,
		int32_t width, int32_t height, std::vector<std::string> arguments,
		const std::string &permissions, bool packaged_game) {
	// ArkUI owns its UI thread. All engine initialization, input and teardown
	// happen on this single thread; VSync callbacks only wake it.
	OS_OpenHarmony os;
	OS_OpenHarmony::EXEC_PATH = packaged_game ? "template" : "godot";
	FileAccessOpenHarmony::setup(resources);
	DirAccessOpenHarmony::setup(resources);
	if (packaged_game) {
		const Error error = packaged_arguments(resources, arguments);
		if (error != OK) {
			state = -int(error);
			return;
		}
	}
	// A crash in this process kills the engine, and the process that
	// started it stays alive: leave the backtrace where the application can read
	// it, next to the engine log and named after this process.
	const std::string log_prefix = (os.get_data_path() + "/godot-").utf8().get_data() + std::to_string(getpid());
	std::string crash_log;
	bool has_log_file = false;
	for (size_t i = 0; i + 1 < arguments.size(); i++) {
		if (arguments[i] == "--" || arguments[i] == "++") {
			break;
		}
		if (arguments[i] == "--log-file") {
			has_log_file = true;
			const std::string &engine_log = arguments[i + 1];
			const size_t slash = engine_log.find_last_of('/');
			if (slash != std::string::npos) {
				crash_log = engine_log.substr(0, slash + 1) + "godot-" + std::to_string(getpid()) + "-crash.log";
			}
			break;
		}
	}
	if (!has_log_file) {
		arguments.insert(arguments.begin(), { "--log-file", log_prefix + "-engine.log" });
	}
	if (crash_log.empty()) {
		crash_log = log_prefix + "-crash.log";
	}
	ohos_crash_handler_install(crash_log.c_str());
	os.set_native_window(static_cast<OHNativeWindow *>(window));
	os.set_window_id(window_id);
	os.set_display_size(Size2i(width, height));
	os.set_allowed_permissions(permissions.c_str());
	std::vector<char *> argv;
	for (std::string &argument : arguments) {
		argv.push_back(argument.data());
	}
	Error error = Main::setup(OS_OpenHarmony::EXEC_PATH, argv.size(), argv.data());
	if (error != OK) {
		state = -int(error);
		return; // Main::setup unwinds its own failed initialization.
	}
	// Editor binaries treat project-loading failures as a request to open the
	// project manager. A packaged launch must never accept that fallback (also
	// covers corrupt packs and invalid explicit --main-pack paths).
	if (packaged_game && (Engine::get_singleton()->is_project_manager_hint() || Engine::get_singleton()->is_editor_hint())) {
		ERR_PRINT("The packaged game could not load its project; refusing to start the editor or project manager.");
		Main::cleanup();
		state = -int(ERR_FILE_CORRUPT);
		return;
	}
	if (Main::start() != EXIT_SUCCESS || !os.get_main_loop()) {
		Main::cleanup();
		state = -int(FAILED);
		return;
	}
	OH_NativeVSync *vsync = OH_NativeVSync_Create_ForAssociatedWindow(window_id, "GodotHost", 9);
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
			if (vsync && !frame_requested) {
				frame_requested = true;
				lock.unlock();
				const int requested = OH_NativeVSync_RequestFrame(vsync, on_vsync, nullptr);
				lock.lock();
				if (requested != 0) {
					frame_requested = false;
				}
			}
			// Hidden windows may receive no VSync. Continue pumping lifecycle
			// events, but do not accumulate outstanding frame requests.
			frame_ready.wait_for(lock, std::chrono::milliseconds(frame_requested ? 100 : 16), [] { return frame || stopping; });
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

struct EngineLaunch {
	NativeResourceManager *resources;
	void *window;
	int32_t window_id;
	int32_t width;
	int32_t height;
	std::vector<std::string> arguments;
	std::string permissions;
	bool packaged_game;
};

void *engine_thread_entry(void *p_data) {
	EngineLaunch *launch = static_cast<EngineLaunch *>(p_data);
	run(launch->resources, launch->window, launch->window_id, launch->width, launch->height,
			std::move(launch->arguments), launch->permissions, launch->packaged_game);
	delete launch;
	return nullptr;
}
} // namespace

int godot_host_start(NativeResourceManager *resources, void *window, int32_t window_id,
		int32_t width, int32_t height, int argc, const char *const *argv,
		const char *granted_permissions, bool packaged_game) {
	std::lock_guard<std::mutex> lifetime_lock(lifetime_mutex);
	if (!resources || !window || window_id < 0 || width <= 0 || height <= 0 || argc < 0 || (argc && !argv) || !granted_permissions) {
		return -int(ERR_INVALID_PARAMETER);
	}
	for (int i = 0; i < argc; ++i) {
		if (!argv[i]) {
			return -int(ERR_INVALID_PARAMETER);
		}
	}
	int expected = 0;
	if (!state.compare_exchange_strong(expected, 1)) {
		return -int(ERR_ALREADY_IN_USE);
	}
	EngineLaunch *launch = new EngineLaunch{ resources, window, window_id, width, height, {}, granted_permissions, packaged_game };
	for (int i = 0; i < argc; ++i) {
		launch->arguments.emplace_back(argv[i]);
	}
	pthread_attr_t attributes;
	int error = pthread_attr_init(&attributes);
	if (error == 0) {
		error = pthread_attr_setstacksize(&attributes, ENGINE_THREAD_STACK_SIZE);
		if (error == 0) {
			error = pthread_create(&engine_thread, &attributes, engine_thread_entry, launch);
		}
		pthread_attr_destroy(&attributes);
	}
	if (error != 0) {
		delete launch;
		state = -int(ERR_CANT_CREATE);
		return state.load();
	}
	engine_thread_started = true;
	return 0;
}

void godot_host_stop() {
	std::lock_guard<std::mutex> lifetime_lock(lifetime_mutex);
	{
		std::lock_guard<std::mutex> lock(event_mutex);
		stopping = true;
		events.clear();
		frame_ready.notify_one();
	}
	if (engine_thread_started) {
		pthread_join(engine_thread, nullptr);
		engine_thread_started = false;
	}
	if (state.load() >= 0) {
		state = 3;
	}
}
int godot_host_state() {
	return state.load();
}
void godot_host_set_create_instance_callback(GodotCreateInstanceCallback callback) {
	create_instance_callback = callback;
}
int32_t godot_host_create_instance(int argc, const char *const *argv) {
	const auto callback = create_instance_callback.load();
	return callback ? callback(argc, argv) : -1;
}
void godot_host_touch(const GodotTouchEvent *p_events, int count) {
	if (!p_events || count <= 0) {
		return;
	}
	for (int i = 0; i < count; i++) {
		Event event{};
		event.type = Event::TOUCH;
		event.touch = p_events[i];
		enqueue(event);
	}
}
void godot_host_mouse(const GodotMouseEvent *p_event) {
	if (!p_event) {
		return;
	}
	Event event{};
	event.type = Event::MOUSE;
	event.mouse = *p_event;
	enqueue(event);
}
void godot_host_key(const GodotKeyEvent *p_event) {
	if (!p_event) {
		return;
	}
	Event event{};
	event.type = Event::KEY;
	event.key = *p_event;
	enqueue(event);
}
void godot_host_resize(int32_t width, int32_t height) {
	if (width <= 0 || height <= 0) {
		return;
	}
	Event event{};
	event.type = Event::RESIZE;
	event.first = width;
	event.second = height;
	enqueue(event);
}
void godot_host_window_event(int32_t p_event) {
	Event event{};
	event.type = Event::WINDOW;
	event.first = p_event;
	enqueue(event);
}
