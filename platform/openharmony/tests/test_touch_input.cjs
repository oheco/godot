// Godot Engine contributors. SPDX-License-Identifier: MIT
// Host-model regression tests, NOT HAP/device or native NAPI acceptance tests.
// Transpile the real TouchInput and Index methods with an explicitly supplied
// SDK compiler. Only ArkUI build/decorators are removed; OS/NAPI APIs are mocks.
// No network access or dependencies are downloaded. Transpilation artifacts live
// under TMPDIR and are removed even when an assertion fails.
//
// node platform/openharmony/tests/test_touch_input.cjs --compiler /path/to/typescript.js
// Or set GODOT_ARKTS_TYPESCRIPT. TMPDIR must name an existing temporary directory.
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { types: nativeTypes } = require('node:util');

function compilerPath() {
  const args = process.argv.slice(2);
  if (args.includes('--help') || args.includes('-h')) {
    console.log('Usage: node test_touch_input.cjs [--compiler /path/to/typescript.js]\n' +
      'Compiler fallback: GODOT_ARKTS_TYPESCRIPT. Temporary files: TMPDIR. No network access.');
    process.exit(0);
  }
  if (args.length !== 0 && (args.length !== 2 || args[0] !== '--compiler')) {
    throw new Error('Expected --compiler /path/to/typescript.js (or --help)');
  }
  const compiler = args[1] || process.env.GODOT_ARKTS_TYPESCRIPT;
  if (!compiler) { throw new Error('Supply --compiler or GODOT_ARKTS_TYPESCRIPT; no compiler is downloaded automatically'); }
  return path.resolve(compiler);
}

const ts = require(compilerPath());
const repo = path.resolve(__dirname, '../../..');
const sourceRoot = path.join(repo, 'misc/dist/openharmony_template/entry/src/main/ets');
const plain = (value) => JSON.parse(JSON.stringify(value));
const FLOAT32_MAX = 3.4028234663852886e38;
// SDK ets/component/common.d.ts SourceType; ets/component/enums.d.ts
// TouchType, MouseButton, MouseAction. These are SDK values, not Godot buttons.
const SourceType = Object.freeze({ Unknown: 0, Mouse: 1, TouchScreen: 2, KEY: 4, JOYSTICK: 5 });
const TouchType = Object.freeze({ Down: 0, Up: 1, Move: 2, Cancel: 3,
  HOVER_ENTER: 9, HOVER_MOVE: 10, HOVER_EXIT: 11, HOVER_CANCEL: 12 });
const MouseButton = Object.freeze({ Left: 0, Right: 1, Middle: 2, Back: 3, Forward: 4, None: 5 });
const MouseAction = Object.freeze({ Press: 0, Release: 1, Move: 2, Hover: 3 });
const point = (id, x = 10, y = 20) => ({ id, x, y });
const identity = (value) => value;
const pixels = (value) => value * 2.5;
function event(type, changedTouches = [], source = SourceType.TouchScreen, deviceId = undefined) {
  return { type, source, deviceId, changedTouches, stopPropagation: () => assert.fail('Touch routing must not stop propagation') };
}
function dto(type, id, x = 10, y = 20) { return { type, id, x, y }; }

// Model the native touch contract strictly enough to catch the original routing
// failure even if a future native implementation defensively drops bad indices.
function expectNativeTouches(touches) {
  assert.ok(Array.isArray(touches) && !nativeTypes.isProxy(touches), 'Native requires a plain Array');
  assert.equal(Object.getPrototypeOf(touches).constructor.name, 'Array');
  for (const touch of touches) {
    assert.ok(touch !== null && typeof touch === 'object' && !nativeTypes.isProxy(touch), 'DTO must be a plain Object');
    const prototype = Object.getPrototypeOf(touch);
    assert.equal(prototype.constructor.name, 'Object');
    assert.equal(Object.getPrototypeOf(prototype), null);
    assert.deepEqual(Object.keys(touch).sort(), ['id', 'type', 'x', 'y']);
    assert.ok(Number.isInteger(touch.type) && touch.type >= 0 && touch.type <= 3, 'Native touch type range');
    assert.ok(Number.isInteger(touch.id) && touch.id >= 0 && touch.id < 32, 'Native touch id range');
    for (const value of [touch.x, touch.y]) {
      assert.ok(Number.isFinite(value) && Math.abs(value) <= FLOAT32_MAX, 'Native float32 coordinate range');
    }
  }
}
function expectTouches(actual, expected) {
  expectNativeTouches(actual);
  assert.deepEqual(plain(actual), expected);
}

