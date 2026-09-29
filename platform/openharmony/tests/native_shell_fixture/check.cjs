// Godot Engine contributors. SPDX-License-Identifier: MIT
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const cp = require('node:child_process');

const dir = process.env.GODOT_NATIVE_TEST_DIR;
if (!process.argv[2]) {
  // Every prerequisite arrives first and last; additional seeded permutations
  // exercise combinations without making failures nondeterministic.
  const orders = new Set();
  for (let i = 0; i < 6; ++i) {
    const a = [0, 1, 2, 3, 4, 5].filter(n => n !== i);
    a.push(i);
    orders.add(a.join(''));
    orders.add([...a].reverse().join(''));
  }
  let seed = 12345;
  for (let i = 0; i < 12; i++) {
    const a = [0, 1, 2, 3, 4, 5];
    for (let j = 5; j > 0; --j) {
      seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
      const k = seed % (j + 1);
      [a[j], a[k]] = [a[k], a[j]];
    }
    orders.add(a.join(''));
  }
  const run = mode => {
    const env = { ...process.env };
    if (mode === 'failure') env.GODOT_TEST_FAIL_START = '1';
    const result = cp.spawnSync(process.execPath, [__filename, mode], { env, encoding: 'utf8' });
    assert.equal(result.status, 0, `${mode}: ${result.stdout}${result.stderr}`);
  };
  for (const order of orders) run(order);
  for (const mode of ['sdk', 'missingSdkAssets', 'unknown', 'failure', 'earlyDestroy']) run(mode);
  console.log(`PASS: ${orders.size} startup orders + sdk/missing-assets/unknown/error/early-destroy; strict types, permissions, lifetime and release order`);
  process.exit(0);
}

const addon = require(path.join(dir, 'entry.node'));
const root = path.join(dir, `case-${process.pid}`);
const files = path.join(root, 'files');
const cache = path.join(root, 'cache');
const runtime = path.join(root, 'runtime');
const mode = process.argv[2];
// SDK-mode tests must not inspect the user's real package-manager installation.
process.env.OHECO_ROOT = path.join(root, 'no-sdk');
process.env.GODOT_OHOS_DOTNET_ROOT = path.join(root, 'no-sdk');
for (const key of ['GODOT_SHARP_ROOT', 'DOTNET_ROOT', 'DOTNET_ROOT_ARM64', 'NUGET_PACKAGES', 'GODOT_NUGET_SOURCE']) {
  delete process.env[key];
}
if (mode === 'unknown') {
  assert.throws(() => addon.configure(files, cache, '', 'bogus'), /Unknown managed/);
  assert.equal(fs.existsSync(files), false);
  addon.destroySurface();
  addon.destroySurface();
  process.exit(0);
}
if (mode === 'earlyDestroy') {
  addon.destroySurface();
  addon.destroySurface();
  assert.equal(addon.state(), 3);
  assert.throws(() => addon.setup([], [], false), /lifetime has ended/);
  process.exit(0);
}
if (mode === 'missingSdkAssets') {
  assert.throws(() => addon.configure(files, cache, runtime, 'sdk'), /GodotSharp assemblies are missing/);
  assert.equal(addon.state(), 0);
  // A rejected configuration must not count as a committed startup.
  addon.configure(files, cache, '', 'none');
  assert.equal(process.env.GODOT_SHARP_ROOT, undefined);
  addon.destroySurface();
  process.exit(0);
}
if (mode === 'sdk') {
  fs.mkdirSync(path.join(runtime, 'GodotSharp/Api/Debug'), { recursive: true });
  fs.writeFileSync(path.join(runtime, 'GodotSharp/Api/Debug/GodotSharp.dll'), 'fixture');
  fs.mkdirSync(path.join(runtime, 'Examples/CSharpSmoke'), { recursive: true });
  fs.writeFileSync(path.join(runtime, 'Examples/CSharpSmoke/project.godot'), 'fixture');
  addon.configure(files, cache, runtime, 'sdk');
  assert.equal(process.env.GODOT_SHARP_ROOT, path.join(runtime, 'GodotSharp'));
  assert.equal(process.env.DOTNET_ROOT, undefined);
  assert.ok(fs.existsSync(path.join(files, 'Projects/NuGet.Config')));
  assert.ok(fs.existsSync(path.join(files, 'Projects/CSharpSmoke-4.7.2-ohos.2/project.godot')));
  addon.destroySurface();
  process.exit(0);
}

