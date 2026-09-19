// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

#include <string>

// Returns an empty string on success. The report includes the application's
// actual security domain and subprocess output, never account credentials.
std::string check_dotnet_sdk(const std::string &executable, const std::string &report);

// Reports what the application sandbox actually permits: the security domain,
// whether the extracted .NET runtime can be loaded from application data, and
// whether the platform native child process API is available. Used as the
// diagnostic for the exec restriction on application data files.
std::string probe_sandbox(const std::string &dotnet_root, const std::string &files_dir, const std::string &cache_dir);

// Resolves the .NET SDK to run: by default the one installed by the oheco
// package manager, so the engine does not ship a copy and stays decoupled from
// its version. GODOT_OHOS_DOTNET_ROOT overrides it for testing. Returns an empty
// string when no usable installation is present.
std::string resolve_dotnet_root();
