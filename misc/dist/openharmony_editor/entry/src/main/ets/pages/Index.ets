// Godot Engine contributors. SPDX-License-Identifier: MIT
import plugin, { SimplifiedTouchEvent, SimplifiedMouseEvent, SimplifiedKeyEvent } from 'libentry.so';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { KeyCode } from '@kit.InputKit';
import { mapKeyCode } from './KeyMap';
import { prepareRuntime, requestDotnetAccess } from '../runtime/Runtime';
import { crashLogTime, describe, engineLogPath, fail, initDiagnostics, logPermissionState, probeEnvironment, readFullDiagnostics, record, setStep } from '../runtime/Diagnostics';
import { childHasExited, reportExit, spawnedChildPid } from '../runtime/Launch';

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
  private mouseMask: number = 0;
  private metaPressed: boolean = false;
  private pressedKeys: Set<number> = new Set<number>();
  private lastClickTime: number = 0;
  private lastClickButton: number = 0;
  private lastClickX: number = 0;
  private lastClickY: number = 0;

  aboutToAppear(): void {
    // Remembered so a crash log written later by another process is noticed.
    this.crashTime = crashLogTime();
    this.prepare();
  }
  aboutToDisappear(): void {
    if (this.timer !== -1) { clearInterval(this.timer); }
    plugin.destroySurface();
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
      record('--- sandbox probe ---');
      const probeLines: string[] = plugin.probeSandbox().split('\n');
      for (const line of probeLines) {
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
      const defaults = ['--single-window', '--rendering-driver', 'vulkan', '--verbose'];
      const engineArgs = [...defaults.filter((flag: string) => !args.includes(flag)),
        '--log-file', engineLogPath(plugin.processId()), ...args];
      record(`godot command line: ${engineArgs.join(' ')}`);
      plugin.setup(engineArgs);
      record('engine thread started');
      this.ready = true;
      this.timer = setInterval(() => {
        const state: number = plugin.state();
        if (state < 0) { this.message = `Godot 启动失败（${state}），点“显示日志”查看详情。`; this.ready = false; }
        // A process this one started died from a fatal signal: come back with its
        // log, which is the only place that records why it died.
        if (!this.crashed && crashLogTime() > this.crashTime) {
          this.crashed = true;
          this.ready = false;
          this.message = '编辑器进程崩溃了';
          this.logText = readFullDiagnostics();
          this.showLog = true;
        }
        if (state === 3) {
          // The engine is gone. A process started with
          // NEW_PROCESS_ATTACH_TO_PARENT exits together with the process that
          // started it, so while such a child runs this window has to stay open -
          // the window is what keeps the ability, and therefore the process,
          // alive. EditorAbility cannot hide it: Window.hide() is a system API.
          this.spawnedPid = spawnedChildPid();
          if (this.spawnedPid > 0 && !this.childRunning && !childHasExited(context, this.spawnedPid)) {
            this.childRunning = true;
            this.ready = false;
            this.message = '编辑器已在新窗口打开；本窗口保持打开以维持它的进程。';
            record(`engine exited; holding the process for pid ${this.spawnedPid}`);
          } else if (this.childRunning) {
            if (childHasExited(context, this.spawnedPid)) { context.terminateSelf(); }
          } else {
            reportExit(context);
            context.terminateSelf();
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
    Stack({ alignContent: Alignment.TopStart }) {
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
      // Kept over the engine surface as well: the project manager stays alive
      // while the editor it started runs, so this is the only place that can
      // still show the editor's log after the editor process died.
      Column() {
        Row({ space: 8 }) {
          Button(this.showLog ? '关闭日志' : '日志').fontSize(12).height(30)
            .backgroundColor('#99000000').fontColor('#dddddd')
            .onClick(() => {
              this.logText = readFullDiagnostics();
              this.showLog = !this.showLog;
            })
          if (this.crashed) {
            Text('另一个 Godot 进程崩溃了，日志里有回溯').fontSize(12).fontColor('#ffb366')
              .copyOption(CopyOptions.LocalDevice)
          }
        }
        if (this.showLog) {
          Scroll() {
            Text(this.logText).fontSize(11).fontColor('#cccccc').textAlign(TextAlign.Start)
              .copyOption(CopyOptions.LocalDevice).width('100%')
          }.layoutWeight(1).width('100%').padding(8).backgroundColor('#ee111111')
        }
      }
      .alignItems(HorizontalAlign.Start)
      .width(this.showLog ? '100%' : 'auto')
      .height(this.showLog ? '100%' : 'auto')
      .padding(6)
    }.width('100%').height('100%').backgroundColor('#242424')
  }
}
