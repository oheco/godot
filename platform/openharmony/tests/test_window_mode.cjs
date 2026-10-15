// Godot Engine contributors. SPDX-License-Identifier: MIT
// Focused production WindowModeController tests, using the existing SDK compiler.
// SDK calls remain deferred; their actual status is set independently of requests.
// No real windows, input, engine/HAP builds, installs, or dependency downloads.
// node platform/openharmony/tests/test_window_mode.cjs --compiler /path/to/typescript.js
'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const args = process.argv.slice(2);
if (args.includes('--help') || args.includes('-h')) {
  console.log('Usage: node test_window_mode.cjs --compiler /path/to/typescript.js\n' +
    'Compiler fallback: GODOT_ARKTS_TYPESCRIPT. Uses only WindowMode.ets and mocked SDK/NAPI.');
  process.exit(0);
}
assert.ok(args.length === 0 || (args.length === 2 && args[0] === '--compiler'),
  'Expected --compiler /path/to/typescript.js, or GODOT_ARKTS_TYPESCRIPT');
const compiler = args[1] || process.env.GODOT_ARKTS_TYPESCRIPT;
assert.ok(compiler, 'An existing SDK TypeScript compiler is required; nothing is downloaded');
const ts = require(path.resolve(compiler));
const sourcePath = path.resolve(__dirname,
  '../../../misc/dist/openharmony_template/entry/src/main/ets/runtime/WindowMode.ets');
const source = fs.readFileSync(sourcePath, 'utf8');
const compiled = ts.transpileModule(source, {
  fileName: 'WindowMode.ts', reportDiagnostics: true,
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
});
const errors = (compiled.diagnostics || []).filter(item => item.category === ts.DiagnosticCategory.Error);
assert.equal(errors.length, 0,
  errors.map(item => ts.flattenDiagnosticMessageText(item.messageText, '\n')).join('\n'));

const Status = Object.freeze({ UNDEFINED: 0, FULL_SCREEN: 1, MAXIMIZE: 2, MINIMIZE: 3, FLOATING: 4, SPLIT_SCREEN: 5 });
const Presentation = Object.freeze({ FOLLOW_APP_IMMERSIVE_SETTING: 0, EXIT_IMMERSIVE: 1,
  ENTER_IMMERSIVE: 2, ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER: 3 });
// One event-loop turn drains all Promise continuations; no timing waits or polling.
const drain = () => new Promise(resolve => setImmediate(resolve));

