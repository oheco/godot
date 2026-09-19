// Godot Engine contributors. SPDX-License-Identifier: MIT
import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';
import { fileIo as fs, hash } from '@kit.CoreFileKit';
import { BusinessError, zlib } from '@kit.BasicServicesKit';
import { util } from '@kit.ArkTS';
import { describe, record, setStep } from './Diagnostics';
import { ensureDirectory, exists, removeTree } from './FileSystem';

interface RuntimeManifest { version: string; sha256: string; size: number; }

// The .NET SDK lives in the user directory because the oo package manager owns
// it. READ_WRITE_USER_FILE is a user_grant restricted permission, so the
// platform shows a dialog and the directory stays unreadable until it is
// granted. ALLOW_EXTERNAL_NATIVE_CODE is system_grant and the profile carries it.
export async function requestDotnetAccess(context: common.UIAbilityContext): Promise<void> {
  const permissions: Permissions[] = ['ohos.permission.READ_WRITE_USER_FILE'];
  try {
    const result = await abilityAccessCtrl.createAtManager().requestPermissionsFromUser(context, permissions);
    const results: number[] = result.authResults;
    record(`READ_WRITE_USER_FILE authResults=[${results.join(',')}] (0 means granted)`);
  } catch (error) {
    record(`READ_WRITE_USER_FILE request failed: ${describe(error as BusinessError)}`);
  }
}

export async function prepareRuntime(context: common.UIAbilityContext): Promise<string> {
  const data = await context.resourceManager.getRawFileContent('runtime-manifest.json');
  const manifest = JSON.parse(new util.TextDecoder().decodeWithStream(data)) as RuntimeManifest;
  if (!/^[a-f0-9]{64}$/.test(manifest.sha256) || manifest.size <= 0) {
    throw new Error('Invalid runtime manifest');
  }
  record(`runtime manifest: version=${manifest.version} size=${manifest.size} sha256=${manifest.sha256}`);
  const parent = `${context.filesDir}/runtimes`;
  ensureDirectory(parent);
  // The content digest is also the directory identity, so upgrades never reuse stale files.
  const destination = `${parent}/${manifest.sha256}`;
  if (exists(`${destination}/.ready`)) {
    record(`runtime already extracted: ${destination}`);
    return destination;
  }
  setStep('runtime:prepare-staging');
  const staging = `${destination}.extracting`;
  removeTree(staging);
  ensureDirectory(staging);
  setStep('runtime:write-archive');
  const archive = `${context.cacheDir}/godot-runtime.zip`;
  // Read a bounded buffer; the complete SDK archive can be hundreds of MB.
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
  record(`runtime archive copied and verified: ${archive}`);
  setStep('runtime:decompress');
  await zlib.decompressFile(archive, staging);
  setStep('runtime:publish');
  record(`decompressed into ${staging}`);
  // Mark the tree complete before publishing it, so a destination that exists is
  // always a usable one and the rename is the only visible transition.
  const marker = fs.openSync(`${staging}/.ready`, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY);
  fs.closeSync(marker);
  // An earlier attempt may have been interrupted before the rename completed.
  removeTree(destination);
  fs.renameSync(staging, destination);
  fs.unlinkSync(archive);
  record(`runtime published: ${destination}`);
  return destination;
}
