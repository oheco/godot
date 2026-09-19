// Godot Engine contributors. SPDX-License-Identifier: MIT
#include "crash_handler_openharmony.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

namespace {
constexpr int MAX_FRAMES = 64;
int crash_fd = -1;

void write_text(int p_fd, const char *p_text) {
	if (p_fd < 0 || p_text == nullptr) {
		return;
	}
	size_t remaining = strlen(p_text);
	while (remaining > 0) {
		ssize_t written = write(p_fd, p_text, remaining);
		if (written <= 0) {
			return;
		}
		p_text += written;
		remaining -= size_t(written);
	}
}

// The crash path must not allocate: build every line in a fixed buffer.
struct Line {
	char buffer[768];
	size_t used = 0;

	void text(const char *p_text) {
		size_t length = strlen(p_text);
		if (length > sizeof(buffer) - 1 - used) {
			length = sizeof(buffer) - 1 - used;
		}
		memcpy(buffer + used, p_text, length);
		used += length;
	}

	void hex(uintptr_t p_value) {
		static const char digits[] = "0123456789abcdef";
		char reversed[2 * sizeof(uintptr_t) + 1];
		int length = 0;
		do {
			reversed[length++] = digits[p_value & 0xf];
			p_value >>= 4;
		} while (p_value != 0);
		text("0x");
		while (length > 0 && used < sizeof(buffer) - 1) {
			buffer[used++] = reversed[--length];
		}
	}

	void decimal(int64_t p_value) {
		char reversed[24];
		int length = 0;
		bool negative = p_value < 0;
		uint64_t value = negative ? uint64_t(-p_value) : uint64_t(p_value);
		do {
			reversed[length++] = char('0' + value % 10);
			value /= 10;
		} while (value != 0);
		if (negative) {
			text("-");
		}
		while (length > 0 && used < sizeof(buffer) - 1) {
			buffer[used++] = reversed[--length];
		}
	}

	const char *c_str() {
		buffer[used] = '\0';
		return buffer;
	}
};

void dump_backtrace(int p_fd, int p_signal, siginfo_t *p_info) {
	Line header;
	header.text("=== Godot (OpenHarmony) fatal signal === signal=");
	header.decimal(p_signal);
	if (p_info != nullptr) {
		header.text(" address=");
		header.hex(reinterpret_cast<uintptr_t>(p_info->si_addr));
	}
	header.text("\n");
	write_text(p_fd, header.c_str());

	void *frames[MAX_FRAMES];
	int count = backtrace(frames, MAX_FRAMES);
	for (int i = 0; i < count; i++) {
		Line line;
		line.text("[");
		line.decimal(i);
		line.text("] ");
		line.hex(reinterpret_cast<uintptr_t>(frames[i]));
		Dl_info info;
		if (dladdr(frames[i], &info) != 0 && info.dli_fname != nullptr) {
			const char *slash = strrchr(info.dli_fname, '/');
			line.text(" ");
			line.text(slash != nullptr ? slash + 1 : info.dli_fname);
			line.text("+");
			// Symbol offset inside the module: resolve it afterwards with
			// llvm-addr2line against the unstripped engine library.
			line.hex(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(info.dli_fbase));
			if (info.dli_sname != nullptr) {
				line.text(" ");
				line.text(info.dli_sname);
			}
		}
		line.text("\n");
		write_text(p_fd, line.c_str());
	}
	write_text(p_fd, "=== end of OpenHarmony crash backtrace ===\n");
}

void handle_crash(int p_signal, siginfo_t *p_info, void *) {
	if (crash_fd >= 0) {
		dump_backtrace(crash_fd, p_signal, p_info);
		fsync(crash_fd);
	}
	dump_backtrace(STDERR_FILENO, p_signal, p_info);
	// Restore the default action and re-raise, so the platform still records and
	// terminates the process the way it would without this handler.
	signal(p_signal, SIG_DFL);
	raise(p_signal);
}
} // namespace

void ohos_crash_handler_install(const char *p_log_path) {
	if (crash_fd < 0 && p_log_path != nullptr && p_log_path[0] != '\0') {
		crash_fd = open(p_log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
	}
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = handle_crash;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	const int signals[] = { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL };
	for (int signal_number : signals) {
		sigaction(signal_number, &action, nullptr);
	}
}
