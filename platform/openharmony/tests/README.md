# Shared OpenHarmony host verification

Run from the Godot source root. Tests download nothing. Set `TMPDIR` to a writable
native temporary filesystem; fixtures clean up their own temporary directories.
Persistent build evidence belongs in a separate cache/output directory, not the
source template or a user's configured DevEco project.

## Native EGL/GLES manager and export rendering selection

```sh
python3 platform/openharmony/tests/test_gl_manager.py \
  --native-sdk "$NATIVE_SDK" --output "$NEW_EGL_RECEIPT"
python3 platform/openharmony/tests/test_rendering_export.py \
  --sdk "$NATIVE_SDK" --library "$SIGNED_EDITOR_LIBRARY" --library-only
```

The manager fixture compiles the complete production manager and the bundled
GLAD sources, signs the executable and runs failure/cleanup/context-transfer
cases. Its real EGL/GLES probe replaces only the window surface with a pbuffer;
clear/readback and rendering-thread migration use the real driver. Pbuffer
results do not establish XComponent presentation or actual window VSync support.
The configuration cases also execute the platform configuration and GLAD build
rules, including the Vulkan-only build option.

The export fixture starts independent native headless Editors with isolated
project/XDG/Godot paths and keeps the real HOME. It covers Compatibility,
Mobile/Forward+, feature and preset overrides, the inactive driver's settings,
unsupported driver/method errors and the retained ETC2/ASTC requirement. Use
`--library-only` against the final rebuilt signed Editor to test its compiled
exporter. Without that option it compiles the byte-exact production validation
method into the fixture, allowing a previous signed library to exercise the new
validation before a full build. `--generated-headers "$PREVIOUS_SOURCE_ROOT"`
can supply missing generated headers for this development mode.
`--expect-original-bug` runs a Vulkan control and requires the old library to
reject two otherwise valid Compatibility configurations. No configuration test
opens a GPU window or establishes renderer presentation.

## Signed engine rendering into a real native window

After the platform build has generated its headers, select the corresponding
signed Editor, debug or release library:

```sh
python3 -B platform/openharmony/tests/test_rendering_window.py \
  --native-sdk "$NATIVE_SDK" --library "$SIGNED_EDITOR_LIBRARY" \
  --engine-target editor --method gl_compatibility \
  --render-thread separate --resize --output "$NEW_WINDOW_EVIDENCE_DIR"
python3 -B platform/openharmony/tests/test_rendering_window.py \
  --native-sdk "$NATIVE_SDK" --library "$SIGNED_DEBUG_LIBRARY" \
  --engine-target template_debug --editor-cli "$SIGNED_EDITOR_CLI" \
  --template-zip "$SIGNED_DEBUG_TEMPLATE_ZIP" --sdk-root "$SDK_ROOT" \
  --method gl_compatibility --output "$NEW_TEMPLATE_WINDOW_EVIDENCE_DIR"
```

Use `--method mobile` or `--method forward_plus` for Vulkan. For release, select
`--engine-target template_release` and its matching signed library/template ZIP.
Template cases require a signed Editor CLI and a real template ZIP whose native
payload matches the selected library and adjacent build-info provenance. The
actual OpenHarmony project-only exporter produces the fixture PCK with build/sign
disabled. The runner automatically discovers its adjacent `runner.pck` in a
separate runtime directory, without `--path`, `--main-pack`, a loose project in
cwd or modification of the signed ELF. Editor cases retain loose-project startup.
`--sdk-root` selects a real export SDK; its default is the caller's
`GODOT_OHOS_SDK_ROOT` or the active Command Line Tools' bundled SDK. Export logs,
PCK/template/native hashes and runtime arguments/cwd are recorded in the receipt;
the exported project and sidecar PCK are temporary. This covers pack loading and
ConsumerSurface rendering; HAP NativeResourceManager startup remains separate.
Omit `--resize` for 320×240 and use `--render-thread safe` for a main-thread run.
The test preserves HOME and isolates project/XDG/Godot/oo paths in TMPDIR. It
compiles and signs only its small runner, verifies the selected library's signed
ELF metadata and actual loaded path, then uses normal OS/Main startup. Project
settings select the backend; no rendering-method/driver command-line override is
passed.