assert.throws(() => addon.configure(files, cache, '', 7), TypeError);
assert.throws(() => addon.configure(files, cache, '', 'none', 'extra'), TypeError);
assert.throws(() => addon.configure('relative', cache, '', 'none'), /absolute sandbox/);
assert.throws(() => addon.configure(files, cache, '', 'sdk'), /absolute sandbox/);
assert.throws(() => addon.setup({ 0: '--editor', length: 1 }, [], false), /setup\.arguments: expected a native array/);
assert.throws(() => addon.setup([], { length: 0 }, false), /setup\.grantedPermissions: expected a native array/);
assert.throws(() => addon.setup(new Array(65537).fill('x'), [], false), /setup\.arguments: expected at most 65536/);
assert.throws(() => addon.inputTouch({ length: 0 }), /inputTouch\.events: expected a native array/);
assert.throws(() => addon.setup(['ok', 7], [], false), TypeError);
assert.throws(() => addon.setup([], ['bad,permission'], true), TypeError);
assert.throws(() => addon.setup(['bad\0argument'], [], false), TypeError);
assert.throws(() => addon.setup([], [], 'false'), TypeError);
assert.throws(() => addon.setWindowId(5.5), TypeError);
assert.throws(() => addon.setSurfaceId(23), TypeError);
assert.throws(() => addon.changeSurface(23n, NaN, 600), TypeError);
assert.throws(() => addon.setLauncher(7), TypeError);
assert.throws(() => addon.inputTouch([{ type: 2, id: -1, x: 0, y: 0 }]), TypeError);
assert.throws(() => addon.inputMouse({ type: 2, button: 0, mask: 0, x: NaN, y: 0 }), TypeError);
assert.throws(() => addon.sendWindowEvent(9), TypeError);
assert.throws(() => addon.spawnResult('request-id', 123), TypeError);
assert.equal(addon.state(), 0);
assert.equal(addon.processId(), process.pid);
const sequence = mode === 'failure' ? '012345' : mode;
const packagedGame = Number(sequence[0]) % 2 === 0;
const granted = packagedGame ? [] : ['ohos.permission.INTERNET'];
process.env.GODOT_TEST_EXPECT_PERMISSIONS = granted.join(',');
if (packagedGame) process.env.GODOT_TEST_PACKAGED = '1';
else delete process.env.GODOT_TEST_PACKAGED;
const actions = [
  () => addon.configure(files, cache, '', 'none'),
  () => addon.setup(['--path', 'test project'], granted, packagedGame),
  () => addon.setResourceManager({}),
  () => addon.setWindowId(5),
  () => addon.setSurfaceId(23n),
  () => addon.changeSurface(23n, 800, 600)
];
for (let i = 0; i < sequence.length; i++) {
  const run = actions[Number(sequence[i])];
  if (mode === 'failure' && i === 5) assert.throws(run, /Unable to start Godot engine thread: -7/);
  else run();
  if (i < 5) assert.equal(addon.state(), 0);
}
assert.equal(process.env.GODOT_OHOS_DATA_DIR, files);
assert.equal(process.env.GODOT_OHOS_CACHE_DIR, cache);
assert.equal(process.env.TMPDIR, path.join(cache, 'tmp'));
assert.ok(fs.existsSync(path.join(cache, 'tmp')));
assert.equal(process.env.GODOT_SHARP_ROOT, undefined);
assert.equal(process.env.DOTNET_ROOT, undefined);
assert.equal(process.env.NUGET_PACKAGES, undefined);
assert.equal(fs.existsSync(path.join(files, 'nuget')), false);
assert.equal(fs.existsSync(path.join(files, 'Projects')), false);
assert.equal(addon.state(), mode === 'failure' ? -7 : 2);
if (mode !== 'failure') {
  // The optional launcher may be registered after engine startup.
  addon.setLauncher(() => {});
  assert.throws(() => addon.changeSurface(24n, 800, 600), /mismatched surface/);
  assert.throws(() => addon.setSurfaceId(24n), /cannot replace/);
}
assert.throws(() => addon.setup([], [], false));
addon.inputTouch([]);
addon.destroySurface();
addon.destroySurface();
assert.equal(addon.state(), mode === 'failure' ? -7 : 3);
assert.throws(() => addon.configure(files, cache, '', 'none'));
assert.throws(() => addon.setSurfaceId(23n));
assert.throws(() => addon.setup([], [], false));
