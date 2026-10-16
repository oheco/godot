# Native automatic debug HAP signing

The Editor launches the signed C++ executable `tools/auto-sign --request-json <JSON>`. Automatic signing has no JavaScript entrypoint or Node preload. The existing JavaScript build runner is retained for the official Hvigor build interface.

Broker use is explicit. Ordinary tool execution, Node.js and manual exports run directly in the Editor environment. HDC uses the broker when enabled because it requires the terminal device environment. The separate native automatic-signing operation uses the broker because official DevEco CLI calls query HDC internally, and its private signing materials, native profile verification and signing build must remain in the same filesystem namespace. Turning off `export/openharmony/use_broker` invokes these operations directly. This is not a general command-proxy setting.

Only public request metadata crosses argv. The worker owns private CLI login copies, keys, profiles, encrypted password frames and build requests; none are sent back to the Editor. A successful receipt identifies only the existing public HAP. Original login files, source export presets and application data are preserved. No command logs in, installs tools, changes an npm prefix, uninstalls an application or clears its data.

## Request and response

The request is one strict JSON object with exactly these 13 required fields:

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

String fields reject NUL and sizes over 32768 bytes. Project/tool paths must be absolute. Unknown fields and old confirmation fields are rejected. Automatic signing is limited to a debug HAP for a selected HDC target; ordinary exports retain their manual signing flow.

An empty CLI path discovers the installed official public entrypoint without importing private CLI modules or invoking a shell. Selected Node must be version 22 or newer. An empty login directory uses the inherited `DEVECO_CLI_DATA_DIR`, then the original HOME's CLI data directory. An empty team requires exactly one team from `auth team list`; generation always supplies `--team-id`. The HDC serial selects the device; `hdc -t <target> shell bm get -u` supplies its actual 64-hex UDID.

Stdout is exactly one compact JSON object and a newline. Expected errors exit zero. Unexpected internal errors exit one. Child output is bounded, captured and discarded; raw exceptions, CLI diagnostics, certificates, passwords, account identifiers and private paths are never protocol output.

```json
{"schemaVersion":1,"status":"ok","signed":true,"format":"hap","mode":"debug","outputPath":"/absolute/public/project/entry/build/default/outputs/default/entry-signed.hap","profileValidated":true}
```

```json
{"schemaVersion":1,"status":"error","code":"LOGIN_REQUIRED","message":"Run devecocli auth login outside the editor, then retry. The helper never logs in."}
```

The Editor verifies the schema/status/exit code, signed-debug-HAP receipt and canonical regular artifact path inside its owned generated project. It never parses the last JSON-looking line from mixed tool output. The native executable's Unix execution permissions are restored after template extraction.

## Private state and validation

Persistent state is under verified 0700 `XDG_CONFIG_HOME/godot-openharmony-signing`, outside HOME/hmdfs. Temporary requests use permission-capable TMPDIR. Private regular files are caller-owned, single-linked and verified 0600; successful `chmod` alone is insufficient. Symlink traversal is rejected for private state and materials.

The worker copies the existing CLI login, supported KEK versions and available HDC key components into a private CLI HOME without altering originals. Inherited Node injection and DevEco Code authentication selectors are cleared. A private CLT overlay preserves the selected SDK/toolchain structure and replaces only its fixed HDC entry with a signed native proxy. That proxy restores the original host HOME and HDC service variables for the exact original HDC executable. CLI and unrelated children keep their private environment. There is no JavaScript HDC interception, installed SDK patch, shell command, broker restart or HDC reconnect.

Valid matching materials are reused. Missing, expired or incompatible materials trigger exactly one official `signature generate` attempt without confirmation, `--force` or an internal cloud retry. Project identity, application bundle and team form the cache key; actual device and permission requirements are rechecked. CLI/project/team locks and checks under those locks prevent stale cache decisions and concurrent mutation.

The selected SDK's native verifier must accept the generated profile. Bundle, device, debug policy, validity, developer certificate match and actual required ACL grants are checked. When no permission requires an ACL, optional empty/absent ACL policy fields do not invalidate an otherwise valid profile. Required ACL permissions still require explicit supported grants. Stable validation enums can appear in safe diagnostics without exposing raw profile or CLI contents.

Local cache/CLI files are backed up and restored on generation failure. Failed rollback retains the only recovery copy in private storage and reports `ROLLBACK_FAILED`. Cloud certificate replacement cannot be rolled back locally; the official CLI's team-wide cloud replacement behavior is intentional. The worker preserves the generated project's original build profile, and never writes signing settings into the source export preset.

Signing CLI children use only the selected CLT's required C++ runtime search directory when it exists; global App/broker loader configuration is unchanged. The standalone worker statically links its C++ runtime and requires no Editor App `libc++_shared.so`. Only explicit proxy variables, or the Editor's configured HTTP proxy, are forwarded to the signing operation; terminal HOME/XDG/cwd and HDC identity remain inherited. CLI 1.3.5's empty-proxy-auth issue uses an anonymous placeholder only for an unauthenticated loopback proxy configured by the Editor.

## Build and regression coverage

The native worker is built offline from the repository's existing JsonCpp and mbedTLS sources by `platform/openharmony/build-auto-signer.py`, signed with PATH `binary-sign-tool`, and packaged in game-template ZIPs by the OpenHarmony SCons bundle action. This worker is currently built for native OpenHarmony arm64 hosts; other hosts retain manual exports. Editor App projects do not embed game-template ZIPs or the signing worker.

The business regression fixture can exercise the signed native worker with local fake CLI, HDC, SDK verifier and build executables. The native Editor protocol suite uses a separate signed ELF fixture to cover safe receipts, legacy/invalid status rejection, artifact paths, executable extraction and unchanged presets. Broker fixtures cover exact argv/environment framing, stream separation, deadlines, cancellation and no retry. These fixtures perform no real cloud signing or device installation; real remote-debug acceptance is separate.