The fixture waits for 20 post-draw frames, reads back 2D, 3D, depth and background
pixels, saves a PNG, and independently consumes at least 10 real presentation
buffers. Optional resize also requires at least 5 actual 640×480 buffers through
the public display resize path. It checks VSync 0/1 requests and getters, reports
EGL swap-interval errors, waits for GPU fences and verifies balanced buffer release
and surface destruction. It does not measure display scanout timing or cover
ArkTS, installed HAP startup, UIAbility or visible ArkUI windows.

## Template backend metadata and real export rejection

```sh
python3 platform/openharmony/tests/test_template_rendering.py --generation-only
python3 platform/openharmony/tests/test_template_rendering.py \
  --godot "$FINAL_SIGNED_EDITOR_CLI" --jobs 2
```

Generation runs the complete production bundle action with actual template staging
and ZIP serialization, adapting only SCons I/O boundaries and disabling the native
signer for explicitly non-executable ELF data. It checks the declared capabilities
follow the build flags. The metadata-only Vulkan-disabled fixture does not establish
that this platform supports a Vulkan-disabled engine build.

The CLI matrix performs 19 real project-only exports, covering selected debug and
release templates, old Vulkan templates, new GLES templates, default/custom
selection and malformed JSON/driver metadata. It requires unsupported backend
combinations to fail with an update/rebuild diagnostic. Fixture libraries and
exported PCKs are never executed; runnable template and HAP acceptance are separate.

## Child-game crash isolation and Output

```sh
python3 platform/openharmony/tests/test_child_crash.py \
  --sdk "$NATIVE_SDK" --library "$SIGNED_EDITOR_LIBRARY"
```

This native fixture links the rebuilt editor, uses the real UIAbility
`create_instance` path with a controlled launcher, and checks the actual Output
RichTextLabel. It covers an early crash before the launcher returns, reused PIDs,
historical log prefixes, a shorter fresh engine log, repeated polls, later runs,
ordinary exits, empty crash files, foreign editor/user arguments and failed
launches. It also execs signed native game subprocesses: crash, normal run, then
another crash, while the same parent EditorNode continues iterating. Both the
final UTF-8 error and the production signal-handler backtrace must reach Output,
even with the parent in `--quiet` mode. The host diagnostics fixture separately
checks that a live parent's surface stays mounted and no status overlay appears;
CompileArkTS and application runtime acceptance remain separate checks.

Only newly written nonempty native crash logs identify a fatal crash. SIGKILL,
OS termination, and failures before the crash handler can write a report cannot
be diagnosed from this file channel. The report includes bounded 64 KiB tails
and the full log paths; normal debugger output is not continuously replayed.

## Configuration and migration

```sh
python3 -m unittest discover -s platform/openharmony/tests -p 'test_*.py' -v
```

These tests cover game/editor/native-only profiles, SDK26/API26-compatibility defaults,
automatic HarmonyOS/OpenHarmony mode selection from SDK metadata, dotted API26
SDK fields, rejection of obsolete version syntax and inconsistent SDK layouts,
conversion of preserved profiles, escaped strings, supported permissions,
explicit restricted-permission flags, unknown
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
node platform/openharmony/tests/test_host_focus.cjs --compiler "$TYPESCRIPT_JS"
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

## Desktop integration

The host logic suite also covers the logo/loading/status state transition,
external folder/link requests, File Manager launch failures, and explicit
unsupported terminal/local-file-editor notifications with no launch. The native
shell fixture retains low-level queue/worker lifecycle coverage, including
Promise failures; its internal terminal API is not used by the disabled menus.
These are queue/host tests, separate from installed application acceptance.

With a newly signed Editor CLI and the existing managed inputs, run:

```sh
python3 platform/openharmony/tests/test_desktop_defaults.py \
  --godot "$GODOT_CLI" --godotsharp "$GODOTSHARP" --dotnet-sdk "$DOTNET_SDK"
```

