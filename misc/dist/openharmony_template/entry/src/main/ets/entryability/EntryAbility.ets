// Godot Engine contributors. SPDX-License-Identifier: MIT
// Both packaged games and the editor use this entry point and the same surface.
import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import window from '@ohos.window';
import plugin from 'libentry.so';
import { engineArguments, HostConfig, hostStorage, instancesEnabled, loadHostConfig } from '../runtime/Config';
import { describe, initDiagnostics, record, setDiagnosticsLevel } from '../runtime/Diagnostics';
import { openExternal } from '../runtime/ExternalApps';
import { hasEngineFlag, launchInstance, logInstanceState, reportExit, reportLaunchFailure, reportStarted, reportWindowReady } from '../runtime/Launch';

export default class EntryAbility extends UIAbility {
  private launcherEnabled: boolean = false;

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    try {
      const config: HostConfig = loadHostConfig(this.context);
      initDiagnostics(this.context, hostStorage(this.context, config));
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET);
      plugin.setExternalOpener((kind: number, target: string) => {
        openExternal(this.context, kind, target);
      });
      AppStorage.setOrCreate('godotHostConfig', config);
      AppStorage.setOrCreate('godotConfigError', '');
      setDiagnosticsLevel(config.diagnostics.level);
      const args = engineArguments(config, want);
      this.launcherEnabled = instancesEnabled(config);
      const acceptingLaunch = this.launcherEnabled && config.launch.acceptProjectRequests;
      const candidate = acceptingLaunch ? want.parameters?.godotLaunchKey : undefined;
      const key = typeof candidate === 'string' && /^[0-9]+-[0-9]+-[0-9]+$/.test(candidate) ? candidate : '';
      const independent: boolean = acceptingLaunch && want.parameters?.godotIndependentInstance === true;
      record(`ability onCreate pid=${plugin.processId()} role=${config.host.role} args=[${args.join(' ')}] key='${key}' independent=${independent}`);
      AppStorage.setOrCreate('godotArguments', args);
      AppStorage.setOrCreate('godotLaunchKey', key);
      AppStorage.setOrCreate('godotIndependentInstance', independent);
      if (this.launcherEnabled) {
        logInstanceState(this.context, 'ability-onCreate');
        const projectManager = hasEngineFlag(args, '--project-manager', '-p');
        plugin.setLauncher((request: number, launchArgs: string[]) => {
          launchInstance(this.context, request, launchArgs, projectManager, config);
        });
        // Independent editors acknowledge when the window loads, not when the
        // runtime extraction or native project loading eventually completes.
        if (!independent) { reportStarted(this.context, key); }
      }
    } catch (error) {
      // Invalid/missing config still gets a private error log, without guessing
      // the role or moving a game's existing application-scoped user:// data.
      initDiagnostics(this.context);
      const detail = describe(error as BusinessError);
      record(`host configuration failed: ${detail}`);
      AppStorage.setOrCreate('godotConfigError', detail);
    }
  }

  onWindowStageCreate(stage: window.WindowStage): void {
    record('ability onWindowStageCreate');
    AppStorage.setOrCreate('godotWindowId', stage.getMainWindowSync().getWindowProperties().id);
    stage.on('windowStageEvent', (event: window.WindowStageEventType) => plugin.sendWindowEvent(event));
    stage.loadContent('pages/Index', (error: BusinessError) => {
      const key: string = AppStorage.get<string>('godotLaunchKey') ?? '';
      const independent: boolean = AppStorage.get<boolean>('godotIndependentInstance') ?? false;
      if (error && error.code) {
        const detail = describe(error);
        record(`window content failed: ${detail}`);
        if (this.launcherEnabled && independent) { reportLaunchFailure(this.context, key, detail); }
        return;
      }
      record('window content ready');
      if (this.launcherEnabled) {
        if (independent) { reportWindowReady(this.context, key); }
        logInstanceState(this.context, 'window-ready');
      }
    });
  }

  onForeground(): void { plugin.sendWindowEvent(5); }
  onBackground(): void { plugin.sendWindowEvent(6); }
  onWindowStageDestroy(): void { record('ability onWindowStageDestroy'); plugin.destroySurface(); }
  onDestroy(): void {
    record('ability onDestroy');
    if (this.launcherEnabled) {
      logInstanceState(this.context, 'ability-onDestroy');
      reportExit(this.context);
    }
    plugin.destroySurface();
  }
}
