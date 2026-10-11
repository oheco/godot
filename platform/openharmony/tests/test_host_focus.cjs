// Godot Engine contributors. SPDX-License-Identifier: MIT
// Focus-only production-helper and EntryAbility tests. No engine/HAP build,
// signing, install, download, GUI operation, or changes to other agents' tests.
// node platform/openharmony/tests/test_host_focus.cjs --compiler /path/to/typescript.js
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { execFileSync } = require('node:child_process');

const args = process.argv.slice(2);
assert.ok(args.length === 0 || (args.length === 2 && args[0] === '--compiler'),
  'Expected --compiler /path/to/typescript.js, or GODOT_ARKTS_TYPESCRIPT');
const compiler = args[1] || process.env.GODOT_ARKTS_TYPESCRIPT;
assert.ok(compiler, 'An existing SDK TypeScript compiler is required; nothing is downloaded');
const ts = require(path.resolve(compiler));
const repo = path.resolve(__dirname, '../../..');
const entry = path.join(repo, 'misc/dist/openharmony_template/entry/src/main/ets/entryability/EntryAbility.ets');
const result = ts.transpileModule(fs.readFileSync(entry, 'utf8'), {
  fileName: 'EntryAbility.ts', reportDiagnostics: true,
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
});
const errors = (result.diagnostics || []).filter(item => item.category === ts.DiagnosticCategory.Error);
assert.equal(errors.length, 0, errors.map(item => ts.flattenDiagnosticMessageText(item.messageText, '\n')).join('\n'));

// Execute the unchanged production C++ helper's static assertions with the
// native compiler. Compile-only constant evaluation creates no executable.
execFileSync(process.env.CXX || 'clang++', [
  '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsyntax-only',
  path.join(__dirname, 'test_host_focus.cpp'),
], { cwd: repo, stdio: 'inherit' });
console.log('PASS production C++ focus snapshots: 11 compile-time regression cases');

for (const scenario of ['inactive', 'active', 'query-failure']) {
  const initialFocused = scenario === 'active';
  const calls = [];
  const storage = new Map();
  const stageEvents = { SHOWN: 1, ACTIVE: 2, INACTIVE: 3, HIDDEN: 4, RESUMED: 5, PAUSED: 6 };
  const plugin = { sendWindowEvent: event => calls.push(['native', event]) };
  const dependencies = {
    '@kit.AbilityKit': { UIAbility: class {} },
    '@ohos.window': { default: { WindowStageEventType: stageEvents } },
    'libentry.so': { default: plugin },
    '../runtime/Config': {},
    '../runtime/Diagnostics': { record: () => {}, describe: String },
    '../runtime/ExternalApps': {},
    '../runtime/Launch': {},
  };
  const sandbox = {
    exports: {},
    AppStorage: {
      setOrCreate: (key, value) => storage.set(key, value),
      get: key => storage.get(key),
    },
    require: id => {
      assert.ok(Object.hasOwn(dependencies, id), `Unexpected dependency: ${id}`);
      return dependencies[id];
    },
  };
  vm.runInNewContext(result.outputText, sandbox, { filename: entry });
  const ability = new sandbox.exports.default();
  let listener;
  const mainWindow = {
    getWindowProperties: () => ({ id: 31 }),
    isFocused: () => {
      assert.equal(typeof listener, 'function', 'Register stage events before taking the initial snapshot');
      calls.push(['query']);
      if (scenario === 'query-failure') { throw new Error('window query failed'); }
      return initialFocused;
    },
  };
  const stage = {
    getMainWindowSync: () => mainWindow,
    on: (name, callback) => { assert.equal(name, 'windowStageEvent'); listener = callback; },
    loadContent: (page, callback) => {
      assert.equal(page, 'pages/Index');
      assert.deepEqual(calls, [['query'], ['native', initialFocused ? 8 : 9]],
        'Publish the initial actual-focus snapshot before surface content loads');
      calls.push(['load']);
      callback({ code: 0 });
    },
  };
  ability.onWindowStageCreate(stage);
  assert.equal(storage.get('godotWindowId'), 31);
  calls.length = 0;
  for (const event of [stageEvents.SHOWN, stageEvents.ACTIVE, stageEvents.INACTIVE, stageEvents.HIDDEN,
    stageEvents.RESUMED, stageEvents.PAUSED, 999]) {
    listener(event);
  }
  assert.deepEqual(calls, [['native', 1], ['native', 8], ['native', 9], ['native', 4], ['native', 5], ['native', 6]],
    'Stage activation must not collide with surface codes 2/3; unknown stage events are ignored');
  calls.length = 0;
  ability.onForeground();
  ability.onBackground();
  assert.deepEqual(calls, [['native', 5], ['native', 6]], 'Keep lifecycle wire codes unchanged');
}
console.log('PASS EntryAbility: UI initial snapshot and failed query before loadContent; stage 1/8/9/4; preserved lifecycle 5/6');
console.log('Focus tests use production C++ constant evaluation and mocked ArkUI/NAPI; runtime IME/device acceptance remains separate');
