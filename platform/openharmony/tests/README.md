# Shared OpenHarmony host verification

Run from the Godot source root. Tests download nothing. Set `TMPDIR` to a writable
native temporary filesystem; fixtures clean up their own temporary directories.
Persistent build evidence belongs in a separate cache/output directory, not the
source template or a user's configured DevEco project.

## Configuration and migration

```sh
python3 -m unittest discover -s platform/openharmony/tests -p 'test_*.py' -v
```

These tests cover game/editor/native-only profiles, API 23 defaults, escaped
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
python3 platform/openharmony/tests/test_native_shell.py --sdk "$NATIVE_SDK"
python3 platform/openharmony/tests/test_engine_arguments.py --sdk "$NATIVE_SDK"
```

The ArkTS harness loads actual shared source with explicit Harmony/NAPI mocks;
its runtime extraction tests use real temporary files and independent module
contexts. The native shell fixture compiles and signs the actual NAPI glue, but
mocks the engine/window/resource boundaries. The argument test includes the
production helper and executes both normal and hardened builds. See the
[native fixture details](<README_native_shell.md>) for requirements and limits.

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
host/project schemas. It is not a Vulkan, input, UIAbility or installed-game test.
For SDK-enabled Editor regression, also rerun
[test-dotnet.py](<../test-dotnet.py>) with the prepared native SDK/GodotSharp/feed.

## Generated DevEco compilation

```sh
python3 platform/openharmony/tests/build_deveco_project.py \
  --project "$GENERATED_PROJECT" --node-modules "$PREPARED_HVIGOR_NODE_MODULES" \
  --sdk "$HARMONY_SDK_ROOT" --output "$NEW_BUILD_EVIDENCE_DIR"
```

The source project is not modified; a private copy is built without signing or
installing. API 23 HarmonyOS configuration is preserved by default. If only an
OpenHarmony SDK is available, `--openharmony-sdk-version 26.0.0` selects an
**explicit alternate-SDK compatibility build**. This result must not be described
as HarmonyOS API 23 acceptance. `--arkts-only` checks the real ArkTS compiler without
assembling a HAP. Existing personal signing material/history is never copied into
this test's distributable output.

## Required signed-application acceptance

Before release, build with the actual HarmonyOS API 23 toolchain and signing ACLs,
then verify on a device:

- Game: PCK launch and arguments, empty/granted/denied permissions, keyboard/mouse/
  touch/IME, relative mouse and fine wheel, resize/safe-area/background/exit.
- Editor: cold/warm/concurrent runtime preparation, project manager to independent
  editor, Run/Stop attached game, successful and failed .NET builds, SDK permission
  denial, crash reporting and parent/child lifetime independence.
- Both: shutdown during startup and a new process after termination. The shared
  engine intentionally has one lifetime per process, not an in-process restart API.

Do not tag/publish a release based only on mocks, headless tests or unsigned HAP
compilation. Record the tested source commit, SDK, device and artifact hashes.
