// Godot Engine contributors. SPDX-License-Identifier: MIT

// AppStorage exposes ArkUI observed Proxy arrays. JavaScript can read/iterate
// them, but Ark N-API's native-array checks do not unwrap those proxies. Always
// materialize an ordinary, detached array at this boundary; never relax native
// validation to accept arbitrary objects with a length property.
export function nativeStringArray(values: string[], name: string): string[] {
  if (!Array.isArray(values) || values.length > 65536) {
    throw new Error(`${name} must be an array with at most 65536 elements`);
  }
  const count: number = values.length;
  const snapshot: string[] = [];
  for (let index = 0; index < count; index++) {
    const value = values[index];
    // Validate the actual copied value too: Array.some skips sparse holes.
    if (typeof value !== 'string' || value.includes('\0')) {
      throw new Error(`${name}[${index}] must be a string without NUL characters`);
    }
    snapshot.push(value);
  }
  return snapshot;
}
