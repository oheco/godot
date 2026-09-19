// Godot Engine contributors. SPDX-License-Identifier: MIT
#include "runtime_probe.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

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

std::string probe_sandbox(const std::string &dotnet_root, const std::string &cache_dir) {
	std::string report;
	std::ifstream attributes("/proc/self/attr/current");
	std::string domain;
	std::getline(attributes, domain);
	report += "security_domain=" + domain + "\n";

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
