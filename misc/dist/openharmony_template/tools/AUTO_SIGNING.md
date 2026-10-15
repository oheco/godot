# Automatic debug HAP signing backend

The editor invokes the selected Node with `auto-sign.cjs --request-json <JSON>` through its normal tool executor, including its broker when required. All request fields are public metadata. The helper selects and validates private signing materials and runs `tools/build.cjs` in the same host namespace. The editor must not attempt to open or translate the helper's private material paths.

No command logs in, installs the CLI, or modifies the user's npm prefix. Existing valid application materials are reused without `signature generate`. Every branch that could call that command first returns a confirmation response. The editor must display the team-wide certificate replacement warning and repeat the request with the returned token only after the user confirms it.

## Request and response

The request is one strict JSON object; all listed keys are required and unknown keys are rejected:

```json
{
  "schemaVersion": 1,
  "projectDir": "/absolute/public/exported-deveco-project",
  "projectIdentity": "/absolute/original/source/project.godot",
  "nodePath": "/absolute/selected/node",
  "commandLineToolsRoot": "/absolute/command-line-tools",
  "sdkRoot": "/absolute/selected/sdk",
  "hdcPath": "/absolute/selected/hdc",
  "target": "hdc-device-serial",
  "format": "hap",
  "buildMode": "debug",
  "cliPath": "",
  "teamId": "",
  "loginDataDir": "",
  "confirmationToken": ""
}
```

`projectIdentity` is the stable absolute source project filename, never the temporary exported directory. The cache key is SHA-256 of project identity, bundle name and selected team. UDID and permissions are deliberately excluded from this key so target or ACL changes are checked against the same application cache. A confirmed CLI regeneration can still replace its signing key and cloud certificate.

An empty `cliPath` searches PATH for `devecocli`, the selected Node prefix's `lib/node_modules/@deveco/deveco-cli/cli.js`, then the running oo Node installation's `bin/devecocli`. An explicit path must identify an entry file or a symlink to it; no shell command is accepted. The selected Node must be version 22 or newer; the build runner retains its own stricter toolchain requirements.

An empty `loginDataDir` uses the original `DEVECO_CLI_DATA_DIR`, then the original `HOME/.config/deveco-cli`. An empty team is accepted only when `auth team list` identifies exactly one team. Generation always supplies `--team-id` explicitly. The helper reads the device UDID with the supplied `hdc -t <target> shell bm get -u`; it never treats an HDC serial as a UDID.

Stdout contains exactly one JSON object and a newline. All expected outcomes exit zero:

```json
{"schemaVersion":1,"status":"confirmation_required","confirmationToken":"<64 lowercase hex characters>","reason":"CACHE_MISSING","message":"<team certificate replacement warning>"}
```

```json
{"schemaVersion":1,"status":"ok","signed":true,"format":"hap","mode":"debug","outputPath":"/absolute/public/exported-project/entry/build/default/outputs/default/entry-signed.hap","profileValidated":true}
```

```json
{"schemaVersion":1,"status":"error","code":"LOGIN_REQUIRED","message":"Run devecocli auth login outside the editor, then retry. The helper never logs in."}
```

Unexpected fatal failures also produce one safe JSON error object but exit one. CLI/build stdout and stderr are captured and discarded; raw exceptions, credentials, encrypted passwords, private material paths and login contents are never forwarded. The editor should show only the safe `message` and use `code`/`reason` for decisions.

`confirmationToken` is an unpredictable, privately persisted single-attempt capability, valid for ten minutes. It is bound to source project identity, bundle, target serial, actual UDID, team, requested permissions, material content, CLI signing cache state and invalidation reason. A different/stale token returns another confirmation response and cannot generate materials. Moving an otherwise identical exported directory does not invalidate it. Device and material state are checked again after the team/CLI locks are acquired. A failed generation consumes the confirmation and needs fresh user confirmation for another attempt.

## Private state and validation

Persistent state lives only at `XDG_CONFIG_HOME/godot-openharmony-signing`. Both `XDG_CONFIG_HOME` and `TMPDIR` must resolve to real directories outside the original HOME and hmdfs. The helper checks ownership and actual `0700`/`0600` modes after chmod; chmod returning success is insufficient. All temporary build requests, profile verification output and rollback snapshots live in an automatically cleaned `0700` directory below `TMPDIR`.

The CLI's child HOME is the persistent private `cli-home`, so its fixed `HOME/.ohos/config` output remains private. Original `token.enc`, `token.dek`, `kek-v1.bin` through `kek-v3.bin`, and `HOME/.harmony/hdckey{,.pub}` are copied read-only into the corresponding private HOME locations. Source metadata and digests are recorded privately: auth refresh is preserved in the private copy until the original login files change, when a new private copy is synchronized. `HDC_SERVER` and other existing HDC environment selections are inherited. Original login and HDC configuration files are never written. Inherited DevEco Code authentication redirects are cleared: this backend reuses the explicit DevEco CLI login in its private HOME. CLI-created files and directories are hardened and their effective modes checked after each command.

