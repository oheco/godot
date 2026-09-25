// Godot Engine contributors. SPDX-License-Identifier: MIT
import plugin, { SimplifiedTouchEvent, SimplifiedMouseEvent, SimplifiedKeyEvent } from 'libentry.so';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { KeyCode } from '@kit.InputKit';
import { mapKeyCode } from './KeyMap';
import { prepareRuntime, requestDotnetAccess } from '../runtime/Runtime';
import { crashLogTime, describe, engineLogPath, fail, initDiagnostics, logPermissionState, probeEnvironment, readFullDiagnostics, record, setStep } from '../runtime/Diagnostics';
import { childHasExited, logInstanceState, reportExit, spawnedChildPid } from '../runtime/Launch';
import { probeBrokerConnection } from '../runtime/BrokerProbe';

class GodotSurface extends XComponentController {
  onSurfaceCreated(surfaceId: string): void { plugin.setSurfaceId(BigInt(surfaceId)); }
  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    plugin.changeSurface(BigInt(surfaceId), rect.surfaceWidth, rect.surfaceHeight);
  }
  onSurfaceDestroyed(surfaceId: string): void { plugin.destroySurface(); }
}

@Entry
@Component
struct Index {
  @State ready: boolean = false;
  @State message: string = '正在准备 Godot Editor…';
  @State showLog: boolean = false;
  @State logText: string = '';
  @State crashed: boolean = false;
  private controller: GodotSurface = new GodotSurface();
  private timer: number = -1;
  private crashTime: number = 0;
  private spawnedPid: number = 0;
  private childRunning: boolean = false;
  private terminating: boolean = false;
  private engineReadyReported: boolean = false;
  private instanceCheckAt: number = 0;
  private mouseMask: number = 0;
  private metaPressed: boolean = false;
  private pressedKeys: Set<number> = new Set<number>();
  private lastClickTime: number = 0;
  private lastClickButton: number = 0;
  private lastClickX: number = 0;
  private lastClickY: number = 0;

