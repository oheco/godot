#!/usr/bin/env node
'use strict';

// Use the installed official wrapper with prepared offline inputs. Only this
// runner's outer spawn is shell-free; official descendant tools may use shells.
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { StringDecoder } = require('node:string_decoder');
const { resolveSdk, api: sdkApi } = require('./sdk-config.cjs');

const { createHash } = require('node:crypto');
const { resolveToolchain, atLeast, MIN_NODE_VERSION, ToolchainError } = require('./toolchain-config.cjs');
const REQUEST_KEYS = ['schemaVersion', 'projectDir', 'sdkRoot', 'commandLineToolsRoot', 'buildMode', 'format', 'workDir'];
const SIGNING_KEYS = ['certificate', 'profile', 'storeFile', 'keyAlias', 'signAlg', 'keyPasswordEncrypted', 'storePasswordEncrypted'];
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
      'Request must be a regular private JSON file with one hard link, no larger than 1 MiB.');
    ownedPrivate(stat, 0o600, 'request file');
    return jsonDocument(fs.readFileSync(fd), 'request');
  } finally { fs.closeSync(fd); }
}
function validateRequest(request) {
  exactKeys(request, REQUEST_KEYS, ['signing', 'hvigorUserHome']);
  requireValue(request.schemaVersion === 2, 'Unsupported request schemaVersion (expected 2).');
  for (const field of ['projectDir', 'sdkRoot', 'commandLineToolsRoot', 'workDir']) absolute(request[field], field);
  if (Object.hasOwn(request, 'hvigorUserHome')) absolute(request.hvigorUserHome, 'hvigorUserHome');
  requireValue(['debug', 'release'].includes(request.buildMode), 'buildMode must be debug or release.');
  requireValue(['hap', 'app'].includes(request.format), 'format must be hap or app.');
  directory(request.projectDir, 'projectDir');
  directory(fs.realpathSync(request.sdkRoot), 'sdkRoot');
  directory(request.commandLineToolsRoot, 'commandLineToolsRoot');
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
    for (const key of ['keyPasswordEncrypted', 'storePasswordEncrypted']) encryptedPassword(request.signing[key]);
    const material = path.join(path.dirname(request.signing.storeFile), 'material');
    directory(material, 'Signing companion material');
    for (const name of ['fd', 'ac', 'ce']) {
      const target = path.join(material, name);
      directory(target, 'Signing companion material directory');
      requireValue(fs.readdirSync(target).some(entry => entry !== '.DS_Store'), 'Signing companion material directory is empty.');
    }
  }
  return request;
}
function encryptedPassword(value) {
  requireValue(/^[0-9a-fA-F]+$/.test(value) && value.length % 2 === 0 && value.length >= 64 && value.length <= 2 * 1024 * 1024,
    'Signing password must be an official encrypted hexadecimal frame.');
  const bytes = Buffer.from(value, 'hex');
  // Documented frame: uint32 BE content length (ciphertext + 16-byte auth tag),
  // 12-byte IV, then content. The original hex representation is never changed.
  requireValue(bytes.readUInt32BE(0) >= 16 && bytes.readUInt32BE(0) === bytes.length - 16,
    'Signing password encrypted frame has an invalid content length.');
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
function childEnvironment(request, selection, toolchain) {
  const env = { ...process.env };
  for (const key of Object.keys(env)) {
    if (/^(?:HVIGOR_|OHECO_HVIGOR_)/i.test(key) ||
      /^(?:NODE_OPTIONS|NODE_PATH|NODE_OPTS|NODE_HOME|JAVA_HOME|DEVECO_NODE_HOME|DEVECO_HVIGOR_HOME|OHPM_HOME|CANGJIE_BUILD_SUPPORT_PATH|DEVECO_CANGJIE_PATH|DEVECO_HVIGOR_CANGJIE_PLUGIN|WORKSPACE_DIR)$/i.test(key)) delete env[key];
  }
  if (selection.runtimeOS === 'HarmonyOS') {
    env.DEVECO_SDK_HOME = selection.sdkRoot;
    delete env.OHOS_SDK_HOME;
    delete env.OHOS_BASE_SDK_HOME;
  } else {
    if (env.DEVECO_SDK_HOME) {
      try {
        if (fs.realpathSync(env.DEVECO_SDK_HOME) !== selection.sdkRoot) delete env.DEVECO_SDK_HOME;
      } catch { delete env.DEVECO_SDK_HOME; }
    }
    env.OHOS_SDK_HOME = selection.sdkRoot;
    env.OHOS_BASE_SDK_HOME = selection.sdkRoot;
  }
  env.HVIGOR_USER_HOME = toolchain.hvigorUserHome;
  env.NODE_HOME = path.dirname(toolchain.selectedNode);
  env.PATH = path.dirname(toolchain.selectedNode) + path.delimiter + (env.PATH || '');
  for (const [key, name] of [['TMPDIR', 'tmp'], ['HOME', 'home']]) {
    const target = path.join(request.workDir, name);
    fs.mkdirSync(target, { mode: 0o700, recursive: true });
    privateDirectory(target, `child ${key}`);
    env[key] = target;
  }
  env.TMP = env.TEMP = env.TMPDIR;
  env.npm_config_offline = env.NPM_CONFIG_OFFLINE = 'true';
  // HOME/TMP are child-only; the prepared wrapper cache stays persistent.
  return env;
}
// Probe payload travels on stdin, never argv/env. Response uses a dedicated pipe
// so plugin logging cannot become a successful response. All diagnostics are
// captured and discarded, including a logger that calls process.exit().
const NODE_PROBE = String.raw`
'use strict';
const fs = require('node:fs');
try {
  const input = JSON.parse(fs.readFileSync(0, 'utf8'));
  let result;
  if (input.kind === 'version') {
    result = {version:process.versions.node, pnpm:require.resolve('pnpm',{paths:[input.toolsDir]})};
  } else {
    // The wrapper normally links this peer in its workspace. Before the
    // wrapper runs, bind just that peer to the selected bundled engine using
    // Node's public synchronous resolver hook; no SDK or module tree changes.
    const engine = require.resolve(input.engine);
    require('node:module').registerHooks({resolve(specifier, context, next) {
      return next(specifier === '@ohos/hvigor' ? engine : specifier, context);
    }});
    const {DecipherUtil} = require(input.module);
    if (typeof DecipherUtil?.decryptPwd !== 'function') throw new Error();
    result = {passwords:input.encrypted.map(value => DecipherUtil.decryptPwd(input.materialDir,value,input.profilePath))};
  }
  fs.writeSync(3, JSON.stringify(result));
} catch { process.exitCode = 1; }
`;
function execute(request, toolchain, args, env, secrets, payload) {
  return new Promise((resolve, reject) => {
    const probe = payload !== undefined;
    const out = redactedStream(process.stdout, secrets);
    const err = redactedStream(process.stderr, secrets);
    const response = [];
    let capturedBytes = 0, overflow = false, interrupted = false, failed = false;
    let timer;
    const child = spawn(toolchain.selectedNode, probe ? ['--eval', NODE_PROBE] : ['--', toolchain.wrapperJS, ...args], {
      cwd: request.projectDir, env, shell: false, detached: process.platform !== 'win32',
      stdio: probe ? ['pipe', 'pipe', 'pipe', 'pipe'] : ['ignore', 'pipe', 'pipe'],
    });
    const forward = signal => {
      try {
        if (process.platform !== 'win32' && child.pid) process.kill(-child.pid, signal);
        else child.kill(signal);
      } catch {}
    };
    const handlers = Object.fromEntries(['SIGINT','SIGTERM'].map(signal => [signal, () => {
      interrupted = true; forward(signal);
    }]));
    for (const [signal, handler] of Object.entries(handlers)) process.on(signal, handler);
    function capture(bytes, result = false) {
      capturedBytes += bytes.length;
      if (capturedBytes > 1024 * 1024) { overflow = true; forward('SIGKILL'); }
      else if (result) response.push(bytes);
    }
    child.stdout.on('data', bytes => probe ? capture(bytes) : out.write(bytes));
    child.stderr.on('data', bytes => probe ? capture(bytes) : err.write(bytes));
    if (probe) {
      child.stdio[3].on('data', bytes => capture(bytes, true));
      child.stdin.on('error', () => { failed = true; });
      child.stdin.end(JSON.stringify(payload));
      timer = setTimeout(() => { overflow = true; forward('SIGKILL'); }, 15000);
    }
    child.on('error', () => { failed = true; });
    child.on('close', (code, signal) => {
      clearTimeout(timer);
      for (const [name, handler] of Object.entries(handlers)) process.off(name, handler);
      out.end(); err.end();
      if (failed || overflow || interrupted || code !== 0 || signal) {
        reject(new RunnerError(probe ? 'Official toolchain private preflight failed.' : 'Hvigor build failed; no result was produced.'));
      } else { resolve(Buffer.concat(response)); }
    });
  });
}
async function verifyToolchain(request, toolchain, env) {
  let info;
  try {
    info = JSON.parse(await execute(request, toolchain, [], env, [], {kind:'version', toolsDir:toolchain.toolsDir}));
    requireValue(typeof info.version === 'string' && atLeast(info.version, MIN_NODE_VERSION), 'Bundled Node must be version 24.14.1 or newer.');
    const relative = path.relative(fs.realpathSync(toolchain.toolsDir), fs.realpathSync(info.pnpm));
    requireValue(relative !== '..' && !relative.startsWith(`..${path.sep}`) && !path.isAbsolute(relative),
      'Bundled Node cannot resolve the prepared wrapper pnpm package.');
  } catch (error) {
    if (error instanceof RunnerError || error instanceof ToolchainError) throw error;
    throw new RunnerError('Official toolchain private preflight failed.');
  }
}
async function plaintextForRedaction(request, toolchain, env) {
  if (!request.signing) return [];
  const signing = request.signing;
  try {
    regularFile(toolchain.decipherModule, 'Official password decrypt module');
    const response = await execute(request, toolchain, [], env, [], {kind:'decrypt', module:toolchain.decipherModule,
      engine:path.join(request.commandLineToolsRoot, 'hvigor/hvigor'), materialDir:path.dirname(signing.storeFile), profilePath:path.join(request.projectDir, PROFILE_NAME),
      encrypted:[signing.keyPasswordEncrypted, signing.storePasswordEncrypted]});
    const {passwords} = JSON.parse(response);
    requireValue(Array.isArray(passwords) && passwords.length === 2 &&
      passwords.every(value => typeof value === 'string' && !value.includes('\0')), 'Invalid private decrypt response.');
    return passwords;
  } catch { throw new RunnerError('Official signing password preflight failed.'); }
}
function localDependencies(filename, field, empty = false) {
  regularFile(filename, field);
  const doc = jsonDocument(fs.readFileSync(filename), field, true);
  requireValue(!Object.hasOwn(doc, 'dynamicDependencies') && !Object.hasOwn(doc, 'scripts'),
    'Dynamic dependencies and package scripts are not supported.');
  for (const key of ['dependencies', 'devDependencies']) {
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
function checkSdkProperty(root, selection) {
  const filename = path.join(root, 'local.properties');
  if (selection.runtimeOS !== 'OpenHarmony' || !fs.existsSync(filename)) return;
  const decode = value => value.replace(/\\u([0-9a-fA-F]{4})|\\(.)/g,
    (_, unicode, escaped) => unicode ? String.fromCharCode(parseInt(unicode, 16)) : ({t:'\t', r:'\r', n:'\n', f:'\f'}[escaped] || escaped));
  const text = fs.readFileSync(filename, 'utf8').replace(/(?<!\\)\\\r?\n[ \t]*/g, '');
  let configured;
  for (const line of text.split(/\r?\n/)) {
    const match = /^\s*([^#!=: \t]+)\s*[=: \t]\s*(.*)$/.exec(line);
    if (match && decode(match[1]) === 'sdk.dir') configured = decode(match[2]);
  }
  if (configured !== undefined) {
    let matches = false;
    try { matches = fs.realpathSync(path.resolve(root, configured)) === selection.sdkRoot; } catch {}
    requireValue(matches, 'local.properties sdk.dir conflicts with the selected SDK root. Update it before building.');
  }
}
function stockTasks(root) {
  for (const [relative, task] of [['hvigorfile.ts','appTasks'], ['entry/hvigorfile.ts','hapTasks']]) {
    const filename = path.join(root, relative);
    regularFile(filename, 'Generated Hvigor task file');
    // Accept the stock static template with whitespace/comments; no TS eval.
    const text = fs.readFileSync(filename, 'utf8').replace(/\/\/[^\r\n]*|\/\*[\s\S]*?\*\//g, '').replace(/\s+/g, '');
    const pattern = new RegExp(`^import\\{${task}\\}from['\"]@ohos/hvigor-ohos-plugin['\"];?exportdefault\\{system:${task},plugins:\\[\\],?\\};?$`);
    requireValue(pattern.test(text), 'Only stock static Hvigor tasks without custom plugins are supported.');
  }
}
function hvigorConfig(filename) {
  const config = localDependencies(filename, 'hvigor config', true);
  exactKeys(config, [], ['modelVersion', 'dependencies', 'devDependencies', 'execution']);
  if (Object.hasOwn(config, 'execution')) {
    exactKeys(config.execution, [], ['daemon', 'incremental', 'parallel']);
    requireValue(Object.values(config.execution).every(value => typeof value === 'boolean'), 'Invalid Hvigor execution options.');
  }
  return config;
}
function wrapperConfiguration(request, toolchain) {
  const globalConfig = path.join(toolchain.hvigorUserHome, 'hvigor-config.json5');
  if (fs.existsSync(globalConfig)) hvigorConfig(globalConfig);
  // The official major-7 wrapper hashes the absolute project path with SHA256.
  // An old nonempty workspace manifest otherwise makes even empty project deps
  // enter its installer. Reject it instead of modifying the persistent cache.
  const hash = createHash('sha256').update(path.resolve(request.projectDir)).digest('hex').slice(0, 32);
  const workspace = path.join(toolchain.hvigorUserHome, 'project_caches', hash, 'workspace');
  const manifest = path.join(workspace, 'package.json');
  if (fs.existsSync(manifest)) localDependencies(manifest, 'wrapper workspace package', true);
}
function prepareProfile(request) {
  const root = request.projectDir;
  localDependencies(path.join(root, 'oh-package.json5'), 'project package');
  localDependencies(path.join(root, 'entry/oh-package.json5'), 'entry package');
  hvigorConfig(path.join(root, 'hvigor/hvigor-config.json5'));
  stockTasks(root);
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
  const compatibleApi = typeof product.compatibleSdkVersion === 'number' ? product.compatibleSdkVersion : sdkApi(product.compatibleSdkVersion);
  const sdkSelection = resolveSdk(product.compileSdkVersion, compatibleApi, request.sdkRoot);
  checkSdkProperty(root, sdkSelection);
  requireValue(product.runtimeOS === sdkSelection.runtimeOS,
    'Generated runtimeOS does not match the selected SDK metadata. Regenerate the project with the selected SDK.');
  for (const field of ['compileSdkVersion', 'targetSdkVersion', 'compatibleSdkVersion', 'runtimeOS']) product[field] = sdkSelection[field];
  doc.app.signingConfigs = [];
  delete product.signingConfig;
  if (request.signing) {
    const signing = request.signing;
    doc.app.signingConfigs = [{ name: 'oheco-build-runner', type: sdkSelection.runtimeOS, material: {
      certpath: signing.certificate, profile: signing.profile, storeFile: signing.storeFile,
      keyAlias: signing.keyAlias, signAlg: signing.signAlg,
      keyPassword: signing.keyPasswordEncrypted, storePassword: signing.storePasswordEncrypted,
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
  return { bytes: Buffer.from(JSON.stringify(doc, null, 2) + '\n'), allSigned, sdkSelection };
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
  // No major-7 verification establishes that the old adapter's --parallel
  // workaround is still required. Signed APP options remain explicit above.
  return args;
}
async function run(filename) {
  // Keep a handler through restoration, including gaps between child processes.
  // execute() forwards active signals; this guard also cancels a completed child
  // whose signal arrives before its profile transaction/result is finalized.
  let interrupted = false;
  const rememberSignal = () => { interrupted = true; };
  for (const signal of ['SIGINT','SIGTERM']) process.on(signal, rememberSignal);
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
    const toolchain = resolveToolchain(request);
    wrapperConfiguration(request, toolchain);
    // Lock before reading the profile: another workDir must not snapshot a
    // concurrent build's temporary signing/pack options during private probes.
    const projectLockPath = path.join(request.projectDir, LOCK_NAME);
    fs.mkdirSync(projectLockPath);
    projectLock = projectLockPath;
    const backupPath = path.join(request.projectDir, BACKUP_NAME);
    requireValue(!fs.existsSync(backupPath), 'An interrupted build profile backup exists; restore it before retrying.');
    const prepared = prepareProfile(request);
    const env = childEnvironment(request, prepared.sdkSelection, toolchain);
    await verifyToolchain(request, toolchain, env);
    requireValue(!interrupted, 'Build runner was interrupted; no result was produced.');
    secrets = [...new Set([...secrets, ...await plaintextForRedaction(request, toolchain, env)].filter(string))]
      .sort((a, b) => b.length - a.length);
    requireValue(!interrupted, 'Build runner was interrupted; no result was produced.');
    profile = path.join(request.projectDir, PROFILE_NAME);
    // Same-filesystem rename protects original bytes even if writing the
    // generated profile or restoring by a content write would fail with ENOSPC.
    fs.renameSync(profile, backupPath);
    backup = backupPath;
    try {
      fs.writeFileSync(profile, prepared.bytes, { flag: 'wx', mode: 0o600 });
      removeStaleArtifacts(request);
      await execute(request, toolchain, buildArguments(request), env, secrets);
      result = { schemaVersion: 2, outputPath: selectArtifact(request, prepared.allSigned),
        mode: request.buildMode, format: request.format, signed: Boolean(request.signing) };
    } finally {
      fs.renameSync(backup, profile);
      backup = undefined;
    }
    // Write only after the original profile has been restored successfully.
    requireValue(!interrupted, 'Build runner was interrupted; no result was produced.');
    fs.writeFileSync(resultPath, JSON.stringify(result, null, 2) + '\n', { flag: 'wx', mode: 0o600 });
    return result;
  } catch (error) {
    // Do not print native filesystem/JSON exceptions: they may include paths or
    // request/password fragments. Public errors never interpolate input values.
    const message = error instanceof RunnerError || error instanceof ToolchainError ? error.message :
      'Build runner filesystem/process failure; check private paths, locks and any retained profile backup.';
    process.stderr.write(`build runner: ${redact(message, secrets)}\n`);
    throw new RunnerError('Build runner failed.');
  } finally {
    try {
      // Leave the project lock in place if restoration failed, for crash recovery.
      if (projectLock && !backup) fs.rmdirSync(projectLock);
      if (workLock) fs.rmdirSync(workLock);
    } finally {
      for (const signal of ['SIGINT','SIGTERM']) process.off(signal, rememberSignal);
    }
  }
}

if (require.main === module) {
  if (process.argv.length !== 4 || process.argv[2] !== '--request') {
    process.stderr.write('Usage: node <project>/tools/build.cjs --request <absolute-private-json>\n');
    process.exitCode = 1;
  } else { run(process.argv[3]).catch(() => { process.exitCode = 1; }); }
}
module.exports = { run, buildArguments, redact, redactedStream };
