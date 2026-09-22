// Godot Engine contributors. SPDX-License-Identifier: MIT
#include "editor_bridge_openharmony.h"
#include "runtime_probe.h"

#include <napi/native_api.h>
#include <native_window/external_window.h>
#include <rawfile/raw_file_manager.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
NativeResourceManager *resources = nullptr;
OHNativeWindow *window = nullptr;
int32_t window_id = -1;
int32_t width = 0, height = 0;
bool configured = false, requested = false, started = false;
std::string sdk_executable, sdk_report, bundled_root, cache_directory, files_directory, missing_dotnet;
std::vector<std::string> arguments;
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

int32_t create_instance(int argc, const char *const *argv) {
	auto request = std::make_shared<SpawnRequest>();
	for (int i = 0; i < argc; i++) {
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
	}
	auto *data = new std::shared_ptr<SpawnRequest>(request);
	if (napi_call_threadsafe_function(launch_function, data, napi_tsfn_nonblocking) != napi_ok) {
		delete data;
		std::lock_guard<std::mutex> lock(spawn_mutex);
		spawns.erase(request->id);
		return -1;
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
	napi_value args[2], receiver, ignored;
	napi_get_undefined(env, &receiver);
	napi_create_uint32(env, request.id, &args[0]);
	napi_create_array_with_length(env, request.args.size(), &args[1]);
	for (size_t i = 0; i < request.args.size(); i++) {
		napi_value arg;
		napi_create_string_utf8(env, request.args[i].data(), request.args[i].size(), &arg);
		napi_set_element(env, args[1], i, arg);
	}
	napi_call_function(env, receiver, callback, 2, args, &ignored);
}

napi_value undefined(napi_env env) {
	napi_value result;
	napi_get_undefined(env, &result);
	return result;
}
bool values(napi_env env, napi_callback_info info, size_t count, napi_value *args) {
	size_t actual = count;
	if (napi_get_cb_info(env, info, &actual, args, nullptr, nullptr) != napi_ok || actual != count) {
		napi_throw_type_error(env, nullptr, "Incorrect number of arguments");
		return false;
	}
	return true;
}
std::string string_value(napi_env env, napi_value value) {
	size_t length = 0;
	if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
		napi_throw_type_error(env, nullptr, "Expected a string");
		return {};
	}
	std::vector<char> buffer(length + 1);
	napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(), &length);
	return std::string(buffer.data(), length);
}
double number(napi_env env, napi_value object, const char *name) {
	napi_value value;
	double result = 0;
	if (napi_get_named_property(env, object, name, &value) == napi_ok) {
		napi_get_value_double(env, value, &result);
	}
	return result;
}
bool boolean(napi_env env, napi_value object, const char *name) {
	napi_value value;
	bool result = false;
	if (napi_get_named_property(env, object, name, &value) == napi_ok) {
		napi_get_value_bool(env, value, &result);
	}
	return result;
}
void maybe_start(napi_env env) {
	if (!started && configured && requested && resources && window && window_id >= 0 && width > 0 && height > 0) {
		std::vector<const char *> argv;
		for (const auto &arg : arguments) {
			argv.push_back(arg.c_str());
		}
		int result = godot_editor_start(resources, window, window_id, width, height, argv.size(), argv.data());
		if (result != 0) {
			napi_throw_error(env, nullptr, "Unable to start Godot engine thread");
			return;
		}
		started = true;
	}
}
void cleanup(void *) {
	{
		std::lock_guard<std::mutex> lock(spawn_mutex);
		launcher_stopping = true;
		for (auto &entry : spawns) {
			entry.second->result.set_value(-1);
		}
		spawns.clear();
	}
	godot_editor_stop();
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
	napi_value args[3];
	if (!values(env, info, 3, args)) {
		return nullptr;
	}
	const std::string files = string_value(env, args[0]);
	const std::string cache = string_value(env, args[1]);
	const std::string runtime = string_value(env, args[2]);
	if (files.empty() || cache.empty() || runtime.empty() || started) {
		napi_throw_error(env, nullptr, "Invalid runtime configuration");
		return nullptr;
	}
	// The .NET SDK is not shipped with the application: it is resolved from the
	// oheco package installation so the engine stays decoupled from its version.
	// A missing SDK is not fatal here: the editor still starts and Godot itself
	// reports the missing runtime. Reaching the SDK at all requires the two
	// restricted permissions declared in module.json5.
	const std::string dotnet = resolve_dotnet_root();
	if (dotnet.empty()) {
		missing_dotnet = "No usable .NET SDK found under the oheco package root "
				"(install it with 'oo install dotnet-sdk', and grant "
				"ohos.permission.READ_WRITE_USER_FILE and "
				"ohos.permission.ALLOW_EXTERNAL_NATIVE_CODE); "
				"GODOT_OHOS_DOTNET_ROOT overrides the location.";
	} else {
		// HarmonyOS loads a library outside the application bundle only from a
		// directory registered with the linker, and the restricted
		// ohos.permission.kernel.LOAD_INDEPENDENT_LIBRARY permission is what makes
		// the registration effective. Register the runtime directories now: the
		// .NET host loads the rest of the runtime itself, without going through
		// the paths this application opens by name.
		for (const auto &subdirectory : { "/host/fxr", "/shared/Microsoft.NETCore.App", "/shared/Microsoft.AspNetCore.App", "" }) {
			const std::string directory = dotnet + subdirectory;
			DIR *entries = opendir(directory.c_str());
			if (entries == nullptr) {
				continue;
			}
			while (dirent *entry = readdir(entries)) {
				if (entry->d_name[0] == '.') {
					continue;
				}
				const std::string versioned = directory + "/" + entry->d_name;
				add_independent_library_directory(versioned);
			}
			closedir(entries);
			add_independent_library_directory(directory);
		}
		// The runtime keeps its default write-xor-execute mode: it maps a shared
		// memory object once for execution and once for writing, and
		// ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY is what allows the
		// executable mapping. If that mapping is ever refused again, setting
		// DOTNET_EnableWriteXorExecute=0 makes the runtime commit plain
		// read/write/execute pages instead.
	}
	if (access((runtime + "/GodotSharp/Api/Debug/GodotSharp.dll").c_str(), F_OK) != 0) {
		napi_throw_error(env, nullptr, "The packaged GodotSharp assemblies are missing");
		return nullptr;
	}
	std::error_code error;
	for (const auto &directory : { files + "/Projects", files + "/config", files + "/nuget", files + "/dotnet-cli", cache + "/tmp" }) {
		std::filesystem::create_directories(directory, error);
		if (error) {
			// Name the path: a bare "File exists" does not say which entry blocked it.
			const std::string message = "Cannot prepare " + directory + ": " + error.message();
			napi_throw_error(env, nullptr, message.c_str());
			return nullptr;
		}
	}
	const std::string sample = files + "/Projects/CSharpSmoke-4.7.2-ohos.2";
	if (!std::filesystem::exists(sample, error)) {
		std::filesystem::copy(runtime + "/Examples/CSharpSmoke", sample, std::filesystem::copy_options::recursive, error);
		if (error) {
			napi_throw_error(env, nullptr, "Unable to prepare the bundled C# example");
			return nullptr;
		}
	}
	const std::string nuget_config = files + "/Projects/NuGet.Config";
	if (!std::filesystem::exists(nuget_config, error)) {
		std::ofstream output(nuget_config);
		output << "<configuration><packageSources><clear/>"
				  "<add key=\"godot-bundled\" value=\"%GODOT_NUGET_SOURCE%\"/>"
				  "</packageSources></configuration>\n";
		output.close();
		if (!output) {
			napi_throw_error(env, nullptr, "Unable to prepare the local NuGet feed configuration");
			return nullptr;
		}
	}
	auto set = [](const char *key, const std::string &value) { setenv(key, value.c_str(), 1); };
	set("GODOT_OHOS_DATA_DIR", files);
	set("GODOT_OHOS_CACHE_DIR", cache);
	set("GODOT_SHARP_ROOT", runtime + "/GodotSharp");
	if (!dotnet.empty()) {
		set("DOTNET_ROOT", dotnet);
		set("DOTNET_ROOT_ARM64", dotnet);
		set("PATH", dotnet + ":" + (getenv("PATH") ? getenv("PATH") : "/system/bin"));
	}
	set("DOTNET_CLI_HOME", files + "/dotnet-cli");
	set("NUGET_PACKAGES", files + "/nuget");
	set("GODOT_NUGET_SOURCE", runtime + "/nuget");
	set("TMPDIR", cache + "/tmp");
	set("DOTNET_OHOS_TMPDIR", cache + "/tmp");
	set("DOTNET_CLI_TELEMETRY_OPTOUT", "1");
	set("DOTNET_GENERATE_ASPNET_CERTIFICATE", "false");
	set("DOTNET_CLI_DO_NOT_USE_MSBUILD_SERVER", "1");
	set("DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE", "true");
	set("MSBUILDDISABLENODEREUSE", "1");
	set("UseSharedCompilation", "false");
	set("NuGetAudit", "false");
	configured = true;
	bundled_root = dotnet;
	cache_directory = cache;
	files_directory = files;
	sdk_executable = dotnet.empty() ? std::string() : dotnet + "/dotnet";
	sdk_report = cache + "/godot-dotnet-startup-" + std::to_string(getpid()) + ".log";
	maybe_start(env);
	return undefined(env);
}

