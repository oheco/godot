// Godot Engine contributors. SPDX-License-Identifier: MIT
import { BusinessError } from '@kit.BasicServicesKit';
import window from '@ohos.window';
import plugin from 'libentry.so';
import { describe, record } from './Diagnostics';

interface WindowModeRequest {
  requestId: number;
  mode: number;
}

// Owns window operations on ArkUI's thread. Native request IDs distinguish an
// older completion from a newer desired mode while the SDK operation is pending.
export class WindowModeController {
  private mainWindow: window.Window;
  private destroyed: boolean = false;
  private inFlight: boolean = false;
  private queuedRequest: WindowModeRequest | undefined = undefined;
  private readonly statusHandler: (status: window.WindowStatusType) => void = (status: window.WindowStatusType): void => {
    if (this.destroyed) { return; }
    const mode: number = this.modeFromStatus(status);
    this.publishStatus(mode);
  };

  constructor(mainWindow: window.Window) {
    this.mainWindow = mainWindow;
    try {
      this.mainWindow.on('windowStatusDidChange', this.statusHandler);
    } catch (error) {
      record(`window mode listener registration failed: ${describe(error as BusinessError)}`);
    }
    // Publish the initial actual mode before loadContent can create the surface.
    this.publishStatus(this.queryActualMode());
  }

  request(requestId: number, mode: number): void {
    if (this.destroyed) { return; }
    // Native normalizes exclusive fullscreen too; keep this boundary consistent.
    this.queuedRequest = { requestId: requestId, mode: mode === 4 ? 3 : mode };
    this.startNext();
  }

  destroy(): void {
    if (this.destroyed) { return; }
    this.destroyed = true;
    this.queuedRequest = undefined;
    try {
      this.mainWindow.off('windowStatusDidChange', this.statusHandler);
    } catch (error) {
      record(`window mode listener cleanup failed: ${describe(error as BusinessError)}`);
    }
  }

  private modeFromStatus(status: window.WindowStatusType): number {
    switch (status) {
      case window.WindowStatusType.FULL_SCREEN: return 3;
      case window.WindowStatusType.MAXIMIZE: return 2;
      case window.WindowStatusType.MINIMIZE: return 1;
      case window.WindowStatusType.FLOATING:
      case window.WindowStatusType.SPLIT_SCREEN: return 0;
      default: return -1;
    }
  }

  private queryActualMode(): number {
    try {
      const mode: number = this.modeFromStatus(this.mainWindow.getWindowStatus());
      if (mode >= 0) { return mode; }
    } catch (error) {
      record(`window mode status query failed: ${describe(error as BusinessError)}`);
    }
    // WindowProperties.isFullScreen describes system-bar layout, not mode.
    // Keep an unavailable status unknown instead of fabricating WINDOWED.
    return -1;
  }

  private publishStatus(mode: number): void {
    if (!this.destroyed && mode >= 0 && mode <= 3) { plugin.windowModeStatus(mode); }
  }

  private startNext(): void {
    if (this.destroyed || this.inFlight) { return; }
    const request: WindowModeRequest | undefined = this.queuedRequest;
    if (request === undefined) { return; }
    this.queuedRequest = undefined;
    this.inFlight = true;
    this.applyRequest(request);
  }

  private async applyRequest(request: WindowModeRequest): Promise<void> {
    try {
      try {
        if (request.mode !== 1 && this.queryActualMode() === 1) {
          // recover() changes the presentation; restore() unhides a minimized
          // main window. The SDK enforces its foreground requirement.
          await this.mainWindow.restore();
          if (this.destroyed) { return; }
        }
        // Always perform the requested operation: an older event snapshot is
        // insufficient to skip a transition after a different pending request.
        switch (request.mode) {
          case 0: await this.mainWindow.recover(); break;
          case 1: await this.mainWindow.minimize(); break;
          case 2: await this.mainWindow.maximize(window.MaximizePresentation.EXIT_IMMERSIVE); break;
          case 3: await this.mainWindow.maximize(window.MaximizePresentation.ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER); break;
        }
      } catch (error) {
        if (!this.destroyed) { record(`window mode change failed: ${describe(error as BusinessError)}`); }
      }
      if (this.destroyed) { return; }
      // Success and failure both report the actual state. Unknown (-1) clears
      // native pending state without replacing its last known actual mode.
      plugin.windowModeResult(request.requestId, this.queryActualMode());
    } finally {
      this.inFlight = false;
      this.startNext();
    }
  }
}
