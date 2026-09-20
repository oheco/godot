// Godot Engine contributors. SPDX-License-Identifier: MIT
// This ability is both the application entry point and the engine host: a plain
// launch (the application icon) opens the project manager, and a want that names
// a project opens that project in a new process.
import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import window from '@ohos.window';
import plugin from 'libentry.so';
import { launchInstance, reportStarted } from '../runtime/Launch';

export default class EditorAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    AppStorage.setOrCreate('godotArguments', this.engineArguments(want));
    plugin.setLauncher((request: number, launchArgs: string[]) => { launchInstance(this.context, request, launchArgs); });
    reportStarted(this.context, (want.parameters?.godotLaunchKey as string | undefined) ?? '');
  }
  // Explicit engine arguments win; a project directory in the want opens that
  // project in the editor; anything else opens the project manager.
  private engineArguments(want: Want): string[] {
    const explicit = want.parameters?.godotArguments as string[] | undefined;
    if (explicit !== undefined) { return explicit; }
    const project = want.parameters?.godotProject as string | undefined;
    if (project !== undefined && project.length > 0) { return ['--path', project, '--editor']; }
    return ['--project-manager'];
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
