# Desktop integration candidate validation — 2026-10-07

Candidate: `4.7.2-ohos.4`, bundle `org.oheco.godoteditor`.
The first installed candidate used host ABI **2**. Editor and both game targets
have since been rebuilt with host ABI **3**, including the IME surface-position
API and popup mouse-motion bridge fix. The ABI-3 signed HAP passed CompileArkTS
and was installed successfully; it is still a dirty-source integration candidate.
Environment: native HarmonyOS aarch64, MOR-M2 / MateBook Pro S, 2in1,
`MOR-W52 7.0.0.107`; native/ArkTS SDK `26.0.0.35-Beta`, minimum API 23.
Base source: `42d64b94967b21e6982735d4ac85563987126c9c`, with the uncommitted
desktop adaptation in this working tree. This is a candidate record, not a
validated release commit.

## Completed

- Native Editor and both mono-enabled game targets rebuilt using the existing
  SCons object cache. The Editor library and CLI were signed with the native
  binary signing tool. A new-directory clean native build is still pending.
- 27 Python configuration/generation/pack-audit tests passed.
- ArkTS host logic suite passed, including loading/running states, literal
  Unicode/spaced folder URIs, browser links, and asynchronous launch/broker
  failures. These use SDK TypeScript and boundary mocks.
- Real N-API glue compiled/signed and loaded by native Node. Startup order,
  permission/type/lifetime checks, nonblocking external queue delivery from a
  worker, and background terminal Promise success/failure checks passed.
- Real headless Editor passed the filesystem dialog home/default-project-path
  checks and retained an explicitly selected directory. Early Editor exit
  reported filesystem-scan-abort and RID/ObjectDB cleanup warnings; these were
  not hidden or classified as installed GUI acceptance.
- The existing DevEco project was updated in place; generator reported preserved
  signing settings and package lock. App version is `4.7.2-ohos.4`.
- That project built in an isolated fresh copy through native CMake/Ninja,
  CompileArkTS, resources and HAP assembly, with the official Godot splash
  resource. Output is an **unsigned HAP**, not an installed application.
- `git diff --check` passed. Installed clang-format is 15, while the upstream
  style targets 18. The compatibility check passed with equivalent
  `AlignTrailingComments: false` and no unavailable semicolon-removal option.

## Pending installed acceptance

At 12:48 the user explicitly deferred the missing pasteboard ACL and clipboard
acceptance. The candidate no longer automatically declares or requests
`READ_PASTEBOARD`. Clipboard read/write acceptance is excluded from this
revision; actual clipboard contents were not read or overwritten.

Before the user's service start, the real terminal probe returned
`UNAVAILABLE [connect]: Broker connection failed` promptly. After the user
started `oheco-broker shell serve` in the system terminal, the identical native
probe returned success with an empty diagnostic. This proves the system aa
request was accepted through the broker; HiShell window/cwd and the installed
Editor's actual menu still require device acceptance.

The existing personal-profile project subsequently compiled and signed
successfully (34 Hvigor tasks, `BUILD SUCCESSFUL`) and was installed on MOR-M2.
BundleManager reports `4.7.2-ohos.4` / versionCode `4070204`, debug enabled and
no declared `READ_PASTEBOARD` permission.

Actual installed GUI evidence obtained after unlocking the device:

- Normal Project Manager reached an XComponent engine surface with no failure
  log controls. A transient logo frame has not yet been visually captured.
- The real Editor plugin reports `display=OpenHarmony`, automatic scale mode
  `0`, and both editor and screen scale `1.89999997615814` (passed).
- A real `FileDialog` with `ACCESS_FILESYSTEM` reports
  `/storage/Users/currentUser`, matching `OS.SYSTEM_DIR_DESKTOP` (passed).
- The existing saved `default_project_path` is an older private `Projects`
  directory. It was preserved rather than changing user preferences for a test.
- Actual FileSystemDock menus dispatched via their Tree/PopupMenu signal chain:
  menu 15 returned in 1 ms, menu 17 in 0 ms, and the next editor frame was reached
  in both cases. Their target was the app-private temporary project; these
  results prove responsiveness, not the external application's directory.
- Opening the public home through `OS.shell_show_in_file_manager` produced File
  Manager's "个人" view with URI nodes for `Videos`, `Images`, `Documents` and
  `Download` under `file://docs/storage/Users/currentUser` (passed for home).
