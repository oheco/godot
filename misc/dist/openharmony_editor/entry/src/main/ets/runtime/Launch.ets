// Godot Engine contributors. SPDX-License-Identifier: MIT
import { common, contextConstant } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import plugin from 'libentry.so';
import { describe, record } from './Diagnostics';
import { ensureDirectory, exists } from './FileSystem';

// Pid of the process this one started, 0 when it started none.
let childPid: number = 0;

export function spawnedChildPid(): number {
  return childPid;
}

// A process that quits on purpose leaves this marker behind; a process killed by
// a fatal signal cannot, which is how a surviving parent tells the two apart.
function exitMarker(context: common.UIAbilityContext, pid: number): string {
  return `${context.cacheDir}/godot-launch/exited-${pid}`;
}

export function reportExit(context: common.UIAbilityContext): void {
  try {
    const directory = `${context.cacheDir}/godot-launch`;
    ensureDirectory(directory);
    const file = fs.openSync(exitMarker(context, plugin.processId()), fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
    fs.closeSync(file);
  } catch (ignored) {
    // Best effort: a missing marker only delays the parent's own exit.
  }
}

export function childHasExited(context: common.UIAbilityContext, pid: number): boolean {
  return pid > 0 && exists(exitMarker(context, pid));
}

export function reportStarted(context: common.UIAbilityContext, key: string): void {
  if (!/^[0-9]+-[0-9]+$/.test(key)) { return; }
  const directory = `${context.cacheDir}/godot-launch`;
  ensureDirectory(directory);
  const pending = `${directory}/${key}.tmp`;
  const response = `${directory}/${key}`;
  const file = fs.openSync(pending, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
  try { fs.writeSync(file.fd, String(plugin.processId())); } finally { fs.closeSync(file); }
  if (exists(response)) { fs.unlinkSync(response); }
  fs.renameSync(pending, response);
}

export async function launchInstance(context: common.UIAbilityContext, request: number, args: string[]): Promise<void> {
  const key = `${plugin.processId()}-${request}`;
  const response = `${context.cacheDir}/godot-launch/${key}`;
  record(`spawn ${request}: request args=[${args.join(' ')}] key='${key}'`);
  try {
    // The engine asked for another Godot instance (opening a project from the
    // project manager, or restarting itself). EditorAbility is multiton, so this
    // starts a new ability instance in its own process; the child reports its pid
    // through the cache directory and the engine waits for it.
    await context.startAbility({
      bundleName: context.abilityInfo.bundleName, abilityName: 'EditorAbility',
      parameters: { godotArguments: args, godotLaunchKey: key }
    }, {
      processMode: contextConstant.ProcessMode.NEW_PROCESS_ATTACH_TO_PARENT,
      startupVisibility: contextConstant.StartupVisibility.STARTUP_SHOW
    });
    record(`spawn ${request}: startAbility returned`);
    const deadline = Date.now() + 20000;
    while (Date.now() < deadline) {
      if (exists(response)) {
        const pid = Number(fs.readTextSync(response));
        fs.unlinkSync(response);
        record(`spawn ${request}: child pid ${pid}`);
        childPid = Number.isInteger(pid) && pid > 0 ? pid : 0;
        plugin.spawnResult(request, childPid > 0 ? childPid : -1);
        return;
      }
      await new Promise<void>((resolve) => { setTimeout(resolve, 50); });
    }
    record(`spawn ${request}: no child reported its pid within 20s`);
  } catch (error) {
    const cause = error as BusinessError;
    record(`spawn ${request} failed: ${describe(cause)}`);
    console.error(`Godot process launch failed: ${cause.message}`);
  }
  plugin.spawnResult(request, -1);
}
