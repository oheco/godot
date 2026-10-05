// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <vector>

extern char **environ;

namespace OpenHarmonyProcess {
struct Result {
	int error = 0;
	int exit_code = -1;
	std::string output;
};

// Capture exec's bytes without a shell or post-fork allocation in a threaded
// UIAbility. A descendant holding stdout open must not hide the direct child's
// exit forever. Background services belong to create_process, not execute.
inline Result execute(const std::string &p_command, const std::vector<std::string> &p_arguments, bool p_capture, bool p_stderr) {
	Result result;
	if (p_command.empty() || p_command.find('\0') != std::string::npos) {
		result.error = EINVAL;
		return result;
	}
	for (const std::string &argument : p_arguments) {
		if (argument.find('\0') != std::string::npos) {
			result.error = EINVAL;
			return result;
		}
	}
	std::vector<char *> argv{ const_cast<char *>(p_command.c_str()) };
	for (const std::string &argument : p_arguments) {
		argv.push_back(const_cast<char *>(argument.c_str()));
	}
	argv.push_back(nullptr);
	int pipes[2] = { -1, -1 };
	posix_spawn_file_actions_t actions;
	int error = posix_spawn_file_actions_init(&actions);
	if (error != 0) {
		result.error = error;
		return result;
	}
	if (p_capture) {
		if (pipe2(pipes, O_CLOEXEC) != 0) {
			result.error = errno;
			posix_spawn_file_actions_destroy(&actions);
			return result;
		}
		// UIAbility hosts need not have all standard descriptors open. Keep our
		// temporary pipe descriptors away from dup/open/close targets 0..2.
		for (int &fd : pipes) {
			if (fd < 3) {
				const int replacement = fcntl(fd, F_DUPFD_CLOEXEC, 3);
				if (replacement < 0) {
					result.error = errno;
					close(pipes[0]);
					close(pipes[1]);
					posix_spawn_file_actions_destroy(&actions);
					return result;
				}
				close(fd);
				fd = replacement;
			}
		}
		error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
		if (error == 0) {
			error = posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
		}
		if (error == 0) {
			error = p_stderr ? posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO) : posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
		}
		if (error == 0) {
			error = posix_spawn_file_actions_addclose(&actions, pipes[0]);
		}
		if (error == 0) {
			error = posix_spawn_file_actions_addclose(&actions, pipes[1]);
		}
		if (error == 0 && fcntl(pipes[0], F_SETFL, O_NONBLOCK) < 0) {
			error = errno;
		}
	}
	pid_t child = -1;
	if (error == 0) {
		error = posix_spawnp(&child, p_command.c_str(), &actions, nullptr, argv.data(), environ);
	}
	posix_spawn_file_actions_destroy(&actions);
	if (pipes[1] >= 0) {
		close(pipes[1]);
	}
	if (error != 0) {
		if (pipes[0] >= 0) {
			close(pipes[0]);
		}
		result.error = error;
		return result;
	}
	if (p_capture) {
		bool eof = false;
		bool exited = false;
		while (!eof) {
			char buffer[65536];
			for (;;) {
				const ssize_t count = read(pipes[0], buffer, sizeof(buffer));
				if (count > 0) {
					// A compiler/tool must not allocate unbounded Editor memory.
					if (result.output.size() + size_t(count) > 64 * 1024 * 1024) {
						result.error = EFBIG;
						eof = true;
						break;
					}
					result.output.append(buffer, size_t(count));
				} else if (count == 0) {
					eof = true;
					break;
				} else if (errno == EINTR) {
					continue;
				} else {
					if (errno != EAGAIN && errno != EWOULDBLOCK) {
						result.error = errno;
						eof = true;
					}
					break;
				}
			}
			if (exited) {
				break; // Final drain after observing exit closes the write race.
			}
			siginfo_t info{};
			if (waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == child) {
				exited = true;
				// Drain once more, then stop waiting for inherited pipe descriptors.
				if (!eof) {
					continue;
				}
			}
			if (!eof) {
				pollfd fd{ pipes[0], POLLIN | POLLHUP, 0 };
				if (poll(&fd, 1, 100) < 0 && errno != EINTR) {
					result.error = errno;
					break;
				}
			}
		}
		close(pipes[0]);
	}
	if (result.error != 0) {
		kill(child, SIGKILL); // Only our unreaped direct child.
	}
	int status = 0;
	while (waitpid(child, &status, 0) < 0) {
		if (errno == EINTR) {
			continue;
		}
		result.error = errno;
		return result;
	}
	result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
	return result;
}
} // namespace OpenHarmonyProcess
