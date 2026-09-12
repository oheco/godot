# Validation status

This is an experimental Godot 4.7.2 OpenHarmony ARM64 editor project, with
GodotSharp 4.7.2-ohos.2 and the packaged `oheco/dotnet-sdk` 10.0.401-ohos.2
(runtime 10.0.12, target RID `openharmony-arm64`).
`build-inputs.json` identifies the exact engine commit and input file digests;
`entry/src/main/resources/rawfile/runtime-manifest.json` identifies the runtime
archive. The project contains no account credentials or application signature.

## Passed on the OpenHarmony development host

- Full native Godot editor build from a clean tree with the packaged OpenHarmony
  SDK (ARM64, Vulkan only, OpenGL disabled).
- Signed headless editor startup and a complete offline GodotSharp
  Debug/Release/tools build using the packaged `dotnet-sdk` 10.0.401-ohos.2.
- Real C# scene: Node bindings, signals, Vector3, private-file names,
  worker-thread DynamicMethod JIT and compacting garbage collection.
- Rebuild and assembly-context unload/reload in one running headless editor.
- Self-contained managed publish for `openharmony-arm64` with an empty package
  source and an empty NuGet cache: `libcoreclr.so`, the JIT/GC/host libraries,
  ICU and OpenSSL are present as musl AArch64 ELF.
- Project audit: all 5,452 packaged SDK files matched the recorded inventory and
  the project contains no signing material or absolute host path.

These native runtime checks ran in the terminal application's security domain
`u:r:hishell_hap:s0`. They do not establish permissions in this application's
debug HAP sandbox. The host GPU reported Vulkan 1.3.309; the bundled headers are
1.4.362 and shader dependencies are pinned to Vulkan SDK 1.4.357.0.

## Required application checks after DevEco signing

1. Build and install `entry` using your own DevEco account. Start the app and
   allow the bundled runtime to finish extracting into private app storage.
2. Check the bundled SDK startup result. Failure details, the security domain
   and subprocess errno are in app cache `godot-dotnet-startup-<pid>.log`.
3. Confirm the project manager renders through Vulkan. Import the bundled
   `Projects/CSharpSmoke-4.7.2-ohos.2/project.godot`, build C# and press F5;
   expect a rotating cube and `C# PASS` label.
4. Check stop/restart, switching projects, C# rebuild, keyboard shortcuts,
   mouse movement/buttons/wheel, Chinese input, resize and background/restore.

No signed Godot HAP has yet been installed or run. SDK subprocess execution,
embedded CoreCLR JIT, Vulkan presentation and these UI checks remain unverified
in the target application's sandbox. No system security policy is modified.

The current implementation uses embedded dialogs in one native window.
Detached editor subwindows and the system file picker are not implemented.
External projects need an accessible granted path or import into private
storage. The managed toolchain publishes for `openharmony-arm64` offline, but
exporting a game from inside the device editor is not yet validated.
