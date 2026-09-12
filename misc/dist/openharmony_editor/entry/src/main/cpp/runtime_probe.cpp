// Godot Engine contributors. SPDX-License-Identifier: MIT
#include "runtime_probe.h"

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
