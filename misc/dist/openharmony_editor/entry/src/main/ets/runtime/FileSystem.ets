// Godot Engine contributors. SPDX-License-Identifier: MIT
// Application-sandbox helpers. On this platform fs.mkdirSync(path, true) still
// reports EEXIST when the target directory already exists, and fs.rmdirSync has
// no recursive form, so every directory operation goes through here.
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo as fs } from '@kit.CoreFileKit';

const EEXIST = 13900015;

export function exists(path: string): boolean {
  try {
    return fs.accessSync(path);
  } catch (error) {
    // accessSync may report "No such file or directory" instead of returning false.
    return false;
  }
}

export function ensureDirectory(path: string): void {
  if (exists(path)) {
    if (!fs.statSync(path).isDirectory()) {
      throw new Error(`${path} already exists and is not a directory`);
    }
    return;
  }
  try {
    fs.mkdirSync(path, true);
  } catch (error) {
    // Losing a race with another creator is harmless; anything else is not.
    if ((error as BusinessError).code !== EEXIST) {
      throw error;
    }
  }
}

export function removeTree(path: string): void {
  if (!exists(path)) {
    return;
  }
  if (!fs.statSync(path).isDirectory()) {
    fs.unlinkSync(path);
    return;
  }
  for (const entry of fs.listFileSync(path)) {
    removeTree(`${path}/${entry}`);
  }
  fs.rmdirSync(path);
}
