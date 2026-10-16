// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

struct DevelopmentKitCandidate {
	std::string root;
	std::string manifest;
};
// Explicit override, active oo executable, then installed versions descending.
// Manifests are bounded UTF-8 JSON text; the ArkTS caller validates compatibility.
std::vector<DevelopmentKitCandidate> development_kit_candidates();
bool publish_development_kit_templates(const std::string &files, const std::string &runtime,
		std::string &notice);
// For the oo headless executable only: preserve caller overrides and NuGet state.
bool configure_cli_development_kit(const std::string &executable, std::string &notice);

// "none" prepares only sandbox paths and accepts an empty runtime. "sdk"
// additionally prepares the external development kit, SDK and NuGet setup.
// Unknown modes and invalid paths fail before mutating the environment.
bool configure_runtime_paths(const std::string &files, const std::string &cache,
		const std::string &runtime, const std::string &managed_mode, std::string &error,
		const std::string &temporary = "");

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
