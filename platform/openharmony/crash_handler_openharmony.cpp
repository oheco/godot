// Godot Engine contributors. SPDX-License-Identifier: MIT
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "crash_handler_openharmony.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

namespace {
constexpr int MAX_FRAMES = 64;
int crash_fd = -1;
char alternate_stack[32 * 1024];

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
	char buffer[1024];
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

// Prints one address and, when it belongs to a loaded module, the offset inside
// that module plus the closest exported symbol. The offsets are resolved
// afterwards with llvm-addr2line against the unstripped engine library.
void write_address(int p_fd, const char *p_label, uintptr_t p_address) {
	Line line;
	line.text(p_label);
	line.text("=");
	line.hex(p_address);
	Dl_info info;
	if (p_address != 0 && dladdr(reinterpret_cast<void *>(p_address), &info) != 0 && info.dli_fname != nullptr) {
		const char *slash = strrchr(info.dli_fname, '/');
		line.text(" ");
		line.text(slash != nullptr ? slash + 1 : info.dli_fname);
		line.text("+");
		line.hex(p_address - reinterpret_cast<uintptr_t>(info.dli_fbase));
		if (info.dli_sname != nullptr) {
			line.text(" ");
			line.text(info.dli_sname);
		}
	}
	line.text("\n");
	write_text(p_fd, line.c_str());
}

void dump_crash(int p_fd, int p_signal, siginfo_t *p_info, void *p_context) {
	Line header;
	header.text("=== Godot (OpenHarmony) fatal signal === signal=");
	header.decimal(p_signal);
	if (p_info != nullptr) {
		header.text(" code=");
		header.decimal(p_info->si_code);
		header.text(" address=");
		header.hex(reinterpret_cast<uintptr_t>(p_info->si_addr));
	}
	header.text("\n");
	write_text(p_fd, header.c_str());

	ucontext_t *context = static_cast<ucontext_t *>(p_context);
	if (context != nullptr) {
		const uintptr_t pc = context->uc_mcontext.pc;
		const uintptr_t lr = context->uc_mcontext.regs[30];
		const uintptr_t sp = context->uc_mcontext.sp;
		const uintptr_t fp = context->uc_mcontext.regs[29];
		write_address(p_fd, "pc", pc);
		write_address(p_fd, "lr", lr);
		write_address(p_fd, "sp", sp);
		write_address(p_fd, "fp", fp);
		for (int i = 0; i < 31; i++) {
			Line line;
			line.text("x");
			line.decimal(i);
			line.text("=");
			line.hex(context->uc_mcontext.regs[i]);
			line.text(i + 1 == 31 ? "\n" : " ");
			write_text(p_fd, line.c_str());
		}
		// Best effort: the engine is built without frame pointers, so only trust
		// the chain while it stays inside the current stack.
		uintptr_t frame = fp;
		for (int i = 0; i < 32 && frame != 0; i++) {
			if ((frame & 7) != 0 || frame < sp || frame > sp + (1u << 20)) {
				break;
			}
			const uintptr_t next = *reinterpret_cast<const uintptr_t *>(frame);
			const uintptr_t return_address = *reinterpret_cast<const uintptr_t *>(frame + 8);
			Line line;
			line.text("frame[");
			line.decimal(i);
			line.text("] return=");
			line.hex(return_address);
			line.text(" next=");
			line.hex(next);
			line.text("\n");
			write_text(p_fd, line.c_str());
			if (next <= frame) {
				break;
			}
			frame = next;
		}
	}

	// backtrace() needs frame pointers, which this build omits; keep it as a
	// bonus for builds that have them.
	void *frames[MAX_FRAMES];
	int count = backtrace(frames, MAX_FRAMES);
	for (int i = 0; i < count; i++) {
		Line label;
		label.text("backtrace[");
		label.decimal(i);
		label.text("]");
		write_address(p_fd, label.c_str(), reinterpret_cast<uintptr_t>(frames[i]));
	}
	write_text(p_fd, "=== end of OpenHarmony crash backtrace ===\n");
}

void handle_crash(int p_signal, siginfo_t *p_info, void *p_context) {
	if (crash_fd >= 0) {
		dump_crash(crash_fd, p_signal, p_info, p_context);
		fsync(crash_fd);
	}
	dump_crash(STDERR_FILENO, p_signal, p_info, p_context);
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
	// A crash caused by a stack overflow can only be reported on its own stack.
	stack_t stack;
	memset(&stack, 0, sizeof(stack));
	stack.ss_sp = alternate_stack;
	stack.ss_size = sizeof(alternate_stack);
	sigaltstack(&stack, nullptr);
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = handle_crash;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	const int signals[] = { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL };
	for (int signal_number : signals) {
		sigaction(signal_number, &action, nullptr);
	}
}