The stable private minimal signing project contains AppScope app metadata, the root build profile and entry module configuration; it excludes application libraries and PCK files. The full CLI signing configuration, including encrypted passwords, stays in the private cache. Imported certificate, profile, keystore and companion `material/{fd,ac,ce}` are copied into the application's own cache, preventing a later CLI team rebuild from overwriting another application's local snapshot.

The helper invokes the selected SDK's official native `hap-sign-tool verify-profile -inFile ... -outFile ...`, using its directly emitted profile JSON only after a successful verification process. It checks debug type, exact bundle, validity period, explicit target UDID membership, required ACL inclusion, validity of the matching certificate in a DER certificate or PEM chain and equality with the profile's development certificate, encrypted password frames and private local material paths. Missing or ambiguous information never counts as ready. Required ACLs follow the CLI 1.3.5 rules using its public SDK `permissionDefinitions.json` and `sdk-pkg.json` inputs: include FILE_ACCESS_PERSIST and READ_WRITE_DOWNLOAD_DIRECTORY/READ_WRITE_DOCUMENTS_DIRECTORY; exclude READ_DOCUMENT/WRITE_DOCUMENT; otherwise require `system_basic`, `NORMAL` and `provisionEnable: true`. Permission since/minimum API must be supported by the Command Line Tools' bundled SDK; numeric and dotted version strings are accepted. Missing or ambiguous metadata is an explicit error. INTERNET and MICROPHONE therefore need no ACL. Changes to the required ACL set invalidate the cache, including removal of an old ACL. The verifier reads the authentic `debug-info.device-ids` field; unrecognized all-device profile policies are rejected conservatively.

The contract follows the researched official CLI 1.3.5 (installed OpenHarmony bundle SHA-256 `1c10b4effe3e62f0af28eebb0077459a0152ab4963da7141f7ff517d32352f61`). The helper uses public commands and SDK metadata only; it never extracts/imports private CLI internals. CLI 1.3.5 only opens browser authorization in its explicit `auth login` command; signing reports missing credentials instead of starting a login flow. The official public generation command is `signature generate --team-id <team> --product default`. It never supplies `--force`. An unchanged invalid result after generation is an error, not a retry loop; a missing target UDID returns `PROFILE_DEVICE_MISMATCH`. Local app and CLI team materials are backed up privately before generation and restored if generation or post-generation validation fails. A cloud certificate replacement cannot be rolled back locally, so the failure message does not claim otherwise.

Project locks serialize a stable app cache. Team locks serialize team certificate generation, and a CLI HOME lock serializes auth refresh and shared CLI files across teams. Locks are released on normal error paths and handled SIGINT/SIGTERM. SIGKILL, power loss or process crashes can leave a lock; there is no unsafe automatic stale-lock deletion. Check for an active helper before manually removing such a private lock.

## Build handoff

Once signing is validated, the helper creates a `0600` schemaVersion 2 request for the existing build runner, supplying `certificate`, `profile`, `storeFile`, `keyAlias`, `signAlg`, `keyPasswordEncrypted` and `storePasswordEncrypted`. `projectDir` remains the exported public project. `hvigorUserHome` uses the original HOME's `.hvigor` to preserve prepared offline dependencies. The selected Node executes `projectDir/tools/build.cjs --request <private-file>` with no secret on argv.

The helper checks byte-for-byte restoration of the exported build profile on success, failure and handled interruption. It accepts only a schemaVersion 2 receipt with `signed: true`, the requested format/mode and a nonempty regular artifact at a canonical absolute path within the exported project. Private signing state is never added to export presets or the source project's signing files.

All external processes use `execFile`, closed stdin, disabled colors, captured bounded output and timeouts. The build runner gets time to handle SIGTERM and restore its profile; a hard kill bounds tools that ignore termination.

## Local tests and remaining integration

Run from the repository root:

```sh
node --test platform/openharmony/tests/test_auto_sign.cjs
```

Tests execute the actual helper with local fake CLI, HDC, native verifier and build runner programs, including paths with spaces. They cover private filesystem permissions, silent chmod failure, exit-zero unauthenticated status, multiple teams, serial/UDID distinction, confirmation binding, expired/mismatching profiles, cache reuse, one confirmed generation, failed generation rollback, build errors/receipts, unchanged original login/build profiles, temporary cleanup, lock ownership and secret sentinel suppression. They make no real cloud signing request.

The CommonJS test API exports `run(request, env?)`, `main(argv?)`, `validateRequest`, `parseTeams` and `SigningError`; `run` returns a receipt and throws only on unexpected fatal failures. The Editor still owns the confirmation UI, request construction and HDC install/run of the public signed artifact. Ordinary exports retain their existing manual signing flow and build runner.
