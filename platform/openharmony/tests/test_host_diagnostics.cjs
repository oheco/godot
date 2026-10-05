// Godot Engine contributors. SPDX-License-Identifier: MIT
// Diagnostics regression tests, NOT CompileArkTS or HAP/device acceptance.
// Transpile the real host sources with an installed SDK compiler, then run with
// mocked HarmonyOS APIs/NAPI and a private filesystem fixture under TMPDIR.
// No dependencies are downloaded and all temporary files are removed on failure.
//   node platform/openharmony/tests/test_host_diagnostics.cjs --compiler /path/to/typescript.js
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function compilerPath() {
  const args = process.argv.slice(2);
  if (args.includes('--help') || args.includes('-h')) {
    console.log('Usage: node test_host_diagnostics.cjs --compiler /path/to/typescript.js\n' +
      'Use the installed SDK compiler. Temporary files: TMPDIR. No network access.');
    process.exit(0);
  }
  if (args.length !== 2 || args[0] !== '--compiler' || !args[1]) {
    throw new Error('Expected --compiler /path/to/typescript.js (or --help); no compiler is downloaded');
  }
  return path.resolve(args[1]);
}

const ts = require(compilerPath());
const repo = path.resolve(__dirname, '../../..');
const sourceRoot = path.join(repo, 'misc/dist/openharmony_template/entry/src/main/ets');
const startMs = Date.UTC(2026, 0, 1, 0, 0, 0, 500);
const startSeconds = Math.floor(startMs / 1000);
const currentPid = 101;
const otherPid = 202;
const childPid = 303;

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
    exports: {}, console, Date,
    require: (id) => {
      assert.ok(Object.hasOwn(dependencies, id), `Unexpected dependency ${id} in ${file}`);
      return dependencies[id];
    },
    ...globals,
  };
  vm.runInNewContext(result.outputText, sandbox, { filename: file });
  return sandbox.exports;
}

function fixture(root, name, nanoseconds = true) {
  const directory = path.join(root, name);
  fs.mkdirSync(directory);
  const paths = { filesDir: directory, cacheDir: directory, tempDir: directory };
  const context = { ...paths, abilityInfo: { bundleName: 'org.oheco.fixture', moduleName: 'entry' } };
  const reads = [];
  const openFiles = new Set();
  const failures = new Set();
  let now = startMs;
  let scanned = 0;
  class Clock extends Date {
    constructor(...args) { super(...(args.length ? args : [now])); }
    static now() { return now; }
  }
  const fileIo = {
    OpenMode: { CREATE: fs.constants.O_CREAT, WRITE_ONLY: fs.constants.O_WRONLY, APPEND: fs.constants.O_APPEND },
    accessSync: (file) => fs.existsSync(file),
    statSync: (file) => {
      if (failures.has(file)) { throw Object.assign(new Error('fixture stat failure'), { code: 13900005 }); }
      const info = fs.statSync(file, { bigint: true });
      // SDK @ohos.file.fs.d.ts: Stat.mtime is Unix SECONDS, not Node's Date or
      // mtimeMs. mtimeNs is optional (API 15+). Do not accidentally mock ms here.
      return {
        size: Number(info.size), mtime: Number(info.mtimeNs / 1000000000n),
        mtimeNs: nanoseconds ? info.mtimeNs : undefined,
        isFile: () => info.isFile(), isDirectory: () => info.isDirectory(),
      };
    },
    listFileSync: (file) => { scanned++; return fs.readdirSync(file); },
    openSync: (file, flags) => {
      const fd = fs.openSync(file, flags);
      openFiles.add(fd);
      return { fd };
    },
    closeSync: (file) => { fs.closeSync(file.fd); openFiles.delete(file.fd); },
    writeSync: (fd, text) => fs.writeSync(fd, text),
    readTextSync: (file, options = {}) => {
      reads.push({ file, ...options });
      const data = fs.readFileSync(file);
      const offset = options.offset ?? 0;
      return data.subarray(offset, options.length === undefined ? undefined : offset + options.length).toString();
    },
  };
  const plugin = { processId: () => currentPid, state: () => 2 };
  const diagnostics = load('runtime/Diagnostics.ets', {
    '@kit.CoreFileKit': { fileIo }, 'libentry.so': { default: plugin },
    './Config': { hostStorage: () => paths },
  }, { Date: Clock });
  const file = (kind, pid = currentPid) => path.join(directory, pid === null ? `godot-${kind}.log` : `godot-${pid}-${kind}.log`);
  const put = (kind, pid, text, time = startSeconds + 1) => {
    const target = file(kind, pid);
    fs.writeFileSync(target, text);
    fs.utimesSync(target, time, time);
    return target;
  };
  return {
    diagnostics, plugin, paths, context, file, put, reads, failures,
    init: () => diagnostics.initDiagnostics(context, paths),
    advance: (milliseconds) => { now += milliseconds; },
    scans: () => scanned,
    assertClosed: () => assert.equal(openFiles.size, 0, 'Diagnostics must close every log descriptor'),
  };
}

