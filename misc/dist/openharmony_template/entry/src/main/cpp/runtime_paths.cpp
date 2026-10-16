// Godot Engine contributors. SPDX-License-Identifier: MIT
#include "runtime_paths.h"

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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
bool absolute_path(const std::string &path) {
	return !path.empty() && path[0] == '/' && path.find('\0') == std::string::npos;
}
std::string canonical_path(const std::string &path) {
	if (!absolute_path(path)) { return {}; }
	char *resolved = realpath(path.c_str(), nullptr);
	if (!resolved) { return {}; }
	const std::string result(resolved);
	free(resolved);
	return result;
}
bool regular_file(const std::string &path) {
	struct stat info {};
	return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0;
}
std::string read_bounded(const std::string &path, size_t limit = 65536) {
	struct stat info {};
	if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 || uint64_t(info.st_size) > limit) { return {}; }
	std::ifstream input(path, std::ios::binary);
	std::string result(static_cast<size_t>(info.st_size), '\0');
	if (!input.read(result.data(), result.size()) || input.peek() != EOF) { return {}; }
	return result;
}
bool lower_hash(const std::string &value) {
	return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char ch) {
		return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
	});
}
std::string template_pointer(const std::string &hash) {
	return "{\"schemaVersion\":1,\"sha256\":\"" + hash + "\",\"source\":\"oheco-devkit\"}\n";
}
bool own_template_pointer(const std::string &bytes) {
	const std::string prefix = "{\"schemaVersion\":1,\"sha256\":\"";
	if (bytes.size() < prefix.size() + 64) { return false; }
	const std::string hash = bytes.substr(prefix.size(), 64);
	return lower_hash(hash) && bytes == template_pointer(hash);
}
bool version_less(const std::string &left, const std::string &right) {
	size_t a = 0, b = 0;
	while (a < left.size() && b < right.size()) {
		if (left[a] >= '0' && left[a] <= '9' && right[b] >= '0' && right[b] <= '9') {
			size_t ae = a, be = b;
			while (ae < left.size() && left[ae] >= '0' && left[ae] <= '9') { ++ae; }
			while (be < right.size() && right[be] >= '0' && right[be] <= '9') { ++be; }
			while (a + 1 < ae && left[a] == '0') { ++a; }
			while (b + 1 < be && right[b] == '0') { ++b; }
			if (ae - a != be - b) { return ae - a < be - b; }
			const int comparison = left.compare(a, ae - a, right, b, be - b);
			if (comparison != 0) { return comparison < 0; }
			a = ae; b = be;
		} else {
			if (left[a] != right[b]) { return left[a] < right[b]; }
			++a; ++b;
		}
	}
	return a == left.size() && b != right.size();
}
bool managed_assets(const std::string &root) {
	if (!regular_file(root + "/share/godot/GodotSharp/Api/Debug/GodotSharp.dll")) { return false; }
	for (const char *name : { "Godot.NET.Sdk", "Godot.SourceGenerators", "GodotSharp", "GodotSharpEditor" }) {
		if (!regular_file(root + "/share/godot/GodotSharp/Tools/nupkgs/" + name + ".4.7.2-ohos.3.nupkg")) { return false; }
	}
	return true;
}
} // namespace

std::vector<DevelopmentKitCandidate> development_kit_candidates() {
	std::vector<DevelopmentKitCandidate> result;
	auto append = [&](const std::string &candidate) {
		const std::string resolved = canonical_path(candidate);
		const std::string root = resolved.empty() ? candidate : resolved;
		if (std::any_of(result.begin(), result.end(), [&](const DevelopmentKitCandidate &entry) { return entry.root == root; })) { return; }
		result.push_back({ root, absolute_path(root) ? read_bounded(root + "/godot-devkit.json") : "" });
	};
	const char *override_root = getenv("GODOT_OHOS_DEVKIT_ROOT");
	if (override_root != nullptr) {
		append(override_root);
		return result; // An explicit invalid selection must not silently choose another kit.
	}
	const std::string active = canonical_path(oheco_root() + "/bin/godot");
	if (!active.empty() && active.size() > 10 && active.compare(active.size() - 10, 10, "/bin/godot") == 0) {
		append(active.substr(0, active.size() - 10));
	}
	const std::string packages = oheco_root() + "/packages/godot-editor";
	std::vector<std::string> versions;
	DIR *handle = opendir(packages.c_str());
	if (handle) {
		while (dirent *entry = readdir(handle)) {
			if (entry->d_name[0] != '.') { versions.push_back(packages + "/" + entry->d_name); }
		}
		closedir(handle);
	}
	std::sort(versions.rbegin(), versions.rend(), version_less);
	for (const auto &path : versions) {
		if (result.size() >= 128) { break; }
		append(path);
	}
	return result;
}

