// Godot Engine contributors. SPDX-License-Identifier: MIT
import { common, contextConstant, wantConstant } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import plugin from 'libentry.so';
import { describe, record } from './Diagnostics';
import { ensureDirectory, exists } from './FileSystem';

// Only parent-attached launches need the old keep-alive behavior. An independent
// editor instance must never make its project manager stay alive.
let childPid: number = 0;

interface LaunchResponse {
  pid: number;
  instanceKey: string;
  phase: string;
  error: string;
}

export function spawnedChildPid(): number {
  return childPid;
}

export function logInstanceState(context: common.UIAbilityContext, event: string): void {
  const pid: number = plugin.processId();
  try {
    const application = context.getApplicationContext();
    const key: string = application.getCurrentInstanceKey();
    record(`instance event=${event} pid=${pid} key='${key}'`);
    application.getAllRunningInstanceKeys().then((keys: string[]) => {
      record(`instance event=${event} pid=${pid} runningKeys=${JSON.stringify(keys)}`);
    }).catch((error: BusinessError) => {
      record(`instance list failed: ${describe(error)}`);
    });
  } catch (error) {
    record(`instance event=${event} pid=${pid} query failed: ${describe(error as BusinessError)}`);
  }
}

// Do not mistake game/user arguments after the separator for engine switches.
export function hasEngineFlag(args: string[], longFlag: string, shortFlag: string): boolean {
  for (const argument of args) {
    if (argument === '--' || argument === '++') { break; }
    if (argument === longFlag || argument === shortFlag) { return true; }
  }
  return false;
}

// A process that quits on purpose leaves this marker behind; a process killed by
// a fatal signal cannot. This is retained for parent-attached launches only.
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
    // Best effort: a missing marker only delays an attached parent's own exit.
  }
}

export function childHasExited(context: common.UIAbilityContext, pid: number): boolean {
  return pid > 0 && exists(exitMarker(context, pid));
}

function publishLaunchResponse(context: common.UIAbilityContext, key: string, phase: string, error: string): void {
  if (!/^[0-9]+-[0-9]+-[0-9]+$/.test(key)) { return; }
  try {
    const directory = `${context.cacheDir}/godot-launch`;
    ensureDirectory(directory);
    const pending = `${directory}/${key}.tmp`;
    const response = `${directory}/${key}`;
    const result: LaunchResponse = {
      pid: plugin.processId(),
      instanceKey: context.getApplicationContext().getCurrentInstanceKey(),
      phase: phase,
      error: error
    };
    const file = fs.openSync(pending, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
    try { fs.writeSync(file.fd, JSON.stringify(result)); } finally { fs.closeSync(file); }
    fs.renameSync(pending, response);
    record(`handoff key='${key}' phase=${phase} pid=${result.pid} instance='${result.instanceKey}'`);
  } catch (cause) {
    record(`handoff key='${key}' write failed: ${describe(cause as BusinessError)}`);
  }
}

// Attached launches preserve their previous onCreate acknowledgement. Independent
// editors acknowledge as soon as their window content loads, without waiting for
// Godot's native initialization or project loading.
export function reportStarted(context: common.UIAbilityContext, key: string): void {
  publishLaunchResponse(context, key, 'ability-created', '');
}

export function reportWindowReady(context: common.UIAbilityContext, key: string): void {
  publishLaunchResponse(context, key, 'window-ready', '');
}

export function reportLaunchFailure(context: common.UIAbilityContext, key: string, error: string): void {
  publishLaunchResponse(context, key, 'failed', error);
}

export async function launchInstance(context: common.UIAbilityContext, request: number, args: string[], fromProjectManager: boolean): Promise<void> {
  const parentPid: number = plugin.processId();
  const key = `${parentPid}-${request}-${Date.now()}`;
  const response = `${context.cacheDir}/godot-launch/${key}`;
  // Limit the behavior change to PM -> editor. Running games and other existing
  // launches retain their parent-attached process mode.
  const independent: boolean = fromProjectManager && hasEngineFlag(args, '--editor', '-e');
  record(`spawn ${request}: mode=${independent ? 'multiInstance' : 'attached'} args=[${args.join(' ')}] key='${key}'`);
  try {
    const parentInstance: string = context.getApplicationContext().getCurrentInstanceKey();
    logInstanceState(context, `spawn-${request}-before`);
    // Must fit inside the native bridge's 30-second callback wait. This deadline
    // includes startAbility and the window-ready handshake, not just polling.
    const deadline = Date.now() + 20000;
    if (independent) {
      // Fresh Want: CREATE_APP_INSTANCE_KEY and APP_INSTANCE_KEY are mutually
      // exclusive. Do not carry the caller's instance key or any processMode.
      await context.startAbility({
        bundleName: context.abilityInfo.bundleName,
        abilityName: 'EditorAbility',
        parameters: {
          [wantConstant.Params.CREATE_APP_INSTANCE_KEY]: true,
          godotArguments: args,
          godotLaunchKey: key,
          godotIndependentInstance: true
        }
      });
    } else {
      await context.startAbility({
        bundleName: context.abilityInfo.bundleName, abilityName: 'EditorAbility',
        parameters: { godotArguments: args, godotLaunchKey: key }
      }, {
        processMode: contextConstant.ProcessMode.NEW_PROCESS_ATTACH_TO_PARENT,
        startupVisibility: contextConstant.StartupVisibility.STARTUP_SHOW
      });
    }
    record(`spawn ${request}: startAbility returned`);
    while (Date.now() < deadline) {
      if (exists(response)) {
        const info = fs.statSync(response);
        if (!info.isFile() || info.size <= 0 || info.size > 16384) {
          throw new Error('Invalid launch response file');
        }
        const result: LaunchResponse = JSON.parse(fs.readTextSync(response, { length: info.size })) as LaunchResponse;
        fs.unlinkSync(response);
        if (!result || !Number.isInteger(result.pid) || result.pid <= 0 || result.pid === parentPid ||
          typeof result.instanceKey !== 'string' || typeof result.phase !== 'string') {
          throw new Error('Invalid launch response');
        }
        if (result.phase === 'failed') {
          throw new Error(`Target startup failed: ${result.error}`);
        }
        if (independent && (result.phase !== 'window-ready' || result.instanceKey.length === 0 || result.instanceKey === parentInstance)) {
          throw new Error('Target did not confirm a ready window in an independent application instance');
        }
        if (!independent && result.phase !== 'ability-created') {
          throw new Error('Unexpected attached launch response');
        }
        record(`spawn ${request}: handoff accepted mode=${independent ? 'multiInstance' : 'attached'} parentPid=${parentPid} parentInstance='${parentInstance}' targetPid=${result.pid} targetInstance='${result.instanceKey}' phase=${result.phase}`);
        if (!independent) { childPid = result.pid; }
        // Godot quits its project manager only after create_instance succeeds.
        plugin.spawnResult(request, result.pid);
        return;
      }
      await new Promise<void>((resolve) => { setTimeout(resolve, 50); });
    }
    record(`spawn ${request}: no valid startup acknowledgement within 20s; caller stays open`);
  } catch (error) {
    const cause = error as BusinessError;
    record(`spawn ${request} failed: ${describe(cause)}`);
    console.error(`Godot process launch failed: ${cause.message}`);
  }
  plugin.spawnResult(request, -1);
}