function load(file, dependencies, globals, temporary, transform = (source) => source) {
  const source = transform(fs.readFileSync(path.join(sourceRoot, file), 'utf8'));
  const result = ts.transpileModule(source, {
    fileName: file.replace(/\.ets$/, '.ts'), reportDiagnostics: true,
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
  });
  const errors = (result.diagnostics || []).filter((item) => item.category === ts.DiagnosticCategory.Error);
  assert.equal(errors.length, 0, errors.map((item) => ts.flattenDiagnosticMessageText(item.messageText, '\n')).join('\n'));
  const artifact = path.join(temporary, file.replaceAll('/', '_').replace(/\.ets$/, '.js'));
  fs.writeFileSync(artifact, result.outputText);
  const sandbox = {
    exports: {}, console, SourceType, TouchType, MouseAction, MouseButton,
    require: (id) => {
      assert.ok(Object.hasOwn(dependencies, id), `Unexpected runtime dependency ${id} in ${file}`);
      return dependencies[id];
    },
    ...globals,
  };
  vm.runInNewContext(fs.readFileSync(artifact, 'utf8'), sandbox, { filename: file });
  return sandbox.exports;
}

function testSourceAndTypeFiltering(TouchInput) {
  const input = new TouchInput();
  const unexpectedConversion = () => assert.fail('Filtered input must not convert coordinates');
  for (const source of [SourceType.Mouse, SourceType.Unknown, SourceType.KEY, SourceType.JOYSTICK, 3, 99, undefined, null, '2']) {
    for (const type of Object.values(TouchType)) {
      const ignored = event(type, [point(-100000)], source);
      // Explicit undefined must not use event()'s default TouchScreen source.
      ignored.source = source;
      expectTouches(input.convert(ignored, unexpectedConversion), []);
    }
  }
  for (const type of [TouchType.HOVER_ENTER, TouchType.HOVER_MOVE, TouchType.HOVER_EXIT, TouchType.HOVER_CANCEL,
    -1, 4, 8, 13, 255, 0.5, NaN, Infinity, undefined, null, '0']) {
    expectTouches(input.convert(event(type, [point(31)]), unexpectedConversion), []);
  }
  expectTouches(input.cancelAll(), []);
  expectTouches(input.convert(event(TouchType.Down, [point(100)]), pixels), [dto(0, 0, 25, 50)]);
  expectTouches(input.convert(event(TouchType.Cancel, [], SourceType.Mouse), unexpectedConversion), []);
  expectTouches(input.convert(event(TouchType.HOVER_CANCEL), unexpectedConversion), []);
  expectTouches(input.cancelAll(), [dto(3, 0, 25, 50)]);
  console.log('PASS filtering: only TouchScreen Down/Move/Up/Cancel; mouse/unknown/key/joystick/hover ignored without side effects');
}