bool publish_development_kit_templates(const std::string &files, const std::string &runtime, std::string &notice) {
	notice.clear();
	const std::string templates = canonical_path(runtime + "/templates");
	const std::string marker = templates.empty() ? "" : read_bounded(templates + "/.ready", 65);
	if (templates.empty() || marker.size() != 65 || marker.back() != '\n' || !lower_hash(marker.substr(0, 64))) {
		notice = "Development kit templates have no valid .ready digest"; return false;
	}
	const std::string hash = marker.substr(0, 64);
	for (const char *name : { "openharmony_debug_arm64-v8a.zip", "openharmony_release_arm64-v8a.zip" }) {
		if (!regular_file(templates + "/" + name)) { notice = "Development kit is missing an export-template archive"; return false; }
	}
	if (!absolute_path(files)) { notice = "Template data root must be absolute"; return false; }
	const std::string parent = files + "/export_templates", pointer = parent + "/current.json";
	std::error_code error;
	std::filesystem::create_directories(parent, error);
	if (error) { notice = "Cannot prepare template data root: " + error.message(); return false; }
	auto pointer_writable = [&]() {
		std::error_code status_error;
		const auto status = std::filesystem::symlink_status(pointer, status_error);
		if (status.type() == std::filesystem::file_type::not_found) { return true; }
		return !status_error && status.type() == std::filesystem::file_type::regular && own_template_pointer(read_bounded(pointer, 256));
	};
	if (!pointer_writable()) { notice = "Existing custom export-template pointer preserved"; return true; }
	const std::string link = parent + "/" + hash;
	const auto status = std::filesystem::symlink_status(link, error);
	if (error && error != std::errc::no_such_file_or_directory) { notice = "Cannot inspect template set: " + error.message(); return false; }
	if (status.type() == std::filesystem::file_type::not_found) {
		std::filesystem::create_directory_symlink(templates, link, error);
		if (error && error != std::errc::file_exists) { notice = "Cannot link development-kit templates: " + error.message(); return false; }
	}
	if (canonical_path(link) != templates) {
		// Existing real directories remain intact. A complete manually installed
		// set with the same digest can already serve this engine's finder.
		std::error_code existing_error;
		const auto existing = std::filesystem::symlink_status(link, existing_error);
		if (existing_error || existing.type() != std::filesystem::file_type::directory || read_bounded(link + "/.ready", 65) != marker ||
				!regular_file(link + "/openharmony_debug_arm64-v8a.zip") || !regular_file(link + "/openharmony_release_arm64-v8a.zip")) {
			notice = "Existing export-template set preserved; it is not the selected development-kit link"; return false;
		}
	}
	const std::string desired = template_pointer(hash);
	if (read_bounded(pointer, 256) == desired) { return true; }
	const char *temporary = getenv("TMPDIR");
	if (!temporary || !absolute_path(temporary)) { notice = "An absolute TMPDIR is required to publish the template pointer"; return false; }
	std::string pattern = std::string(temporary) + "/godot-devkit-pointer-XXXXXX";
	std::vector<char> bytes(pattern.begin(), pattern.end()); bytes.push_back('\0');
	const int fd = mkstemp(bytes.data());
	if (fd < 0) { notice = "Cannot create temporary template pointer: " + std::string(strerror(errno)); return false; }
	const std::string temporary_file(bytes.data());
	bool written = true; size_t offset = 0;
	while (offset < desired.size()) {
		const ssize_t count = write(fd, desired.data() + offset, desired.size() - offset);
		if (count < 0 && errno == EINTR) { continue; }
		if (count <= 0) { written = false; break; }
		offset += static_cast<size_t>(count);
	}
	if (written && fsync(fd) != 0) { written = false; }
	if (close(fd) != 0) { written = false; }
	bool success = false;
	if (!written) { notice = "Cannot write temporary template pointer"; }
	else if (!pointer_writable()) { notice = "Concurrent custom export-template pointer preserved"; success = true; }
	else if (rename(temporary_file.c_str(), pointer.c_str()) != 0) { notice = "Cannot publish development-kit template pointer: " + std::string(strerror(errno)); }
	else { success = true; }
	// Only the exact mkstemp-created pathname is ever removed.
	if (unlink(temporary_file.c_str()) != 0 && errno != ENOENT && success) { notice = "Unable to clean the own temporary template pointer"; }
	return success;
}

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

