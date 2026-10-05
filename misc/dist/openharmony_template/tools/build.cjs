#!/usr/bin/env node
'use strict';

// This entry belongs to the generated project. No npm, shell, Java, installation,
// network fetch, or modification of the installed adapter/SDK is performed here.
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { StringDecoder } = require('node:string_decoder');

const ADAPTER_VERSION = '6.26.4-ohos.1';
const KEY_ENV = 'OHECO_HVIGOR_KEY_PASSWORD';
const STORE_ENV = 'OHECO_HVIGOR_STORE_PASSWORD';
const REQUEST_KEYS = ['schemaVersion', 'projectDir', 'sdkRoot', 'hvigorEntry', 'buildMode', 'format', 'workDir'];
const SIGNING_KEYS = ['certificate', 'profile', 'storeFile', 'keyAlias', 'signAlg', 'keyPassword', 'storePassword'];
const PROFILE_NAME = 'build-profile.json5';
const BACKUP_NAME = '.godot-build-runner-original.json5';
const LOCK_NAME = '.godot-build-runner.lock';

class RunnerError extends Error {}
function requireValue(condition, message) {
  if (!condition) throw new RunnerError(message);
}
function object(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}
function exactKeys(value, required, optional = []) {
  requireValue(object(value), 'Expected an object in the build request.');
  requireValue(required.every(key => Object.hasOwn(value, key)) &&
    Object.keys(value).every(key => required.includes(key) || optional.includes(key)),
  'Missing or unknown build request fields.');
}
function string(value) {
  return typeof value === 'string' && value.length > 0 && !value.includes('\0');
}
function absolute(value, field) {
  requireValue(string(value) && path.isAbsolute(value), `${field} must be an absolute path without NUL.`);
}
function regularFile(filename, field) {
  let stat;
  try { stat = fs.lstatSync(filename); } catch { throw new RunnerError(`${field} is not an accessible file.`); }
  requireValue(stat.isFile() && !stat.isSymbolicLink(), `${field} must be a regular file, not a symlink.`);
  return stat;
}
function directory(filename, field) {
  let stat;
  try { stat = fs.lstatSync(filename); } catch { throw new RunnerError(`${field} is not an accessible directory.`); }
  requireValue(stat.isDirectory() && !stat.isSymbolicLink(), `${field} must be a directory, not a symlink.`);
  requireValue(fs.realpathSync(filename) === path.resolve(filename), `${field} must not traverse symlinks.`);
  return stat;
}
function ownedPrivate(stat, bits, field) {
  // Reject hmdfs's effective 0660/02770 modes rather than trusting chmod's exit
  // status. The caller must CREATE credentials/cache in a permission-capable FS.
  requireValue(typeof process.getuid === 'function' && stat.uid === process.getuid() &&
    (stat.mode & 0o777) === bits, `${field} must be caller-owned and private (${bits.toString(8)}).`);
}
function privateDirectory(filename, field) {
  ownedPrivate(directory(filename, field), 0o700, field);
}
function jsonDocument(bytes, field, comments = false) {
  let text = bytes.toString('utf8');
  if (comments) {
    // Generated documents use JSON, plus optional comments/trailing commas.
    // Do not eval JSON5 or load code from the project to parse a document.
    text = text.replace(/"(?:[^"\\]|\\.)*"|\/\/[^\r\n]*|\/\*[\s\S]*?\*\//g,
      token => token.startsWith('"') ? token : ' ');
    text = text.replace(/"(?:[^"\\]|\\.)*"|,\s*(?=[}\]])/g,
      token => token.startsWith('"') ? token : '');
  }
  let value;
  try { value = JSON.parse(text); } catch { throw new RunnerError(`${field} is invalid or unsupported JSON.`); }
  requireValue(object(value), `${field} must contain an object.`);
  return value;
}
function readRequest(filename) {
  absolute(filename, 'request path');
  privateDirectory(path.dirname(filename), 'request parent directory');
  const fd = fs.openSync(filename, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW);
  try {
    const stat = fs.fstatSync(fd);
    requireValue(stat.isFile() && stat.nlink === 1 && stat.size <= 1024 * 1024,
      'Request must be a regular, unlinked private JSON file no larger than 1 MiB.');
    ownedPrivate(stat, 0o600, 'request file');
    return jsonDocument(fs.readFileSync(fd), 'request');
  } finally { fs.closeSync(fd); }
}
function validateRequest(request) {
  exactKeys(request, REQUEST_KEYS, ['signing']);
  requireValue(request.schemaVersion === 1, 'Unsupported request schemaVersion (expected 1).');
  for (const field of ['projectDir', 'sdkRoot', 'hvigorEntry', 'workDir']) absolute(request[field], field);
  requireValue(['debug', 'release'].includes(request.buildMode), 'buildMode must be debug or release.');
  requireValue(['hap', 'app'].includes(request.format), 'format must be hap or app.');
  directory(request.projectDir, 'projectDir');
  directory(request.sdkRoot, 'sdkRoot');
  regularFile(request.hvigorEntry, 'hvigorEntry');
  requireValue(request.hvigorEntry.endsWith('.cjs'), 'hvigorEntry must be the adapted .cjs entry, not a bin launcher.');
  privateDirectory(request.workDir, 'workDir');
  const relative = path.relative(request.projectDir, request.workDir);
  requireValue(relative.startsWith(`..${path.sep}`) || relative === '..' || path.isAbsolute(relative),
    'workDir must be outside the generated project.');
  if (Object.hasOwn(request, 'signing')) {
    exactKeys(request.signing, SIGNING_KEYS);
    for (const key of SIGNING_KEYS) requireValue(string(request.signing[key]), `signing.${key} must be a nonempty string without NUL.`);
    for (const key of ['certificate', 'profile', 'storeFile']) {
      absolute(request.signing[key], `signing.${key}`);
      regularFile(request.signing[key], `signing.${key}`);
    }
  }
  return request;
}
function secretsFrom(request) {
  if (!object(request) || !object(request.signing)) return [];
  // Material paths and alias are private too. Never print request contents.
  return [...new Set(SIGNING_KEYS.filter(key => key !== 'signAlg')
    .map(key => request.signing[key]).filter(string))].sort((a, b) => b.length - a.length);
}
function redact(text, secrets) {
  for (const secret of secrets) text = text.split(secret).join('[REDACTED]');
  return text;
}
function redactedStream(destination, secrets) {
  const decoder = new StringDecoder('utf8');
  const keep = Math.max(0, ...secrets.map(secret => secret.length - 1));
  let pending = '';
  function flush(final) {
    while (pending.length > (final ? 0 : keep)) {
      const boundary = final ? pending.length : pending.length - keep;
      let index = -1;
      let match;
      for (const secret of secrets) {
        const found = pending.indexOf(secret);
        if (found >= 0 && (index < 0 || found < index)) { index = found; match = secret; }
      }
      if (index >= 0 && index < boundary) {
        destination.write(pending.slice(0, index) + '[REDACTED]');
        pending = pending.slice(index + match.length);
      } else {
        destination.write(pending.slice(0, boundary));
        pending = pending.slice(boundary);
      }
    }
  }
  return {
    write(bytes) { pending += decoder.write(bytes); flush(false); },
    end() { pending += decoder.end(); flush(true); },
  };
}
function childEnvironment(request) {
  const env = { ...process.env };
  delete env.JAVA_HOME;
  // Do not carry unrelated signing secrets into an unsigned build.
  delete env[KEY_ENV];
  delete env[STORE_ENV];
  if (env.DEVECO_SDK_HOME) {
    try {
      if (fs.realpathSync(env.DEVECO_SDK_HOME) !== fs.realpathSync(request.sdkRoot)) delete env.DEVECO_SDK_HOME;
    } catch { delete env.DEVECO_SDK_HOME; }
  }
  env.OHOS_SDK_HOME = request.sdkRoot;
  env.OHOS_BASE_SDK_HOME = request.sdkRoot;
  env.HVIGOR_USER_HOME = request.workDir;
  for (const [key, name] of [['TMPDIR', 'tmp'], ['HOME', 'home']]) {
    const target = path.join(request.workDir, name);
    fs.mkdirSync(target, { mode: 0o700, recursive: true });
    privateDirectory(target, `child ${key}`);
    env[key] = target;
  }
  env.TMP = env.TEMP = env.TMPDIR;
  env.npm_config_offline = env.NPM_CONFIG_OFFLINE = 'true';
  // HOME is child-only; execPath, NODE_PATH and the adapter's own dependency
  // resolution remain unchanged. Do not invoke npm/ohpm to fill missing inputs.
  if (request.signing) {
    env[KEY_ENV] = request.signing.keyPassword;
    env[STORE_ENV] = request.signing.storePassword;
  }
  return env;
}
function execute(request, args, env, secrets, probe = false) {
  return new Promise((resolve, reject) => {
    const out = redactedStream(process.stdout, secrets);
    const err = redactedStream(process.stderr, secrets);
    let captured = '';
    let overflow = false;
    let timer;
    const child = spawn(process.execPath, [request.hvigorEntry, ...args], {
      cwd: request.projectDir, env, shell: false, stdio: ['ignore', 'pipe', 'pipe'],
    });
    const forward = signal => child.kill(signal);
    const handlers = { SIGINT: () => forward('SIGINT'), SIGTERM: () => forward('SIGTERM') };
    for (const [signal, handler] of Object.entries(handlers)) process.on(signal, handler);
    child.stdout.on('data', bytes => {
      if (!probe) return out.write(bytes);
      if (Buffer.byteLength(captured) + bytes.length > 1024 * 1024) {
        overflow = true; child.kill('SIGTERM');
      } else { captured += bytes.toString('utf8'); }
    });
    child.stderr.on('data', bytes => err.write(bytes));
    if (probe) timer = setTimeout(() => { overflow = true; child.kill('SIGTERM'); }, 15000);
    let failed = false;
    child.on('error', () => { failed = true; });
    child.on('close', (code, signal) => {
      clearTimeout(timer);
      for (const [name, handler] of Object.entries(handlers)) process.off(name, handler);
      out.end(); err.end();
      if (failed || overflow || code !== 0 || signal) {
        reject(new RunnerError(probe ? 'Adapter preflight failed.' : 'Hvigor build failed; no result was produced.'));
      } else { resolve(captured); }
    });
  });
}
async function verifyAdapter(request, env, secrets) {
  const output = await execute(request, ['--adapter-info'], env, secrets, true);
  const info = jsonDocument(Buffer.from(output), 'adapter-info');
  requireValue(info.adapter === ADAPTER_VERSION && info.host === 'openharmony' &&
    info.arch === 'arm64' && info.upstream === '6.26.4' && info.pinnedInterfacesUnmodified === true,
  'Requires unmodified @oheco/hvigor 6.26.4-ohos.1 on OpenHarmony arm64.');
}
function localDependencies(filename, field, empty = false) {
  regularFile(filename, field);
  const doc = jsonDocument(fs.readFileSync(filename), field, true);
  for (const key of ['dependencies', 'devDependencies', 'dynamicDependencies']) {
    if (!Object.hasOwn(doc, key)) continue;
    requireValue(object(doc[key]), `${field} has invalid dependencies.`);
    for (const value of Object.values(doc[key])) {
      requireValue(!empty && string(value) && value.startsWith('file:'),
        'Offline runner requires pre-staged local dependencies; registry resolution is not allowed.');
      requireValue(fs.existsSync(path.resolve(path.dirname(filename), value.slice(5))),
        'A pre-staged local dependency is missing.');
    }
  }
  return doc;
}
function prepareProfile(request) {
  const root = request.projectDir;
  localDependencies(path.join(root, 'oh-package.json5'), 'project package');
  localDependencies(path.join(root, 'entry/oh-package.json5'), 'entry package');
  const config = localDependencies(path.join(root, 'hvigor/hvigor-config.json5'), 'hvigor config', true);
  requireValue(!config.buildCacheDir, 'Custom build cache directories are not supported.');
  const filename = path.join(root, PROFILE_NAME);
  regularFile(filename, 'generated build-profile');
  const original = fs.readFileSync(filename);
  const doc = jsonDocument(original, 'generated build-profile', true);
  requireValue(object(doc.app) && Array.isArray(doc.app.products), 'Generated profile requires app.products.');
  const products = doc.app.products.filter(product => object(product) && product.name === 'default');
  requireValue(products.length === 1, 'Generated profile requires exactly one default product.');
  requireValue(Array.isArray(doc.modules) && doc.modules.length === 1 && doc.modules[0].name === 'entry' &&
    ['./entry', 'entry'].includes(doc.modules[0].srcPath), 'Runner supports only the generated entry module.');
  const product = products[0];
  doc.app.signingConfigs = [];
  delete product.signingConfig;
  if (request.signing) {
    const signing = request.signing;
    doc.app.signingConfigs = [{ name: 'oheco-build-runner', type: 'OpenHarmony', material: {
      certpath: signing.certificate, profile: signing.profile, storeFile: signing.storeFile,
      keyAlias: signing.keyAlias, signAlg: signing.signAlg,
      keyPassword: `env:${KEY_ENV}`, storePassword: `env:${STORE_ENV}`,
    } }];
    product.signingConfig = 'oheco-build-runner';
  }
  const allSigned = request.format === 'app' && Boolean(request.signing);
  if (allSigned) {
    // A signed APP export promises signed inner packages, not merely a signed
    // outer ZIP. Override only this temporary selected-product profile; the
    // caller's original bytes/options are restored after every outcome.
    if (!Object.hasOwn(product, 'buildOption')) product.buildOption = {};
    requireValue(object(product.buildOption), 'Invalid selected product buildOption for signed APP.');
    if (!Object.hasOwn(product.buildOption, 'packOptions')) product.buildOption.packOptions = {};
    requireValue(object(product.buildOption.packOptions), 'Invalid selected product packOptions for signed APP.');
    product.buildOption.packOptions.appWithSignedPkg = true;
    product.buildOption.packOptions.buildAppSkipSignHap = false;
  }
  return { bytes: Buffer.from(JSON.stringify(doc, null, 2) + '\n'), allSigned };
}
function artifactDirectory(request) {
  return request.format === 'hap' ? path.join(request.projectDir, 'entry/build/default/outputs/default') :
    path.join(request.projectDir, 'build/outputs/default');
}
function removeStaleArtifacts(request) {
  const target = artifactDirectory(request);
  if (!fs.existsSync(target)) return;
  directory(target, 'artifact directory');
  for (const name of fs.readdirSync(target)) {
    if (!name.endsWith(`.${request.format}`)) continue;
    const filename = path.join(target, name);
    regularFile(filename, 'previous artifact');
    fs.unlinkSync(filename);
  }
}
function selectArtifact(request, allSigned) {
  const target = artifactDirectory(request);
  directory(target, 'artifact directory');
  const suffix = request.signing ? (allSigned ? '-all-signed' : '-signed') : '-unsigned';
  const candidates = fs.readdirSync(target).filter(name => name.endsWith(`${suffix}.${request.format}`) &&
    (allSigned || !name.endsWith(`-all-signed.${request.format}`)));
  requireValue(candidates.length === 1, 'Expected exactly one matching signed/unsigned artifact in the default output directory.');
  const outputPath = path.join(target, candidates[0]);
  requireValue(regularFile(outputPath, 'output artifact').size > 0, 'Output artifact is empty.');
  requireValue(fs.realpathSync(outputPath) === outputPath, 'Output artifact must not traverse symlinks.');
  return outputPath;
}
function buildArguments(request) {
  const args = ['--mode', request.format === 'hap' ? 'module' : 'project'];
  if (request.format === 'hap') args.push('-p', 'module=entry@default');
  args.push('-p', 'product=default', '-p', `buildMode=${request.buildMode}`,
    request.format === 'hap' ? 'assembleHap' : 'assembleApp', '--no-daemon', '--no-incremental');
  // Fixed 6.26.4 SignPackagesFromApp's worker-rejection fallback never resolves
  // its success Promise. Enable worker dispatch for all-signed APPs, as in the
  // adapter's native e2e matrix; do not patch official packages or use a daemon.
  if (request.format === 'app' && request.signing) args.push('--parallel');
  return args;
}
async function run(filename) {
  let secrets = [];
  let request;
  let projectLock;
  let workLock;
  let backup;
  let profile;
  let result;
  try {
    request = readRequest(filename);
    secrets = secretsFrom(request);
    validateRequest(request);
    const workLockPath = path.join(request.workDir, '.runner-lock');
    fs.mkdirSync(workLockPath, { mode: 0o700 });
    workLock = workLockPath;
    // A failure must never leave a success result from an earlier invocation.
    const resultPath = path.join(request.workDir, 'result.json');
    if (fs.existsSync(resultPath)) { regularFile(resultPath, 'previous result'); fs.unlinkSync(resultPath); }
    const env = childEnvironment(request);
    await verifyAdapter(request, env, secrets);
    const prepared = prepareProfile(request);
    const projectLockPath = path.join(request.projectDir, LOCK_NAME);
    fs.mkdirSync(projectLockPath);
    projectLock = projectLockPath;
    const backupPath = path.join(request.projectDir, BACKUP_NAME);
    requireValue(!fs.existsSync(backupPath), 'An interrupted build profile backup exists; restore it before retrying.');
    profile = path.join(request.projectDir, PROFILE_NAME);
    // Same-filesystem rename protects original bytes even if writing the
    // generated profile or restoring by a content write would fail with ENOSPC.
    fs.renameSync(profile, backupPath);
    backup = backupPath;
    try {
      fs.writeFileSync(profile, prepared.bytes, { flag: 'wx', mode: 0o600 });
      removeStaleArtifacts(request);
      await execute(request, buildArguments(request), env, secrets);
      result = { schemaVersion: 1, outputPath: selectArtifact(request, prepared.allSigned),
        mode: request.buildMode, format: request.format, signed: Boolean(request.signing) };
    } finally {
      fs.renameSync(backup, profile);
      backup = undefined;
    }
    // Write only after the original profile has been restored successfully.
    fs.writeFileSync(resultPath, JSON.stringify(result, null, 2) + '\n', { flag: 'wx', mode: 0o600 });
    return result;
  } catch (error) {
    // Do not print native filesystem/JSON exceptions: they may include paths or
    // request/password fragments. Public errors never interpolate input values.
    const message = error instanceof RunnerError ? error.message :
      'Build runner filesystem/process failure; check private paths, locks and any retained profile backup.';
    process.stderr.write(`build runner: ${redact(message, secrets)}\n`);
    throw new RunnerError('Build runner failed.');
  } finally {
    // Leave the project lock in place if restoration failed, for crash recovery.
    if (projectLock && !backup) fs.rmdirSync(projectLock);
    if (workLock) fs.rmdirSync(workLock);
  }
}

if (require.main === module) {
  if (process.argv.length !== 4 || process.argv[2] !== '--request') {
    process.stderr.write('Usage: node <project>/tools/build.cjs --request <absolute-private-json>\n');
    process.exitCode = 1;
  } else { run(process.argv[3]).catch(() => { process.exitCode = 1; }); }
}
module.exports = { run, buildArguments, redact, redactedStream, ADAPTER_VERSION };
