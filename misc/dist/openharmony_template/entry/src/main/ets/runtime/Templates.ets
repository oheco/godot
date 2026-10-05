// Godot Engine contributors. SPDX-License-Identifier: MIT
import { common } from '@kit.AbilityKit';
import { fileIo as fs, hash } from '@kit.CoreFileKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { util } from '@kit.ArkTS';
import plugin from 'libentry.so';
import { describe, record, setStep } from './Diagnostics';
import { ensureDirectory } from './FileSystem';

interface TemplateFile { name: string; sha256: string; size: number; }
interface TemplateManifest { schemaVersion: number; sha256: string; files: TemplateFile[]; }
interface TemplatePointer { schemaVersion: number; sha256: string; }
let preparing: Promise<void> | undefined = undefined;
let preparingFilesDir: string = '';
let preparationSequence: number = 0;
const MANIFEST_LIMIT: number = 16384;
const NOT_FOUND: number = 13900002;

function requireObject(value: Object, keys: string[]): void {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error('Invalid export template manifest object');
  }
  const actual = Object.keys(value);
  if (actual.length !== keys.length || actual.some((key: string) => !keys.includes(key))) {
    throw new Error('Invalid export template manifest fields');
  }
}

function parseManifest(text: string): TemplateManifest {
  const manifest = JSON.parse(text) as TemplateManifest;
  requireObject(manifest, ['schemaVersion', 'sha256', 'files']);
  if (manifest.schemaVersion !== 1 || typeof manifest.sha256 !== 'string' ||
    !/^[a-f0-9]{64}$/.test(manifest.sha256) || !Array.isArray(manifest.files) || manifest.files.length !== 2) {
    throw new Error('Invalid export template manifest');
  }
  const names: string[] = [];
  for (const file of manifest.files) {
    requireObject(file, ['name', 'sha256', 'size']);
    if (typeof file.name !== 'string' ||
      !['openharmony_debug_arm64-v8a.zip', 'openharmony_release_arm64-v8a.zip'].includes(file.name) ||
      names.includes(file.name) || typeof file.sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(file.sha256) ||
      !Number.isSafeInteger(file.size) || file.size <= 0) {
      throw new Error('Invalid export template file manifest');
    }
    names.push(file.name);
  }
  return manifest;
}

function present(path: string): boolean {
  try { fs.lstatSync(path); return true; } catch (error) {
    if ((error as BusinessError).code === NOT_FOUND) { return false; }
    throw error as Error;
  }
}

function requireDirectory(path: string): void {
  const info = fs.lstatSync(path);
  if (info.isSymbolicLink() || !info.isDirectory()) {
    throw new Error(`Export template path must be a directory without symlinks: ${path}`);
  }
}

function safeFilesDir(path: string): void {
  if (typeof path !== 'string' || path.length <= 1 || !path.startsWith('/') || path.includes('\0')) {
    throw new Error('Export template filesDir must be an absolute private directory');
  }
  const parts = path.substring(1).split('/');
  if (parts.some((part: string) => part.length === 0 || part === '.' || part === '..')) {
    throw new Error('Export template filesDir contains unsafe path components');
  }
  // Reject symlink ancestors too: lstat of the final component alone would
  // still follow /private/link/files. Context supplies the private anchor;
  // these checked-path snapshots are not an OS security sandbox.
  let current = '';
  for (const part of parts) { current += `/${part}`; requireDirectory(current); }
}

function ensurePrivateDirectory(path: string): void {
  if (present(path)) { requireDirectory(path); return; }
  ensureDirectory(path);
  requireDirectory(path);
}

function regularFile(path: string, size: number): boolean {
  if (!present(path)) { return false; }
  const info = fs.lstatSync(path);
  return !info.isSymbolicLink() && info.isFile() && info.size === size;
}

function sameManifest(left: TemplateManifest, right: TemplateManifest): boolean {
  return left.schemaVersion === right.schemaVersion && left.sha256 === right.sha256 &&
    left.files.every((file: TemplateFile) => right.files.some((expected: TemplateFile) =>
      file.name === expected.name && file.sha256 === expected.sha256 && file.size === expected.size));
}

