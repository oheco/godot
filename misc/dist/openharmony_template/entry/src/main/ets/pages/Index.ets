// Godot Engine contributors. SPDX-License-Identifier: MIT
import plugin, { SimplifiedTouchEvent, SimplifiedMouseEvent, SimplifiedKeyEvent } from 'libentry.so';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { KeyCode } from '@kit.InputKit';
import { mapKeyCode } from './KeyMap';
import { prepareRuntime } from '../runtime/Runtime';
import { prepareTemplates } from '../runtime/Templates';
import { crashLogTime, describe, fail, initDiagnostics, probeEnvironment, readFullDiagnostics, record, setStep } from '../runtime/Diagnostics';
import { HostConfig, hostStorage, instancesEnabled } from '../runtime/Config';
import { grantedPermissions, requestUserPermissions } from '../runtime/Permissions';
import { nativeStringArray } from '../runtime/NativeArguments';
import { TouchInput } from '../runtime/TouchInput';
import { childHasExited, logInstanceState, reportExit, spawnedChildPid } from '../runtime/Launch';

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
  @State message: string = '正在准备 Godot…';
  @State expandSystemArea: boolean = false;
  @State showLog: boolean = false;
  @State logText: string = '';
  @State crashed: boolean = false;
  private instanceSupport: boolean = false;
  private disposed: boolean = false;
  private controller: GodotSurface = new GodotSurface();
  private touchInput: TouchInput = new TouchInput();
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
    this.disposed = false;
    this.prepare();
  }
  aboutToDisappear(): void {
    this.disposed = true;
    if (this.timer !== -1) { clearInterval(this.timer); this.timer = -1; }
    this.releaseInput();
    plugin.destroySurface();
  }
  private async terminateHost(context: common.UIAbilityContext): Promise<void> {
    if (this.terminating) { return; }
    this.terminating = true;
    if (this.timer !== -1) { clearInterval(this.timer); this.timer = -1; }
    record(`engine exited; terminateSelf current ability pid=${plugin.processId()}`);
    if (this.instanceSupport) {
      logInstanceState(context, 'terminateSelf-request');
      reportExit(context);
    }
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
      const error = AppStorage.get<string>('godotConfigError') ?? '';
      if (error.length > 0) { throw new Error(error); }
      const config = AppStorage.get<HostConfig>('godotHostConfig');
      if (config === undefined) { throw new Error('Host configuration was not loaded by EntryAbility'); }
      const paths = hostStorage(context, config);
      initDiagnostics(context, paths);
      const packagedGame: boolean = config.host.role === 'game';
      this.expandSystemArea = config.window.expandIntoSystemArea;
      this.instanceSupport = instancesEnabled(config);
      if (this.instanceSupport) { this.crashTime = crashLogTime(); }
      this.message = packagedGame ? '正在启动游戏…' : '正在准备 Godot Editor…';
      let runtime: string = '';
      if (!packagedGame && config.managed.mode === 'sdk') {
        setStep('host:extract-runtime');
        runtime = await prepareRuntime(context);
        if (this.disposed) { return; }
        record(`runtime directory: ${runtime}`);
      }
      if (!packagedGame) {
        setStep('host:prepare-export-templates');
        await prepareTemplates(context, paths.filesDir);
        if (this.disposed) { return; }
      }
      setStep('host:request-user-permissions');
      await requestUserPermissions(context);
      if (this.disposed) { return; }
      const permissions = grantedPermissions();
      setStep('host:configure');
      plugin.configure(paths.filesDir, paths.cacheDir, runtime, config.managed.mode);
      plugin.setResourceManager(context.resourceManager);
      plugin.setWindowId(AppStorage.get<number>('godotWindowId') ?? -1);
      const args = AppStorage.get<string[]>('godotArguments');
      if (args === undefined) { throw new Error('Host launch arguments are missing'); }
      setStep('host:start-engine');
      record(`godot command line: ${args.join(' ')} packagedGame=${packagedGame}`);
      // No _cl_ parsing, PCK selection or global renderer/window flags here.
      // The native packaged-game path owns _cl_ and the embedded PCK. Default
      // logs for both roles are native-configured under the private filesDir.
      // Do not pass AppStorage's observed Proxy array into native_array APIs.
      const nativeArgs = nativeStringArray(args, 'setup.arguments');
      const nativePermissions = nativeStringArray(permissions, 'setup.grantedPermissions');
      record(`native setup snapshot: arguments=${nativeArgs.length} permissions=${nativePermissions.length}`);
      plugin.setup(nativeArgs, nativePermissions, packagedGame);
      record('engine setup accepted; waiting for surface/start state');
      this.ready = true;
      this.timer = setInterval(() => this.updateEngineState(context), 250);
    } catch (error) {
      initDiagnostics(context);
      const cause = error as BusinessError;
      fail(cause);
      probeEnvironment(context);
      this.ready = false;
      this.message = `无法启动 Godot（点“显示日志”查看详情）：${cause.message}`;
    }
  }
  private updateEngineState(context: common.UIAbilityContext): void {
    if (this.terminating || this.disposed) { return; }
    const state: number = plugin.state();
    if (state === 2 && !this.engineReadyReported) {
      this.engineReadyReported = true;
      record('engine running');
      if (this.instanceSupport) {
        logInstanceState(context, 'engine-running');
        if (AppStorage.get<boolean>('godotIndependentInstance') ?? false) {
          this.instanceCheckAt = Date.now() + 3000;
        }
      }
    }
    if (state < 0) {
      this.message = `Godot 启动失败（${state}），点“显示日志”查看详情。`;
      this.ready = false;
    }
    // Packaged games never inspect instance keys, launch markers or child PIDs.
    if (!this.instanceSupport) {
      if (state === 3) { this.terminateHost(context); }
      return;
    }
    if (this.instanceCheckAt > 0 && Date.now() >= this.instanceCheckAt) {
      this.instanceCheckAt = 0;
      logInstanceState(context, `after-handoff-engine-state-${state}`);
    }
    const attachedPid = spawnedChildPid();
    if (attachedPid > 0 && !this.crashed && crashLogTime(attachedPid) > this.crashTime) {
      record(`attached child crash detected pid=${attachedPid}`);
      this.crashed = true;
      this.ready = false;
      this.message = '子进程崩溃了';
      this.logText = readFullDiagnostics(attachedPid, this.crashTime);
      this.showLog = true;
    }
    if (state === 3 && !this.crashed) {
      // PM -> editor is independent. Only running games launched with
      // NEW_PROCESS_ATTACH_TO_PARENT need to keep their parent window alive.
      this.spawnedPid = attachedPid;
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
  private touch(event: TouchEvent): void {
    if (this.disposed || this.terminating) { return; }
    const touches: SimplifiedTouchEvent[] = this.touchInput.convert(event,
      (value: number): number => this.getUIContext().vp2px(value));
    if (touches.length > 0) { plugin.inputTouch(touches); }
  }
  private releaseInput(): void {
    const cancelled: SimplifiedTouchEvent[] = this.touchInput.cancelAll();
    if (cancelled.length > 0) { plugin.inputTouch(cancelled); }
    this.pressedKeys.clear();
    this.metaPressed = false;
    this.mouseMask = 0;
    this.lastClickTime = 0;
    this.lastClickButton = 0;
    plugin.sendWindowEvent(3);
  }
  build() {
    Stack() {
      if (this.ready) {
        XComponent({ id: 'godot', type: XComponentType.SURFACE, controller: this.controller })
          .width('100%').height('100%')
          .expandSafeArea(this.expandSystemArea ? [SafeAreaType.SYSTEM] : [],
            this.expandSystemArea ? [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM, SafeAreaEdge.START, SafeAreaEdge.END] : [])
          .focusable(true).defaultFocus(true)
          .onKeyEvent((event: KeyEvent) => this.handleKey(event))
          .onMouse((event: MouseEvent) => this.mouse(event))
          .onAxisEvent((event: AxisEvent) => {
            this.wheel(event, event.getVerticalAxisValue(), 4, 5);
            this.wheel(event, event.getHorizontalAxisValue(), 6, 7);
          })
          .onTouch((event: TouchEvent) => this.touch(event))
          .onBlur(() => this.releaseInput())
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
      .expandSafeArea(this.expandSystemArea ? [SafeAreaType.SYSTEM] : [],
        this.expandSystemArea ? [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM, SafeAreaEdge.START, SafeAreaEdge.END] : [])
  }
}
