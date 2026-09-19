// Godot Engine contributors. SPDX-License-Identifier: MIT
// Records a diagnostic trace the user can read and copy out of the running
// application. Nothing in this file may throw: failing to record a diagnostic
// must never replace the original error.
import { abilityAccessCtrl, bundleManager, common, Permissions } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo as fs } from '@kit.CoreFileKit';

let logFile: string = '';
let engineLogFile: string = '';
let crashLogFile: string = '';
let step: string = 'startup';

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
  logFile = `${context.filesDir}/godot-diagnostics.log`;
  // The engine writes its own output to the first file (see --log-file) and the
  // native crash handler writes to the second one. Both survive the editor
  // process, which is why the launcher can still report them after a crash.
  engineLogFile = `${context.filesDir}/godot-engine.log`;
  crashLogFile = `${context.filesDir}/godot-crash.log`;
  record('--- session ---');
  record(`filesDir=${context.filesDir}`);
  record(`cacheDir=${context.cacheDir}`);
  record(`tempDir=${context.tempDir}`);
  record(`bundle=${context.abilityInfo.bundleName} module=${context.abilityInfo.moduleName}`);
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

// Reads only the tail of a file the engine process wrote, so a long engine log
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

export function readEngineLog(): string {
  return readTail(engineLogFile, 'engine log');
}

function logHasContent(path: string): boolean {
  try {
    return path.length > 0 && fs.accessSync(path) && fs.statSync(path).size > 0;
  } catch (ignored) {
    return false;
  }
}

// The crash log of the run before the current one is kept beside it, so clicking
// "打开项目管理器" again after a crash does not destroy the evidence.
export function readCrashLog(): string {
  if (crashLogFile.length === 0) { return 'crash log: not initialised yet'; }
  return `${readTail(`${crashLogFile}.previous`, 'previous run')}\n${readTail(crashLogFile, 'last run')}`;
}

export function hasCrashLog(): boolean {
  return logHasContent(crashLogFile) || logHasContent(`${crashLogFile}.previous`);
}

export function clearCrashLog(): void {
  if (crashLogFile.length === 0) { return; }
  const previous = `${crashLogFile}.previous`;
  try {
    if (fs.accessSync(previous)) { fs.unlinkSync(previous); }
    if (fs.accessSync(crashLogFile)) { fs.renameSync(crashLogFile, previous); }
  } catch (ignored) {
    // Best effort: a stale crash log only adds a hint to the launcher.
  }
}

// Everything the application can observe about the last engine start, including
// the engine's own output and the native backtrace of a crashed engine process.
export function readFullDiagnostics(): string {
  return `${readDiagnostics()}\n\n===== engine log (${engineLogFile}) =====\n${readEngineLog()}\n\n` +
    `===== crash log (${crashLogFile}) =====\n${readCrashLog()}`;
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