function readyTemplates(destination: string, manifest: TemplateManifest): boolean {
  try {
    if (!present(destination)) { return false; }
    requireDirectory(destination);
    const marker = `${destination}/.ready`;
    if (!present(marker)) { return false; }
    const info = fs.lstatSync(marker);
    if (info.isSymbolicLink() || !info.isFile() || info.size <= 0 || info.size > MANIFEST_LIMIT) { return false; }
    const ready = parseManifest(fs.readTextSync(marker, { length: info.size }));
    return sameManifest(ready, manifest) && manifest.files.every((file: TemplateFile) =>
      regularFile(`${destination}/${file.name}`, file.size));
  } catch (error) {
    record(`export templates ready validation failed: ${describe(error as BusinessError)}`);
    return false;
  }
}

function removeOwnedTree(path: string): void {
  if (!present(path)) { return; }
  const info = fs.lstatSync(path);
  // FileSystem.removeTree follows statSync links, so cleanup uses lstat here.
  // A link inside our staging tree is unlinked, never traversed.
  if (info.isSymbolicLink() || !info.isDirectory()) { fs.unlinkSync(path); return; }
  for (const entry of fs.listFileSync(path)) {
    if (entry.length === 0 || entry === '.' || entry === '..' || entry.includes('/') || entry.includes('\0')) {
      throw new Error('Unsafe name in own export template staging directory');
    }
    removeOwnedTree(`${path}/${entry}`);
  }
  fs.rmdirSync(path);
}

function cleanupOwned(path: string): void {
  try { removeOwnedTree(path); } catch (error) {
    record(`export templates cleanup failed for own path ${path}: ${describe(error as BusinessError)}`);
  }
}

function writeDocument(path: string, text: string): void {
  const bytes = new util.TextEncoder().encodeInto(text);
  const buffer = new ArrayBuffer(bytes.byteLength);
  new Uint8Array(buffer).set(bytes);
  const file = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC | fs.OpenMode.NOFOLLOW);
  try {
    let written = 0;
    while (written < buffer.byteLength) {
      const count = fs.writeSync(file.fd, buffer.slice(written));
      if (!Number.isInteger(count) || count <= 0 || count > buffer.byteLength - written) {
        throw new Error('Unable to write export template document');
      }
      written += count;
    }
  } finally { fs.closeSync(file); }
}

async function copyTemplate(context: common.UIAbilityContext, staging: string, manifest: TemplateFile): Promise<void> {
  const resource = `export-templates/${manifest.name}`;
  const raw = await context.resourceManager.getRawFd(resource);
  try {
    if (!Number.isSafeInteger(raw.fd) || raw.fd < 0 || !Number.isSafeInteger(raw.offset) || raw.offset < 0 ||
      raw.length !== manifest.size || !Number.isSafeInteger(raw.offset + raw.length)) {
      throw new Error('Export template raw descriptor/size mismatch');
    }
    const destination = `${staging}/${manifest.name}`;
    requireDirectory(staging);
    const file = fs.openSync(destination, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC | fs.OpenMode.NOFOLLOW);
    try {
      fs.lseek(raw.fd, raw.offset, fs.WhenceType.SEEK_SET);
      const buffer = new ArrayBuffer(1024 * 1024);
      let copied = 0;
      while (copied < raw.length) {
        const length = Math.min(buffer.byteLength, raw.length - copied);
        const count = await fs.read(raw.fd, buffer, { length });
        if (!Number.isInteger(count) || count <= 0 || count > length) {
          throw new Error('Unexpected end of export template archive');
        }
        let written = 0;
        while (written < count) {
          const size = await fs.write(file.fd, buffer.slice(written, count));
          if (!Number.isInteger(size) || size <= 0 || size > count - written) {
            throw new Error('Unable to write export template archive');
          }
          written += size;
        }
        copied += count;
      }
    } finally { fs.closeSync(file); }
    if (!regularFile(destination, manifest.size) ||
      (await hash.hash(destination, 'sha256')).toLowerCase() !== manifest.sha256) {
      throw new Error('Export template archive checksum mismatch');
    }
  } finally { await context.resourceManager.closeRawFd(resource); }
}

