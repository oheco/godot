// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace GodotOpenHarmony {

// Protected by the host's event_mutex. Window APIs execute asynchronously on
// ArkUI; queries retain the latest accepted intent until its actual result.
class HostWindowModeState {
	int32_t actual_mode = -1;
	int32_t requested_mode = 0;
	uint32_t serial = 0;
	bool pending = false;

public:
	static constexpr int32_t normalize(int32_t p_mode) {
		return p_mode == 4 ? 3 : (p_mode >= 0 && p_mode <= 3 ? p_mode : -1);
	}

	int32_t get_mode() const { return pending ? requested_mode : (actual_mode >= 0 ? actual_mode : 0); }

	bool request(int32_t p_mode, uint32_t &r_request) {
		const int32_t mode = normalize(p_mode);
		if (mode < 0) {
			return false;
		}
		r_request = 0;
		if (mode == (pending ? requested_mode : actual_mode)) {
			return true;
		}
		requested_mode = mode;
		pending = true;
		if (++serial == 0) {
			++serial;
		}
		r_request = serial;
		return true;
	}

	void observe(int32_t p_actual) {
		const int32_t mode = normalize(p_actual);
		if (mode >= 0) {
			actual_mode = mode;
		}
	}

	void complete(uint32_t p_request, int32_t p_actual) {
		observe(p_actual);
		cancel(p_request);
	}

	void cancel(uint32_t p_request) {
		if (pending && p_request == serial) {
			pending = false;
		}
	}
};

} // namespace GodotOpenHarmony