struct RuntimeCheck {
	napi_async_work work = nullptr;
	napi_deferred deferred = nullptr;
	std::string executable, report, error;
};
napi_value check_runtime(napi_env env, napi_callback_info info) {
	if (!configured || started) {
		napi_throw_error(env, nullptr, "Configure the runtime before checking the SDK");
		return nullptr;
	}
	if (sdk_executable.empty()) {
		const std::string message = missing_dotnet.empty() ? "No .NET SDK is available" : missing_dotnet;
		napi_throw_error(env, nullptr, message.c_str());
		return nullptr;
	}
	auto check = std::make_unique<RuntimeCheck>();
	check->executable = sdk_executable;
	check->report = sdk_report;
	napi_value promise, name;
	napi_create_promise(env, &check->deferred, &promise);
	napi_create_string_utf8(env, "GodotCheckDotnet", NAPI_AUTO_LENGTH, &name);
	auto execute = [](napi_env, void *data) {
		auto *check = static_cast<RuntimeCheck *>(data);
		check->error = check_dotnet_sdk(check->executable, check->report);
	};
	auto complete = [](napi_env env, napi_status status, void *data) {
		std::unique_ptr<RuntimeCheck> check(static_cast<RuntimeCheck *>(data));
		if (status != napi_ok && check->error.empty()) {
			check->error = ".NET SDK check was cancelled";
		}
		if (check->error.empty()) {
			napi_resolve_deferred(env, check->deferred, undefined(env));
		} else {
			const std::string message = check->error + "\n" + check->report;
			napi_value text, error;
			napi_create_string_utf8(env, message.c_str(), message.size(), &text);
			napi_create_error(env, nullptr, text, &error);
			napi_reject_deferred(env, check->deferred, error);
		}
		napi_delete_async_work(env, check->work);
	};
	if (napi_create_async_work(env, nullptr, name, execute, complete, check.get(), &check->work) != napi_ok ||
			napi_queue_async_work(env, check->work) != napi_ok) {
		if (check->work) {
			napi_delete_async_work(env, check->work);
		}
		napi_throw_error(env, nullptr, "Cannot schedule .NET SDK check");
		return nullptr;
	}
	check.release();
	return promise;
}
napi_value set_resources(napi_env env, napi_callback_info info) {
	napi_value args[1];
	if (!values(env, info, 1, args)) {
		return nullptr;
	}
	if (!resources) {
		resources = OH_ResourceManager_InitNativeResourceManager(env, args[0]);
	}
	maybe_start(env);
	return undefined(env);
}
napi_value set_window(napi_env env, napi_callback_info info) {
	napi_value args[1];
	if (!values(env, info, 1, args)) {
		return nullptr;
	}
	napi_get_value_int32(env, args[0], &window_id);
	maybe_start(env);
	return undefined(env);
}
napi_value set_surface(napi_env env, napi_callback_info info) {
	napi_value args[1];
	if (!values(env, info, 1, args)) {
		return nullptr;
	}
	uint64_t id = 0;
	bool lossless = false;
	if (window || napi_get_value_bigint_uint64(env, args[0], &id, &lossless) != napi_ok || !lossless ||
			OH_NativeWindow_CreateNativeWindowFromSurfaceId(id, &window) != 0) {
		napi_throw_error(env, nullptr, "Cannot obtain XComponent native window");
		return nullptr;
	}
	maybe_start(env);
	return undefined(env);
}
napi_value resize(napi_env env, napi_callback_info info) {
	napi_value args[3];
	if (!values(env, info, 3, args)) {
		return nullptr;
	}
	napi_get_value_int32(env, args[1], &width);
	napi_get_value_int32(env, args[2], &height);
	if (started) {
		godot_editor_resize(width, height);
	} else {
		maybe_start(env);
	}
	return undefined(env);
}
napi_value destroy(napi_env env, napi_callback_info info) {
	cleanup(nullptr);
	return undefined(env);
}
napi_value setup(napi_env env, napi_callback_info info) {
	napi_value args[1];
	if (!values(env, info, 1, args)) {
		return nullptr;
	}
	uint32_t count = 0;
	if (napi_get_array_length(env, args[0], &count) != napi_ok) {
		napi_throw_type_error(env, nullptr, "Expected command line arguments");
		return nullptr;
	}
	arguments.clear();
	for (uint32_t i = 0; i < count; i++) {
		napi_value value;
		napi_get_element(env, args[0], i, &value);
		arguments.push_back(string_value(env, value));
	}
	requested = true;
	maybe_start(env);
	return undefined(env);
}
napi_value status(napi_env env, napi_callback_info info) {
	napi_value value;
	napi_create_int32(env, godot_editor_state(), &value);
	return value;
}
napi_value input_touch(napi_env env, napi_callback_info info) {
	napi_value args[1];
	if (!values(env, info, 1, args)) {
		return nullptr;
	}
	uint32_t count = 0;
	napi_get_array_length(env, args[0], &count);
	std::vector<GodotTouchEvent> events;
	for (uint32_t i = 0; i < count; i++) {
		napi_value value;
		napi_get_element(env, args[0], i, &value);
		GodotTouchEvent event{};
		event.type = number(env, value, "type");
		event.id = number(env, value, "id");
		event.x = number(env, value, "x");
		event.y = number(env, value, "y");
		if (event.id < 32) {
			events.push_back(event);
		}
	}
	godot_editor_touch(events.data(), events.size());
	return undefined(env);
}
napi_value input_key(napi_env env, napi_callback_info info) {
	napi_value args[1];
	if (!values(env, info, 1, args)) {
		return nullptr;
	}
	auto value = args[0];
	GodotKeyEvent event{};
	event.code = number(env, value, "code");
	event.unicode = number(env, value, "unicode");
	event.pressed = boolean(env, value, "pressed");
	event.echo = boolean(env, value, "echo");
	event.alt = boolean(env, value, "alt");
	event.ctrl = boolean(env, value, "ctrl");
	event.shift = boolean(env, value, "shift");
	event.meta = boolean(env, value, "meta");
	godot_editor_key(&event);
	return undefined(env);
}
napi_value input_mouse(napi_env env, napi_callback_info info) {
	napi_value args[1];
	if (!values(env, info, 1, args)) {
		return nullptr;
	}
	auto value = args[0];
	GodotMouseEvent event{};
	event.type = number(env, value, "type");
	event.button = number(env, value, "button");
	event.mask = number(env, value, "mask");
	event.factor = number(env, value, "factor");
	event.x = number(env, value, "x");
	event.y = number(env, value, "y");
	event.alt = boolean(env, value, "alt");
	event.ctrl = boolean(env, value, "ctrl");
	event.shift = boolean(env, value, "shift");
	event.meta = boolean(env, value, "meta");
	event.double_click = boolean(env, value, "doubleClick");
	event.has_relative = boolean(env, value, "hasRelative");
	event.relative_x = number(env, value, "relativeX");
	event.relative_y = number(env, value, "relativeY");
	godot_editor_mouse(&event);
	return undefined(env);
}
napi_value window_event(napi_env env, napi_callback_info info) {
	napi_value args[1];
	if (!values(env, info, 1, args)) {
		return nullptr;
	}
	int32_t event = 0;
	napi_get_value_int32(env, args[0], &event);
	godot_editor_window_event(event);
	return undefined(env);
}
napi_value set_launcher(napi_env env, napi_callback_info info) {
	napi_value args[1];
	if (!values(env, info, 1, args)) {
		return nullptr;
	}
	if (launch_function || started) {
		napi_throw_error(env, nullptr, "Launcher already configured");
		return nullptr;
	}
	napi_value name;
	napi_create_string_utf8(env, "GodotLaunchAbility", NAPI_AUTO_LENGTH, &name);
	if (napi_create_threadsafe_function(env, args[0], nullptr, name, 0, 1, nullptr, nullptr, nullptr,
				launch_on_ui, &launch_function) != napi_ok) {
		return nullptr;
	}
	godot_editor_set_create_instance_callback(create_instance);
	return undefined(env);
}
napi_value spawn_result(napi_env env, napi_callback_info info) {
	napi_value args[2];
	if (!values(env, info, 2, args)) {
		return nullptr;
	}
	uint32_t id = 0;
	int32_t pid = -1;
	napi_get_value_uint32(env, args[0], &id);
	napi_get_value_int32(env, args[1], &pid);
	std::lock_guard<std::mutex> lock(spawn_mutex);
	auto found = spawns.find(id);
	if (found != spawns.end()) {
		found->second->result.set_value(pid);
		spawns.erase(found);
	}
	return undefined(env);
}
napi_value process_id(napi_env env, napi_callback_info info) {
	napi_value result;
	napi_create_int32(env, getpid(), &result);
	return result;
}
napi_value sandbox_probe(napi_env env, napi_callback_info info) {
	if (!configured) {
		napi_throw_error(env, nullptr, "Configure the runtime before probing the sandbox");
		return nullptr;
	}
	const std::string report = probe_sandbox(bundled_root, files_directory, cache_directory);
	napi_value result;
	napi_create_string_utf8(env, report.c_str(), report.size(), &result);
	return result;
}
napi_value init(napi_env env, napi_value exports) {
	const napi_property_descriptor properties[] = {
#define METHOD(name, callback) { name, nullptr, callback, nullptr, nullptr, nullptr, napi_default, nullptr }
		METHOD("setLauncher", set_launcher),
		METHOD("spawnResult", spawn_result),
		METHOD("processId", process_id),
		METHOD("probeSandbox", sandbox_probe),
		METHOD("configure", configure),
		METHOD("checkRuntime", check_runtime),
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
extern "C" __attribute__((constructor)) void RegisterGodotEditorModule() {
	napi_module_register(&module);
}
