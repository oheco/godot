# Godot shared OpenHarmony host and .NET Editor

This branch adapts Godot **4.7.2-stable** for native ARM64 OpenHarmony development.
Games and the Editor use one [DevEco source project](<misc/dist/openharmony_template/README.md>)
and the same ArkTS/N-API/native engine host. Generated configuration selects the
application identity, startup policy and optional Editor/.NET environment. Both
profiles default to the oo **OpenHarmony SDK 26.0.0**, with minimum compatible
API **26**. Game device types default to **`default`**; the Editor remains
2-in-1-only because its external build tools/user-file permissions require that
profile. Both profiles detect `runtimeOS` from the selected SDK metadata: a
complete HarmonyOS SDK bundle uses `HarmonyOS`; a standalone versioned
OpenHarmony SDK uses `OpenHarmony`. API26 and later use dotted strings such as
`26.0.0` for compile, target and compatible SDK fields in both modes. Source-only
templates retain an OpenHarmony baseline until a SDK is selected. The person
installing the Editor supplies their own signing material.

The thin Editor APP contains its native engine/host and startup resources. The
optional oo `godot-editor` development kit provides the matching headless engine,
GodotSharp components, four Godot build packages and both export templates.
C# development uses the external oheco .NET SDK and normal NuGet restore; no
complete SDK or general offline feed is shipped in the APP. Ordinary exported
games retain their self-contained runtime and have no external SDK dependency.

**Experimental.** Native/headless tests, mock boundary tests, compilation and
signed HAP UI acceptance are separate validation scopes. A successful native C#
scene or unsigned HAP build does not establish permission, Vulkan, input or
multi-instance behaviour in the final signed application. Revalidate these
application behaviours after changing the common host.

## Editor updates in 4.7.2-ohos.6

This revision separates the thin Editor APP from its optional oo development kit.
See [release validation](<platform/openharmony/tests/release-validation-ohos6.md>)
for native, managed, packaging and deferred device-validation scopes.

Install the development kit with `oo update && oo install godot-editor`. Export
an editable Editor project with `oo export godot-editor app -o ./GodotEditor`.
The kit depends on `dotnet-sdk` **10.0.401-ohos.2**; the APP and kit do not contain
a complete SDK. The APP and new headless launcher discover compatible installed
kits automatically. C# projects use the four matching local Godot NuGet packages
plus normal NuGet.org restore for other dependencies. A standalone `dotnet build`
outside these hosts needs `GODOT_NUGET_SOURCE` or an equivalent local NuGet source.

- Mouse selection in Output/error logs no longer triggers touchscreen panning.
  Dragging above/below the log scrolls and extends the selection; holding the
  pointer still continues selection, and releasing stops it. Finger panning is
  retained.
- HDC deployments reserve device-side debug/file-server ports separately from
  Editor listeners and write those mapped ports into the game's arguments.
  Existing rules are preserved and only this deployment's rules are removed.
- Export templates are included in the optional development kit and are also
  separate Release downloads. The Editor APP carries neither template ZIP.
  Compatible kit templates are located automatically; OpenHarmony preset Custom
  Template fields remain available and take precedence over the automatic choice.
- Remote Deploy supports DevEco CLI automatic debug signing. Auto Signing and
  Always Auto Sign default to enabled. Matching private cached materials are reused;
  missing, expired or mismatched materials are generated automatically without
  changing export presets. The CLI may update a team-wide cloud certificate.
  All OpenHarmony Editor Settings have native help tooltips.
- Game export presets expose **Build > Device Types** as checkboxes, defaulting
  to phone. Select any combination of phone, tablet, 2in1, tv, wearable and car.
  The OpenHarmony SDK spells the phone type `default`; the HarmonyOS SDK uses
  `phone`. The other selected types are retained in the generated module.
- The Editor project's staged HAP stripping, numeric application version name
  and CUSTOM_SANDBOX permission selection are recovered in the generator.
- The Editor and both export templates include the accepted ABI-4 window-mode
  and mouse-capture host changes. The managed API and four local Godot packages
  remain 4.7.2-ohos.3.

