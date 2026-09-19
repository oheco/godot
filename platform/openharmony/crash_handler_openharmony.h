// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

// Writes a native backtrace to the given file (and to stderr) when the engine
// dies from a fatal signal. The application reads that file afterwards: on
// OpenHarmony the editor runs in its own process, so a crash in it leaves no
// trace the user can reach otherwise.
void ohos_crash_handler_install(const char *p_log_path);
