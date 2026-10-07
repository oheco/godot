# Godot shared OpenHarmony host and .NET Editor

This branch adapts Godot **4.7.2-stable** for native ARM64 OpenHarmony development.
Games and the Editor use one [DevEco source project](<misc/dist/openharmony_template/README.md>)
and the same ArkTS/N-API/native engine host. Generated configuration selects the
application identity, startup policy and optional Editor/.NET environment. Both
profiles default to the oo **OpenHarmony SDK 26.0.0**, with minimum compatible
API **23**. Game device types default to **`default`**; the Editor remains
2-in-1-only because its external build tools/user-file permissions require that
profile. The person installing the Editor supplies their own signing material.

The Editor payload includes GodotSharp and an offline feed, **not a .NET SDK**.
The installed oheco SDK is an external runtime dependency of the SDK-enabled
Editor only; ordinary exported games have no such dependency.

**Experimental.** Native/headless tests, mock boundary tests, compilation and
signed HAP UI acceptance are separate validation scopes. A successful native C#
scene or unsigned HAP build does not establish permission, Vulkan, input or
multi-instance behaviour in the final signed application. Revalidate these
application behaviours after changing the common host.

## Desktop integration in the 4.7.2-ohos.4 candidate

- The ArkUI startup surface shows the application/Godot logo while runtime,
  templates and the native engine initialize. Log controls appear for failures
  and child-process status, rather than during normal startup.
- Editor auto scaling uses the system's display `densityPixels` (physical pixels
  per vp). It does not use font `scaledDensity`; manual Editor scale settings
  retain their usual behaviour. Display-scale changes take effect on restart.
- Editor filesystem dialogs and the initial new-project directory use
  `/storage/Users/currentUser` when user-file access is granted. Games without
  that permission start in their own private directory; resource/user-data
  dialogs and explicitly chosen paths keep their existing behaviour.
- Clipboard writes validate allocation and every UDMF stage. Reads use primary
  plain text across the entire pasteboard data set, with HTML plain-content
  fallback, and report permission/API failures without logging the copied text.
- This revision does **not** automatically request `READ_PASTEBOARD`, and
  installed clipboard acceptance is explicitly deferred. Cross-application
  reads may consequently fail. For an opt-in deployment, add
  `ohos.permission.READ_PASTEBOARD` to the project configuration only after
  obtaining its **system_basic, restricted user-grant** signing-profile ACL.
  A declaration or user dialog alone is insufficient. Clipboard writes do not
  require this read permission.
- File manager requests run asynchronously on the UIAbility thread and open
  Huawei File Manager (`com.huawei.hmos.filemanager/MainAbility`) at the selected
  directory or a file's parent. File selection highlighting is not guaranteed.
- Opening a system terminal or an external file/script editor is disabled in
  this revision. These menu actions display an unsupported-feature notification;
  they do not run a terminal command, start an external IDE, or wait for a
  process. Scripts continue to open in Godot's internal editor. File Manager
  requests and browser links remain available. This follows the installed
  device result: the broker could submit HiShell requests, but creation of a
  terminal in the requested directory was not established. Broker support for
  export/build and HDC operations is separate and remains available.

The common host ABI is now **3**. This includes the external application bridge
and the physical screen position of the actual XComponent surface used for IME
cursor placement. Rebuild editor and both game template libraries and rerun
`build-cli.py` before regenerating projects; ABI-1 and ABI-2 caches are rejected.
The managed Godot SDK remains `4.7.2-ohos.3`, since the managed API is unchanged.

Desktop input fixes in this candidate provide the velocity fields expected by
PopupMenu's hover filter and map the caret through canvas and Window transforms
once, then add the actual XComponent surface origin in physical screen pixels.
IME updates cache only successfully submitted coordinates so a transient SDK
failure does not suppress retries at the same caret. Real-control native tests
cover root and nested embedded windows at ordinary and 2x stretch. Installed
startup, popup IME, menu hover, File Manager and unsupported-feature acceptance
is recorded separately in [desktop-validation.md](<platform/openharmony/tests/desktop-validation.md>).

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

## One project, configured profiles

