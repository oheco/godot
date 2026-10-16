<!-- SPDX-License-Identifier: MIT -->
# Standalone OpenHarmony automatic signing worker

[auto_signer.cpp](auto_signer.cpp) is a standalone C++17 executable, installed as the **signed** game-template file `tools/auto-sign`. It has no Godot Core dependency. Editor integration, broker invocation, build-system changes, signing the executable, and packaging are deliberately outside this directory.

The Editor sends only public request metadata to the existing terminal broker. The broker starts this executable in its terminal namespace. The worker invokes the installed official DevEco CLI, SDK native profile verifier, and existing project `tools/build.cjs` runner in that namespace. Login copies, certificates, private keys, encrypted password frames, private CLI HOME, verifier output, and build requests never travel back to the Editor. The worker returns one safe JSON receipt and, on success, the existing public project HAP path.

There is no auto-sign JavaScript entry, Node preload, JavaScript HDC broker, private CLI import, shell command construction, login, installation, confirmation token, force generation, or cloud-generation retry in this implementation. The existing JavaScript build runner remains the public build interface.

## Public protocol

Invoke the executable directly with exactly two arguments:

```text
auto-sign --request-json <one JSON object>
```

The object has exactly these **13** mandatory keys, preserving the original helper's `KEYS` contract:

| Key | Type / constraint |
| --- | --- |
| `schemaVersion` | Number `1` |
| `projectDir` | Absolute generated-project directory |
| `projectIdentity` | Absolute stable source-project identity, used only in the cache identity |
| `nodePath` | Absolute selected Node executable; runtime version must be at least 22 |
| `commandLineToolsRoot` | Absolute selected CLT directory |
| `sdkRoot` | Absolute selected SDK home, bundle, version directory, or component root |
| `hdcPath` | Absolute selected original HDC executable |
| `target` | Nonempty selected HDC serial, without CR or LF |
| `format` | `hap` |
| `buildMode` | `debug` |
| `cliPath` | Absolute public CLI entrypoint, or empty for discovery |
| `teamId` | `[A-Za-z0-9_-]{1,256}`, or empty for exactly-one-team selection |
| `loginDataDir` | Absolute existing original CLI data directory, or empty for the normal environment/HOME fallback |

All string fields reject NUL and exceedance of 32768 bytes. Unknown fields, including the old `hdcBrokerEndpoint` and `confirmationToken`, are rejected. The worker itself is already inside the broker; nested HDC broker requests are unnecessary.

Normal stdout consists of one compact schema-version-1 JSON object and one newline. Normal signing errors exit **0**; unexpected internal failures exit **1**. Child stdout/stderr are captured and discarded and never become protocol output. No raw exception, CLI diagnostic, certificate, encrypted password, device UDID, account identifier, or private path is printed.

```json
{"schemaVersion":1,"status":"ok","signed":true,"format":"hap","mode":"debug","outputPath":"/absolute/public/project/entry/build/default/outputs/default/application-signed.hap","profileValidated":true}
```

```json
{"schemaVersion":1,"status":"error","code":"LOGIN_REQUIRED","message":"Run devecocli auth login outside the editor, then retry. The helper never logs in."}
```

The JSON member order is unspecified. Success requires a nonempty, existing, regular `.hap` with a canonical path strictly inside the public project and a matching signed debug-HAP build receipt. This is the same build-receipt trust boundary as the original helper; SDK profile signature verification is a separate mandatory step.

Stable public error families include `INVALID_REQUEST`, `INVALID_JSON`, `INVALID_PROJECT`, `NODE_MISSING`, `NODE_VERSION_TOO_OLD`, `CLI_MISSING`, `CLT_MISSING`, `PRIVATE_STORAGE_REQUIRED`, `LOGIN_DATA_INVALID`, `LOGIN_REQUIRED`, `TEAM_REQUIRED`, `DEVICE_UNAVAILABLE`, `DEVICE_UNAUTHORIZED`, `ACL_METADATA_INVALID`, `ACL_UNSUPPORTED`, `CACHE_BUSY`, `CACHE_INVALID`, `PROJECT_CHANGED`, `NETWORK_ERROR`, `SIGNING_FAILED`, `PROFILE_INVALID`, `PROFILE_DEVICE_MISMATCH`, `BUILD_FAILED`, `BUILD_PROFILE_NOT_RESTORED`, `INTERRUPTED`, and `INTERNAL_ERROR`. `ROLLBACK_FAILED` explicitly reports an unsuccessful local recovery rather than claiming old files were retained.