  aboutToAppear(): void {
    // Baseline for detecting a later crash in a parent-attached child.
    this.crashTime = crashLogTime();
    this.prepare();
  }
  aboutToDisappear(): void {
    if (this.timer !== -1) { clearInterval(this.timer); }
    plugin.destroySurface();
  }
  private async terminateHost(context: common.UIAbilityContext): Promise<void> {
    if (this.terminating) { return; }
    this.terminating = true;
    if (this.timer !== -1) { clearInterval(this.timer); this.timer = -1; }
    record(`engine exited; terminateSelf current ability pid=${plugin.processId()}`);
    logInstanceState(context, 'terminateSelf-request');
    reportExit(context);
    try {
      // Only terminate this UIAbility. Never force-stop the application bundle.
      await context.terminateSelf();
      record('terminateSelf returned');
    } catch (error) {
      record(`terminateSelf failed: ${describe(error as BusinessError)}`);
      this.ready = false;
      this.message = '引擎已退出，但关闭当前窗口失败，请查看日志。';
      this.logText = readFullDiagnostics();
      this.showLog = true;
    }
  }
  private async prepare(): Promise<void> {
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
    try {
      initDiagnostics(context);
      setStep('editor:extract-runtime');
      const runtime = await prepareRuntime(context);
      record(`runtime directory: ${runtime}`);
      setStep('editor:request-permission');
      await requestDotnetAccess(context);
      logPermissionState();
      setStep('editor:configure');
      plugin.configure(context.filesDir, context.cacheDir, runtime);
      setStep('editor:broker-tcp-test');
      this.message = '正在测试命令服务 TCP 连接…';
      await probeBrokerConnection();
      record('--- sandbox probe ---');
      const probeLines: string[] = plugin.probeSandbox().split('\n');
      for (const line of probeLines) {
        if (line.length > 0) { record(line); }
      }
      // The engine loads the .NET runtime from the package directory, but starting
      // a process there is a separate kernel decision: this is where the answer is
      // recorded, together with the errno the platform returned.
      record('--- dotnet exec test ---');
      const execLines: string[] = plugin.probeDotnetExec().split('\n');
      for (const line of execLines) {
        if (line.length > 0) { record(line); }
      }
      setStep('editor:check-dotnet');
      this.message = '正在检查 .NET SDK…';
      // Running the bundled CLI from application data is denied on this platform,
      // so record the failure and continue: the editor itself must still start.
      try {
        await plugin.checkRuntime();
        record('dotnet --version check passed');
      } catch (error) {
        record(`dotnet --version check failed, continuing: ${describe(error as BusinessError)}`);
      }
      setStep('editor:start-engine');
      plugin.setResourceManager(context.resourceManager);
      plugin.setWindowId(AppStorage.get<number>('godotWindowId') ?? -1);
      const args = AppStorage.get<string[]>('godotArguments') ?? ['--project-manager'];
      // The engine forwards its own copies of these flags when it asks for a new
      // instance, so only add the ones it did not provide. Every process logs to
      // its own file, so a crashed editor keeps its own trace and does not
      // overwrite the log of the process that started it.
      const defaults = ['--single-window', '--rendering-driver', 'vulkan'];
      const engineArgs = [...defaults.filter((flag: string) => !args.includes(flag)),
        '--log-file', engineLogPath(plugin.processId()), ...args];
      record(`godot command line: ${engineArgs.join(' ')}`);
      plugin.setup(engineArgs);
      record('engine thread started');
      this.ready = true;
      this.timer = setInterval(() => {
        if (this.terminating) { return; }
        const state: number = plugin.state();
        if (state === 2 && !this.engineReadyReported) {
          this.engineReadyReported = true;
          logInstanceState(context, 'engine-running');
          if (AppStorage.get<boolean>('godotIndependentInstance') ?? false) {
            // Diagnostic only. Handoff already happened when the page loaded;
            // this timer never delays the project manager's exit.
            this.instanceCheckAt = Date.now() + 3000;
          }
        }
        if (this.instanceCheckAt > 0 && Date.now() >= this.instanceCheckAt) {
          this.instanceCheckAt = 0;
          logInstanceState(context, `after-handoff-engine-state-${state}`);
        }
        if (state < 0) {
          this.message = `Godot 启动失败（${state}），点“显示日志”查看详情。`;
          this.ready = false;
        }
        // Only watch an attached child. An unrelated independent editor crash
        // must not hide this instance's surface or stop its engine.
        const attachedPid: number = spawnedChildPid();
        if (attachedPid > 0 && !this.crashed && crashLogTime(attachedPid) > this.crashTime) {
          record(`attached child crash detected pid=${attachedPid}`);
          this.crashed = true;
          this.ready = false;
          this.message = '编辑器进程崩溃了';
          this.logText = readFullDiagnostics();
          this.showLog = true;
        }
        if (state === 3) {
          // Independent editor instances never enter this hold path. Preserve it
          // only for launches that still use NEW_PROCESS_ATTACH_TO_PARENT.
          this.spawnedPid = spawnedChildPid();
          if (this.spawnedPid > 0 && !this.childRunning && !childHasExited(context, this.spawnedPid)) {
            this.childRunning = true;
            this.ready = false;
            this.message = '子进程仍在运行；本窗口保持打开以维持它的进程。';
            record(`engine exited; holding for attached pid ${this.spawnedPid}`);
          } else if (this.childRunning) {
            if (childHasExited(context, this.spawnedPid)) { this.terminateHost(context); }
          } else {
            this.terminateHost(context);
          }
        }
      }, 250);
    } catch (error) {
      const cause = error as BusinessError;
      fail(cause);
      probeEnvironment(context);
      this.message = `无法启动 Godot（点“显示日志”查看详情）：${cause.message}`;
    }
  }
  private button(button: MouseButton): number {
    switch (button) {
      case MouseButton.Left: return 1;
      case MouseButton.Right: return 2;
      case MouseButton.Middle: return 3;
      case MouseButton.Back: return 8;
      case MouseButton.Forward: return 9;
      default: return 0;
    }
  }
  private mouse(event: MouseEvent): void {
    const button = this.button(event.button);
    const type = event.action === MouseAction.Press ? 0 : event.action === MouseAction.Release ? 1 : 2;
    if (button > 0) {
      const bit = 1 << (button - 1);
      if (type === 0) { this.mouseMask |= bit; }
      if (type === 1) { this.mouseMask &= ~bit; }
    }
    const x = this.getUIContext().vp2px(event.x);
    const y = this.getUIContext().vp2px(event.y);
    const now = Date.now();
    let doubleClick = false;
    if (type === 0) {
      doubleClick = button === this.lastClickButton && now - this.lastClickTime < 500 &&
        Math.abs(x - this.lastClickX) < 5 && Math.abs(y - this.lastClickY) < 5;
      this.lastClickTime = doubleClick ? 0 : now;
      this.lastClickButton = button; this.lastClickX = x; this.lastClickY = y;
    }
    const input: SimplifiedMouseEvent = {
      type: type, button: button, mask: this.mouseMask, x: x, y: y,
      alt: event.getModifierKeyState?.(['alt']) ?? false,
      ctrl: event.getModifierKeyState?.(['ctrl']) ?? false,
      shift: event.getModifierKeyState?.(['shift']) ?? false,
      meta: this.metaPressed, doubleClick: doubleClick, factor: 1,
      hasRelative: event.rawDeltaX !== undefined && event.rawDeltaY !== undefined,
      relativeX: event.rawDeltaX ?? 0, relativeY: event.rawDeltaY ?? 0
    };
    plugin.inputMouse(input);
  }
  private wheel(event: AxisEvent, value: number, negative: number, positive: number): void {
    if (value === 0) { return; }
    const input: SimplifiedMouseEvent = {
      type: 0, button: value < 0 ? negative : positive, mask: this.mouseMask,
      x: this.getUIContext().vp2px(event.x), y: this.getUIContext().vp2px(event.y),
      alt: event.getModifierKeyState?.(['alt']) ?? false,
      ctrl: event.getModifierKeyState?.(['ctrl']) ?? false,
      shift: event.getModifierKeyState?.(['shift']) ?? false,
      meta: this.metaPressed, doubleClick: false, factor: Math.abs(value) / (event.scrollStep || 120),
      hasRelative: false, relativeX: 0, relativeY: 0
    };
    plugin.inputMouse(input); input.type = 1; plugin.inputMouse(input);
  }
  private handleKey(event: KeyEvent): boolean {
    const code = mapKeyCode(event.keyCode);
    if (code === 0) { return false; }
    const pressed = event.type === KeyType.Down;
    if (event.keyCode === KeyCode.KEYCODE_META_LEFT || event.keyCode === KeyCode.KEYCODE_META_RIGHT) {
      this.metaPressed = pressed;
    }
    const input: SimplifiedKeyEvent = {
      code: code, unicode: event.unicode ?? 0, pressed: pressed,
      echo: pressed && this.pressedKeys.has(event.keyCode),
      alt: event.getModifierKeyState?.(['alt']) ?? false,
      ctrl: event.getModifierKeyState?.(['ctrl']) ?? false,
      shift: event.getModifierKeyState?.(['shift']) ?? false, meta: this.metaPressed
    };
    if (pressed) { this.pressedKeys.add(event.keyCode); } else { this.pressedKeys.delete(event.keyCode); }
    plugin.inputKey(input); return true;
  }
  build() {
    Stack() {
      if (this.ready) {
        XComponent({ id: 'godot', type: XComponentType.SURFACE, controller: this.controller })
          .width('100%').height('100%')
          .focusable(true).defaultFocus(true)
          .onKeyEvent((event: KeyEvent) => this.handleKey(event))
          .onMouse((event: MouseEvent) => this.mouse(event))
          .onAxisEvent((event: AxisEvent) => {
            this.wheel(event, event.getVerticalAxisValue(), 4, 5);
            this.wheel(event, event.getHorizontalAxisValue(), 6, 7);
          })
          .onTouch((event: TouchEvent) => {
            const touches: SimplifiedTouchEvent[] = [];
            for (const touch of event.changedTouches) {
              touches.push({ type: event.type, id: touch.id,
                x: this.getUIContext().vp2px(touch.x), y: this.getUIContext().vp2px(touch.y) });
            }
            plugin.inputTouch(touches);
          })
          .onBlur(() => { this.pressedKeys.clear(); this.metaPressed = false; this.mouseMask = 0; plugin.sendWindowEvent(3); })
          .onFocus(() => plugin.sendWindowEvent(2))
      } else {
        Column({ space: 16 }) {
          Text(this.message).fontColor('#eeeeee').padding(24).copyOption(CopyOptions.LocalDevice)
          if (this.crashed) {
            Text('上次引擎启动崩溃了，点“显示日志”查看回溯。').fontSize(14).fontColor('#ffb366')
              .padding({ left: 24, right: 24 }).copyOption(CopyOptions.LocalDevice)
          }
          Button('显示日志').onClick(() => {
            this.logText = readFullDiagnostics();
            this.showLog = !this.showLog;
          })
          if (this.showLog) {
            Scroll() {
              Text(this.logText).fontSize(12).fontColor('#cccccc').textAlign(TextAlign.Start)
                .copyOption(CopyOptions.LocalDevice).width('100%')
            }.layoutWeight(1).width('100%').padding(12).backgroundColor('#111111')
          }
        }.width('100%').height('100%').padding(12)
      }
    }.width('100%').height('100%').backgroundColor('#242424')
  }
}
