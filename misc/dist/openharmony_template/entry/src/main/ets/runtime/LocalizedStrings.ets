// Godot Engine contributors. SPDX-License-Identifier: MIT
import { common } from '@kit.AbilityKit';
import { UIContext } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import { resourceManager } from '@kit.LocalizationKit';
import { describe, record } from './Diagnostics';

// Resolve on each call so strings follow the resource manager's current locale.
// An unavailable resource must not turn a notification into an application error.
export function localizedString(manager: resourceManager.ResourceManager, resource: Resource,
  ...args: Array<string | number>): string {
  try {
    // The Resource overload is deprecated; API26 supports the numeric ID overload.
    return manager.getStringSync(resource.id, ...args);
  } catch (error) {
    record(`localized string unavailable id=${resource.id}: ${describe(error as BusinessError)}`);
    return '';
  }
}

// Component callers can pass this.getUIContext() without casting its host context.
export function localizedUIString(uiContext: UIContext, resource: Resource,
  ...args: Array<string | number>): string {
  try {
    const context: common.Context | undefined = uiContext.getHostContext();
    if (context === undefined) {
      record(`localized string host context unavailable id=${resource.id}`);
      return '';
    }
    return localizedString(context.resourceManager, resource, ...args);
  } catch (error) {
    record(`localized string UI context unavailable id=${resource.id}: ${describe(error as BusinessError)}`);
    return '';
  }
}