function assertAbsent(text, kind) {
  assert.match(text, new RegExp(`${kind} log: nothing recorded for this session yet`));
  assert.ok(text.includes(`pid=${currentPid}`));
}

function testSetupFailure(root) {
  for (const scenario of ['no-files', 'legacy-only', 'other-pid-only', 'both']) {
    const f = fixture(root, scenario);
    const history = [];
    for (const kind of ['engine', 'crash']) {
      if (scenario === 'legacy-only' || scenario === 'both') {
        history.push([f.put(kind, null, `LEGACY dda70 ${kind}`, startSeconds - 86400), `LEGACY dda70 ${kind}`]);
      }
      if (scenario === 'other-pid-only' || scenario === 'both') {
        // Even a newer, live independent process is not this session.
        history.push([f.put(kind, otherPid, `OTHER PID ${kind}`, startSeconds + 100), `OTHER PID ${kind}`]);
      }
    }
    f.init();
    f.diagnostics.setStep('host:start-engine');
    f.diagnostics.fail(Object.assign(new Error('plugin.setup argument check failed'), { code: 401 }));
    const engine = f.diagnostics.readEngineLog();
    const crash = f.diagnostics.readCrashLog();
    assertAbsent(engine, 'engine');
    assertAbsent(crash, 'crash');
    assert.equal(f.scans(), 0, 'Display selection must not scan for a newest log');
    const full = f.diagnostics.readFullDiagnostics();
    assert.match(full, /failed in step 'host:start-engine'.*plugin.setup argument check failed/);
    assertAbsent(full, 'engine');
    assertAbsent(full, 'crash');
    assert.ok(!full.includes('engine thread started'));
    assert.ok(!full.includes('LEGACY dda70'));
    assert.ok(!full.includes('OTHER PID'));
    assert.ok(f.reads.every((read) => read.file === f.diagnostics.diagnosticsPath()), 'No historical content is read');
    for (const [file, text] of history) {
      assert.equal(fs.readFileSync(file, 'utf8'), text, 'History must not be deleted or rewritten');
    }
    f.assertClosed();
  }
  console.log('PASS pre-engine setup failure: absent current PID; legacy dda70/other PID never substituted; history preserved');
}