- Terminal launch acceptance was logged. A signed read-only broker probe under
  HiShell's UID subsequently read actual shell cwd values successfully, while
  the earlier HDC `/proc/*/cwd` reads were inconclusive. Opening the public
  platform test directory still produced no matching shell cwd or new HiShell
  window, despite an accepted request. This is a failed terminal-directory
  acceptance case under investigation, not a successful terminal launch.

The early `-31` test failure was caused by using a project under the shell's
`/data/local/tmp`, which was inaccessible to the application. `Main::setup`
rejects an invalid `--path` with `ERR_INVALID_PARAMETER`; this error code alone
was not evidence of a missing window or resource argument. Deploying the fixture
through `hdc file send -b org.oheco.godoteditor` into its actual
`/data/storage/el2/base/haps/entry/temp` allowed the Editor probe to run. A second
attempt after a failed engine lifetime also correctly reported that a new
process was required. Use a fresh owned process for subsequent acceptance.

The additional IME and PopupMenu source fixes passed native regressions:
real system CursorInfo/TextConfig objects and coherent geometry snapshots, plus
real Input/Viewport/Button/embedded PopupMenu hover and click behavior. Mouse
screen positions and Window rectangle callbacks now share the surface origin.
The first CompileArkTS attempt caught a wrong Kit import for `window`; correcting
it to `@kit.ArkUI` allowed the actual signed HAP build to pass. This illustrates
why mocked/transpiled host tests remain separate from the real SDK compiler.

After ABI-3 installation, the user reported correct IME placement below the
Project Manager's filter input and the main Editor inputs. The first ABI-3
candidate still had an offset inside popup windows: wrapper-only geometry tests
did not cover the actual embedded LineEdit/TextEdit
coordinate submission. A signed native real-control regression reproduced the
extra popup position: a popup at (170, 130) added that placement twice, and a
2x canvas stretch doubled the error. The OpenHarmony control path now maps the
local caret through the rendering canvas and Window screen transforms once.
The regression explicitly verifies genuine two-level embedding, a nonzero native
window origin and real LineEdit/TextEdit submissions. Both the old-library
reproduction and the fixed production-control build passed their expected
assertions: all six fixed scenarios aligned within one physical pixel. It does
not attach a system IME. The rebuilt Editor/debug/release targets and the tested
signed Editor library were assembled into a new signed HAP and installed. The
user then confirmed that the popup's Chinese candidate panel was aligned. This
is user-reported installed acceptance, separate from the captured native test.
Device input injection remains paused during the user's own acceptance.

The user also confirmed that the rebuilt candidate's main menus, context menus
and submenus all respond to pointer hover and switch their highlighted rows.
This is user-reported installed acceptance in addition to the native regression.

The user confirmed that the central Godot logo appears during startup and the
old log buttons are absent. The official image's packaged hash was checked
separately; transient startup appearance is user-reported acceptance.

The user subsequently deferred terminal and external-editor integration, asking
for an explicit unsupported-feature notification when these entries are clicked.
The current change disables their launch paths and preserves File Manager,
browser links and the internal script editor. Terminal cwd acceptance is no
longer a delivery gate. The updated notification candidate was installed, and
the user reported that the displayed English message matched their expectation.
The strings follow the Editor language, with Simplified/Traditional Chinese
translations available. Native, managed and ArkTS logic checks have passed.

Still required: final clean source build and release checks; immutable release
and software-catalog/index install validation. No tag, release, catalog deployment
or formal-index installation was performed.

## Recorded artifacts

Cache root: `/data/storage/el2/base/haps/entry/cache/godot-editor-ohos4-desktop`.
The following are validation artifacts from a dirty source tree:

| Artifact | SHA-256 |
|---|---|
| `cli-abi3-final-desktop/godot` (signed) | `893b5e3f8c095bb9213d3401daed874b7926abeb5a22e27ced80799bca19373c` |
| `cli-abi3-final-desktop/libgodot.so` (signed) | `fe75e84e975d398a7957c894e780a93652872e6f18f3404321efbc003b1c72fe` |
| Installed `entry-default-signed.hap` | `f5ad5e34076ff0f49a20cd060865fab691b1acea4c362d42ced0540bfd924ed5` |

Latest candidate evidence: `desktop-notify-retry-candidate-build.json`,
`desktop-notify-retry-{editor,template_debug,template_release,controls,hap}.log`,
`cli-abi3-final-desktop/build-info.json`, `desktop-notify-retry-hap-audit.json`,
and `desktop-notify-retry-install.log` below the cache root. The HAP audit
confirms its packaged Editor library matches the signed library hash and that
`READ_PASTEBOARD` is absent. These installed candidate artifacts precede the
final clean-source release, which must record its own committed provenance.
