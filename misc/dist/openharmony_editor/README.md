# Godot Editor .NET — DevEco project

Open this directory in DevEco Studio with OpenHarmony SDK API 26. Select an ARM64
2-in-1 device (API 22+), configure automatic signing using your own developer
account, and build/run the `entry` module. No signing credentials are included.

The prepared project contains `entry/libs/arm64-v8a/libgodot.so`, the N-API bridge
headers, and `entry/src/main/resources/rawfile/runtime.zip`. The resource archive
contains GodotSharp, the packaged `oheco/dotnet-sdk` 10.0.401-ohos.2 (target RID
`openharmony-arm64`) and an offline NuGet feed. On first launch these are
extracted to private application storage. Keep adequate free space for both the
archive and the extracted SDK.

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
