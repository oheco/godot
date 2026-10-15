// Godot Engine contributors. SPDX-License-Identifier: MIT
// The runner supplies the unchanged production HostWindowModeState class.
// This fixture exercises its public API only; it never calls the window SDK.
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>

namespace GodotOpenHarmony {
#include "window_mode_state.inc"
} // namespace GodotOpenHarmony

namespace {
using GodotOpenHarmony::HostWindowModeState;
int checks = 0;
int failures = 0;

void check(bool condition, const char *message) {
	++checks;
	if (!condition) {
		++failures;
		std::fprintf(stderr, "FAIL %s\n", message);
	}
}

void unknown_initial_state_applies_windowed() {
	HostWindowModeState state;
	uint32_t id = 0;
	check(state.get_mode() == 0, "query has a safe default before an actual snapshot arrives");
	check(state.request(0, id) && id != 0, "unknown initial state must apply WINDOWED instead of assuming it is already actual");
	state.complete(id, 0);
	check(state.request(0, id) && id == 0, "known WINDOWED is deduplicated after actual acknowledgement");
}

void normalization_and_invalid_requests() {
	const int32_t expected[] = { 0, 1, 2, 3, 3 };
	for (int32_t mode = 0; mode <= 4; ++mode) {
		check(HostWindowModeState::normalize(mode) == expected[mode], "normalize public mode IDs, including exclusive fullscreen");
	}
	HostWindowModeState state;
	state.observe(2);
	for (const int32_t mode : { -1, 5, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max() }) {
		uint32_t id = 0x12345678;
		check(HostWindowModeState::normalize(mode) == -1, "invalid modes normalize to unknown");
		check(!state.request(mode, id), "reject invalid request");
		check(id == 0x12345678 && state.get_mode() == 2, "invalid request changes neither output nor state");
	}
	uint32_t pending = 0;
	check(state.request(3, pending) && pending != 0, "valid request records a nonzero serial");
	uint32_t invalid = 73;
	check(!state.request(-1, invalid) && invalid == 73 && state.get_mode() == 3, "invalid request preserves pending desired mode");
	state.complete(pending, -1);
	check(state.get_mode() == 2, "invalid request also preserves the latest pending serial");
}

void observed_actual_and_const_query() {
	HostWindowModeState state;
	const HostWindowModeState &snapshot = state;
	for (const int32_t mode : { 0, 1, 2, 3, 4 }) {
		state.observe(mode);
		check(snapshot.get_mode() == (mode == 4 ? 3 : mode), "const query returns normalized observed actual");
	}
	for (const int32_t invalid : { -1, -2, 5, std::numeric_limits<int32_t>::max() }) {
		state.observe(invalid);
		check(snapshot.get_mode() == 3, "invalid observations retain actual");
	}
}

void same_effective_mode_and_exclusive_fallback() {
	HostWindowModeState state;
	state.observe(0);
	uint32_t id = 91;
	check(state.request(0, id) && id == 0, "same actual mode needs no SDK request");
	check(state.request(4, id) && id != 0 && state.get_mode() == 3, "exclusive request uses a real normalized fullscreen request");
	const uint32_t fullscreen = id;
	id = 91;
	check(state.request(3, id) && id == 0 && state.get_mode() == 3, "fullscreen equals pending exclusive fallback");
	state.complete(fullscreen, 3);
	id = 91;
	check(state.request(4, id) && id == 0, "exclusive equals actual fullscreen without an SDK request");
	check(state.request(1, id) && id != 0 && state.get_mode() == 1, "minimize becomes pending desired");
	const uint32_t minimize = id;
	id = 91;
	check(state.request(1, id) && id == 0, "same pending desired mode does not allocate another serial");
	state.complete(minimize, 0);
	check(state.get_mode() == 0, "a zero-request no-op does not replace the outstanding serial");
}

void stale_completion_keeps_newer_desired() {
	HostWindowModeState state;
	state.observe(0);
	uint32_t first = 0;
	uint32_t latest = 0;
	check(state.request(3, first) && first != 0, "first fullscreen request is pending");
	check(state.request(2, latest) && latest != 0 && latest != first, "newer desired mode gets a distinct nonzero serial");
	state.complete(first, 3);
	check(state.get_mode() == 2, "stale first result observes actual but preserves newer desired");
	state.cancel(first);
	check(state.get_mode() == 2, "stale cancel cannot clear newer desired");
	state.observe(1);
	check(state.get_mode() == 2, "external minimize observation cannot clear newer desired");
	state.complete(first, -1);
	check(state.get_mode() == 2, "stale unknown result also preserves newer desired");
	state.complete(latest, -1);
	check(state.get_mode() == 1, "matching unknown result clears pending and retains last actual");
	state.complete(first, 3);
	check(state.get_mode() == 3, "valid late results still update actual after pending is cleared");
}

void failed_request_rolls_back_to_actual() {
	HostWindowModeState state;
	state.observe(0);
	uint32_t id = 0;
	check(state.request(3, id) && state.get_mode() == 3, "getter exposes desired while SDK call is pending");
	state.complete(id, 0);
	check(state.get_mode() == 0, "failed fullscreen request returns queried actual windowed mode");
	check(state.request(3, id) && id != 0, "failed request can be retried");
	state.complete(id, -1);
	check(state.get_mode() == 0, "failed actual query retains known actual and clears matching pending");
	check(state.request(3, id) && id != 0, "request remains usable after unknown result");
	state.complete(id, 2);
	check(state.get_mode() == 2, "success with different SDK actual does not fabricate requested mode");
}

void cancel_only_matching_request() {
	HostWindowModeState state;
	state.observe(2);
	uint32_t first = 0;
	check(state.request(3, first), "request before cancellation");
	state.observe(1);
	state.cancel(0);
	check(state.get_mode() == 3, "zero is never a matching pending request");
	state.cancel(first);
	check(state.get_mode() == 1, "matching cancel retains externally observed actual");
	uint32_t latest = 0;
	check(state.request(0, latest) && latest != first && latest != 0, "request after cancellation has a new serial");
	state.cancel(first);
	check(state.get_mode() == 0, "cancelled serial cannot cancel a later request");
	state.cancel(latest);
	check(state.get_mode() == 1, "latest cancellation reveals actual rather than desired");
}

void window_fullscreen_window_and_serials() {
	HostWindowModeState state;
	state.observe(3);
	uint32_t window = 0;
	uint32_t full = 0;
	uint32_t last_window = 0;
	check(state.request(0, window) && state.request(3, full) && state.request(0, last_window), "window/fullscreen/window sequence accepted");
	check(window != 0 && full != 0 && last_window != 0 && window != full && full != last_window && window != last_window,
			"each effective desired change allocates a distinct serial");
	state.complete(window, 0);
	state.complete(full, 3);
	check(state.get_mode() == 0, "earlier results cannot resurrect fullscreen over last desired windowed");
	state.complete(last_window, 0);
	uint32_t previous = last_window;
	for (int step = 0; step < 1024; ++step) {
		const int32_t desired = step % 2 + 1;
		uint32_t id = 0;
		check(state.request(desired, id) && id != 0 && id != previous, "serial stays nonzero and changes across repeated transitions");
		check(state.get_mode() == desired, "repeated transition exposes its desired mode");
		state.complete(id, 0);
		check(state.get_mode() == 0, "repeated matching completion exposes SDK actual");
		previous = id;
	}
}
} // namespace

