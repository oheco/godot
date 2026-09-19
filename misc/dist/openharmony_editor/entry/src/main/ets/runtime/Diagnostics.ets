// Godot Engine contributors. SPDX-License-Identifier: MIT
// Records a diagnostic trace the user can read and copy out of the running
// application. Nothing in this file may throw: failing to record a diagnostic
// must never replace the original error.
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo as fs } from '@kit.CoreFileKit';

let logFile: string = '';
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
