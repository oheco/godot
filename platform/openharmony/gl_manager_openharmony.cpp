/**************************************************************************/
/*  gl_manager_openharmony.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "gl_manager_openharmony.h"

#ifdef GLES3_ENABLED

#include "platform_gl.h"

#include "core/error/error_macros.h"

#include <cstdio>

Error GLManagerOpenHarmony::_initialize_error(Error p_error, const char *p_message) {
	// Read the error before cleanup calls overwrite the driver's thread-local state.
	const EGLint egl_error = eglGetError ? eglGetError() : EGL_SUCCESS;
	char message[512];
	std::snprintf(message, sizeof(message), "%s EGL error: 0x%04x.", p_message, static_cast<unsigned int>(egl_error));
	ERR_PRINT(message);
	_cleanup();
	return p_error;
}

Error GLManagerOpenHarmony::initialize(OHNativeWindow *p_window, DisplayServerEnums::VSyncMode p_vsync_mode) {
	ERR_FAIL_COND_V_MSG(display != EGL_NO_DISPLAY, ERR_ALREADY_IN_USE, "OpenHarmony EGL is already initialized.");
	ERR_FAIL_NULL_V(p_window, ERR_INVALID_PARAMETER);

	// Before eglInitialize(), GLAD can only discover the bootstrap EGL API.
	if (!gladLoaderLoadEGL(EGL_NO_DISPLAY)) {
		// GLAD may have unloaded a partially loaded library; do not call EGL here.
		ERR_PRINT("OpenHarmony: Cannot load EGL.");
		return ERR_UNAVAILABLE;
	}
	display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (display == EGL_NO_DISPLAY) {
		return _initialize_error(ERR_CANT_CREATE, "OpenHarmony: Cannot get the default EGL display.");
	}
	EGLint major = 0;
	EGLint minor = 0;
	if (!eglInitialize(display, &major, &minor)) {
		return _initialize_error(ERR_CANT_CREATE, "OpenHarmony: Cannot initialize EGL.");
	}
	display_initialized = true;
	if (major < 1 || (major == 1 && minor < 4) || !gladLoaderLoadEGL(display) || !GLAD_EGL_VERSION_1_4) {
		return _initialize_error(ERR_UNAVAILABLE, "OpenHarmony: EGL 1.4 or newer is required.");
	}
	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		return _initialize_error(ERR_UNAVAILABLE, "OpenHarmony: Cannot bind the OpenGL ES API.");
	}

	const EGLint config_attributes[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24,
		EGL_STENCIL_SIZE, 8,
		EGL_NONE
	};
	EGLint count = 0;
	if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || count == 0) {
		return _initialize_error(ERR_UNAVAILABLE, "OpenHarmony: Cannot find an EGL window configuration for GLES3.");
	}
	const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
	if (context == EGL_NO_CONTEXT) {
		return _initialize_error(ERR_CANT_CREATE, "OpenHarmony: Cannot create a GLES3 context.");
	}
	// OpenHarmony passes an OHNativeWindow pointer, not an X11 integer handle.
	surface = eglCreateWindowSurface(display, config, static_cast<EGLNativeWindowType>(p_window), nullptr);
	if (surface == EGL_NO_SURFACE) {
		return _initialize_error(ERR_CANT_CREATE, "OpenHarmony: Cannot create an EGL window surface.");
	}
	if (!eglMakeCurrent(display, surface, surface, context)) {
		return _initialize_error(ERR_CANT_CREATE, "OpenHarmony: Cannot make the GLES3 context current.");
	}
	// Use the current EGL driver's entry points. The generic GLES loader looks
	// for libGLESv2, which OpenHarmony does not ship (it provides libGLESv3).
	if (!gladLoadGLES2(reinterpret_cast<GLADloadfunc>(eglGetProcAddress)) || !GLAD_GL_ES_VERSION_3_0) {
		return _initialize_error(ERR_UNAVAILABLE, "OpenHarmony: Cannot load GLES3 entry points.");
	}

	set_vsync_mode(p_vsync_mode);
	_apply_vsync(true);
	return OK;
}

void GLManagerOpenHarmony::_apply_vsync(bool p_force) {
	const int interval = requested_vsync_mode.load(std::memory_order_relaxed) == DisplayServerEnums::VSYNC_DISABLED ? 0 : 1;
	if (!p_force && interval == applied_swap_interval) {
		return;
	}
	if (eglSwapInterval(display, interval)) {
		applied_swap_interval = interval;
		reported_vsync_error = -1;
	} else {
		const EGLint egl_error = eglGetError();
		if (egl_error != reported_vsync_error) {
			char message[128];
			std::snprintf(message, sizeof(message), "OpenHarmony: Cannot apply the EGL swap interval. EGL error: 0x%04x.", static_cast<unsigned int>(egl_error));
			ERR_PRINT(message);
			reported_vsync_error = egl_error;
		}
	}
}

void GLManagerOpenHarmony::make_current() {
	ERR_FAIL_COND_MSG(context == EGL_NO_CONTEXT, "OpenHarmony EGL is not initialized.");
	// The bound client API is thread-local; explicitly select GLES after migration.
	ERR_FAIL_COND_MSG(!eglBindAPI(EGL_OPENGL_ES_API), "OpenHarmony: Cannot bind the OpenGL ES API on the rendering thread.");
	ERR_FAIL_COND_MSG(!eglMakeCurrent(display, surface, surface, context), "OpenHarmony: Cannot make the GLES3 context current on the rendering thread.");
	_apply_vsync(true);
}

void GLManagerOpenHarmony::release_current() {
	if (context != EGL_NO_CONTEXT && eglGetCurrentContext() == context && eglGetCurrentDisplay() == display) {
		ERR_FAIL_COND_MSG(!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), "OpenHarmony: Cannot release the GLES3 context.");
	}
}

void GLManagerOpenHarmony::swap_buffers() {
	ERR_FAIL_COND_MSG(context == EGL_NO_CONTEXT, "OpenHarmony EGL is not initialized.");
	ERR_FAIL_COND_MSG(eglGetCurrentContext() != context || eglGetCurrentDisplay() != display, "OpenHarmony: Swap requires the current GLES3 context.");
	_apply_vsync();
	ERR_FAIL_COND_MSG(!eglSwapBuffers(display, surface), "OpenHarmony: Cannot swap EGL buffers.");
}

void GLManagerOpenHarmony::set_vsync_mode(DisplayServerEnums::VSyncMode p_mode) {
	// EGL 1.4 exposes swap intervals, but no mailbox or adaptive presentation mode.
	if (p_mode != DisplayServerEnums::VSYNC_DISABLED && p_mode != DisplayServerEnums::VSYNC_ENABLED) {
		WARN_PRINT("OpenHarmony GLES: Adaptive and mailbox VSync use enabled VSync.");
		p_mode = DisplayServerEnums::VSYNC_ENABLED;
	}
	requested_vsync_mode.store(p_mode, std::memory_order_relaxed);
}

DisplayServerEnums::VSyncMode GLManagerOpenHarmony::get_vsync_mode() const {
	return requested_vsync_mode.load(std::memory_order_relaxed);
}

void GLManagerOpenHarmony::_cleanup() {
	if (display_initialized) {
		release_current();
		if (surface != EGL_NO_SURFACE) {
			eglDestroySurface(display, surface);
		}
		if (context != EGL_NO_CONTEXT) {
			eglDestroyContext(display, context);
		}
		eglTerminate(display);
	}
	surface = EGL_NO_SURFACE;
	context = EGL_NO_CONTEXT;
	display = EGL_NO_DISPLAY;
	config = nullptr;
	display_initialized = false;
	applied_swap_interval = -1;
	reported_vsync_error = -1;
}

GLManagerOpenHarmony::~GLManagerOpenHarmony() {
	_cleanup();
}

#endif // GLES3_ENABLED