## Namespace and private storage

The worker uses its actual broker-provided environment, including the original terminal `HOME`. Both `XDG_CONFIG_HOME` and `TMPDIR` must name existing real directories outside HOME/hmdfs; neither may traverse symlinks. The generated signing cache and temporary work directory must pass effective Unix-mode and caller-ownership checks. A successful `chmod` return alone is insufficient on HarmonyOS hmdfs.

Long-lived state lives under:

```text
$XDG_CONFIG_HOME/godot-openharmony-signing/
  locks/{cli,project-<sha256>,team-<sha256>}/
  cli-home/
  login-source.json
  projects/<sha256>/
    signing-config.json
    signing-context.json
    materials/{certificate.cer,profile.p7b,store.p12,material/...}
    project/...
```

Generated private directories use 0700; private files use 0600. Files must be caller-owned, regular, single-linked, bounded in size, and free of symlink traversal where they are used as signing material. Reads use `O_NOFOLLOW` and compare opened-file identity. Writes verify ownership/link count and effective permissions before truncation. Executable overlay copies use 0700: the signed worker at the HDC proxy path, the original signed CLT Node, and bin siblings whose source has executable bits. Their bytes remain unchanged; Node and other copied launchers have a separate 128 MiB bound. The read-only original tool and login files are not chmodded or rewritten.

Only existing `token.enc`, `token.dek`, KEK v1/v2/v3, and optional `.harmony/{hdckey,hdckey.key,hdckey.pub}` are copied. Source size, mtime and SHA-256 determine when to refresh copies; removed source files remove their obsolete private copies. The source login directory must not be inside the worker's destination cache. Auth status must prove a verified CLI login even when the CLI exits zero.

Private CLI HOME, XDG configuration/data/cache/state paths and TMP variables are child-local. `NODE_OPTIONS`, `NODE_PATH`, `NODE_OPTS`, `DEVECO_CLI_AUTH_SOURCE` and `DEVECO_CODE_AUTH*` are removed before starting Node. CLI output is never relayed. Child processes get closed stdin and bounded captured streams.

For the broker namespace's selected CLT Node loader issue, the worker checks for the selected CLT's regular runtime at `sdk/default/openharmony/native/llvm/lib/aarch64-linux-ohos/libc++_shared.so`. When present, it creates a caller-owned real 0700 directory at `<request work directory>/node-runtime` containing exactly one symlink, `libc++_shared.so`, to that runtime's canonical absolute path. Every reuse checks the directory's effective permissions and ownership, its exact contents, and the link's ownership and exact target. A stale link to another SDK, an extra library, or a symlink replacing the directory is rejected. When the source runtime is absent, no loader path is added.

Only Node version, CLI and build child environments prepend this private directory to `LD_LIBRARY_PATH`, preserving the broker's original value as the unchanged suffix. An absent or empty original value adds no empty search component. Their descendants inherit the same one-SONAME directory. The compiler library directory itself must never be added: its `libc++.so` is a linker input rather than a suitable system runtime and can break HDC and other SDK children that require the system `libc++.so`. The private directory exposes only Node's `libc++_shared.so`, allowing those other SONAMEs to resolve normally.

Version checking runs after the request's verified TMPDIR work directory is created, so all three Node call sites use that same request-local runtime. Normal cleanup removes it with the work directory, including on cancellation and recoverable failure. The worker's own environment, direct HDC and verifier loader environments, and original HOME sources are unchanged. The selected Node remains selected for CLI and build calls; it is not silently replaced with the global Node. No additional public request or broker environment fields are needed.

