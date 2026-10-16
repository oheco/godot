# 4.7.2-ohos.6 validation

This release separates the native Editor APP from the optional oo development
kit. It also includes Output drag selection and edge scrolling, separate HDC
host/device debugger ports, native DevEco automatic signing, native Editor
Settings help, device-type presets and the accepted ABI-4 host changes.

## Release contents and dependencies

- The signed thin Editor HAP is 171,268,639 bytes, down from 373,855,395 bytes
  in the preceding local candidate (54.189%). Its raw resources contain only
  the host configuration and a 236-byte development-kit compatibility record.
- The public Editor project ZIP is 76,859,548 bytes. It is produced before
  personal signing configuration and excludes build caches and credentials.
- The oo `godot-editor` archive contains the signed native headless launcher,
  its matching engine, complete GodotSharp API/editor runtime dependencies,
  four Godot 4.7.2-ohos.3 NuGet packages and both game export templates.
- The external dependency is exactly `dotnet-sdk` 10.0.401-ohos.2. Neither the
  APP nor the kit includes a complete SDK or the general 145-package feed.
  Ordinary game dependencies restore through normal NuGet sources.

Formal asset names use 4.7.2-ohos.6; the APP already identifies itself as
4.7.2.6, versionCode 4070206. Release preparation preserves the signed HAP,
project ZIP, templates, executable and assembly bytes from the validated
candidate. Only the kit's package-version metadata and archive root change.
`SHA256SUMS` and `RELEASE-VALIDATION.json` attached to the Release identify the
final assets and retain the candidate provenance.

## Source and binary provenance

The engine was built from `01b970dc7e2ae2f45dbea6358b640524e9cb7089`.
Its signed input SHA-256 is
`01030b50d17991bc2250d3b41dfc21322a58095bea452e638d86e8e1940fc303`.
The changed native headless host was compiled and signed from the matching
C++ source in `c6776dc57248f312da23b8052ffe974c02808f5d`; its SHA-256 is
`3c6326ee662fbe7c34196ff57f99a642ba9c80cd0be987716cb421ffc9884b52`.
The final host/configuration snapshot is
`f9061404d1255e9d33208d36570376dfd1c4717b`. Subsequent release documentation
and the packager's default version do not rebuild or alter these binaries.

The user's GPU compatibility source remains unchanged, SHA-256
`7963449e01b6e1d95b9b2b076d825aebb0ff698bad0aaf41b7ef8a7f63a978d4`.
The new production ProjectEditor assembly is
`7b6b59d5026478a39a68dac9f874491a7796c92ed5710cd5665acb0bc5ae2625`;
its net8.0 target and assembly identity are retained. API, Core, Shared and
other runtime dependencies preserve their validated baseline bytes.

## Native and managed evidence

- Thirteen new targeted cases pass: thin project generation, complete kit
  layout, native discovery/template publication/CLI overrides, native and
  managed NuGet writers, and both supported SDK provenance filenames.
- A real native headless Editor/GDScript session runs without a managed kit.
  Its deliberately early exit has scanner/RID cleanup diagnostics; this is
  startup evidence rather than installed-GUI or clean-shutdown acceptance.
- A fresh-cache project using the four local Godot packages retrieves
  Newtonsoft.Json 13.0.3 from NuGet.org through the configured proxy and builds
  with zero warnings/errors. Three real restores succeeded; the first two
  harnesses misidentified the relocated assets path. One real build completed.
- The actual production ProjectEditor project builds with zero warnings/errors.
  A real runtime reflection probe checks new config creation, exact legacy
  migration and preservation of custom config with the final assembly.
- The final local kit is unpacked once into an oo layout containing spaces.
  Six actual CLI invocations each run once: versioned entry, active alias,
  automatic-root/CLR probe, GodotTools C# build, the compiled C# scene and a
  default-template GDScript project export.
- The automatic-root probe initializes hostfxr 10.0.12 and GodotPlugins,
  completes scanning/editor layout, resolves the kit/SDK, and preserves caller
  HOME, TMPDIR, NuGet cache and DOTNET_CLI_HOME.
- The actual GodotTools build takes 32.57 seconds, and the fresh C# scene runs
  successfully in 2.36 seconds. Default-template export takes 18.83 seconds;
  no custom template path is supplied. PCK, game native library, updated ETS
  files and the default phone device type match the selected template.
- Signed Editor SDK compilation succeeds in 74.21 seconds after fixing the
  first attempt's ArkTS inference errors. Final HAP signature verification
  succeeds once; CRC and public-project exclusion checks pass.

The relocated C# project uses a manual equivalent local-Godot plus NuGet.org
configuration; production config generation is covered separately. The SDK
resolver retrieves three vulnerability-metadata JSONs, but no third-party
package archive is downloaded again. All four restored Godot packages come
from the relocated kit. No global NuGet cache or general offline feed is used.

## Preserved scope and limitations

The custom Editor SceneTree probe emits RID/detached-node diagnostics on exit;
normal GodotTools build, C# scene execution and template export have no ERROR
lines. The initial project-assembly-missing notice occurs before the first
successful build and is retained in the evidence.

Default-template verification is project-only export: NativeAOT publication,
the native automatic-signing backend suite and unchanged earlier suites are
not rerun. The SDK's standard APP signing path and native automatic-signing
backend retain their prior scoped evidence; no extra inner-HAP signing gate
is introduced.

The installed Editor APP is not replaced. Its GUI, permission prompts,
clipboard, Vulkan/input/window behaviour and all shutdown paths are not
established by these headless results. Physical game startup/debugger
handshake/scene acceptance remains deferred by the user while the device is
locked (AA 10106102); no automatic device retries or reinstall occur.

Original projects, export presets, signing profile bytes/mtime and application
data are preserved. Native/managed test workspaces are isolated under TMPDIR
and cleaned; no owned test processes remain. Post-publication formal-index
installation and remote-asset verification are recorded separately from this
pre-publication evidence.
