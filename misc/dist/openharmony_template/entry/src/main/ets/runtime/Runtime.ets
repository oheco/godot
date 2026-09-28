// Godot Engine contributors. SPDX-License-Identifier: MIT
import { common } from '@kit.AbilityKit';
import { fileIo as fs, hash } from '@kit.CoreFileKit';
import { BusinessError, zlib } from '@kit.BasicServicesKit';
import { util } from '@kit.ArkTS';
import plugin from 'libentry.so';
import { describe, record, setStep } from './Diagnostics';
import { ensureDirectory, exists, removeTree } from './FileSystem';

interface RuntimeManifest { version: string; sha256: string; size: number; }
let preparing: Promise<string> | undefined = undefined;
let extractionSequence: number = 0;

function readyRuntime(destination: string, manifest: RuntimeManifest): boolean {
  const path = `${destination}/.ready`;
  if (!exists(path)) { return false; }
  try {
    const info = fs.statSync(path);
    if (!fs.statSync(destination).isDirectory() || !info.isFile() || info.size <= 0 || info.size > 4096) { return false; }
    const ready = JSON.parse(fs.readTextSync(path, { length: info.size })) as RuntimeManifest;
    return ready !== null && ready.version === manifest.version && ready.sha256 === manifest.sha256 && ready.size === manifest.size;
  } catch (error) {
    record(`runtime ready validation failed: ${describe(error as BusinessError)}`);
    return false;
  }
}

function cleanupOwned(path: string): void {
  try { removeTree(path); } catch (error) {
    record(`runtime cleanup failed for own path ${path}: ${describe(error as BusinessError)}`);
  }
}

async function extractRuntime(context: common.UIAbilityContext): Promise<string> {
  const data = await context.resourceManager.getRawFileContent('runtime-manifest.json');
  const manifest = JSON.parse(new util.TextDecoder().decodeWithStream(data)) as RuntimeManifest;
  if (manifest === null || typeof manifest !== 'object' || Array.isArray(manifest) ||
    typeof manifest.version !== 'string' || manifest.version.length === 0 ||
    typeof manifest.sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(manifest.sha256) ||
    !Number.isSafeInteger(manifest.size) || manifest.size <= 0) {
    throw new Error('Invalid runtime manifest');
  }
  record(`runtime manifest: version=${manifest.version} size=${manifest.size} sha256=${manifest.sha256}`);
  const parent = `${context.filesDir}/runtimes`;
  ensureDirectory(parent);
  // Version the publication protocol to avoid trusting the legacy empty .ready
  // marker. The old tree is never removed: another process may still use it.
  const destination = `${parent}/v1-${manifest.sha256}`;
  if (readyRuntime(destination, manifest)) {
    record(`runtime already extracted: ${destination}`);
    return destination;
  }
  if (exists(destination)) {
    // Another process may have published between the first ready check and stat.
    if (readyRuntime(destination, manifest)) { return destination; }
    throw new Error(`Invalid published runtime: ${destination}`);
  }
  const operation = `${plugin.processId()}-${Date.now()}-${extractionSequence++}-${util.generateRandomUUID(true)}`;
  // Staging is a sibling of destination so publication is an atomic same-volume
  // rename. Both staging and archive are private to this extraction, not merely
  // to a digest or a PID (which can be reused on a later launch).
  const staging = `${destination}.extracting-${operation}`;
  const archive = `${context.tempDir}/godot-runtime-${operation}.zip`;
  try {
    setStep('runtime:prepare-staging');
    ensureDirectory(staging);
    setStep('runtime:write-archive');
    const raw = await context.resourceManager.getRawFd('runtime.zip');
    try {
      if (raw.length !== manifest.size) { throw new Error('Runtime archive size mismatch'); }
      const file = fs.openSync(archive, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      try {
        fs.lseek(raw.fd, raw.offset, fs.WhenceType.SEEK_SET);
        const buffer = new ArrayBuffer(1024 * 1024);
        let copied = 0;
        while (copied < raw.length) {
          const count = await fs.read(raw.fd, buffer, { length: Math.min(buffer.byteLength, raw.length - copied) });
          if (count === 0) { throw new Error('Unexpected end of runtime archive'); }
          let written = 0;
          while (written < count) {
            const size = await fs.write(file.fd, buffer.slice(written, count));
            if (size === 0) { throw new Error('Unable to write runtime archive'); }
            written += size;
          }
          copied += count;
        }
      } finally { fs.closeSync(file); }
    } finally { await context.resourceManager.closeRawFd('runtime.zip'); }
    if ((await hash.hash(archive, 'sha256')).toLowerCase() !== manifest.sha256) {
      throw new Error('Runtime archive checksum mismatch');
    }
    setStep('runtime:decompress');
    await zlib.decompressFile(archive, staging);
    setStep('runtime:publish');
    // The marker travels with the completed tree. Never delete destination to
    // win a race: that can remove a runtime already used by a different process.
    const marker = fs.openSync(`${staging}/.ready`, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
    try { fs.writeSync(marker.fd, JSON.stringify(manifest)); } finally { fs.closeSync(marker); }
    try {
      fs.renameSync(staging, destination);
    } catch (error) {
      if (!readyRuntime(destination, manifest)) { throw error as Error; }
      record(`runtime publication won by another process: ${destination}`);
    }
    if (!readyRuntime(destination, manifest)) { throw new Error('Published runtime failed ready validation'); }
    record(`runtime ready: ${destination}`);
    return destination;
  } finally {
    cleanupOwned(staging);
    cleanupOwned(archive);
  }
}

export async function prepareRuntime(context: common.UIAbilityContext): Promise<string> {
  // Also coalesce concurrent callers in one process: raw resource descriptors
  // may share an offset, so two local reads must not race each other.
  if (preparing !== undefined) { return preparing; }
  const operation = extractRuntime(context);
  preparing = operation;
  try { return await operation; } finally { preparing = undefined; }
}