## Native HDC proxy and CLT overlay

The official CLI locates HDC at `COMMAND_LINE_TOOL_PATH/sdk/default/openharmony/toolchains/hdc`. A private CLI HOME must not create a new HDC shared-service identity. Before the first CLI invocation, the worker therefore creates a temporary CLT overlay with real 0700 ancestor directories down to `toolchains` and an unchanged byte copy of the signed worker at `toolchains/hdc`.

Official CLI 1.3.5's OpenHarmony provider realpaths the explicit `node/bin/node`, `ohpm/bin/pm-cli.js`, `hvigor/bin/hvigorw.js`, `sdk`, and `sdk/default/openharmony/toolchains/hdc` entries and requires them to remain within the CLT root. Its emulator and Java paths are empty; clangd and LSP paths are not part of this guard. A symlink for any of the three guarded tool entries would resolve outside the temporary root and fail initialization before HDC or generation. The overlay therefore uses real `node`, `ohpm`, `hvigor` and `bin` directories and native byte copies of their regular bin files, including launcher siblings needed for relative imports. The Node copy retains its original ELF signature bytes and receives effective 0700 permissions. Its required `libc++_shared.so` comes from the selected CLT through the private one-SONAME runtime directory described above.

Non-guarded siblings, including `node_modules`, `engine`, `plugins`, `lib`, `version.txt`, SDK metadata and hms, may remain symlinks to the original read-only components. Only explicit guarded paths must resolve within the overlay. No installed CLT, SDK or CLI component is modified, and no guard is disabled.

Only CLI subprocesses receive `COMMAND_LINE_TOOL_PATH=<temporary overlay>`. Builds explicitly receive the **original selected CLT**. No overlay is used as a build toolchain selection.

When its executable basename is `hdc`, the worker enters proxy mode and reads its caller-owned 0600 sibling `.godot-hdc-host.json` inside the real 0700 toolchains directory. The equivalent internal interface is:

```text
auto-sign --hdc-host <private absolute config path> -- <original HDC arguments...>
```

The configuration has `schemaVersion:1`, canonical `hdcPath`, and `hostEnvironment` containing original `HOME` plus the originally present `HDC_SERVER` and `OHOS_HDC_SERVER_PORT`. No other environment keys are accepted. The proxy restores exactly those values, removes a server override if it was absent originally, rejects recursion, and `execve`s the original selected HDC with the unchanged argument vector. Proxy errors exit 126/127 without diagnostics. HDC streams are visible only to the invoking CLI, whose own streams the worker captures. Unrelated CLI children keep the private CLI HOME.

The normal device-UDID query invokes the original selected HDC directly with original host HOME and arguments `-t <target> shell bm get -u`. The serial is never substituted for the one unique 64-hex-character UDID. Unauthorized and unavailable devices produce distinct safe errors.

## Cache, generation and verification

The cache identity is SHA-256 of the compact JSON array `[projectIdentity,bundleName,teamId]`. Temporary export-directory changes therefore reuse the same application's cache. Cache files retain the original layout and encrypted frame representation.

The worker authenticates and selects the team under the CLI lock, then holds a project lock through validation/build. When generation is necessary it additionally acquires the team and CLI locks, refreshes original login copies, authenticates, checks team stability, queries the current device again, refreshes live project permissions, rejects a changed bundle, and rechecks the cache. A cache that became valid during this interval is reused without generating. Locks are never stolen or automatically deleted as stale; destructors remove only a lock inode created by this process.

ACL derivation intentionally reads the same **selected CLT SDK** public inputs as the original helper and official CLI: `sdk/default/sdk-pkg.json` and `sdk/default/openharmony/toolchains/lib/permissionDefinitions.json`. It validates API/since/minimum metadata, duplicate definitions, complete requested-permission definitions and supported API levels. It preserves the three explicit provision-inclusion exceptions and two document-permission exclusions. Unknown permissions fail; a blind list of all requested permissions is never treated as required ACLs.

