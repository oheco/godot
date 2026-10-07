// Godot Engine contributors. SPDX-License-Identifier: MIT
// The runner includes the complete, byte-exact production method. Only its
// owner/state, mapping and logging are substituted; CursorInfo uses the real SDK.
#include <inputmethod/inputmethod_inputmethod_proxy_capi.h>

#include <cstdio>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

struct Point2 {
	double x = 0, y = 0;
	Point2() = default;
	Point2(double p_x, double p_y) : x(p_x), y(p_y) {}
	bool operator==(const Point2 &p_other) const { return x == p_other.x && y == p_other.y; }
};

namespace {
int failures = 0;
unsigned char proxy_cookie;
// This address is only consumed by __wrap_NotifyCursorUpdate, never by SDK IME.
auto *const fake_proxy = reinterpret_cast<InputMethod_InputMethodProxy *>(&proxy_cookie);
const Point2 anchor(-68.5, 135.25);
const Point2 changed_anchor(720.5, 760.25);
const Point2 sentinel(-999, -998);
struct Calls {
	int create_attempts = 0, created = 0, destroyed = 0, notify = 0, errors = 0;
	bool allocation_failure = false;
	Point2 expected_anchor;
	std::set<InputMethod_CursorInfo *> live;
	std::vector<InputMethod_ErrorCode> results;
} calls;

void check(bool p_condition, const char *p_message) {
	if (!p_condition) {
		++failures;
		fprintf(stderr, "FAIL %s\n", p_message);
	}
}

void error_log(const std::string &p_message) {
	check(!p_message.empty(), "error log contains a message");
	++calls.errors;
}

std::string vformat(const char *p_format, InputMethod_ErrorCode p_code) {
	char text[160];
	snprintf(text, sizeof(text), p_format, static_cast<int>(p_code));
	return text;
}
} // namespace

class DisplayServerOpenHarmony {
public:
	bool ime_active = true;
	InputMethod_InputMethodProxy *input_method_proxy = fake_proxy;
	bool ime_screen_position_valid = false;
	Point2 last_ime_screen_position = sentinel;
	bool geometry_available = true;
	Point2 screen_position = anchor;
	mutable int mapping_calls = 0;

	bool _get_ime_screen_position(Point2 &r_position) const {
		++mapping_calls;
		r_position = screen_position;
		return geometry_available;
	}
	void _update_ime_cursor();
};

#define ERR_PRINT(message) error_log(message)
#include "ime_cursor_method.inc"
#undef ERR_PRINT

extern "C" InputMethod_CursorInfo *__real_OH_CursorInfo_Create(double, double, double, double);
extern "C" void __real_OH_CursorInfo_Destroy(InputMethod_CursorInfo *);

extern "C" InputMethod_CursorInfo *__wrap_OH_CursorInfo_Create(double p_x, double p_y, double p_width, double p_height) {
	++calls.create_attempts;
	if (calls.allocation_failure) {
		return nullptr;
	}
	auto *info = __real_OH_CursorInfo_Create(p_x, p_y, p_width, p_height);
	check(info != nullptr, "real SDK CursorInfo allocation succeeds");
	if (info) {
		++calls.created;
		check(calls.live.insert(info).second, "CursorInfo has unique live ownership");
	}
	return info;
}

extern "C" void __wrap_OH_CursorInfo_Destroy(InputMethod_CursorInfo *p_info) {
	if (calls.live.erase(p_info) != 1) {
		check(false, "destroy exactly one owned real CursorInfo");
		return;
	}
	++calls.destroyed;
	__real_OH_CursorInfo_Destroy(p_info);
}

extern "C" InputMethod_ErrorCode __wrap_OH_InputMethodProxy_NotifyCursorUpdate(
		InputMethod_InputMethodProxy *p_proxy, InputMethod_CursorInfo *p_info) {
	check(p_proxy == fake_proxy, "fake proxy reaches only our Notify mock");
	check(calls.live.count(p_info) == 1, "Notify receives a live real SDK CursorInfo");
	double x = 0, y = 0, width = -1, height = -1;
	check(OH_CursorInfo_GetRect(p_info, &x, &y, &width, &height) == IME_ERR_OK, "real SDK reads CursorInfo rectangle");
	check(x == calls.expected_anchor.x && y == calls.expected_anchor.y && width == 0 && height == 0,
			"Notify receives the exact fractional screen anchor with zero size");
	const auto index = static_cast<size_t>(calls.notify++);
	if (index >= calls.results.size()) {
		check(false, "Notify call has a scripted SDK result");
		return IME_ERR_IMMS;
	}
	return calls.results[index];
}

