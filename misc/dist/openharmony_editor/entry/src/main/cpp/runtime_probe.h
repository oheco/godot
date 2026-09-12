// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

#include <string>

// Returns an empty string on success. The report includes the application's
// actual security domain and subprocess output, never account credentials.
std::string check_dotnet_sdk(const std::string &executable, const std::string &report);