function testOpaqueIdsAndLifecycle(TouchInput) {
  const input = new TouchInput();
  const ids = [-77, 2 ** 40, Number.MAX_SAFE_INTEGER, Number.MIN_SAFE_INTEGER, 0, 31, 32];
  expectTouches(input.convert(event(0, ids.map((id, i) => point(id, i + 0.25, -i - 0.5))), pixels),
    ids.map((id, i) => dto(0, i, (i + 0.25) * 2.5, (-i - 0.5) * 2.5)));
  expectTouches(input.convert(event(2, [point(ids[2], 3, 5), point(ids[0], -1, -2)]), pixels),
    [dto(2, 2, 7.5, 12.5), dto(2, 0, -2.5, -5)]);
  expectTouches(input.convert(event(0, [point(ids[0], 999, 999), point(ids[0])]), identity), []);
  expectTouches(input.convert(event(1, [point(ids[1], 7, 8)]), pixels), [dto(1, 1, 17.5, 20)]);
  expectTouches(input.convert(event(0, [point(-999999, 2, 4)]), pixels), [dto(0, 1, 5, 10)]);
  expectTouches(input.convert(event(3, [point(ids[2], -3, -4)]), pixels), [dto(3, 2, -7.5, -10)]);
  expectTouches(input.convert(event(0, [point(9876543210, 6, 8)]), pixels), [dto(0, 2, 15, 20)]);
  const expected = [dto(3, 0, -2.5, -5), ...[3, 4, 5, 6].map((i) => dto(3, i, (i + 0.25) * 2.5, (-i - 0.5) * 2.5)),
    dto(3, 1, 5, 10), dto(3, 2, 15, 20)];
  expectTouches(input.convert(event(3), () => assert.fail('Empty Cancel uses last pixel coordinates')), expected);
  expectTouches(input.cancelAll(), []);
  expectTouches(input.convert(event(0, [point(-1)]), identity), [dto(0, 0)]);
  expectTouches(input.cancelAll(), [dto(3, 0)]);
  console.log('PASS opaque IDs: sparse/negative/safe-integer limits; stable reordered Move/Up/Cancel; released slots reused; last-coordinate cancellation');
}

function testDeviceIsolation(TouchInput) {
  const input = new TouchInput();
  const deviceEvent = (deviceId, type, points = []) => event(type, points, SourceType.TouchScreen, deviceId);
  expectTouches(input.convert(deviceEvent(1, 0, [point(7)]), identity), [dto(0, 0)]);
  expectTouches(input.convert(deviceEvent(2, 0, [point(7, 30, 40)]), identity), [dto(0, 1, 30, 40)]);
  expectTouches(input.convert(event(0, [point(7, 50, 60)]), identity), [dto(0, 2, 50, 60)]);
  expectTouches(input.convert(deviceEvent(1, 0, [point(7)]), identity), []);
  expectTouches(input.convert(deviceEvent(2, 2, [point(7, 3, 4)]), pixels), [dto(2, 1, 7.5, 10)]);
  expectTouches(input.convert(deviceEvent(1, 2, [point(7, 5, 6)]), pixels), [dto(2, 0, 12.5, 15)]);
  expectTouches(input.convert(deviceEvent(3, 1, [point(7)]), identity), []);
  expectTouches(input.convert(deviceEvent(1, 1, [point(7, 8, 9)]), identity), [dto(1, 0, 8, 9)]);
  expectTouches(input.convert(deviceEvent(2, 2, [point(7, 5, 6)]), identity), [dto(2, 1, 5, 6)]);
  expectTouches(input.convert(deviceEvent(1, 0, [point(7, 1, 2)]), identity), [dto(0, 0, 1, 2)]);
  expectTouches(input.convert(deviceEvent(1, 3), identity), [dto(3, 0, 1, 2)]);
  expectTouches(input.convert(deviceEvent(2, 2, [point(7, 7, 8)]), identity), [dto(2, 1, 7, 8)]);
  expectTouches(input.convert(event(3), identity), [dto(3, 2, 50, 60)]);
  expectTouches(input.convert(deviceEvent(2, 1, [point(7)]), identity), [dto(1, 1)]);
  expectTouches(input.cancelAll(), []);

  // A missing deviceId is the SDK default zero, not a separate namespace.
  expectTouches(input.convert(deviceEvent(0, 0, [point(88)]), identity), [dto(0, 0)]);
  expectTouches(input.convert(event(2, [point(88, 2, 3)]), identity), [dto(2, 0, 2, 3)]);
  expectTouches(input.convert(deviceEvent(null, 3), identity), [dto(3, 0, 2, 3)]);
  for (const deviceId of [NaN, Infinity, -Infinity, 1.5, Number.MAX_SAFE_INTEGER + 1, '1', 1n]) {
    for (const type of [0, 1, 2, 3]) {
      expectTouches(input.convert(deviceEvent(deviceId, type, type === 3 ? [] : [point(7)]),
        () => assert.fail('Invalid device ID must not convert')), []);
    }
  }
  expectTouches(input.cancelAll(), []);

  // The capacity is global, not 32 per input device. Device cancellation frees
  // only its slots; another device's equal raw IDs remain mapped and active.
  const points = Array.from({ length: 20 }, (_, id) => point(id));
  expectTouches(input.convert(deviceEvent(1, 0, points), identity), points.map((touch, i) => dto(0, i)));
  expectTouches(input.convert(deviceEvent(2, 0, points), identity), points.slice(0, 12).map((touch, i) => dto(0, i + 20)));
  expectTouches(input.convert(deviceEvent(1, 3), identity), points.map((touch, i) => dto(3, i)));
  expectTouches(input.convert(deviceEvent(2, 2, points), identity), points.slice(0, 12).map((touch, i) => dto(2, i + 20)));
  expectTouches(input.convert(deviceEvent(3, 0, points), identity), points.map((touch, i) => dto(0, i)));
  const cancelled = input.cancelAll();
  expectNativeTouches(cancelled);
  assert.equal(cancelled.length, 32);
  assert.equal(new Set(cancelled.map((touch) => touch.id)).size, 32);
  assert.ok(cancelled.every((touch) => touch.type === 3));
  console.log('PASS device identity: equal raw IDs isolated by device; default device zero; device-scoped empty Cancel; safe device IDs; global 32-slot cap');
}

