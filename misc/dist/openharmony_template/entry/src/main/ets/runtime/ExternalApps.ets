// Godot Engine contributors. SPDX-License-Identifier: MIT
import { common, Want } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileUri } from '@kit.CoreFileKit';
import window from '@ohos.window';
import { describe, record } from './Diagnostics';

async function showUnsupported(context: common.UIAbilityContext, message: string): Promise<void> {
  record(message);
  try {
    const mainWindow = await window.getLastWindow(context);
    mainWindow.getUIContext().getPromptAction().showToast({ message: message });
  } catch (error) {
    // The request can outlive its window during application teardown.
    record(`unsupported feature notification unavailable: ${describe(error as BusinessError)}`);
  }
}

// Invoked on ArkUI's thread by the nonblocking native queue. No shell command,
// process wait, or Godot engine callback is involved in an external app launch.
export async function openExternal(context: common.UIAbilityContext, kind: number, target: string): Promise<void> {
  try {
    let want: Want;
    if (kind === 1) {
      want = {
        bundleName: 'com.huawei.hmos.filemanager', abilityName: 'MainAbility',
        parameters: { fileUri: fileUri.getUriFromPath(target) }
      };
    } else if (kind === 2) {
      await showUnsupported(context, '当前版本暂不支持打开系统终端。');
      return;
    } else if (kind === 0) {
      if (target.startsWith('/') || target.startsWith('file://')) {
        await showUnsupported(context, '当前版本暂不支持使用外部编辑器打开文件。');
        return;
      }
      if (target.startsWith('https://') || target.startsWith('http://')) {
        await context.openLink(target, { appLinkingOnly: false });
        record('external link launch accepted');
        return;
      }
      const uri = target.startsWith('/') ? fileUri.getUriFromPath(target) : target;
      want = { action: 'ohos.want.action.viewData', uri: uri };
    } else {
      throw new Error(`Unknown external application request: ${kind}`);
    }
    await context.startAbility(want);
    record(`external application launch accepted kind=${kind}`);
  } catch (error) {
    record(`external application launch failed kind=${kind}: ${describe(error as BusinessError)}`);
    try {
      const mainWindow = await window.getLastWindow(context);
      mainWindow.getUIContext().getPromptAction().showToast({
        message: kind === 2 ? '无法打开系统终端，请查看 Godot 日志。' : '无法打开外部应用，请查看 Godot 日志。'
      });
    } catch (ignored) {
      // Window teardown must not turn an asynchronous launch error into a crash.
    }
  }
}
