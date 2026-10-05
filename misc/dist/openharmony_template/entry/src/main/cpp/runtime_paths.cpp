// Godot Engine contributors. SPDX-License-Identifier: MIT
#include "runtime_paths.h"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>

namespace {
std::string oheco_root() {
	const char *configured = getenv("OHECO_ROOT");
	if (configured != nullptr && configured[0] != '\0') {
		return configured;
	}
	return "/storage/Users/currentUser/.oheco";
}

// Returns a complete versioned child of dir joined with suffix, or empty.
std::string versioned_entry(const std::string &dir, const std::string &suffix) {
	DIR *handle = opendir(dir.c_str());
	if (handle == nullptr) {
		return {};
	}
	std::string found;
	while (dirent *entry = readdir(handle)) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		const std::string candidate = dir + "/" + entry->d_name + suffix;
		struct stat info {};
		if (stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode) && candidate > found) {
			found = candidate;
		}
	}
	closedir(handle);
	return found;
}

bool has_sdk_layout(const std::string &root) {
	struct stat info {};
	if (stat((root + "/dotnet").c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
		return false;
	}
	if (versioned_entry(root + "/sdk", "/dotnet.dll").empty()) {
		return false;
	}
	return !versioned_entry(root + "/shared/Microsoft.NETCore.App", "/libcoreclr.so").empty();
}
} // namespace

std::string resolve_dotnet_root() {
	const char *override_root = getenv("GODOT_OHOS_DOTNET_ROOT");
	if (override_root != nullptr && override_root[0] != '\0' && has_sdk_layout(override_root)) {
		return override_root;
	}
	// `oo` links the active version from bin/dotnet, so prefer whatever it points at.
	const std::string root = oheco_root();
	char resolved[4096] = {};
	const std::string active = root + "/bin/dotnet";
	if (realpath(active.c_str(), resolved) != nullptr) {
		const std::string marker = "/bin/dotnet";
		const std::string path = resolved;
		if (path.size() > marker.size() && path.compare(path.size() - marker.size(), marker.size(), marker) == 0) {
			const std::string candidate = path.substr(0, path.size() - marker.size());
			if (has_sdk_layout(candidate)) {
				return candidate;
			}
		}
	}
	// Otherwise take the greatest installed version directory that is complete.
	const std::string base = root + "/packages/dotnet-sdk";
	DIR *dir = opendir(base.c_str());
	if (dir == nullptr) {
		return {};
	}
	std::string best;
	while (dirent *entry = readdir(dir)) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		const std::string candidate = base + "/" + entry->d_name;
		if (has_sdk_layout(candidate) && candidate > best) {
			best = candidate;
		}
	}
	closedir(dir);
	return best;
}

void add_independent_library_directory(const std::string &directory) {
	if (directory.empty()) {
		return;
	}
	void *libc = dlopen("libc.so", RTLD_LAZY);
	if (libc == nullptr) {
		return;
	}
	typedef int (*AddPluginPathFunc)(char *);
	AddPluginPathFunc add_path =
			reinterpret_cast<AddPluginPathFunc>(dlsym(libc, "dlns_add_plugin_default_ld_dictionary"));
	if (add_path != nullptr) {
		// The linker stores the pointer, so the buffer has to stay writable.
		static std::mutex directory_mutex;
		static std::list<std::string> registered;
		std::lock_guard<std::mutex> lock(directory_mutex);
		for (const auto &path : registered) {
			if (path == directory) {
				dlclose(libc);
				return;
			}
		}
		registered.push_back(directory);
		add_path(registered.back().data());
	}
	dlclose(libc);
}


bool configure_runtime_paths(const std::string &files, const std::string &cache,
		const std::string &runtime, const std::string &managed_mode, std::string &message) {
	message.clear();
	if (managed_mode != "none" && managed_mode != "sdk") {
		message = "Unknown managed mode (expected none or sdk)";
		return false;
	}
	auto absolute_path = [](const std::string &path) {
		return !path.empty() && path[0] == '/' && path.find('\0') == std::string::npos;
	};
	if (!absolute_path(files) || !absolute_path(cache) || (managed_mode == "sdk" && !absolute_path(runtime))) {
		message = "Expected absolute sandbox paths (and runtime path in sdk mode)";
		return false;
	}
	std::error_code sandbox_error;
	for (const auto &directory : { files, cache, files + "/config", cache + "/tmp" }) {
		std::filesystem::create_directories(directory, sandbox_error);
		if (sandbox_error) {
			message = "Cannot prepare " + directory + ": " + sandbox_error.message();
			return false;
		}
	}
	bool environment_ok = true;
	auto set = [&](const char *key, const std::string &value) {
		if (setenv(key, value.c_str(), 1) != 0) {
			environment_ok = false;
			message = std::string("Cannot set ") + key + ": " + strerror(errno);
		}
	};
	set("GODOT_OHOS_DATA_DIR", files);
	set("GODOT_OHOS_CACHE_DIR", cache);
	set("TMPDIR", cache + "/tmp");
	if (managed_mode == "none") {
		// No SDK discovery, GodotSharp checks, examples, NuGet or .NET env.
		return environment_ok;
	}
	// The .NET SDK is not shipped with the application: it is resolved from the
	// oheco package installation so the engine stays decoupled from its version.
	// A missing SDK is not fatal here: the editor still starts and Godot itself
	// reports the missing runtime. Reaching the SDK at all requires the two
	// restricted permissions declared in module.json5.
	const std::string dotnet = resolve_dotnet_root();
	if (!dotnet.empty()) {
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
				struct stat info {};
				if (stat(versioned.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
					add_independent_library_directory(versioned);
				}
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
		message = "The packaged GodotSharp assemblies are missing";
		return false;
	}
	std::error_code error;
	for (const auto &directory : { files + "/Projects", files + "/config", files + "/nuget", files + "/dotnet-cli", cache + "/tmp" }) {
		std::filesystem::create_directories(directory, error);
		if (error) {
			// Name the path: a bare "File exists" does not say which entry blocked it.
			message = "Cannot prepare " + directory + ": " + error.message();
			return false;
		}
	}
	const std::string sample = files + "/Projects/CSharpSmoke-4.7.2-ohos.3";
	if (!std::filesystem::exists(sample, error)) {
		std::filesystem::copy(runtime + "/Examples/CSharpSmoke", sample, std::filesystem::copy_options::recursive, error);
		if (error) {
			message = "Unable to prepare the bundled C# example: " + error.message();
			return false;
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
			message = "Unable to prepare the local NuGet feed configuration";
			return false;
		}
	}
	set("GODOT_SHARP_ROOT", runtime + "/GodotSharp");
	if (!dotnet.empty()) {
		set("DOTNET_ROOT", dotnet);
		set("DOTNET_ROOT_ARM64", dotnet);
		set("PATH", dotnet + ":" + (getenv("PATH") ? getenv("PATH") : "/system/bin"));
	}
	set("DOTNET_CLI_HOME", files + "/dotnet-cli");
	set("NUGET_PACKAGES", files + "/nuget");
	set("GODOT_NUGET_SOURCE", runtime + "/nuget");
	set("DOTNET_OHOS_TMPDIR", cache + "/tmp");
	set("DOTNET_CLI_TELEMETRY_OPTOUT", "1");
	set("DOTNET_GENERATE_ASPNET_CERTIFICATE", "false");
	set("DOTNET_CLI_DO_NOT_USE_MSBUILD_SERVER", "1");
	set("DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE", "true");
	set("MSBUILDDISABLENODEREUSE", "1");
	set("UseSharedCompilation", "false");
	set("NuGetAudit", "false");
	return environment_ok;
}
