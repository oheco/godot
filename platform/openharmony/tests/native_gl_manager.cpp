// Godot Engine contributors. SPDX-License-Identifier: MIT
// Compile the complete production manager with SDK types and GLAD. Only logging
// is stubbed. Mock mode injects EGL boundaries; real mode substitutes a pbuffer
// for the OHNativeWindow surface and otherwise uses the actual EGL/GLES driver.
#include "gl_manager_openharmony.h"
#include "platform_gl.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace DisplayServerEnums;

static bool real_driver = false;
static int failures = 0;
static int cases = 0;
#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
			++failures; \
		} \
	} while (0)

static EGLDisplay test_display = reinterpret_cast<EGLDisplay>(0x1000);
static EGLConfig test_config = reinterpret_cast<EGLConfig>(0x2000);
static EGLContext test_context = reinterpret_cast<EGLContext>(0x3000);
static EGLSurface test_surface = reinterpret_cast<EGLSurface>(0x4000);
static OHNativeWindow *test_window = reinterpret_cast<OHNativeWindow *>(0x5000);
static thread_local EGLContext current_context = EGL_NO_CONTEXT;
static thread_local EGLDisplay current_display = EGL_NO_DISPLAY;
static std::atomic<int> interval_calls{ 0 };
static std::atomic<int> swap_calls{ 0 };
static std::atomic<int> wrong_thread_calls{ 0 };
static int last_interval = -1;
static int load_calls = 0;
static std::vector<std::string> cleanup;

enum Failure { NONE,
	BOOTSTRAP,
	DISPLAY,
	INITIALIZE,
	OLD_EGL,
	RELOAD,
	BIND,
	CHOOSE,
	NO_CONFIG,
	CONTEXT,
	SURFACE,
	CURRENT,
	GLES_LOAD,
	GLES2_ONLY,
	INTERVAL,
	SWAP,
	RELEASE };
static Failure injected_failure = NONE;

