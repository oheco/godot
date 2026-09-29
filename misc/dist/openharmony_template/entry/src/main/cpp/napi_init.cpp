// Godot Engine contributors. SPDX-License-Identifier: MIT
#include "engine_host_openharmony.h"
#include "runtime_paths.h"

#include <napi/native_api.h>
#include <native_window/external_window.h>
#include <rawfile/raw_file_manager.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
// This module owns one engine lifetime, including before-start destruction.
NativeResourceManager *resources = nullptr;
OHNativeWindow *window = nullptr;
int32_t window_id = -1;
uint64_t surface_id = 0, size_surface_id = 0;
int32_t width = 0, height = 0;
bool configured = false, requested = false, started = false, destroyed = false;
bool packaged_game = false;
std::vector<std::string> arguments;
std::string permissions;

struct SpawnRequest {
	uint32_t id;
	std::vector<std::string> args;
	std::promise<int32_t> result;
};
std::mutex spawn_mutex;
std::map<uint32_t, std::shared_ptr<SpawnRequest>> spawns;
uint32_t next_spawn = 0;
bool launcher_stopping = false;
napi_threadsafe_function launch_function = nullptr;

napi_value undefined(napi_env env) {
	napi_value result;
	napi_get_undefined(env, &result);
	return result;
}
bool type_error(napi_env env, const char *message) {
	napi_throw_type_error(env, nullptr, message);
	return false;
}
bool values(napi_env env, napi_callback_info info, size_t count, napi_value *args) {
	// Query first: otherwise N-API truncates extra arguments to the buffer size.
	size_t actual = 0;
	if (napi_get_cb_info(env, info, &actual, nullptr, nullptr, nullptr) != napi_ok || actual != count) {
		return type_error(env, "Incorrect number of arguments");
	}
	if (count && napi_get_cb_info(env, info, &actual, args, nullptr, nullptr) != napi_ok) {
		return type_error(env, "Unable to read arguments");
	}
	return true;
}
bool string_value(napi_env env, napi_value value, std::string &result) {
	size_t length = 0;
	if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
		return type_error(env, "Expected a string");
	}
	std::vector<char> buffer(length + 1);
	if (napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(), &length) != napi_ok) {
		return type_error(env, "Unable to read string");
	}
	result.assign(buffer.data(), length);
	if (result.find('\0') != std::string::npos) {
		return type_error(env, "Strings must not contain NUL characters");
	}
	return true;
}
bool array_length(napi_env env, napi_value value, uint32_t &count, const char *name) {
	bool array = false;
	napi_status status = napi_is_array(env, value, &array);
	if (status != napi_ok) {
		return status == napi_pending_exception ? false : type_error(env,
				(std::string(name) + ": napi_is_array failed, status=" + std::to_string(status)).c_str());
	}
	if (!array) {
		return type_error(env, (std::string(name) + ": expected a native array (copy ArkUI observed arrays before calling N-API)").c_str());
	}
	status = napi_get_array_length(env, value, &count);
	if (status != napi_ok) {
		return status == napi_pending_exception ? false : type_error(env,
				(std::string(name) + ": napi_get_array_length failed, status=" + std::to_string(status)).c_str());
	}
	if (count > 65536) {
		return type_error(env, (std::string(name) + ": expected at most 65536 elements").c_str());
	}
	return true;
}
bool string_array(napi_env env, napi_value value, std::vector<std::string> &result, const char *name) {
	uint32_t count;
	if (!array_length(env, value, count, name)) {
		return false;
	}
	for (uint32_t i = 0; i < count; ++i) {
		napi_value element;
		std::string text;
		if (napi_get_element(env, value, i, &element) != napi_ok || !string_value(env, element, text)) {
			return false;
		}
		result.push_back(std::move(text));
	}
	return true;
}
bool number_value(napi_env env, napi_value value, double &result) {
	if (napi_get_value_double(env, value, &result) != napi_ok || !std::isfinite(result)) {
		return type_error(env, "Expected a finite number");
	}
	return true;
}
template <typename T>
bool integer_value(napi_env env, napi_value value, T &result) {
	double number;
	if (!number_value(env, value, number)) {
		return false;
	}
	if (std::trunc(number) != number || number < double(std::numeric_limits<T>::lowest()) || number > double(std::numeric_limits<T>::max())) {
		return type_error(env, "Expected an in-range integer");
	}
	result = static_cast<T>(number);
	return true;
}
bool bool_value(napi_env env, napi_value value, bool &result) {
	if (napi_get_value_bool(env, value, &result) != napi_ok) {
		return type_error(env, "Expected a boolean");
	}
	return true;
}
bool surface_value(napi_env env, napi_value value, uint64_t &id) {
	bool lossless = false;
	if (napi_get_value_bigint_uint64(env, value, &id, &lossless) != napi_ok || !lossless || id == 0) {
		return type_error(env, "Expected a positive uint64 surface bigint");
	}
	return true;
}
bool property(napi_env env, napi_value object, const char *name, napi_value &value, bool optional = false) {
	napi_valuetype type;
	if (napi_typeof(env, object, &type) != napi_ok || type != napi_object) {
		return type_error(env, "Expected an input event object");
	}
	if (optional) {
		bool has = false;
		if (napi_has_named_property(env, object, name, &has) != napi_ok) {
			return type_error(env, "Cannot read input event property");
		}
		if (!has) {
			value = nullptr;
			return true;
		}
	}
	if (napi_get_named_property(env, object, name, &value) != napi_ok) {
		return type_error(env, "Cannot read input event property");
	}
	return true;
}
bool integer(napi_env env, napi_value object, const char *name, uint32_t &result) {
	napi_value value;
	return property(env, object, name, value) && integer_value(env, value, result);
}
bool number(napi_env env, napi_value object, const char *name, float &result, bool optional = false) {
	napi_value value;
	if (!property(env, object, name, value, optional)) {
		return false;
	}
	if (!value) {
		return true;
	}
	double parsed;
	if (!number_value(env, value, parsed)) {
		return false;
	}
	if (std::abs(parsed) > std::numeric_limits<float>::max()) {
		return type_error(env, "Input coordinate is out of range");
	}
	result = parsed;
	return true;
}
bool boolean(napi_env env, napi_value object, const char *name, bool &result, bool optional = false) {
	napi_value value;
	return property(env, object, name, value, optional) && (!value || bool_value(env, value, result));
}
bool alive(napi_env env) {
	const int state = godot_host_state();
	if (destroyed || state < 0 || state == 3) {
		napi_throw_error(env, nullptr, "Godot engine lifetime has ended; launch a new process to restart");
		return false;
	}
	return true;
}
bool maybe_start(napi_env env) {
	if (!alive(env)) {
		return false;
	}
	if (!started && configured && requested && resources && window && window_id >= 0 && width > 0 && height > 0 && surface_id == size_surface_id) {
		std::vector<const char *> argv;
		for (const auto &arg : arguments) {
			argv.push_back(arg.c_str());
		}
		const int result = godot_host_start(resources, window, window_id, width, height, argv.size(), argv.data(), permissions.c_str(), packaged_game);
		if (result != 0) {
			const std::string message = "Unable to start Godot engine thread: " + std::to_string(result);
			napi_throw_error(env, nullptr, message.c_str());
			return false;
		}
		started = true;
	}
	return true;
}

