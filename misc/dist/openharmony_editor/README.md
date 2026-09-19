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
`godot-diagnostics.log`. Upgrading .NET is therefore an `oo install` away and
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

If these generated files are absent, this is the source template. Run
`platform/openharmony/export-editor-project.py` in the Godot source repository
with the validated native/managed inputs before building a full application.

The shell uses Vulkan only. Vulkan 1.4 headers do not require or guarantee a
Vulkan 1.4 device; capabilities are selected from the installed driver.

This is an experimental port. Consult the accompanying validation record before
assuming that GUI rendering, F5, .NET JIT in the HAP sandbox or external project
access have been verified. Terminal .NET validation alone does not establish
those application capabilities.

The first launch copies a C# verification project into the application's
`Projects/CSharpSmoke-4.7.2-ohos.2` directory. Import its `project.godot` from the
project manager and press F5. A successful run displays a rotating cube and a
`C# PASS` label. Existing example files are preserved on subsequent launches.

Before starting the editor, the app checks whether its bundled .NET SDK can
start. Diagnostic output is saved in the application cache as
`godot-dotnet-startup-<pid>.log`, including the actual app security domain and
subprocess error. A successful startup check still requires the example's
build/JIT and GUI checks to establish complete C# support.
