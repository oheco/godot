// Godot Engine contributors. SPDX-License-Identifier: MIT
import plugin, { SimplifiedTouchEvent, SimplifiedMouseEvent, SimplifiedKeyEvent } from 'libentry.so';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { KeyCode } from '@kit.InputKit';
import { mapKeyCode } from './KeyMap';
import { prepareRuntime, requestDotnetAccess } from '../runtime/Runtime';
import { clearCrashLog, describe, fail, initDiagnostics, logPermissionState, probeEnvironment, readFullDiagnostics, record, setStep } from '../runtime/Diagnostics';

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
  private controller: GodotSurface = new GodotSurface();
  private timer: number = -1;
  private mouseMask: number = 0;
  private metaPressed: boolean = false;
  private pressedKeys: Set<number> = new Set<number>();
  private lastClickTime: number = 0;
  private lastClickButton: number = 0;
  private lastClickX: number = 0;
  private lastClickY: number = 0;

  aboutToAppear(): void { this.prepare(); }
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
      // A crash log from an earlier start must not hide a clean one.
      clearCrashLog();
      plugin.setResourceManager(context.resourceManager);
      plugin.setWindowId(AppStorage.get<number>('godotWindowId') ?? -1);
      const args = AppStorage.get<string[]>('godotArguments') ?? ['--project-manager'];
      // The engine writes its own output into application storage and --verbose
      // records how far startup reached. Both are temporary while engine startup
      // is still being stabilised on OpenHarmony.
      const engineArgs = ['--single-window', '--rendering-driver', 'vulkan', '--verbose',
        '--log-file', `${context.filesDir}/godot-engine.log`, ...args];
      record(`godot command line: ${engineArgs.join(' ')}`);
      plugin.setup(engineArgs);
      record('engine thread started');
      this.ready = true;
      this.timer = setInterval(() => {
        const state: number = plugin.state();
        if (state < 0) { this.message = `Godot 启动失败（${state}），点“显示日志”查看详情。`; this.ready = false; }
        if (state === 3) { context.terminateSelf(); }
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
