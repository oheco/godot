// Godot Engine contributors. SPDX-License-Identifier: MIT
// The ability window is hidden rather than destroyed while a process it started
// is running: a process created with NEW_PROCESS_ATTACH_TO_PARENT is cleaned up
// together with the process that started it, so that process has to stay alive
// even after its own engine has quit.
import window from '@ohos.window';

let stage: window.WindowStage | undefined = undefined;

export function rememberWindowStage(value: window.WindowStage): void {
  stage = value;
}

export function hideWindow(): void {
  try {
    if (stage !== undefined) {
      stage.getMainWindowSync().hide().catch(() => {});
    }
  } catch (ignored) {
    // Best effort: failing to hide only leaves an extra window visible.
  }
}

export function showWindow(): void {
  try {
    if (stage !== undefined) {
      stage.getMainWindowSync().show().catch(() => {});
    }
  } catch (ignored) {
    // Best effort: the log panel is still usable if the window cannot be shown.
  }
}