function testUnknownAndCapacity(TouchInput) {
  const input = new TouchInput();
  for (const type of [1, 2, 3]) {
    expectTouches(input.convert(event(type, [point(987654)]), () => assert.fail('Unknown ID must not allocate or convert')), []);
  }
  expectTouches(input.cancelAll(), []);
  expectTouches(input.convert(event(0, [point(123), point(123)]), identity), [dto(0, 0)]);
  expectTouches(input.convert(event(2, [point(999), point(123, 3, 4)]), identity), [dto(2, 0, 3, 4)]);
  expectTouches(input.cancelAll(), [dto(3, 0, 3, 4)]);

  const ids = Array.from({ length: 40 }, (_, i) => -(2 ** 40) - i * 999);
  expectTouches(input.convert(event(0, ids.map((id, i) => point(id, i, -i))), identity),
    ids.slice(0, 32).map((id, i) => dto(0, i, i, i === 0 ? 0 : -i)));
  for (const type of [1, 2, 3]) {
    expectTouches(input.convert(event(type, ids.slice(32).map((id) => point(id))), identity), []);
  }
  expectTouches(input.convert(event(2, ids.slice(0, 32).reverse().map((id) => point(id, 7, 9))), identity),
    ids.slice(0, 32).map((id, i) => dto(2, i, 7, 9)).reverse());
  expectTouches(input.convert(event(1, [point(ids[17])]), identity), [dto(1, 17)]);
  expectTouches(input.convert(event(2, [point(ids[32])]), identity), []);
  expectTouches(input.convert(event(1, [point(ids[32])]), identity), []);
  expectTouches(input.convert(event(0, [point(ids[32], -3, 4), point(ids[33])]), identity), [dto(0, 17, -3, 4)]);
  const cancelled = input.cancelAll();
  expectNativeTouches(cancelled);
  assert.equal(cancelled.length, 32);
  assert.equal(new Set(cancelled.map((touch) => touch.id)).size, 32, 'Overflow must never alias active slots');
  assert.ok(cancelled.every((touch) => touch.type === 3));
  expectTouches(cancelled.filter((touch) => touch.id === 17), [dto(3, 17, -3, 4)]);
  expectTouches(input.cancelAll(), []);
  expectTouches(input.convert(event(0, ids.slice(0, 32).map((id) => point(id))), identity),
    ids.slice(0, 32).map((id, i) => dto(0, i)));
  input.cancelAll();
  console.log('PASS capacity: 32 unique slots; extra Down discarded without alias; unknown/overflow Move/Up/Cancel never create ghosts');
}