static EGLDisplay EGLAPIENTRY fake_get_display(EGLNativeDisplayType p_display) {
	CHECK(p_display == EGL_DEFAULT_DISPLAY);
	return injected_failure == DISPLAY ? EGL_NO_DISPLAY : test_display;
}
static EGLBoolean EGLAPIENTRY fake_initialize(EGLDisplay p_display, EGLint *r_major, EGLint *r_minor) {
	CHECK(p_display == test_display);
	*r_major = 1;
	*r_minor = injected_failure == OLD_EGL ? 3 : 4;
	return injected_failure != INITIALIZE;
}
static EGLBoolean EGLAPIENTRY fake_bind(EGLenum p_api) {
	CHECK(p_api == EGL_OPENGL_ES_API);
	return injected_failure != BIND;
}
static EGLBoolean EGLAPIENTRY fake_choose(EGLDisplay p_display, const EGLint *p_attributes, EGLConfig *r_config, EGLint p_size, EGLint *r_count) {
	CHECK(p_display == test_display && p_size == 1);
	bool window = false;
	bool es3 = false;
	for (const EGLint *a = p_attributes; *a != EGL_NONE; a += 2) {
		window |= a[0] == EGL_SURFACE_TYPE && a[1] == EGL_WINDOW_BIT;
		es3 |= a[0] == EGL_RENDERABLE_TYPE && a[1] == EGL_OPENGL_ES3_BIT;
	}
	CHECK(window && es3);
	*r_config = test_config;
	*r_count = injected_failure == NO_CONFIG ? 0 : 1;
	return injected_failure != CHOOSE;
}
static EGLContext EGLAPIENTRY fake_create_context(EGLDisplay p_display, EGLConfig p_config, EGLContext p_share, const EGLint *p_attributes) {
	CHECK(p_display == test_display && p_config == test_config && p_share == EGL_NO_CONTEXT);
	CHECK(p_attributes[0] == EGL_CONTEXT_CLIENT_VERSION && p_attributes[1] == 3 && p_attributes[2] == EGL_NONE);
	return injected_failure == CONTEXT ? EGL_NO_CONTEXT : test_context;
}
static EGLSurface EGLAPIENTRY fake_create_surface(EGLDisplay p_display, EGLConfig p_config, EGLNativeWindowType p_window, const EGLint *) {
	CHECK(p_display == test_display && p_config == test_config);
	CHECK(p_window == static_cast<EGLNativeWindowType>(test_window));
	return injected_failure == SURFACE ? EGL_NO_SURFACE : test_surface;
}
static EGLBoolean EGLAPIENTRY fake_make_current(EGLDisplay p_display, EGLSurface p_draw, EGLSurface p_read, EGLContext p_context) {
	CHECK(p_display == test_display);
	if (p_context == EGL_NO_CONTEXT) {
		CHECK(p_draw == EGL_NO_SURFACE && p_read == EGL_NO_SURFACE);
		if (injected_failure == RELEASE) {
			return EGL_FALSE;
		}
		cleanup.push_back("release");
	} else {
		CHECK(p_context == test_context && p_draw == test_surface && p_read == test_surface);
		if (injected_failure == CURRENT) {
			return EGL_FALSE;
		}
	}
	current_context = p_context;
	current_display = p_context == EGL_NO_CONTEXT ? EGL_NO_DISPLAY : p_display;
	return EGL_TRUE;
}
static EGLContext EGLAPIENTRY fake_get_current_context() {
	return current_context;
}
static EGLDisplay EGLAPIENTRY fake_get_current_display() {
	return current_display;
}
static EGLBoolean EGLAPIENTRY fake_interval(EGLDisplay p_display, EGLint p_interval) {
	if (current_context != test_context || current_display != test_display) {
		++wrong_thread_calls;
	}
	CHECK(p_display == test_display);
	++interval_calls;
	last_interval = p_interval;
	return injected_failure != INTERVAL;
}
static EGLBoolean EGLAPIENTRY fake_swap(EGLDisplay p_display, EGLSurface p_surface) {
	CHECK(p_display == test_display && p_surface == test_surface);
	if (current_context != test_context) {
		++wrong_thread_calls;
	}
	++swap_calls;
	return injected_failure != SWAP;
}
static EGLBoolean EGLAPIENTRY fake_destroy_surface(EGLDisplay p_display, EGLSurface p_surface) {
	CHECK(p_display == test_display && p_surface == test_surface);
	cleanup.push_back("surface");
	return EGL_TRUE;
}
static EGLBoolean EGLAPIENTRY fake_destroy_context(EGLDisplay p_display, EGLContext p_context) {
	CHECK(p_display == test_display && p_context == test_context);
	cleanup.push_back("context");
	return EGL_TRUE;
}
static EGLBoolean EGLAPIENTRY fake_terminate(EGLDisplay p_display) {
	CHECK(p_display == test_display);
	cleanup.push_back("terminate");
	return EGL_TRUE;
}

static PFNEGLCHOOSECONFIGPROC real_choose = nullptr;
static PFNEGLSWAPINTERVALPROC real_interval = nullptr;
static PFNEGLGETERRORPROC real_get_error = nullptr;
static int real_interval_failures = 0;
static EGLint real_interval_last_error = EGL_SUCCESS;
static EGLBoolean EGLAPIENTRY pbuffer_choose(EGLDisplay p_display, const EGLint *p_attributes, EGLConfig *r_config, EGLint p_size, EGLint *r_count) {
	std::vector<EGLint> attributes;
	for (const EGLint *a = p_attributes; *a != EGL_NONE; a += 2) {
		attributes.push_back(a[0]);
		attributes.push_back(a[0] == EGL_SURFACE_TYPE ? EGL_PBUFFER_BIT : a[1]);
	}
	attributes.push_back(EGL_NONE);
	return real_choose(p_display, attributes.data(), r_config, p_size, r_count);
}
static EGLSurface EGLAPIENTRY pbuffer_surface(EGLDisplay p_display, EGLConfig p_config, EGLNativeWindowType p_window, const EGLint *) {
	CHECK(p_window == static_cast<EGLNativeWindowType>(test_window));
	const EGLint attributes[] = { EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE };
	return eglCreatePbufferSurface(p_display, p_config, attributes);
}
static EGLBoolean EGLAPIENTRY count_real_interval(EGLDisplay p_display, EGLint p_interval) {
	++interval_calls;
	last_interval = p_interval;
	const EGLBoolean result = real_interval(p_display, p_interval);
	if (!result) {
		++real_interval_failures;
	}
	return result;
}
static EGLint EGLAPIENTRY track_real_error() {
	const EGLint error = real_get_error();
	if (real_interval_failures > 0) {
		real_interval_last_error = error;
	}
	return error;
}
static EGLint EGLAPIENTRY fake_get_error() {
	CHECK(injected_failure != BOOTSTRAP); // A failed loader can leave stale pointers.
	return injected_failure == INTERVAL ? EGL_BAD_SURFACE : EGL_BAD_ALLOC;
}

