// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

#include <string>

// Resolves the .NET SDK to run: by default the one installed by the oheco
// package manager, so the engine does not ship a copy and stays decoupled from
// its version. GODOT_OHOS_DOTNET_ROOT overrides it for testing. Returns an empty
// string when no usable installation is present.
std::string resolve_dotnet_root();

// HarmonyOS loads a plugin only from a directory that the process registered
// with the linker, and the registration only takes effect once the restricted
// ohos.permission.kernel.LOAD_INDEPENDENT_LIBRARY permission is granted. The
// .NET host loads most of the runtime itself, so every directory it may need has
// to be registered before the runtime is initialized.
void add_independent_library_directory(const std::string &directory);