function testInvalidPoints(TouchInput) {
  const input = new TouchInput();
  const invalidIds = [NaN, Infinity, -Infinity, Number.MAX_SAFE_INTEGER + 1, Number.MIN_SAFE_INTEGER - 1,
    1.5, -0.5, undefined, null, '1', 1n];
  for (const id of invalidIds) {
    expectTouches(input.convert(event(0, [point(id)]), () => assert.fail('Invalid ID must not convert')), []);
  }
  const invalidValues = [NaN, Infinity, -Infinity, undefined, null, '10'];
  for (const value of invalidValues) {
    for (const axis of ['x', 'y']) {
      const invalid = point(2);
      invalid[axis] = value;
      expectTouches(input.convert(event(0, [invalid]), () => assert.fail('Invalid source coordinate must not convert')), []);
    }
  }
  for (const value of [...invalidValues, Number.MAX_VALUE, -Number.MAX_VALUE, FLOAT32_MAX * (1 + Number.EPSILON),
    -FLOAT32_MAX * (1 + Number.EPSILON)]) {
    for (const badAxis of [10, 20]) {
      expectTouches(input.convert(event(0, [point(2)]), (coordinate) => coordinate === badAxis ? value : coordinate), []);
    }
  }
  expectTouches(input.cancelAll(), []);
  // Invalid points are skipped, not allowed to suppress valid neighbors or use a slot.
  expectTouches(input.convert(event(0, [point(NaN), point(-3, Infinity), point(999, 2, 3), point(1.25), point(-55, 4, 5)]), pixels),
    [dto(0, 0, 5, 7.5), dto(0, 1, 10, 12.5)]);
  for (const type of [0, 2]) {
    expectTouches(input.convert(event(type, [point(999, NaN, 4)]), identity), []);
    expectTouches(input.convert(event(type, [point(-55, 1, 2)]), () => Infinity), []);
  }
  expectTouches(input.cancelAll(), [dto(3, 0, 5, 7.5), dto(3, 1, 10, 12.5)]);
  // Terminal events must still release known contacts when their position is
  // unusable. Send only the last valid position, never invalid SDK coordinates.
  for (const type of [1, 3]) {
    for (const invalid of [point(987, NaN, 4), point(987, 4, Infinity)]) {
      expectTouches(input.convert(event(0, [point(987, 3, 5)]), pixels), [dto(0, 0, 7.5, 12.5)]);
      expectTouches(input.convert(event(type, [invalid]), () => assert.fail('Invalid source coordinate must not convert')),
        [dto(type, 0, 7.5, 12.5)]);
      expectTouches(input.cancelAll(), []);
      expectTouches(input.convert(event(2, [point(987)]), identity), []);
    }
    for (const badConversion of [NaN, Infinity, -Infinity, FLOAT32_MAX * 2, -FLOAT32_MAX * 2]) {
      expectTouches(input.convert(event(0, [point(987, 3, 5)]), pixels), [dto(0, 0, 7.5, 12.5)]);
      expectTouches(input.convert(event(type, [point(987)]), () => badConversion), [dto(type, 0, 7.5, 12.5)]);
      expectTouches(input.cancelAll(), []);
      expectTouches(input.convert(event(2, [point(987)]), identity), []);
    }
    expectTouches(input.convert(event(type, [point(987, NaN)]), identity), []);
  }
  expectTouches(input.convert(event(0, [point(1, FLOAT32_MAX, -FLOAT32_MAX)]), identity), [dto(0, 0, FLOAT32_MAX, -FLOAT32_MAX)]);
  expectTouches(input.convert(event(2, [point(1, 0.1, -0.1)]), identity), [dto(2, 0, 0.1, -0.1)]);
  expectTouches(input.convert(event(1, [point(1, Number.MAX_VALUE, -Number.MAX_VALUE)]), (value) => value / Number.MAX_VALUE),
    [dto(1, 0, 1, -1)]);
  expectTouches(input.cancelAll(), []);
  console.log('PASS validation: unsafe/non-numeric IDs; raw/transformed NaN/Infinity/float32 overflow; mixed valid points; bad Down/Move ignored; bad-position Up/Cancel release at last valid coordinates');
}

