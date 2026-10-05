// Godot Engine contributors. SPDX-License-Identifier: MIT
// Include the production algorithm; do not emulate OS::execute with a shell.
#include "process_openharmony.h"

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/stat.h>

namespace {
int checks = 0;
const std::string out_bytes("out\0raw\n", 8);
const std::string err_bytes("err\0raw\n", 8);

bool write_all(int fd, const std::string &data) {
	size_t offset = 0;
	while (offset < data.size()) {
		ssize_t count = write(fd, data.data() + offset, data.size() - offset);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) return false;
		offset += size_t(count);
	}
	return true;
}

std::string encoded(const std::vector<std::string> &arguments) {
	std::string result;
	for (const auto &argument : arguments) {
		const uint32_t size = uint32_t(argument.size());
		for (int byte = 0; byte < 4; ++byte) result.push_back(char((size >> (8 * byte)) & 255));
		result += argument;
	}
	return result;
}

void check(bool condition, const std::string &name) {
	if (!condition) throw std::runtime_error(name);
	++checks;
	std::cout << "PASS: " << name << std::endl;
}

bool absent(const std::string &path) {
	struct stat info{};
	return lstat(path.c_str(), &info) < 0 && errno == ENOENT;
}

struct OwnedChild {
	pid_t pid = -1;
	~OwnedChild() {
		if (pid > 0) {
			// A subreaper owns this unreaped child; its PID cannot be reused.
			kill(pid, SIGKILL);
			while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
		}
	}
};

int child_mode(int argc, char **argv) {
	const std::string mode = argv[1];
	if (mode == "--child-argv") {
		std::vector<std::string> arguments;
		for (int i = 2; i < argc; ++i) arguments.emplace_back(argv[i]);
		return write_all(STDOUT_FILENO, encoded(arguments)) ? 0 : 71;
	}
	if (mode == "--child-io") {
		return write_all(STDOUT_FILENO, out_bytes) && write_all(STDERR_FILENO, err_bytes) ? 0 : 72;
	}
	if (mode == "--child-stdin-and-io") {
		char byte = 0;
		return read(STDIN_FILENO, &byte, 1) == 0 && write_all(STDOUT_FILENO, out_bytes) &&
				write_all(STDERR_FILENO, err_bytes) ? 0 : 81;
	}
	if (mode == "--child-tail") {
		return write_all(STDOUT_FILENO, "immediate stdout tail\n") &&
				write_all(STDERR_FILENO, "immediate stderr tail\n") ? 0 : 82;
	}
	if (mode == "--child-stdin-eof") {
		char byte = 0;
		const ssize_t size = read(STDIN_FILENO, &byte, 1);
		return size == 0 && write_all(STDOUT_FILENO, "stdin EOF\n") ? 0 : 73;
	}
	if (mode == "--child-exit") return 37;
	if (mode == "--child-signal") {
		kill(getpid(), SIGTERM);
		return 74;
	}
	if (mode == "--child-touch" && argc > 2) {
		const int fd = open(argv[2], O_CREAT | O_EXCL | O_WRONLY, 0600);
		if (fd >= 0) close(fd);
		return 75;
	}
	if (mode == "--child-flood") {
		for (int i = 0; i < 32; ++i) {
			if (!write_all(STDOUT_FILENO, std::string(65536, char('A' + i % 26))) ||
					!write_all(STDERR_FILENO, std::string(65536, char('a' + i % 26)))) return 76;
		}
		return 0;
	}
	if (mode == "--child-inherited-pipe") {
		pid_t descendant = fork();
		if (descendant < 0) return 77;
		if (descendant == 0) {
			// Hold inherited stdout/stderr open without producing data. The
			// fixture subreaper kills and reaps us immediately after execute.
			for (;;) pause();
		}
		return write_all(STDOUT_FILENO, "grandchild=" + std::to_string(descendant) + "\ndirect exited\n") ? 0 : 78;
	}
	if (mode == "--child-closed-fds" && argc == 3) {
		const int diagnostic = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
		if (diagnostic < 0) return 83;
		const int mask = std::atoi(argv[2]);
		for (int fd = 0; fd < 3; ++fd) {
			if (mask & (1 << fd)) close(fd);
		}
		auto result = OpenHarmonyProcess::execute(argv[0], { "--child-stdin-and-io" }, true, true);
		const bool valid = result.error == 0 && result.exit_code == 0 && result.output == out_bytes + err_bytes;
		if (!valid) write_all(diagnostic, "closed-fds mask=" + std::to_string(mask) + " result: error=" +
				std::to_string(result.error) + " exit=" + std::to_string(result.exit_code) +
				" output-bytes=" + std::to_string(result.output.size()) + "\n");
		close(diagnostic);
		return valid ? 0 : 79;
	}
	return 80;
}
} // namespace