The [game host defaults](<misc/dist/openharmony_template/entry/src/main/resources/rawfile/godot_host.json>)
and [Editor preset](<platform/openharmony/profiles/editor.json>) configure one shell;
there is no second Editor DevEco template. `EntryAbility` is the stable entry for
both roles. Editor and template engine libraries are still distinct build targets.

[project_config.py](<platform/openharmony/project_config.py>) validates and applies
project-generation settings. The Editor generator accepts `--config overrides.json`;
only recognised fields can be overridden:

- `application`: bundleId, displayName, vendor, versionCode, versionName,
  deviceTypes, orientation (`system` means no manifest override), icons
  (foreground/background PNG paths).
- `build`: sdkVersion (default `26.0.0`, applied to compile/target), compatibleApi
  (default `23`), architectures. OpenHarmony SDK views use runtimeOS=OpenHarmony;
  legacy HarmonyOS version strings remain an explicit project-generation option,
  not an automatic fallback. The current Editor generator accepts ARM64 only.
- `engine.target`: editor, template_debug or template_release, matching the role
  and the separately compiled input library; this is not a runtime engine switch.
- `host`, `launch`, `instances`, `managed`, `window`, `diagnostics`: the runtime
  settings documented in the [shared project README](<misc/dist/openharmony_template/README.md>).
- `permissions`: additional declared permissions. Restricted kernel/sandbox
  capabilities require the explicit generator ACL flags instead.

A native-only Editor override can be as small as:

```json
{"managed":{"mode":"none"}}
```

Use a matching native editor library; GodotSharp, the .NET SDK and NuGet inputs
are not required in this mode. Native-only Editors still need user-file/external
code access and the weak sandbox to run the oo export tools. C# games keep host
managed.mode=none: the .NET export plugin embeds a signed self-contained runtime
in the PCK, rather than adding another runtime ZIP or an external SDK dependency.

Generate the game archive with the ordinary SCons target:

```sh
python3 -m SCons platform=openharmony target=template_debug arch=arm64 \
  module_mono_enabled=yes vulkan=yes opengl3=no generate_bundle=yes \
  debug_symbols=no dev_build=no OPENHARMONY_SDK_PATH="$NATIVE_SDK" -j4
```

Use `target=template_release` for release. Archives are staged outside the source
template and contain no IDE cache, signing credentials or stale native library.
The Godot exporter updates JSON fields structurally (including escaped app names,
permission Ability names and all SDK fields); old split-host templates must be
rebuilt. Its preset exposes version, orientation, system-area and diagnostic
settings, and project-only export does not require Hvigor to be installed.

Native build provenance carries a shared-host ABI version. Rebuild the engine and
rerun the current [build-cli.py](<platform/openharmony/build-cli.py>) when migrating
an old CLI cache; the generator rejects legacy provenance before changing a
project, instead of copying a library with incompatible bridge symbols.

`--update` migrates the former Editor entry to the common `EntryAbility`, removes
stale ArkTS/native sources, and preserves DevEco signing, dependency locks and
local settings. Required SDK/native-layout/no-strip fields are migrated; unsupported
local JSON5 syntax or structure fails before overwriting the project. Existing
comments and trailing commas are accepted; generated documents use the JSON subset
of JSON5. Unchanged profiles retain their exact bytes. Changed protected profiles
have durable rollback versions under `.godot-config-history` (never included in
project archives); a failed installation restores them without rewriting their
contents. After an uncatchable interruption, restore a missing/invalid profile
from that history before retrying. A project with personal signing configuration
cannot be archived for distribution: generate a fresh unsigned project instead.

## Native build inputs

Prepare these before the offline build:

- OpenHarmony SDK API **26**, including its native LLVM compiler and sysroot
  (`oo install ohos-sdk-native`).
- Native Python 3 and SCons, available on PATH (`oo install python3`).
- The existing native LLVM tools and `binary-sign-tool` on PATH
  (`oo install ohos-sdk-toolchains`).
- The packaged native .NET SDK (`oo install dotnet-sdk`) and the pinned NuGet feed.
- Native Node 24, oo Hvigor adapter **6.26.4-ohos.1**, all five matching SDK
  components, and the fixed SDK view. These export tools are not installed by
  the project packager. No Java, broker, npx download or SDK file patch is used.
