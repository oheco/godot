// Godot Engine contributors. SPDX-License-Identifier: MIT
import type { SimplifiedTouchEvent } from 'libentry.so';

interface TouchSlot {
  id: number;
  deviceId: number;
  x: number;
  y: number;
}

// TouchObject.id is opaque and device-local, not Godot's 0..31 touch index.
// Keep state outside ArkUI @State so all native DTOs are fresh plain objects.
export class TouchInput {
  private active: Map<string, TouchSlot> = new Map<string, TouchSlot>();
  private occupied: boolean[] = new Array<boolean>(32).fill(false);

  convert(event: TouchEvent, toPixels: (value: number) => number): SimplifiedTouchEvent[] {
    const result: SimplifiedTouchEvent[] = [];
    // ArkUI can synthesize touch callbacks from mouse/hover dispatch. Mouse
    // already has its own input route; never turn those callbacks into touches.
    if (event.source !== SourceType.TouchScreen ||
      (event.type !== TouchType.Down && event.type !== TouchType.Move &&
      event.type !== TouchType.Up && event.type !== TouchType.Cancel)) {
      return result;
    }
    const deviceId: number = event.deviceId ?? 0;
    if (!Number.isSafeInteger(deviceId)) { return result; }
    if (event.type === TouchType.Cancel && event.changedTouches.length === 0) {
      return this.cancelDevice(deviceId);
    }
    for (const touch of event.changedTouches) {
      if (!Number.isSafeInteger(touch.id)) { continue; }
      const key = `${deviceId}:${touch.id}`;
      let slot = this.active.get(key);
      if (event.type === TouchType.Down) {
        if (slot !== undefined || this.active.size >= 32) { continue; }
      } else if (slot === undefined) {
        // An ignored/overflow Down must not acquire a slot on Move or Up.
        continue;
      }
      let x: number = NaN;
      let y: number = NaN;
      if (Number.isFinite(touch.x) && Number.isFinite(touch.y)) {
        x = toPixels(touch.x);
        y = toPixels(touch.y);
      }
      // Native coordinates are float32. Check the range before crossing NAPI,
      // including a finite VP value whose pixel conversion overflows float32.
      if (!Number.isFinite(x) || !Number.isFinite(y) ||
        Math.abs(x) > 3.4028234663852886e38 || Math.abs(y) > 3.4028234663852886e38) {
        if (slot === undefined || (event.type !== TouchType.Up && event.type !== TouchType.Cancel)) {
          continue;
        }
        // Ignore bad coordinates, but do not strand a known contact forever
        // when its terminal event has no usable position.
        x = slot.x;
        y = slot.y;
      }
      if (slot === undefined) {
        const id = this.occupied.indexOf(false);
        if (id < 0) { continue; }
        slot = { id: id, deviceId: deviceId, x: x, y: y };
        this.occupied[id] = true;
        this.active.set(key, slot);
      } else {
        slot.x = x;
        slot.y = y;
      }
      result.push({ type: event.type, id: slot.id, x: x, y: y });
      if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
        this.occupied[slot.id] = false;
        this.active.delete(key);
      }
    }
    return result;
  }

  private cancelDevice(deviceId: number): SimplifiedTouchEvent[] {
    const result: SimplifiedTouchEvent[] = [];
    this.active.forEach((slot: TouchSlot, key: string) => {
      if (slot.deviceId === deviceId) {
        result.push({ type: TouchType.Cancel, id: slot.id, x: slot.x, y: slot.y });
        this.occupied[slot.id] = false;
        this.active.delete(key);
      }
    });
    return result;
  }

  cancelAll(): SimplifiedTouchEvent[] {
    const result: SimplifiedTouchEvent[] = [];
    this.active.forEach((slot: TouchSlot) => {
      result.push({ type: TouchType.Cancel, id: slot.id, x: slot.x, y: slot.y });
    });
    this.active.clear();
    this.occupied.fill(false);
    return result;
  }
}
