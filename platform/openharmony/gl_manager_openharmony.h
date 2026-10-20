/**************************************************************************/
/*  gl_manager_openharmony.h                                              */
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

#pragma once

#ifdef GLES3_ENABLED

#include "platform_egl.h"

#include "core/error/error_list.h"
#include "servers/display/display_server_enums.h"

#include <native_window/external_window.h>

#include <atomic>

// The host owns the native window. It stops and joins the engine before
// destroying the window; the rendering thread must release its context first.
class GLManagerOpenHarmony {
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLContext context = EGL_NO_CONTEXT;
	EGLSurface surface = EGL_NO_SURFACE;
	EGLConfig config = nullptr;
	bool display_initialized = false;

	std::atomic<DisplayServerEnums::VSyncMode> requested_vsync_mode{ DisplayServerEnums::VSYNC_ENABLED };
	int applied_swap_interval = -1; // Accessed only by the current context's thread.
	EGLint reported_vsync_error = -1;

	void _cleanup();
	Error _initialize_error(Error p_error, const char *p_message);
	void _apply_vsync(bool p_force = false);

public:
	Error initialize(OHNativeWindow *p_window, DisplayServerEnums::VSyncMode p_vsync_mode);
	void make_current();
	void release_current();
	void swap_buffers();

	// May be requested from any engine thread; EGL is updated on the current
	// rendering thread at make_current()/swap_buffers(). Unsupported modes use FIFO.
	void set_vsync_mode(DisplayServerEnums::VSyncMode p_mode);
	DisplayServerEnums::VSyncMode get_vsync_mode() const;

	EGLDisplay get_display() const { return display; }
	EGLContext get_context() const { return context; }
	EGLConfig get_config() const { return config; }

	GLManagerOpenHarmony() = default;
	GLManagerOpenHarmony(const GLManagerOpenHarmony &) = delete;
	GLManagerOpenHarmony &operator=(const GLManagerOpenHarmony &) = delete;
	~GLManagerOpenHarmony();
};

#endif // GLES3_ENABLED