int32_t create_instance(int argc, const char *const *argv) {
	auto request = std::make_shared<SpawnRequest>();
	for (int i = 0; i < argc; ++i) {
		request->args.emplace_back(argv[i]);
	}
	auto result = request->result.get_future();
	{
		std::lock_guard<std::mutex> lock(spawn_mutex);
		if (!launch_function || launcher_stopping) {
			return -1;
		}
		request->id = ++next_spawn;
		spawns[request->id] = request;
		auto *data = new std::shared_ptr<SpawnRequest>(request);
		if (napi_call_threadsafe_function(launch_function, data, napi_tsfn_nonblocking) != napi_ok) {
			delete data;
			spawns.erase(request->id);
			return -1;
		}
	}
	if (result.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
		std::lock_guard<std::mutex> lock(spawn_mutex);
		spawns.erase(request->id);
		return -1;
	}
	return result.get();
}
void launch_on_ui(napi_env env, napi_value callback, void *, void *data) {
	std::unique_ptr<std::shared_ptr<SpawnRequest>> owner(static_cast<std::shared_ptr<SpawnRequest> *>(data));
	if (!env || !callback) {
		return;
	}
	auto &request = **owner;
	{
		std::lock_guard<std::mutex> lock(spawn_mutex);
		if (launcher_stopping || spawns.find(request.id) == spawns.end()) {
			return;
		}
	}
	napi_value args[2], receiver, ignored;
	napi_get_undefined(env, &receiver);
	napi_create_uint32(env, request.id, &args[0]);
	napi_create_array_with_length(env, request.args.size(), &args[1]);
	for (size_t i = 0; i < request.args.size(); ++i) {
		napi_value arg;
		napi_create_string_utf8(env, request.args[i].data(), request.args[i].size(), &arg);
		napi_set_element(env, args[1], i, arg);
	}
	if (napi_call_function(env, receiver, callback, 2, args, &ignored) != napi_ok) {
		std::lock_guard<std::mutex> lock(spawn_mutex);
		auto found = spawns.find(request.id);
		if (found != spawns.end()) {
			found->second->result.set_value(-1);
			spawns.erase(found);
		}
	}
}
void cleanup(void *) {
	if (destroyed) {
		return;
	}
	destroyed = true;
	{
		std::lock_guard<std::mutex> lock(spawn_mutex);
		launcher_stopping = true;
		for (auto &entry : spawns) {
			entry.second->result.set_value(-1);
		}
		spawns.clear();
	}
	godot_host_set_create_instance_callback(nullptr);
	// Stop and join before releasing ANY resource the engine may still use.
	godot_host_stop();
	if (launch_function) {
		napi_release_threadsafe_function(launch_function, napi_tsfn_abort);
		launch_function = nullptr;
	}
	if (window) {
		OH_NativeWindow_DestroyNativeWindow(window);
		window = nullptr;
	}
	if (resources) {
		OH_ResourceManager_ReleaseNativeResourceManager(resources);
		resources = nullptr;
	}
}

