// Godot Engine contributors. SPDX-License-Identifier: MIT
// Host-logic regression tests, NOT HAP/device acceptance tests. They transpile
// the real ArkTS sources and replace HarmonyOS APIs/NAPI with controlled mocks.
// Index's ArkUI build() is deliberately excluded; CompileArkTS must separately
// verify decorators, component attributes (including expandSafeArea), and SDK APIs.
//
// No dependencies are downloaded. Supply an existing SDK TypeScript compiler:
//   node platform/openharmony/tests/test_arkts_host.cjs --compiler /path/to/typescript.js
// Or set GODOT_ARKTS_TYPESCRIPT. TMPDIR must name an existing temporary directory.
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
    startAbility: async want => { calls.push(['ability', plain(want)]); if (launchFailure) throw new Error('platform denied'); },
    openLink: async (uri, options) => calls.push(['link', uri, plain(options)]),
  };
  const external = load('runtime/ExternalApps.ets', {
    '@kit.CoreFileKit': { fileUri: { getUriFromPath: value => 'file://fixture' + encodeURI(value) } },
    '@ohos.window': { default: { getLastWindow: async () => ({ getUIContext: () => ({ getPromptAction: () => ({ showToast: value => toasts.push(value) }) }) }) } },
    'libentry.so': { default: { openTerminal: async uri => { calls.push(['terminal-worker', uri]); if (launchFailure) throw new Error('broker missing'); } } },
    './Diagnostics': { describe: String, record: value => logs.push(value) },
  });
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
    let currentState = 1;
    const plugin = {
      configure: (...args) => calls.push(['configure', ...args]), setResourceManager: () => {}, setWindowId: () => {},
      setup: (...args) => {
        expectNativeArray(args[0]);
        expectNativeArray(args[1]);
        calls.push(['setup', ...args]);
      }, state: () => currentState,
      processId: () => 11, destroySurface: () => {},
      setSurfacePosition: (x, y) => calls.push(['surface-position', x, y]),
      inputTouch: () => {}, sendWindowEvent: () => {},
    };
    const paths = mode === 'game' ? applicationPaths : modulePaths;
    const userPermissions = ['ohos.permission.MICROPHONE'];
    if (mode === 'editor-sdk') { userPermissions.push('ohos.permission.READ_WRITE_USER_FILE'); }
    const granted = ['ohos.permission.INTERNET', ...userPermissions];
    const context = {
      ...hostContext(),
      resourceManager: { getStringByNameSync: () => [...userPermissions, ...userPermissions, ' '].join(',') },
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
    let geometryListener;
    const hostWindow = {
      on: (name, listener) => { assert.equal(name, 'windowRectChange'); geometryListener = listener; },
      off: (name, listener) => { assert.equal(name, 'windowRectChange'); assert.equal(listener, geometryListener); geometryListener = null; },
    };
    const dependencies = {
      '@kit.AbilityKit': {},
      '@kit.ArkUI': { window: { getLastWindow: async () => hostWindow } },
      'libentry.so': { default: plugin }, '@kit.InputKit': { KeyCode: {} }, './KeyMap': { mapKeyCode: () => 0 },
      '../runtime/Config': config,
      '../runtime/NativeArguments': nativeArguments,
      '../runtime/TouchInput': touchInput,
      '../runtime/Permissions': permissions,
      '../runtime/Runtime': {
        prepareRuntime: async () => { calls.push(['runtime']); return '/private/runtime'; },
      },
      '../runtime/Templates': {
        prepareTemplates: async (_context, filesDir) => { calls.push(['templates', filesDir]); },
      },
      '../runtime/Diagnostics': hostDiagnostics,
      '../runtime/ExternalApps': { openExternal: async () => {} },
      '../runtime/Launch': {
        spawnedChildPid: () => { calls.push(['child-query']); return 0; }, childHasExited: () => false,
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
    const { Index } = load('pages/Index.ets', dependencies, {
      XComponentController: class { getXComponentSurfaceRect() { return surfaceRect; } },
      AppStorage: { get: (key) => storage[key] }, setInterval: () => 1, clearInterval: () => {},
    }, transform);
    const page = new Index();
    page.getUIContext = () => ({
      getHostContext: () => context, vp2px: (value) => value * 2.5,
      getFrameNodeById: (id) => { assert.equal(id, 'godot-surface'); return { getGlobalPositionOnDisplay: () => framePosition }; },
    });
    await page.prepare();
    assert.equal(page.ready, true, `${mode}: native setup failed: ${page.message}`);
    assert.ok(calls.some((call) => call[0] === 'setup'), `${mode}: setup must reach the plain-array native boundary`);
    assert.deepEqual(plain(calls.find((call) => call[0] === 'configure')),
      ['configure', paths.filesDir, paths.cacheDir, mode === 'editor-sdk' ? '/private/runtime' : '', mode === 'editor-sdk' ? 'sdk' : 'none']);
    assert.deepEqual(plain(calls.find((call) => call[0] === 'setup')),
      ['setup', plain(storage.godotArguments), granted, mode === 'game']);
    assert.equal(calls.some((call) => call[0] === 'runtime'), mode === 'editor-sdk');
    assert.equal(calls.some((call) => call[0] === 'templates'), mode !== 'game');
    if (mode !== 'game') { assert.deepEqual(calls.find((call) => call[0] === 'templates'), ['templates', paths.filesDir]); }
    // All roles request declared user grants, including non-SDK editor MIC.
    // Even duplicated SDK file-access resource entries yield only one request.
    assert.deepEqual(plain(calls.filter((call) => call[0] === 'user-permissions')),
      [['user-permissions', userPermissions]], mode);
    assert.equal(hostDiagnostics.diagnosticsPath(), `${paths.filesDir}/godot-11-diagnostics.log`, mode);
    assert.equal(hostDiagnostics.engineLogPath(11), `${paths.filesDir}/godot-11-engine.log`, mode);
    assert.ok(hostDiagnostics.readDiagnostics().includes(`cacheDir=${paths.cacheDir}`), mode);
    assert.equal(page.showStatus, false);
    assert.equal(page.engineRunning, false);
    page.updateImeSurfacePosition();
    assert.deepEqual(calls.at(-1), ['surface-position', 327, 121.125], 'convert only node vp, retain surface px offsets');
    framePosition = { x: 250.25, y: 100.75 };
    geometryListener();
    assert.deepEqual(calls.at(-1), ['surface-position', 638.875, 247.375], 'window-only move refreshes actual screen origin');
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
    page.aboutToDisappear();
    assert.equal(geometryListener, null, 'window geometry listener is released');
    const count = calls.length;
    page.updateImeSurfacePosition();
    assert.equal(calls.length, count, 'destroyed pages never publish surface geometry');
  }
  console.log('PASS Index logic: game application/editor module NAPI and logs; all roles request declared permissions once; SDK-only runtime; no game child logic');
}

async function testLauncher() {
  const profile = editorProfile();
  const scenarios = [
    { name: 'PM editor independent', pm: true, args: ['--editor'], independent: true },
    { name: 'PM game attached', pm: true, args: ['--path', '/game'], independent: false },
    { name: 'editor game attached', pm: false, args: ['--path', '/game'], independent: false },
    { name: 'user arguments ignored', pm: true, args: ['--', '--editor'], independent: false },
    { name: 'ability fallback', pm: true, args: ['-e'], independent: true, fallback: true },
    { name: 'game blocked', pm: true, args: ['-e'], blocked: true, profile: gameProfile() },
    { name: 'disabled blocked', pm: true, args: ['-e'], blocked: true, profile: { ...profile, instances: { policy: 'disabled', maxCount: 1 } } },
    { name: 'Want disabled blocked', pm: true, args: ['-e'], blocked: true, profile: { ...profile, launch: { ...profile.launch, acceptProjectRequests: false } } },
    { name: 'max count blocked', pm: true, args: ['-e'], blocked: true, full: true },
  ];
  for (const scenario of scenarios) {
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
      'libentry.so': { default: { processId: () => 11, spawnResult: (request, pid) => results.push([request, pid]) } },
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
  }
  console.log(`PASS launcher: ${scenarios.length} scenarios; dynamic target; PM independent/game attached; policy/Want/maxCount gates`);
}

async function testRuntime() {
  if (!process.env.TMPDIR) { throw new Error('TMPDIR is required for runtime filesystem fixtures'); }
  const temporary = fs.mkdtempSync(path.join(process.env.TMPDIR, 'godot-host-tests-'));
  try {
    const app = path.join(temporary, 'app');
    const temp = path.join(temporary, 'temp');
    fs.mkdirSync(app); fs.mkdirSync(temp);
    const payload = Buffer.from('runtime-archive-fixture-with-partial-write-test');
    const sha = crypto.createHash('sha256').update(payload).digest('hex');
    const source = path.join(temporary, 'runtime.zip');
    fs.writeFileSync(source, payload);
    const manifest = { version: 'fixture-v1', sha256: sha, size: payload.length };
    const logs = [];
    const offsets = new Map();
    const stagingPaths = [];
    const archives = [];
    let rawOpens = 0;
    let rawCloses = 0;
    let decompressions = 0;
    const fileIo = {
      OpenMode: { CREATE: fs.constants.O_CREAT, WRITE_ONLY: fs.constants.O_WRONLY, TRUNC: fs.constants.O_TRUNC },
      WhenceType: { SEEK_SET: 0 }, accessSync: (file) => fs.existsSync(file), statSync: (file) => fs.statSync(file),
      mkdirSync: (file) => fs.mkdirSync(file, { recursive: true }), listFileSync: (file) => fs.readdirSync(file),
      rmdirSync: (file) => fs.rmdirSync(file), unlinkSync: (file) => fs.unlinkSync(file), renameSync: (from, to) => fs.renameSync(from, to),
      openSync: (file, flags) => ({ fd: fs.openSync(file, flags) }), closeSync: (file) => fs.closeSync(file.fd),
      lseek: (fd, offset) => offsets.set(fd, offset),
      read: async (fd, buffer, options) => {
        const count = fs.readSync(fd, Buffer.from(buffer), 0, options.length, offsets.get(fd));
        offsets.set(fd, offsets.get(fd) + count);
        return count;
      },
      write: async (fd, buffer) => fs.writeSync(fd, Buffer.from(buffer).subarray(0, 7)),
      writeSync: (fd, value) => fs.writeSync(fd, value),
      readTextSync: (file, options) => {
        const buffer = fs.readFileSync(file);
        const offset = options?.offset ?? 0;
        return buffer.subarray(offset, options?.length ? offset + options.length : undefined).toString();
      },
    };
    let release;
    const gate = new Promise((resolve) => { release = resolve; });
    const zlib = { decompressFile: async (archive, staging) => {
      archives.push(archive); stagingPaths.push(staging); decompressions++;
      if (decompressions === 2) { release(); }
      await gate;
      fs.writeFileSync(path.join(staging, 'runtime.data'), fs.readFileSync(archive));
    } };
    const helpers = load('runtime/FileSystem.ets', { '@kit.CoreFileKit': { fileIo } });
    const runtime = (pid) => load('runtime/Runtime.ets', {
      '@kit.AbilityKit': {},
      '@kit.CoreFileKit': { fileIo, hash: { hash: async (file) => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex') } },
      '@kit.BasicServicesKit': { zlib }, '@kit.ArkTS': { util }, 'libentry.so': { default: { processId: () => pid } },
      './Diagnostics': { ...diagnostics, record: (message) => logs.push(message) }, './FileSystem': helpers,
    });
    const context = () => {
      let fd;
      return { filesDir: app, cacheDir: temp, tempDir: temp, resourceManager: {
        getRawFileContent: async () => Buffer.from(JSON.stringify(manifest)),
        getRawFd: async () => { rawOpens++; fd = fs.openSync(source, 'r'); return { fd, offset: 0, length: payload.length }; },
        closeRawFd: async () => { rawCloses++; fs.closeSync(fd); },
      } };
    };
    // Two isolated module scopes model independent processes and share a real
    // temporary filesystem. A barrier guarantees both reach first extraction.
    const a = runtime(101);
    const b = runtime(202);
    const ca = context();
    const cb = context();
    const foreign = path.join(app, 'runtimes', `v1-${sha}.extracting-foreign`);
    fs.mkdirSync(foreign, { recursive: true }); fs.writeFileSync(path.join(foreign, 'keep'), 'foreign');
    let timeout;
    let results;
    try {
      results = await Promise.race([
        Promise.all([a.prepareRuntime(ca), a.prepareRuntime(ca), b.prepareRuntime(cb)]),
        new Promise((resolve, reject) => {
          timeout = setTimeout(() => reject(new Error('Runtime race did not finish within 5 seconds')), 5000);
        }),
      ]);
    } finally {
      clearTimeout(timeout);
    }
    assert.equal(new Set(results).size, 1);
    assert.equal(rawOpens, 2); assert.equal(rawCloses, 2);
    assert.equal(new Set(stagingPaths).size, 2); assert.equal(new Set(archives).size, 2);
    assert.deepEqual(JSON.parse(fs.readFileSync(path.join(results[0], '.ready'), 'utf8')), manifest);
    assert.equal(fs.readFileSync(path.join(results[0], 'runtime.data'), 'utf8'), payload.toString());
    assert.ok(fs.existsSync(path.join(foreign, 'keep')));
    for (const file of [...archives, ...stagingPaths]) { assert.ok(!fs.existsSync(file), file); }
    assert.ok(logs.some((message) => message.includes('won by another process')));
    assert.equal(await a.prepareRuntime(ca), results[0]); assert.equal(rawOpens, 2);
    fs.writeFileSync(path.join(results[0], '.ready'), '{}');
    await assert.rejects(a.prepareRuntime(ca), /Invalid published runtime/);
    assert.ok(fs.existsSync(path.join(results[0], 'runtime.data')));
    assert.ok(fs.existsSync(path.join(foreign, 'keep')));
    // Hash mismatch must clean up this operation without publishing or deleting
    // either the other process's staging directory or the previous destination.
    manifest.sha256 = '0'.repeat(64);
    await assert.rejects(runtime(303).prepareRuntime(context()), /checksum mismatch/);
    assert.equal(fs.readdirSync(temp).length, 0);
    assert.deepEqual(fs.readdirSync(path.join(app, 'runtimes')).sort(),
      [path.basename(foreign), path.basename(results[0])].sort());
    console.log('PASS runtime: first-publication race; local coalescing; partial writes; ready validation; checksum failure; foreign staging preserved');
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
  }
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
