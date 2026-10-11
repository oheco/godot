# Official Hvigor wrapper build runner (schemaVersion 2)

The generated project's [build.cjs](build.cjs) runs the caller-selected official
command-line-tools bundle. It supports the stock Stage project with one
`entry@default` module and the `default` product. [toolchain-config.cjs](toolchain-config.cjs)
checks the installed toolchain and prepared wrapper cache; [sdk-config.cjs](sdk-config.cjs)
retains the SDK metadata selection shared with the generator.

The runner never initializes or downloads a wrapper cache, installs packages, or
patches Hvigor/plugin/SDK files. Its outer process launch uses an argument array
and `shell: false`. Official Hvigor and plugin descendants may invoke shells,
native tools, Java, or other programs internally. This is an offline-input contract
for a trusted generated project, not an operating-system network or process sandbox.
C# source debugging, publishing, and device installation are outside this interface.

## Caller and exact request

Create a unique, caller-owned `workDir` with effective mode 0700 on the application's
permission-capable private temporary/cache filesystem, outside the project. Create
a plain JSON request file with effective mode 0600, one hard link, and size at most
1 MiB; its direct parent must also be caller-owned 0700. HOME/hmdfs does not provide
these permissions and is rejected. The runner checks effective modes and never
relies on chmod succeeding. The caller removes its request/work directory afterward.

Call using an argument array:

```text
<selected-node> <absolute-project>/tools/build.cjs --request <absolute-private-json>
```

The caller must clear bootstrap `NODE_OPTIONS`/`NODE_PATH` before launching Node:
the runner can clear them for its children only after Node has already started.
The request has exactly these seven required keys, with optional `hvigorUserHome`
and `signing`. Unknown, missing, obsolete `hvigorEntry`, and mistyped fields fail.

```json
{
  "schemaVersion": 2,
  "projectDir": "/absolute/generated-project",
  "sdkRoot": "/absolute/selected-sdk-home",
  "commandLineToolsRoot": "/absolute/command-line-tools/26.0.0.201",
  "buildMode": "debug",
  "format": "hap",
  "workDir": "/absolute/private-cache/export-unique-id"
}
```

`schemaVersion` must be numeric 2; `buildMode` is `debug` or `release`, and `format`
is `hap` or `app`. Paths must be absolute and contain no NUL. Project, toolchain,
and work directories cannot traverse symlinks. SDK aliases are accepted and
normalized by the existing SDK resolver. SDK component API/version identities and
profile `runtimeOS` must agree; the runner does not change the SDK family. API26+
profile version fields retain full strings such as `"26.0.0"`. HarmonyOS requires
the complete `default` SDK bundle; OpenHarmony uses a versioned standalone SDK home.

## Official toolchain and persistent wrapper cache

From `commandLineToolsRoot`, derive exactly:

```text
selectedNode = <root>/node/bin/node
wrapperJS   = <root>/hvigor/bin/hvigorw.js
launcher    = <root>/hvigor/bin/hvigorw
```

The shell launcher must exist beside the JS wrapper, but the runner executes the
JS wrapper directly. Installed manifests must identify `@ohos/hvigor` and
`@ohos/hvigor-ohos-plugin`, have identical stable versions, use supported major 7,
and be at least 7.26.8. The plugin's engine peer dependency must match. A captured
bundled-Node probe requires Node 24.14.1 or newer. This does not authenticate an
arbitrary caller-supplied installation: the caller must select a trusted bundle.
No adapter metadata or claim that upstream interfaces are unmodified is used.

`HVIGOR_USER_HOME` remains persistent. Selection order is explicit request
`hvigorUserHome`, inherited `HVIGOR_USER_HOME`, then
`dirname(OHECO_ROOT)/.hvigor`. Native `OHECO_ROOT` defaults to
`/storage/Users/currentUser/.oheco`, so its fallback is
`/storage/Users/currentUser/.hvigor`. This is separate from private HOME/TMP/workDir.
The persistent home must already contain the prepared wrapper tools.

The runner reads the actual `PNPM_VERSION` assignment in the selected wrapper.
The inspected 26.0.0.201 bundle assigns `e.PNPM_VERSION = "10.28.2"`; it contains
Hvigor/plugin 7.26.8 and Node 24.18.1. It requires both:

```text
<H>/wrapper/tools/<PNPM_VERSION>/node_modules/.bin/pnpm
require.resolve('pnpm', { paths: [<H>/wrapper/tools/<PNPM_VERSION>] })
```