extern "C" int __real_gladLoaderLoadEGL(EGLDisplay);
extern "C" int __real_gladLoadGLES2(GLADloadfunc);
extern "C" int __wrap_gladLoaderLoadEGL(EGLDisplay p_display) {
	if (real_driver) {
		int version = __real_gladLoaderLoadEGL(p_display);
		if (version) {
			real_choose = glad_eglChooseConfig;
			real_interval = glad_eglSwapInterval;
			real_get_error = glad_eglGetError;
			glad_eglGetError = track_real_error;
			glad_eglChooseConfig = pbuffer_choose;
			glad_eglCreateWindowSurface = pbuffer_surface;
			glad_eglSwapInterval = count_real_interval;
		}
		return version;
	}
	++load_calls;
	glad_eglGetError = fake_get_error;
	glad_eglGetDisplay = fake_get_display;
	glad_eglInitialize = fake_initialize;
	glad_eglBindAPI = fake_bind;
	glad_eglChooseConfig = fake_choose;
	glad_eglCreateContext = fake_create_context;
	glad_eglCreateWindowSurface = fake_create_surface;
	glad_eglMakeCurrent = fake_make_current;
	glad_eglGetCurrentContext = fake_get_current_context;
	glad_eglGetCurrentDisplay = fake_get_current_display;
	glad_eglSwapInterval = fake_interval;
	glad_eglSwapBuffers = fake_swap;
	glad_eglDestroySurface = fake_destroy_surface;
	glad_eglDestroyContext = fake_destroy_context;
	glad_eglTerminate = fake_terminate;
	GLAD_EGL_VERSION_1_4 = 1;
	if ((injected_failure == BOOTSTRAP && p_display == EGL_NO_DISPLAY) || (injected_failure == RELOAD && p_display != EGL_NO_DISPLAY)) {
		return 0;
	}
	return GLAD_MAKE_VERSION(1, 4);
}
extern "C" int __wrap_gladLoadGLES2(GLADloadfunc p_loader) {
	if (real_driver) {
		return __real_gladLoadGLES2(p_loader);
	}
	GLAD_GL_ES_VERSION_3_0 = injected_failure != GLES2_ONLY;
	return injected_failure == GLES_LOAD ? 0 : GLAD_MAKE_VERSION(3, 2);
}

