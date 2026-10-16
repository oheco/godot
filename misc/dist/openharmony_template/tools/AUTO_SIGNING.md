# Automatic debug HAP signing backend

The editor invokes the selected Node with `auto-sign.cjs --request-json <JSON>` through its normal tool executor, including its broker when required. All request fields are public metadata. The helper selects and validates private signing materials and runs `tools/build.cjs` in the same host namespace. The editor must not attempt to open or translate the helper's private material paths.

Remote debug signing is automatic: valid application materials are reused without `signature generate`; missing, expired or incompatible materials trigger one generation attempt and then a build. There is no confirmation response, confirmation token, pending capability or HMAC key. Existing legacy confirmation files are not used or unnecessarily removed. The official CLI can replace team-wide cloud certificates during generation; this automatic behavior is intentional. Failed generation restores local backups only and cannot undo a cloud certificate replacement.

No command logs in, installs the CLI, changes the user's npm prefix, installs/uninstalls an application or blindly retries generation. Ordinary exports retain their existing manual signing flow. Signing configurations are never written back to the source export preset.

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
  "loginDataDir": ""
}
```

`projectIdentity` is the stable absolute source project filename, never the temporary exported directory. The cache key is SHA-256 of project identity, bundle name and selected team. UDID and permissions are deliberately excluded from this key so target or ACL changes are checked against the same application cache. CLI regeneration can replace its signing key and cloud certificate.

An empty `cliPath` searches PATH for `devecocli`, the selected Node prefix's `lib/node_modules/@deveco/deveco-cli/cli.js`, then the running oo Node installation's `bin/devecocli`. An explicit path must identify an entry file or a symlink to it; no shell command is accepted. The selected Node must be version 22 or newer; the build runner retains its own stricter toolchain requirements.

An empty `loginDataDir` uses the original `DEVECO_CLI_DATA_DIR`, then the original `HOME/.config/deveco-cli`. An empty team is accepted only when `auth team list` identifies exactly one team. Generation always supplies `--team-id` explicitly. The helper reads the device UDID with the supplied `hdc -t <target> shell bm get -u`; it never treats an HDC serial as a UDID.

Stdout contains exactly one JSON object and a newline. Expected success and error outcomes exit zero:

```json
{"schemaVersion":1,"status":"ok","signed":true,"format":"hap","mode":"debug","outputPath":"/absolute/public/exported-project/entry/build/default/outputs/default/entry-signed.hap","profileValidated":true}
```

```json
{"schemaVersion":1,"status":"error","code":"LOGIN_REQUIRED","message":"Run devecocli auth login outside the editor, then retry. The helper never logs in."}
```

Unexpected fatal failures also produce one safe JSON error object but exit one. CLI/build stdout and stderr are captured and discarded; raw exceptions, credentials, encrypted passwords, private material paths and login contents are never forwarded. The editor should show only the safe `message` and use `code` for decisions. Generated-material validation failures include `Validation reason: <internal-enum>.` in that message, such as `PROFILE_ACL_MISMATCH` or `CERTIFICATE_MISMATCH`, without adding raw CLI output, paths, account data, certificates or UDIDs. Existing protocol codes are unchanged: a device mismatch remains `PROFILE_DEVICE_MISMATCH`; other generated-profile validation failures remain `PROFILE_INVALID`.

## Isolation, validation and automatic generation

Persistent data uses verified 0700 `XDG_CONFIG_HOME/godot-openharmony-signing` outside HOME. Temporary work uses private TMPDIR, with 0600 files and cleanup on every normal/signal exit. HOME/hmdfs paths and silent chmod failures are rejected. The helper clones token.enc/token.dek, KEK versions 1–3 and available HDC key components (`hdckey`, optional `hdckey.key`, `hdckey.pub`) into its own CLI HOME. Original login/key files are read-only. Private refreshes are retained until the original credential metadata changes; inherited DevEco Code authentication selectors are cleared so copied CLI login cannot be redirected to another account source.

HDC is the host-service exception to CLI HOME isolation. Copying HDC keys does not preserve the shared HDC service identity: running HDC under another HOME can replace the host service and lose its connected targets. The helper's direct UDID query therefore uses the original host environment. The CLI still runs with its private HOME, copied login and private `.ohos` materials. An explicit Node `--import` preload adjusts only CLI child-process calls to the selected request `hdcPath` or `commandLineToolsRoot/sdk/default/openharmony/toolchains/hdc`, matched against their resolved executable paths. Only those HDC children receive the original HOME and original presence/values of `HDC_SERVER` and `OHOS_HDC_SERVER_PORT`. Unrelated executables, including another file merely named `hdc`, retain the CLI's environment.

The preload's fixed source and small path/environment JSON are created with verified 0600 permissions inside the verified 0700 request work directory, and removed with that directory. Project input supplies no preload code; inherited NODE_OPTIONS/NODE_PATH/NODE_OPTS remain cleared. The adapter covers `spawn`, `spawnSync`, `execFile`, `execFileSync`, ESM builtin exports and `execFile`'s custom promisifier. It does not patch the installed CLI/SDK, intercept shell command strings or propagate into unrelated Node grandchildren. A future CLI using another HDC path or a shell-based invocation requires an explicit compatibility update. The helper does not start, restart or reconnect HDC; real shared-service/remote/cloud behavior must be verified separately with exclusive device ownership.

Project locks protect each application cache. When generation may be needed, team and CLI locks are acquired in addition to the held project lock. Under those locks the helper rechecks copied login, selected team, actual UDID, current project metadata and the current cache. A newly valid cache is reused immediately; unrelated changes in shared CLI application state do not require user interaction. A bundle or automatically selected team changing during this request produces a safe error instead of generating under a stale cache identity. Permissions/ACL changes are validated using the current metadata, and a changed UDID is used for the current generation.

The helper validates debug Profile bundle, validity, developer certificate, target UDID and ACL subset. Ordinary permissions are not all ACLs. It uses the selected SDK's permissionDefinitions and version metadata to derive actual profile ACLs, including version-dependent ACL exceptions; unknown or unsupported permission metadata produces a safe error. Cached ACL context must match the current required ACLs. When the SDK-derived required ACL list is empty, the optional Profile ACL field is not required to have a particular representation: missing, null, empty or other forms do not create an ACL requirement or grant any permission. When at least one ACL is required, `acls.allowed-acls` must be an explicit array containing only strings and covering every required ACL; missing/null/empty/malformed or unknown policies cannot substitute for those grants. The SDK permission selection algorithm is unchanged, and unknown requested permissions still fail. CER chains may contain the matching developer certificate after other entries; its validity is checked separately. Native SDK Profile verification and all bundle, target-device, validity, certificate and private-storage checks remain required for both ACL-free and ACL-requiring materials.

If no compatible cache remains after the locked recheck, local application and CLI `.ohos` state are backed up. A fresh private minimal project with the current bundle and permissions is supplied to public `signature generate --team-id <id> --product default`; stale cached signing paths are not passed back to the CLI. The helper invokes that command at most once per request, validates its generated files in the same locked context, copies them into private cache and invokes the existing build runner. Generation errors or invalid output restore the prior local cache and CLI state and release locks. Build errors do not discard an otherwise valid cache or trigger another generation attempt. No private CLI signing library is extracted or called.

A complete manual signing preset remains the Editor's policy decision. The automatic helper receives no manual preset credentials. The build runner may temporarily supply its private signing data to the exported project's SDK profile, then must restore the original public profile. A successful receipt must name an existing, signed debug HAP inside the public exported project. The helper does not install that HAP; the Editor still owns HDC deployment and running.

## Tests

```sh
node --test platform/openharmony/tests/test_auto_sign.cjs
```

Tests execute the actual helper with local fake CLI, HDC, native verifier and build runner programs, including paths with spaces. They cover first-call generation without a token, cache hits, automatic expiry/device/ACL updates, real generation lock ownership, locked context/cache rechecks, unused legacy confirmation files, unchanged source presets, failed-generation rollback without retry, authentication/team/device errors, malformed data, private filesystem permissions, CLI discovery, build receipts, original login isolation, signal cleanup and secret sentinel suppression. They also run fake HDC as real CLI child processes through all adapted subprocess APIs, verify both allowed HDC paths use original HOME/server settings, verify CLI and unrelated child HOME remain private, check optional third key copying, preload permissions/cleanup and Node-injection suppression, and check original presets, login and key bytes remain unchanged. They make no real HDC service, device, cloud signing request or SDK build.

The CommonJS test API exports `run(request, env?)`, `main(argv?)`, `validateRequest`, `parseTeams` and `SigningError`; `run` returns a receipt and throws only on unexpected fatal failures. C++ caller and native protocol tests must use the token-free request and accept only `ok` or `error` outcomes.