## Desktop integration in 4.7.2-ohos.5

- The ArkUI startup surface shows the application/Godot logo while the managed
  runtime and native engine initialize. Log controls appear for failures
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
- The Editor profile declares `ohos.permission.READ_PASTEBOARD` with its usage
  reason and requests it through the shared user-grant permission flow. Its
  **system_basic, restricted user-grant** signing-profile ACL must also be approved.
  Installed cross-application clipboard acceptance remains pending; a declaration
  or user dialog alone is insufficient. Clipboard writes do not require this
  read permission.
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

The common host ABI is now **4**. It includes asynchronous main-window mode
requests and actual status snapshots, alongside the external application bridge
and physical XComponent screen position used for IME cursor placement. Rebuild
editor and both game template libraries and rerun `build-cli.py` before
regenerating projects; older host ABI caches are rejected.

Windowed and ordinary fullscreen modes can be switched at runtime. Exclusive
fullscreen requests fall back to ordinary fullscreen and report that mode back
to Godot. Initial project window modes and system title-bar mode changes use the
same state path; window operations execute on ArkUI without blocking the engine.
The managed Godot SDK remains `4.7.2-ohos.3`, since the managed API is unchanged.

Desktop input preserves physical arrows and shortcuts such as Ctrl+X, reconciles
actual window activity with XComponent focus, and distinguishes touchpad vp axis
deltas from mouse-wheel steps. External-action notifications use English base
resources through the internationalized resource lookup.

The IME client remains attached independently of soft-keyboard visibility.
Releasing a client uses one Detach rather than a preliminary focus-dependent
HideKeyboard. Cursor mapping keeps desktop window/mouse coordinates and converts
only the IME anchor to pixels on the current physical display. Display and density
changes refresh one coherent geometry snapshot; changing displays refreshes the
client binding. Cursor deduplication includes display identity and commits only
successful SDK notifications. Real-control tests cover root and nested embedded
windows at ordinary and 2x stretch. Device acceptance of the repaired input and
both-display round trip is recorded in
[release validation](<platform/openharmony/tests/release-validation-ohos5.md>).
Historical startup, popup, menu and external-action results remain in
[desktop-validation.md](<platform/openharmony/tests/desktop-validation.md>).

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
- Reaching the SDK uses `ohos.permission.READ_WRITE_USER_FILE` (user-grant).
  With `--custom-sandbox`, the Editor uses the approved weak sandbox to start
  oo-installed tools and does not additionally request
  `ohos.permission.ALLOW_EXTERNAL_NATIVE_CODE`. Without that flag the generator
  retains the external-native-code permission. The signing profile must cover
  the restricted permissions selected for the application.
- The managed build uses the pinned NuGet archives listed in
  [`platform/openharmony/dotnet/nuget-inputs.json`](platform/openharmony/dotnet/nuget-inputs.json).
  Every entry records its source, version, size, digest and license metadata.

New headers do not upgrade the operating system's Vulkan driver. The current
Maleoon 935 test host reports Vulkan **1.3.309**. The renderer must negotiate its
actual features and extensions. No system loader or driver is replaced.

On OpenHarmony, Huawei GPUs (`vendorID=0x19e5`) use function inlining for compute
shaders, independently of device ID or driver version. This compatibility
transform was validated on Maleoon 935; crash logs from Maleoon 916B also show
failures inside the driver compute pipeline compiler, but the expanded workaround
still needs validation on that device. Set
`GODOT_VULKAN_DISABLE_MALEOON_COMPUTE_INLINE=1` to disable it for comparison.
Transformed pipelines use a separate cache key.

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
- `build`: sdkVersion (default `26.0.0`), compatibleApi (default `26`),
  architectures, sdkRoot and commandLineToolsRoot. Compile/target/compatible fields
  use full dotted versions. SDK metadata selects HarmonyOS for complete bundles or
  OpenHarmony for standalone versioned component sets. The current Editor generator
  accepts ARM64 only.
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