int main() {
	struct Case { const char *name; void (*run)(); };
	const Case cases[] = {
		{ "unknown_initial_state_applies_windowed", unknown_initial_state_applies_windowed },
		{ "normalization_and_invalid_requests", normalization_and_invalid_requests },
		{ "observed_actual_and_const_query", observed_actual_and_const_query },
		{ "same_effective_mode_and_exclusive_fallback", same_effective_mode_and_exclusive_fallback },
		{ "stale_completion_keeps_newer_desired", stale_completion_keeps_newer_desired },
		{ "failed_request_rolls_back_to_actual", failed_request_rolls_back_to_actual },
		{ "cancel_only_matching_request", cancel_only_matching_request },
		{ "window_fullscreen_window_and_serials", window_fullscreen_window_and_serials },
	};
	std::printf("{\"cases\":[");
	bool comma = false;
	for (const Case &test : cases) {
		const int before = failures;
		test.run();
		std::printf("%s{\"name\":\"%s\",\"passed\":%s}", comma ? "," : "", test.name, failures == before ? "true" : "false");
		comma = true;
	}
	std::printf("],\"checks\":%d,\"failures\":%d,\"passed\":%s}\n", checks, failures, failures == 0 ? "true" : "false");
	return failures == 0 ? 0 : 1;
}
