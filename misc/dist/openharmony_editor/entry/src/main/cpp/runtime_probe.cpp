// Godot Engine contributors. SPDX-License-Identifier: MIT
#include "runtime_probe.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <sys/stat.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

extern char **environ;

std::string check_dotnet_sdk(const std::string &executable, const std::string &report) {
	int output = open(report.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (output < 0) {
		return "Cannot create .NET diagnostic report: " + std::string(strerror(errno));
	}
	if (output < 3) {
		int duplicate = fcntl(output, F_DUPFD_CLOEXEC, 3);
		close(output);
		if (duplicate < 0) {
			return "Cannot reserve .NET diagnostic descriptor";
		}
		output = duplicate;
	}
	std::string domain;
	std::ifstream attributes("/proc/self/attr/current");
	std::getline(attributes, domain);
	dprintf(output, "security_domain=%s\ncommand=%s --version\n", domain.c_str(), executable.c_str());
	posix_spawn_file_actions_t actions;
	int error = posix_spawn_file_actions_init(&actions);
	bool initialized = error == 0;
	if (!error) {
		error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	}
	if (!error) {
		error = posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
	}
	if (!error) {
		error = posix_spawn_file_actions_adddup2(&actions, output, STDERR_FILENO);
	}
	pid_t child = -1;
	char option[] = "--version";
	char *args[] = { const_cast<char *>(executable.c_str()), option, nullptr };
	if (!error) {
		error = posix_spawn(&child, executable.c_str(), &actions, nullptr, args, environ);
	}
	if (initialized) {
		posix_spawn_file_actions_destroy(&actions);
	}
	if (error) {
		dprintf(output, "spawn_errno=%d (%s)\n", error, strerror(error));
		close(output);
		return ".NET SDK cannot start: " + std::string(strerror(error));
	}
	int status = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	for (;;) {
		pid_t result = waitpid(child, &status, WNOHANG);
		if (result == child) {
			break;
		}
		if (result < 0 && errno != EINTR) {
			error = errno;
			dprintf(output, "wait_errno=%d (%s)\n", error, strerror(error));
			close(output);
			return "Cannot observe .NET SDK: " + std::string(strerror(error));
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			kill(child, SIGKILL);
			while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
			}
			dprintf(output, "timeout_seconds=60\n");
			close(output);
			return ".NET SDK startup timed out";
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	dprintf(output, "exit_code=%d signal=%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1,
			WIFSIGNALED(status) ? WTERMSIG(status) : 0);
	close(output);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		return ".NET SDK failed; see its diagnostic report";
	}
	return {};
}

namespace {
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

std::string newest_report(const std::string &cache_dir) {
	DIR *handle = opendir(cache_dir.c_str());
	if (handle == nullptr) {
		return {};
	}
	std::string newest;
	while (dirent *entry = readdir(handle)) {
		const std::string name = entry->d_name;
		if (name.rfind("godot-dotnet-startup-", 0) == 0 && name.size() > newest.size()) {
			newest = name;
		}
	}
	closedir(handle);
	if (newest.empty()) {
		return {};
	}
	std::ifstream input(cache_dir + "/" + newest);
	std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	return "report " + newest + ":\n" + text;
}
} // namespace

namespace {
std::string oheco_root() {
	const char *configured = getenv("OHECO_ROOT");
	if (configured != nullptr && configured[0] != '\0') {
		return configured;
	}
	return "/storage/Users/currentUser/.oheco";
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

std::string probe_sandbox(const std::string &dotnet_root, const std::string &cache_dir) {
	std::string report;
	std::ifstream attributes("/proc/self/attr/current");
	std::string domain;
	std::getline(attributes, domain);
	report += "security_domain=" + domain + "\n";
	report += "dotnet_root=" + (dotnet_root.empty() ? std::string("<none>") : dotnet_root) + "\n";
	report += std::string("dotnet_executable=") +
			(access((dotnet_root + "/dotnet").c_str(), F_OK) == 0 ? "present" : "missing") + "\n";

	const std::string hostfxr = versioned_entry(dotnet_root + "/host/fxr", "/libhostfxr.so");
	report += "hostfxr=" + (hostfxr.empty() ? std::string("<missing>") : hostfxr) + "\n";
	if (!hostfxr.empty()) {
		void *handle = dlopen(hostfxr.c_str(), RTLD_LAZY | RTLD_LOCAL);
		report += std::string("dlopen(hostfxr)=") + (handle != nullptr ? "ok" : dlerror()) + "\n";
		if (handle != nullptr) {
			void *symbol = dlsym(handle, "hostfxr_main_startupinfo");
			report += std::string("dlsym(hostfxr_main_startupinfo)=") + (symbol != nullptr ? "ok" : dlerror()) + "\n";
		}
	}

	const std::string coreclr = versioned_entry(dotnet_root + "/shared/Microsoft.NETCore.App", "/libcoreclr.so");
	report += "coreclr=" + (coreclr.empty() ? std::string("<missing>") : coreclr) + "\n";
	if (!coreclr.empty()) {
		void *handle = dlopen(coreclr.c_str(), RTLD_LAZY | RTLD_LOCAL);
		report += std::string("dlopen(coreclr)=") + (handle != nullptr ? "ok" : dlerror()) + "\n";
	}

	// Reading the runtime and mapping it are separate capabilities: report both,
	// because a read denial and an execute/mmap denial need different fixes.
	if (!hostfxr.empty()) {
		const int runtime_fd = open(hostfxr.c_str(), O_RDONLY | O_CLOEXEC);
		if (runtime_fd < 0) {
			report += std::string("read(hostfxr)=failed: ") + strerror(errno) + "\n";
		} else {
			unsigned char magic[4] = {};
			const ssize_t got = read(runtime_fd, magic, sizeof(magic));
			close(runtime_fd);
			char text[16] = {};
			snprintf(text, sizeof(text), "%02x%02x%02x%02x", magic[0], magic[1], magic[2], magic[3]);
			report += "read(hostfxr)=" + std::to_string(got) + " bytes, magic=" + text + "\n";
		}
	}

	// Control: can this security domain exec anything at all?
	{
		pid_t child = -1;
		char option[] = "-c";
		char command[] = "exit 0";
		char *args[] = { const_cast<char *>("/system/bin/sh"), option, command, nullptr };
		const int error = posix_spawn(&child, "/system/bin/sh", nullptr, nullptr, args, environ);
		if (error != 0) {
			report += std::string("exec(/system/bin/sh)=failed: ") + strerror(error) + "\n";
		} else {
			int status = 0;
			waitpid(child, &status, 0);
			report += "exec(/system/bin/sh)=ok exit=" + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + "\n";
		}
	}

	// Control: the same library copied into the application's own sandbox, which
	// separates a location policy from a per-file policy.
	if (!cache_dir.empty() && !hostfxr.empty()) {
		const std::string copy = cache_dir + "/libhostfxr-probe.so";
		std::ifstream source(hostfxr, std::ios::binary);
		std::ofstream destination(copy, std::ios::binary | std::ios::trunc);
		destination << source.rdbuf();
		destination.close();
		const bool copied = source.good() || source.eof();
		void *handle = copied ? dlopen(copy.c_str(), RTLD_LAZY | RTLD_LOCAL) : nullptr;
		report += std::string("dlopen(app-cache copy)=") +
				(handle != nullptr ? "ok" : (copied ? dlerror() : "copy failed")) + "\n";
	}

	void *child = dlopen("libchild_process.so", RTLD_LAZY | RTLD_LOCAL);
	report += std::string("dlopen(libchild_process.so)=") + (child != nullptr ? "ok" : dlerror()) + "\n";
	if (child != nullptr) {
		auto supported = reinterpret_cast<bool (*)()>(dlsym(child, "OH_Ability_IsNativeChildProcessSupported"));
		auto start = dlsym(child, "OH_Ability_StartNativeChildProcess");
		report += std::string("dlsym(OH_Ability_IsNativeChildProcessSupported)=") +
				(supported != nullptr ? "ok" : "missing") + "\n";
		report += std::string("dlsym(OH_Ability_StartNativeChildProcess)=") + (start != nullptr ? "ok" : "missing") + "\n";
		if (supported != nullptr) {
			report += std::string("native_child_process_supported=") + (supported() ? "true" : "false") + "\n";
		}
	}

	const std::string previous = newest_report(cache_dir);
	if (!previous.empty()) {
		report += previous + "\n";
	}
	return report;
}