napi_value configure(napi_env env, napi_callback_info info) {
	napi_value args[4];
	std::string files, cache, runtime, mode;
	if (!values(env, info, 4, args) || !string_value(env, args[0], files) || !string_value(env, args[1], cache) ||
			!string_value(env, args[2], runtime) || !string_value(env, args[3], mode) || !alive(env)) {
		return nullptr;
	}
	if (configured || started) {
		napi_throw_error(env, nullptr, "Runtime configuration is already committed");
		return nullptr;
	}
	std::string error;
	if (!configure_runtime_paths(files, cache, runtime, mode, error)) {
		napi_throw_error(env, nullptr, error.c_str());
		return nullptr;
	}
	configured = true;
	return maybe_start(env) ? undefined(env) : nullptr;
}
napi_value set_resources(napi_env env, napi_callback_info info) {
	napi_value args[1];
	if (!values(env, info, 1, args) || !alive(env)) {
		return nullptr;
	}
	napi_valuetype type;
	if (napi_typeof(env, args[0], &type) != napi_ok || type != napi_object) {
		type_error(env, "Expected a ResourceManager object");
		return nullptr;
	}
	if (!resources) {
		resources = OH_ResourceManager_InitNativeResourceManager(env, args[0]);
		if (!resources) {
			napi_throw_error(env, nullptr, "Cannot obtain native ResourceManager");
			return nullptr;
		}
	}
	return maybe_start(env) ? undefined(env) : nullptr;
}
napi_value set_window(napi_env env, napi_callback_info info) {
	napi_value args[1];
	int32_t id;
	if (!values(env, info, 1, args) || !integer_value(env, args[0], id) || !alive(env)) {
		return nullptr;
	}
	if (id < 0 || (started && id != window_id)) {
		napi_throw_error(env, nullptr, "Invalid or changed window ID");
		return nullptr;
	}
	window_id = id;
	return maybe_start(env) ? undefined(env) : nullptr;
}
napi_value set_surface(napi_env env, napi_callback_info info) {
	napi_value args[1];
	uint64_t id;
	if (!values(env, info, 1, args) || !surface_value(env, args[0], id) || !alive(env)) {
		return nullptr;
	}
	if (window && id != surface_id) {
		napi_throw_error(env, nullptr, "An engine lifetime cannot replace its native surface");
		return nullptr;
	}
	if (!window) {
		OHNativeWindow *created = nullptr;
		if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(id, &created) != 0 || !created) {
			napi_throw_error(env, nullptr, "Cannot obtain XComponent native window");
			return nullptr;
		}
		window = created;
		surface_id = id;
	}
	return maybe_start(env) ? undefined(env) : nullptr;
}
napi_value resize(napi_env env, napi_callback_info info) {
	napi_value args[3];
	uint64_t id;
	int32_t new_width, new_height;
	if (!values(env, info, 3, args) || !surface_value(env, args[0], id) || !integer_value(env, args[1], new_width) ||
			!integer_value(env, args[2], new_height) || !alive(env)) {
		return nullptr;
	}
	if (new_width <= 0 || new_height <= 0 || (window && id != surface_id)) {
		napi_throw_error(env, nullptr, "Invalid surface dimensions or mismatched surface ID");
		return nullptr;
	}
	width = new_width;
	height = new_height;
	size_surface_id = id;
	if (started) {
		godot_host_resize(width, height);
	}
	return maybe_start(env) ? undefined(env) : nullptr;
}
napi_value destroy(napi_env env, napi_callback_info info) {
	if (!values(env, info, 0, nullptr)) {
		return nullptr;
	}
	cleanup(nullptr);
	return undefined(env);
}
napi_value setup(napi_env env, napi_callback_info info) {
	napi_value args[3];
	std::vector<std::string> parsed_args, granted;
	bool packaged;
	if (!values(env, info, 3, args) || !string_array(env, args[0], parsed_args, "setup.arguments") || !string_array(env, args[1], granted, "setup.grantedPermissions") ||
			!bool_value(env, args[2], packaged) || !alive(env)) {
		return nullptr;
	}
	if (requested || started) {
		napi_throw_error(env, nullptr, "Engine setup is already committed; a process cannot restart Godot");
		return nullptr;
	}
	std::string joined;
	for (const auto &permission : granted) {
		if (permission.empty() || permission.find(',') != std::string::npos) {
			type_error(env, "Expected nonempty permission names without commas");
			return nullptr;
		}
		if (!joined.empty()) {
			joined += ',';
		}
		joined += permission;
	}
	arguments = std::move(parsed_args);
	permissions = std::move(joined);
	packaged_game = packaged;
	requested = true;
	return maybe_start(env) ? undefined(env) : nullptr;
}
napi_value status(napi_env env, napi_callback_info info) {
	if (!values(env, info, 0, nullptr)) {
		return nullptr;
	}
	napi_value value;
	napi_create_int32(env, godot_host_state(), &value);
	return value;
}
napi_value input_touch(napi_env env, napi_callback_info info) {
	napi_value args[1];
	uint32_t count;
	if (!values(env, info, 1, args) || !array_length(env, args[0], count, "inputTouch.events")) {
		return nullptr;
	}
	std::vector<GodotTouchEvent> events;
	for (uint32_t i = 0; i < count; ++i) {
		napi_value value, type_value, id_value;
		double type, id;
		GodotTouchEvent event{};
		if (napi_get_element(env, args[0], i, &value) != napi_ok ||
				!property(env, value, "type", type_value) || !number_value(env, type_value, type) ||
				!property(env, value, "id", id_value) || !number_value(env, id_value, id) ||
				!number(env, value, "x", event.x) || !number(env, value, "y", event.y)) {
			return nullptr;
		}
		if (std::trunc(type) != type || std::trunc(id) != id) {
			type_error(env, "Touch type and normalized ID must be integers");
			return nullptr;
		}
		// ArkUI may deliver mouse-synthesized/hover contacts with opaque IDs.
		// The host maps real contacts to 32 slots. Reject unsupported samples
		// without throwing through an input callback and terminating the Ability;
		// never use a raw device ID to resize the engine's touch history vector.
		if (type < 0 || type > 3 || id < 0 || id >= 32) {
			continue;
		}
		event.type = static_cast<uint32_t>(type);
		event.id = static_cast<uint32_t>(id);
		events.push_back(event);
	}
	godot_host_touch(events.data(), events.size());
	return undefined(env);
}
napi_value input_key(napi_env env, napi_callback_info info) {
	napi_value args[1];
	if (!values(env, info, 1, args)) {
		return nullptr;
	}
	auto value = args[0];
	GodotKeyEvent event{};
	uint32_t unicode;
	if (!integer(env, value, "code", event.code) || !integer(env, value, "unicode", unicode) ||
			!boolean(env, value, "pressed", event.pressed) || !boolean(env, value, "echo", event.echo, true) ||
			!boolean(env, value, "alt", event.alt) || !boolean(env, value, "ctrl", event.ctrl) ||
			!boolean(env, value, "shift", event.shift) || !boolean(env, value, "meta", event.meta)) {
		return nullptr;
	}
	if (unicode > 0x10ffff || (unicode >= 0xd800 && unicode <= 0xdfff)) {
		type_error(env, "Expected a Unicode scalar value");
		return nullptr;
	}
	event.unicode = unicode;
	godot_host_key(&event);
	return undefined(env);
}
napi_value input_mouse(napi_env env, napi_callback_info info) {
	napi_value args[1];
	if (!values(env, info, 1, args)) {
		return nullptr;
	}
	auto value = args[0];
	GodotMouseEvent event{};
	if (!integer(env, value, "type", event.type) || !integer(env, value, "button", event.button) ||
			!integer(env, value, "mask", event.mask) || !number(env, value, "x", event.x) || !number(env, value, "y", event.y) ||
			!number(env, value, "factor", event.factor, true) || !boolean(env, value, "alt", event.alt, true) ||
			!boolean(env, value, "ctrl", event.ctrl, true) || !boolean(env, value, "shift", event.shift, true) ||
			!boolean(env, value, "meta", event.meta, true) || !boolean(env, value, "doubleClick", event.double_click, true) ||
			!boolean(env, value, "hasRelative", event.has_relative, true) ||
			!number(env, value, "relativeX", event.relative_x, !event.has_relative) || !number(env, value, "relativeY", event.relative_y, !event.has_relative)) {
		return nullptr;
	}
	if (event.type > 2) {
		type_error(env, "Mouse type is out of range");
		return nullptr;
	}
	godot_host_mouse(&event);
	return undefined(env);
}
napi_value window_event(napi_env env, napi_callback_info info) {
	napi_value args[1];
	int32_t event;
	if (!values(env, info, 1, args) || !integer_value(env, args[0], event)) {
		return nullptr;
	}
	if (event < 1 || event > 7) {
		type_error(env, "Window event is out of range");
		return nullptr;
	}
	godot_host_window_event(event);
	return undefined(env);
}
napi_value set_launcher(napi_env env, napi_callback_info info) {
	napi_value args[1];
	if (!values(env, info, 1, args) || !alive(env)) {
		return nullptr;
	}
	napi_valuetype type;
	if (napi_typeof(env, args[0], &type) != napi_ok || type != napi_function) {
		type_error(env, "Expected a launcher function");
		return nullptr;
	}
	// The launcher is optional and may arrive after other startup prerequisites.
	std::lock_guard<std::mutex> lock(spawn_mutex);
	if (launch_function) {
		napi_throw_error(env, nullptr, "Launcher already configured");
		return nullptr;
	}
	napi_value name;
	napi_create_string_utf8(env, "GodotLaunchAbility", NAPI_AUTO_LENGTH, &name);
	if (napi_create_threadsafe_function(env, args[0], nullptr, name, 0, 1, nullptr, nullptr, nullptr,
				launch_on_ui, &launch_function) != napi_ok) {
		napi_throw_error(env, nullptr, "Cannot create launcher thread-safe function");
		return nullptr;
	}
	godot_host_set_create_instance_callback(create_instance);
	return undefined(env);
}
napi_value spawn_result(napi_env env, napi_callback_info info) {
	napi_value args[2];
	uint32_t id;
	int32_t pid;
	if (!values(env, info, 2, args) || !integer_value(env, args[0], id) || !integer_value(env, args[1], pid)) {
		return nullptr;
	}
	std::lock_guard<std::mutex> lock(spawn_mutex);
	auto found = spawns.find(id);
	if (found != spawns.end()) {
		found->second->result.set_value(pid);
		spawns.erase(found);
	}
	return undefined(env);
}
napi_value process_id(napi_env env, napi_callback_info info) {
	if (!values(env, info, 0, nullptr)) {
		return nullptr;
	}
	napi_value result;
	napi_create_int32(env, getpid(), &result);
	return result;
}
napi_value init(napi_env env, napi_value exports) {
	const napi_property_descriptor properties[] = {
#define METHOD(name, callback) { name, nullptr, callback, nullptr, nullptr, nullptr, napi_default, nullptr }
		METHOD("setLauncher", set_launcher),
		METHOD("spawnResult", spawn_result),
		METHOD("processId", process_id),
		METHOD("configure", configure),
		METHOD("setResourceManager", set_resources),
		METHOD("setWindowId", set_window),
		METHOD("setSurfaceId", set_surface),
		METHOD("changeSurface", resize),
		METHOD("destroySurface", destroy),
		METHOD("setup", setup),
		METHOD("state", status),
		METHOD("inputTouch", input_touch),
		METHOD("inputKey", input_key),
		METHOD("inputMouse", input_mouse),
		METHOD("sendWindowEvent", window_event),
#undef METHOD
	};
	napi_define_properties(env, exports, sizeof(properties) / sizeof(properties[0]), properties);
	napi_add_env_cleanup_hook(env, cleanup, nullptr);
	return exports;
}
napi_module module = { 1, 0, nullptr, init, "entry", nullptr, { 0 } };
} // namespace
extern "C" __attribute__((constructor)) void RegisterGodotEntryModule() {
	napi_module_register(&module);
}
