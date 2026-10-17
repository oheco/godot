// Godot Engine contributors. SPDX-License-Identifier: MIT
// Host-logic regression tests, NOT HAP/device acceptance tests. They transpile
// the real ArkTS sources and replace HarmonyOS APIs/NAPI with controlled mocks.
// Index's ArkUI build() is deliberately excluded; CompileArkTS must separately
// verify decorators, component attributes (including expandSafeArea), and SDK APIs.
//
// No dependencies are downloaded. Supply an existing SDK TypeScript compiler:
//   node platform/openharmony/tests/test_arkts_host.cjs --compiler /path/to/typescript.js
// Or set GODOT_ARKTS_TYPESCRIPT. Filesystem fixtures stay in memory.
'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { types: nativeTypes } = require('node:util');

function compilerPath() {
  const args = process.argv.slice(2);
  if (args.includes('--help') || args.includes('-h')) {
    console.log('Usage: node test_arkts_host.cjs [--compiler /path/to/typescript.js]\n' +
      'Compiler fallback: GODOT_ARKTS_TYPESCRIPT. In-memory filesystem fixtures. No network access.');
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
const util = {
  TextDecoder: class { decodeWithStream(data) { return new TextDecoder().decode(data); } },
  generateRandomUUID: () => crypto.randomUUID(),
};
const diagnostics = {
  describe: (error) => String(error), record: () => {}, setStep: () => {},
  initDiagnostics: () => {}, fail: () => {}, probeEnvironment: () => {},
  crashLogTime: () => 0, readFullDiagnostics: () => '',
};

function load(file, dependencies, globals = {}, transform = (source) => source) {
  const source = transform(fs.readFileSync(path.join(sourceRoot, file), 'utf8'));
  const result = ts.transpileModule(source, {
    fileName: file.replace(/\.ets$/, '.ts'),
    reportDiagnostics: true,
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
  });
  const errors = (result.diagnostics || []).filter((item) => item.category === ts.DiagnosticCategory.Error);
  assert.equal(errors.length, 0, errors.map((item) => ts.flattenDiagnosticMessageText(item.messageText, '\n')).join('\n'));
  const sandbox = {
    exports: {}, console, TextDecoder, Date, ArrayBuffer, Uint8Array, Promise, Set, setTimeout,
    require: (id) => {
      assert.ok(Object.hasOwn(dependencies, id), `Unexpected dependency ${id} in ${file}`);
      return dependencies[id];
    },
    ...globals,
  };
  vm.runInNewContext(result.outputText, sandbox, { filename: file });
  return sandbox.exports;
}

const config = load('runtime/Config.ets', { '@kit.ArkTS': { util } });
const nativeArguments = load('runtime/NativeArguments.ets', {});
const localizedStrings = load('runtime/LocalizedStrings.ets', { './Diagnostics': diagnostics });
const packagedStrings = JSON.parse(fs.readFileSync(path.join(sourceRoot, '../resources/base/element/string.json'), 'utf8')).string;
const stringResourceIds = new Map(packagedStrings.map((entry, index) => ['app.string.' + entry.name, index + 1]));
function stringResource(name) {
  assert.ok(stringResourceIds.has(name), `Unknown resource ${name}`);
  return { id: stringResourceIds.get(name), type: 10003, params: [name], bundleName: '', moduleName: '' };
}
function stringResourceManager(overrides = {}) {
  return { getStringSync: (id, ...args) => {
    assert.equal(typeof id, 'number', 'API26 localized lookup uses a numeric resource ID');
    const entry = packagedStrings[id - 1];
    assert.ok(entry, `Unknown string resource ID ${id}`);
    let argument = 0;
    return (overrides[entry.name] ?? entry.value).replace(/%[sd]/g, () => String(args[argument++]));
  } };
}
const touchInput = load('runtime/TouchInput.ets', {}, {
  SourceType: { Unknown: 0, Mouse: 1, TouchScreen: 2 },
  TouchType: { Down: 0, Up: 1, Move: 2, Cancel: 3 },
});

// ArkUI AppStorage returns observed Proxy arrays. V8's napi_is_array accepts
// those, whereas Ark N-API checks the raw JSArray/SharedArray kinds. Model this
// measured Ark boundary explicitly, rather than accidentally testing V8 semantics.
function expectNativeArray(value) {
  assert.ok(Array.isArray(value) && !nativeTypes.isProxy(value), 'Ark native boundary requires a plain array');
}
function testNativeArguments() {
  const source = ['--path', '/a project/中文🎮', '--', '', 'user-argument'];
  const observed = new Proxy(source, {});
  const nested = new Proxy(observed, {});
  assert.ok(Array.isArray(nested));
  assert.equal(nested.join(' '), source.join(' '));
  assert.throws(() => expectNativeArray(nested), /plain array/);
  for (const input of [source, observed, nested]) {
    const snapshot = nativeArguments.nativeStringArray(input, 'setup.arguments');
    expectNativeArray(snapshot);
    assert.notEqual(snapshot, input);
    assert.deepEqual(plain(snapshot), source);
    snapshot[0] = 'changed';
    assert.equal(source[0], '--path');
  }
  expectNativeArray(nativeArguments.nativeStringArray(new Proxy([], {}), 'setup.grantedPermissions'));
  const maximum = nativeArguments.nativeStringArray(new Proxy(new Array(65536).fill('x'), {}), 'setup.arguments');
  expectNativeArray(maximum);
  assert.equal(maximum.length, 65536);
  for (const input of [null, { 0: 'x', length: 1 }, new Set(['x']), new Uint8Array(1),
    ['bad\0arg'], [1], [undefined], new Array(1), new Array(65537).fill('x')]) {
    assert.throws(() => nativeArguments.nativeStringArray(input, 'setup.arguments'));
  }
  console.log('PASS native arrays: observed/nested Proxy detachment; Unicode/order/empty values; strict rejection of array-like and invalid inputs');
}
function gameProfile() {
  return {
    schemaVersion: 1, host: { role: 'game' },
    launch: { defaultMode: 'packaged-game', defaultArguments: ['--verbose'], acceptProjectRequests: false },
    instances: { policy: 'disabled', maxCount: 1 }, managed: { mode: 'none', sdkSource: 'oheco' },
    window: { expandIntoSystemArea: true }, diagnostics: { level: 'normal' },
  };
}
function editorProfile(mode = 'none') {
  const profile = gameProfile();
  profile.host.role = 'editor';
  profile.launch = { defaultMode: 'project-manager', defaultArguments: ['--single-window'], acceptProjectRequests: true };
  profile.instances = { policy: 'editor', maxCount: 5 };
  profile.managed.mode = mode;
  return profile;
}

function testConfig() {
  const game = gameProfile();
  const editor = editorProfile();
  for (const value of [game, editor, editorProfile('sdk')]) {
    assert.deepEqual(plain(config.parseHostConfig(JSON.stringify(value))), value);
  }
  const invalid = [
    null, [], {}, { ...game, schemaVersion: 2 }, { ...game, extra: true },
    { ...game, host: { role: 'editor', extra: true } },
    { ...game, launch: { ...game.launch, defaultArguments: [3] } },
    { ...game, launch: { ...game.launch, acceptProjectRequests: 'false' } },
    { ...game, window: { expandIntoSystemArea: 1 } },
    { ...game, instances: { policy: 'disabled', maxCount: 0 } },
    { ...game, instances: { policy: 'editor', maxCount: 6 } },
    { ...game, managed: { mode: 'sdk', sdkSource: 'oheco' } },
    { ...game, managed: { mode: 'none', sdkSource: 'other' } },
    { ...game, launch: { ...game.launch, acceptProjectRequests: true } },
    { ...editor, launch: { ...editor.launch, defaultMode: 'packaged-game' } },
    { ...game, diagnostics: { level: 'debug' } }, { ...game, host: null },
    { ...game, launch: { ...game.launch, defaultArguments: ['bad\0arg'] } },
  ];
  for (const value of invalid) { assert.throws(() => config.parseHostConfig(JSON.stringify(value))); }
  assert.deepEqual(plain(config.engineArguments(game, { parameters: { godotArguments: ['--editor'] } })), ['--verbose']);
  assert.deepEqual(plain(config.engineArguments(editor, {})), ['--single-window', '--project-manager']);
  assert.deepEqual(plain(config.engineArguments(editor, { parameters: { godotProject: '/a project' } })),
    ['--single-window', '--path', '/a project', '--editor']);
  assert.deepEqual(plain(config.engineArguments(editor, { parameters: { godotArguments: ['--single-window', '--path', '/game'] } })),
    ['--single-window', '--path', '/game']);
  const noRequests = editorProfile();
  noRequests.launch.acceptProjectRequests = false;
  assert.deepEqual(plain(config.engineArguments(noRequests, { parameters: { godotArguments: ['--editor'] } })),
    ['--single-window', '--project-manager']);
  assert.deepEqual(plain(config.mergeDefaultArguments(['--rendering-driver', 'vulkan', '--single-window'],
    ['--rendering-driver', 'opengl3', '--', '--single-window'])),
    ['--single-window', '--rendering-driver', 'opengl3', '--', '--single-window']);
  assert.deepEqual(plain(config.mergeDefaultArguments(['--resolution', '800x600'], ['--resolution=1024x768'])),
    ['--resolution=1024x768']);
  assert.throws(() => config.engineArguments(editor, { parameters: { godotArguments: 'bad' } }));
  assert.throws(() => config.engineArguments(editor, { parameters: { godotProject: 42 } }));
  console.log(`PASS config: 3 valid / ${invalid.length} invalid profiles; Want/default flag-value merging and game isolation`);
}

const applicationPaths = { filesDir: '/private/application/files', cacheDir: '/private/application/cache', tempDir: '/private/application/temp' };
const modulePaths = { filesDir: '/private/module/files', cacheDir: '/private/module/cache', tempDir: '/private/module/temp' };
function hostContext() {
  const application = { ...applicationPaths, setColorMode: () => {} };
  return {
    ...modulePaths, abilityInfo: { bundleName: 'org.oheco.fixture', moduleName: 'entry', name: 'EntryAbility' },
    resourceManager: {}, getApplicationContext: () => application,
  };
}

function diagnosticFixture(plugin, events = []) {
  const logs = new Map();
  const inspected = [];
  const hostDiagnostics = load('runtime/Diagnostics.ets', {
    './Config': config, 'libentry.so': { default: plugin },
    '@kit.CoreFileKit': { fileIo: {
      OpenMode: { CREATE: 1, WRITE_ONLY: 2, APPEND: 4 },
      openSync: (file) => { events.push('log-open'); return { fd: file }; },
      closeSync: () => {}, writeSync: (fd, text) => logs.set(fd, (logs.get(fd) || '') + text),
      accessSync: (file) => { inspected.push(file); return logs.has(file); },
      statSync: (file) => ({ size: Buffer.byteLength(logs.get(file) || '') }),
      readTextSync: (file, options = {}) => {
        const text = logs.get(file) || '';
        return text.slice(options.offset || 0, options.length === undefined ? undefined : (options.offset || 0) + options.length);
      },
      listFileSync: (directory) => [...logs.keys()].filter((file) => file.startsWith(directory + '/')).map((file) => path.basename(file)),
    } },
  });
  return { hostDiagnostics, logs, inspected };
}

function testStorageAndAbility() {
  const context = hostContext();
  assert.deepEqual(plain(config.hostStorage(context, gameProfile())), applicationPaths);
  assert.deepEqual(plain(config.hostStorage(context, editorProfile())), modulePaths);
  assert.deepEqual(plain(config.hostStorage(context, editorProfile('sdk'))), modulePaths);
  assert.deepEqual(plain(config.hostStorage(context)), modulePaths);
  for (const scenario of ['game', 'editor-none', 'editor-sdk', 'invalid-config', 'missing-config']) {
    const profile = scenario === 'game' ? gameProfile() : editorProfile(scenario === 'editor-sdk' ? 'sdk' : 'none');
    const events = [];
    const plugin = { processId: () => 11, setLauncher: () => events.push('launcher'), setExternalOpener: () => events.push('external-opener') };
    const { hostDiagnostics, inspected } = diagnosticFixture(plugin, events);
    const storage = {};
    const context = hostContext();
    context.resourceManager.getRawFileContentSync = (name) => {
      assert.equal(name, 'godot_host.json');
      events.push('config-read');
      if (scenario === 'missing-config') { throw new Error('missing config'); }
      return Buffer.from(scenario === 'invalid-config' ? '{' : JSON.stringify(profile));
    };
    const { default: EntryAbility } = load('entryability/EntryAbility.ets', {
      '@kit.AbilityKit': { UIAbility: class {}, ConfigurationConstant: { ColorMode: { COLOR_MODE_NOT_SET: 0 } } },
      '@ohos.window': {}, 'libentry.so': { default: plugin }, '../runtime/Config': config,
      '../runtime/Diagnostics': hostDiagnostics,
      '../runtime/ExternalApps': { openExternal: async () => {} },
      '../runtime/WindowMode': { WindowModeController: class {} },
      '../runtime/Launch': {
        hasEngineFlag: (args, flag, short) => args.includes(flag) || args.includes(short),
        logInstanceState: () => {}, reportStarted: () => {},
      },
    }, { AppStorage: { setOrCreate: (key, value) => { storage[key] = value; } } });
    const ability = new EntryAbility();
    ability.context = context;
    ability.onCreate({}, {});
    const paths = scenario === 'game' ? applicationPaths : modulePaths;
    assert.equal(events[0], 'config-read', 'Config must be loaded before the log scope is selected');
    assert.equal(hostDiagnostics.diagnosticsPath(), `${paths.filesDir}/godot-11-diagnostics.log`, scenario);
    assert.equal(hostDiagnostics.engineLogPath(11), `${paths.filesDir}/godot-11-engine.log`, scenario);
    const error = scenario.endsWith('-config');
    assert.equal(!!storage.godotConfigError, error, scenario);
    assert.equal(events.includes('launcher'), !error && scenario !== 'game', scenario);
    const log = hostDiagnostics.readDiagnostics();
    assert.ok(log.includes(`filesDir=${paths.filesDir}`), scenario);
    assert.ok(log.includes(`cacheDir=${paths.cacheDir}`), scenario);
    assert.ok(log.includes(`tempDir=${paths.tempDir}`), scenario);
    assert.ok(log.includes(`crash log=${paths.filesDir}/godot-11-crash.log`), scenario);
    if (error) { assert.ok(log.includes('host configuration failed'), scenario); }
    // Repeated fallback initialization must not move an already selected game log.
    hostDiagnostics.initDiagnostics(context);
    assert.equal(hostDiagnostics.diagnosticsPath(), `${paths.filesDir}/godot-11-diagnostics.log`, scenario);
    hostDiagnostics.setDiagnosticsLevel('verbose');
    hostDiagnostics.probeEnvironment(context);
    assert.ok(inspected.includes(`${paths.cacheDir}/tmp`), scenario);
    assert.ok(inspected.includes(`${paths.filesDir}/Projects`), scenario);
    if (scenario === 'game') { assert.ok(!inspected.some((file) => file.startsWith(modulePaths.filesDir))); }
  }
  console.log('PASS storage/EntryAbility/Diagnostics: game application scope; editor module scope; selected log/probe roots; invalid/missing config fallback');
}

function testDirectoryRace() {
  let directory = true;
  let code = 13900015;
  const helpers = load('runtime/FileSystem.ets', { '@kit.CoreFileKit': { fileIo: {
    accessSync: () => false,
    mkdirSync: () => { throw Object.assign(new Error('concurrent create'), { code }); },
    statSync: () => ({ isDirectory: () => directory }),
  } } });
  assert.doesNotThrow(() => helpers.ensureDirectory('/mock/concurrent-directory'));
  directory = false;
  assert.throws(() => helpers.ensureDirectory('/mock/concurrent-file'), /not a directory/);
  code = 13900013;
  assert.throws(() => helpers.ensureDirectory('/mock/permission-denied'), /concurrent create/);
  console.log('PASS filesystem: EEXIST only accepts a directory; other creation errors propagate');
}

async function testExternalApps() {
  const calls = [], toasts = [], logs = [];
  let launchFailure = false;
  const context = {
    resourceManager: stringResourceManager({
      external_terminal_unsupported: '当前版本暂不支持打开系统终端。',
      external_editor_unsupported: '当前版本暂不支持使用外部编辑器打开文件。',
    }),
    startAbility: async want => { calls.push(['ability', plain(want)]); if (launchFailure) throw new Error('platform denied'); },
    openLink: async (uri, options) => calls.push(['link', uri, plain(options)]),
  };
  const external = load('runtime/ExternalApps.ets', {
    '@kit.CoreFileKit': { fileUri: { getUriFromPath: value => 'file://fixture' + encodeURI(value) } },
    '@kit.ArkUI': { window: { getLastWindow: async () => ({ getUIContext: () => ({ getPromptAction: () => ({ showToast: value => toasts.push(value) }) }) }) } },
    './LocalizedStrings': localizedStrings,
    'libentry.so': { default: { openTerminal: async uri => { calls.push(['terminal-worker', uri]); if (launchFailure) throw new Error('broker missing'); } } },
    './Diagnostics': { describe: String, record: value => logs.push(value) },
  }, { $r: stringResource });
  const directory = "/storage/Users/currentUser/a 中文 'directory'";
  const uri = 'file://fixture' + encodeURI(directory);
  await external.openExternal(context, 1, directory);
  assert.deepEqual(calls.pop(), ['ability', { bundleName: 'com.huawei.hmos.filemanager', abilityName: 'MainAbility', parameters: { fileUri: uri } }]);
  await external.openExternal(context, 2, directory);
  assert.deepEqual(calls, [], 'Unsupported terminal must not call the broker or launch an ability');
  assert.equal(toasts.pop().message, '当前版本暂不支持打开系统终端。');
  for (const target of [directory + '/script.cs', uri + '/script.cs']) {
    await external.openExternal(context, 0, target);
    assert.deepEqual(calls, [], 'Unsupported local file editor must not launch an ability');
    assert.equal(toasts.pop().message, '当前版本暂不支持使用外部编辑器打开文件。');
  }
  await external.openExternal(context, 0, 'https://godotengine.org/?x=中文');
  assert.deepEqual(calls.pop(), ['link', 'https://godotengine.org/?x=中文', { appLinkingOnly: false }]);
  launchFailure = true;
  await assert.doesNotReject(external.openExternal(context, 1, directory));
  assert.equal(toasts.length, 1);
  assert.equal(logs.filter(value => value.includes('launch failed')).length, 1);
  assert.equal(logs.some(value => value.includes('broker missing')), false);
  console.log('PASS external apps: literal directory URI, unsupported terminal/file editor notifications without launch, browser link, asynchronous File Manager failure');
}

async function testPermissions() {
  const prompts = [];
  let introspectionFailed = false;
  const tokenStates = {
    'ohos.permission.INTERNET': 0, 'ohos.permission.KEEP_BACKGROUND_RUNNING': -1,
    'ohos.permission.CAMERA': 0, 'ohos.permission.MICROPHONE': -1,
  };
  const manager = {
    requestPermissionsFromUser: async (context, names) => {
      prompts.push([...names]);
      return { authResults: names.map((name) => tokenStates[name] ?? -1) };
    },
    checkAccessTokenSync: (token, name) => tokenStates[name] ?? -1,
  };
  const ability = {
    abilityAccessCtrl: { createAtManager: () => manager, GrantStatus: { PERMISSION_GRANTED: 0 } },
    bundleManager: {
      BundleFlag: { GET_BUNDLE_INFO_WITH_APPLICATION: 1, GET_BUNDLE_INFO_WITH_REQUESTED_PERMISSION: 2 },
      getBundleInfoForSelfSync: (flags) => {
        assert.equal(flags, 3);
        if (introspectionFailed) { throw new Error('unavailable'); }
        return {
          appInfo: { accessTokenId: 123 },
          reqPermissionDetails: Object.keys(tokenStates).concat('  ').map((name) => ({ name })),
          // Deliberately stale bundle state: live token denial must win.
          permissionGrantStates: [0, 0, 0, -1, 0],
        };
      },
    },
  };
  const permissions = load('runtime/Permissions.ets', { '@kit.AbilityKit': ability, './Diagnostics': diagnostics });
  const context = (resource) => ({ resourceManager: { getStringByNameSync: () => resource } });
  await permissions.requestUserPermissions(context(' ,ohos.permission.CAMERA,, ohos.permission.MICROPHONE, ohos.permission.CAMERA, '));
  assert.deepEqual(prompts, [['ohos.permission.CAMERA', 'ohos.permission.MICROPHONE']]);
  assert.deepEqual(plain(permissions.grantedPermissions()), ['ohos.permission.INTERNET', 'ohos.permission.CAMERA']);
  await permissions.requestUserPermissions(context(' , , '));
  assert.equal(prompts.length, 1);
  manager.requestPermissionsFromUser = async () => { throw new Error('cancelled'); };
  await permissions.requestUserPermissions(context('ohos.permission.CAMERA'));
  assert.deepEqual(plain(permissions.grantedPermissions()), ['ohos.permission.INTERNET', 'ohos.permission.CAMERA']);
  introspectionFailed = true;
  assert.deepEqual(plain(permissions.grantedPermissions()), []);
  console.log('PASS permissions: empty/duplicate filtering; system grants; live token truth; cancellation; fail closed');
}

async function testIndex() {
  for (const mode of ['game', 'editor-none', 'editor-sdk']) {
    const profile = mode === 'game' ? gameProfile() : editorProfile(mode === 'editor-sdk' ? 'sdk' : 'none');
    const calls = [];
    const geometrySnapshots = [];
    const displayOrigins = new Map([[0, [0, 0]], [1, [2560, 0]], [2, [-1920, -1080]]]);
    let sdkDisplayId = 0;
    let sdkWindowRect = { left: -100, top: 350, width: 2000, height: 1500 };
    let currentState = 1;
    const plugin = {
      configure: (...args) => calls.push(['configure', ...args]), setResourceManager: () => {}, setWindowId: () => {},
      setup: (...args) => {
        expectNativeArray(args[0]);
        expectNativeArray(args[1]);
        calls.push(['setup', ...args]);
      }, state: () => currentState,
      processId: () => 11, destroySurface: () => {},
      setSurfacePosition: (x, y) => {
        calls.push(['surface-position', x, y]);
        // Model the native UI-owner SDK query, not event payloads or JS caches.
        // The real SDK snapshot/map is independently covered by native_ime_geometry.
        const properties = hostWindow.getWindowProperties();
        geometrySnapshots.push({ global: [x, y], displayId: properties.displayId,
          origin: displayOrigins.get(properties.displayId), rect: properties.windowRect });
      },
      inputTouch: () => {}, sendWindowEvent: () => {},
    };
    const paths = mode === 'game' ? applicationPaths : modulePaths;
    const userPermissions = ['ohos.permission.MICROPHONE'];
    if (mode === 'editor-sdk') { userPermissions.push('ohos.permission.READ_WRITE_USER_FILE'); }
    const granted = ['ohos.permission.INTERNET', ...userPermissions];
    const context = {
      ...hostContext(),
      resourceManager: { ...stringResourceManager(), getStringByNameSync: () => [...userPermissions, ...userPermissions, ' '].join(',') },
      terminateSelf: async () => calls.push(['terminateSelf']),
    };
    const { hostDiagnostics } = diagnosticFixture(plugin);
    const permissions = load('runtime/Permissions.ets', {
      './Diagnostics': hostDiagnostics,
      '@kit.AbilityKit': {
        abilityAccessCtrl: {
          GrantStatus: { PERMISSION_GRANTED: 0 },
          createAtManager: () => ({
            requestPermissionsFromUser: async (requestContext, names) => {
              assert.equal(requestContext, context);
              calls.push(['user-permissions', [...names]]);
              return { authResults: names.map(() => 0) };
            },
            checkAccessTokenSync: () => 0,
          }),
        },
        bundleManager: {
          BundleFlag: { GET_BUNDLE_INFO_WITH_APPLICATION: 1, GET_BUNDLE_INFO_WITH_REQUESTED_PERMISSION: 2 },
          getBundleInfoForSelfSync: () => ({
            appInfo: { accessTokenId: 123 }, reqPermissionDetails: granted.map((name) => ({ name })),
            permissionGrantStates: granted.map(() => 0),
          }),
        },
      },
    });
    const storage = {
      godotHostConfig: profile, godotConfigError: '',
      godotArguments: new Proxy(config.engineArguments(profile, {}), {}), godotWindowId: 7,
    };
    const geometryEvents = ['windowRectChange', 'displayIdChange', 'systemDensityChange'];
    const geometryListeners = new Map();
    const releasedGeometryEvents = [];
    const hostWindow = {
      getWindowProperties: () => ({ displayId: sdkDisplayId, windowRect: { ...sdkWindowRect } }),
      on: (name, listener) => {
        assert.ok(geometryEvents.includes(name), `Unexpected window event ${name}`);
        assert.equal(geometryListeners.has(name), false, `Duplicate ${name} subscription`);
        geometryListeners.set(name, listener);
        listener({ ignoredStalePayload: true });
        assert.equal(geometrySnapshots.length, 0, 'not-ready pages never publish during registration');
      },
      off: (name, listener) => {
        assert.equal(listener, geometryListeners.get(name), `${name} cleanup uses the registered handler`);
        geometryListeners.delete(name);
        releasedGeometryEvents.push(name);
      },
    };
    const dependencies = {
      '@kit.AbilityKit': {},
      '@kit.ArkUI': { window: { getLastWindow: async () => hostWindow } },
      'libentry.so': { default: plugin }, '@kit.InputKit': { KeyCode: {} }, './KeyMap': { mapKeyCode: () => 0 },
      '../runtime/Config': config,
      '../runtime/NativeArguments': nativeArguments,
      '../runtime/LocalizedStrings': localizedStrings,
      '../runtime/TouchInput': touchInput,
      '../runtime/AxisInput': load('runtime/AxisInput.ets', {}),
      '../runtime/Permissions': permissions,
      '../runtime/Runtime': {
        prepareRuntime: async () => { calls.push(['runtime']); return '/private/runtime'; },
      },
      '../runtime/Diagnostics': hostDiagnostics,
      '../runtime/ExternalApps': { openExternal: async () => {} },
      '../runtime/Launch': {
        spawnedChildPid: () => { calls.push(['child-query']); return 0; }, spawnedChildLogDirectory: () => '', childHasExited: () => false,
        reportExit: () => calls.push(['exit-marker']), logInstanceState: () => calls.push(['instance-state']),
      },
    };
    const transform = (source) => {
      assert.ok(source.includes('  build() {'), 'Index structure changed; update the logic-only test transformation');
      return (source.slice(0, source.indexOf('  build() {')) + '}\n')
        .replace('@Entry\n@Component\nstruct Index', 'export class Index').replaceAll('@State ', '');
    };
    const surfaceRect = { surfaceWidth: 800, surfaceHeight: 600, offsetX: 13.25, offsetY: -4.5 };
    let framePosition = { x: 125.5, y: 50.25 };
    let density = 2.5;
    let framePresent = true;
    const { Index } = load('pages/Index.ets', dependencies, {
      XComponentController: class { getXComponentSurfaceRect() { return surfaceRect; } },
      AppStorage: { get: (key) => storage[key] }, $r: stringResource, setInterval: () => 1, clearInterval: () => {},
    }, transform);
    const page = new Index();
    page.getUIContext = () => ({
      getHostContext: () => context, vp2px: (value) => value * density,
      getFrameNodeById: (id) => {
        assert.equal(id, 'godot-surface');
        return framePresent ? { getGlobalPositionOnDisplay: () => framePosition } : null;
      },
    });
    await page.prepare();
    assert.equal(page.ready, true, `${mode}: native setup failed: ${page.message}`);
    assert.ok(calls.some((call) => call[0] === 'setup'), `${mode}: setup must reach the plain-array native boundary`);
    assert.deepEqual(plain(calls.find((call) => call[0] === 'configure')),
      ['configure', paths.filesDir, paths.cacheDir, mode === 'editor-sdk' ? '/private/runtime' : '', mode === 'editor-sdk' ? 'sdk' : 'none', paths.tempDir]);
    assert.deepEqual(plain(calls.find((call) => call[0] === 'setup')),
      ['setup', plain(storage.godotArguments), granted, mode === 'game']);
    assert.equal(calls.some((call) => call[0] === 'runtime'), mode === 'editor-sdk');
    // All roles request declared user grants, including non-SDK editor MIC.
    // Even duplicated SDK file-access resource entries yield only one request.
    assert.deepEqual(plain(calls.filter((call) => call[0] === 'user-permissions')),
      [['user-permissions', userPermissions]], mode);
    assert.equal(hostDiagnostics.diagnosticsPath(), `${paths.filesDir}/godot-11-diagnostics.log`, mode);
    assert.equal(hostDiagnostics.engineLogPath(11), `${paths.filesDir}/godot-11-engine.log`, mode);
    assert.ok(hostDiagnostics.readDiagnostics().includes(`cacheDir=${paths.cacheDir}`), mode);
    assert.equal(page.showStatus, false);
    assert.equal(page.engineRunning, false);
    assert.deepEqual([...geometryListeners.keys()].sort(), [...geometryEvents].sort());
    assert.equal(new Set(geometryListeners.values()).size, 1, 'rect/display/density use the same refresh handler');
    const fireGeometry = (event) => geometryListeners.get(event)({ staleDisplayId: 999, staleRect: { left: 9999 }, staleDensity: 99 });
    const assertGeometry = (global, displayId, message) => {
      assert.deepEqual(calls.at(-1), ['surface-position', ...global], message);
      assert.deepEqual(geometrySnapshots.at(-1), { global, displayId,
        origin: displayOrigins.get(displayId), rect: sdkWindowRect }, message);
    };
    page.updateImeSurfacePosition();
    assertGeometry([327, 121.125], 0, 'convert only global node vp, retain surface px offsets');
    framePosition = { x: 250.25, y: 100.75 };
    fireGeometry('windowRectChange');
    assertGeometry([638.875, 247.375], 0, 'window-only move refreshes the global surface origin');

    // Events can arrive in either order; each callback fetches the current SDK
    // display snapshot and actual FrameNode position rather than event arguments.
    sdkDisplayId = 1;
    sdkWindowRect = { ...sdkWindowRect, left: 2460 };
    framePosition = { x: 1149.5, y: 50.25 };
    for (const event of ['windowRectChange', 'displayIdChange']) {
      const previous = geometrySnapshots.length;
      fireGeometry(event);
      assert.equal(geometrySnapshots.length, previous + 1, `${event} refreshes in rect-first ordering`);
      assertGeometry([2887, 121.125], 1, 'right display keeps its independent global desktop coordinates');
    }
    sdkDisplayId = 2;
    sdkWindowRect = { ...sdkWindowRect, left: -2020, top: -730 };
    framePosition = { x: -642.5, y: -381.75 };
    for (const event of ['displayIdChange', 'windowRectChange']) {
      const previous = geometrySnapshots.length;
      fireGeometry(event);
      assert.equal(geometrySnapshots.length, previous + 1, `${event} refreshes in id-first ordering`);
      assertGeometry([-1593, -958.875], 2, 'above/left display preserves negative global coordinates');
    }

    // A changed display ID must publish even if both rectangles are unchanged.
    sdkDisplayId = 1;
    const beforeDisplayOnly = geometrySnapshots.length;
    fireGeometry('displayIdChange');
    assert.equal(geometrySnapshots.length, beforeDisplayOnly + 1, 'same-rect display change is not deduplicated');
    assertGeometry([-1593, -958.875], 1, 'IME display metadata refresh does not rebase global surface coordinates');

    // Reflow may change vp and density while physical px stay fixed; offsets
    // remain px. Later reflow updates are read afresh by density and rect events.
    density = 1.25;
    framePosition = { x: framePosition.x * 2, y: framePosition.y * 2 };
    fireGeometry('systemDensityChange');
    assertGeometry([-1593, -958.875], 1, 'density reflow preserves physical global coordinates');
    framePosition = { x: 10.5, y: 20.25 };
    for (const event of ['systemDensityChange', 'windowRectChange']) {
      const previous = geometrySnapshots.length;
      fireGeometry(event);
      assert.equal(geometrySnapshots.length, previous + 1, 'density/rect ordering always refreshes geometry');
      assertGeometry([26.375, 20.8125], 1, 'new reflow position applies current density once, without scaling px offsets');
    }
    const beforeGuardedEvents = geometrySnapshots.length;
    framePresent = false;
    for (const event of geometryEvents) { fireGeometry(event); }
    framePresent = true;
    page.terminating = true;
    for (const event of geometryEvents) { fireGeometry(event); }
    page.terminating = false;
    assert.equal(geometrySnapshots.length, beforeGuardedEvents, 'missing frames and terminating hosts do not publish');
    page.updateEngineState(context);
    assert.equal(page.engineRunning, false, 'loading must keep the splash');
    currentState = 2;
    page.updateEngineState(context);
    assert.equal(page.engineRunning, true, 'running releases the splash');
    currentState = 3;
    page.updateEngineState(context);
    await Promise.resolve();
    assert.ok(calls.some((call) => call[0] === 'terminateSelf'));
    if (mode === 'game') {
      assert.ok(!calls.some((call) => ['child-query', 'instance-state', 'exit-marker'].includes(call[0])));
    }
    const lateGeometryCallbacks = [...geometryListeners.values()];
    page.aboutToDisappear();
    assert.equal(geometryListeners.size, 0, 'all geometry listeners are released');
    assert.deepEqual(releasedGeometryEvents.sort(), [...geometryEvents].sort(), 'cleanup covers rect, display ID and density');
    const count = calls.length;
    page.terminating = false; // Isolate the disposed guard from the terminate guard.
    page.updateImeSurfacePosition();
    for (const listener of lateGeometryCallbacks) { listener(); }
    assert.equal(calls.length, count, 'destroyed pages and queued callbacks never publish surface geometry');
  }
  console.log('PASS Index logic: game/editor NAPI, logs and permission grants; rect/display/density subscription and cleanup, both event orders, same-rect display changes, density reflow and late callback guards; SDK-only runtime; no game child logic');
}

async function testLauncher() {
  const profile = editorProfile();
  const scenarios = [
    { name: 'PM editor independent', pm: true, args: ['--editor'], independent: true },
    { name: 'PM game attached', pm: true, args: ['--path', '/game'], independent: false },
    { name: 'editor game attached', pm: false, args: ['--path', '/game'], independent: false },
    { name: 'custom child logs', pm: false, args: ['--log-file', '/custom logs 中文/engine.log'], independent: false, logDirectory: '/custom logs 中文' },
    { name: 'root child logs', pm: false, args: ['--log-file', '/engine.log'], independent: false, logDirectory: '/' },
    { name: 'first log switch', pm: false, args: ['--log-file', '/first/engine.log', '--log-file', '/later/engine.log'], independent: false, logDirectory: '/first' },
    { name: 'user log switch ignored', pm: false, args: ['++', '--log-file', '/foreign/engine.log'], independent: false },
    { name: 'user arguments ignored', pm: true, args: ['--', '--editor'], independent: false },
    { name: 'ability fallback', pm: true, args: ['-e'], independent: true, fallback: true },
    { name: 'game blocked', pm: true, args: ['-e'], blocked: true, profile: gameProfile() },
    { name: 'disabled blocked', pm: true, args: ['-e'], blocked: true, profile: { ...profile, instances: { policy: 'disabled', maxCount: 1 } } },
    { name: 'Want disabled blocked', pm: true, args: ['-e'], blocked: true, profile: { ...profile, launch: { ...profile.launch, acceptProjectRequests: false } } },
    { name: 'max count blocked', pm: true, args: ['-e'], blocked: true, full: true },
  ];
  for (const scenario of scenarios) {
    let childAlive = true;
    const files = new Map();
    const calls = [];
    const results = [];
    const application = {
      getCurrentInstanceKey: () => 'parent-instance',
      getAllRunningInstanceKeys: async () => scenario.full ? ['1', '2', '3', '4', '5'] : ['parent-instance'],
    };
    const context = {
      cacheDir: '/private/cache', abilityInfo: { name: scenario.fallback ? '' : 'RenamedHostAbility', bundleName: 'org.oheco.dynamic' },
      getApplicationContext: () => application,
      startAbility: async (want, options) => {
        calls.push({ want, options });
        files.set('/private/cache/godot-launch/' + want.parameters.godotLaunchKey, JSON.stringify({
          pid: 123, instanceKey: scenario.independent ? 'child-instance' : 'parent-instance',
          phase: scenario.independent ? 'window-ready' : 'ability-created', error: '',
        }));
      },
    };
    const launch = load('runtime/Launch.ets', {
      '@kit.AbilityKit': {
        contextConstant: { ProcessMode: { NEW_PROCESS_ATTACH_TO_PARENT: 1 }, StartupVisibility: { STARTUP_SHOW: 2 } },
        wantConstant: { Params: { CREATE_APP_INSTANCE_KEY: 'create-key' } },
      },
      '@kit.CoreFileKit': { fileIo: {
        statSync: (file) => ({ isFile: () => true, size: files.get(file).length }),
        readTextSync: (file) => files.get(file), unlinkSync: (file) => files.delete(file),
      } },
      'libentry.so': { default: { processId: () => 11, spawnResult: (request, pid) => results.push([request, pid]), processIsRunning: () => childAlive } },
      './Diagnostics': diagnostics, './FileSystem': { ensureDirectory: () => {}, exists: (file) => files.has(file) }, './Config': config,
    }, { console: { ...console, error: () => {} } });
    await launch.launchInstance(context, 77, scenario.args, scenario.pm, scenario.profile || profile);
    assert.deepEqual(plain(results), [[77, scenario.blocked ? -1 : 123]], scenario.name);
    if (scenario.blocked) { assert.equal(calls.length, 0, scenario.name); continue; }
    assert.equal(calls[0].want.bundleName, 'org.oheco.dynamic');
    assert.equal(calls[0].want.abilityName, scenario.fallback ? 'EntryAbility' : 'RenamedHostAbility');
    assert.equal(!!calls[0].want.parameters['create-key'], scenario.independent);
    assert.equal(calls[0].options === undefined, scenario.independent);
    assert.equal(launch.spawnedChildPid(), scenario.independent ? 0 : 123);
    assert.equal(launch.spawnedChildLogDirectory(), scenario.independent ? '' : scenario.logDirectory || '', 'Child crash lookup follows the actual engine log directory');
    assert.equal(launch.childHasExited(context, 123), false, 'Live child has no exit marker');
    childAlive = false;
    assert.equal(launch.childHasExited(context, 123), true, 'Fatal exit without a marker is still detected');
    childAlive = true;
    files.set('/private/cache/godot-launch/exited-123', '');
    assert.equal(launch.childHasExited(context, 123), true, 'Orderly exit marker takes effect immediately');
    assert.equal(launch.childHasExited(context, 0), false, 'No attached PID is not queried');
  }
  console.log(`PASS launcher: ${scenarios.length} scenarios; dynamic target; PM independent/game attached; policy/Want/maxCount gates`);
}

async function testRuntime() {
  const expected = {
    schemaVersion: 1, engineVersion: '4.7.2', hostAbiVersion: 4,
    managedVersion: '4.7.2-ohos.3', architecture: 'arm64-v8a', engineLibrarySha256: 'a'.repeat(64),
  };
  const manifest = {
    ...expected, managedRoot: 'share/godot', templatesRoot: 'share/godot/templates', templateSetSha256: 'b'.repeat(64),
  };
  const root = '/external/development kit 中文';
  const fallbackRoot = '/external/another kit';
  const candidate = (kitRoot = root, value = manifest) => ({ root: kitRoot, manifest: JSON.stringify(value) });
  const assets = [
    'bin/libgodot.so', 'share/godot/GodotSharp/Api/Debug/GodotSharp.dll',
    ...['Godot.NET.Sdk', 'Godot.SourceGenerators', 'GodotSharp', 'GodotSharpEditor'].map(
      (name) => `share/godot/GodotSharp/Tools/nupkgs/${name}.${expected.managedVersion}.nupkg`),
    'share/godot/templates/.ready', 'share/godot/templates/openharmony_debug_arm64-v8a.zip',
    'share/godot/templates/openharmony_release_arm64-v8a.zip',
  ];
  const fixtures = [];
  function fixture(candidates = [candidate()]) {
    const files = new Map();
    for (const kitRoot of [root, fallbackRoot]) {
      for (const name of assets) { files.set(`${kitRoot}/${name}`, 'fixture asset'); }
      files.set(`${kitRoot}/share/godot/templates/.ready`, `${manifest.templateSetSha256}\n`);
    }
    const state = { candidates, applicationManifest: JSON.stringify(expected) };
    const logs = [], steps = [], resources = [], inspected = [], reads = [], unexpected = [];
    const calls = { discovery: 0 };
    // Runtime discovery is read-only. Keep assets in memory and detect unexpected
    // filesystem APIs even when the production error boundary catches the error.
    const fileIo = new Proxy({
      statSync: (file) => {
        inspected.push(file);
        if (!files.has(file)) { throw new Error(`ENOENT: ${file}`); }
        const value = files.get(file);
        if (value instanceof Error) { throw value; }
        return { isFile: () => typeof value === 'string', size: typeof value === 'string' ? Buffer.byteLength(value) : 0 };
      },
      readTextSync: (file, options) => {
        reads.push([file, plain(options)]);
        if (state.readError) { throw state.readError; }
        return files.get(file).slice(0, options.length);
      },
    }, { get: (target, key) => {
      if (Object.hasOwn(target, key)) { return target[key]; }
      unexpected.push(String(key));
      throw new Error(`Unexpected Runtime filesystem API: ${String(key)}`);
    } });
    const runtime = load('runtime/Runtime.ets', {
      '@kit.CoreFileKit': { fileIo }, '@kit.ArkTS': { util },
      'libentry.so': { default: { developmentKits: () => {
        calls.discovery++;
        if (state.discoveryError) { throw state.discoveryError; }
        return state.candidates;
      } } },
      './Diagnostics': { ...diagnostics, record: (message) => logs.push(message), setStep: (step) => steps.push(step) },
    });
    const context = { ...hostContext(), resourceManager: { getRawFileContent: async (name) => {
      resources.push(name);
      if (state.gate) { await state.gate; }
      if (state.resourceError) { throw state.resourceError; }
      return Buffer.from(state.applicationManifest);
    } } };
    const result = { runtime, context, state, files, logs, steps, resources, inspected, reads, unexpected, calls };
    fixtures.push(result);
    return result;
  }
  const prepare = (test) => test.runtime.prepareRuntime(test.context);
  const uses = (test, kitRoot) => test.logs.includes(`using external development kit: ${kitRoot}`);

  // Concurrent callers share only the in-flight discovery, not a permanent cache.
  const concurrent = fixture([candidate(), candidate(fallbackRoot)]);
  let release;
  concurrent.state.gate = new Promise((resolve) => { release = resolve; });
  const pending = [prepare(concurrent), prepare(concurrent), prepare(concurrent)];
  assert.equal(concurrent.resources.length, 1);
  assert.equal(concurrent.calls.discovery, 0);
  release();
  assert.deepEqual(await Promise.all(pending), new Array(3).fill(`${root}/share/godot`));
  assert.equal(concurrent.calls.discovery, 1);
  assert.equal(uses(concurrent, root), true);
  assert.deepEqual([...new Set(concurrent.inspected)].sort(), assets.map((name) => `${root}/${name}`).sort());
  assert.deepEqual(concurrent.reads, [[`${root}/share/godot/templates/.ready`, { length: 65 }]]);
  concurrent.state.candidates = [];
  assert.equal(await prepare(concurrent), '', 'a removed kit is not cached after discovery completes');
  assert.equal(concurrent.calls.discovery, 2);
  concurrent.state.candidates = [candidate(fallbackRoot)];
  assert.equal(await prepare(concurrent), `${fallbackRoot}/share/godot`, 'a newly installed kit is discovered on retry');
  assert.equal(concurrent.calls.discovery, 3);

  // Native discovery owns ordering and explicit-root selection. ArkTS must skip
  // incompatible candidates without probing assets and accept the next good kit.
  const incompatible = [
    null, [], {}, { ...manifest, schemaVersion: 2 }, { ...manifest, engineVersion: 'other' },
    { ...manifest, hostAbiVersion: 5 }, { ...manifest, managedVersion: 'other' },
    { ...manifest, architecture: 'x86_64' }, { ...manifest, engineLibrarySha256: 'c'.repeat(64) },
    { ...manifest, managedRoot: 'elsewhere' }, { ...manifest, templatesRoot: 'elsewhere' },
    { ...manifest, templateSetSha256: 'B'.repeat(64) },
  ];
  for (const value of incompatible) {
    const test = fixture([candidate('/external/incompatible', value), candidate()]);
    assert.equal(await prepare(test), `${root}/share/godot`, JSON.stringify(value));
    assert.ok(test.logs.includes('development kit incompatible with this Editor: /external/incompatible'));
    assert.ok(test.inspected.every((file) => file.startsWith(root + '/')));
  }
  for (const bad of [
    { root: 'relative/kit', manifest: JSON.stringify(manifest) },
    { root: '/external/bad\0kit', manifest: JSON.stringify(manifest) },
    { root: '/external/no manifest', manifest: '' }, { root: '/external/bad json', manifest: '{' },
  ]) {
    const test = fixture([bad, candidate()]);
    assert.equal(await prepare(test), `${root}/share/godot`);
    assert.ok(test.logs.some((message) => message.includes(bad.root) && /unavailable|rejected/.test(message)));
    assert.ok(test.inspected.every((file) => file.startsWith(root + '/')));
  }

  // Every required asset matters. A missing file or a directory in its place
  // rejects that candidate while preserving discovery of the next complete kit.
  for (const name of assets) {
    for (const kind of ['missing', 'directory']) {
      const test = fixture([candidate(), candidate(fallbackRoot)]);
      if (kind === 'missing') { test.files.delete(`${root}/${name}`); } else { test.files.set(`${root}/${name}`, null); }
      assert.equal(await prepare(test), `${fallbackRoot}/share/godot`, `${kind}: ${name}`);
      assert.equal(uses(test, root), false, `${kind}: ${name}`);
      assert.equal(uses(test, fallbackRoot), true, `${kind}: ${name}`);
      assert.ok(test.logs.some((message) => message.startsWith(kind === 'missing' ?
        `development kit rejected (${root}):` : `development kit assets incomplete: ${root}`)));
    }
  }
  for (const marker of ['', manifest.templateSetSha256, 'c'.repeat(64) + '\n', manifest.templateSetSha256 + '\nextra']) {
    const test = fixture([candidate(), candidate(fallbackRoot)]);
    test.files.set(`${root}/share/godot/templates/.ready`, marker);
    assert.equal(await prepare(test), `${fallbackRoot}/share/godot`, `invalid ready marker: ${JSON.stringify(marker)}`);
    assert.ok(test.logs.includes(`development kit assets incomplete: ${root}`));
  }
  for (const operation of ['stat', 'read']) {
    const test = fixture([candidate()]);
    if (operation === 'stat') { test.files.set(`${root}/bin/libgodot.so`, new Error('EACCES fixture')); }
    else { test.state.readError = new Error('EACCES fixture'); }
    assert.equal(await prepare(test), '');
    assert.ok(test.logs.some((message) => message.startsWith(`development kit rejected (${root}):`) && message.includes('EACCES fixture')));
    test.files.set(`${root}/bin/libgodot.so`, 'fixture asset');
    delete test.state.readError;
    assert.equal(await prepare(test), `${root}/share/godot`, `${operation} failure must not poison later discovery`);
  }

  const absent = fixture([]);
  assert.equal(await prepare(absent), '');
  assert.ok(absent.logs.some((message) => message.startsWith('No compatible godot-editor development kit found;')));
  assert.deepEqual(absent.inspected, []);
  // Native test_development_kit_discovery.py checks that an explicit invalid root
  // yields only this empty-manifest candidate. A compatible kit exists in the
  // fixture filesystem, but ArkTS must not independently scan or select it.
  const explicit = fixture([{ root: '/explicit/invalidroot', manifest: '' }]);
  assert.equal(await prepare(explicit), '');
  assert.ok(explicit.logs.includes('development kit unavailable: /explicit/invalidroot'));
  assert.deepEqual(explicit.inspected, []);
  assert.equal(explicit.calls.discovery, 1);

  const invalidRequirements = [
    '{', ...[null, [], {}, { ...expected, schemaVersion: 2 }, { ...expected, engineVersion: '' },
      { ...expected, hostAbiVersion: 0 }, { ...expected, hostAbiVersion: 1.5 }, { ...expected, managedVersion: '' },
      { ...expected, architecture: 'x86_64' }, { ...expected, engineLibrarySha256: 'A'.repeat(64) }].map((value) => JSON.stringify(value)),
  ];
  for (const requirement of invalidRequirements) {
    const test = fixture();
    test.state.applicationManifest = requirement;
    assert.equal(await prepare(test), '', requirement);
    assert.equal(test.calls.discovery, 0, 'invalid application requirements must not reach native discovery');
    assert.deepEqual(test.inspected, []);
    assert.ok(test.logs.some((message) => message.startsWith('Development-kit discovery unavailable; starting the native Editor:')));
  }
  for (const failure of ['resourceError', 'discoveryError']) {
    const test = fixture();
    test.state[failure] = new Error(`fixture ${failure}`);
    assert.equal(await prepare(test), '', `${failure} must fall back to the native Editor`);
    assert.equal(test.calls.discovery, failure === 'resourceError' ? 0 : 1);
    assert.deepEqual(test.inspected, []);
    assert.ok(test.logs.some((message) => message.startsWith('Development-kit discovery unavailable;') && message.includes(`fixture ${failure}`)));
    delete test.state[failure];
    assert.equal(await prepare(test), `${root}/share/godot`, `${failure} must not leave a stale in-flight promise`);
  }
  for (const test of fixtures) {
    assert.ok(test.resources.length > 0);
    assert.ok(test.resources.every((name) => name === 'development-kit.json'), 'only the compatibility record is read from raw resources');
    assert.deepEqual(test.steps, new Array(test.resources.length).fill('runtime:discover-development-kit'));
    assert.deepEqual(test.unexpected, [], 'discovery must use only read-only filesystem APIs');
  }
  console.log('PASS runtime: compatible ordered discovery; every required asset/ready validation; no-kit and explicit-invalid-root fallback; local coalescing and rediscovery; candidate/resource/native error boundaries');
}

async function main() {
  console.log('ArkTS host logic tests (mocks; not a substitute for CompileArkTS or HAP/device acceptance)');
  testConfig();
  testNativeArguments();
  testStorageAndAbility();
  testDirectoryRace();
  await testExternalApps();
  await testPermissions();
  await testIndex();
  await testLauncher();
  await testRuntime();
  console.log('PASS all ArkTS host logic regression tests');
}
main().catch((error) => { console.error(error); process.exitCode = 1; });
