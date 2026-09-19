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
- Native .NET runtime **10.0.12** and SDK **10.0.401** from the packaged
  <https://github.com/oheco/dotnet-sdk> release, installed with
  `oo install dotnet-sdk` (version `10.0.401-ohos.2`). Its target RID is
  **openharmony-arm64**. A generic Linux ARM64 .NET SDK cannot replace it.
- The SDK is a **build input and a runtime dependency, never a shipped one**:
  GodotSharp is compiled against it, and the application resolves the installed
  package at startup (`~/.oheco/packages/dotnet-sdk/<version>`, honouring
  `OHECO_ROOT`, with `GODOT_OHOS_DOTNET_ROOT` as a test override). The project
  therefore carries no .NET SDK and stays decoupled from its version; upgrading
  it is `oo install dotnet-sdk` on the device.
- Reaching the SDK requires the restricted permissions
  `ohos.permission.READ_WRITE_USER_FILE` (user-grant) and
  `ohos.permission.ALLOW_EXTERNAL_NATIVE_CODE` (system-grant). Both are
  `system_basic`, available to 2-in-1 device applications only, and the signing
  profile's ACL must carry them. Declaring a restricted permission without that
  ACL makes the **installation** fail with `grant request permissions failed`
  (9568289), so the ACL has to be applied for before the first install. In the
  debug phase DevEco's automatic signing submits that application to AGC for you
  and a short-lived temporary profile covers the wait.
- The managed build uses the pinned NuGet archives listed in
  [`platform/openharmony/dotnet/nuget-inputs.json`](platform/openharmony/dotnet/nuget-inputs.json).
  Every entry records its source, version, size, digest and license metadata.

New headers do not upgrade the operating system's Vulkan driver. The current
Maleoon 935 test host reports Vulkan **1.3.309**. The renderer must negotiate its
actual features and extensions. No system loader or driver is replaced.

## Native build inputs

Prepare these before the offline build:

- OpenHarmony SDK API **26**, including its native LLVM compiler and sysroot
  (`oo install ohos-sdk-native`).
- Native Python 3 and SCons, available on PATH (`oo install python3`).
- The existing native LLVM tools and `binary-sign-tool` on PATH
  (`oo install ohos-sdk-toolchains`).
- The packaged native .NET SDK (`oo install dotnet-sdk`) and the pinned NuGet feed.
- DevEco Studio and the matching SDK for the final application build. Optional
  native Hvigor validation uses the user's adapted Node 24 and isolated Hvigor
  6.26.4 tooling; these tools are not installed by the project packager.

Commands below run on the OpenHarmony host, from this source checkout. The `oo`
package manager installs every toolchain input; no build command downloads
anything. Adjust the `ohos-sdk-native` and `dotnet-sdk` versions to what
`oo list` reports.

```sh
OO_ROOT=${OHECO_ROOT:-$HOME/.oheco}
NATIVE_SDK=$OO_ROOT/packages/ohos-sdk-native/26.0.0.35-Beta
DOTNET_SDK=$OO_ROOT/packages/dotnet-sdk/10.0.401-ohos.2

python3 -m SCons platform=openharmony target=editor arch=arm64 \
  module_mono_enabled=yes vulkan=yes opengl3=no generate_bundle=no \
  debug_symbols=no dev_build=no OPENHARMONY_SDK_PATH="$NATIVE_SDK" -j4

python3 platform/openharmony/build-cli.py \
  --library bin/libgodot.openharmony.editor.arm64.so \
  --native-sdk "$NATIVE_SDK" --output /path/to/new-cli-directory

python3 platform/openharmony/build-dotnet.py \
  --godot /path/to/new-cli-directory/godot \
  --dotnet-sdk "$DOTNET_SDK" \
  --nuget-feed /path/to/pinned-nuget-feed \
  --log /path/to/managed-build.log
```

The managed build uses an empty private NuGet cache and locked dependencies.
Godot package versions are pinned to **4.7.2-ohos.2** to avoid selecting upstream
packages with different bindings. Godot API/tool assemblies retain their upstream
.NET 8 target and run on .NET 10; newly created OpenHarmony C# projects target
.NET 10. A project-local `NuGet.Config` uses the bundled feed through the
`GODOT_NUGET_SOURCE` environment variable.

Run the native integration acceptance after the managed build:

```sh
python3 platform/openharmony/test-dotnet.py \
  --godot /path/to/new-cli-directory/godot \
  --godotsharp bin/GodotSharp \
  --dotnet-sdk "$DOTNET_SDK" \
  --nuget-feed /path/to/pinned-nuget-feed \
  --output /path/to/new-csharp-check-directory
```

This checks a real C# scene, signals, private file access, worker-thread JIT/GC,
and rebuilding/reloading an assembly in one headless editor process. It uses a
project path containing spaces and removes loader-path overrides. HAP sandbox
and Vulkan presentation acceptance remain separate checks.

## Assemble a DevEco project

After the native editor and managed assemblies have been validated, export once
into a directory you keep open in DevEco, then refresh that same directory in
place on every later iteration:

