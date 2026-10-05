// Godot Engine contributors. SPDX-License-Identifier: MIT
#ifndef OPENHARMONY_BROKER_CLIENT_H
#define OPENHARMONY_BROKER_CLIENT_H

#include "../../../thirdparty/oheco-broker/oheco_broker.h"

#include <chrono>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <vector>

namespace OpenHarmonyBroker {
struct Result {
	int error = 0; // ob_error; a remote nonzero exit is reported in exit_code.
	int exit_code = -1;
	std::string output;
	std::string diagnostic;
};

namespace Detail {
using Clock = std::chrono::steady_clock;
inline std::string diagnostic_text(const ob_diagnostic &p_diagnostic) {
	return std::string(ob_error_name(p_diagnostic.code)) + " [" + p_diagnostic.stage + "]: " + p_diagnostic.message;
}
struct Start {
	std::string endpoint;
	std::string command;
	std::vector<std::string> args;
	std::mutex mutex;
	std::condition_variable changed;
	bool done = false;
	bool abandoned = false;
	ob_process *process = nullptr;
	ob_error error = OB_OK;
	ob_diagnostic diagnostic{};
};
inline void *start(void *p_context) {
	std::shared_ptr<Start> state = *static_cast<std::shared_ptr<Start> *>(p_context);
	delete static_cast<std::shared_ptr<Start> *>(p_context);
	std::vector<const char *> args;
	for (const auto &arg : state->args) {
		args.push_back(arg.c_str());
	}
	ob_options options{};
	options.executable = state->command.c_str();
	options.args = args.data();
	options.argc = args.size();
	options.stdin_enabled = 0;
	ob_process *process = nullptr;
	ob_diagnostic diagnostic{};
	const ob_error error = ob_start(state->endpoint.empty() ? nullptr : state->endpoint.c_str(), &options, &process, &diagnostic);
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->error = error;
		state->diagnostic = diagnostic;
		if (!state->abandoned) {
			state->process = process;
			process = nullptr;
		}
		state->done = true;
	}
	// A late START is never retried or left managed without an owner.
	ob_release(process);
	state->changed.notify_one();
	return nullptr;
}
} // namespace Detail

// Each call sends exactly one managed START using direct executable/argv. Empty
// endpoint selects the SDK default ($HOME/.oheco/broker/endpoint). No shell/retry.
// timeout_ms is a positive, monotonic total budget, including discovery/START.
// Output (including discarded stderr) has a 64 MiB total cap and is raw bytes.
// The command has no stdin. Shared project HAPs are passed as paths in argv;
// HDC performs its own device-side transfer during installation.
// Errors cancel the managed request; the original SDK owns transport cleanup.
inline Result execute(const std::string &p_endpoint_file, const std::string &p_command,
		const std::vector<std::string> &p_args, bool p_read_stderr = true,
		int p_timeout_ms = 120000) {
	Result result;
	const auto end = Detail::Clock::now() + std::chrono::milliseconds(p_timeout_ms > 0 ? p_timeout_ms : 0);
	bool embedded_nul = p_endpoint_file.find('\0') != std::string::npos || p_command.find('\0') != std::string::npos;
	for (const auto &arg : p_args) {
		embedded_nul = embedded_nul || arg.find('\0') != std::string::npos;
	}
	// C strings cannot represent embedded NUL; all other strict UTF-8 and START
	// size validation is delegated to the unchanged SDK before connecting.
	if (p_timeout_ms <= 0 || embedded_nul) {
		result.error = OB_INVALID_ARGUMENT;
		result.diagnostic = "A positive timeout and strings without embedded NUL are required.";
		return result;
	}
	auto starting = std::make_shared<Detail::Start>();
	starting->endpoint = p_endpoint_file;
	starting->command = p_command;
	starting->args = p_args;
	auto *start_context = new std::shared_ptr<Detail::Start>(starting);
	pthread_t start_thread;
	if (pthread_create(&start_thread, nullptr, Detail::start, start_context) != 0) {
		delete start_context;
		result.error = OB_IO;
		result.diagnostic = "Cannot create broker startup thread.";
		return result;
	}
	pthread_detach(start_thread);
	std::shared_ptr<ob_process> process;
	{
		std::unique_lock<std::mutex> lock(starting->mutex);
		if (!starting->changed.wait_until(lock, end, [&] { return starting->done; }) || Detail::Clock::now() >= end) {
			starting->abandoned = true;
			// The worker may already have published its handle at this deadline.
			ob_release(starting->process);
			starting->process = nullptr;
			result.error = OB_TIMEOUT;
			result.diagnostic = "Broker total deadline expired during START; outcome may be unknown; no retry; a late managed handle will be released.";
			return result;
		}
		if (starting->error != OB_OK) {
			result.error = starting->error;
			result.diagnostic = Detail::diagnostic_text(starting->diagnostic);
			return result;
		}
		process = std::shared_ptr<ob_process>(starting->process, ob_release);
		starting->process = nullptr;
	}
	constexpr size_t MAX_OUTPUT = 64u * 1024u * 1024u;
	size_t received = 0;
	bool exited = false;
	while (!exited && result.error == OB_OK) {
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end - Detail::Clock::now()).count();
		if (remaining <= 0) {
			result.error = OB_TIMEOUT;
			result.diagnostic = "Broker total deadline expired; managed command cancelled.";
			break;
		}
		ob_event event{};
		ob_diagnostic diagnostic{};
		const ob_error error = ob_read_event(process.get(), static_cast<int>(remaining > 50 ? 50 : remaining), &event, &diagnostic);
		if (error == OB_TIMEOUT) {
			continue; // Continue this frame, never reconnect or send a new START.
		}
		if (error != OB_OK) {
			result.error = error;
			result.diagnostic = Detail::diagnostic_text(diagnostic);
			break;
		}
		if (event.type == OB_EXIT) {
			exited = true;
			if (event.result.reason == 2) {
				result.error = OB_IO;
				result.diagnostic = "Broker reported command cancellation.";
			} else if (event.result.reason == 1) {
				if (event.result.signal > static_cast<unsigned>(std::numeric_limits<int>::max() - 128)) {
					result.error = OB_PROTOCOL;
					result.diagnostic = "Broker returned an unrepresentable exit signal.";
				} else {
					result.exit_code = 128 + static_cast<int>(event.result.signal);
					result.diagnostic = "Command terminated by signal " + std::to_string(event.result.signal) + ".";
				}
			} else {
				result.exit_code = event.result.exit_code;
				if (result.exit_code != 0) {
					result.diagnostic = "Command exited with code " + std::to_string(result.exit_code) + ".";
				}
			}
		} else {
			if (event.size > MAX_OUTPUT - received) {
				result.error = OB_LIMIT;
				result.diagnostic = "Broker output exceeded the 64 MiB total limit.";
				break;
			}
			received += event.size;
			if (event.type == OB_STDOUT || p_read_stderr) {
				result.output.append(reinterpret_cast<const char *>(event.data), event.size);
			}
		}
	}
	if (!exited) {
		// CANCEL is bounded by the original SDK transport deadline.
		ob_cancel(process.get(), nullptr);
	}
	// Last owner releases; on errors the managed request was also cancelled.
	return result;
}
} // namespace OpenHarmonyBroker
#endif // OPENHARMONY_BROKER_CLIENT_H