static void reset(Failure p_failure = NONE) {
	injected_failure = p_failure;
	cleanup.clear();
	interval_calls = 0;
	swap_calls = 0;
	wrong_thread_calls = 0;
	load_calls = 0;
	last_interval = -1;
	current_context = EGL_NO_CONTEXT;
	current_display = EGL_NO_DISPLAY;
}
static void failure_cases() {
	const struct {
		Failure failure;
		Error error;
		const char *cleanup;
	} inputs[] = {
		{ BOOTSTRAP, ERR_UNAVAILABLE, "" },
		{ DISPLAY, ERR_CANT_CREATE, "" },
		{ INITIALIZE, ERR_CANT_CREATE, "" },
		{ OLD_EGL, ERR_UNAVAILABLE, "terminate " },
		{ RELOAD, ERR_UNAVAILABLE, "terminate " },
		{ BIND, ERR_UNAVAILABLE, "terminate " },
		{ CHOOSE, ERR_UNAVAILABLE, "terminate " },
		{ NO_CONFIG, ERR_UNAVAILABLE, "terminate " },
		{ CONTEXT, ERR_CANT_CREATE, "terminate " },
		{ SURFACE, ERR_CANT_CREATE, "context terminate " },
		{ CURRENT, ERR_CANT_CREATE, "surface context terminate " },
		{ GLES_LOAD, ERR_UNAVAILABLE, "release surface context terminate " },
		{ GLES2_ONLY, ERR_UNAVAILABLE, "release surface context terminate " },
	};
	for (const auto &input : inputs) {
		reset(input.failure);
		{
			GLManagerOpenHarmony manager;
			CHECK(manager.initialize(test_window, VSYNC_ENABLED) == input.error);
			CHECK(manager.get_display() == EGL_NO_DISPLAY && manager.get_context() == EGL_NO_CONTEXT && manager.get_config() == nullptr);
		}
		std::string events;
		for (const auto &event : cleanup) {
			events += event + " ";
		}
		CHECK(events == input.cleanup);
		++cases;
	}
	reset();
	GLManagerOpenHarmony manager;
	CHECK(manager.initialize(nullptr, VSYNC_ENABLED) == ERR_INVALID_PARAMETER && load_calls == 0);
	// A failed attempt leaves the same object reusable.
	injected_failure = SURFACE;
	CHECK(manager.initialize(test_window, VSYNC_ENABLED) == ERR_CANT_CREATE);
	injected_failure = NONE;
	CHECK(manager.initialize(test_window, VSYNC_ENABLED) == OK);
	const int loads = load_calls;
	CHECK(manager.initialize(test_window, VSYNC_ENABLED) == ERR_ALREADY_IN_USE && load_calls == loads);
	cases += 3;
}

static void mock_thread_cases() {
	reset();
	{
		GLManagerOpenHarmony manager;
		CHECK(manager.initialize(test_window, VSYNC_DISABLED) == OK);
		CHECK(manager.get_display() == test_display && manager.get_context() == test_context && manager.get_config() == test_config);
		CHECK(interval_calls == 1 && last_interval == 0);
		manager.set_vsync_mode(VSYNC_MAILBOX);
		CHECK(manager.get_vsync_mode() == VSYNC_ENABLED && interval_calls == 1);
		manager.swap_buffers();
		CHECK(interval_calls == 2 && last_interval == 1 && swap_calls == 1);
		manager.set_vsync_mode(VSYNC_ADAPTIVE);
		manager.swap_buffers();
		CHECK(interval_calls == 2 && manager.get_vsync_mode() == VSYNC_ENABLED);
		manager.release_current();
		manager.swap_buffers();
		CHECK(swap_calls == 2); // Refuse a swap without a current context.
		const int before_failed_bind = interval_calls;
		injected_failure = BIND;
		manager.make_current();
		CHECK(current_context == EGL_NO_CONTEXT && interval_calls == before_failed_bind);
		injected_failure = CURRENT;
		manager.make_current();
		CHECK(current_context == EGL_NO_CONTEXT && interval_calls == before_failed_bind);
		injected_failure = NONE;
		std::mutex mutex;
		std::condition_variable condition;
		bool ready = false;
		bool proceed = false;
		std::thread renderer([&] {
			manager.make_current();
			{
				std::lock_guard<std::mutex> lock(mutex);
				ready = true;
			}
			condition.notify_one();
			{
				std::unique_lock<std::mutex> lock(mutex);
				condition.wait(lock, [&] { return proceed; });
			}
			manager.swap_buffers();
			manager.release_current();
		});
		{
			std::unique_lock<std::mutex> lock(mutex);
			condition.wait(lock, [&] { return ready; });
		}
		const int before = interval_calls;
		manager.set_vsync_mode(VSYNC_DISABLED);
		manager.release_current(); // A foreign thread must not unbind the renderer.
		CHECK(interval_calls == before && manager.get_vsync_mode() == VSYNC_DISABLED);
		{
			std::lock_guard<std::mutex> lock(mutex);
			proceed = true;
		}
		condition.notify_one();
		renderer.join();
		CHECK(interval_calls == before + 1 && last_interval == 0 && wrong_thread_calls == 0);
		manager.make_current();
		injected_failure = INTERVAL;
		manager.set_vsync_mode(VSYNC_ENABLED);
		manager.swap_buffers();
		manager.swap_buffers(); // Retry the same failure without repeating its log.
		const int failed_interval = interval_calls;
		injected_failure = NONE;
		manager.swap_buffers();
		CHECK(interval_calls == failed_interval + 1 && last_interval == 1);
		manager.set_vsync_mode(VSYNC_DISABLED);
		injected_failure = INTERVAL;
		manager.swap_buffers(); // A successful application resets error suppression.
		manager.swap_buffers();
		injected_failure = NONE;
		manager.swap_buffers();
		CHECK(last_interval == 0);
		injected_failure = SWAP;
		manager.swap_buffers();
		injected_failure = RELEASE;
		manager.release_current();
		CHECK(current_context == test_context);
		injected_failure = NONE;
	}
	CHECK(cleanup.size() >= 4);
	CHECK(cleanup[cleanup.size() - 4] == "release" && cleanup[cleanup.size() - 3] == "surface" && cleanup[cleanup.size() - 2] == "context" && cleanup.back() == "terminate");
	cases += 10;
}

