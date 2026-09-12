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
export const configure: (filesDir: string, cacheDir: string, runtimeDir: string) => void;
export const checkRuntime: () => Promise<void>;
export const setResourceManager: (resources: resourceManager.ResourceManager) => void;
export const setWindowId: (id: number) => void;
export const setSurfaceId: (id: bigint) => void;
export const changeSurface: (id: bigint, width: number, height: number) => void;
export const destroySurface: () => void;
export const sendWindowEvent: (event: number) => void;
export const setup: (args: string[]) => void;
export const state: () => number;
export const inputTouch: (events: SimplifiedTouchEvent[]) => void;
export const inputKey: (event: SimplifiedKeyEvent) => void;
export const inputMouse: (event: SimplifiedMouseEvent) => void;
export const setLauncher: (callback: (requestId: number, args: string[]) => void) => void;
export const spawnResult: (requestId: number, pid: number) => void;
export const processId: () => number;
