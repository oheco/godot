# Native OpenHarmony C# NativeAOT game export regression

Run on OpenHarmony with the oo .NET 10 SDK, rebuilt Godot managed packages
`4.7.2-ohos.3`, and the rebuilt native editor/templates. Version `.3` supplies
OpenHarmony AOT defaults, game/GodotSharp roots, and the resolved-reference guard
against editor-only assemblies. The editor refuses unsafe older `.2` AOT exports;
do not replace an immutable `.2` nupkg with different contents.

```sh
python3 modules/mono/tests/openharmony_export/run.py \
  --dotnet-sdk "$OO_ROOT/packages/dotnet-sdk/10.0.401-ohos.2" \
  --native-sdk "$OO_ROOT/packages/ohos-sdk-native/26.0.0.35-Beta" \
  --nuget-feed /path/to/pinned-offline-feed \
  --godotsharp bin/GodotSharp \
  --editor /path/to/rebuilt-editor-cli/godot \
  --restricted-tool-path \
  --template-debug /path/to/mono-template-debug-cli/godot \
  --template-release /path/to/mono-template-release-cli/godot \
  --output "$XDG_CACHE_HOME/new-nativeaot-export-check"
```

Use a fresh evidence directory. Projects, settings, restore caches and build
outputs live in `TemporaryDirectory` under `$TMPDIR`, with automatic cleanup.
Only requested evidence stays in cache. Native ELF outputs are signed with the
installed binary-sign-tool by the .NET SDK, using temporary publish copies. No
SDK/project input is signed or modified, no personal HAP identity is used, and no
HAP is signed, installed, published or committed.

The fixture exercises real C# script construction, serialized exported property
values and metadata, generated property/method dispatch, signals, Callables,
typed collections and generic Variant conversion, RPC metadata with an offline
local call, ICU culture lookup, OpenSSL-backed hashing, user-private Unicode
files, a worker task and GC, and `_Process` after signal awaits. NativeAOT must
report both `RuntimeFeature.IsDynamicCodeSupported=false` and
`IsDynamicCodeCompiled=false`. Reflection.Emit and JIT generation are explicitly
outside the NativeAOT runtime contract; they are not used to fake a pass. The
success markers appear only after all of these checks.

The regression also checks:

- Actual `PublishAot=true`, `NativeLib=Shared`, `openharmony-arm64` publish, with
  detailed trim/AOT warnings retained. Managed DLL/CoreCLR fallback fails.
- Standalone helper tests for ELF64/AArch64/ET_DYN, executable/dynamic segments,
  an exported defined global initialization function in DYNSYM, malformed tables,
  architecture/type errors, detached debug ELF rejection, missing dependencies,
  flattened basename collisions, reserved filename collision, and no mutation of
  native input files.
- Native deployment uses `libgodot-csharp-game.so` plus the six ICU/OpenSSL/C++
  dependencies in `entry/libs/arm64-v8a`; versioned `.so.78` and `.so.3` basenames
  must remain unchanged. Every deployed native library has a code signature.
- PCK entry MD5, C# source stripping, no ELF/DLL/CoreCLR/manifest/runtimeconfig or
  debug symbols inside PCK, and exact `.godot/mono/openharmony_aot.json` contents:

```json
{"schemaVersion":1,"mode":"native-aot","library":"libgodot-csharp-game.so"}
```

- Debug symbol helper tests export only to `<target-basename>.symbols/arm64`,
  preserving the original SDK debuglink basename and providing the reserved
  `libgodot-csharp-game.so.dbg` alias plus mapping. Debug files are never signed,
  treated as loadable shared libraries, or embedded in PCK/HAP libs.
- Resolve-only fake tool fixtures and a restricted Editor PATH (`/system/bin`) test
  absolute SDK signer/clang discovery, LLVM tool availability, configuration
  precedence, and child-only environment changes. The Editor receives neither an
  injected `OpenHarmonySigningTool` nor `CppCompilerAndLinker`; its BuildSystem
  must configure its own publish child. An optional absolute `--editor-sdk-root`
  writes only private EditorSettings and exercises the explicit SDK-view route.
- Debug/release exported PCKs start twice under the matching native template,
  using physical adjacent libraries with no external SDK/feed environment.
  No old CoreCLR extraction manifest may be created.

Omit editor/template arguments to test actual publish and helper behavior only.
Omit template arguments to audit PCK export without running a game. A PCK alone
cannot carry the NativeAOT libraries: the template-only test explicitly stages
its independently published native libraries beside the signed template CLI.

For full native project export, provide `--hap-template-debug` and/or
`--hap-template-release` with mono-enabled template ZIPs. Add `--build-bundles`
to build unsigned HAPs with the existing native exporter/oheco Hvigor adapter.
These checks verify same-package native libraries, small PCK, no personal signing
configuration, and removal of the two JIT/independent-library kernel permissions.
A real resolved `GodotSharpEditor.dll` reference must fail before AOT compilation
and prevent the native exporter from creating a HAP. A copied DLL content file is
not used as a substitute for this dependency-graph test.

Terminal/headless success does not constitute a signed-HAP sandbox or GUI
acceptance. Installation and final application loading remain separate native
acceptance steps; this runner does not perform them.