Use a matching native editor library. APP generation never requires GodotSharp,
the .NET SDK or a NuGet feed; SDK mode enables optional development-kit discovery,
while none mode skips it. Native-only Editors still need user-file/external
code access and the weak sandbox to run the oo export tools. C# games keep host
managed.mode=none: the .NET export plugin embeds a signed self-contained runtime
in the PCK, rather than adding another runtime ZIP or an external SDK dependency.

Generate the game archive with the ordinary SCons target:

```sh
python3 -m SCons platform=openharmony target=template_debug arch=arm64 \
  module_mono_enabled=yes vulkan=yes opengl3=no generate_bundle=yes \
  debug_symbols=no dev_build=no OPENHARMONY_SDK_PATH="$NATIVE_SDK" -j8
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
- Official Command Line Tools with bundled Node **24.14.1 or later**, coherent
  Hvigor/plugin **7.26.8 or later in major 7**, and a complete API26+ SDK.
  The wrapper's pinned pnpm cache must be prepared before offline export. These
  tools are not installed by the project packager. Native HarmonyOS signing uses
  the SDK executable; supplied signing material avoids default credential generation.
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
  debug_symbols=no dev_build=no OPENHARMONY_SDK_PATH="$NATIVE_SDK" -j8

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
Godot managed package versions are pinned to **4.7.2-ohos.3** to avoid selecting upstream
packages with different bindings. Godot API/tool assemblies retain their upstream
.NET 8 target and run on .NET 10; newly created OpenHarmony C# projects target
.NET 10. A project-local `NuGet.Config` uses the engine-matched Godot packages
through `GODOT_NUGET_SOURCE` and NuGet.org for normal dependency restoration.
The fixed offline feed is a source-build input; it is not bundled into the Editor App.

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

Prepare the official build tools once:

```sh
oo install command-line-tools
```

Prepare the wrapper's pinned pnpm cache before an offline build. The runner
checks that cache and uses the empty Hvigor dependency map to select the bundled
engine and plugin. Project packages must already contain only staged local inputs.

Editor Settings exposes `export/openharmony/command_line_tools_root` and
`export/openharmony/sdk_root` as directories. Empty tools selects oo's active
`hvigorw` package; empty SDK selects that package's complete bundled SDK. HDC has
an optional `export/openharmony/hdc_path` file override. Node and hvigorw.js come
from the selected tools together. C# build/publish resolves compiler and ELF signer
from the same selected SDK metadata and passes their absolute paths only to the
MSBuild child environment.
A project-only export
needs no build tools. Full HAP/APP export calls the template's
[build runner](<misc/dist/openharmony_template/tools/BUILD_RUNNER.md>) directly with
Node, uses only installed dependencies, and never changes the Editor's global
PATH/cwd to imitate DevEco. Signing configuration is passed in a checked private
cache request. Password fields contain the unchanged DevEco encrypted values;
retain the adjacent keystore material directory. The official SDK decrypts those
values for its native signer. The original profile is restored after the build,
and output is redacted. Signed exports require the user's actual certificate,
profile and keystore; file-signature validation does not prove device trust.

### Automatic signing for Remote Deploy

Install the official `@deveco/deveco-cli` npm package separately and complete its
`devecocli auth login` flow before deploying. The selected Command Line Tools must
provide Node.js 22 or newer, Hvigor, the SDK and HDC. The integration is checked
against DevEco CLI 1.3.5; it does not install packages or open a login flow silently.

Under **Editor Settings → Export → OpenHarmony**:

| Setting | Default | Remote Deploy behavior |
| --- | --- | --- |
| Auto Signing | On | Allows automatic debug signing when needed. Off requires complete manual preset signing. |
| Always Auto Sign | On | With Auto Signing on, ignores manual preset signing. Off prefers a complete manual configuration and otherwise uses automatic signing. |
| DevEcoCLI Path | Empty | Discovers the installed CLI; accepts an explicit JavaScript entry file if discovery fails. |
| Auto Signing Team ID | Empty | Selects the only available team. Multiple teams require an explicit ID from `devecocli auth team list`. |
| Auto Signing Data Dir | Empty | Reuses the existing CLI login directory, including an inherited `DEVECO_CLI_DATA_DIR`; set it when login used a different directory. |

These settings apply to Remote Deploy. Ordinary export and distribution signing
continue to use the preset's Build > Sign and sign/* fields. A complete manual
configuration whose files disappeared remains a manual error when Always Auto
Sign is off; the Editor does not silently change that configured identity.

The native C++ signing backend queries the selected HDC target's actual UDID and checks the verified
debug profile's bundle, device list, ACL permissions, validity and matching
certificate. An HDC serial or network address is not a signing UDID. Matching
cached materials are reused. Missing or mismatched materials are regenerated
automatically without a confirmation dialog. CLI 1.3.5 can replace its team-wide
cloud certificate when regenerating for a new device, changed permissions or expiry. Failed signing
is reported, and local previous materials are retained. Existing applications are
never automatically uninstalled to bypass a signature mismatch.

The official CLI uses the broker's normal HOME, XDG paths and HDC identity;
the backend does not copy login credentials, create a separate CLI HOME or CLT
overlay, or impose a global umask/chmod policy. Generated signing results are
cached in `XDG_CONFIG_HOME`, and owned temporary work uses `TMPDIR` with cleanup.
Source export presets remain unchanged. Missing tools,
logged-out or expired sessions, ambiguous teams, unavailable devices, invalid
profiles and build failures produce actionable errors. See the [backend contract](<misc/dist/openharmony_template/tools/AUTO_SIGNING.md>)
and [native implementation](<platform/openharmony/export/auto_signer/README.md>)
for cache, rollback and child-process details. There is no automatic-signing JS
entrypoint, Node preload or JavaScript HDC broker. SCons builds and signs the
standalone `tools/auto-sign` from existing vendored JsonCpp/mbedTLS sources; its
C++ runtime is static so the terminal does not need the Editor App's libraries.

Build both `module_mono_enabled=yes` templates with `generate_bundle=yes`.
The Editor App does not bundle or extract export templates. The matching oo
development kit supplies both ZIPs, which the host exposes through the existing
template pointer. Separately downloaded `openharmony_debug_arm64-v8a.zip` and
`openharmony_release_arm64-v8a.zip` from the matching
[GitHub Release](https://github.com/oheco/godot/releases) can still be selected
under **Export → OpenHarmony → Custom Template → Debug / Release**. Explicit
preset paths retain precedence. Keep the ZIP files intact; the APP has no
managed runtime archive or general offline NuGet feed.

- F5/F6 launch another attached UIAbility process with the project/scene/debugger
  arguments. F8 stops the recorded game PID; no HAP packaging or HDC is involved.
- Remote Deploy requires an authorized HDC target and either automatic debug
  signing or a complete manual signing preset. Every install/start/forward command
  names that target and checks success. Debug and
  file-server deployments reserve distinct device-side ports before exporting,
  remap the game's loopback arguments to those ports, and retain the Editor's
  listening ports. This also works when HDC targets the Editor's own device.
  These deployments add INTERNET. Only owned forwarding rules are removed. F8 and debugger disconnect
  explicitly stop the device bundle; failed cleanup is retained for retry before
  the next deployment, not silently forgotten. Do not concurrently manage the
  same device ports from independent HDC clients.
- On HarmonyOS, **Editor Settings → Export → OpenHarmony → Use Broker** is
  enabled by default. Start `oheco-broker shell serve` in the terminal (`oo install
  oheco-broker` if needed). The editor reads `~/.oheco/broker/endpoint`, independently
  of the SDK/tool root, and connects to its numeric loopback endpoint;
  there is no port setting. Only explicit HDC operations (discovery, deployment,
  ability start/stop and debugger/file-server forwarding/cleanup) and the native
  automatic-signing operation use that service. The signing operation needs the
  terminal because official DevEco CLI queries HDC internally; its private
  credential validation and signing build remain in that same filesystem namespace.
  Ordinary Node.js/build tools, manual exports and unrelated external programs
  execute directly. The general tool executor never applies the broker implicitly.
  Commands keep their executable and argument arrays; they are never shell strings.
  Broker failures are reported without retrying or falling back to direct HDC.
  Only explicit network proxy variables, or the Editor's HTTP proxy setting, are
  forwarded to signing; terminal HOME/XDG/cwd/loader and HDC identity are retained.
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
  Ordinary manual exports and local UIAbility F5/F6/F8 execution retain their
  existing toolchain. Turning Use Broker off invokes HDC and the native automatic
  signing backend directly in the Editor's namespace. Native signing is currently
  packaged for OpenHarmony arm64 hosts; other hosts retain manual exports.
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
  --output /path/to/GodotEditor

# later iterations: same directory, no new copy to open
python3 platform/openharmony/export-editor-project.py \
  --library /path/to/new-cli-directory/libgodot.so \
  --output /path/to/GodotEditor --update
```

