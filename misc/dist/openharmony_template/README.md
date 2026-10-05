# Godot — shared OpenHarmony DevEco host

This is the **only** DevEco source project for exported games and the Godot
Editor. Both use the same Ability, ArkTS page, input mapping, N-API contract and
engine thread. Defaults use the oo OpenHarmony SDK **26.0.0**, minimum compatible
API **23**, ARM64 and Vulkan. Games use deviceTypes **`default`**. Supply your own
signing material when needed; no credentials are shipped and Java is not required.

## Configuration, not a second shell

The generated [host configuration](<entry/src/main/resources/rawfile/godot_host.json>)
selects runtime behaviour. Application identity, supported devices, permissions,
SDK and multi-instance declarations are generated alongside it. Do not change
only the runtime role of an already packaged app: its manifest and engine library
must agree with that role.

| Field | Game | Editor preset |
| --- | --- | --- |
| `host.role` | `game` | `editor` |
| `launch.defaultMode` | `packaged-game` | `project-manager` |
| `launch.defaultArguments` | empty | `--single-window` |
| `launch.acceptProjectRequests` | false | true |
| `instances.policy` | `disabled` | `editor` |
| `instances.maxCount` | unused | 5 (range 1–5) |
| `managed.mode` | `none` | `sdk`, or `none` with a matching engine |
| `managed.sdkSource` | unused | `oheco` |
| `window.expandIntoSystemArea` | true | false |
| `diagnostics.level` | `normal` | `normal` (`verbose` is optional) |

`schemaVersion` is 1. Unknown fields, unsupported modes and contradictory
combinations fail explicitly. C# ARM64 games publish a .NET 10 NativeAOT shared
library and signed native dependencies into the HAP's `libs/arm64-v8a` directory.
The PCK contains scenes, resources and a small NativeAOT initialization record;
the engine loads native code through the HAP library namespace. A game never
extracts Editor runtime resources or depends on an installed .NET SDK. Current
native multi-window support is unchanged; application instances are not detached
Godot subwindows.

Storage scope is preserved: games use ApplicationContext files/cache directories
(the legacy application-wide `user://` root), while the Editor retains its
UIAbility/module-private directories. Paths come from the corresponding context,
not a hardcoded developer machine path. All roles request the user-grant
permissions listed in their generated resources through one shared path.

Both roles use a fixed `EntryAbility` entry point. Editor project-open requests
and new-instance launches address that same Ability. Project-manager → editor
uses an independent application instance; Run Project retains its attached-child
policy. The host role describes capabilities, not whether this particular engine
invocation is editing or playing a project.

## Generated payloads

All native libraries use `entry/libs/<ABI>/libgodot.so`. Public bridge headers are
placed in `entry/src/main/cpp/include`. Do not strip or rewrite a signed library
after signing; the shared DevEco build disables stripping.

- Games contain their PCK and export arguments in the raw resources. The engine
  loads the PCK explicitly; the native host merges the exported command line with
  configured arguments.
- The .NET Editor additionally contains `runtime.zip` and its size/SHA-256
  manifest. It extracts GodotSharp, a C# example and an offline NuGet feed into
  private storage. Concurrent starts use separate staging paths.
- The Editor may also carry the debug/release export template archives and their
  digest manifest. Only Editors publish these into private versioned storage;
  games do not run template preparation.
- The .NET SDK is **not** shipped. Install it using `oo install dotnet-sdk`.
  The Editor resolves the oheco package, honouring `OHECO_ROOT` and the explicit
  `GODOT_OHOS_DOTNET_ROOT` environment override.

If the native payloads are absent, this is the source template, not a ready-to-build
application. Generate it from the Godot source checkout before building in DevEco.

## Editor permissions and tools

Access to user projects and oo Node/Hvigor/SDK needs `ohos.permission.READ_WRITE_USER_FILE` (user grant) and
`ohos.permission.ALLOW_EXTERNAL_NATIVE_CODE`. These restricted permissions need
matching ACL entries in the signing profile and are limited to 2-in-1 apps.
Declaring an unapproved permission can prevent installation; setting a host flag
does not grant it. The shared host passes actual permission status to Godot.

The Editor generator only declares these additional restricted permissions when
explicitly requested and authorised in the signing profile:

- `--load-independent-library`: `ohos.permission.kernel.LOAD_INDEPENDENT_LIBRARY`;
- `--writable-code-memory`: `ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY`;
- `--custom-sandbox`: `ohos.permission.CUSTOM_SANDBOX`, needed to execute the
  adhoc-signed oo Node/SDK/.NET tools from their user directories.

SDK and .NET tools run as direct child processes. The native Editor's HDC commands
use `oheco-broker shell serve` when **Use Broker** is enabled, with automatic
`HOME/.oheco/broker/endpoint` discovery. Remote Deploy builds its project and HAP
under the current project's `.godot` and passes the same absolute HAP path to HDC.
The private tool environment uses `GODOT_SHARP_ROOT`, `GODOT_NUGET_SOURCE`,
`NUGET_PACKAGES`, `DOTNET_CLI_HOME` and `TMPDIR`. User projects and existing NuGet
configuration are not overwritten by host initialization.

For full export, prepare oo Node, the **6.26.4-ohos.1** Hvigor adapter and a fixed
SDK view from `oo sdk create 26.0.0.35-Beta`. The native Editor resolves their paths;
[the build runner](<tools/BUILD_RUNNER.md>) does not download dependencies, launch
Java or modify official packages. F5/F6 use the Editor's attached local game
process, not HAP installation. Remote Deploy uses an authorized HDC target,
checked target-scoped port forwarding, and explicit remote Stop/cleanup.

Legacy CoreCLR C# games need `ohos.permission.kernel.LOAD_INDEPENDENT_LIBRARY` and
`ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY` ACLs. NativeAOT exports use
signed HAP libraries and do not require these runtime ACLs; the exporter declares
them only for the legacy CoreCLR path. No Editor SDK-access permissions are
borrowed. GDScript debug templates support breakpoints, locals, step/continue and
RemoteSceneTree; C# source debugging is not implemented.

## Diagnostics and validation

Per-process engine, diagnostic and crash logs use application-private storage,
not a developer workstation path. Runtime argument or permission failures must
be reported, not converted into a different launch mode.

Configuration tests and compilation are distinct from signed HAP acceptance.
Before release, verify game PCK/arguments/permissions, input (including IME),
resize/background/exit, and Editor cold/warm startup, multi-instance handoff,
Run/Stop and .NET build/run on the actual signed target application. No new system picker
or detached-window support is implied by sharing this project.
