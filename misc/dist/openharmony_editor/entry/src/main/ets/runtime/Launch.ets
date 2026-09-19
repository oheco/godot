// Godot Engine contributors. SPDX-License-Identifier: MIT
import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import plugin from 'libentry.so';
import { ensureDirectory, exists } from './FileSystem';

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
  try {
    await context.startAbility({
      bundleName: context.abilityInfo.bundleName, abilityName: 'EntryAbility',
      parameters: { godotArguments: args, godotLaunchKey: key }
    });
    const deadline = Date.now() + 20000;
    while (Date.now() < deadline) {
      if (exists(response)) {
        const pid = Number(fs.readTextSync(response));
        fs.unlinkSync(response);
        plugin.spawnResult(request, Number.isInteger(pid) && pid > 0 ? pid : -1);
        return;
      }
      await new Promise<void>((resolve) => { setTimeout(resolve, 50); });
    }
  } catch (error) {
    console.error(`Godot process launch failed: ${error.message}`);
  }
  plugin.spawnResult(request, -1);
}