This boots the actual headless Editor in a temporary project and isolated
settings directories. It verifies the filesystem dialog and initial project
path use the desktop home, and an explicitly selected directory is preserved.
It does not test the ArkUI splash, installed DPI scaling or pasteboard grants.
The short-lived headless Editor can report scan-abort/RID cleanup warnings on
exit; the assertions and process exit code are recorded separately.

The desktop input regressions run against native SDK/system objects and the
signed engine library:

```sh
python3 platform/openharmony/tests/test_ime_geometry.py --native-sdk "$NATIVE_SDK"
python3 platform/openharmony/tests/test_ime_cursor_notify.py --native-sdk "$NATIVE_SDK"
python3 platform/openharmony/tests/test_ime_client_lifecycle.py --native-sdk "$NATIVE_SDK"
python3 platform/openharmony/tests/test_mouse_capture.py --native-sdk "$NATIVE_SDK"
python3 platform/openharmony/tests/test_ime_controls.py \
  --native-sdk "$NATIVE_SDK" --library "$SIGNED_ENGINE_LIBRARY" --compile-controls
python3 platform/openharmony/tests/test_popup_mouse_motion.py \
  --sdk "$NATIVE_SDK" --library "$SIGNED_ENGINE_LIBRARY"
```

The IME fixture checks surface-to-screen pixel mapping, real CursorInfo/TextConfig
objects and concurrent geometry snapshots. Its WMS properties boundary is
mocked; it never attaches or types into a user's input method. The cursor-notify
fixture compiles the complete current production method, uses real SDK
CursorInfo objects, and injects Notify errors and allocation failures. It
verifies retries at an unchanged caret, successful update deduplication and
balanced object ownership; `--baseline` restores the old failure fallthrough and
is expected to fail. It substitutes mapping/state rather than attaching an IME.
The controls fixture
captures actual LineEdit/TextEdit submissions to a headless DisplayServer and
compares them with the real canvas/embedded rendering transforms. It covers a
nonzero native window origin, decorated and nested windows, display scale 1.9
and actual 2x canvas stretching. `--compile-controls` compiles the current
production controls; with an unfixed signed library, `--baseline` reproduces the
doubled popup placement. This check closes the dimension missed by wrapper-only
point mapping; it does not attach the commercial IME or operate a device GUI.
The popup fixture uses real engine Input/Viewport/Button/PopupMenu objects and compiles the actual
platform bridge. It checks motion velocity, zero raw-delta fallback, captured
pointer motion and click preservation. Installed ArkUI GUI acceptance remains
separate from these native regression fixtures.
The mouse-capture fixture extracts the current production mouse methods, teardown,
focus queries and member declarations, then signs and runs them on HarmonyOS with
an independent mock cursor SDK and deterministic clock. It covers system unlock
on app switching, rapid activation pairs, background requests, overrides, release
after surface blur, stop/join cleanup, untouched visible mode without cursor
permission and transient-failure retries. It neither monitors real input nor
adds mouse-event logs. Installed TPS app-switch capture acceptance remains a
manual device check.

Window mode regressions cover the production asynchronous state helper and ArkUI
controller, using actual state independently of requested modes:

```sh
python3 platform/openharmony/tests/test_window_mode.py --native-sdk "$NATIVE_SDK"
node platform/openharmony/tests/test_window_mode.cjs --compiler "$TYPESCRIPT_JS"
```

They check window/fullscreen switching, exclusive-to-ordinary fallback, external
status snapshots, superseded requests, SDK failure rollback and destruction while
an operation is pending. GUI acceptance still requires an installed application
switching its real main window and receiving the resulting surface resize.

Rebuild the common ABI-4 host, editor and both templates, then verify Chinese
candidate placement below the caret in moved/maximized windows and embedded
LineEdit/TextEdit popups, as well as real mouse hover over menus and submenus.
Screen mouse positions and Window rectangle callbacks must use the same surface
origin as the IME mapping so that absolute viewport transforms remain correct.

This revision intentionally does not launch a terminal or an external editor.
Click those entries and verify the unsupported-feature notification, continued
Editor responsiveness and absence of an external launch. Verify that File
Manager still opens the requested directory, browser links still open, and
scripts can still be edited internally. Broker and terminal cwd integration are
not acceptance requirements for these disabled menus.

