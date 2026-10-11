// Godot Engine contributors. SPDX-License-Identifier: MIT
import { common, Want } from '@kit.AbilityKit';
import { UIContext, window } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileUri } from '@kit.CoreFileKit';
import { describe, record } from './Diagnostics';
import { localizedString } from './LocalizedStrings';

async function showNotification(context: common.UIAbilityContext, message: Resource): Promise<void> {
  try {
    const text: string = localizedString(context.resourceManager, message);
    if (text.length === 0) { return; }
    const mainWindow: window.Window = await window.getLastWindow(context);
    const uiContext: UIContext = mainWindow.getUIContext();
    uiContext.getPromptAction().showToast({ message: text });
  } catch (error) {
    // The request can outlive its window during application teardown.
    record(`external application notification unavailable: ${describe(error as BusinessError)}`);
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
      record('opening the system terminal is not supported in this version');
      await showNotification(context, $r('app.string.external_terminal_unsupported'));
      return;
    } else if (kind === 0) {
      if (target.startsWith('/') || target.startsWith('file://')) {
        record('opening files in an external editor is not supported in this version');
        await showNotification(context, $r('app.string.external_editor_unsupported'));
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
    await showNotification(context, kind === 2 ? $r('app.string.external_terminal_open_failed') :
      $r('app.string.external_app_open_failed'));
  }
}
