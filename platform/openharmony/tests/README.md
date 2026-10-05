# Shared OpenHarmony host verification

Run from the Godot source root. Tests download nothing. Set `TMPDIR` to a writable
native temporary filesystem; fixtures clean up their own temporary directories.
Persistent build evidence belongs in a separate cache/output directory, not the
source template or a user's configured DevEco project.

## Configuration and migration

```sh
python3 -m unittest discover -s platform/openharmony/tests -p 'test_*.py' -v
```

These tests cover game/editor/native-only profiles, SDK26/API23-compatibility defaults, escaped
strings, supported permissions, explicit restricted-permission flags, unknown
fields, schema failures before source replacement, preserving signing/locks,
removal of obsolete generated entry points, real file-quota failure, rollback
installation failure and interrupted migration recovery. Editor generation uses
clearly marked non-executable ELF fixtures: it does not prove a runnable HAP.

## ArkTS logic and native boundary

Use the TypeScript implementation from the prepared ArkTS SDK, not a downloaded
replacement:

```sh
node platform/openharmony/tests/test_arkts_host.cjs --compiler "$TYPESCRIPT_JS"
node platform/openharmony/tests/test_host_diagnostics.cjs --compiler "$TYPESCRIPT_JS"
node platform/openharmony/tests/test_touch_input.cjs --compiler "$TYPESCRIPT_JS"
node platform/openharmony/tests/test_templates.cjs --compiler "$TYPESCRIPT_JS"
node --test platform/openharmony/tests/test_build_runner.cjs
python3 platform/openharmony/tests/test_device_run.py --sdk "$SDK_VIEW_ROOT"
python3 platform/openharmony/tests/test_native_shell.py --sdk "$NATIVE_SDK"
python3 platform/openharmony/tests/test_engine_arguments.py --sdk "$NATIVE_SDK"
```

The ArkTS harness loads actual shared source with explicit Harmony/NAPI mocks;
its runtime extraction tests use real temporary files and independent module
contexts. The native shell fixture compiles and signs the actual NAPI glue, but
mocks the engine/window/resource boundaries. The argument test includes the
production helper and executes both normal and hardened builds. See the
[native fixture details](<README_native_shell.md>) for requirements and limits.

Important regression: ArkUI AppStorage wraps stored arrays in state-management
Proxies. On the inspected native API 26 / OpenHarmony 7.0.0.105 installation,
`napi_is_array` uses raw `JSValueRef::IsJSArray`/`IsSharedArray`, not the
ECMAScript `IsArray` proxy-unwrapping path. `join()` and `Array.isArray()` succeeding
is therefore insufficient. V8-based Node N-API tests alone missed this difference.
The host fixture now returns an observed Proxy from AppStorage and explicitly
models that Ark native-array boundary, requiring a detached plain snapshot.
It checks sparse/non-string/NUL/oversized inputs, Unicode, empty arguments and
snapshot independence without accepting arbitrary array-like objects. This is
still not an API 23 HAP runtime probe; target-device startup remains required.

The diagnostics fixture verifies that a pre-engine `setup` failure cannot display
legacy or another PID's engine log as this session's output. It covers PID reuse,
seconds/nanoseconds timestamps, bounded reads, and explicitly identified fresh
attached-child crash logs without changing the multi-process crash baseline.

The touch regression exercises real `Index` input methods, not just startup:
ArkUI delivers a mouse left click to both `onMouse` and a synthesized `onTouch`.
Only the mouse route should reach Godot for that input. Touchscreen IDs are opaque,
not native vector indices; the router maps `(deviceId, rawId)` to at most 32 active
slots and explicitly handles release/cancel/blur/disposal. Hover/unknown sources,
capacity overflow and orphan updates cannot create ghost contacts. The native
fixture verifies unsupported integer touch types/slots are safely dropped without
letting huge IDs reach the engine; malformed JS types remain rejected. These
fixtures do not substitute for real mouse/touch/IME/device acceptance.

## Real native engine and export

Build/sign the Editor CLI using the platform scripts, and build an actual game
ZIP with `generate_bundle=yes target=template_debug` (or template_release).
Then run:

```sh
python3 platform/openharmony/tests/test_game_export.py \
  --godot "$GODOT_CLI" --template "$GAME_TEMPLATE_ZIP" --output "$NEW_EXPORT_EVIDENCE_DIR"
```

This runs the real Editor import/export pipeline, checks application/SDK/permission
metadata and byte-identical shell sources, boots the generated PCK headlessly,
verifies nonzero script/export exit codes, and rejects API 18 and corrupted nested
host/project schemas. Optionally pass `--runtime-godot "$TEMPLATE_CLI"` to also run
the PCK with the actual signed template engine produced by `build-cli.py`. This
relocates the CLI/library and uses executable-adjacent pack discovery, without
weakening the template's default disabled command-line path overrides.
It is not a Vulkan, input, UIAbility or installed-game test.
For SDK-enabled Editor regression, also rerun
[test-dotnet.py](<../test-dotnet.py>) with the prepared native SDK/GodotSharp/feed.

## Real native HAP/APP export and GDScript protocol

```sh
python3 platform/openharmony/tests/test_game_export.py \
  --godot "$GODOT_CLI" --template "$GAME_TEMPLATE_ZIP" \
  --release-template "$RELEASE_TEMPLATE_ZIP" --runtime-godot "$TEMPLATE_CLI" \
  --build-bundles --output "$NEW_EXPORT_EVIDENCE_DIR"
python3 platform/openharmony/tests/test_debug_protocol.py \
  --editor "$GODOT_CLI" --template-debug "$TEMPLATE_CLI" \
  --template-release "$RELEASE_TEMPLATE_CLI" --template-zip "$GAME_TEMPLATE_ZIP" \
  --output "$NEW_DEBUG_EVIDENCE_DIR"
```

