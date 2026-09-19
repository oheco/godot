// Godot Engine contributors. SPDX-License-Identifier: MIT
// This persistent launcher owns the separate Godot processes. Closing a project
// manager during an editor transition must not terminate the new editor.
import { AbilityConstant, contextConstant, UIAbility, Want } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import window from '@ohos.window';
import { prepareRuntime } from '../runtime/Runtime';
import { fail, initDiagnostics, probeEnvironment, record, setStep } from '../runtime/Diagnostics';

export default class EntryAbility extends UIAbility {
  private ready: Promise<string> | undefined = undefined;
  onWindowStageCreate(stage: window.WindowStage): void {
    initDiagnostics(this.context);
    setStep('launcher:load-content');
    stage.loadContent('pages/Launcher');
    setStep('launcher:extract-runtime');
    this.ready = prepareRuntime(this.context);
    this.launch(['--project-manager'], '');
  }
  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    const args = want.parameters?.godotArguments as string[] | undefined;
    const key = want.parameters?.godotLaunchKey as string | undefined;
    setStep('launcher:new-want');
    this.launch(args ?? ['--project-manager'], key ?? '');
  }
  private async launch(args: string[], key: string): Promise<void> {
    try {
      if (!this.ready) { this.ready = prepareRuntime(this.context); }
      await this.ready;
      setStep('launcher:start-editor-ability');
      record(`launching EditorAbility args=[${args.join(' ')}] key='${key}'`);
      await this.context.startAbility({
        bundleName: this.context.abilityInfo.bundleName, abilityName: 'EditorAbility',
        parameters: { godotArguments: args, godotLaunchKey: key }
      }, {
        processMode: contextConstant.ProcessMode.NEW_PROCESS_ATTACH_TO_PARENT,
        startupVisibility: contextConstant.StartupVisibility.STARTUP_SHOW
      });
      record('EditorAbility start requested');
    } catch (error) {
      const cause = error as BusinessError;
      fail(cause);
      probeEnvironment(this.context);
      AppStorage.setOrCreate('godotLauncherMessage', `无法启动 Godot（点“显示日志”查看详情）：${cause.message}`);
    }
  }
}
