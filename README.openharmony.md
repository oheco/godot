# Godot Editor .NET for OpenHarmony

This branch adapts Godot **4.7.2-stable** for native ARM64 OpenHarmony development.
The intended deliverable is a **DevEco project**, containing the ArkTS UIAbility
shell, C++ N-API bridge, native Godot editor library, GodotSharp and a complete
native .NET SDK. The person installing it builds and signs the project in DevEco
with their own developer account.

**Experimental; application acceptance pending.** The native engine, complete
GodotSharp toolchain, real C# scene and same-process assembly reload have passed
on OpenHarmony. A complete DevEco project also builds an unsigned HAP. HAP
installation, Vulkan presentation, C# execution inside its application sandbox,
F5 and window lifecycle still need testing before calling this a working GUI
editor. The earlier ArkTS compiler fixture is not an installable Godot editor.

## Baseline and dependencies

- Upstream: `godotengine/godot`, `4.7.2-stable`, commit
  `ed1daf0bf001b61586d9930840f2f1394092c079`.
- Existing port: `kdada/godot`, `port-to-openharmony`, commit
  `bc9e9662253b879384a7332de99a1e8d6f7e0b1c`; merged with history retained.
- Adaptation repository: <https://github.com/oheco/godot>, branch
  `ohos/4.7.2-dotnet`.
- Vulkan-Headers **1.4.362**, volk regenerated against that registry, shader
  dependencies from **Vulkan SDK 1.4.357.0**. Exact archives, SHA-256 and sources:
  [`thirdparty/vulkan/openharmony-inputs.json`](thirdparty/vulkan/openharmony-inputs.json).
  Godot's vendored dependency licenses are retained.
- Native .NET runtime **10.0.12** and SDK **10.0.401** from
  <https://github.com/oheco/dotnet-runtime> and <https://github.com/oheco/dotnet-sdk>.
  Use the adapted OpenHarmony SDK; generic Linux ARM64 .NET cannot replace it.
- The managed build uses the pinned NuGet archives listed in
  [`platform/openharmony/dotnet/nuget-inputs.json`](platform/openharmony/dotnet/nuget-inputs.json).
  Every entry records its source, version, size, digest and license metadata.

New headers do not upgrade the operating system's Vulkan driver. The current
Maleoon 935 test host reports Vulkan **1.3.309**. The renderer must negotiate its
actual features and extensions. No system loader or driver is replaced.

## Native build inputs

Prepare these before the offline build:

- OpenHarmony SDK API **26**, including its native LLVM compiler and sysroot.
- Native Python 3 and SCons, available on PATH.
- The existing native LLVM tools and `binary-sign-tool` on PATH.
- The complete signed native .NET SDK layout and the pinned NuGet feed.
- DevEco Studio and the matching SDK for the final application build. Optional
  native Hvigor validation uses the user's adapted Node 24 and isolated Hvigor
  6.26.4 tooling; these tools are not installed by the project packager.

Commands below run on the OpenHarmony host, from this source checkout. Replace
`/path/to/...` with prepared local inputs; no build command downloads dependencies.

```sh
python3 -m SCons platform=openharmony target=editor arch=arm64 \
  module_mono_enabled=yes vulkan=yes opengl3=no generate_bundle=no \
  debug_symbols=no dev_build=no OPENHARMONY_SDK_PATH=/path/to/ohos-sdk/ohos -j2

python3 platform/openharmony/build-cli.py \
  --library bin/libgodot.openharmony.editor.arm64.so \
  --native-sdk /path/to/ohos-sdk/ohos/native --output /path/to/new-cli-directory

python3 platform/openharmony/build-dotnet.py \
  --godot /path/to/new-cli-directory/godot \
  --dotnet-sdk /path/to/native-dotnet-sdk \
  --nuget-feed /path/to/pinned-nuget-feed \
  --log /path/to/managed-build.log
```