The Editor profile declares `READ_PASTEBOARD` and requests it through the shared
user-grant flow. Installed clipboard acceptance remains pending and requires its
approved signing-profile ACL and the application's granted permission;
preserve/restore clipboard contents when performing that probe.

## Output selection on hybrid mouse/touch devices

```sh
python3 platform/openharmony/tests/test_log_selection.py \
  --sdk "$NATIVE_SDK" --library "$SIGNED_EDITOR_LIBRARY"
```

The native fixture runs real OpenHarmony mouse/touch conversion, Input, Viewport,
RichTextLabel, ScrollBar and selection timers against a touchscreen-capable
headless display. Both directions must scroll monotonically while a physical
mouse is held outside the log, including stationary and zero-raw-delta motion;
release stops scrolling and preserves the selection. In-bounds mouse selection
must not pan content, and native finger panning remains functional.
`--expect-original-bug` with the previous library reproduces the opposing pan and
selection scroll paths. This is an actual native control regression; graphical
acceptance in the installed HAP is a separate check.

The DeviceRunSession fixture now covers distinct device/host port pairs,
occupied candidates with a real local Editor listener, simultaneous debugger and
file-server command-line remapping, borrowed-rule preservation, and failure
rollback/cleanup retries. The real HDC broker port test separately verifies TCP
byte roundtrips and preservation of all preexisting rules.

`test_debug_port_export.py --sdk "$NATIVE_SDK" --library "$SIGNED_EDITOR_LIBRARY"
--template "$GAME_TEMPLATE_ZIP"` separately starts the real Editor debugger with
its configured port occupied. It requires the actual fallback listener port in
both HDC mapping preparation and the `_cl_` emitted by the real C++ exporter,
including remote file-server arguments. It uses temporary settings/project
paths, simulated HDC commands and project-only export; no HAP install occurs.
The previous library must fail the normal regression; `--expect-original-bug`
requires a successful matching-port control followed by the exact fallback
mapping error.

The native device-type configuration test in `test_project_config.py` accepts
`GODOT_OHOS_TEST_NATIVE_SDK` and `GODOT_OHOS_TEST_LIBRARY`. It exercises all 63
nonempty selection masks for each SDK mode and requires invalid input to leave
all configuration documents unchanged. The real export script also checks
preset defaults, mixed masks and rejection before creating an output directory.

Editor-generation tests require both fresh projects and updated archives to
omit bundled template ZIPs/manifests. Host startup runs without template-resource
imports; separate template downloads use preset Custom Template paths.

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

The first test calls the actual C++ exporter and official Hvigor wrapper
for unsigned debug HAP, release HAP and APP output, including paths with spaces.
No signing/install/dependency download is performed. Its runtime OS and device
expectations come from the selected real SDK metadata: HarmonyOS/phone for the
official complete bundle, OpenHarmony/default for a versioned component set.
Use `--command-line-tools-root` and `--sdk-root` to select explicit installations;
compile/target/compatible SDK remain dotted 26.0.0.
The second uses a real Godot TCP protocol peer and real exported GDScript: it
checks breakpoint/stack/local values, next/continue, actual RemoteSceneTree and
release debugging rejection with a positive release-boot control. It is not
GUI/HAP/HDC acceptance. Both clean up their isolated runtime projects/processes.

[build_deveco_project.py](<build_deveco_project.py>) remains useful for an isolated
source project or `--arkts-only`; select official tools with
`--command-line-tools-root`. It validates the coherent Hvigor/plugin and prepared
wrapper cache, then invokes the official `hvigorw.js` using the bundled Node. Personal signing
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

## Automatic debug signing and Editor Settings help

