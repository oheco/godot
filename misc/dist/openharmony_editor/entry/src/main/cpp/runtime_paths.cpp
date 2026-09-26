// Godot Engine contributors. SPDX-License-Identifier: MIT
#include "runtime_paths.h"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>

namespace {
std::string oheco_root() {
	const char *configured = getenv("OHECO_ROOT");
	if (configured != nullptr && configured[0] != '\0') {
		return configured;
	}
	return "/storage/Users/currentUser/.oheco";
}

// Returns the single versioned child of dir joined with suffix, or empty.
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
		found = dir + "/" + entry->d_name + suffix;
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
		std::string mutable_directory = directory;
		add_path(mutable_directory.data());
	}
	dlclose(libc);
}
