# Validation status

This is an experimental Godot 4.7.2 OpenHarmony ARM64 editor project, with
GodotSharp 4.7.2-ohos.1, native .NET SDK 10.0.401 and runtime 10.0.12.
`build-inputs.json` identifies the exact engine commit and input file digests;
`entry/src/main/resources/rawfile/runtime-manifest.json` identifies the runtime
archive. The project contains no account credentials or application signature.

## Passed on the OpenHarmony development host

- Native Godot editor build with Vulkan enabled and OpenGL disabled.
- Signed headless editor startup and complete offline GodotSharp build.
- Real C# scene: Node bindings, signals, Vector3, Chinese private-file names,
  worker-thread DynamicMethod JIT and compacting garbage collection.
- Rebuild and assembly-context unload/reload in one running headless editor.
- Complete runtime extraction into a new private directory containing spaces;
  all 5,022 SDK file hashes checked, then C# build/run/reload repeated using the
  extracted SDK and GodotSharp without loader search-path overrides.
- Full native DevEco/Hvigor build of the ArkTS/C++ project and unsigned HAP using
  the user's adapted Node runtime. Packaging preserved signed engine bytes.
- Offline Vulkan dependency regeneration matched all 216 checked source files.

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
   `Projects/CSharpSmoke-4.7.2-ohos.1/project.godot`, build C# and press F5;
   expect a rotating cube and `C# PASS` label.
4. Check stop/restart, switching projects, C# rebuild, keyboard shortcuts,
   mouse movement/buttons/wheel, Chinese input, resize and background/restore.

No signed Godot HAP has yet been installed or run. SDK subprocess execution,
embedded CoreCLR JIT, Vulkan presentation and these UI checks remain unverified
in the target application's sandbox. No system security policy is modified.

The current implementation uses embedded dialogs in one native window.
Detached editor subwindows and the system file picker are not implemented.
External projects need an accessible granted path or import into private
storage. Managed game export is not yet validated.