```sh
node --test platform/openharmony/tests/test_auto_sign.cjs
python3 platform/openharmony/tests/test_auto_sign_policy.py \
  --sdk "$NATIVE_SDK" --library "$SIGNED_EDITOR_LIBRARY"
python3 platform/openharmony/tests/test_editor_settings_docs.py \
  --sdk "$NATIVE_SDK" --library "$SIGNED_EDITOR_LIBRARY"
python3 platform/openharmony/tests/test_auto_sign_export.py \
  --sdk "$NATIVE_SDK" --library "$SIGNED_EDITOR_LIBRARY" \
  --template "$DEBUG_TEMPLATE_ZIP" --command-line-tools-root "$CLT_ROOT"
```

The Node suite executes the production helper with local executable CLI/HDC,
profile-verifier and build-runner substitutes. It covers exact public CLI/profile
schemas, SDK ACL classification, zero-exit logged-out status, team selection,
UDID/ACL/expiry changes, automatic first generation and renewal, cache reuse,
generation rollback, stable project identities, real permission checks after
chmod, copied login isolation, certificate bundles, locks, SIGTERM cleanup,
restored public profiles and redacted one-object receipts. It cannot issue a
certificate, log in, install an application or send a cloud request.

The native policy fixture checks the production inline signing decision on real
EditorSettings and export presets, including default-enabled switches and all
manual fields. Complete but invalid manual signing remains a manual error when
Always Auto Sign is off. Ordinary public export validation stays strict.

The documentation fixture checks real EditorHelp descriptions both after normal
startup and after regeneration, using an existing core setting as a positive
control. With an old signed Editor library, `--expect-missing-docs` must reproduce
the missing exporter descriptions. This is documentation-data acceptance; it does
not hover a setting in an installed GUI.

The native export fixture needs the rebuilt library's Dictionary context API. It
exports real projects/PCKs from copies of a real debug template and invokes real
CLT Node through the production C++ bridge. Only the helper entry is replaced by
a fixed local protocol substitute. Success without confirmation, rejection of
legacy confirmation helpers, logged-out messages, malformed/secret output, invalid signing receipts, unsafe
artifact paths and old templates are checked without CLI/HDC/network calls.
The output HAP bytes are synthetic. Real cloud issuance, the installed GUI's
automatic signing/deployment and a device's signature trust remain separate acceptance.

## Native manual signing with disposable credentials

```sh
python3 platform/openharmony/tests/test_signed_export.py \
  --editor "$SIGNED_EDITOR_CLI" \
  --template-debug-zip "$DEBUG_TEMPLATE_ZIP" \
  --template-release-zip "$RELEASE_TEMPLATE_ZIP" \
  --sdk-root "$SDK26_VIEW" --command-line-tools-root "$CLT_ROOT" \
  --output "$NEW_PRIVATE_SIGNED_EXPORT_EVIDENCE_DIR"
```

This gate generates a disposable local CA, certificates and PKCS12 in private
TMPDIR, then performs real C++/Hvigor signed debug HAP and release APP exports,
verifies each exported HAP/APP's own signature with the native SDK signer, and rejects a wrong
password. It never issues cloud certificates, uses personal signing material or
installs an application. Correct and incorrect passwords use opaque AES-GCM
frames with adjacent private material; the selected SDK's exported DecipherUtil
must recover each original value exactly before the test proceeds. This producer
exists only in the fixture; production keeps its encrypted-password validation.

The versioned signer view must expose `26.0.0/toolchains/lib/hap-sign-tool` and
preserve the selected SDK's actual runtime metadata. For an official bundled
HarmonyOS SDK, keep `default/sdk-pkg.json` and its real component tree visible;
a version-only OpenHarmony component alias loses the HarmonyOS runtime identity.
Use a private prepared `HVIGOR_USER_HOME` for wrapper/project caches. A copied
offline wrapper cache can be reused in TMPDIR without changing the original
cache; temporary SDK views and cache copies must be cleaned on exit. Evidence
retains only redacted logs, hashes and checks, and explicitly records that file
signature verification does not prove device installation trust.

APP acceptance uses the standard single `assembleApp` invocation and verifies
the exported APP's signature. It does not force the optional all-signed extension
or require each embedded HAP to be signed independently. Earlier failures from
that extra requirement are retained as historical evidence, not the acceptance
criterion for ordinary signed APP export. Source/material/output paths with spaces,
profile restoration, secret cleanup and wrong-password rejection remain covered.