The managed build uses an empty private NuGet cache and locked dependencies.
Godot package versions are pinned to **4.7.2-ohos.1** to avoid selecting upstream
packages with different bindings. Godot API/tool assemblies retain their upstream
.NET 8 target and run on .NET 10; newly created OpenHarmony C# projects target
.NET 10. A project-local `NuGet.Config` uses the bundled feed through the
`GODOT_NUGET_SOURCE` environment variable.

Run the native integration acceptance after the managed build:

```sh
python3 platform/openharmony/test-dotnet.py \
  --godot /path/to/new-cli-directory/godot \
  --godotsharp bin/GodotSharp \
  --dotnet-sdk /path/to/native-dotnet-sdk \
  --nuget-feed /path/to/pinned-nuget-feed \
  --output /path/to/new-csharp-check-directory
```

This checks a real C# scene, signals, private file access, worker-thread JIT/GC,
and rebuilding/reloading an assembly in one headless editor process. It uses a
project path containing spaces and removes loader-path overrides. HAP sandbox
and Vulkan presentation acceptance remain separate checks.

## Assemble a DevEco project

After the native editor and managed assemblies have been validated:

```sh
python3 platform/openharmony/export-editor-project.py \
  --library /path/to/new-cli-directory/libgodot.so \
  --godotsharp bin/GodotSharp \
  --dotnet-sdk /path/to/native-dotnet-sdk \
  --nuget-feed /path/to/pinned-nuget-feed \
  --output /path/to/new-GodotEditor-project \
  --archive /path/to/GodotEditor-4.7.2-ohos.1-project.zip
```

The script verifies required ARM64 inputs and the fixed feed. It packages the
SDK, managed tools, native headers, licenses and a resource digest manifest.
Runtime extraction uses the application's private files and cache directories.
Project files contain no maintainer signing profile, account or absolute SDK path.
The project archive includes a checksum file alongside it.

Open the extracted root directory in DevEco, select the SDK, configure automatic
signing with your own account, and build the `entry` module. The current shell
targets **2-in-1 devices**, API 22 or newer, and Vulkan only.

## Design and verification scope

The application keeps a lightweight launcher UIAbility alive and starts each
Godot editor/game in a separate UIAbility process attached to that launcher.
This avoids terminating a new editor when its project manager closes. A native
engine thread owns Godot initialization, input dispatch and teardown; ArkUI and
native VSync callbacks send it events. Spawn requests return the actual PID so
Godot can stop and observe the game process.

Current checks:

- Native CoreCLR embedding with callbacks, a native worker thread, JIT and
  compacting GC: passed in the terminal application's sandbox.
- Native offline build of GodotTools.ProjectEditor, Core and Shared from a fresh
  NuGet cache: passed.
- ArkTS UIAbility, window/input shell and runtime extraction code: compiled on
  OpenHarmony using the adapted Node runtime.
- C++ N-API shell: native compiler syntax check passed.
- SDK subprocess startup check and missing-SDK error: passed in the terminal
  security domain; the same check runs asynchronously inside the application.
- Offline Vulkan update in a separate directory: all 216 regenerated files
  matched the source tree, including the rebased Godot glslang patches.
- Full native Godot editor and complete GodotSharp Debug/Release/tools build:
  passed, with locked offline NuGet inputs.
- Real C# scene (bindings, signals, private files, worker-thread JIT and compacting
  GC), followed by assembly unload/reload in the same editor process: passed.
- Exported runtime extracted into a new private path containing spaces: all
  5,022 SDK file hashes matched; C# scene/build/reload passed again using the
  extracted SDK and GodotSharp, without loader-path overrides.
- Complete DevEco project with real engine, SDK and GodotSharp: native build of
  the N-API bridge, ArkTS and unsigned HAP passed; signed engine bytes preserved.
- Signed HAP installation, GUI rendering, input, process launch, sandboxed .NET
  execution and project portability: pending.

Current implementation uses embedded Godot dialogs in a single native window.
Native detached editor subwindows and operating-system file picker integration
are not implemented. Editing external documents requires a suitable granted
path or import into the application sandbox. Managed game export is not yet
validated; native editor C# support does not imply every export target works.
