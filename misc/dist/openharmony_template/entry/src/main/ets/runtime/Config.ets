// Godot Engine contributors. SPDX-License-Identifier: MIT
import { common, Want } from '@kit.AbilityKit';
import { util } from '@kit.ArkTS';

export interface HostRoleConfig { role: string; }
export interface HostLaunchConfig { defaultMode: string; defaultArguments: string[]; acceptProjectRequests: boolean; }
export interface HostInstancesConfig { policy: string; maxCount: number; }
export interface HostManagedConfig { mode: string; sdkSource: string; }
export interface HostWindowConfig { expandIntoSystemArea: boolean; }
export interface HostDiagnosticsConfig { level: string; }
export interface HostConfig {
  schemaVersion: number;
  host: HostRoleConfig;
  launch: HostLaunchConfig;
  instances: HostInstancesConfig;
  managed: HostManagedConfig;
  window: HostWindowConfig;
  diagnostics: HostDiagnosticsConfig;
}

export interface HostStoragePaths { filesDir: string; cacheDir: string; tempDir: string; }

// Preserve existing user:// roots: exported games historically used application
// storage, while the editor used the UIAbility's module storage. Resolve SDK
// Context paths rather than hardcoding either sandbox layout. Without a valid
// config, diagnostics can safely fall back to this ability's private directory.
export function hostStorage(context: common.UIAbilityContext, config?: HostConfig): HostStoragePaths {
  const storage: common.Context = config?.host.role === 'game' ? context.getApplicationContext() : context;
  return { filesDir: storage.filesDir, cacheDir: storage.cacheDir, tempDir: storage.tempDir };
}

// JSON.parse's type assertion is not validation. Reject missing/unknown keys,
// arrays in place of objects, wrong scalar types and incompatible capabilities.
function requireObject(value: Object, keys: string[], name: string): void {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error(`godot_host.json: ${name} must be an object`);
  }
  const actual = Object.keys(value);
  if (actual.length !== keys.length || actual.some((key: string) => !keys.includes(key))) {
    throw new Error(`godot_host.json: ${name} must contain exactly ${keys.join(', ')}`);
  }
}

export function validateArguments(value: string[], name: string): void {
  if (!Array.isArray(value) || value.some((argument: string) =>
    typeof argument !== 'string' || argument.includes('\0'))) {
    throw new Error(`${name} must be an array of strings without NUL characters`);
  }
}

export function parseHostConfig(text: string): HostConfig {
  const config = JSON.parse(text) as HostConfig;
  requireObject(config, ['schemaVersion', 'host', 'launch', 'instances', 'managed', 'window', 'diagnostics'], 'root');
  requireObject(config.host, ['role'], 'host');
  requireObject(config.launch, ['defaultMode', 'defaultArguments', 'acceptProjectRequests'], 'launch');
  requireObject(config.instances, ['policy', 'maxCount'], 'instances');
  requireObject(config.managed, ['mode', 'sdkSource'], 'managed');
  requireObject(config.window, ['expandIntoSystemArea'], 'window');
  requireObject(config.diagnostics, ['level'], 'diagnostics');
  if (config.schemaVersion !== 1 || !['game', 'editor'].includes(config.host.role) ||
    !['packaged-game', 'project-manager'].includes(config.launch.defaultMode) ||
    !['disabled', 'editor'].includes(config.instances.policy) ||
    !['none', 'sdk'].includes(config.managed.mode) || config.managed.sdkSource !== 'oheco' ||
    !['normal', 'verbose'].includes(config.diagnostics.level) ||
    typeof config.launch.acceptProjectRequests !== 'boolean' ||
    typeof config.window.expandIntoSystemArea !== 'boolean' ||
    !Number.isInteger(config.instances.maxCount) || config.instances.maxCount < 1 || config.instances.maxCount > 5) {
    throw new Error('godot_host.json: invalid schema version, enum, boolean or maxCount (1..5)');
  }
  validateArguments(config.launch.defaultArguments, 'godot_host.json: launch.defaultArguments');
  if (config.host.role === 'game' && (config.launch.defaultMode !== 'packaged-game' ||
    config.launch.acceptProjectRequests || config.instances.policy !== 'disabled' || config.managed.mode !== 'none')) {
    throw new Error('godot_host.json: game requires packaged-game, disabled instances, managed none and no project requests');
  }
  if (config.host.role === 'editor' && config.launch.defaultMode !== 'project-manager') {
    throw new Error('godot_host.json: editor requires project-manager defaultMode');
  }
  return config;
}

export function loadHostConfig(context: common.UIAbilityContext): HostConfig {
  const bytes = context.resourceManager.getRawFileContentSync('godot_host.json');
  return parseHostConfig(new util.TextDecoder().decodeWithStream(bytes));
}

export function instancesEnabled(config: HostConfig): boolean {
  return config.host.role === 'editor' && config.instances.policy === 'editor';
}

function optionName(argument: string): string {
  const equal = argument.indexOf('=');
  return equal < 0 ? argument : argument.substring(0, equal);
}

function isOption(argument: string): boolean {
  // Negative numeric option values are not switches.
  return /^--?[^0-9.]/.test(argument);
}

function separatorIndex(args: string[]): number {
  const index = args.findIndex((argument: string) => argument === '--' || argument === '++');
  return index < 0 ? args.length : index;
}

// Keep flag/value groups together: removing only a duplicate flag would leave
// e.g. a stray "vulkan" positional argument. User arguments stay after '--'.
export function mergeDefaultArguments(defaults: string[], requested: string[]): string[] {
  const defaultEnd = separatorIndex(defaults);
  const requestedEnd = separatorIndex(requested);
  const requestedEngine = requested.slice(0, requestedEnd);
  const result: string[] = [];
  for (let index = 0; index < defaultEnd;) {
    const start = index++;
    if (isOption(defaults[start]) && !defaults[start].includes('=')) {
      while (index < defaultEnd && !isOption(defaults[index])) { index++; }
    }
    const duplicate = isOption(defaults[start]) && requestedEngine.some((argument: string) =>
      isOption(argument) && optionName(argument) === optionName(defaults[start]));
    if (!duplicate) { result.push(...defaults.slice(start, index)); }
  }
  result.push(...requestedEngine);
  if (defaultEnd < defaults.length || requestedEnd < requested.length) {
    result.push('--', ...defaults.slice(defaultEnd + 1), ...requested.slice(requestedEnd + 1));
  }
  return result;
}

export function engineArguments(config: HostConfig, want: Want): string[] {
  if (config.host.role === 'game') { return config.launch.defaultArguments.slice(); }
  if (config.launch.acceptProjectRequests) {
    const explicit = want.parameters?.godotArguments as string[] | undefined;
    if (explicit !== undefined) {
      validateArguments(explicit, 'Want.godotArguments');
      return mergeDefaultArguments(config.launch.defaultArguments, explicit);
    }
    const project = want.parameters?.godotProject as string | undefined;
    if (project !== undefined) {
      if (typeof project !== 'string' || project.length === 0 || project.includes('\0')) {
        throw new Error('Want.godotProject must be a nonempty path without NUL characters');
      }
      return mergeDefaultArguments(config.launch.defaultArguments, ['--path', project, '--editor']);
    }
  }
  return mergeDefaultArguments(config.launch.defaultArguments, ['--project-manager']);
}