function testPidReuse(root) {
  for (const nanoseconds of [true, false]) {
    for (const age of [-86400, 0]) {
      const f = fixture(root, `pid-reuse-${nanoseconds}-${age}`, nanoseconds);
      for (const kind of ['engine', 'crash']) {
        f.put(kind, currentPid, `OLD REUSED PID ${kind}`, startSeconds + age);
      }
      f.init();
      assertAbsent(f.diagnostics.readEngineLog(), 'engine');
      assertAbsent(f.diagnostics.readCrashLog(), 'crash');
      assert.ok(!f.diagnostics.readFullDiagnostics().includes('OLD REUSED PID'));
      // The history stays on disk; repeated init must not move the session cutoff.
      for (const kind of ['engine', 'crash']) {
        assert.equal(fs.readFileSync(f.file(kind), 'utf8'), `OLD REUSED PID ${kind}`);
        f.put(kind, currentPid, `NEW CURRENT ${kind}`, startSeconds + 1);
      }
      f.advance(3000);
      f.init();
      assert.match(f.diagnostics.readEngineLog(), /NEW CURRENT engine/);
      assert.match(f.diagnostics.readCrashLog(), /NEW CURRENT crash/);
      f.assertClosed();
    }
  }
  // With ns metadata even same-size rewrites within the startup second can be
  // distinguished. Without ns an unchanged file remains conservatively hidden.
  const f = fixture(root, 'subsecond-rewrite');
  f.put('engine', currentPid, 'OLD', startSeconds + 0.1);
  f.init();
  assertAbsent(f.diagnostics.readEngineLog(), 'engine');
  f.put('engine', currentPid, 'NEW', startSeconds + 0.75);
  assert.match(f.diagnostics.readEngineLog(), /\nNEW$/);
  // An old same-PID file discovered after initialization also fails the cutoff.
  const late = fixture(root, 'late-old-file');
  late.init();
  late.put('engine', currentPid, 'OLD LATE FILE', startSeconds - 1);
  assertAbsent(late.diagnostics.readEngineLog(), 'engine');
  console.log('PASS session isolation: PID reuse including same-second history; seconds/ns units; idempotent init');
}

function testCurrentLogs(root) {
  const f = fixture(root, 'current-logs');
  assert.match(f.diagnostics.readEngineLog(), /not initialised yet/);
  assert.match(f.diagnostics.readCrashLog(), /not initialised yet/);
  f.init();
  // The first new log may be written in the same Unix second as initialization.
  f.put('engine', currentPid, 'CURRENT ENGINE', startSeconds + 0.75);
  f.put('crash', currentPid, '', startSeconds + 1);
  f.put('engine', otherPid, 'NEWER OTHER ENGINE', startSeconds + 100);
  f.put('crash', otherPid, 'NEWER OTHER CRASH', startSeconds + 100);
  f.put('engine', null, 'NEWER LEGACY ENGINE', startSeconds + 200);
  assert.match(f.diagnostics.readEngineLog(), /current session, pid=101.*\nCURRENT ENGINE/s);
  assertAbsent(f.diagnostics.readCrashLog(), 'crash');
  f.put('crash', currentPid, 'CURRENT CRASH BACKTRACE', startSeconds + 2);
  const full = f.diagnostics.readFullDiagnostics();
  assert.match(full, /CURRENT ENGINE/);
  assert.match(full, /CURRENT CRASH BACKTRACE/);
  assert.ok(!full.includes('NEWER OTHER'));
  assert.ok(!full.includes('NEWER LEGACY'));
  const long = 'OMITTED HEAD\n' + 'x'.repeat(70 * 1024) + '\nCURRENT TAIL';
  f.put('engine', currentPid, long, startSeconds + 3);
  const tail = f.diagnostics.readEngineLog();
  assert.ok(!tail.includes('OMITTED HEAD'));
  assert.ok(tail.endsWith('CURRENT TAIL'));
  assert.deepEqual(f.reads.at(-1), { file: f.file('engine'), offset: Buffer.byteLength(long) - 65536, length: 65536 });
  f.failures.add(f.file('engine'));
  assert.match(f.diagnostics.readEngineLog(), /cannot read current session log.*fixture stat failure/);
  assert.ok(!f.diagnostics.readFullDiagnostics().includes('NEWER OTHER'));
  f.assertClosed();
  console.log('PASS current PID: fresh/empty logs, no newer foreign fallback, bounded 64 KiB tail, read errors stay best effort');
}

