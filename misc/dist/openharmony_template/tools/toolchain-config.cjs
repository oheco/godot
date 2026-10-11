'use strict';

// Inspect the caller-selected installation and already prepared wrapper cache.
// Never load installed JavaScript here, create cache entries, or install packages.
const fs = require('node:fs');
const path = require('node:path');
const MIN_HVIGOR_VERSION = '7.26.8';
const MIN_NODE_VERSION = '24.14.1';
const DEFAULT_OHECO_ROOT = '/storage/Users/currentUser/.oheco';
class ToolchainError extends Error {}
function requireValue(condition, message) { if (!condition) throw new ToolchainError(message); }
function version(value) {
  requireValue(typeof value === 'string' && /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/.test(value),
    'Toolchain package versions must be stable three-part versions.');
  return value.split('.').map(Number);
}
function atLeast(actual, minimum) {
  const left = version(actual), right = version(minimum);
  for (let i = 0; i < 3; ++i) if (left[i] !== right[i]) return left[i] > right[i];
  return true;
}
function file(filename, field) {
  let stat;
  try { stat = fs.statSync(filename); } catch { throw new ToolchainError(`${field} is missing or inaccessible.`); }
  requireValue(stat.isFile(), `${field} must be a file.`);
}
function manifest(filename, name) {
  file(filename, 'Toolchain package manifest');
  let doc;
  try { doc = JSON.parse(fs.readFileSync(filename, 'utf8')); } catch { throw new ToolchainError('Invalid toolchain package manifest.'); }
  requireValue(doc && doc.name === name, 'Unexpected toolchain package identity.');
  version(doc.version);
  return doc;
}
function inside(root, target) {
  const relative = path.relative(root, target);
  return relative === '' || (!relative.startsWith(`..${path.sep}`) && relative !== '..' && !path.isAbsolute(relative));
}
function hvigorUserHome(request, env = process.env) {
  const root = env.OHECO_ROOT || DEFAULT_OHECO_ROOT;
  const selected = request.hvigorUserHome ?? env.HVIGOR_USER_HOME ?? path.join(path.dirname(root), '.hvigor');
  requireValue(typeof selected === 'string' && selected.length > 0 && !selected.includes('\0') && path.isAbsolute(selected),
    'HVIGOR_USER_HOME must be a nonempty absolute path.');
  return path.resolve(selected);
}
function resolveToolchain(request) {
  const root = request.commandLineToolsRoot;
  const selectedNode = path.join(root, 'node/bin/node');
  const wrapperJS = path.join(root, 'hvigor/bin/hvigorw.js');
  file(selectedNode, 'Bundled Node');
  file(wrapperJS, 'Official Hvigor wrapper');
  file(path.join(root, 'hvigor/bin/hvigorw'), 'Official wrapper shell launcher');
  const engine = manifest(path.join(root, 'hvigor/hvigor/package.json'), '@ohos/hvigor');
  const plugin = manifest(path.join(root, 'hvigor/hvigor-ohos-plugin/package.json'), '@ohos/hvigor-ohos-plugin');
  requireValue(engine.version === plugin.version && version(engine.version)[0] === 7 && atLeast(engine.version, MIN_HVIGOR_VERSION),
    'Requires coherent official Hvigor/plugin major 7, version 7.26.8 or newer.');
  requireValue(plugin.peerDependencies?.['@ohos/hvigor'] === engine.version,
    'Official plugin peer dependency does not match the bundled Hvigor engine.');
  // Read the actual assignment; a hardcoded cache version can trigger downloads
  // after an installation upgrade. Conflicting/missing assignments fail closed.
  const assignments = [...fs.readFileSync(wrapperJS, 'utf8').matchAll(/\b(?:[A-Za-z_$][\w$]*\.)?PNPM_VERSION\s*=\s*["'](\d+\.\d+\.\d+)["']/g)];
  const versions = [...new Set(assignments.map(match => match[1]))];
  requireValue(versions.length === 1, 'Cannot determine the official wrapper PNPM_VERSION assignment.');
  const pnpmVersion = versions[0];
  version(pnpmVersion);
  const home = hvigorUserHome(request);
  const toolsDir = path.join(home, 'wrapper/tools', pnpmVersion);
  file(path.join(toolsDir, 'node_modules/.bin/pnpm'), 'Prepared wrapper pnpm launcher');
  let pnpmEntry, pnpmManifest;
  try {
    pnpmEntry = require.resolve('pnpm', { paths: [toolsDir] });
    // pnpm 10 exports only "." as package.json; its package.json subpath is
    // intentionally not exported. Inspect the local manifest without resolving
    // that forbidden subpath or executing pnpm's CLI.
    pnpmManifest = path.join(toolsDir, 'node_modules/pnpm/package.json');
    const canonicalTools = fs.realpathSync(toolsDir);
    requireValue(inside(canonicalTools, fs.realpathSync(pnpmEntry)) && inside(canonicalTools, fs.realpathSync(pnpmManifest)),
      'Prepared pnpm must resolve inside the selected wrapper tools directory.');
  } catch (error) {
    if (error instanceof ToolchainError) throw error;
    throw new ToolchainError('Prepared wrapper pnpm package is missing or cannot be resolved.');
  }
  requireValue(manifest(pnpmManifest, 'pnpm').version === pnpmVersion,
    'Prepared wrapper pnpm package version does not match PNPM_VERSION.');
  return { selectedNode, wrapperJS, hvigorUserHome: home, toolsDir, pnpmVersion, version: engine.version,
    decipherModule: path.join(root, 'hvigor/hvigor-ohos-plugin/src/utils/decipher-util.js') };
}
module.exports = { resolveToolchain, hvigorUserHome, atLeast, MIN_NODE_VERSION, MIN_HVIGOR_VERSION, DEFAULT_OHECO_ROOT, ToolchainError };