The script verifies the signed ARM64 editor library and its shared-host ABI. The
APP contains the native editor, HarmonyOS host, startup resources and a small
development-kit compatibility record. It contains no GodotSharp payload, NuGet
feed, headless launcher, .NET SDK or game-template archives. The native/GDScript
editor starts when no development kit is installed; C# development requires the
matching optional kit and external `dotnet-sdk`.

The `godot-editor` oo development kit contains the signed headless launcher and
its matching engine library, GodotSharp API and editor tools, exactly four
engine-matched Godot NuGet packages and both game export templates. Third-party
game packages use normal NuGet restore. Prepare its local archive separately:

```sh
python3 platform/openharmony/package-development-kit.py \
  --cli /path/to/new-cli-directory --godotsharp bin/GodotSharp \
  --dotnet-sdk "$DOTNET_SDK" \
  --debug-template bin/openharmony_debug_arm64-v8a.zip \
  --release-template bin/openharmony_release_arm64-v8a.zip \
  --output /path/to/new-development-kit \
  --archive /path/to/new-development-kit.tar.gz
```

The app discovers compatible kits from the active oo `godot` command and
`OHECO_ROOT/packages/godot-editor`, or `GODOT_OHOS_DEVKIT_ROOT`. Managed components
stay in the kit; template links and the existing template pointer are published
in the app's own data directory without changing export presets. Existing custom
NuGet settings and manually selected templates remain authoritative. Project
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
devices** with SDK API **26 or newer** and Vulkan only. The engine itself is
compiled against the OpenHarmony native SDK, which is a separate input.