int main(int argc, char **argv) {
	if (argc >= 2 && std::strncmp(argv[1], "--child-", 8) == 0) return child_mode(argc, argv);
	if (argc != 2) {
		std::cerr << "Usage: process <private-fixture-directory>" << std::endl;
		return 2;
	}
	const std::string self = argv[0];
	const std::string directory = argv[1];
	const std::string sentinel = directory + "/shell sentinel must not exist";
	try {
		check(absent(sentinel), "sentinel starts absent");
		const std::vector<std::string> literal{
			"", " ", "two words", "single'quote", "double\"quote", "$HOME",
			"$(touch '" + sentinel + "')", "`touch '" + sentinel + "'`", "; touch '" + sentinel + "';",
			"中文🙂", "tabs\tand\nnewlines", "\\backslash\\", "trailing ", "--not-an-option"
		};
		std::vector<std::string> arguments{ "--child-argv" };
		arguments.insert(arguments.end(), literal.begin(), literal.end());
		auto result = OpenHarmonyProcess::execute(self, arguments, true, true);
		check(result.error == 0 && result.exit_code == 0 && result.output == encoded(literal), "exact binary-framed argv; no shell expansion");
		check(absent(sentinel), "dollar/backtick/metacharacters never execute touch");
		const char *old_path = std::getenv("PATH");
		const bool had_path = old_path != nullptr;
		const std::string saved_path = old_path ? old_path : "";
		setenv("PATH", self.substr(0, self.find_last_of('/')).c_str(), 1);
		result = OpenHarmonyProcess::execute(self.substr(self.find_last_of('/') + 1), arguments, true, true);
		if (had_path) setenv("PATH", saved_path.c_str(), 1); else unsetenv("PATH");
		check(result.error == 0 && result.exit_code == 0 && result.output == encoded(literal), "posix_spawnp PATH lookup preserves quoted executable basename and argv");

		result = OpenHarmonyProcess::execute(self, { "--child-io" }, true, true);
		check(result.error == 0 && result.exit_code == 0 && result.output == out_bytes + err_bytes, "capture binary stdout and stderr in order");
		result = OpenHarmonyProcess::execute(self, { "--child-io" }, true, false);
		check(result.error == 0 && result.exit_code == 0 && result.output == out_bytes, "stderr=false silences stderr while preserving stdout");
		result = OpenHarmonyProcess::execute(self, { "--child-stdin-eof" }, true, true);
		check(result.error == 0 && result.exit_code == 0 && result.output == "stdin EOF\n", "captured child stdin is /dev/null");
		result = OpenHarmonyProcess::execute(self, { "--child-exit" }, true, true);
		check(result.error == 0 && result.exit_code == 37 && result.output.empty(), "preserve nonzero exit status");
		result = OpenHarmonyProcess::execute(self, { "--child-exit" }, false, false);
		check(result.error == 0 && result.exit_code == 37 && result.output.empty(), "noncaptured direct execution preserves status");
		result = OpenHarmonyProcess::execute(self, { "--child-signal" }, true, true);
		check(result.error == 0 && result.exit_code == 128 + SIGTERM, "signaled child reports 128 plus signal");

		result = OpenHarmonyProcess::execute(directory + "/missing executable", {}, true, true);
		check(result.error == ENOENT && result.exit_code == -1 && result.output.empty(), "nonexistent executable returns ENOENT");
		result = OpenHarmonyProcess::execute("", {}, true, true);
		check(result.error == EINVAL && result.exit_code == -1, "empty executable rejected");
		const std::string nul_command = self + std::string("\0ignored", 8);
		result = OpenHarmonyProcess::execute(nul_command, { "--child-touch", sentinel }, true, true);
		check(result.error == EINVAL && result.exit_code == -1 && absent(sentinel), "NUL in command rejected before spawn");
		result = OpenHarmonyProcess::execute(self, { "--child-touch", sentinel, std::string("a\0b", 3) }, true, true);
		check(result.error == EINVAL && result.exit_code == -1 && absent(sentinel), "NUL in argv rejected before spawn");

		result = OpenHarmonyProcess::execute(self, { "--child-flood" }, true, true);
		std::string expected;
		for (int i = 0; i < 32; ++i) {
			expected.append(65536, char('A' + i % 26));
			expected.append(65536, char('a' + i % 26));
		}
		check(result.error == 0 && result.exit_code == 0 && result.output == expected, "4 MiB alternating stdout/stderr exceeds pipe capacity without deadlock or truncation");
		result = OpenHarmonyProcess::execute(self, { "--child-io" }, true, true);
		check(result.error == 0 && result.exit_code == 0 && result.output == out_bytes + err_bytes, "execute remains usable after flood and spawn errors");

		const char *subreaper = std::getenv("GODOT_PROCESS_TEST_SUBREAPER");
		if (subreaper && std::strcmp(subreaper, "1") == 0 && prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0) {
			const auto start = std::chrono::steady_clock::now();
			result = OpenHarmonyProcess::execute(self, { "--child-inherited-pipe" }, true, true);
			const auto elapsed = std::chrono::steady_clock::now() - start;
			OwnedChild descendant;
			if (result.output.rfind("grandchild=", 0) == 0) {
				descendant.pid = pid_t(std::stol(result.output.substr(11)));
			}
			check(result.error == 0 && result.exit_code == 0 && descendant.pid > 0 &&
					result.output.find("\ndirect exited\n") != std::string::npos,
					"direct-child output and status survive descendant holding pipe open");
			check(elapsed < std::chrono::seconds(3), "execute does not wait for inherited pipe after direct child exit");
		} else {
			std::cout << "SKIP: inherited-pipe case (subreaper unavailable; do not leave descendants)" << std::endl;
		}

		for (int mask = 1; mask < 8; ++mask) {
			result = OpenHarmonyProcess::execute(self, { "--child-closed-fds", std::to_string(mask) }, true, true);
			if (result.exit_code != 0) std::cerr << result.output;
			check(result.error == 0 && result.exit_code == 0, "low-FD collision: closed stdio mask " + std::to_string(mask));
		}
		for (int attempt = 0; attempt < 32; ++attempt) {
			result = OpenHarmonyProcess::execute(self, { "--child-tail" }, true, true);
			check(result.error == 0 && result.exit_code == 0 &&
					result.output == "immediate stdout tail\nimmediate stderr tail\n", "short-exit final drain preserves tail " + std::to_string(attempt));
		}
		check(absent(sentinel), "all literal/invalid argv cases leave sentinel absent");
		std::cout << "OHOS_PROCESS_EXECUTION_PASS checks=" << checks << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "FAIL: " << error.what() << std::endl;
		return 1;
	}
}