The resolved package must live inside that tools directory and its manifest must
identify `pnpm` at the assigned version. An ancestor/global package is insufficient.
Missing launcher/package, version mismatch, or missing/conflicting assignment fails
before the wrapper starts. The runner does not run pnpm or prepare its cache.

Hvigor configuration dependencies and devDependencies must be empty. With empty
dependencies the official wrapper links its bundled engine/plugin into its workspace.
The selected persistent home's optional Hvigor config must also satisfy the restricted
static config contract. An existing selected-project workspace manifest with nonempty
dependencies is rejected: the major-7 wrapper would otherwise enter its installer.
The workspace path is its SHA256 absolute-project-path hash (first 32 hex characters)
under `H/project_caches/<hash>/workspace`; its cache is not modified by preflight.

Root/entry oh-package dependencies may only be already present `file:` inputs.
Dynamic dependencies, package scripts, custom Hvigor parameters/properties/cache
paths, and executable custom tasks/plugins are rejected. The root/entry Hvigor task
files must contain the stock static `appTasks`/`hapTasks` exports with `plugins: []`.
Whitespace/comments are accepted; the runner never evaluates project files to validate
them. Supported Hvigor config keys are `modelVersion`, empty dependency maps, and
boolean `execution.daemon`, `execution.incremental`, `execution.parallel`.

## Encrypted signing fields and diagnostic redaction

Omitting `signing` forces an unsigned build; original profile signing is not reused.
When present, signing has exactly these seven nonempty-string fields:

```json
{
  "certificate": "/absolute/signing/application.cer",
  "profile": "/absolute/signing/application.p7b",
  "storeFile": "/absolute/signing/application.p12",
  "keyAlias": "application",
  "signAlg": "SHA256withECDSA",
  "keyPasswordEncrypted": "<official-encrypted-hex>",
  "storePasswordEncrypted": "<official-encrypted-hex>"
}
```

Material files must exist as regular files. Companion directories
`dirname(storeFile)/material/{fd,ac,ce}` must exist and be nonempty. Password fields
must be even-length hexadecimal official frames: four-byte big-endian content length,
12-byte IV, encrypted content, and a 16-byte authentication tag. The content length
includes the tag and must match the frame. Case and every original hex byte are
forwarded unchanged as `material.keyPassword`/`material.storePassword` in the
transactional profile. `certificate` maps to `material.certpath`; signing `type`
uses the selected SDK `runtimeOS`. Plain passwords and old `env:` references are
not accepted by this protocol.

Official signing may decrypt passwords and put plaintext into its own descendant
signer argv and diagnostics. Before wrapper launch, the runner privately invokes
only the public `DecipherUtil.decryptPwd(dirname(storeFile), encrypted, profilePath)`
API exported by the installed plugin at:

```text
<root>/hvigor/hvigor-ohos-plugin/src/utils/decipher-util.js
```

Its declaration documents this method and frame. The runner does not use private
crypto methods, derive keys, or implement official decryption. Before the wrapper
links its workspace, the probe resolves only the plugin's `@ohos/hvigor` peer to the
selected bundled engine with Node's public synchronous module hook. It changes no
SDK files or module trees. The isolated selected Node probe receives credentials on stdin, captures/discards both stdout and stderr,
and returns plaintext through a dedicated private pipe solely to add it to in-memory
stream redaction. A helper printing, throwing, exiting early, producing invalid output,
exceeding 1 MiB output, or taking over 15 seconds fails privately before profile mutation
and wrapper launch. Diagnostics are generic and never replay captured credentials.

Both build output streams redact the full plaintext and encrypted passwords,
material paths, and alias across stream chunks and UTF-8 byte boundaries. The runner
never passes plaintext passwords through wrapper argv, environment, or its generated
profile. Official signer descendants may use plaintext argv internally; this runner
does not claim otherwise. Redaction covers exact values, not arbitrary encodings or
transformations by third-party programs. The caller supplies all signing materials;
there is no personal-profile discovery or signing-material generation.

## Commands and child environment

The actual outer launch is:

```js
spawn(selectedNode, ['--', wrapperJS, ...args], { shell: false, ...options })
```

HAP arguments:

```text
--mode module -p module=entry@default -p product=default -p buildMode=<debug|release> assembleHap --no-daemon --no-incremental
```

APP arguments:

```text
--mode project -p product=default -p buildMode=<debug|release> assembleApp --no-daemon --no-incremental
```