```sh
python3 platform/openharmony/export-editor-project.py \
  --library /path/to/new-cli-directory/libgodot.so \
  --godotsharp bin/GodotSharp \
  --dotnet-sdk "$DOTNET_SDK" \
  --nuget-feed /path/to/pinned-nuget-feed \
  --output /path/to/GodotEditor

# later iterations: same directory, no new copy to open
python3 platform/openharmony/export-editor-project.py \
  --library /path/to/new-cli-directory/libgodot.so \
  --godotsharp bin/GodotSharp \
  --dotnet-sdk "$DOTNET_SDK" \
  --nuget-feed /path/to/pinned-nuget-feed \
  --output /path/to/GodotEditor --update
```

The script verifies the ARM64 inputs, the .NET SDK it was built against (RID,
reference and runtime packs) and the fixed feed. It packages the signed editor
library, Godot's managed assemblies and native tools, the pinned NuGet feed,
native headers, licenses and a resource digest manifest — but **no .NET SDK**,
which the application resolves from the `oo` installation at startup. Runtime
extraction uses the application's private files and cache directories. Project
files contain no maintainer signing profile, account or absolute SDK path.

With `--update`, files DevEco owns after the first build are preserved:
`build-profile.json5` (automatic signing configuration), `oh-package-lock.json5`
and `entry/oh-package-lock.json5`, `local.properties`, and `.clang-tidy`/
`.clangd`. Everything the packager generates is replaced, so the signing
configuration and cached build state survive across iterations. A ZIP via
`--archive` is optional and not needed while iterating.

The packager stops at the project: it neither builds nor signs a HAP, and it does
not publish anything. Published adaptation packages carry immutable project
archives in the `projects` field of their version descriptor, which `oo export`
downloads, verifies and extracts.

Open the project root in DevEco, select the SDK, configure automatic signing with
your own account, and build the `entry` module. The shell targets **2-in-1
devices** with the HarmonyOS SDK **6.1.0(23)** and Vulkan only. The engine itself
is still compiled against the OpenHarmony native SDK, which is a separate input.

The application needs `ohos.permission.READ_WRITE_USER_FILE` and
`ohos.permission.ALLOW_EXTERNAL_NATIVE_CODE` to read and run the `oo`-installed
.NET SDK outside its sandbox. Both are in the platform's restricted-permission
list with exactly this scenario — an IDE or developer tool running on PC/2-in-1 —
and both are `system_basic`, so the signing profile needs an AGC-approved ACL
before the first install succeeds. In the debug phase, signing automatically from
DevEco submits the application for you; a temporary profile is issued while it is
pending (around three working days). `READ_WRITE_USER_FILE` is user-grant, so the
application also requests it at runtime.

## Design and verification scope

The application keeps a lightweight launcher UIAbility alive and starts each
Godot editor/game in a separate UIAbility process attached to that launcher.
This avoids terminating a new editor when its project manager closes. A native
engine thread owns Godot initialization, input dispatch and teardown; ArkUI and
native VSync callbacks send it events. Spawn requests return the actual PID so
Godot can stop and observe the game process.

Checks for the **4.7.2-ohos.2** project:

- Full native Godot editor build from a clean tree with the packaged OpenHarmony
  SDK, ARM64, Vulkan only, OpenGL disabled: passed (2,663 translation units,
  49m19s, `libgodot.so` SHA-256 `eec572f470f1fe784fcd513e5f16903cbbfd30e54cd604b451bc0a964e7966fd`).
- Complete offline GodotSharp Debug/Release/tools build with the packaged
  `dotnet-sdk` 10.0.401-ohos.2 (RID `openharmony-arm64`) and locked NuGet inputs:
  passed; the managed tree only contains `4.7.2-ohos.2` packages.
- Real C# scene (bindings, signals, private files, worker-thread JIT and
  compacting GC), followed by assembly unload/reload in the same headless editor
  process: passed using the packaged SDK.
- Managed publish for `openharmony-arm64`: passed offline with an empty package
  source and an empty NuGet cache. The self-contained output contains
  `libcoreclr.so`, `libclrjit.so`, `libclrgc.so`, `libhostfxr.so`, ICU and
  OpenSSL as musl AArch64 ELF, plus `Smoke.dll` and `GodotSharp.dll`.
- DevEco project assembly and independent audit: the runtime manifest matched the
  archive, the project bundles no .NET SDK and records the SDK requirement it
  expects to find on the device, and it contains no signing material or absolute
  host path.

Earlier rounds additionally passed the ArkTS/Hvigor build of the DevEco project
and an unsigned HAP, offline Vulkan dependency regeneration, and runtime
extraction into a path containing spaces. The template changed in this round, so
those application-level checks were not repeated.

Signed HAP installation, GUI rendering, input, process launch, sandboxed .NET
execution and project portability: pending, to be performed in DevEco with your
own account.

Current implementation uses embedded Godot dialogs in a single native window.
Native detached editor subwindows and operating-system file picker integration
are not implemented. Editing external documents requires a suitable granted
path or import into the application sandbox.

The managed toolchain now publishes for `openharmony-arm64` offline. The export
plugin's own game-export path still drives the DevEco/JBR command-line tools and
has not been reworked for the native toolchain, so exporting a game from inside
the device editor is not yet validated.