function fixture(options = {}) {
  const statuses = [];
  const results = [];
  const calls = [];
  const operations = [];
  const offCalls = [];
  let listener;
  let listenerType;
  let active = 0;
  const sdk = {
    status: options.status ?? Status.FLOATING,
    fullScreen: options.fullScreen ?? false,
    failStatus: options.failStatus ?? false,
    failProperties: options.failProperties ?? false,
    throwNext: false,
    statusQueries: 0,
    propertyQueries: 0,
    registrations: 0,
    calls, operations, offCalls,
    actual(status, fullScreen = false) { sdk.status = status; sdk.fullScreen = fullScreen; },
    emit(status) {
      assert.equal(typeof listener, 'function', 'The controller must own a status listener');
      sdk.status = status;
      listener(status);
    },
    callback() { return listener; },
    settle(index, failure = false) {
      const operation = operations[index];
      assert.ok(operation && !operation.settled, 'Settle each explicitly selected SDK call once');
      operation.settled = true;
      active--;
      if (failure) { operation.reject(new Error('SDK transition rejected')); }
      else { operation.resolve(); }
    },
  };
  function invoke(name, values) {
    calls.push([name, ...values]);
    assert.equal(active, 0, 'Window SDK operations must never overlap');
    if (sdk.throwNext) { sdk.throwNext = false; throw new Error('SDK synchronous transition failure'); }
    active++;
    return new Promise((resolve, reject) => operations.push({ resolve, reject, settled: false }));
  }
  const mainWindow = {
    on: (name, callback) => {
      assert.ok(['windowStatusChange', 'windowStatusDidChange'].includes(name), `Unexpected window event ${name}`);
      assert.equal(typeof callback, 'function');
      sdk.registrations++;
      if (options.registrationFailure) { throw new Error('SDK listener registration failed'); }
      assert.equal(listener, undefined, 'Register one owned status listener');
      listenerType = name;
      listener = callback;
    },
    off: (name, callback) => {
      offCalls.push([name, callback]);
      assert.ok(['windowStatusChange', 'windowStatusDidChange'].includes(name));
      if (listener) {
        assert.equal(name, listenerType, 'Remove the same event that was registered');
        assert.ok(callback === undefined || callback === listener, 'Remove the registered callback');
      }
      if (options.removalFailure) { throw new Error('SDK listener removal failed'); }
      listener = undefined;
    },
    getWindowStatus: () => {
      sdk.statusQueries++;
      if (sdk.failStatus) { throw new Error('SDK status query failed'); }
      return sdk.status;
    },
    getWindowProperties: () => {
      sdk.propertyQueries++;
      if (sdk.failProperties) { throw new Error('SDK properties query failed'); }
      return { id: 31, isFullScreen: sdk.fullScreen };
    },
    recover: (...values) => invoke('recover', values),
    minimize: (...values) => invoke('minimize', values),
    restore: (...values) => invoke('restore', values),
    maximize: (...values) => invoke('maximize', values),
  };
  const plugin = {
    windowModeStatus: mode => { assert.ok(Number.isInteger(mode) && mode >= -1 && mode <= 3); statuses.push(mode); },
    windowModeResult: (id, actual) => {
      assert.ok(Number.isInteger(id) && id > 0);
      assert.ok(Number.isInteger(actual) && actual >= -1 && actual <= 3);
      results.push([id, actual]);
    },
  };
  const windowApi = { WindowStatusType: Status, MaximizePresentation: Presentation };
  const dependencies = {
    '@ohos.window': { default: windowApi, ...windowApi },
    'libentry.so': { default: plugin, ...plugin },
    './Diagnostics': { record: () => {}, describe: String },
  };
  const sandbox = {
    exports: {}, Promise, console,
    require: id => {
      assert.ok(Object.hasOwn(dependencies, id), `Unexpected WindowMode dependency ${id}`);
      return dependencies[id];
    },
  };
  vm.runInNewContext(compiled.outputText, sandbox, { filename: sourcePath });
  assert.equal(typeof sandbox.exports.WindowModeController, 'function');
  const controller = new sandbox.exports.WindowModeController(mainWindow);
  return { controller, sdk, statuses, results };
}

const cases = [];
async function test(name, body) {
  await body();
  cases.push(name);
  console.log(`PASS ${name}`);
}

