#!/usr/bin/env node
'use strict';

// This process owns the entire host/broker signing namespace. Only public
// metadata enters argv; credentials, CLI diagnostics and build requests stay here.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { execFile } = require('node:child_process');
const KEYS = ['schemaVersion', 'projectDir', 'projectIdentity', 'nodePath', 'commandLineToolsRoot', 'sdkRoot',
  'hdcPath', 'target', 'format', 'buildMode', 'cliPath', 'teamId', 'loginDataDir'];
const MATERIAL_KEYS = ['certpath', 'profile', 'storeFile', 'keyAlias', 'signAlg', 'keyPassword', 'storePassword'];
const LIMIT = 4 * 1024 * 1024;
class SigningError extends Error {
  constructor(code, message) { super(message); this.code = code; }
}
function fail(code, message) { throw new SigningError(code, message); }
function need(ok, code, message) { if (!ok) fail(code, message); }
function object(value) { return value !== null && typeof value === 'object' && !Array.isArray(value); }
function text(value) { return typeof value === 'string' && !value.includes('\0') && value.length <= 32768; }
function absolute(value) { return text(value) && path.isAbsolute(value); }
function inside(root, filename) {
  const relative = path.relative(root, filename);
  return relative !== '' && relative !== '..' && !relative.startsWith('..' + path.sep) && !path.isAbsolute(relative);
}
function digest(bytes) { return crypto.createHash('sha256').update(bytes).digest('hex'); }
function parseJSON(bytes, comments = false) {
  let source = bytes.toString('utf8');
  need(Buffer.byteLength(source) <= LIMIT, 'INVALID_PROJECT', 'A JSON input exceeds the supported size.');
  if (comments) {
    source = source.replace(/"(?:[^"\\]|\\.)*"|\/\/[^\r\n]*|\/\*[\s\S]*?\*\//g,
      token => token.startsWith('"') ? token : ' ');
    source = source.replace(/"(?:[^"\\]|\\.)*"|,\s*(?=[}\]])/g, token => token.startsWith('"') ? token : '');
  }
  let result;
  try { result = JSON.parse(source); } catch { fail('INVALID_JSON', 'Expected a supported JSON object.'); }
  need(object(result), 'INVALID_JSON', 'Expected a JSON object.');
  return result;
}
function regular(filename, code = 'CACHE_INVALID') {
  let stat;
  try { stat = fs.lstatSync(filename); } catch { fail(code, 'A required file is missing or inaccessible.'); }
  need(stat.isFile() && !stat.isSymbolicLink() && stat.size <= LIMIT && stat.nlink === 1,
    code, 'A required file must be a regular file with one link and a bounded size.');
  return stat;
}
function directory(filename, code) {
  let stat;
  try { stat = fs.lstatSync(filename); } catch { fail(code, 'A required directory is missing or inaccessible.'); }
  need(stat.isDirectory() && !stat.isSymbolicLink() && fs.realpathSync(filename) === path.resolve(filename),
    code, 'A required directory must not traverse symbolic links.');
  return stat;
}
function privateMode(filename, bits, isDir) {
  try {
    const stat = isDir ? directory(filename, 'PRIVATE_STORAGE_REQUIRED') : regular(filename, 'PRIVATE_STORAGE_REQUIRED');
    fs.chmodSync(filename, bits);
    const checked = fs.lstatSync(filename);
    need((checked.mode & 0o7777) === bits && typeof process.getuid === 'function' && checked.uid === process.getuid() &&
      stat.uid === checked.uid, 'PRIVATE_STORAGE_REQUIRED', 'Use permission-capable XDG_CONFIG_HOME and TMPDIR outside HOME (0700 directories, 0600 files).');
  } catch (error) {
    if (error instanceof SigningError) throw error;
    fail('PRIVATE_STORAGE_REQUIRED', 'Use writable private XDG_CONFIG_HOME and TMPDIR with working Unix permissions.');
  }
}
function privateDir(filename) {
  try { fs.mkdirSync(filename, { recursive: true, mode: 0o700 }); } catch { fail('PRIVATE_STORAGE_REQUIRED', 'Cannot create a private signing directory.'); }
  privateMode(filename, 0o700, true);
  return filename;
}
function privateWrite(filename, data) {
  privateDir(path.dirname(filename));
  if (fs.existsSync(filename)) regular(filename, 'PRIVATE_STORAGE_REQUIRED');
  fs.writeFileSync(filename, data, { mode: 0o600, flag: 'w' });
  privateMode(filename, 0o600, false);
}
function writeJSON(filename, value) { privateWrite(filename, JSON.stringify(value, null, 2) + '\n'); }
function readJSON(filename, comments = false) { regular(filename); return parseJSON(fs.readFileSync(filename), comments); }
function validateRequest(request) {
  need(object(request) && KEYS.every(key => Object.hasOwn(request, key)) && Object.keys(request).every(key => KEYS.includes(key)),
    'INVALID_REQUEST', 'Missing or unknown automatic signing request fields.');
  need(request.schemaVersion === 1, 'INVALID_REQUEST', 'Automatic signing requires schemaVersion 1.');
  for (const key of KEYS.filter(key => key !== 'schemaVersion')) need(text(request[key]), 'INVALID_REQUEST', 'Request fields must be bounded strings without NUL.');
  for (const key of ['projectDir', 'projectIdentity', 'nodePath', 'commandLineToolsRoot', 'sdkRoot', 'hdcPath']) {
    need(absolute(request[key]), 'INVALID_REQUEST', 'Project and tool paths must be absolute.');
  }
  for (const key of ['cliPath', 'loginDataDir']) need(!request[key] || absolute(request[key]), 'INVALID_REQUEST', 'Optional tool and login paths must be absolute file paths.');
  need(request.target.length > 0 && !/[\r\n]/.test(request.target), 'INVALID_REQUEST', 'Select a target device.');
  need(request.format === 'hap' && request.buildMode === 'debug', 'INVALID_REQUEST', 'Automatic signing supports debug HAP builds only.');
  need(!request.teamId || /^[A-Za-z0-9_-]{1,256}$/.test(request.teamId), 'INVALID_REQUEST', 'The selected team ID is invalid.');
  directory(request.projectDir, 'INVALID_PROJECT');
  directory(request.commandLineToolsRoot, 'CLT_MISSING');
  try { directory(fs.realpathSync(request.sdkRoot), 'CLT_MISSING'); }
  catch { fail('CLT_MISSING', 'The selected SDK root is missing or inaccessible.'); }
  return request;
}
function execute(file, args, options = {}) {
  return new Promise((resolve, reject) => {
    const { signal, ...childOptions } = options;
    let child, forceKill, hardDeadline;
    const interrupt = () => {
      child?.kill('SIGTERM');
      forceKill ||= setTimeout(() => child?.kill('SIGKILL'), 5000);
    };
    const done = (error, stdout, stderr) => {
      signal?.removeEventListener('abort', interrupt);
      clearTimeout(forceKill); clearTimeout(hardDeadline);
      if (signal?.aborted) return reject(new SigningError('INTERRUPTED', 'Automatic signing was interrupted; retry when ready.'));
      resolve({ ok: !error, stdout: stdout || '', stderr: stderr || '',
        missing: error?.code === 'ENOENT', timedOut: Boolean(error?.killed), code: error?.code });
    };
    try {
      // Wait for child close before cleaning its private request. Native
      // AbortSignal callbacks can fire before a build runner restores its file.
      child = execFile(file, args, { encoding: 'utf8', shell: false, windowsHide: true, timeout: 20000,
        maxBuffer: LIMIT, ...childOptions, stdio: ['ignore', 'pipe', 'pipe'] }, done);
      signal?.addEventListener('abort', interrupt, { once: true });
      hardDeadline = setTimeout(() => child.kill('SIGKILL'), (childOptions.timeout || 20000) + 5000);
      if (signal?.aborted) interrupt();
      // execFile implementations may create a pipe despite stdio; close it at
      // once. No CLI command ever gets interactive input or credential payloads.
      child.stdin?.end();
    } catch {
      signal?.removeEventListener('abort', interrupt);
      clearTimeout(forceKill); clearTimeout(hardDeadline);
      if (signal?.aborted) reject(new SigningError('INTERRUPTED', 'Automatic signing was interrupted; retry when ready.'));
      else resolve({ ok: false, stdout: '', stderr: '', missing: true });
    }
  });
}
function cleanEnvironment(env) {
  const result = { ...env, NO_COLOR: '1', FORCE_COLOR: '0', TERM: 'dumb' };
  for (const key of ['NODE_OPTIONS', 'NODE_PATH', 'NODE_OPTS', 'DEVECO_CLI_AUTH_SOURCE', 'DEVECO_CODE_AUTH_DIR']) delete result[key];
  return result;
}
async function nodeCheck(request, env, signal) {
  const result = await execute(request.nodePath, ['--version'], { env, signal });
  need(!result.missing, 'NODE_MISSING', 'Select an installed Node.js 22 or newer in the editor toolchain settings.');
  const version = /^v(\d+)\.\d+\.\d+\s*$/.exec(result.stdout);
  need(result.ok && version, 'NODE_MISSING', 'The selected Node.js executable cannot report its version.');
  need(Number(version[1]) >= 22, 'NODE_VERSION_TOO_OLD', 'Automatic signing requires Node.js 22 or newer.');
}
function discoverCLI(request, env) {
  const candidates = [];
  if (request.cliPath) candidates.push(request.cliPath);
  else {
    for (const dir of (env.PATH || '').split(path.delimiter).filter(Boolean)) candidates.push(path.resolve(dir, 'devecocli'));
    const prefix = path.dirname(path.dirname(request.nodePath));
    candidates.push(path.join(prefix, 'lib/node_modules/@deveco/deveco-cli/cli.js'));
    // An editor can launch a CLT-bundled Node with a minimal PATH while the CLI
    // was installed in oo's independently selected current Node installation.
    try {
      if (absolute(env.HOME)) candidates.push(path.join(path.dirname(fs.realpathSync(path.join(env.HOME, '.oheco/bin/node'))), 'devecocli'));
    } catch {}
    candidates.push(path.join(path.dirname(process.execPath), 'devecocli'));
  }
  for (const filename of candidates) {
    try {
      const resolved = fs.realpathSync(filename);
      if (fs.statSync(resolved).isFile()) return resolved;
    } catch {}
  }
  fail('CLI_MISSING', 'Install the official CLI with npm install -g @deveco/deveco-cli, or set its entry file in EditorSettings.');
}
function discoverVerifier(request) {
  const roots = [path.join(request.commandLineToolsRoot, 'sdk'), request.sdkRoot];
  const candidates = [];
  for (const root of roots) {
    candidates.push(path.join(root, 'default/openharmony/toolchains/lib/hap-sign-tool'), path.join(root, 'toolchains/lib/hap-sign-tool'));
    try {
      for (const name of fs.readdirSync(root).sort()) {
        if (/^[\d.]+$/.test(name)) candidates.push(path.join(root, name, 'toolchains/lib/hap-sign-tool'));
      }
    } catch {}
  }
  for (const filename of candidates) {
    try { if (fs.statSync(filename).isFile()) return fs.realpathSync(filename); } catch {}
  }
  fail('CLT_MISSING', 'The selected SDK must provide the native toolchains/lib/hap-sign-tool profile verifier.');
}
function acquire(filename) {
  try { fs.mkdirSync(filename, { mode: 0o700 }); }
  catch (error) {
    if (error.code === 'EEXIST') fail('CACHE_BUSY', 'Another signing operation is active; retry after it finishes. Remove a stale private lock only after checking no helper is running.');
    fail('PRIVATE_STORAGE_REQUIRED', 'Cannot lock the private signing cache.');
  }
  try { privateMode(filename, 0o700, true); }
  catch (error) { fs.rmdirSync(filename); throw error; }
  return () => fs.rmdirSync(filename);
}
function copyFilePrivate(source, destination) {
  regular(source);
  privateWrite(destination, fs.readFileSync(source));
}
function copyTree(source, destination, state = { count: 0, bytes: 0 }) {
  directory(source, 'CACHE_INVALID');
  privateDir(destination);
  for (const name of fs.readdirSync(source).sort()) {
    need(++state.count <= 4096, 'CACHE_INVALID', 'Signing cache contains too many files.');
    const from = path.join(source, name), to = path.join(destination, name);
    const stat = fs.lstatSync(from);
    need(!stat.isSymbolicLink(), 'CACHE_INVALID', 'Signing material must not contain symbolic links.');
    if (stat.isDirectory()) copyTree(from, to, state);
    else {
      state.bytes += stat.size;
      need(state.bytes <= 32 * LIMIT, 'CACHE_INVALID', 'Signing material exceeds the supported cache size.');
      copyFilePrivate(from, to);
    }
  }
}
function hardenTree(root, state = { count: 0 }) {
  privateMode(root, 0o700, true);
  for (const name of fs.readdirSync(root)) {
    need(++state.count <= 4096, 'PRIVATE_STORAGE_REQUIRED', 'The private CLI home exceeds the supported file count.');
    const filename = path.join(root, name), stat = fs.lstatSync(filename);
    if (stat.isDirectory() && !stat.isSymbolicLink()) hardenTree(filename, state);
    else privateMode(filename, 0o600, false);
  }
}
function removeOwned(root, target) {
  need(inside(root, path.resolve(target)) && fs.realpathSync(root) === root, 'CACHE_INVALID', 'Refusing to remove a path outside the private workspace.');
  fs.rmSync(target, { recursive: true, force: true });
}
function sourceFingerprint(filename) {
  if (!fs.existsSync(filename)) return null;
  const stat = regular(filename, 'LOGIN_DATA_INVALID');
  return { size: stat.size, mtime: stat.mtimeMs, sha256: digest(fs.readFileSync(filename)) };
}
function syncLogin(ctx) {
  const login = ctx.request.loginDataDir || ctx.originalEnv.DEVECO_CLI_DATA_DIR || path.join(ctx.originalHome, '.config/deveco-cli');
  need(absolute(login), 'LOGIN_DATA_INVALID', 'The original CLI login data directory must be absolute.');
  const files = [
    ...['token.enc', 'token.dek'].map(name => [path.join(login, name), path.join(ctx.cliData, name)]),
    ...[1, 2, 3].map(n => [path.join(ctx.originalHome, `.local/share/deveco-cli/keys/kek-v${n}.bin`), path.join(ctx.cliHome, `.local/share/deveco-cli/keys/kek-v${n}.bin`)]),
    ...['hdckey', 'hdckey.pub'].map(name => [path.join(ctx.originalHome, '.harmony', name), path.join(ctx.cliHome, '.harmony', name)]),
  ];
  const metadata = { source: login, originalHome: ctx.originalHome, files: files.map(([source]) => sourceFingerprint(source)) };
  const metaPath = path.join(ctx.root, 'login-source.json');
  let previous;
  if (fs.existsSync(metaPath)) previous = readJSON(metaPath);
  if (JSON.stringify(previous) === JSON.stringify(metadata)) return;
  for (let index = 0; index < files.length; index++) {
    const [source, destination] = files[index];
    if (metadata.files[index]) copyFilePrivate(source, destination);
    else if (fs.existsSync(destination)) { regular(destination); fs.unlinkSync(destination); }
  }
  writeJSON(metaPath, metadata);
}
function cliEnvironment(ctx) {
  return { ...ctx.env, HOME: ctx.cliHome, XDG_CONFIG_HOME: path.join(ctx.cliHome, '.config'),
    XDG_DATA_HOME: path.join(ctx.cliHome, '.local/share'), DEVECO_CLI_DATA_DIR: ctx.cliData,
    TMPDIR: ctx.work, TMP: ctx.work, TEMP: ctx.work, COMMAND_LINE_TOOL_PATH: ctx.request.commandLineToolsRoot,
    PATH: path.dirname(ctx.request.nodePath) + path.delimiter + (ctx.env.PATH || '') };
}
async function cli(ctx, args) {
  // Public JS entrypoints and devecocli symlinks are interpreted by selected Node.
  try {
    return await execute(ctx.request.nodePath, [ctx.cli, ...args], { cwd: ctx.minimal || ctx.request.projectDir,
      env: cliEnvironment(ctx), signal: ctx.signal, timeout: args[0] === 'signature' ? 120000 : 20000 });
  } finally { hardenTree(ctx.cliHome); }
}
function networkFailure(result) { return /ENOTFOUND|ECONNREFUSED|ECONNRESET|ETIMEDOUT|network|fetch failed|proxy|timeout/i.test(result.stdout + result.stderr); }
async function authenticate(ctx) {
  const result = await cli(ctx, ['auth', 'status']);
  need(!/Not logged in/i.test(result.stdout + result.stderr), 'LOGIN_REQUIRED', 'Run devecocli auth login outside the editor, then retry. The helper never logs in.');
  if (!result.ok && networkFailure(result)) fail('NETWORK_ERROR', 'The CLI cannot reach its service; check the network/proxy and retry.');
  need(result.ok && /^\s*Current user:\s*\S+/m.test(result.stdout), 'LOGIN_REQUIRED', 'No verified CLI login was found. Run devecocli auth login outside the editor, then retry.');
}
function parseTeams(output) {
  const clean = output.replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, '');
  const ids = [];
  const lines = clean.split(/\r?\n/);
  let table = false;
  for (const line of lines) {
    const fields = line.replace(/[│┃|]/g, ' ').trim().split(/\s+/);
    if (/^Id$/i.test(fields[0]) && /^Name$/i.test(fields[1])) { table = true; continue; }
    if (/[└╰]/.test(line)) { table = false; continue; }
    if (!table || /^[\s─━┌┐└┘├┤┬┴┼+\-=]*$/.test(line)) continue;
    if (/^[A-Za-z0-9_-]{1,256}$/.test(fields[0]) && fields.length >= 2) ids.push(fields[0]);
  }
  return [...new Set(ids)];
}
async function selectTeam(ctx) {
  if (ctx.request.teamId) return ctx.request.teamId;
  const result = await cli(ctx, ['auth', 'team', 'list']);
  if (!result.ok && networkFailure(result)) fail('NETWORK_ERROR', 'Cannot list teams; check the network/proxy and retry.');
  need(result.ok, 'TEAM_REQUIRED', 'Cannot list teams. Set the signing team ID in EditorSettings.');
  const teams = parseTeams(result.stdout);
  need(teams.length === 1, 'TEAM_REQUIRED', 'Set the signing team ID in EditorSettings; automatic selection requires exactly one team.');
  return teams[0];
}
async function deviceUDID(ctx) {
  let result;
  try {
    result = await execute(ctx.request.hdcPath, ['-t', ctx.request.target, 'shell', 'bm', 'get', '-u'], { env: cliEnvironment(ctx), signal: ctx.signal });
  } finally { hardenTree(ctx.cliHome); }
  if (/unauthori[sz]ed|not authori[sz]ed|authorize|authentication fail/i.test(result.stdout + result.stderr)) {
    fail('DEVICE_UNAUTHORIZED', 'Authorize the selected HDC device and retry.');
  }
  const matches = [...new Set((result.stdout.match(/\b[0-9a-fA-F]{64}\b/g) || []).map(value => value.toLowerCase()))];
  need(result.ok && matches.length === 1, 'DEVICE_UNAVAILABLE', 'Cannot read the selected device UDID with hdc shell bm get -u; check its connection and authorization.');
  return matches[0];
}
function apiLevel(value) {
  if (Number.isSafeInteger(value) && value > 0) return value;
  if (typeof value === 'string' && /^\d+(?:\.\d+){0,3}$/.test(value) && Number(value.split('.')[0]) > 0) return Number(value.split('.')[0]);
  fail('ACL_METADATA_INVALID', 'The SDK permission API metadata is unsupported. Select a complete compatible Command Line Tools SDK.');
}
function requiredACL(request, permissions) {
  // CLI 1.3.5's public SDK input: never derive ACLs from a blind list of all
  // requested permissions or by importing/extracting its private bundle.
  const sdk = path.join(request.commandLineToolsRoot, 'sdk/default');
  let definitions, metadata;
  try {
    definitions = readJSON(path.join(sdk, 'openharmony/toolchains/lib/permissionDefinitions.json'));
    metadata = readJSON(path.join(sdk, 'sdk-pkg.json'));
  } catch { fail('ACL_METADATA_INVALID', 'The selected Command Line Tools SDK is missing valid permissionDefinitions.json or sdk-pkg.json.'); }
  const api = apiLevel(metadata.data?.apiVersion);
  need(Array.isArray(definitions.definePermissions) && definitions.definePermissions.length <= 20000,
    'ACL_METADATA_INVALID', 'The SDK permission definitions are invalid. Select a complete compatible SDK.');
  const index = new Map();
  for (const definition of definitions.definePermissions) {
    need(object(definition) && typeof definition.name === 'string' && !index.has(definition.name),
      'ACL_METADATA_INVALID', 'The SDK permission definitions contain malformed or duplicate names.');
    index.set(definition.name, definition);
  }
  const includes = ['ohos.permission.FILE_ACCESS_PERSIST', 'ohos.permission.READ_WRITE_DOWNLOAD_DIRECTORY', 'ohos.permission.READ_WRITE_DOCUMENTS_DIRECTORY'];
  const excludes = ['ohos.permission.READ_DOCUMENT', 'ohos.permission.WRITE_DOCUMENT'];
  const result = [];
  for (const name of permissions) {
    const definition = index.get(name);
    need(object(definition) && typeof definition.availableLevel === 'string' && typeof definition.availableType === 'string' &&
      typeof definition.provisionEnable === 'boolean', 'ACL_METADATA_INVALID', 'A requested permission has no complete SDK definition. Check the selected SDK.');
    const since = apiLevel(definition.since ?? definition.minSupportApiLevel);
    const minimum = definition.minSupportApiLevel === undefined ? since : apiLevel(definition.minSupportApiLevel);
    need(Math.max(since, minimum) <= api, 'ACL_UNSUPPORTED', 'A requested permission requires a newer SDK API. Update the selected SDK or remove that permission.');
    if (!excludes.includes(name) && (includes.includes(name) ||
      (definition.availableLevel === 'system_basic' && definition.availableType === 'NORMAL' && definition.provisionEnable === true))) result.push(name);
  }
  return { names: result, metadataFingerprint: digest(JSON.stringify([metadata, definitions])) };
}
function projectMetadata(request) {
  const rootProfile = readJSON(path.join(request.projectDir, 'build-profile.json5'), true);
  const app = readJSON(path.join(request.projectDir, 'AppScope/app.json5'), true);
  need(object(app.app) && typeof app.app.bundleName === 'string' && /^[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+$/.test(app.app.bundleName),
    'INVALID_PROJECT', 'The exported project has no supported bundle name.');
  need(object(rootProfile.app) && Array.isArray(rootProfile.app.products) &&
    rootProfile.app.products.every(p => object(p) && typeof p.name === 'string') && rootProfile.app.products.filter(p => p.name === 'default').length === 1 &&
    Array.isArray(rootProfile.modules) && rootProfile.modules.length === 1 && object(rootProfile.modules[0]) && rootProfile.modules[0].name === 'entry' &&
    ['entry', './entry'].includes(rootProfile.modules[0].srcPath), 'INVALID_PROJECT', 'Automatic signing requires the generated default product and entry module.');
  const module = readJSON(path.join(request.projectDir, 'entry/src/main/module.json5'), true);
  need(object(module.module) && (module.module.requestPermissions === undefined || Array.isArray(module.module.requestPermissions)), 'INVALID_PROJECT', 'The entry permission configuration is invalid.');
  const permissions = (module.module.requestPermissions || []).map(item => {
    need(object(item) && typeof item.name === 'string' && /^[A-Za-z0-9_.]+$/.test(item.name), 'INVALID_PROJECT', 'A requested permission is invalid.');
    return item.name;
  });
  const unique = [...new Set(permissions)].sort();
  const acl = requiredACL(request, unique);
  return { bundle: app.app.bundleName, permissions: unique, requiredACL: acl.names, aclMetadata: acl.metadataFingerprint, rootProfile, app, module };
}
function prepareMinimal(ctx) {
  const target = path.join(ctx.cache, 'project');
  privateDir(target);
  const profile = structuredClone(ctx.metadata.rootProfile);
  profile.app.signingConfigs = [];
  for (const product of profile.app.products) delete product.signingConfig;
  // Only an invalid/missing cache reaches generation. Do not hand stale or
  // malformed cached signing paths back to the CLI; it writes a fresh config.
  writeJSON(path.join(target, 'build-profile.json5'), profile);
  writeJSON(path.join(target, 'AppScope/app.json5'), ctx.metadata.app);
  writeJSON(path.join(target, 'entry/src/main/module.json5'), ctx.metadata.module);
  const entryBuild = path.join(ctx.request.projectDir, 'entry/build-profile.json5');
  if (fs.existsSync(entryBuild)) writeJSON(path.join(target, 'entry/build-profile.json5'), readJSON(entryBuild, true));
  ctx.minimal = target;
}
function frame(value) {
  if (typeof value !== 'string' || !/^[a-fA-F0-9]{64,}$/.test(value) || value.length % 2 || value.length > LIMIT) return false;
  const bytes = Buffer.from(value, 'hex');
  return bytes.readUInt32BE(0) >= 16 && bytes.readUInt32BE(0) === bytes.length - 16;
}
function configMaterial(config) {
  need(object(config) && typeof config.name === 'string' && config.name.length > 0 && object(config.material), 'CACHE_INVALID', 'The CLI signing configuration is incomplete.');
  const material = config.material;
  need(MATERIAL_KEYS.every(key => typeof material[key] === 'string' && material[key].length > 0 && !material[key].includes('\0')) &&
    frame(material.keyPassword) && frame(material.storePassword) && ['SHA256withECDSA', 'SHA384withECDSA'].includes(material.signAlg),
  'CACHE_INVALID', 'The CLI signing configuration does not contain supported encrypted signing material.');
  return material;
}
function certificateCheck(material, profile, now) {
  let cert;
  try {
    const bytes = fs.readFileSync(material.certpath);
    const blocks = bytes.toString('utf8').match(/-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----/g) || [bytes];
    const embedded = new crypto.X509Certificate(profile['bundle-info']['development-certificate']);
    // SDK CER files may contain a chain. Match the profile's developer
    // certificate rather than assuming it is the first certificate in the file.
    cert = blocks.map(block => new crypto.X509Certificate(block)).find(candidate => candidate.fingerprint256 === embedded.fingerprint256);
  } catch { return 'CERTIFICATE_INVALID'; }
  if (!cert) return 'CERTIFICATE_MISMATCH';
  if (!(Date.parse(cert.validFrom) <= now && now < Date.parse(cert.validTo))) return 'CERTIFICATE_EXPIRED';
  return '';
}
async function inspectSigning(ctx, config) {
  let material;
  try {
    const context = readJSON(path.join(ctx.cache, 'signing-context.json'));
    if (JSON.stringify(context.requiredACL) !== JSON.stringify(ctx.metadata.requiredACL)) return { ready: false, reason: 'PROFILE_ACL_MISMATCH' };
    material = configMaterial(config);
    for (const key of ['certpath', 'profile', 'storeFile']) {
      need(absolute(material[key]) && inside(path.join(ctx.cache, 'materials'), material[key]), 'CACHE_INVALID', 'Signing paths must remain in this private application cache.');
      regular(material[key]);
      privateMode(material[key], 0o600, false);
      need(fs.realpathSync(material[key]) === material[key], 'CACHE_INVALID', 'Signing material must not traverse symlinks.');
    }
    for (const name of ['fd', 'ac', 'ce']) {
      const dir = path.join(path.dirname(material.storeFile), 'material', name);
      directory(dir, 'CACHE_INVALID');
      privateMode(dir, 0o700, true);
      need(fs.readdirSync(dir).length > 0, 'CACHE_INVALID', 'Signing companion material is incomplete.');
    }
  } catch (error) {
    if (error.code === 'PRIVATE_STORAGE_REQUIRED') throw error;
    return { ready: false, reason: 'CACHE_INVALID' };
  }
  const output = path.join(ctx.work, 'verified-profile.json');
  if (fs.existsSync(output)) fs.unlinkSync(output);
  const result = await execute(ctx.verifier, ['verify-profile', '-inFile', material.profile, '-outFile', output], { env: cliEnvironment(ctx), signal: ctx.signal });
  if (result.missing || result.code === 'EACCES') fail('CLT_MISSING', 'The selected SDK native profile verifier is not executable. Check the Command Line Tools installation.');
  if (!result.ok || !fs.existsSync(output)) return { ready: false, reason: 'PROFILE_UNVERIFIED' };
  let profile;
  try { privateMode(output, 0o600, false); profile = readJSON(output); }
  catch (error) {
    if (error.code === 'PRIVATE_STORAGE_REQUIRED') throw error;
    return { ready: false, reason: 'PROFILE_UNVERIFIED' };
  }
  const now = Date.now();
  if (profile.type !== 'debug' || !object(profile['bundle-info']) || profile['bundle-info']['bundle-name'] !== ctx.metadata.bundle) return { ready: false, reason: 'PROFILE_BUNDLE_MISMATCH' };
  const validity = profile.validity;
  if (!object(validity) || !Number.isSafeInteger(validity['not-before']) || !Number.isSafeInteger(validity['not-after']) ||
    validity['not-before'] * 1000 > now || validity['not-after'] * 1000 <= now) return { ready: false, reason: 'PROFILE_EXPIRED' };
  const debug = profile['debug-info'];
  // Empty/unknown device policies never imply all devices. Accept explicit UDID
  // profiles; unrecognized all-device policies require manual signing instead.
  if (!object(debug) || String(debug['device-id-type']).toLowerCase() !== 'udid' || !Array.isArray(debug['device-ids']) ||
    !debug['device-ids'].every(item => typeof item === 'string' && /^[0-9a-fA-F]{64}$/.test(item)) || !debug['device-ids'].some(item => item.toLowerCase() === ctx.udid)) return { ready: false, reason: 'PROFILE_DEVICE_MISMATCH' };
  const acls = profile.acls?.['allowed-acls'] ?? (profile.acls === undefined ? [] : null);
  if (!Array.isArray(acls) || !acls.every(item => typeof item === 'string') ||
    ctx.metadata.requiredACL.some(permission => !acls.includes(permission))) return { ready: false, reason: 'PROFILE_ACL_MISMATCH' };
  const certificateError = certificateCheck(material, profile, now);
  if (certificateError) return { ready: false, reason: certificateError };
  return { ready: true, material };
}
function refreshMetadata(ctx) {
  const current = projectMetadata(ctx.request);
  need(current.bundle === ctx.metadata.bundle, 'PROJECT_CHANGED', 'The project bundle changed during automatic signing. Retry with the current project configuration.');
  ctx.metadata = current;
}
async function cachedSigning(ctx) {
  const filename = path.join(ctx.cache, 'signing-config.json');
  if (!fs.existsSync(filename)) return { ready: false, reason: 'CACHE_MISSING' };
  try { return await inspectSigning(ctx, readJSON(filename)); }
  catch (error) {
    if (ctx.signal.aborted || ['PRIVATE_STORAGE_REQUIRED', 'CLT_MISSING'].includes(error.code)) throw error;
    return { ready: false, reason: 'CACHE_INVALID' };
  }
}
function importGenerated(ctx) {
  const profile = readJSON(path.join(ctx.minimal, 'build-profile.json5'), true);
  need(object(profile.app) && Array.isArray(profile.app.products) && Array.isArray(profile.app.signingConfigs),
    'SIGNING_FAILED', 'The CLI returned a malformed signing project configuration.');
  const product = profile.app.products.find(item => object(item) && item.name === 'default');
  const configs = profile.app.signingConfigs.filter(item => object(item) && item.name === product?.signingConfig);
  need(Array.isArray(configs) && configs.length === 1, 'SIGNING_FAILED', 'The CLI did not write one selected signing configuration.');
  const config = structuredClone(configs[0]);
  const material = configMaterial(config);
  const stage = path.join(ctx.work, 'new-materials');
  privateDir(stage);
  for (const [key, name] of [['certpath', 'certificate.cer'], ['profile', 'profile.p7b'], ['storeFile', 'store.p12']]) {
    need(absolute(material[key]), 'SIGNING_FAILED', 'The CLI returned a nonabsolute signing path.');
    const source = fs.realpathSync(material[key]);
    need(inside(ctx.cliHome, source) || inside(ctx.cache, source), 'SIGNING_FAILED', 'The CLI returned signing files outside its private namespace.');
    copyFilePrivate(source, path.join(stage, name));
  }
  copyTree(path.join(path.dirname(fs.realpathSync(material.storeFile)), 'material'), path.join(stage, 'material'));
  const destination = path.join(ctx.cache, 'materials');
  if (fs.existsSync(destination)) removeOwned(ctx.cache, destination);
  fs.renameSync(stage, destination);
  material.certpath = path.join(destination, 'certificate.cer');
  material.profile = path.join(destination, 'profile.p7b');
  material.storeFile = path.join(destination, 'store.p12');
  writeJSON(path.join(ctx.cache, 'signing-config.json'), config);
  writeJSON(path.join(ctx.cache, 'signing-context.json'), { requiredACL: ctx.metadata.requiredACL });
  return config;
}
async function generate(ctx) {
  const releaseTeam = acquire(path.join(ctx.root, 'locks', 'team-' + digest(ctx.team)));
  let releaseCLI;
  const cacheBackup = path.join(ctx.work, 'cache-backup');
  const ohos = path.join(ctx.cliHome, '.ohos');
  const ohosBackup = path.join(ctx.work, 'cli-backup');
  let backedUp = false, hadOhos = false;
  try {
    releaseCLI = acquire(path.join(ctx.root, 'locks', 'cli'));
    // The project lock is already held. Re-read live request context under the
    // team/CLI locks rather than binding generation to an earlier snapshot.
    syncLogin(ctx);
    await authenticate(ctx);
    need(await selectTeam(ctx) === ctx.team, 'TEAM_REQUIRED', 'The available signing team changed. Select the intended team ID in EditorSettings and retry.');
    ctx.udid = await deviceUDID(ctx);
    refreshMetadata(ctx);
    const current = await cachedSigning(ctx);
    if (current.ready) return current;
    copyTree(ctx.cache, cacheBackup);
    hadOhos = fs.existsSync(ohos);
    if (hadOhos) copyTree(ohos, ohosBackup);
    backedUp = true;
    prepareMinimal(ctx);
    const result = await cli(ctx, ['signature', 'generate', '--team-id', ctx.team, '--product', 'default']);
    if (!result.ok) {
      if (networkFailure(result)) fail('NETWORK_ERROR', 'Signing service request failed. Check the network/proxy; old local signing materials were retained.');
      fail('SIGNING_FAILED', 'The official CLI failed to generate signing materials. Old local files were retained; a cloud certificate change cannot be rolled back locally.');
    }
    const config = importGenerated(ctx);
    const checked = await inspectSigning(ctx, config);
    need(checked.ready, checked.reason === 'PROFILE_DEVICE_MISMATCH' ? 'PROFILE_DEVICE_MISMATCH' : 'PROFILE_INVALID',
      'The generated profile did not pass bundle, device, permission, validity and certificate checks. Old local files were retained; check the official CLI account/device setup.');
    return checked;
  } catch (error) {
    if (backedUp) {
      // Backups are in the same verified private temporary namespace. Preserve
      // both old app materials and the CLI's team files on every failed attempt.
      removeOwned(ctx.root, ctx.cache);
      copyTree(cacheBackup, ctx.cache);
      if (fs.existsSync(ohos)) removeOwned(ctx.cliHome, ohos);
      if (hadOhos) copyTree(ohosBackup, ohos);
    }
    throw error;
  } finally {
    try { releaseCLI?.(); } finally { releaseTeam(); }
  }
}
async function build(ctx, material) {
  const runner = path.join(ctx.request.projectDir, 'tools/build.cjs');
  regular(runner, 'BUILD_FAILED');
  const profilePath = path.join(ctx.request.projectDir, 'build-profile.json5');
  const original = fs.readFileSync(profilePath);
  const workDir = privateDir(path.join(ctx.work, 'build'));
  const request = { schemaVersion: 2, projectDir: ctx.request.projectDir, sdkRoot: ctx.request.sdkRoot,
    commandLineToolsRoot: ctx.request.commandLineToolsRoot, buildMode: ctx.request.buildMode, format: ctx.request.format,
    workDir, hvigorUserHome: path.join(ctx.originalHome, '.hvigor'), signing: {
      certificate: material.certpath, profile: material.profile, storeFile: material.storeFile,
      keyAlias: material.keyAlias, signAlg: material.signAlg, keyPasswordEncrypted: material.keyPassword,
      storePasswordEncrypted: material.storePassword,
    } };
  const filename = path.join(workDir, 'request.json');
  writeJSON(filename, request);
  let result, executionError;
  try {
    result = await execute(ctx.request.nodePath, [runner, '--request', filename], {
      cwd: ctx.request.projectDir, env: ctx.env, signal: ctx.signal, timeout: 10 * 60 * 1000,
    });
  } catch (error) { executionError = error; }
  let restored = false;
  try { regular(profilePath, 'BUILD_PROFILE_NOT_RESTORED'); restored = original.equals(fs.readFileSync(profilePath)); } catch {}
  need(restored, 'BUILD_PROFILE_NOT_RESTORED', 'The build profile was not restored. Recover the build runner backup before retrying.');
  if (executionError) throw executionError;
  need(result.ok, 'BUILD_FAILED', 'Signed HAP build failed. Check the prepared offline toolchain and build inputs.');
  let receipt;
  try { receipt = readJSON(path.join(workDir, 'result.json')); } catch { fail('BUILD_FAILED', 'The build runner did not produce a valid result.'); }
  need(receipt.schemaVersion === 2 && receipt.signed === true && receipt.format === ctx.request.format && receipt.mode === ctx.request.buildMode &&
    absolute(receipt.outputPath) && inside(ctx.request.projectDir, receipt.outputPath), 'BUILD_FAILED', 'The build runner returned an invalid signed artifact receipt.');
  let stat;
  try { stat = fs.lstatSync(receipt.outputPath); } catch { fail('BUILD_FAILED', 'The signed artifact is missing or inaccessible.'); }
  need(stat.isFile() && !stat.isSymbolicLink() && stat.size > 0 && fs.realpathSync(receipt.outputPath) === receipt.outputPath,
    'BUILD_FAILED', 'The signed artifact is missing, empty, or traverses symbolic links.');
  return { schemaVersion: 1, status: 'ok', signed: true, format: receipt.format, mode: receipt.mode,
    outputPath: receipt.outputPath, profileValidated: true };
}
async function perform(request, originalEnv, signal) {
  validateRequest(request);
  const env = cleanEnvironment(originalEnv);
  await nodeCheck(request, env, signal);
  const originalHome = originalEnv.HOME;
  need(absolute(originalHome), 'PRIVATE_STORAGE_REQUIRED', 'An absolute original HOME is required to copy the existing CLI login.');
  const configHome = originalEnv.XDG_CONFIG_HOME, tempHome = originalEnv.TMPDIR;
  for (const root of [configHome, tempHome]) {
    need(absolute(root) && root !== originalHome && !inside(originalHome, root) && !/(?:^|\/)hmdfs(?:\/|$)/i.test(root),
      'PRIVATE_STORAGE_REQUIRED', 'Set XDG_CONFIG_HOME and TMPDIR to permission-capable private storage outside HOME/hmdfs.');
    directory(root, 'PRIVATE_STORAGE_REQUIRED');
  }
  const root = privateDir(path.join(configHome, 'godot-openharmony-signing'));
  privateDir(path.join(root, 'locks'));
  const cliHome = privateDir(path.join(root, 'cli-home'));
  const cliData = privateDir(path.join(cliHome, '.config/deveco-cli'));
  const work = fs.mkdtempSync(path.join(tempHome, 'godot-auto-sign-'));
  let releaseProject;
  try {
    privateMode(work, 0o700, true);
    const ctx = { request, originalEnv, env, originalHome, root, cliHome, cliData, work, signal };
    ctx.cli = discoverCLI(request, env);
    ctx.verifier = discoverVerifier(request);
    ctx.metadata = projectMetadata(request);
    const releaseCLI = acquire(path.join(root, 'locks', 'cli'));
    try {
      syncLogin(ctx);
      ctx.udid = await deviceUDID(ctx);
      await authenticate(ctx);
      ctx.team = await selectTeam(ctx);
    } finally { releaseCLI(); }
    const key = digest(JSON.stringify([request.projectIdentity, ctx.metadata.bundle, ctx.team]));
    releaseProject = acquire(path.join(root, 'locks', 'project-' + key));
    ctx.cache = privateDir(path.join(root, 'projects', key));
    hardenTree(ctx.cache);
    refreshMetadata(ctx);
    let checked = await cachedSigning(ctx);
    if (!checked.ready) checked = await generate(ctx);
    return await build(ctx, checked.material);
  } finally {
    try { releaseProject?.(); }
    finally { removeOwned(path.resolve(tempHome), work); }
  }
}
async function run(request, env = process.env) {
  const controller = new AbortController();
  const interrupt = () => controller.abort();
  for (const name of ['SIGINT', 'SIGTERM']) process.on(name, interrupt);
  try { return await perform(request, env, controller.signal); }
  catch (error) {
    if (error instanceof SigningError) return { schemaVersion: 1, status: 'error', code: error.code, message: error.message };
    if (['EACCES', 'EPERM', 'EROFS'].includes(error?.code)) return { schemaVersion: 1, status: 'error', code: 'PRIVATE_STORAGE_REQUIRED', message: 'Signing storage or tools are inaccessible. Check private storage permissions and tool paths.' };
    if (error?.code === 'ENOENT') return { schemaVersion: 1, status: 'error', code: 'CACHE_INVALID', message: 'A required signing/build file disappeared. Check the selected toolchain and retry.' };
    throw error;
  } finally {
    for (const name of ['SIGINT', 'SIGTERM']) process.off(name, interrupt);
  }
}
async function main(argv = process.argv.slice(2)) {
  let response, exitCode = 0;
  try {
    need(argv.length === 2 && argv[0] === '--request-json', 'INVALID_REQUEST', 'Use auto-sign.cjs --request-json with one JSON metadata object.');
    response = await run(parseJSON(Buffer.from(argv[1])));
  } catch (error) {
    const expected = error instanceof SigningError;
    response = { schemaVersion: 1, status: 'error', code: expected ? error.code : 'INTERNAL_ERROR',
      message: expected ? error.message : 'Automatic signing failed internally. Check the private cache and selected tools.' };
    exitCode = expected ? 0 : 1;
  }
  process.stdout.write(JSON.stringify(response) + '\n');
  process.exitCode = exitCode;
}
if (require.main === module) main();
module.exports = { run, main, validateRequest, parseTeams, SigningError };