function testAttachedChildDisplay(root) {
  const f = fixture(root, 'attached-child-display');
  f.put('crash', childPid, 'OLD CHILD BACKTRACE', startSeconds - 1);
  const oldEngine = f.put('engine', childPid, 'OLD CHILD ENGINE', startSeconds - 1);
  f.put('crash', otherPid, 'FOREIGN BACKTRACE', startSeconds + 100);
  f.put('crash', null, 'LEGACY BACKTRACE', startSeconds + 200);
  f.init();
  f.put('engine', currentPid, 'PARENT ENGINE');
  f.put('crash', currentPid, 'PARENT BACKTRACE');
  const old = f.diagnostics.readFullDiagnostics(childPid, startSeconds - 10);
  assert.match(old, /attached child crash log \(pid=303\): nothing recorded for this session/);
  assert.ok(!old.includes('OLD CHILD BACKTRACE'), 'Child log predating parent session must be rejected even above baseline');
  f.put('crash', childPid, 'NEW CHILD BACKTRACE', startSeconds);
  const equal = f.diagnostics.readFullDiagnostics(childPid, startSeconds);
  assert.ok(!equal.includes('NEW CHILD BACKTRACE'), 'Child mtime must be strictly newer than crash baseline');
  assert.ok(!f.diagnostics.readFullDiagnostics(childPid, startSeconds + 1).includes('NEW CHILD BACKTRACE'));
  const full = f.diagnostics.readFullDiagnostics(childPid, startSeconds - 1);
  assert.match(full, /===== engine log =====\n.*current session, pid=101.*\nPARENT ENGINE/);
  assert.match(full, /===== crash log =====\n.*current session, pid=101.*\nPARENT BACKTRACE/);
  assert.match(full, /===== attached child crash log \(pid=303\) =====\n.*attached child, pid=303.*\nNEW CHILD BACKTRACE/);
  assert.ok(!full.includes('OLD CHILD ENGINE'), 'Explicit child crash display must never substitute child engine logs');
  assert.ok(!full.includes('FOREIGN BACKTRACE'));
  assert.ok(!full.includes('LEGACY BACKTRACE'));
  const defaultLog = f.diagnostics.readFullDiagnostics();
  assert.ok(!defaultLog.includes('attached child crash log'));
  assert.ok(!defaultLog.includes('NEW CHILD BACKTRACE'), 'Default view must remain parent-session only');
  assert.ok(!defaultLog.includes('FOREIGN BACKTRACE'));
  assert.ok(!defaultLog.includes('LEGACY BACKTRACE'));
  assert.ok(!f.diagnostics.readFullDiagnostics(505).includes('NEW CHILD BACKTRACE'), 'Missing child must not fall back');
  f.put('crash', childPid, '', startSeconds + 1);
  assert.match(f.diagnostics.readFullDiagnostics(childPid), /attached child crash log \(pid=303\): nothing recorded/);
  f.failures.add(f.file('crash', childPid));
  assert.match(f.diagnostics.readFullDiagnostics(childPid), /attached child crash log \(pid=303\): cannot read.*fixture stat failure/);
  const allowed = [f.diagnostics.diagnosticsPath(), f.file('engine'), f.file('crash'), f.file('crash', childPid)];
  assert.ok(f.reads.every((read) => allowed.includes(read.file)), 'Only explicit child crash content may be read in addition to parent logs');
  assert.equal(fs.readFileSync(oldEngine, 'utf8'), 'OLD CHILD ENGINE');
  f.assertClosed();
  console.log('PASS explicit attached child: labeled fresh crash only; parent headers preserved; session/baseline/empty/missing/error guards; default stays isolated');
}