async function publishPointer(parent: string, operation: string, manifest: TemplateManifest): Promise<void> {
  const temporary = `${parent}/.pointer-${operation}`;
  let owned = false;
  try {
    requireDirectory(parent);
    // Exclusive directory creation claims this operation's unique temp file.
    // Unlike ensureDirectory, EEXIST must not grant ownership of another tree.
    fs.mkdirSync(temporary);
    owned = true;
    requireDirectory(temporary);
    const pointer: TemplatePointer = { schemaVersion: 1, sha256: manifest.sha256 };
    writeDocument(`${temporary}/current.json`, JSON.stringify(pointer));
    const current = `${parent}/current.json`;
    if (present(current)) {
      const info = fs.lstatSync(current);
      if (info.isSymbolicLink() || !info.isFile()) { throw new Error('Invalid export template current pointer path'); }
    }
    requireDirectory(parent);
    fs.renameSync(`${temporary}/current.json`, current);
  } finally { if (owned) { cleanupOwned(temporary); } }
}

async function extractTemplates(context: common.UIAbilityContext, filesDir: string): Promise<void> {
  let data: Uint8Array;
  try {
    data = await context.resourceManager.getRawFileContent('export-templates.json');
  } catch (error) {
    const code = (error as BusinessError).code;
    // Fixed, valid raw resource name: ResourceManager reports 9001005 when it
    // is absent. Do not turn malformed JSON, copying, hashing or generic I/O
    // failures into optional-resource fallback.
    if (code !== 9001005 && code !== 9001004 && code !== NOT_FOUND) { throw error as Error; }
    record(`export templates manifest absent; keeping existing/custom template fallback: ${describe(error as BusinessError)}`);
    return;
  }
  if (data.byteLength <= 0 || data.byteLength > MANIFEST_LIMIT) { throw new Error('Invalid export template manifest size'); }
  const manifest = parseManifest(new util.TextDecoder().decodeWithStream(data));
  safeFilesDir(filesDir);
  const parent = `${filesDir}/export_templates`;
  ensurePrivateDirectory(parent);
  const destination = `${parent}/${manifest.sha256}`;
  const operation = `${plugin.processId()}-${Date.now()}-${preparationSequence++}-${util.generateRandomUUID(true)}`;
  if (!readyTemplates(destination, manifest)) {
    if (present(destination)) {
      if (!readyTemplates(destination, manifest)) { throw new Error('Invalid published export templates; existing tree preserved'); }
    } else {
      const staging = `${destination}.preparing-${operation}`;
      let owned = false;
      try {
        setStep('templates:prepare-staging');
        requireDirectory(parent);
        fs.mkdirSync(staging);
        owned = true;
        requireDirectory(staging);
        setStep('templates:copy-archives');
        for (const file of manifest.files) { await copyTemplate(context, staging, file); }
        writeDocument(`${staging}/.ready`, JSON.stringify(manifest));
        setStep('templates:publish');
        requireDirectory(parent);
        if (present(destination)) {
          if (!readyTemplates(destination, manifest)) { throw new Error('Invalid raced export template destination'); }
          record(`export templates publication won by another process: ${destination}`);
        } else {
          try { fs.renameSync(staging, destination); } catch (error) {
            if (!readyTemplates(destination, manifest)) { throw error as Error; }
            record(`export templates publication won by another process: ${destination}`);
          }
        }
      } finally { if (owned) { cleanupOwned(staging); } }
    }
  } else { record(`export templates already prepared: ${destination}`); }
  if (!readyTemplates(destination, manifest)) { throw new Error('Published export templates failed ready validation'); }
  await publishPointer(parent, operation, manifest);
  record(`export templates ready: ${destination}`);
}

export async function prepareTemplates(context: common.UIAbilityContext, filesDir: string): Promise<void> {
  // A resource descriptor can share its cursor. Coalesce the same private root,
  // and serialize distinct roots rather than allowing local reads to overlap.
  if (preparing !== undefined) {
    const existing = preparing;
    const sameRoot = preparingFilesDir === filesDir;
    await existing;
    if (sameRoot) { return; }
    return prepareTemplates(context, filesDir);
  }
  const operation = extractTemplates(context, filesDir);
  preparing = operation;
  preparingFilesDir = filesDir;
  try { await operation; } finally { preparing = undefined; preparingFilesDir = ''; }
}
