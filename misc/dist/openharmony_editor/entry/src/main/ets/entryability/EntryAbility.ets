// Godot Engine contributors. SPDX-License-Identifier: MIT
// This persistent launcher owns the separate Godot processes. Closing a project
// manager during an editor transition must not terminate the new editor.
import { AbilityConstant, contextConstant, UIAbility, Want } from '@kit.AbilityKit';
import window from '@ohos.window';
import { prepareRuntime } from '../runtime/Runtime';

export default class EntryAbility extends UIAbility {
  private ready: Promise<string> | undefined = undefined;
  onWindowStageCreate(stage: window.WindowStage): void {
    stage.loadContent('pages/Launcher');
    this.ready = prepareRuntime(this.context);
    this.launch(['--project-manager'], '');
  }
  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    const args = want.parameters?.godotArguments as string[] | undefined;
    const key = want.parameters?.godotLaunchKey as string | undefined;
    this.launch(args ?? ['--project-manager'], key ?? '');
  }
  private async launch(args: string[], key: string): Promise<void> {
    try {
      if (!this.ready) { this.ready = prepareRuntime(this.context); }
      await this.ready;
      await this.context.startAbility({
        bundleName: this.context.abilityInfo.bundleName, abilityName: 'EditorAbility',
        parameters: { godotArguments: args, godotLaunchKey: key }
      }, {
        processMode: contextConstant.ProcessMode.NEW_PROCESS_ATTACH_TO_PARENT,
        startupVisibility: contextConstant.StartupVisibility.STARTUP_SHOW
      });
    } catch (error) {
      AppStorage.setOrCreate('godotLauncherMessage', `无法启动 Godot：${error.message}`);
    }
  }
}
