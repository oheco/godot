// Godot Engine contributors. SPDX-License-Identifier: MIT
import { abilityAccessCtrl, bundleManager, common, Permissions } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { describe, record } from './Diagnostics';

export async function requestGamePermissions(context: common.UIAbilityContext): Promise<void> {
  let resource: string;
  try {
    resource = context.resourceManager.getStringByNameSync('user_permissions');
  } catch (error) {
    record(`user_permissions unavailable: ${describe(error as BusinessError)}`);
    return;
  }
  const permissions: Permissions[] = [];
  for (const entry of resource.split(',')) {
    const name = entry.trim() as Permissions;
    if (name.length > 0 && !permissions.includes(name)) { permissions.push(name); }
  }
  if (permissions.length === 0) { return; }
  try {
    const result = await abilityAccessCtrl.createAtManager().requestPermissionsFromUser(context, permissions);
    record(`user_permissions authResults=[${result.authResults.join(',')}] (0 means granted)`);
  } catch (error) {
    // A denied/cancelled dialog must not prevent a game from starting. Query the
    // actual token below, rather than treating a failed dialog as full denial.
    record(`user_permissions request failed: ${describe(error as BusinessError)}`);
  }
}

export function grantedPermissions(): string[] {
  const granted: string[] = [];
  try {
    const flags = bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION |
      bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_REQUESTED_PERMISSION;
    const info = bundleManager.getBundleInfoForSelfSync(flags);
    const manager = abilityAccessCtrl.createAtManager();
    // Include declared system_grant permissions (e.g. INTERNET), not only the
    // user_grant names that appeared in the dialog. Never infer grants from ACL.
    for (let index = 0; index < info.reqPermissionDetails.length; index++) {
      const name = info.reqPermissionDetails[index].name.trim() as Permissions;
      if (name.length === 0 || granted.includes(name)) { continue; }
      try {
        const state = manager.checkAccessTokenSync(info.appInfo.accessTokenId, name);
        record(`permission ${name}: bundle=${info.permissionGrantStates[index]} token=${state}`);
        if (state === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED) { granted.push(name); }
      } catch (error) {
        record(`permission ${name} check failed: ${describe(error as BusinessError)}`);
      }
    }
  } catch (error) {
    // Fail closed: native receives no invented permissions if introspection fails.
    record(`declared permission query failed: ${describe(error as BusinessError)}`);
  }
  return granted;
}