function testCrashMonitoring(root) {
  const f = fixture(root, 'crash-monitoring');
  f.put('crash', null, 'LEGACY BASELINE', startSeconds - 20);
  f.put('crash', childPid, 'OLD CHILD CRASH', startSeconds - 30);
  f.put('crash', otherPid, 'OLD INDEPENDENT CRASH', startSeconds - 10);
  f.put('crash', 404, '', startSeconds + 100);
  f.init();
  assert.equal(f.diagnostics.crashLogTime(), startSeconds - 10, 'Baseline must include historical other-PID crashes');
  assert.equal(f.diagnostics.crashLogTime(childPid), startSeconds - 30);
  assert.equal(f.diagnostics.crashLogTime(404), 0, 'Empty crash files do not indicate a fatal signal');
  assert.equal(f.diagnostics.crashLogTime(505), 0, 'Missing attached PID must not fall back to another PID');
  assertAbsent(f.diagnostics.readCrashLog(), 'crash');

  // Exercise the actual attached-child poller as in test_arkts_host.cjs: exclude
  // only ArkUI build()/decorators, not updateEngineState or crashLogTime logic.
  const { Index } = load('pages/Index.ets', {
    'libentry.so': { default: f.plugin }, '@kit.InputKit': { KeyCode: {} }, './KeyMap': { mapKeyCode: () => 0 },
    '../runtime/Diagnostics': f.diagnostics, '../runtime/Config': {}, '../runtime/Permissions': {}, '../runtime/Runtime': {},
    '../runtime/NativeArguments': {}, '../runtime/Templates': {},
    // This fixture exercises the crash poller, not input (covered separately).
    '../runtime/TouchInput': { TouchInput: class {} },
    '../runtime/Launch': {
      spawnedChildPid: () => childPid, childHasExited: () => false, logInstanceState: () => {}, reportExit: () => {},
    },
  }, {
    XComponentController: class {}, AppStorage: { get: () => false }, setInterval: () => 1, clearInterval: () => {},
  }, (source) => {
    assert.ok(source.includes('  build() {'), 'Index structure changed; update the logic-only transformation');
    return (source.slice(0, source.indexOf('  build() {')) + '}\n')
      .replace('@Entry\n@Component\nstruct Index', 'export class Index').replaceAll('@State ', '');
  });
  const page = new Index();
  page.instanceSupport = true;
  page.crashTime = f.diagnostics.crashLogTime();
  page.updateEngineState(f.context);
  assert.equal(page.crashed, false, 'Historical child crash must not trigger');
  f.put('crash', otherPid, 'NEW INDEPENDENT CRASH', startSeconds + 1);
  page.updateEngineState(f.context);
  assert.equal(page.crashed, false, 'An independent instance crash must not trigger the attached-child monitor');
  f.put('crash', childPid, 'NEW ATTACHED CHILD CRASH', startSeconds + 2);
  assert.equal(f.diagnostics.crashLogTime(childPid), startSeconds + 2);
  page.updateEngineState(f.context);
  assert.equal(page.crashed, true, 'New attached-child crash must still trigger');
  assert.equal(page.showLog, true);
  assert.match(page.logText, /attached child crash detected pid=303/);
  assertAbsent(page.logText, 'crash');
  assert.match(page.logText, /===== attached child crash log \(pid=303\) =====/);
  assert.match(page.logText, /NEW ATTACHED CHILD CRASH/, 'The detected attached-child crash body must remain available');
  assert.ok(!f.diagnostics.readFullDiagnostics().includes('NEW ATTACHED CHILD CRASH'), 'Default display still belongs to the parent session');
  assert.equal(f.diagnostics.crashLogTime(), startSeconds + 2);
  f.failures.add(f.file('crash', childPid));
  assert.equal(f.diagnostics.crashLogTime(childPid), 0, 'Monitoring stat errors remain best effort');
  const legacy = fixture(root, 'legacy-baseline');
  legacy.put('crash', null, 'LEGACY ONLY BASELINE', startSeconds - 5);
  legacy.init();
  assert.equal(legacy.diagnostics.crashLogTime(), startSeconds - 5, 'Legacy history is still a monitoring baseline, not display content');
  assertAbsent(legacy.diagnostics.readCrashLog(), 'crash');
  f.assertClosed();
  legacy.assertClosed();
  console.log('PASS crash monitoring: multi-process/legacy baseline; empty/missing/error cases; real Index ignores independent crashes and detects attached child');
}

function main() {
  if (!process.env.TMPDIR) { throw new Error('TMPDIR must name an existing temporary directory'); }
  const temporary = fs.mkdtempSync(path.join(process.env.TMPDIR, 'godot-diagnostics-tests-'));
  try {
    console.log('ArkTS diagnostics logic tests (SDK transpile + mocked APIs; NOT CompileArkTS or HAP/device acceptance)');
    testSetupFailure(temporary);
    testPidReuse(temporary);
    testCurrentLogs(temporary);
    testAttachedChildDisplay(temporary);
    testCrashMonitoring(temporary);
    console.log('PASS all host diagnostics regression tests');
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

try { main(); } catch (error) { console.error(error); process.exitCode = 1; }