- User-supplied signing material for signed HAP/APP export or device deployment.
  DevEco remains optional for manually building/signing the generated Editor project.

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

## On-device export, run and GDScript debug

Prepare the external build tools once (oheco 0.10.0 or newer):

```sh
oo install nodejs
NPM_CONFIG_REGISTRY=https://repo.harmonyos.com/npm/ oo install hvigor
oo install ohos-sdk-native@26.0.0.35-Beta ohos-sdk-ets@26.0.0.35-Beta \
  ohos-sdk-js@26.0.0.35-Beta ohos-sdk-toolchains@26.0.0.35-Beta \
  ohos-sdk-previewer@26.0.0.35-Beta
oo sdk create 26.0.0.35-Beta
"$(npm prefix --global)/bin/hvigor" --adapter-info
```

The native exporter discovers the active oo Node/global adapter and SDK view.
Editor Settings exposes `export/openharmony/{node_path,hvigor_entry,sdk_root,hdc_path}`
for explicit paths; empty settings mean automatic discovery. C# build/publish also
uses SDK Root to resolve `toolchains/lib/binary-sign-tool`, or the oo command when
no root is configured, and passes the absolute `OpenHarmonySigningTool` property
in the MSBuild child environment. Only that child's PATH is extended; a GUI
launch does not need the terminal's PATH. No fifth signing-tool setting is required.
A project-only export
needs no build tools. Full HAP/APP export calls the template's
[build runner](<misc/dist/openharmony_template/tools/BUILD_RUNNER.md>) directly with
Node, uses only installed dependencies, and never changes the Editor's global
PATH/cwd to imitate DevEco. Credentials are passed in a checked private cache
request, not command arguments; generated signing configuration uses environment
references and the original profile is restored after the build. Error output is
redacted. Signed exports require the user's actual certificate/profile/keystore;
file-signature validation does not prove device trust.

Build both `module_mono_enabled=yes` templates. Bundle them with the Editor using
`--game-template-debug` and `--game-template-release` below. On startup only the
Editor streams/verifies these archives into private digest-versioned storage;
ordinary games do not prepare templates. This is separate from `runtime.zip`.
Old/manual export templates remain a fallback when no bundled manifest exists.

- F5/F6 launch another attached UIAbility process with the project/scene/debugger
  arguments. F8 stops the recorded game PID; no HAP packaging or HDC is involved.
- Remote Deploy requires an authorized HDC target and signing preset. Every
  install/start/forward command names that target, uses the actual debugger port
  and loopback address, and checks success. Debug/file-server deployments add
  INTERNET. Only owned forwarding rules are removed. F8 and debugger disconnect
  explicitly stop the device bundle; failed cleanup is retained for retry before
  the next deployment, not silently forgotten. Do not concurrently manage the
  same device ports from independent HDC clients.
- On HarmonyOS, **Editor Settings → Export → OpenHarmony → Use Broker** is
  enabled by default. Start `oheco-broker shell serve` in the terminal (`oo install
  oheco-broker` if needed). The editor reads `~/.oheco/broker/endpoint`, independently
  of the SDK/tool root, and connects to its numeric loopback endpoint;
  there is no port setting. Device discovery, HDC installation, ability start/stop,
  debugger/file-server port forwarding and forwarding cleanup use that service.
  Commands keep their executable and argument arrays; they are never shell strings.
  Broker failures are reported without retrying or falling back to direct HDC.