static bool check_pixel() {
	glViewport(0, 0, 8, 8);
	glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	unsigned char pixel[4] = {};
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	return glGetError() == GL_NO_ERROR && std::abs(int(pixel[0]) - 64) <= 1 && std::abs(int(pixel[1]) - 128) <= 1 && std::abs(int(pixel[2]) - 191) <= 1 && pixel[3] == 255;
}
static void real_cases() {
	GLManagerOpenHarmony manager;
	if (manager.initialize(test_window, VSYNC_ENABLED) != OK) {
		CHECK(false);
		return;
	}
	const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
	std::fprintf(stderr, "native EGL=%s GLES=%s\n", eglQueryString(manager.get_display(), EGL_VERSION), version ? version : "null");
	CHECK(version && std::strstr(version, "OpenGL ES 3"));
	CHECK(check_pixel());
	manager.release_current();
	CHECK(eglGetCurrentContext() == EGL_NO_CONTEXT);
	std::mutex mutex;
	std::condition_variable condition;
	bool ready = false;
	bool proceed = false;
	bool worker_current = false;
	bool worker_pixel = false;
	std::thread renderer([&] {
		manager.make_current();
		worker_current = eglGetCurrentContext() == manager.get_context();
		{
			std::lock_guard<std::mutex> lock(mutex);
			ready = true;
		}
		condition.notify_one();
		{
			std::unique_lock<std::mutex> lock(mutex);
			condition.wait(lock, [&] { return proceed; });
		}
		worker_pixel = check_pixel();
		manager.swap_buffers();
		manager.release_current();
	});
	{
		std::unique_lock<std::mutex> lock(mutex);
		condition.wait(lock, [&] { return ready; });
	}
	const int before = interval_calls;
	manager.set_vsync_mode(VSYNC_DISABLED);
	CHECK(interval_calls == before);
	{
		std::lock_guard<std::mutex> lock(mutex);
		proceed = true;
	}
	condition.notify_one();
	renderer.join();
	CHECK(worker_current && worker_pixel && interval_calls == before + 1 && last_interval == 0);
	manager.make_current();
	CHECK(eglGetCurrentContext() == manager.get_context() && check_pixel());
	manager.swap_buffers();
	cases += 5;
}

int main(int argc, char **argv) {
	real_driver = argc == 2 && std::strcmp(argv[1], "real") == 0;
	if (real_driver) {
		real_cases();
	} else {
		failure_cases();
		mock_thread_cases();
	}
	std::printf("{\"mode\":\"%s\",\"cases\":%d,\"failures\":%d,\"swap_interval_failures\":%d,\"swap_interval_last_error\":%d,\"vsync_effective_verified\":false,\"passed\":%s}\n", real_driver ? "real_pbuffer" : "mock", cases, failures, real_interval_failures, real_interval_last_error, failures == 0 ? "true" : "false");
	return failures ? 1 : 0;
}