Verifier discovery preserves CLT-first discovery and supports SDK home, `default` bundle, direct component root, and numeric version directories. The native `hap-sign-tool verify-profile` is mandatory. Build SDK selection retains the public `sdk-config.cjs` home/bundle contract; a direct Harmony `default/openharmony` component selection is normalized to its SDK home for the build request. ACL metadata remains from the CLT that the official CLI will actually consume.

A reusable/generated profile must pass all of:

- SDK-native signature verification with an existing private verified JSON output.
- Exact debug type and bundle name.
- Integer validity bounds, including strict expiration at the upper boundary.
- Explicit UDID device policy, only well-formed 64-hex device IDs, and actual current device membership.
- Every actually required ACL as an explicit string grant. If no ACL is required, the SDK's optional ACL field may be missing, null, or otherwise represented; it grants no additional permissions through this worker.
- A parseable X.509 certificate matching the profile's development certificate by complete DER bytes and valid at the current UTC time. All certificates in a PEM CER chain are considered, rather than assuming its first entry is the developer certificate. DER CER files are also supported.
- Encrypted key/store passwords retaining the official hexadecimal frame (`uint32 BE content length`, 12-byte IV, content with at least the 16-byte tag), supported signature algorithm, private material paths, and nonempty `material/{fd,ac,ce}` companions.

Only a missing/invalid cache reaches one `signature generate --team-id <team> --product default` call. There is no `--force` and no retry. Generation uses a minimal private project stripped of old signing configurations. Before that call, the full application cache and existing private CLI `.ohos` tree are backed up. Any failed generation/import/verification restores both local trees while locks remain held. Both restoration attempts run independently. On a failed local restore, recovery copies are retained under the private cache's `recovery/recovery-XXXXXX` directory; if configuration-storage failure also prevents that transfer, the original verified 0700 TMPDIR work directory is retained instead of deleting the only complete backups. `ROLLBACK_FAILED` requires inspection of those private recovery copies before retrying, without disclosing their paths in the receipt. Normal success and recoverable failures remove their temporary directories. Staging copies work when configuration and temporary directories are on different filesystems. Existing cloud certificate authorization cannot be rolled back locally; generation failures explicitly say so.

## Build handoff and cancellation

The worker passes a private schema-version-2 request file to the existing generated `tools/build.cjs`. It includes original CLT selection, SDK selection, private work directory, and the seven exact signing fields, with passwords still encrypted. No secret is put in argv or a new environment variable. The Node/build timeout is ten minutes; CLI signature generation is two minutes and normal probes are twenty seconds.

The private `umask(0077)` remains in force for builds. HarmonyOS HOME/hmdfs maps ownership and can make a new owner-only Hvigor `project_caches` directory inaccessible to its creator, so builds must not write those workspaces under HOME. Each request instead uses a real caller-owned 0700 `<request work directory>/hvigor-home` under verified TMPDIR. Its `wrapper` points to the canonical already-prepared wrapper dependency tree selected by the broker's `HVIGOR_USER_HOME`, or the original HOME's `.hvigor` fallback. An existing global `hvigor-config.json5` is copied as a private 0600 file, preserving the public runner's dependency/options validation and preventing changes to the original configuration. Writable project caches and other Hvigor state remain in the private temporary home and are removed with the request; installed dependencies are reused without installing packages or widening private-file permissions.

The worker snapshots public `build-profile.json5` bytes before the runner and compares them after the child has closed, including after interruption. A failed restoration returns `BUILD_PROFILE_NOT_RESTORED`. It does not falsely report an artifact as successful or overwrite the source project's ordinary export presets/credentials. A failed build retains a valid signing cache for a subsequent build.

`SIGINT` and `SIGTERM` set a cancellation flag. Active process groups receive SIGTERM and then SIGKILL after five seconds. `waitid(WNOWAIT)` observes the direct child's exit while reserving its PID until group escalation and a one-second final drain have finished; closing pipes or an early leader exit cannot skip cancellation of remaining descendants. The direct child is then reaped before requests, locks or temporary directories are released. Continuous child output cannot starve the signal/deadline loop. Temporary cleanup is explicit before a successful response, with RAII cleanup on errors; SIGKILL, process crashes and filesystem failures cannot provide the normal graceful-cancellation guarantee.