The Editor requests `ohos.permission.READ_WRITE_USER_FILE` at runtime to access
user projects and the oo-installed SDK. Its approved `CUSTOM_SANDBOX` profile
permits the external tool processes. For profiles generated without
`--custom-sandbox`, the generator instead retains
`ohos.permission.ALLOW_EXTERNAL_NATIVE_CODE`. The signing ACL must match the
selected restricted permissions.

The generated Editor's two `nativeLib.debugSymbol.strip` settings follow the
working DevEco project and are enabled. Hvigor strips the staged HAP libraries
before official HAP signing; the signed input library and CLI files remain
unchanged. Game-template archives continue to preserve their signed libraries.

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

The checks above describe the historical 4.7.2-ohos.2 baseline. Later desktop
acceptance covers the startup logo, popup IME alignment, main/context/submenu
hover, File Manager and unsupported external actions; their original scope and
hashes remain in [desktop-validation.md](<platform/openharmony/tests/desktop-validation.md>).
The 4.7.2-ohos.5 input repair adds physical shortcuts, touchpad normalization,
focus-aware IME ownership, Detach-only release and multi-display cursor mapping.
Its installed candidate passed text-field/application-focus switching and both
screens with a return to primary. See
[release validation](<platform/openharmony/tests/release-validation-ohos5.md>) for
current automated checks, release provenance and the separate device scopes.

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