The old 6.26.4 adapter's signed-APP `--parallel` workaround is not retained:
its necessity on official 7.26.8 has not been verified. Signed APP profile flags
remain `appWithSignedPkg: true` and `buildAppSkipSignHap: false`, and successful
artifact selection requires `-all-signed.app`. Real signed APP validation must check
the outer APP and each inner HAP; fixture success alone does not establish that gate.

Child cwd is `projectDir`. Child HOME is `workDir/home`; TMPDIR/TMP/TEMP are
`workDir/tmp`, created with verified 0700 permissions. `HVIGOR_USER_HOME` is the
selected persistent home. NODE_HOME and the leading PATH entry use the selected
bundled Node bin directory. The runner clears inherited NODE_OPTIONS/NODE_PATH,
NODE_OPTS, Java home, unrelated Hvigor/Node/Cangjie wrapper overrides, and obsolete
OHECO_HVIGOR secret environment fields; it sets npm offline flags as an additional
guard. The parent environment is unchanged.

HarmonyOS sets DEVECO_SDK_HOME to the normalized complete SDK home and clears
both OHOS SDK variables. OpenHarmony sets OHOS_SDK_HOME/OHOS_BASE_SDK_HOME and
retains DEVECO_SDK_HOME only when it resolves to the same SDK home. Existing
OpenHarmony `local.properties` sdk.dir must agree, including escaped-path decoding.

## Profile transaction, signals, and deterministic results

The root profile is parsed as JSON with comments/trailing commas. Only its temporary
selected product is normalized and given the requested signing configuration and
signed APP options. Its original file is renamed on the same filesystem to
`.godot-build-runner-original.json5`, then restored by rename after success, failure,
artifact mismatch, or normal SIGINT/SIGTERM interruption. Original bytes, formatting,
and mode survive. A result is written only after restoration succeeds.

The work and project directory locks are `.runner-lock` and `.godot-build-runner.lock`.
Rejected concurrent calls preserve existing locks. On POSIX the child uses its own
process group, and signals are forwarded to the wrapper and its same-group engine/tools.
A signalled build fails even if descendants handle the signal and exit 0. Independently
detached descendants are outside that group. SIGKILL/power loss cannot run restoration;
a retained original backup and project lock require recovery after all build processes
have stopped. Restoration failure preserves both and produces no success result.

Before building, only old artifacts of the requested format in its flat default output
directory are removed. The runner selects exactly one nonempty regular nonsymlink
artifact, without recursive search or first-match selection:

| Format | Directory | Required suffix |
| --- | --- | --- |
| HAP unsigned/signed | `entry/build/default/outputs/default` | `-unsigned.hap` / `-signed.hap` |
| APP unsigned/signed | `build/outputs/default` | `-unsigned.app` / `-all-signed.app` |

On exit 0, `workDir/result.json` is a private 0600 file with exactly five keys:

```json
{
  "schemaVersion": 2,
  "outputPath": "/absolute/project/entry/build/default/outputs/default/entry-default-unsigned.hap",
  "mode": "debug",
  "format": "hap",
  "signed": false
}
```

The caller checks exit status, result schema/mode/format/signed, and the nonempty
regular output file before copying it. stdout is a build log, not the JSON protocol.
An accepted invocation removes a previous result before preflight; a nonzero exit must
always be treated as failure. Artifacts remain in the generated project. `signed`
reports the requested Hvigor signing path, not independent verification or device trust.

## Validation

Run from the repository root, using only Node's built-in libraries:

```sh
node --check misc/dist/openharmony_template/tools/build.cjs
node --check misc/dist/openharmony_template/tools/toolchain-config.cjs
node --test platform/openharmony/tests/test_build_runner.cjs
```

Fixtures use synthetic SDK metadata, official-layout wrapper/engine/plugin/pnpm
manifests, a public decrypt export, opaque encrypted tokens, and fake package outputs.
They preserve the eight HAP/APP × debug/release × signed/unsigned combinations and
cover both SDK families, selection coherence, missing cache/metadata, Node minimum,
exact argv/env, byte restoration, plaintext/ciphertext failure redaction, noisy/early-exit
decrypt probes, static inputs, artifacts, privacy, locks, and descendant signals.
They perform no real SDK build, signing, installation, network access, or user-profile read.
Actual compatible material decryption, signed APP worker behavior, and full native
Editor export are separate integration gates to be verified with real supplied inputs.
