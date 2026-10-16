// Godot Engine contributors. SPDX-License-Identifier: MIT
import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { util } from '@kit.ArkTS';
import plugin, { DevelopmentKitCandidate } from 'libentry.so';
import { describe, record, setStep } from './Diagnostics';

interface DevelopmentKitManifest {
  schemaVersion: number;
  engineVersion: string;
  hostAbiVersion: number;
  managedVersion: string;
  architecture: string;
  engineLibrarySha256: string;
  managedRoot?: string;
  templatesRoot?: string;
  templateSetSha256?: string;
}
let preparing: Promise<string> | undefined = undefined;

function validCompatibility(manifest: DevelopmentKitManifest): boolean {
  return manifest !== null && typeof manifest === 'object' && !Array.isArray(manifest) &&
    manifest.schemaVersion === 1 && typeof manifest.engineVersion === 'string' && manifest.engineVersion.length > 0 &&
    Number.isSafeInteger(manifest.hostAbiVersion) && manifest.hostAbiVersion > 0 &&
    typeof manifest.managedVersion === 'string' && manifest.managedVersion.length > 0 &&
    manifest.architecture === 'arm64-v8a' && typeof manifest.engineLibrarySha256 === 'string' &&
    /^[a-f0-9]{64}$/.test(manifest.engineLibrarySha256);
}
function compatible(expected: DevelopmentKitManifest, actual: DevelopmentKitManifest): boolean {
  return validCompatibility(actual) && actual.schemaVersion === expected.schemaVersion &&
    actual.engineVersion === expected.engineVersion && actual.hostAbiVersion === expected.hostAbiVersion &&
    actual.managedVersion === expected.managedVersion && actual.architecture === expected.architecture &&
    actual.engineLibrarySha256 === expected.engineLibrarySha256 && actual.managedRoot === 'share/godot' &&
    actual.templatesRoot === 'share/godot/templates' && typeof actual.templateSetSha256 === 'string' &&
    /^[a-f0-9]{64}$/.test(actual.templateSetSha256);
}
function assetFiles(root: string, manifest: DevelopmentKitManifest): boolean {
  const runtime = `${root}/share/godot`;
  if (!fs.statSync(`${root}/bin/libgodot.so`).isFile()) { return false; }
  if (!fs.statSync(`${runtime}/GodotSharp/Api/Debug/GodotSharp.dll`).isFile()) { return false; }
  for (const name of ['Godot.NET.Sdk', 'Godot.SourceGenerators', 'GodotSharp', 'GodotSharpEditor']) {
    if (!fs.statSync(`${runtime}/GodotSharp/Tools/nupkgs/${name}.${manifest.managedVersion}.nupkg`).isFile()) { return false; }
  }
  const ready = `${runtime}/templates/.ready`;
  const info = fs.statSync(ready);
  if (!info.isFile() || info.size !== 65 || fs.readTextSync(ready, { length: 65 }) !== `${manifest.templateSetSha256}\n`) { return false; }
  return fs.statSync(`${runtime}/templates/openharmony_debug_arm64-v8a.zip`).isFile() &&
    fs.statSync(`${runtime}/templates/openharmony_release_arm64-v8a.zip`).isFile();
}
async function discoverRuntime(context: common.UIAbilityContext): Promise<string> {
  setStep('runtime:discover-development-kit');
  try {
    const bytes = await context.resourceManager.getRawFileContent('development-kit.json');
    const expected = JSON.parse(new util.TextDecoder().decodeWithStream(bytes)) as DevelopmentKitManifest;
    if (!validCompatibility(expected)) { throw new Error('Invalid application development-kit compatibility manifest'); }
    const candidates: DevelopmentKitCandidate[] = plugin.developmentKits();
    for (const candidate of candidates) {
      try {
        if (!candidate.root.startsWith('/') || candidate.root.includes('\0') || candidate.manifest.length === 0) {
          record(`development kit unavailable: ${candidate.root}`);
          continue;
        }
        const actual = JSON.parse(candidate.manifest) as DevelopmentKitManifest;
        if (!compatible(expected, actual)) {
          record(`development kit incompatible with this Editor: ${candidate.root}`);
          continue;
        }
        if (!assetFiles(candidate.root, actual)) {
          record(`development kit assets incomplete: ${candidate.root}`);
          continue;
        }
        record(`using external development kit: ${candidate.root}`);
        return `${candidate.root}/share/godot`;
      } catch (error) {
        record(`development kit rejected (${candidate.root}): ${describe(error as BusinessError)}`);
      }
    }
    record('No compatible godot-editor development kit found; starting the native Editor with development-kit features unavailable');
  } catch (error) {
    record(`Development-kit discovery unavailable; starting the native Editor: ${describe(error as BusinessError)}`);
  }
  return '';
}

export async function prepareRuntime(context: common.UIAbilityContext): Promise<string> {
  if (preparing !== undefined) { return preparing; }
  const operation = discoverRuntime(context);
  preparing = operation;
  try { return await operation; } finally { preparing = undefined; }
}