## Building without Godot

Use the native SDK 26 `clang` for the vendored mbedTLS C sources and `clang++` for C++ sources, with the same default mbedTLS configuration for all translation units. Do **not** inherit Godot's custom mbedTLS platform hooks or `-fno-exceptions`.

Inputs:

- This directory's `auto_signer.cpp`, C++17 with exceptions.
- `thirdparty/openxr/src/external/jsoncpp/src/lib_json/{json_reader,json_value,json_writer}.cpp`.
- `thirdparty/mbedtls/library/*.c`, excluding `godot_core_mbedtls_platform.c`. The latter is a Godot platform shim and is unnecessary with mbedTLS's standalone default configuration. Unused default-library features can be removed by normal link-time garbage collection.

Include directories:

```text
thirdparty/openxr/src/external/jsoncpp/include
thirdparty/mbedtls/include
thirdparty/mbedtls/library
```

Compile each source to its own object. Link using native `clang++ -static-libstdc++ -static-libgcc -Wl,--gc-sections <objects> -o <private staging>/auto-sign`. Static C++ runtime linkage is required for the broker App namespace. Inspect dynamic dependencies in the final packaging process to ensure `libc++_shared.so` is not needed by the worker itself. After all ELF changes, the integration owner signs the final worker with the normal platform binary signing tool and installs it at `tools/auto-sign` with executable permissions. The overlay copies those final bytes without altering them.

A syntax-only command, from repository root, is:

```sh
/storage/Users/currentUser/.oheco/packages/ohos-sdk-native/26.0.0.35-Beta/llvm/bin/clang++ \
  -std=c++17 -fexceptions -Wall -Wextra -Wpedantic -Werror -fsyntax-only \
  -Ithirdparty/openxr/src/external/jsoncpp/include -Ithirdparty/mbedtls/include \
  platform/openharmony/export/auto_signer/auto_signer.cpp
```

The existing 37-case fixture suite remains in `platform/openharmony/tests/test_auto_sign.cjs`, with the legacy JavaScript helper as its default reference backend and `GODOT_NATIVE_AUTO_SIGNER` selecting the signed native worker. Its fixed-SDK case retains realpath containment, signed Node byte-copy and relative-launcher checks, and also copies a real signed CLT runtime as fixture data beside a poison `libc++.so` and an unrelated SDK library. `GODOT_NATIVE_NODE_RUNTIME` can select that read-only runtime input; its default is the installed CLT 26.0.0.201 runtime. No installed CLI, HDC or SDK launcher is executed. Fake CLI, ordinary SDK child/grandchild, all existing fake HDC subprocess APIs, unrelated children, verifier and build processes assert their actual inherited loader environments. Native checks require the one-entry private directory and precise selected-runtime target, preserve the original LD suffix, cover an absent original LD value and absent runtime, and reject wrong-SDK links, extra libraries and replaced runtime directories. These native-only checks branch without skipping the legacy backend's 37 cases. All fixture files live under an explicit absolute TMPDIR and are cleaned by test teardown.

Implementation verification so far: native SDK 26 syntax compilation with the above diagnostics passed, and a full standalone default-mbedTLS/JsonCpp link (108 translation units, static C++ runtime flags) passed in a private TMPDIR staging directory which was then removed. Native `llvm-readelf -d` confirmed the linked worker's only `NEEDED` entry was `libc.so`, with no `libc++_shared.so` dependency. No real CLI, HDC, cloud signing, installation, user GPU setting, SCons build or live acceptance was invoked. Runtime fixture parity, signed proxy execution in the broker, selected CLT Node loader behavior, and real signed-HAP acceptance remain the integration owner's validation tasks; a successful compile/link is not a claim that those runtime checks passed.
