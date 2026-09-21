// Godot Engine contributors. SPDX-License-Identifier: MIT
// Records a diagnostic trace the user can read and copy out of the running
// application. Nothing in this file may throw: failing to record a diagnostic
// must never replace the original error.
//
// Every process writes files named after its own pid, so an engine that crashes
// cannot overwrite the log of the process that started it. The project manager
// exits as soon as it hands a project to a new editor process, so during
// development the files also go to a directory outside the application sandbox:
// there is otherwise nothing left in the application that could show them.
import { abilityAccessCtrl, bundleManager, common, Permissions } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import plugin from 'libentry.so';

const HOST_DIRECTORY: string = '/storage/Users/currentUser/dev/ohos/godot-logs';

let logFile: string = '';
let directory: string = '';
let step: string = 'startup';

function writableDirectory(context: common.UIAbilityContext): string {
  try {
    if (fs.accessSync(HOST_DIRECTORY)) {
      // Only use it when this application can really write there.
      const probe = `${HOST_DIRECTORY}/.write-probe`;
      const file = fs.openSync(probe, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      fs.closeSync(file);
      fs.unlinkSync(probe);
      return HOST_DIRECTORY;
    }
  } catch (ignored) {
    // A sandbox path is always correct; the host directory is a development aid.
  }
  return context.filesDir;
}

export function describe(error: BusinessError): string {
  const parts: string[] = [];
  parts.push(`name=${error.name}`);
  parts.push(`code=${error.code}`);
  parts.push(`message=${error.message}`);
  // Only stack is optional; the comparisons above would always be true.
  if (error.stack !== undefined) { parts.push(`stack=${error.stack}`); }
  return parts.join(' | ');
}

function append(line: string): void {
  if (logFile.length === 0) { return; }
  try {
    const file = fs.openSync(logFile, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.APPEND);
    try { fs.writeSync(file.fd, line + '\n'); } finally { fs.closeSync(file); }
  } catch (ignored) {
    // Diagnostics are best effort.
  }
}

export function record(message: string): void {
  append(`${new Date().toISOString()} ${message}`);
}

export function setStep(name: string): void {
  step = name;
  record(`== step: ${name}`);
}

export function fail(error: BusinessError): string {
  const detail = describe(error);
  record(`!! failed in step '${step}': ${detail}`);
  return detail;
}

function inspect(path: string): string {
  try {
    if (!fs.accessSync(path)) { return `${path} -> absent`; }
    const info = fs.statSync(path);
    const kind = info.isDirectory() ? 'directory' : (info.isFile() ? 'FILE' : 'other');
    return `${path} -> ${kind} size=${info.size} mode=${info.mode} uid=${info.uid}`;
  } catch (error) {
    return `${path} -> stat failed: ${describe(error as BusinessError)}`;
  }
}

function listDirectory(path: string): string {
  try {
    if (!fs.accessSync(path)) { return `${path} -> absent`; }
    const entries = fs.listFileSync(path);
    const shown = entries.slice(0, 40);
    return `${path} -> ${entries.length} entries: ${shown.join(', ')}${entries.length > shown.length ? ', ...' : ''}`;
  } catch (error) {
    return `${path} -> list failed: ${describe(error as BusinessError)}`;
  }
}

export function initDiagnostics(context: common.UIAbilityContext): void {
  if (logFile.length > 0) { return; }
  directory = writableDirectory(context);
  logFile = `${directory}/godot-${plugin.processId()}-diagnostics.log`;
  record('--- session ---');
  record(`log directory=${directory}`);
  record(`filesDir=${context.filesDir}`);
  record(`cacheDir=${context.cacheDir}`);
  record(`tempDir=${context.tempDir}`);
  record(`bundle=${context.abilityInfo.bundleName} module=${context.abilityInfo.moduleName}`);
  record(`crash log=${crashLogPath(plugin.processId())}`);
}

// The engine writes its stdout/stderr to this path (--log-file) and the native
// fatal-signal handler writes the backtrace next to it (see the bridge).
export function engineLogPath(pid: number): string {
  return `${directory}/godot-${pid}-engine.log`;
}

function crashLogPath(pid: number): string {
  return `${directory}/godot-${pid}-crash.log`;
}

// Dumps the paths whose state decides whether the native runtime configuration
// can succeed: create_directories reports "File exists" when one of them is a
// plain file instead of a directory.
export function probeEnvironment(context: common.UIAbilityContext): void {
  record('--- environment probe ---');
  record(inspect(`${context.filesDir}/Projects`));
  record(inspect(`${context.filesDir}/config`));
  record(inspect(`${context.filesDir}/nuget`));
  record(inspect(`${context.filesDir}/dotnet-cli`));
  record(inspect(`${context.cacheDir}/tmp`));
  record(inspect(`${context.filesDir}/Projects/CSharpSmoke-4.7.2-ohos.2`));
  record(listDirectory(`${context.filesDir}/runtimes`));
  record(listDirectory(context.filesDir));
}

export function readDiagnostics(): string {
  if (logFile.length === 0) { return 'diagnostics are not initialised yet'; }
  try {
    if (!fs.accessSync(logFile)) { return `no diagnostics written yet (${logFile})`; }
    return fs.readTextSync(logFile);
  } catch (error) {
    return `cannot read ${logFile}: ${describe(error as BusinessError)}`;
  }
}

export function diagnosticsPath(): string {
  return logFile;
}

// Reads only the tail of a log the engine process wrote, so a long engine log
// cannot exhaust memory while the panel is built.
function readTail(path: string, label: string): string {
  try {
    if (path.length === 0) { return `${label}: not initialised yet`; }
    if (!fs.accessSync(path)) { return `${label}: nothing recorded yet (${path})`; }
    const size: number = fs.statSync(path).size;
    const limit: number = 64 * 1024;
    const offset: number = size > limit ? size - limit : 0;
    return fs.readTextSync(path, { offset: offset, length: size - offset });
  } catch (error) {
    return `${label}: cannot read ${path}: ${describe(error as BusinessError)}`;
  }
}

function newestLog(kind: string): string {
  try {
    if (directory.length === 0) { return ''; }
    const suffix = `-${kind}.log`;
    let newest = '';
    let newestTime = -1;
    for (const name of fs.listFileSync(directory)) {
      if (!name.startsWith('godot-') || !name.endsWith(suffix)) { continue; }
      const path = `${directory}/${name}`;
      const time: number = fs.statSync(path).mtime;
      if (time > newestTime) { newestTime = time; newest = path; }
    }
    return newest;
  } catch (ignored) {
    return '';
  }
}

export function readEngineLog(): string {
  const newest = newestLog('engine');
  if (newest.length === 0) { return 'engine log: nothing recorded yet'; }
  return `--- ${newest} ---\n${readTail(newest, 'engine log')}`;
}

export function readCrashLog(): string {
  const newest = newestLog('crash');
  if (newest.length === 0) { return 'crash log: nothing recorded yet'; }
  return `--- ${newest} ---\n${readTail(newest, 'crash log')}`;
}

// Modification time of the newest crash log, or 0 when there is none. Callers
// compare it with a value taken earlier to notice a crash that happened after
// they started, which is how a still running process reports a dead one.
export function crashLogTime(): number {
  const newest = newestLog('crash');
  if (newest.length === 0) { return 0; }
  try {
    return fs.statSync(newest).mtime;
  } catch (ignored) {
    return 0;
  }
}

// Everything the application can observe about the engine processes, including
// each engine's own output and the native backtrace of one that crashed.
export function readFullDiagnostics(): string {
  return `${readDiagnostics()}\n\n===== engine log =====\n${readEngineLog()}\n\n` +
    `===== crash log =====\n${readCrashLog()}\n\n===== log directory =====\n${listDirectory(directory)}`;
}

// A permission can be listed in the signing profile's ACL and still be denied at
// runtime, so report what the system actually granted to this application.
export function logPermissionState(): void {
  try {
    const manager = abilityAccessCtrl.createAtManager();
    const info = bundleManager.getBundleInfoForSelfSync(bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION);
    const token: number = info.appInfo.accessTokenId;
    const names: Permissions[] = ['ohos.permission.READ_WRITE_USER_FILE', 'ohos.permission.ALLOW_EXTERNAL_NATIVE_CODE'];
    for (const name of names) {
      record(`granted ${name} = ${manager.checkAccessTokenSync(token, name)} (0 means granted)`);
    }
    record(`application token=${token} debug=${info.appInfo.debug}`);
  } catch (error) {
    record(`permission state check failed: ${describe(error as BusinessError)}`);
  }
}