function testPlainSnapshots(TouchInput) {
  const input = new TouchInput();
  const sourcePoint = Object.freeze(point(1000000, 3, 4));
  const observedPoint = new Proxy(new Proxy(sourcePoint, {}), {});
  const observedList = new Proxy([observedPoint], {});
  const observedEvent = new Proxy(event(0, observedList), {});
  const first = input.convert(observedEvent, pixels);
  expectTouches(first, [dto(0, 0, 7.5, 10)]);
  assert.notEqual(first, observedList);
  assert.notEqual(first[0], observedPoint);
  first[0].x = 900; first[0].id = 31; first.push(dto(0, 30));
  const moved = input.convert(event(2, [point(1000000, 5, 6)]), pixels);
  expectTouches(moved, [dto(2, 0, 12.5, 15)]);
  const cancelled = input.cancelAll();
  expectTouches(cancelled, [dto(3, 0, 12.5, 15)]);
  assert.notEqual(cancelled, moved);
  assert.notEqual(cancelled[0], moved[0]);
  moved[0].x = -999;
  expectTouches(cancelled, [dto(3, 0, 12.5, 15)]);
  const empty1 = input.cancelAll();
  const empty2 = input.cancelAll();
  expectTouches(empty1, []); expectTouches(empty2, []);
  assert.notEqual(empty1, empty2);
  const ignored1 = input.convert(event(0, [], SourceType.Mouse), identity);
  const ignored2 = input.convert(event(0, [], SourceType.Mouse), identity);
  expectTouches(ignored1, []); expectTouches(ignored2, []);
  assert.notEqual(ignored1, ignored2);
  const secondInput = new TouchInput();
  expectTouches(input.convert(event(0, [point(1)]), identity), [dto(0, 0)]);
  expectTouches(secondInput.cancelAll(), []);
  expectTouches(secondInput.convert(event(0, [point(2)]), identity), [dto(0, 0)]);
  expectTouches(input.cancelAll(), [dto(3, 0)]);
  expectTouches(secondInput.cancelAll(), [dto(3, 0)]);
  console.log('PASS DTO ownership: nested observed proxies detached; plain fresh arrays/objects; consumer mutations and independent instances cannot corrupt state');
}

