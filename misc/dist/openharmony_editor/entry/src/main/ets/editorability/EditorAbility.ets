// Godot Engine contributors. SPDX-License-Identifier: MIT
// This ability is both the application entry point and the engine host: a plain
// launch (the application icon) opens the project manager, and a want that names
// a project opens that project in a new process.
import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import window from '@ohos.window';
import plugin from 'libentry.so';
import { initDiagnostics, record } from '../runtime/Diagnostics';
import { launchInstance, reportStarted } from '../runtime/Launch';

export default class EditorAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    // Initialised here rather than in the page: an ability process that dies
    // before its page loads must still leave a trace, and this is the first code
    // the process runs.
    initDiagnostics(this.context);
    const args = this.engineArguments(want);
    const key = (want.parameters?.godotLaunchKey as string | undefined) ?? '';
    record(`ability onCreate pid=${plugin.processId()} args=[${args.join(' ')}] key='${key}'`);
    AppStorage.setOrCreate('godotArguments', args);
    plugin.setLauncher((request: number, launchArgs: string[]) => { launchInstance(this.context, request, launchArgs); });
    reportStarted(this.context, key);
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
    record('ability onWindowStageCreate');
    AppStorage.setOrCreate('godotWindowId', stage.getMainWindowSync().getWindowProperties().id);
    stage.on('windowStageEvent', (event: window.WindowStageEventType) => plugin.sendWindowEvent(event));
    stage.loadContent('pages/Index');
  }
  onForeground(): void { plugin.sendWindowEvent(5); }
  onBackground(): void { plugin.sendWindowEvent(6); }
  onWindowStageDestroy(): void { record('ability onWindowStageDestroy'); plugin.destroySurface(); }
  onDestroy(): void { record('ability onDestroy'); plugin.destroySurface(); }
}
