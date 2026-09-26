# Godot Editor .NET — DevEco project

Open this directory in DevEco Studio with the HarmonyOS SDK **6.1.0(23)**. Select an
ARM64 2-in-1 device, configure automatic signing using your own developer
account, and build/run the `entry` module. No signing credentials are included.

The prepared project contains `entry/libs/arm64-v8a/libgodot.so`, the N-API bridge
headers, and `entry/src/main/resources/rawfile/runtime.zip`. The resource archive
contains GodotSharp, the bundled C# example and an offline NuGet feed, which are
extracted to private application storage on first launch.

The .NET SDK is **not** bundled. Install it on the device with
`oo install dotnet-sdk`; the application resolves the installed package under
`~/.oheco/packages/dotnet-sdk` at startup and reports what it found in
`godot-<pid>-diagnostics.log`. Upgrading .NET is therefore an `oo install` away and
needs no new Godot build.

Reaching that directory requires the restricted permissions
`ohos.permission.READ_WRITE_USER_FILE` (user-grant) and
`ohos.permission.ALLOW_EXTERNAL_NATIVE_CODE` (system-grant), which the module
declares and which are available to 2-in-1 device applications only. Both are
`system_basic`, so the signing profile needs an AGC-approved ACL before the first
install succeeds; installing while the ACL is still pending is rejected with
`grant request permissions failed` (9568289). In the debug phase, signing
automatically from DevEco submits the application to AGC for you and a
short-lived temporary profile covers the wait. The editor asks for the user-grant
permission at startup.

## Starting executables from the user directory

The `oo`-installed `.NET` host is **adhoc** (self-signed). A strong sandbox refuses
adhoc binaries even with `ohos.permission.ALLOW_EXTERNAL_NATIVE_CODE`.
`ohos.permission.CUSTOM_SANDBOX` instead makes the application a **weak sandbox**,
which may start normal, debug and adhoc binaries alike. The exporter declares it
only with `--custom-sandbox`, and it needs its own AGC-approved ACL entry in the
signing profile; otherwise the install fails the same way.

This permission is what makes the editor able to run `dotnet` itself, so the
exporter now passes `--custom-sandbox` for the editor project. Because a weak
sandbox is a broad capability, the declaration stays an explicit export argument
rather than a default of the template.

The editor runs tool commands as **direct child processes**: `OS.execute`,
`execute_with_pipe` and the GodotTools build path all use the normal local process
APIs. No helper service, endpoint file or socket is involved, and nothing has to be
started in a terminal before building.

Measured on Maleoon 935 / HarmonyOS 6.1.0(23) with the permission granted:

* the sandbox domain becomes `u:r:develop_tools_debug_hap:s0` instead of
  `u:r:debug_hap:s0`;
* `<dotnet-sdk>/dotnet --version` runs and prints `10.0.401`;
* the C# smoke project builds and runs (`C# PASS`).

If a build ever fails with `EPERM`, confirm that the installed signing profile
still carries the `CUSTOM_SANDBOX` ACL: without it the application is a strong
sandbox again and refuses the adhoc .NET host.

## Tool environment

The extracted runtime stays in application-private storage, which is reachable by
the editor's own child processes:

| Variable | Value |
| --- | --- |
| `GODOT_SHARP_ROOT` | extracted `runtime/GodotSharp` |
| `GODOT_NUGET_SOURCE` | extracted `runtime/nuget` (offline feed) |
| `NUGET_PACKAGES` | `filesDir/nuget` |
| `DOTNET_CLI_HOME` | `filesDir/dotnet-cli` |
| `TMPDIR` | `cacheDir/tmp` |

New Godot C# projects get a `NuGet.Config` pointing at
`%GODOT_NUGET_SOURCE%`. An existing project's own configuration is left untouched,
so extra package sources keep working; add the bundled feed alongside them if a
project replaces the default sources.

The C# verification project is copied into the application's private storage on
first launch as `Projects/CSharpSmoke-4.7.2-ohos.2`. Import its `project.godot`
from the project manager and press F5; a successful run displays a rotating cube
and a `C# PASS` label. Existing example files are preserved on later launches.

PM/editor UIAbility multi-instance handoff and Run Project keep their native
application launch mechanism. Export templates do not gain anything from this
document: no helper library is linked and generated games have no extra dependency
or managed reference. Arbitrary `System.Diagnostics.Process` calls in editor
plugins or game code are not intercepted.

If these generated files are absent, this is the source template. Run
`platform/openharmony/export-editor-project.py` in the Godot source repository
with the validated native/managed inputs before building a full application.

The shell uses Vulkan only. Vulkan 1.4 headers do not require or guarantee a
Vulkan 1.4 device; capabilities are selected from the installed driver.

This is an experimental port. Consult the accompanying validation record before
assuming that GUI rendering, F5, .NET JIT in the HAP sandbox or external project
access have been verified. Terminal .NET validation alone does not establish
those application capabilities. Per-process engine/diagnostic/crash logs remain.