- The native HDC socket at `/data/hdc/hdc_debug/hdc_server` is absent from the
  inspected editor application's sandbox: direct `hdc -v` works, but `hdc list
  targets` retries `ENOENT` and returns exit code 0 with empty output. The broker
  runs in the terminal's namespace, where HDC can access the existing service.
  The editor does not request the restricted `MOUNT_HDCDEBUG_PATH` permission.
  Remote Deploy builds its DevEco project and signed debug HAP under the current
  project's `.godot/openharmony/run_<pid>_<ticks>/` directory. HDC receives that same
  absolute HAP path through the broker; the command connection carries only
  executable/arguments and output. The project directory must be accessible to both
  the editor and the terminal running the broker. The owned run directory is
  removed after deployment.
  OpenHarmony export/build/sign operations and local UIAbility F5/F6/F8 execution
  retain their existing toolchain. Turning Use Broker off selects direct HDC.
- Debug templates support GDScript breakpoints, stack/locals, step/continue and
  Remote SceneTree. Release templates intentionally reject remote debugging.
- C# ARM64 games use .NET 10 NativeAOT Shared for `openharmony-arm64` in both
  `ExportDebug` and `ExportRelease`. Use `Godot.NET.Sdk/4.7.2-ohos.3` or newer:
  this SDK preserves GodotSharp, the game assembly and referenced source-project
  assemblies so trimming keeps script constructors and generated bridges.
  Existing projects must update their SDK version; the exporter rejects older
  SDKs before publishing. Normal editor `Debug` builds retain CoreCLR/JIT.
- The AOT game library is packaged as `libs/arm64-v8a/libgodot-csharp-game.so`
  with its ICU, OpenSSL and C++ dependencies beside `libgodot.so`. The PCK holds
  scenes, resources and a small NativeAOT initialization record. Ordinary IL
  DLLs and the CoreCLR/JIT runtime are excluded. Existing native signatures are
  retained and missing signatures are added only to export copies. When debug
  symbols are enabled, they are saved beside the export under `<name>.symbols`.
- NativeAOT games initialize through the HAP library namespace without extracting
  executable code into private data or registering independent library paths.
  Generated games do not request `kernel.LOAD_INDEPENDENT_LIBRARY` or
  `kernel.ALLOW_WRITABLE_CODE_MEMORY`. The editor keeps its development SDK and
  permissions. Editor-only references are rejected before AOT compilation.
  Third-party code must support AOT; dynamic assembly loading, runtime compilation
  and arbitrary reflection may require changes or explicit trimming roots.
  Compiler trimming/AOT warnings remain visible.
  **C# source breakpoints/stepping are not implemented by this adaptation.**

Native CLI/PCK, unsigned Hvigor HAP/APP, protocol-peer and fixture tests are
separate from a signed application's UIAbility/sandbox/permission and physical
HDC-device acceptance. A minimum-API field does not prove all older devices work.

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
  --game-template-debug bin/openharmony_debug_arm64-v8a.zip \
  --game-template-release bin/openharmony_release_arm64-v8a.zip \
  --output /path/to/GodotEditor

# later iterations: same directory, no new copy to open
python3 platform/openharmony/export-editor-project.py \
  --library /path/to/new-cli-directory/libgodot.so \
  --godotsharp bin/GodotSharp \
  --dotnet-sdk "$DOTNET_SDK" \
  --nuget-feed /path/to/pinned-nuget-feed \
  --game-template-debug bin/openharmony_debug_arm64-v8a.zip \
  --game-template-release bin/openharmony_release_arm64-v8a.zip \
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

The checks above describe the historical 4.7.2-ohos.2 baseline. For the current
4.7.2-ohos.4 desktop candidate, native Editor/debug/release builds, offline
managed assemblies, CompileArkTS and signed HAP installation were repeated.
The installed user acceptance covers the official startup logo, popup IME
alignment, main/context/submenu hover, File Manager and unsupported external
app notifications. Native tests additionally cover caret geometry, mouse motion
and retrying a failed IME cursor notification without falsely caching success;
see [desktop-validation.md](<platform/openharmony/tests/desktop-validation.md>)
for artifact hashes and the distinction between captured and user-reported
checks. Final clean-source release provenance and catalog installation are
recorded separately from this installed candidate.

End users still need their own signing account/profile. Process launch,
sandboxed GUI .NET execution and project portability beyond the explicitly
recorded fixtures are not established by the desktop input acceptance.

Current implementation uses embedded Godot dialogs in a single native window.
Native detached editor subwindows and operating-system file picker integration
are not implemented. Editing external documents requires a suitable granted
path or import into the application sandbox.

The managed toolchain now publishes for `openharmony-arm64` offline. The export
plugin's own game-export path still drives the DevEco/JBR command-line tools and
has not been reworked for the native toolchain, so exporting a game from inside
the device editor is not yet validated.
