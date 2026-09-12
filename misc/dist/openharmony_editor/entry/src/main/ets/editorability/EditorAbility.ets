// Godot Engine contributors. SPDX-License-Identifier: MIT
import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import window from '@ohos.window';
import plugin from 'libentry.so';
import { launchInstance, reportStarted } from '../runtime/Launch';

export default class EditorAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    const args = want.parameters?.godotArguments as string[] | undefined;
    AppStorage.setOrCreate('godotArguments', args ?? ['--project-manager']);
    plugin.setLauncher((request: number, launchArgs: string[]) => { launchInstance(this.context, request, launchArgs); });
    reportStarted(this.context, (want.parameters?.godotLaunchKey as string | undefined) ?? '');
  }
  onWindowStageCreate(stage: window.WindowStage): void {
    AppStorage.setOrCreate('godotWindowId', stage.getMainWindowSync().getWindowProperties().id);
    stage.on('windowStageEvent', (event: window.WindowStageEventType) => plugin.sendWindowEvent(event));
    stage.loadContent('pages/Index');
  }
  onForeground(): void { plugin.sendWindowEvent(5); }
  onBackground(): void { plugin.sendWindowEvent(6); }
  onWindowStageDestroy(): void { plugin.destroySurface(); }
  onDestroy(): void { plugin.destroySurface(); }
}