namespace {
void register_dotnet_runtime_directories(const std::string &dotnet) {
	for (const auto &suffix : { "/host/fxr", "/shared/Microsoft.NETCore.App", "/shared/Microsoft.AspNetCore.App", "" }) {
		const std::string directory = dotnet + suffix;
		DIR *entries = opendir(directory.c_str());
		if (entries) {
			while (dirent *entry = readdir(entries)) {
				if (entry->d_name[0] == '.') { continue; }
				const std::string versioned = directory + "/" + entry->d_name;
				struct stat info {};
				if (stat(versioned.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) { add_independent_library_directory(versioned); }
			}
			closedir(entries);
		}
		add_independent_library_directory(directory);
	}
}
} // namespace

bool configure_cli_development_kit(const std::string &executable, std::string &notice) {
	notice.clear();
	std::string root;
	const char *override_root = getenv("GODOT_OHOS_DEVKIT_ROOT");
	if (override_root) { root = canonical_path(override_root); }
	else {
		std::string binary = canonical_path(executable);
		if (binary.empty()) { binary = canonical_path("/proc/self/exe"); }
		if (binary.size() <= 10 || binary.compare(binary.size() - 10, 10, "/bin/godot") != 0) { return true; }
		root = binary.substr(0, binary.size() - 10);
	}
	if (root.empty() || read_bounded(root + "/godot-devkit.json").empty() || !regular_file(root + "/bin/libgodot.so") || !managed_assets(root)) {
		if (override_root) { notice = "The explicit development kit is unavailable; native CLI remains usable"; }
		return true;
	}
	bool ok = true;
	auto set_default = [&](const char *name, const std::string &value) {
		if (!getenv(name) && setenv(name, value.c_str(), 1) != 0) {
			ok = false; notice = std::string("Cannot set ") + name + ": " + strerror(errno);
		}
	};
	const std::string runtime = root + "/share/godot";
	set_default("GODOT_SHARP_ROOT", runtime + "/GodotSharp");
	set_default("GODOT_NUGET_SOURCE", runtime + "/GodotSharp/Tools/nupkgs");
	const char *configured_dotnet = getenv("DOTNET_ROOT");
	const std::string dotnet = configured_dotnet ? configured_dotnet : resolve_dotnet_root();
	if (!dotnet.empty()) {
		set_default("DOTNET_ROOT", dotnet);
		set_default("DOTNET_ROOT_ARM64", dotnet);
		register_dotnet_runtime_directories(dotnet);
	}
	if (!getenv("GODOT_OHOS_DATA_DIR")) {
		const char *config = getenv("XDG_CONFIG_HOME");
		if (config && absolute_path(config)) { set_default("GODOT_OHOS_DATA_DIR", std::string(config) + "/godot-editor"); }
	}
	if (!getenv("GODOT_OHOS_CACHE_DIR")) {
		const char *cache = getenv("XDG_CACHE_HOME");
		if (cache && absolute_path(cache)) { set_default("GODOT_OHOS_CACHE_DIR", std::string(cache) + "/godot-editor"); }
	}
	if (!ok) { return false; }
	const char *files = getenv("GODOT_OHOS_DATA_DIR");
	if (files && absolute_path(files)) { publish_development_kit_templates(files, runtime, notice); }
	else { notice = "No absolute GODOT_OHOS_DATA_DIR/XDG_CONFIG_HOME; template pointer left unchanged"; }
	return true;
}

bool configure_runtime_paths(const std::string &files, const std::string &cache,
		const std::string &runtime, const std::string &managed_mode, std::string &message, const std::string &temporary) {
	message.clear();
	if (managed_mode != "none" && managed_mode != "sdk") {
		message = "Unknown managed mode (expected none or sdk)";
		return false;
	}
	auto absolute_path = [](const std::string &path) {
		return !path.empty() && path[0] == '/' && path.find('\0') == std::string::npos;
	};
	const char *inherited_temporary = getenv("TMPDIR");
	const std::string temporary_root = !temporary.empty() ? temporary :
			(inherited_temporary && absolute_path(inherited_temporary) ? inherited_temporary : cache + "/tmp");
	if (!absolute_path(files) || !absolute_path(cache) || !absolute_path(temporary_root) || (managed_mode == "sdk" && !absolute_path(runtime))) {
		message = "Expected absolute sandbox paths (and runtime path in sdk mode)";
		return false;
	}
	std::error_code sandbox_error;
	for (const auto &directory : { files, cache, files + "/config", temporary_root }) {
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
	set("TMPDIR", temporary_root);
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
		register_dotnet_runtime_directories(dotnet);
		// The runtime keeps its default write-xor-execute mode: it maps a shared
		// memory object once for execution and once for writing, and
		// ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY is what allows the
		// executable mapping. If that mapping is ever refused again, setting
		// DOTNET_EnableWriteXorExecute=0 makes the runtime commit plain
		// read/write/execute pages instead.
	}
	if (access((runtime + "/GodotSharp/Api/Debug/GodotSharp.dll").c_str(), F_OK) != 0) {
		message = "The selected development kit's GodotSharp assemblies are missing";
		return false;
	}
	std::error_code error;
	for (const auto &directory : { files + "/Projects", files + "/config", files + "/nuget", files + "/dotnet-cli", temporary_root }) {
		std::filesystem::create_directories(directory, error);
		if (error) {
			// Name the path: a bare "File exists" does not say which entry blocked it.
			message = "Cannot prepare " + directory + ": " + error.message();
			return false;
		}
	}
	const std::string nuget_config = files + "/Projects/NuGet.Config";
	const std::string legacy_nuget_config = "<configuration><packageSources><clear/>"
			"<add key=\"godot-bundled\" value=\"%GODOT_NUGET_SOURCE%\"/>"
			"</packageSources></configuration>\n";
	const std::string default_nuget_config = "<configuration><packageSources><clear/>"
			"<add key=\"godot-bundled\" value=\"%GODOT_NUGET_SOURCE%\"/>"
			"<add key=\"nuget.org\" value=\"https://api.nuget.org/v3/index.json\" protocolVersion=\"3\"/>"
			"</packageSources></configuration>\n";
	const auto config_status = std::filesystem::symlink_status(nuget_config, error);
	if (error && error != std::errc::no_such_file_or_directory) {
		message = "Unable to inspect the project NuGet configuration: " + error.message();
		return false;
	}
	bool write_config = config_status.type() == std::filesystem::file_type::not_found;
	if (config_status.type() == std::filesystem::file_type::regular) {
		// Upgrade only the exact file written by the previous host. Preserve
		// custom configuration, formatting, BOMs and symbolic links unchanged.
		std::ifstream input(nuget_config, std::ios::binary);
		const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		write_config = input && contents == legacy_nuget_config;
	}
	if (write_config) {
		std::ofstream output(nuget_config, std::ios::binary);
		output << default_nuget_config;
		output.close();
		if (!output) {
			message = "Unable to prepare the project NuGet sources";
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
	set("GODOT_NUGET_SOURCE", runtime + "/GodotSharp/Tools/nupkgs");
	set("DOTNET_OHOS_TMPDIR", temporary_root);
	set("DOTNET_CLI_TELEMETRY_OPTOUT", "1");
	set("DOTNET_GENERATE_ASPNET_CERTIFICATE", "false");
	set("DOTNET_CLI_DO_NOT_USE_MSBUILD_SERVER", "1");
	set("DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE", "true");
	set("MSBUILDDISABLENODEREUSE", "1");
	set("UseSharedCompilation", "false");
	set("NuGetAudit", "false");
	if (!environment_ok) { return false; }
	std::string template_notice;
	publish_development_kit_templates(files, runtime, template_notice);
	message = template_notice;
	if (dotnet.empty()) {
		if (!message.empty()) { message += "; "; }
		message += "No installed .NET SDK found; C# tooling is unavailable";
	}
	return true;
}
