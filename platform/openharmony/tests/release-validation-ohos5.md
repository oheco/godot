# 4.7.2-ohos.5 validation

This revision publishes the SDK26/official-wrapper migration and desktop input
repairs following 4.7.2-ohos.4. GitHub Release attaches the portable native CLI,
a portable DevEco Editor project, checksums and a machine-readable validation
record. The tag identifies the clean source used for release builds.

## Device acceptance of the input repair

The final development candidate was built from baseline
`d5d689f3a8fac7da45951a5ffcbe661898855ed1` plus a captured source overlay,
SHA-256 `f444e44ce8228abf0e7612863452600c770a1b6b278ad4a4e7198dd06a413a99`.
All three native targets were rebuilt on aarch64 HarmonyOS. Official Command
Line Tools 26.0.0.201, bundled Node 24.18.1 and Hvigor/plugin 7.26.8 built the
API26 2-in-1 HAP. Its signature, CRC and signed native bytes were checked before
installation and normal EntryAbility startup.

Candidate HAP SHA-256:
`44f9705eae986e812ffd18bab126c10f2818dcb3aa5aff3f8bcc3abb46c0ebee`.
Size: 429,652,515 bytes. These identify the locally signed acceptance candidate,
not the portable public project or final release archive.

The user confirmed in that installed candidate:

- Normal text input with no recurring hidden-keyboard or connection errors after
  text-field/application focus changes.
- Candidate placement follows the text field on both physical displays and after
  moving the Editor back to the primary display.

Actual SDK display-origin queries returned primary `(0,0)` and secondary
`(-314,-2160)` in desktop pixels. IME mapping removes that desktop displacement
while global window and mouse reporting keep their original coordinate frame.
Post-acceptance engine/host logs contained no matching `ERROR`/`Failed to` entry.
Earlier installed-candidate sessions confirmed arrows, shortcuts, normal touchpad
scroll speed and clipboard input; current automatic regressions preserve them.

## Automated input regression

- 27 exact-production native IME lifecycle scenarios, including 18 combinations
  of effective focus, NONE/HIDE/SHOW status and Detach results.
- 4 display lifecycle scenarios covering equal-coordinate A→B→A transitions,
  unknown metadata, attach failure, focus deferral and explicit cancellation.
- 9 exact-production cursor notification scenarios, including display identity,
  allocation failures, RPC failures/retry and success-only deduplication.
- Production wrapper geometry across primary/right/above-left displays, fractional
  offsets, invalid metadata and coherent concurrent snapshots.
- Actual ArkTS host logic with SDK mocks: display/density event registration,
  both notification orders, unchanged-rectangle display movement, current density
  conversion, listener cleanup and disposed callbacks.
- 52 touchpad combinations, physical arrow/Ctrl+X/key modifier cases and 11
  production C++ effective-focus cases.

Native fixtures run and sign on aarch64 HarmonyOS with actual SDK objects;
service calls and Window/display boundaries are mocked where stated. They are
independent of the installed commercial IME session.

## Release evidence and scope

The attached `RELEASE-VALIDATION.json` records the clean source commit, release
artifact hashes, native signing, complete applicable regressions, native CLI/.NET
and export checks, archive relocation, DevEco compilation and device startup.
Final Release assets and the official package index are verified after upload;
`SHA256SUMS` covers the exact attached archives.

The managed API remains 4.7.2-ohos.3. Verified GodotSharp tools and the fixed 141
NuGet inputs are bundled; the external SDK is dotnet-sdk 10.0.401-ohos.2 and is
not shipped. Previous native CLI C# scene/build/import/run/rebuild/hot reload
acceptance does not establish every sandboxed GUI C# flow. Public projects need
the user's own signing material and approved restricted-permission ACLs.

English internationalizable external-action resources are checked in the source
and compiled HAP. The latest input-only acceptance did not manually trigger an
external application notification. Terminal and external-editor menu launches
remain disabled. Historical desktop results retain their original scope in
[desktop-validation.md](<desktop-validation.md>).
