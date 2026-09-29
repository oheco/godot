// Godot Engine contributors. SPDX-License-Identifier: MIT
// Records a diagnostic trace the user can read and copy out of the running
// application. Nothing in this file may throw: failing to record a diagnostic
// must never replace the original error.
//
// Every process writes private files named after its own pid, so an engine that
// crashes cannot overwrite the log of the process that started it. No developer
// machine paths or external storage permissions are needed for diagnostics.
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import plugin from 'libentry.so';
import { hostStorage, HostStoragePaths } from './Config';

let logFile: string = '';
let directory: string = '';
let sessionPid: number = 0;
let sessionStartTime: number = 0;
let engineLogBaseline: fs.Stat | undefined = undefined;
let crashLogBaseline: fs.Stat | undefined = undefined;
let storagePaths: HostStoragePaths | undefined = undefined;
let step: string = 'startup';
let verbose: boolean = false;

export function setDiagnosticsLevel(level: string): void {
  verbose = level === 'verbose';
  record(`diagnostics level=${level}`);
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

export function initDiagnostics(context: common.UIAbilityContext, paths: HostStoragePaths = hostStorage(context)): void {
  if (logFile.length > 0) { return; }
  storagePaths = paths;
  directory = paths.filesDir;
  sessionPid = plugin.processId();
  // CoreFileKit Stat.mtime is Unix seconds, whereas Date.now() is milliseconds.
  sessionStartTime = Math.floor(Date.now() / 1000);
  engineLogBaseline = existingLogInfo(engineLogPath(sessionPid));
  crashLogBaseline = existingLogInfo(crashLogPath(sessionPid));
  logFile = `${directory}/godot-${sessionPid}-diagnostics.log`;
  record('--- session ---');
  record(`log directory=${directory}`);
  record(`filesDir=${paths.filesDir}`);
  record(`cacheDir=${paths.cacheDir}`);
  record(`tempDir=${paths.tempDir}`);
  record(`bundle=${context.abilityInfo.bundleName} module=${context.abilityInfo.moduleName}`);
  record(`crash log=${crashLogPath(sessionPid)}`);
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
  if (!verbose) { return; }
  const paths = storagePaths ?? hostStorage(context);
  record('--- environment probe ---');
  record(inspect(`${paths.filesDir}/Projects`));
  record(inspect(`${paths.filesDir}/config`));
  record(inspect(`${paths.filesDir}/nuget`));
  record(inspect(`${paths.filesDir}/dotnet-cli`));
  record(inspect(`${paths.cacheDir}/tmp`));
  record(listDirectory(`${paths.filesDir}/runtimes`));
  record(listDirectory(paths.filesDir));
}

export function readDiagnostics(): string {
  if (logFile.length === 0) { return 'diagnostics are not initialised yet'; }
  try {
    if (!fs.accessSync(logFile)) { return `no diagnostics written yet (${logFile})`; }
    return readTail(logFile, 'host diagnostics');
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

// Every engine process creates its crash file when it starts, so only a file
// with content means that a process actually died from a fatal signal.
function newestLog(kind: string, contentOnly: boolean = false): string {
  try {
    if (directory.length === 0) { return ''; }
    const suffix = `-${kind}.log`;
    let newest = '';
    let newestTime = -1;
    for (const name of fs.listFileSync(directory)) {
      if (!name.startsWith('godot-') || !name.endsWith(suffix)) { continue; }
      const path = `${directory}/${name}`;
      const info = fs.statSync(path);
      if (contentOnly && info.size === 0) { continue; }
      if (info.mtime > newestTime) { newestTime = info.mtime; newest = path; }
    }
    return newest;
  } catch (ignored) {
    return '';
  }
}

function existingLogInfo(path: string): fs.Stat | undefined {
  try { return fs.statSync(path); } catch (ignored) { return undefined; }
}

// Display only this process/session, never the newest legacy or other-PID log.
// Keep this separate from newestLog(): crash monitoring needs its historical,
// multi-process baseline. Remember pre-existing files as well as startup time
// so an unchanged log from a reused PID in the same second is not shown.
function readSessionLog(kind: string, baseline: fs.Stat | undefined): string {
  const label = `${kind} log`;
  if (directory.length === 0) { return `${label}: diagnostics are not initialised yet`; }
  const path = kind === 'engine' ? engineLogPath(sessionPid) : crashLogPath(sessionPid);
  const absent = `${label}: nothing recorded for this session yet (pid=${sessionPid}, ${path})`;
  try {
    if (!fs.accessSync(path)) { return absent; }
    const info = fs.statSync(path);
    if (info.size === 0 || info.mtime < sessionStartTime) { return absent; }
    // mtimeNs is optional (API 15+); without it, indistinguishable same-second,
    // same-size rewrites are conservatively hidden rather than showing history.
    if (baseline !== undefined && info.mtime === baseline.mtime && info.size === baseline.size &&
      info.mtimeNs === baseline.mtimeNs) { return absent; }
    return `--- ${path} (current session, pid=${sessionPid}) ---\n${readTail(path, label)}`;
  } catch (error) {
    return `${label}: cannot read current session log ${path}: ${describe(error as BusinessError)}`;
  }
}

export function readEngineLog(): string {
  return readSessionLog('engine', engineLogBaseline);
}

export function readCrashLog(): string {
  return readSessionLog('crash', crashLogBaseline);
}

// With a PID, watch only that parent-attached child: independent application
// instances must not react to each other's crashes. Without a PID, take the
// initial timestamp baseline from the newest existing crash log.
export function crashLogTime(pid: number = 0): number {
  const path: string = pid > 0 ? crashLogPath(pid) : newestLog('crash', true);
  if (path.length === 0) { return 0; }
  try {
    const info = fs.statSync(path);
    return info.size > 0 ? info.mtime : 0;
  } catch (ignored) {
    return 0;
  }
}

// Only the caller that detected an attached child's new crash may request its
// backtrace. Never scan for a child or substitute its old engine output.
function readAttachedChildCrashLog(pid: number, baseline: number): string {
  const label = `attached child crash log (pid=${pid})`;
  if (directory.length === 0) { return `${label}: diagnostics are not initialised yet`; }
  const path = crashLogPath(pid);
  const absent = `${label}: nothing recorded for this session after baseline=${baseline} (${path})`;
  try {
    if (!fs.accessSync(path)) { return absent; }
    const info = fs.statSync(path);
    if (info.size === 0 || info.mtime < sessionStartTime || info.mtime <= baseline) { return absent; }
    return `--- ${path} (attached child, pid=${pid}) ---\n${readTail(path, label)}`;
  } catch (error) {
    return `${label}: cannot read ${path}: ${describe(error as BusinessError)}`;
  }
}

// Engine output and native backtrace default to this process/session. A caller
// may explicitly append a new attached-child crash without replacing parent logs.
// The directory listing may include history, but is never a log-content fallback.
export function readFullDiagnostics(childPid: number = 0, childCrashBaseline: number = 0): string {
  const childLog = childPid > 0 ? `===== attached child crash log (pid=${childPid}) =====\n` +
    `${readAttachedChildCrashLog(childPid, childCrashBaseline)}\n\n` : '';
  return `${readDiagnostics()}\n\n===== engine log =====\n${readEngineLog()}\n\n` +
    `===== crash log =====\n${readCrashLog()}\n\n${childLog}===== log directory =====\n${listDirectory(directory)}`;
}