The first test calls the actual C++ exporter and the installed oo Hvigor adapter
for unsigned debug HAP, release HAP and APP output, including paths with spaces.
No signing/install/dependency download is performed. SDK26/OpenHarmony/default
is the current source configuration, not a fabricated HarmonyOS API23 SDK.
The second uses a real Godot TCP protocol peer and real exported GDScript: it
checks breakpoint/stack/local values, next/continue, actual RemoteSceneTree and
release debugging rejection with a positive release-boot control. It is not
GUI/HAP/HDC acceptance. Both clean up their isolated runtime projects/processes.

[build_deveco_project.py](<build_deveco_project.py>) remains useful for an isolated
source project or `--arkts-only`, but now invokes the installed adapter with
`--hvigor-entry`, never a direct unadapted official Hvigor entry. Personal signing
material/history is not copied into its distributable output. C# self-contained
publish/PCK/template and optional HAP regression is documented
[here](<../../../modules/mono/tests/openharmony_export/README.md>).

## Required signed-application acceptance

Before release, build with the selected oo SDK view, actual signing material and
required ACLs, then verify the signed application on a device:

- Game: PCK launch and arguments, empty/granted/denied permissions, keyboard/mouse/
  touch/IME, relative mouse and fine wheel, resize/safe-area/background/exit.
- Editor: cold/warm/concurrent runtime preparation, project manager to independent
  editor, Run/Stop attached game, successful and failed .NET builds, SDK permission
  denial, crash reporting and parent/child lifetime independence.
- Both: shutdown during startup and a new process after termination. The shared
  engine intentionally has one lifetime per process, not an in-process restart API.

Do not tag/publish a release based only on mocks, headless tests or unsigned HAP
compilation. Record the tested source commit, SDK, device and artifact hashes.

## Real broker Editor integration

```sh
python3 platform/openharmony/tests/test_broker_editor.py \
  --editor "$GODOT_CLI" --template "$GAME_TEMPLATE_ZIP" \
  --output "$NEW_BROKER_EDITOR_EVIDENCE_DIR"
```

Requires native OpenHarmony and an already-running oheco broker. It checks fresh
`Export/OpenHarmony/use_broker` defaults, the loaded runnable preset and actual
Remote Deploy menu (default target `192.168.1.160:34835`, configurable with
`--target`), broker/direct/broker routing changes using a disposable fake HDC,
real project-only export, empty permissions and exported GDScript PCK execution.
It keeps HOME unchanged and points OHECO_ROOT at a temporary directory linking
only the real `bin/packages/sdk`, with no `broker` directory: shell serve discovery
must still use `HOME/.oheco/broker/endpoint`. The route marker exists only in the
native Editor environment; no broker environment overrides are sent. Fixtures/settings are cleaned from
`TMPDIR`; logs, metadata, PCK index and hashes remain in the new evidence directory.
`--skip-routing-toggle` omits only the fake routing transitions. The test also
triggers the real Remote Deploy menu signal into C++ `run()`. The original template
build runner calls a synthetic Hvigor fixture; the broker-side fake HDC reads the
result directly from the project's `.godot/openharmony/run_<pid>_<ticks>/` directory
and rejects installation deliberately. This checks the actual path, PCK content,
command routing and complete owned-directory cleanup without signing a real game
or deploying it. `--sdk-root` can select a canonical SDK view for this build-runner
preflight; the HOME/OHECO_ROOT alias discovery check remains independent.
The test does not start/stop services, install an application or change port rules.

## Broker command protocol

```sh
python3 platform/openharmony/tests/test_broker_client.py --sdk "$NATIVE_SDK" \
  --output "$NEW_COMMAND_PROTOCOL_EVIDENCE_DIR" \
  --real-endpoint "$HOME/.oheco/broker/endpoint" --hdc "$HDC_ELF"
```

Every managed START has stdin disabled. The original vendored C SDK is unchanged.
Fixtures check total startup/command deadlines, late handle release while the host
stays alive, fragmented stdout/stderr, output caps, exit/signal status, argument
preservation and disconnected requests without retry. They reject unexpected
client data after START and do not contain file upload or stdin producer tests.
Omit the final two arguments for protocol fixtures without a real HDC service.

## Broker HDC commands and shared project paths

```sh
python3 platform/openharmony/tests/test_broker_device.py --sdk "$NATIVE_SDK" \
  --same-device-broker-node --shared-hap "$SHARED_PROJECT_HAP" \
  --output "$NEW_COMMAND_DEVICE_EVIDENCE_DIR"
```

This explicit self-device mode first proves that the target IPv4 is local and
that the target HDC shell, broker Node and echo listener share the exact network
namespace. It then verifies a binary nonce through one owned reverse port pair
and removes only that pair, preserving the original rule list. It never stages
or executes an external Node through the target shell, changes execution policy,
or selects a fallback mode. For a different device this particular context is
unsupported and the test fails before creating a rule.

`--shared-hap` is optional: it passes only the existing absolute HAP path inside
its shared project's `.godot` directory to broker Node for stat/SHA-256 checking.
The command control connection sends zero HAP bytes and has no STDIN frames.
An explicitly authorized `--install-hap "$SIGNED_HAP" --editor-process "$BUNDLE"`
adds HDC installation after confirming that process is absent. HDC reads the
passed path and performs its own device upload; this is distinct from a file
transfer over the broker control connection. `--install-only` skips port testing.
No applications are launched/stopped and no services are started or restarted.
