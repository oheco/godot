// Godot Engine contributors. SPDX-License-Identifier: MIT
import { resourceManager } from '@kit.LocalizationKit';
export interface SimplifiedTouchEvent { type: number; id: number; x: number; y: number; }
export interface SimplifiedKeyEvent {
  code: number; unicode: number; pressed: boolean; echo: boolean;
  alt: boolean; ctrl: boolean; shift: boolean; meta: boolean;
}
export interface SimplifiedMouseEvent {
  type: number; button: number; mask: number; x: number; y: number;
  alt: boolean; ctrl: boolean; shift: boolean; meta: boolean;
  doubleClick: boolean; factor: number; hasRelative: boolean; relativeX: number; relativeY: number;
}
// game: runtime='', managedMode='none'; editor SDK: extracted runtime, 'sdk'.
export const configure: (filesDir: string, cacheDir: string, runtime: string, managedMode: string) => void;
export const setResourceManager: (resources: resourceManager.ResourceManager) => void;
export const setWindowId: (id: number) => void;
export const setSurfaceId: (id: bigint) => void;
export const changeSurface: (id: bigint, width: number, height: number) => void;
// Physical screen pixels, including ArkUI layout and surface offsets.
export const setSurfacePosition: (x: number, y: number) => void;
export const destroySurface: () => void;
export const sendWindowEvent: (event: number) => void;
// Native reads _cl_ and the packaged PCK when packagedGame is true.
export const setup: (args: string[], grantedPermissions: string[], packagedGame: boolean) => void;
export const state: () => number;
export const inputTouch: (events: SimplifiedTouchEvent[]) => void;
export const inputKey: (event: SimplifiedKeyEvent) => void;
export const inputMouse: (event: SimplifiedMouseEvent) => void;
export const setLauncher: (callback: (requestId: number, args: string[]) => void) => void;
export const spawnResult: (requestId: number, pid: number) => void;
// kind: 0=URI, 1=folder, 2=terminal. Calls are queued without waiting for launch.
export const setExternalOpener: (callback: (kind: number, target: string) => void) => void;
export const openTerminal: (directoryUri: string) => Promise<void>;
export const processId: () => number;
