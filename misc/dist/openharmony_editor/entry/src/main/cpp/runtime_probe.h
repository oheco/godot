// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

// Returns an empty string on success. The report includes the application's
// actual security domain and subprocess output, never account credentials.
std::string check_dotnet_sdk(const std::string &executable, const std::string &report);

// Reports what the application sandbox actually permits: the security domain,
// whether the extracted .NET runtime can be loaded from application data, and
// whether the platform native child process API is available. Used as the
// diagnostic for the exec restriction on application data files.
std::string probe_sandbox(const std::string &dotnet_root, const std::string &files_dir, const std::string &cache_dir);

// Runs the "dotnet" command line every way the sandbox could plausibly allow
// and reports which one, if any, actually starts. Loading the runtime library
// and starting a process are different kernel decisions, so this is the test
// that answers whether the editor can run the .NET CLI at all.
std::string probe_dotnet_exec(const std::string &dotnet_root, const std::string &files_dir, const std::string &cache_dir);

// Resolves the .NET SDK to run: by default the one installed by the oheco
// package manager, so the engine does not ship a copy and stays decoupled from
// its version. GODOT_OHOS_DOTNET_ROOT overrides it for testing. Returns an empty
// string when no usable installation is present.
std::string resolve_dotnet_root();

// HarmonyOS loads a plugin only from a directory that the process registered with
// the linker, and the registration only takes effect once the restricted
// ohos.permission.kernel.LOAD_INDEPENDENT_LIBRARY permission is granted. Returns a
// one line report of what the linker answered, for the diagnostic trace.
std::string add_independent_library_directory(const std::string &directory);

// Every directory registered so far, one line each, in registration order.
std::string plugin_directory_report();

// Ping-only IPv4 loopback TCP probe; a three-second total deadline bounds I/O.
// Discovery is parsed by ArkTS. The native API never accepts an external host.
// The instance ID and nonce detect stale responses; they are NOT authentication.
std::string probe_broker_tcp(uint16_t port, const std::string &instance_id);