function testIndexIntegration(touchModule, temporary) {
  const calls = [];
  const plugin = {
    inputTouch: (touches) => {
      expectNativeTouches(touches);
      assert.ok(touches.length > 0, 'Index must not dispatch empty touch arrays');
      calls.push(['touch', plain(touches)]);
    },
    inputMouse: (mouse) => calls.push(['mouse', plain(mouse)]),
    sendWindowEvent: (code) => calls.push(['window', code]),
    destroySurface: () => calls.push(['destroy']),
  };
  const transform = (source) => {
    const buildAt = source.indexOf('  build() {');
    assert.ok(buildAt >= 0, 'Index structure changed; update the logic-only transformation');
    const build = source.slice(buildAt);
    assert.match(build, /\.onTouch\(\(event:\s*TouchEvent\)\s*=>\s*this\.touch\(event\)\)/,
      'The real ArkUI onTouch must delegate to the tested touch method');
    assert.match(build, /\.onBlur\(\(\)\s*=>\s*this\.releaseInput\(\)\)/,
      'The real ArkUI onBlur must delegate to the tested releaseInput method');
    assert.match(build, /\.onMouse\(\(event:\s*MouseEvent\)\s*=>\s*this\.mouse\(event\)\)/);
    return (source.slice(0, buildAt) + '}\n')
      .replace('@Entry\n@Component\nstruct Index', 'export class Index').replaceAll('@State ', '');
  };
  const { Index } = load('pages/Index.ets', {
    'libentry.so': { default: plugin }, '@kit.InputKit': { KeyCode: {} }, './KeyMap': { mapKeyCode: () => 0 },
    '../runtime/TouchInput': touchModule,
    '../runtime/Config': {}, '../runtime/NativeArguments': {}, '../runtime/Permissions': {},
    '../runtime/Runtime': {}, '../runtime/Diagnostics': {}, '../runtime/Launch': {},
  }, { XComponentController: class {}, clearInterval: () => calls.push(['clearInterval']) }, temporary, transform);
  const page = new Index();
  page.getUIContext = () => ({ vp2px: pixels });

  // The matching ArkUI binary dispatches synthetic touch BEFORE onMouse.
  // Its compatibility path can expose either 1001 or 0 for a left click;
  // filtering by a guessed ID/range would miss the duplicate id=0 case.
  const mouse = (action, x, y) => ({ action, button: MouseButton.Left, x, y,
    source: SourceType.Mouse, getModifierKeyState: () => false });
  for (const rawId of [1001, 0, -100000]) {
    calls.length = 0;
    page.touch(event(TouchType.Down, [point(rawId, 2, 4)], SourceType.Mouse));
    page.mouse(mouse(MouseAction.Press, 2, 4));
    page.touch(event(TouchType.Move, [point(rawId, 3, 5)], SourceType.Mouse));
    page.mouse(mouse(MouseAction.Move, 3, 5));
    for (const type of [9, 10, 11, 12]) { page.touch(event(type, [point(rawId)], SourceType.Mouse)); }
    page.touch(event(TouchType.Up, [point(rawId, 3, 5)], SourceType.Mouse));
    page.mouse(mouse(MouseAction.Release, 3, 5));
    page.touch(event(TouchType.Cancel, [], SourceType.Mouse));
    assert.equal(calls.filter((call) => call[0] === 'touch').length, 0);
    const mouseCalls = calls.filter((call) => call[0] === 'mouse').map((call) => call[1]);
    assert.deepEqual(mouseCalls.map(({ type, button, mask, x, y }) => ({ type, button, mask, x, y })), [
      { type: 0, button: 1, mask: 1, x: 5, y: 10 },
      { type: 2, button: 1, mask: 1, x: 7.5, y: 12.5 },
      { type: 1, button: 1, mask: 0, x: 7.5, y: 12.5 },
    ]);
    assert.equal(mouseCalls.filter((call) => call.type === 0).length, 1, 'Only one mouse press');
    assert.equal(mouseCalls.filter((call) => call.type === 1).length, 1, 'Only one mouse release');
  }

  calls.length = 0;
  page.touch(event(0, [point(2 ** 40, 1.25, 2.5), point(-7, 8, 4)]));
  page.touch(event(2, [point(-7, 4, 2), point(2 ** 40, 3, 4)]));
  page.touch(event(1, [point(2 ** 40, 5, 6)]));
  page.touch(event(0, [point(Number.MAX_SAFE_INTEGER, 10, 12)]));
  page.touch(event(0, [point(-7, 999, 999)]));
  page.touch(event(2, [point(42, 99, 99)]));
  page.touch(event(0, [point(123, NaN, 0)]));
  for (const type of [9, 10, 11, 12]) { page.touch(event(type, [point(-7)])); }
  assert.deepEqual(calls, [
    ['touch', [dto(0, 0, 3.125, 6.25), dto(0, 1, 20, 10)]],
    ['touch', [dto(2, 1, 10, 5), dto(2, 0, 7.5, 10)]],
    ['touch', [dto(1, 0, 12.5, 15)]],
    ['touch', [dto(0, 0, 25, 30)]],
  ]);

  calls.length = 0;
  page.touch(event(0, [point(1, 6, 7)], SourceType.TouchScreen, 77));
  page.touch(event(0, [point(1, 8, 9)], SourceType.TouchScreen, 88));
  page.touch(event(3, [], SourceType.TouchScreen, 77));
  assert.deepEqual(calls, [
    ['touch', [dto(0, 2, 15, 17.5)]], ['touch', [dto(0, 3, 20, 22.5)]], ['touch', [dto(3, 2, 15, 17.5)]],
  ], 'An empty SDK Cancel only cancels the originating device');
  calls.length = 0;
  page.pressedKeys.add(123);
  page.metaPressed = true;
  page.mouseMask = 3;
  page.releaseInput();
  assert.deepEqual(calls, [['touch', [dto(3, 1, 10, 5), dto(3, 0, 25, 30), dto(3, 3, 20, 22.5)]], ['window', 3]],
    'Blur must cancel all devices before native focus-out');
  assert.equal(page.pressedKeys.size, 0);
  assert.equal(page.metaPressed, false);
  assert.equal(page.mouseMask, 0);
  assert.equal(page.lastClickTime, 0);
  assert.equal(page.lastClickButton, 0);
  calls.length = 0;
  page.releaseInput();
  assert.deepEqual(calls, [['window', 3]], 'Repeated blur must not send stale or empty touch arrays');

  calls.length = 0;
  page.touch(event(2, [point(-7)]));
  page.touch(event(1, [point(Number.MAX_SAFE_INTEGER)]));
  assert.deepEqual(calls, [], 'Post-blur stale Move/Up must not revive released touches');
  page.touch(event(0, [point(-444, 6, 8)]));
  assert.deepEqual(calls, [['touch', [dto(0, 0, 15, 20)]]]);
  calls.length = 0;
  page.aboutToDisappear();
  assert.equal(page.disposed, true);
  const cancelAt = calls.findIndex((call) => call[0] === 'touch');
  const destroyAt = calls.findIndex((call) => call[0] === 'destroy');
  assert.ok(cancelAt >= 0 && destroyAt > cancelAt, 'Disappear must cancel touches before destroying the surface');
  assert.deepEqual(calls[cancelAt], ['touch', [dto(3, 0, 15, 20)]]);
  calls.length = 0;
  page.aboutToDisappear();
  assert.equal(calls.filter((call) => call[0] === 'touch').length, 0, 'Disappear cancellation is idempotent');
  calls.length = 0;
  page.touch(event(0, [point(88)]));
  assert.deepEqual(calls, [], 'Disposed page must ignore late input');
  expectTouches(page.touchInput.cancelAll(), []);
  const terminatingPage = new Index();
  terminatingPage.getUIContext = () => ({ vp2px: pixels });
  terminatingPage.terminating = true;
  terminatingPage.touch(event(0, [point(99)]));
  assert.deepEqual(calls, [], 'Terminating page must ignore new input');
  expectTouches(terminatingPage.touchInput.cancelAll(), []);
  console.log('PASS real Index methods: mouse+synthetic-touch sequence routed once; physical touch pixels/slots; blur clears state and cancels before focus-out; disappear cancels before destroy; late input ignored');
}

if (!process.env.TMPDIR) { throw new Error('TMPDIR is required for transpilation artifacts'); }
const temporary = fs.mkdtempSync(path.join(process.env.TMPDIR, 'godot-touch-tests-'));
try {
  // An empty dependency map also checks that libentry.so is a type-only import.
  const touchModule = load('runtime/TouchInput.ets', {}, {}, temporary);
  testSourceAndTypeFiltering(touchModule.TouchInput);
  testOpaqueIdsAndLifecycle(touchModule.TouchInput);
  testDeviceIsolation(touchModule.TouchInput);
  testUnknownAndCapacity(touchModule.TouchInput);
  testInvalidPoints(touchModule.TouchInput);
  testPlainSnapshots(touchModule.TouchInput);
  testIndexIntegration(touchModule, temporary);
  console.log('PASS touch input source/model regression tests (not HAP/device verification)');
} finally {
  fs.rmSync(temporary, { recursive: true, force: true });
}