namespace {
void begin(std::initializer_list<InputMethod_ErrorCode> p_results) {
	check(calls.live.empty(), "previous scenario leaves no live CursorInfo");
	calls = Calls{};
	calls.results = p_results;
}

void update(DisplayServerOpenHarmony &p_display) {
	calls.expected_anchor = p_display.screen_position;
	p_display._update_ime_cursor();
	check(calls.live.empty() && calls.created == calls.destroyed, "success/failure/early return destroys every CursorInfo");
}

void cache_is(const DisplayServerOpenHarmony &p_display, bool p_valid, const Point2 &p_position) {
	check(p_display.ime_screen_position_valid == p_valid && p_display.last_ime_screen_position == p_position,
			"cache contains only the last successful notification");
}

void report(const char *p_name, int p_previous_failures) {
	printf("%s %s: notify=%d create=%d destroy=%d live=%zu errors=%d\n",
			failures == p_previous_failures ? "PASS" : "FAIL", p_name,
			calls.notify, calls.created, calls.destroyed, calls.live.size(), calls.errors);
}

void retry_same_anchor() {
	const int before = failures;
	begin({ IME_ERR_IMMS, IME_ERR_OK });
	DisplayServerOpenHarmony display;
	update(display);
	check(calls.notify == 1 && calls.errors == 1, "first same-anchor Notify fails with IMMS");
	cache_is(display, false, sentinel);
	update(display);
	check(calls.notify == 2 && calls.errors == 1, "second identical anchor retries Notify and succeeds");
	cache_is(display, true, anchor);
	update(display);
	check(calls.notify == 2 && calls.create_attempts == 2, "third identical anchor deduplicates after success");
	report("failure -> same-anchor success -> dedup", before);
}

void repeated_failures() {
	const int before = failures;
	begin({ IME_ERR_IMMS, IME_ERR_IMCLIENT, IME_ERR_DETACHED, IME_ERR_OK });
	DisplayServerOpenHarmony display;
	for (int i = 1; i <= 3; ++i) {
		update(display);
		check(calls.notify == i && calls.errors == i, "persistent SDK failure keeps retrying the same anchor");
		cache_is(display, false, sentinel);
	}
	update(display);
	check(calls.notify == 4, "same anchor recovers after persistent failures");
	cache_is(display, true, anchor);
	update(display);
	check(calls.notify == 4, "recovery enables deduplication");
	report("IMMS/IMCLIENT/DETACHED retries", before);
}

void set_guard(DisplayServerOpenHarmony &p_display, int p_guard, bool p_enabled) {
	p_display.ime_active = p_guard != 0 || !p_enabled;
	p_display.input_method_proxy = p_guard == 1 && p_enabled ? nullptr : fake_proxy;
	p_display.geometry_available = p_guard != 2 || !p_enabled;
	calls.allocation_failure = p_guard == 3 && p_enabled;
}

void early_returns() {
	const char *names[] = { "inactive", "null proxy", "no geometry", "allocation failure" };
	for (int guard = 0; guard < 4; ++guard) {
		const int before = failures;
		begin({ IME_ERR_OK, IME_ERR_OK });
		DisplayServerOpenHarmony display;
		set_guard(display, guard, true);
		update(display);
		cache_is(display, false, sentinel);
		check(calls.notify == 0 && calls.created == 0, "guard never notifies or commits false success");
		check(display.mapping_calls == (guard < 2 ? 0 : 1), "inactive/null proxy bypass geometry mapping");
		check(calls.create_attempts == (guard == 3 ? 1 : 0), "only allocation failure attempts creation");
		set_guard(display, guard, false);
		update(display);
		cache_is(display, true, anchor);
		update(display);
		check(calls.notify == 1, "same anchor succeeds after guard clears, then deduplicates");

		display.screen_position = changed_anchor;
		set_guard(display, guard, true);
		update(display);
		cache_is(display, true, anchor);
		check(calls.notify == 1, "guard preserves a previous successful cache");
		set_guard(display, guard, false);
		update(display);
		cache_is(display, true, changed_anchor);
		update(display);
		check(calls.notify == 2, "changed anchor retries after guard clears, then deduplicates");
		check(calls.errors == (guard == 3 ? 2 : 0), "allocation failure is logged on each attempt");
		report(names[guard], before);
	}
}

void changed_anchor_retry() {
	const int before = failures;
	begin({ IME_ERR_OK, IME_ERR_IMMS, IME_ERR_OK, IME_ERR_OK });
	DisplayServerOpenHarmony display;
	update(display);
	cache_is(display, true, anchor);
	display.screen_position = changed_anchor;
	update(display);
	check(calls.notify == 2 && calls.errors == 1, "changed anchor triggers a new failing Notify");
	cache_is(display, true, anchor);
	update(display);
	check(calls.notify == 3, "failed changed anchor retries despite a valid older cache");
	cache_is(display, true, changed_anchor);
	update(display);
	check(calls.notify == 3, "new successful anchor deduplicates");
	display.screen_position = anchor;
	update(display);
	check(calls.notify == 4, "returning to an earlier anchor triggers another Notify");
	cache_is(display, true, anchor);
	report("changed anchor failure/retry and return", before);
}
} // namespace

int main() {
	retry_same_anchor();
	repeated_failures();
	early_returns();
	changed_anchor_retry();
	printf("%s native production _update_ime_cursor: %d assertion failures; real SDK CursorInfo; Notify only mocked\n",
			failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