async function main() {
  await test('initial fullscreen and external SDK status changes', async () => {
    const f = fixture({ status: Status.FULL_SCREEN, fullScreen: true });
    try {
      assert.equal(f.statuses.at(-1), 3, 'Publish actual fullscreen before any native request');
      assert.equal(f.sdk.registrations, 1);
      for (const [status, expected] of [[Status.MAXIMIZE, 2], [Status.MINIMIZE, 1],
        [Status.FLOATING, 0], [Status.SPLIT_SCREEN, 0], [Status.FULL_SCREEN, 3]]) {
        f.sdk.emit(status);
        assert.equal(f.statuses.at(-1), expected, `Map external SDK status ${status}`);
      }
      f.sdk.fullScreen = false;
      f.sdk.emit(Status.UNDEFINED);
      assert.equal(f.statuses.at(-1), 3, 'Unknown event preserves the last trustworthy status');
      assert.deepEqual(f.sdk.calls, [], 'External status reports must not issue transition calls');
      assert.deepEqual(f.results, []);
    } finally { f.controller.destroy(); }
  });

  await test('windowed/minimized/maximized/fullscreen SDK method and presentation mapping', async () => {
    for (const [mode, call, status, actual] of [[0, ['recover'], Status.FLOATING, 0],
      [1, ['minimize'], Status.MINIMIZE, 1], [2, ['maximize', 1], Status.MAXIMIZE, 2],
      [3, ['maximize', 3], Status.FULL_SCREEN, 3]]) {
      const f = fixture({ status: mode === 0 ? Status.FULL_SCREEN : Status.FLOATING });
      try {
        f.controller.request(11 + mode, mode);
        await drain();
        assert.deepEqual(f.sdk.calls, [call]);
        assert.deepEqual(f.results, [], 'No acknowledgement before the SDK Promise settles');
        const before = f.sdk.statusQueries;
        f.sdk.actual(status, actual === 3);
        f.sdk.settle(0);
        await drain();
        assert.deepEqual(f.results, [[11 + mode, actual]]);
        assert.ok(f.sdk.statusQueries > before, 'Query actual status after completion');
      } finally { f.controller.destroy(); }
    }
    // Exclusive mode 4 is normalized by the real native helper, tested separately;
    // the controller receives its normalized fullscreen mode 3.
  });

  await test('stale first result while a later desired mode is queued, with no concurrent SDK calls', async () => {
    const f = fixture();
    try {
      f.controller.request(101, 3);
      f.controller.request(102, 2);
      await drain();
      assert.deepEqual(f.sdk.calls, [['maximize', 3]]);
      assert.deepEqual(f.results, []);
      f.sdk.actual(Status.FULL_SCREEN, true);
      f.sdk.settle(0);
      await drain();
      assert.deepEqual(f.results, [[101, 3]], 'Report the completed request ID and queried actual');
      assert.deepEqual(f.sdk.calls, [['maximize', 3], ['maximize', 1]]);
      f.sdk.actual(Status.MAXIMIZE);
      f.sdk.settle(1);
      await drain();
      assert.deepEqual(f.results, [[101, 3], [102, 2]]);
    } finally { f.controller.destroy(); }
  });

  await test('window to fullscreen to window coalesces the last queued request', async () => {
    const f = fixture({ status: Status.FULL_SCREEN, fullScreen: true });
    try {
      f.controller.request(201, 0);
      f.controller.request(202, 3);
      f.controller.request(203, 0);
      await drain();
      assert.deepEqual(f.sdk.calls, [['recover']]);
      f.sdk.actual(Status.FLOATING);
      f.sdk.settle(0);
      await drain();
      assert.ok(f.sdk.calls.length >= 1 && f.sdk.calls.length <= 2);
      assert.ok(f.sdk.calls.every(call => call.length === 1 && call[0] === 'recover'),
        'The superseded queued fullscreen call is skipped');
      // Re-querying the already windowed result may avoid a redundant recover().
      if (f.sdk.operations.length === 2) {
        assert.deepEqual(f.results, [[201, 0]]);
        f.sdk.settle(1);
        await drain();
      }
      assert.deepEqual(f.results, [[201, 0], [203, 0]], 'The latest queued ID must receive its result');
    } finally { f.controller.destroy(); }
  });

  await test('failed transition rolls back to queried actual, including synchronous SDK failure', async () => {
    const f = fixture();
    try {
      f.controller.request(301, 3);
      await drain();
      const before = f.sdk.statusQueries;
      f.sdk.settle(0, true);
      await drain();
      assert.deepEqual(f.results, [[301, 0]], 'Failure must report actual windowed status');
      assert.ok(f.sdk.statusQueries > before);
      f.sdk.actual(Status.MAXIMIZE);
      f.sdk.throwNext = true;
      f.controller.request(302, 2);
      await drain();
      assert.deepEqual(f.results, [[301, 0], [302, 2]], 'Synchronous failure also queries actual');
      f.controller.request(303, 3);
      await drain();
      assert.deepEqual(f.sdk.calls, [['maximize', 3], ['maximize', 1], ['maximize', 3]],
        'A failure must release the serialization gate');
      f.sdk.actual(Status.MAXIMIZE);
      f.sdk.settle(1);
      await drain();
      assert.deepEqual(f.results.at(-1), [303, 2], 'SDK success does not prove the requested mode became actual');
    } finally { f.controller.destroy(); }
  });

  await test('unavailable status remains unknown independently of system-bar fullscreen properties', async () => {
    for (const status of [Status.UNDEFINED, Status.MAXIMIZE, Status.MINIMIZE]) {
      for (const fullScreen of [true, false]) {
        const f = fixture({ status, fullScreen });
        try {
          const known = f.statuses.at(-1);
          f.sdk.failStatus = true;
          f.controller.request(401, 3);
          await drain();
          f.sdk.settle(0, true);
          await drain();
          assert.deepEqual(f.results, [[401, -1]], 'Failure must retain unknown instead of deriving mode from bar visibility');
          assert.equal(f.sdk.propertyQueries, 0, 'isFullScreen does not define WindowStatusType');
          assert.equal(f.statuses.at(-1), known, 'Unknown result preserves the trustworthy native snapshot');
        } finally { f.controller.destroy(); }
      }
    }
  });

  await test('minimized windows restore before changing presentation, with foreground failures retained', async () => {
    for (const [mode, presentation] of [[0, ['recover']], [2, ['maximize', 1]], [3, ['maximize', 3]]]) {
      const f = fixture({ status: Status.MINIMIZE });
      try {
        f.controller.request(501 + mode, mode);
        await drain();
        assert.deepEqual(f.sdk.calls, [['restore']], 'Unhide before changing presentation');
        f.sdk.actual(Status.MAXIMIZE);
        f.sdk.settle(0);
        await drain();
        assert.deepEqual(f.sdk.calls, [['restore'], presentation]);
        f.sdk.actual(mode === 3 ? Status.FULL_SCREEN : mode === 2 ? Status.MAXIMIZE : Status.FLOATING);
        f.sdk.settle(1);
        await drain();
        assert.deepEqual(f.results, [[501 + mode, mode]]);
      } finally { f.controller.destroy(); }
    }
    const f = fixture({ status: Status.MINIMIZE });
    try {
      f.controller.request(510, 0);
      await drain();
      f.sdk.settle(0, true);
      await drain();
      assert.deepEqual(f.sdk.calls, [['restore']], 'Foreground restriction must not be bypassed by recover');
      assert.deepEqual(f.results, [[510, 1]], 'Failed restore retains actual minimized mode');
    } finally { f.controller.destroy(); }
    const closing = fixture({ status: Status.MINIMIZE });
    closing.controller.request(511, 3);
    closing.controller.destroy();
    closing.sdk.actual(Status.MAXIMIZE);
    closing.sdk.settle(0);
    await drain();
    assert.deepEqual(closing.sdk.calls, [['restore']], 'Destroy during restore prevents a following maximize');
    assert.deepEqual(closing.results, []);
  });

  await test('destroy removes registration and drops queued work, late status events and acknowledgements', async () => {
    const f = fixture();
    const callback = f.sdk.callback();
    f.controller.request(501, 3);
    f.controller.request(502, 1);
    await drain();
    const before = [...f.statuses];
    f.controller.destroy();
    f.controller.destroy();
    assert.equal(f.sdk.offCalls.length, 1, 'Destroy is idempotent');
    assert.equal(f.sdk.callback(), undefined, 'Destroy must remove the registered SDK listener');
    callback(Status.MINIMIZE); // An event already queued before off() may still arrive.
    f.sdk.actual(Status.FULL_SCREEN, true);
    f.sdk.settle(0);
    await drain();
    f.controller.request(503, 0);
    await drain();
    assert.deepEqual(f.sdk.calls, [['maximize', 3]], 'Destroyed controller skips queued and new SDK calls');
    assert.deepEqual(f.results, [], 'In-flight settlement after destroy must not acknowledge native');
    assert.deepEqual(f.statuses, before, 'Queued callbacks after destroy must not publish stale status');
  });

  await test('listener registration/removal SDK failures preserve controller lifecycle', async () => {
    const f = fixture({ registrationFailure: true, status: Status.MAXIMIZE });
    try {
      assert.equal(f.statuses.at(-1), 2, 'Registration failure must preserve the initial actual query');
      f.controller.request(601, 0);
      await drain();
      f.sdk.actual(Status.FLOATING);
      f.sdk.settle(0);
      await drain();
      assert.deepEqual(f.results, [[601, 0]]);
    } finally { f.controller.destroy(); }
    const g = fixture({ removalFailure: true });
    const callback = g.sdk.callback();
    const before = [...g.statuses];
    assert.doesNotThrow(() => g.controller.destroy());
    callback(Status.FULL_SCREEN);
    g.controller.request(602, 3);
    await drain();
    assert.deepEqual(g.statuses, before);
    assert.deepEqual(g.results, []);
    assert.deepEqual(g.sdk.calls, []);
  });

  console.log(JSON.stringify({ test: 'window_mode_arkts', passed: true, cases,
    compiler: path.resolve(compiler), source: sourcePath,
    source_sha256: crypto.createHash('sha256').update(source).digest('hex'),
    scope: 'production controller transpilation; deferred SDK and NAPI mocks; native exclusive normalization tested by test_window_mode.py' }));
}

main().catch(error => { console.error(error); process.exitCode = 1; });
