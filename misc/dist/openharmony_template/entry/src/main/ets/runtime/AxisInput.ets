// Godot Engine contributors. SPDX-License-Identifier: MIT
import { SimplifiedMouseEvent } from 'libentry.so';

// MMI wheel axes contain angular degrees multiplied by the configured scroll
// rows. One conventional click is 15 degrees; Godot's normal wheel unit means
// three lines in TextEdit. Preserve the user's row multiplier exactly once.
const WHEEL_DEGREES_PER_UNIT: number = 15 * 3;
// Godot's desktop precise-scroll convention (macOS): logical pixel deltas
// become normalized pan units. Cursor positions remain physical pixels.
const PRECISE_SCROLL_SCALE: number = 0.03;

export class AxisInput {
  private pinchScales: Map<number, number> = new Map<number, number>();

  reset(): void { this.pinchScales.clear(); }

  convert(event: AxisEvent, mask: number, meta: boolean,
    pixels: (value: number) => number, logical: (value: number) => number): SimplifiedMouseEvent[] {
    const device: number = event.deviceId ?? 0;
    const action: number = event.action ?? AxisAction.NONE;
    if (action === AxisAction.END || action === AxisAction.CANCEL) {
      // Terminal events can retain the preceding sample's payload. They must
      // not replay it as an extra wheel tick, pan or pinch increment.
      this.pinchScales.delete(device);
      return [];
    }
    if (action === AxisAction.BEGIN) { this.pinchScales.delete(device); }
    if (action !== AxisAction.NONE && action !== AxisAction.BEGIN && action !== AxisAction.UPDATE) { return []; }
    const x: number = pixels(event.x);
    const y: number = pixels(event.y);
    const horizontal: number = event.getHorizontalAxisValue();
    const vertical: number = event.getVerticalAxisValue();
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(horizontal) || !Number.isFinite(vertical)) { return []; }
    const alt: boolean = event.getModifierKeyState?.(['Alt']) ?? false;
    const ctrl: boolean = event.getModifierKeyState?.(['Ctrl']) ?? false;
    const shift: boolean = event.getModifierKeyState?.(['Shift']) ?? false;
    const result: SimplifiedMouseEvent[] = [];
    if (event.sourceTool === SourceTool.TOUCHPAD) {
      // Finger axes are incremental screen-pixel distances, already adjusted
      // for the system's natural-scroll preference. Never multiply by rows or
      // emit independent wheel pulses for the two components of a diagonal.
      const dx: number = logical(horizontal) * PRECISE_SCROLL_SCALE;
      const dy: number = logical(vertical) * PRECISE_SCROLL_SCALE;
      if (dx !== 0 || dy !== 0) {
        result.push({ type: 3, button: 0, mask: mask, x: x, y: y,
          alt: alt, ctrl: ctrl, shift: shift, meta: meta, doubleClick: false,
          factor: 1, hasRelative: true, relativeX: dx, relativeY: dy });
      }
      const scale: number = event.getPinchAxisScaleValue?.() ?? 0;
      if (Number.isFinite(scale) && scale > 0) {
        const previous: number = this.pinchScales.get(device) ?? 1;
        const increment: number = scale / previous;
        this.pinchScales.set(device, scale);
        if (increment !== 1 && Number.isFinite(increment) && increment > 0) {
          result.push({ type: 4, button: 0, mask: mask, x: x, y: y,
            alt: alt, ctrl: ctrl, shift: shift, meta: meta, doubleClick: false,
            factor: increment, hasRelative: false, relativeX: 0, relativeY: 0 });
        }
      }
      return result;
    }
    // A legacy event may omit sourceTool. Other known tools are not mouse
    // wheels and must not create scrolling from joystick/stylus axis values.
    if (event.sourceTool !== undefined && event.sourceTool !== SourceTool.Unknown && event.sourceTool !== SourceTool.MOUSE) { return []; }
    this.appendWheel(result, vertical, 4, 5, x, y, mask, alt, ctrl, shift, meta);
    this.appendWheel(result, horizontal, 6, 7, x, y, mask, alt, ctrl, shift, meta);
    return result;
  }

  private appendWheel(result: SimplifiedMouseEvent[], value: number, negative: number, positive: number,
    x: number, y: number, mask: number, alt: boolean, ctrl: boolean, shift: boolean, meta: boolean): void {
    if (value === 0) { return; }
    const button: number = value < 0 ? negative : positive;
    const factor: number = Math.abs(value) / WHEEL_DEGREES_PER_UNIT;
    const pressed: SimplifiedMouseEvent = { type: 0, button: button, mask: mask | (1 << (button - 1)), x: x, y: y,
      alt: alt, ctrl: ctrl, shift: shift, meta: meta, doubleClick: false,
      factor: factor, hasRelative: false, relativeX: 0, relativeY: 0 };
    const released: SimplifiedMouseEvent = { type: 1, button: button, mask: mask, x: x, y: y,
      alt: alt, ctrl: ctrl, shift: shift, meta: meta, doubleClick: false,
      factor: factor, hasRelative: false, relativeX: 0, relativeY: 0 };
    result.push(pressed, released);
  }
}
