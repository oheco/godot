<!-- SPDX-License-Identifier: MIT -->
# Standalone OpenHarmony automatic signing worker

[auto_signer.cpp](auto_signer.cpp) is a signed standalone C++17 executable installed as the game-template file `tools/auto-sign`. It has no Godot Core dependency. The Editor sends public request metadata to this worker through the terminal broker when broker use is enabled. Ordinary tools continue to run directly; HDC and the automatic-signing operation use the terminal namespace where necessary.

The worker invokes the installed official DevEco CLI, SDK native profile verifier, and existing project `tools/build.cjs` runner. There is no signing JavaScript helper, Node preload, private CLI import, shell command construction, automatic login, package installation, force generation, or cloud-generation retry.

## Public protocol

Invoke exactly:

```text
auto-sign --request-json <one JSON object>
```

The schema-version-1 object has exactly these 13 required keys:

| Key | Value |
| --- | --- |
| `schemaVersion` | Number `1` |
| `projectDir` | Absolute generated-project directory |
| `projectIdentity` | Absolute stable source-project identity |
| `nodePath` | Absolute selected Node executable, at least version 22 |
| `commandLineToolsRoot` | Absolute selected CLT root |
| `sdkRoot` | Absolute selected SDK home, bundle, version directory, or component root |
| `hdcPath` | Absolute selected original HDC executable |
| `target` | Nonempty selected HDC serial, without CR/LF |
| `format` | `hap` |
| `buildMode` | `debug` |
| `cliPath` | Absolute official public CLI entrypoint, or empty for discovery |
| `teamId` | `[A-Za-z0-9_-]{1,256}`, or empty for exactly-one-team selection |
| `loginDataDir` | Absolute existing CLI data directory, or empty to keep the CLI's normal environment/default |

Strings reject NUL and lengths over 32768 bytes. Unknown fields are rejected. Only public metadata crosses argv; signing frames are passed to the build runner through a request file. No token, password or signing material is returned to the Editor.

Stdout is one compact schema-version-1 JSON object and one newline. Expected errors exit zero; internal errors exit one. Child output is bounded and captured, not relayed into the protocol. A successful receipt has `status:"ok"`, `signed:true`, `format:"hap"`, `mode:"debug"`, `profileValidated:true`, and a canonical, existing, nonempty regular HAP path inside the generated project. Profile validation and artifact receipt validation are separate checks.

## Official CLI environment

The CLI uses the broker's original HOME, XDG paths and HDC identity. An explicit `loginDataDir` selects `DEVECO_CLI_DATA_DIR`; an empty selection leaves the CLI's inherited/default login selection intact. The selected original CLT is provided through `COMMAND_LINE_TOOL_PATH`, and the selected Node directory is put on PATH as needed.

The worker does not clone tokens, KEK files or HDC keys, create a separate CLI HOME, copy SDK launchers into an overlay, or install a native HDC proxy. Official DevEco CLI remains responsible for its own credentials and `.ohos` state.

There is no global `umask(0077)`, recursive chmod, caller-UID gate or exact-mode requirement. Normal file and directory creation respect the caller's environment. The worker does not try to impose Unix permission protection on HOME/hmdfs. Configuration/cache files use `XDG_CONFIG_HOME`; temporary files and directories use `TMPDIR`, with owned cleanup after the operation.

For the selected CLT Node's loader requirement, the worker can create an operation-local `node-runtime` directory containing exactly one `libc++_shared.so` symlink to that CLT's canonical runtime. Node children prepend this directory to `LD_LIBRARY_PATH`, preserving the original suffix. The entire LLVM library directory must not be added: its `libc++.so` is a linker script that breaks HDC/system-library resolution. This is a loader compatibility measure, not a credential or permission sandbox. Contents and target checks prevent accidentally selecting a different SDK runtime.

Hvigor uses the public build runner's ordinary `HVIGOR_USER_HOME` selection. No extra Hvigor HOME, wrapper overlay or process permission mask is imposed by this worker.

## Cache, generation and validation

Persistent worker state lives under:

```text
$XDG_CONFIG_HOME/godot-openharmony-signing/
  locks/{cli,project-<sha256>,team-<sha256>}/
  projects/<sha256>/
    signing-config.json
    signing-context.json
    materials/{certificate.cer,profile.p7b,store.p12,material/...}
    project/...
```

The cache identity is SHA-256 of `[projectIdentity,bundleName,teamId]`, so temporary export-directory changes reuse the application's cached result. Cached generated material is retained for stable reuse; existing CLI login and device keys are not copied or managed by this cache. Old historical private-CLI directories are not automatically deleted.

CLI/project/team locks serialize the relevant operations. A verified login and selected team are required. Before generation the worker checks the current device, live project bundle and actual ACL requirements, and rechecks the cache under locks. Exactly one `signature generate --team-id <team> --product default` is allowed for a missing/invalid cache, without `--force` or an internal retry. A minimal generated project prevents changes to the source export preset.

Validation retains:

- Strict bounded metadata and regular-file/path checks.
- Native SDK `verify-profile` acceptance.
- Debug profile type, exact bundle, validity and selected real device UDID.
- Actual required ACL grants derived from the selected CLT's SDK metadata.
- A valid developer certificate matching the profile certificate by full DER bytes.
- Supported encrypted SDK password frames, signing algorithm and required companion material.

These are signing-content checks, not Unix-mode or file-owner requirements. Verifier discovery and SDK-home normalization retain the public build runner's supported SDK layouts.

Only the worker's own application cache is backed up/restored if generation, import or validation fails. The worker does not back up, delete or restore the official CLI's normal `.ohos` directory. A failed own-cache rollback retains recovery data and reports `ROLLBACK_FAILED`. Cloud and official-CLI state remain managed by the official CLI.

## Build handoff and cancellation

The existing generated `tools/build.cjs` receives a schema-version-2 request file containing the original CLT/SDK selection, an operation work directory and seven signing fields with encrypted passwords. SDK build-tool requirements may require a newer Node than the CLI minimum. Node/build has a ten-minute deadline, generation two minutes and normal probes twenty seconds.

The public generated build profile is snapshotted before the runner and checked afterward, including after interruption. Failed restoration reports `BUILD_PROFILE_NOT_RESTORED`; no signed artifact is claimed. The source export preset remains unchanged and a failed build retains the valid signing cache.

SIGINT/SIGTERM trigger bounded process-group termination and child reaping before requests, locks and owned temporary directories are released. Descendants that outlive the direct child are terminated before cleanup. Recovery data is retained when rollback itself fails. Process crashes, SIGKILL and filesystem errors cannot provide graceful-cleanup guarantees.

## Building and focused verification

Use [build-auto-signer.py](../../build-auto-signer.py) with the native SDK. It builds the repository's vendored JsonCpp and default-config mbedTLS sources, excludes Godot-specific mbedTLS hooks, statically links the C++ runtime, checks dependencies and signs the final ELF using PATH `binary-sign-tool`. The worker's dynamic dependency is only `libc.so`.

The business fixture [test_auto_sign.cjs](../../tests/test_auto_sign.cjs) uses fake CLI/HDC/verifier/build programs under TMPDIR and supports `GODOT_NATIVE_AUTO_SIGNER`. Normal-environment regressions cover inherited HOME/CLI paths, absence of credential copies/overlays, inherited umask, and necessary selected-runtime compatibility. Run only the changed cases when prior business and frontend results already passed; do not mislabel an older full-suite result as validation of a changed policy.

Real cloud signing, game installation, debugger handshake and installed-Editor GUI interaction are separate acceptance scopes. A compile or fixture pass does not establish those scopes.
