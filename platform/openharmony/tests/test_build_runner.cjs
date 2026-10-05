'use strict';

// Synthetic fixtures only: no real signing, package installation or SDK builds.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');
const RUNNER = path.resolve(__dirname, '../../../misc/dist/openharmony_template/tools/build.cjs');
const { redactedStream, buildArguments } = require(RUNNER);

const FAKE_HVIGOR = String.raw`'use strict';
const fs = require('node:fs');
const path = require('node:path');
const root = process.cwd();
const control = JSON.parse(fs.readFileSync(path.join(root, 'fixture-control.json')));
const args = process.argv.slice(2);
if (args[0] === '--adapter-info') {
  if (control.probeFailure) { console.error('probe failure'); process.exit(4); }
  console.log(JSON.stringify({adapter:'6.26.4-ohos.1', host:'openharmony', arch:'arm64',
    upstream:'6.26.4', pinnedInterfacesUnmodified:true, ...control.info}));
} else {
  const profile = fs.readFileSync(path.join(root, 'build-profile.json5'));
  const doc = JSON.parse(profile);
  const material = doc.app.signingConfigs[0]?.material;
  const work = process.env.HVIGOR_USER_HOME;
  fs.writeFileSync(path.join(work, 'generated-profile.json'), profile);
  fs.writeFileSync(path.join(work, 'invocation.json'), JSON.stringify({
    args, execPath: process.execPath, cwd: root,
    env: Object.fromEntries(['JAVA_HOME','DEVECO_SDK_HOME','OHOS_SDK_HOME','OHOS_BASE_SDK_HOME',
      'HVIGOR_USER_HOME','TMPDIR','HOME','NODE_PATH','NPM_CONFIG_OFFLINE'].map(k => [k, process.env[k]])),
    passwordsPresent: !material || (typeof process.env.OHECO_HVIGOR_KEY_PASSWORD === 'string' &&
      typeof process.env.OHECO_HVIGOR_STORE_PASSWORD === 'string'),
    envReferences: !material || (material.keyPassword === 'env:OHECO_HVIGOR_KEY_PASSWORD' &&
      material.storePassword === 'env:OHECO_HVIGOR_STORE_PASSWORD'),
  }));
  const format = args.includes('assembleHap') ? 'hap' : 'app';
  const out = format === 'hap' ? path.join(root, 'entry/build/default/outputs/default') :
    path.join(root, 'build/outputs/default');
  const signed = Boolean(material);
  const all = format === 'app' && doc.app.products[0].buildOption?.packOptions?.appWithSignedPkg === true;
  const suffix = signed ? (all ? '-all-signed' : '-signed') : '-unsigned';
  if (control.failure) {
    const a = 'wrong key password: ' + process.env.OHECO_HVIGOR_KEY_PASSWORD + '\n';
    const b = 'wrong store password: ' + process.env.OHECO_HVIGOR_STORE_PASSWORD + '\n';
    const cutA = Math.floor(a.length / 2), cutB = Math.floor(b.length / 2);
    process.stdout.write(a.slice(0, cutA)); process.stderr.write(b.slice(0, cutB));
    setTimeout(() => { process.stdout.write(a.slice(cutA)); process.stderr.write(b.slice(cutB));
      console.error('material ' + [material.certpath, material.profile, material.storeFile, material.keyAlias].join(' '));
      process.exitCode = 7;
    }, 15);
  } else {
    fs.mkdirSync(out, {recursive:true});
    if (!control.missing) {
      const name = 'fixture-default' + (control.wrongSuffix ? '-unsigned' : suffix) + '.' + format;
      const file = path.join(out, name);
      if (control.symlink) fs.symlinkSync(path.join(root, 'fixture-control.json'), file);
      else fs.writeFileSync(file, control.empty ? '' : 'SYNTHETIC PACKAGE, NOT A REAL HAP/APP');
      if (control.ambiguous) fs.writeFileSync(path.join(out, 'other-default' + suffix + '.' + format), 'OTHER SYNTHETIC PACKAGE');
    }
    // Distractors in a different product, recursion level, and opposite signing
    // class must not influence deterministic artifact selection.
    fs.mkdirSync(path.join(root, 'entry/build/other/outputs/default'), {recursive:true});
    fs.writeFileSync(path.join(root, 'entry/build/other/outputs/default/distractor-signed.hap'), 'DISTRACTOR');
    fs.mkdirSync(path.join(out, 'nested'), {recursive:true});
    fs.writeFileSync(path.join(out, 'nested/distractor' + suffix + '.' + format), 'DISTRACTOR');
    console.log('fixture build stdout'); console.error('fixture build stderr');
    if (control.hold) { console.log('fixture waiting for termination'); setInterval(() => {}, 1000); }
  }
}
`;

function writeJSON(filename, value, mode = 0o600) {
  fs.mkdirSync(path.dirname(filename), { recursive: true, mode: 0o700 });
  fs.writeFileSync(filename, JSON.stringify(value, null, 2), { mode });
}
function fixture(t, options = {}) {
  // os.tmpdir follows this application's permission-capable TMPDIR, not HOME.
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'godot-build-runner-test-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const project = path.join(root, 'project with spaces; no shell');
  const sdk = path.join(root, 'SDK view with spaces', 'root');
  const work = path.join(root, 'private work');
  for (const dir of [project, sdk, work]) fs.mkdirSync(dir, { recursive: true, mode: 0o700 });
  const entry = path.join(root, 'adapter with spaces.cjs');
  fs.writeFileSync(entry, FAKE_HVIGOR);
  const profile = { app: { signingConfigs: [{name:'original-encrypted-signing'}], products: [{
    name:'default', signingConfig:'original-encrypted-signing', runtimeOS:'OpenHarmony',
    ...(Object.hasOwn(options,'packOptions') ? {buildOption:{packOptions:options.packOptions}} : {}),
  }] }, modules:[{name:'entry', srcPath:'./entry', targets:[{name:'default'}]}] };
  // Comments, CRLF, trailing commas and original formatting must return byte-for-byte.
  const original = Buffer.from('// preserve this formatting and CRLF\r\n' + JSON.stringify(profile, null, 3).replace(/\n/g, '\r\n').replace(/\r\n}$/, ',\r\n}') + '\r\n');
  fs.writeFileSync(path.join(project, 'build-profile.json5'), original);
  writeJSON(path.join(project, 'oh-package.json5'), {dependencies:{}, devDependencies:{}});
  writeJSON(path.join(project, 'entry/oh-package.json5'), {dependencies:{'libentry.so':'file:./src/main/cpp/types/libentry'}});
  fs.mkdirSync(path.join(project, 'entry/src/main/cpp/types/libentry'), {recursive:true, mode:0o700});
  writeJSON(path.join(project, 'hvigor/hvigor-config.json5'), {dependencies:{}, execution:{daemon:false, incremental:true}});
  writeJSON(path.join(project, 'fixture-control.json'), options.control || {});
  const request = { schemaVersion:1, projectDir:project, sdkRoot:sdk, hvigorEntry:entry,
    buildMode:options.mode || 'debug', format:options.format || 'hap', workDir:work };
  if (options.signed) {
    const material = path.join(root, 'private signing');
    fs.mkdirSync(material, {mode:0o700});
    for (const name of ['certificate.cer','profile.p7b','store.p12']) fs.writeFileSync(path.join(material,name),'SYNTHETIC MATERIAL', {mode:0o600});
    request.signing = {certificate:path.join(material,'certificate.cer'), profile:path.join(material,'profile.p7b'),
      storeFile:path.join(material,'store.p12'), keyAlias:'fixture private alias', signAlg:'SHA256withECDSA',
      keyPassword:'wrong key with 空格 and spaces !!', storePassword:'wrong store with spaces " / \\ !!'};
  }
  const requestPath = path.join(work, 'request.json');
  function run(changes = {}, env = {}) {
    writeJSON(requestPath, {...request, ...changes});
    return spawnSync(process.execPath, [RUNNER, '--request', requestPath], {
      env:{...process.env, JAVA_HOME:'/no/java/should/be/used', DEVECO_SDK_HOME:'/unrelated/sdk',
        NODE_PATH:'fixture inherited node lookup', ...env}, encoding:'utf8', timeout:20000,
    });
  }
  function restored() { assert.deepEqual(fs.readFileSync(path.join(project,'build-profile.json5')), original); }
  return { root, project, sdk, work, entry, request, requestPath, run, restored,
    resultPath:path.join(work,'result.json'), invocationPath:path.join(work,'invocation.json') };
}
function expectFailure(f, response) {
  assert.equal(response.error, undefined);
  assert.notEqual(response.status, 0);
  assert.equal(fs.existsSync(f.resultPath), false);
  f.restored();
}

for (const format of ['hap','app']) for (const mode of ['debug','release']) for (const signed of [false,true]) {
  test(`${format} ${mode} ${signed ? 'signed' : 'unsigned'}: node spawn, isolated env, exact profile restoration`, t => {
    const f = fixture(t, {format, mode, signed});
    const response = f.run();
    assert.equal(response.error, undefined);
    assert.equal(response.status, 0, response.stderr);
    const invoked = JSON.parse(fs.readFileSync(f.invocationPath));
    assert.deepEqual(invoked.args, buildArguments(f.request));
    assert.equal(invoked.args.includes('--parallel'),signed && format === 'app');
    assert.equal(invoked.args.includes('--no-daemon'),true);
    assert.equal(invoked.args.includes('--no-incremental'),true);
    assert.equal(invoked.execPath, process.execPath);
    assert.equal(invoked.cwd, f.project);
    assert.equal(invoked.env.JAVA_HOME, undefined);
    assert.equal(invoked.env.DEVECO_SDK_HOME, undefined);
    assert.equal(invoked.env.OHOS_SDK_HOME, f.sdk);
    assert.equal(invoked.env.OHOS_BASE_SDK_HOME, f.sdk);
    assert.equal(invoked.env.HVIGOR_USER_HOME, f.work);
    assert.equal(invoked.env.TMPDIR, path.join(f.work,'tmp'));
    assert.equal(invoked.env.HOME, path.join(f.work,'home'));
    assert.equal(invoked.env.NODE_PATH, 'fixture inherited node lookup');
    assert.equal(invoked.env.NPM_CONFIG_OFFLINE, 'true');
    assert.equal(invoked.passwordsPresent, true);
    assert.equal(invoked.envReferences, true);
    const generatedText = fs.readFileSync(path.join(f.work,'generated-profile.json'),'utf8');
    const generated = JSON.parse(generatedText);
    assert.equal(generated.app.signingConfigs.length, signed ? 1 : 0);
    if (signed) {
      assert.equal(generated.app.signingConfigs[0].type, 'OpenHarmony');
      assert.equal(generated.app.signingConfigs[0].material.certpath, f.request.signing.certificate);
      assert.equal(generatedText.includes(f.request.signing.keyPassword), false);
      assert.equal(generatedText.includes(f.request.signing.storePassword), false);
    }
    if (signed && format === 'app') {
      assert.equal(generated.app.products[0].buildOption.packOptions.appWithSignedPkg,true);
      assert.equal(generated.app.products[0].buildOption.packOptions.buildAppSkipSignHap,false);
    }
    const expectedName = `fixture-default-${signed?(format==='app'?'all-signed':'signed'):'unsigned'}.${format}`;
    const outDir = format === 'hap' ? 'entry/build/default/outputs/default' : 'build/outputs/default';
    assert.deepEqual(JSON.parse(fs.readFileSync(f.resultPath)), {schemaVersion:1,
      outputPath:path.join(f.project,outDir,expectedName), mode, format, signed});
    assert.equal(fs.statSync(f.resultPath).mode & 0o777, 0o600);
    assert.match(response.stdout, /fixture build stdout/);
    assert.match(response.stderr, /fixture build stderr/);
    f.restored();
    assert.equal(fs.existsSync(path.join(f.project,'.godot-build-runner.lock')), false);
    assert.equal(fs.existsSync(path.join(f.work,'.runner-lock')), false);
  });
}

for (const [label,packOptions] of [['missing',undefined],['false',{appWithSignedPkg:false,buildAppSkipSignHap:true}],
  ['true',{appWithSignedPkg:true,buildAppSkipSignHap:true}],['already enabled',{appWithSignedPkg:true,buildAppSkipSignHap:false}]]) {
  test(`signed APP overrides ${label} options for inner signing, selects all-signed, then restores bytes`, t => {
    const options={format:'app',signed:true};
    if(packOptions!==undefined) options.packOptions=packOptions;
    const f=fixture(t,options);
    assert.equal(f.run().status,0);
    const generated=JSON.parse(fs.readFileSync(path.join(f.work,'generated-profile.json')));
    assert.equal(generated.app.products[0].buildOption.packOptions.appWithSignedPkg,true);
    assert.equal(generated.app.products[0].buildOption.packOptions.buildAppSkipSignHap,false);
    assert.match(JSON.parse(fs.readFileSync(f.resultPath)).outputPath,/-all-signed\.app$/);
    f.restored();
  });
}
for(const format of ['hap','app']) {
  test(`${format} unsigned does not rewrite existing pack signing options`,t=>{
    const packOptions={appWithSignedPkg:false,buildAppSkipSignHap:true};
    const f=fixture(t,{format,packOptions});assert.equal(f.run().status,0);
    const generated=JSON.parse(fs.readFileSync(path.join(f.work,'generated-profile.json')));
    assert.deepEqual(generated.app.products[0].buildOption.packOptions,packOptions);
    f.restored();
  });
}
test('signed HAP leaves original pack signing options untouched',t=>{
  const packOptions={appWithSignedPkg:false,buildAppSkipSignHap:true};
  const f=fixture(t,{signed:true,packOptions});assert.equal(f.run().status,0);
  const generated=JSON.parse(fs.readFileSync(path.join(f.work,'generated-profile.json')));
  assert.deepEqual(generated.app.products[0].buildOption.packOptions,packOptions);f.restored();
});
test('equal DEVECO SDK is retained, and a workDir can be reused serially', t => {
  const f = fixture(t);
  assert.equal(f.run({}, {DEVECO_SDK_HOME:f.sdk}).status, 0);
  assert.equal(JSON.parse(fs.readFileSync(f.invocationPath)).env.DEVECO_SDK_HOME, f.sdk);
  assert.equal(f.run().status, 0);
  f.restored();
});
test('wrong password build failure leaks no secret even across stream chunks', t => {
  const f = fixture(t, {signed:true, control:{failure:true}});
  const response = f.run();
  expectFailure(f,response);
  const combined = response.stdout + response.stderr;
  for (const [key,value] of Object.entries(f.request.signing)) {
    if (key !== 'signAlg') assert.equal(combined.includes(value), false, `leaked ${key}`);
  }
  assert.match(combined, /\[REDACTED\]/);
  assert.match(combined, /Hvigor build failed/);
});
for (const [label,control] of Object.entries({missing:{missing:true}, empty:{empty:true}, ambiguous:{ambiguous:true},
  'wrong signing suffix':{wrongSuffix:true}, 'artifact symlink':{symlink:true}})) {
  test(`${label} artifact fails closed and restores original profile`, t => {
    const f = fixture(t, {signed:true,control});
    expectFailure(f,f.run());
  });
}
for (const info of [{adapter:'6.26.4-ohos.2'}, {host:'linux'}, {arch:'x64'}, {upstream:'6.26.5'},
  {pinnedInterfacesUnmodified:false}, {pinnedInterfacesUnmodified:'true'}]) {
  test(`adapter preflight rejects ${JSON.stringify(info)} before building`, t => {
    const f = fixture(t, {control:{info}});
    expectFailure(f,f.run());
    assert.equal(fs.existsSync(f.invocationPath), false);
  });
}
test('adapter execution failure does not mutate generated profile', t => {
  const f = fixture(t, {control:{probeFailure:true}});
  expectFailure(f,f.run());
  assert.equal(fs.existsSync(f.invocationPath), false);
});
for (const [label,changes] of Object.entries({unknown:{unknown:true}, version:{schemaVersion:2},
  'numeric version string':{schemaVersion:'1'}, mode:{buildMode:'profile'}, format:{format:'zip'},
  'relative project':{projectDir:'relative'}, 'relative SDK':{sdkRoot:'relative'},
  'invalid signing':{signing:{keyPassword:'never print this password'}}})) {
  test(`strict request rejects ${label}`, t => {
    const f = fixture(t);
    expectFailure(f,f.run(changes));
  });
}
test('normal FS privacy is required; chmod is not attempted by runner', t => {
  const f = fixture(t);
  fs.chmodSync(f.work,0o770); // Test deliberately unsafe effective mode, on TMPDIR.
  expectFailure(f,f.run());
});
test('non-private request and symlink request are rejected without exposing contents', t => {
  const f = fixture(t,{signed:true});
  writeJSON(f.requestPath,f.request,0o660);
  fs.chmodSync(f.requestPath,0o660);
  let response = spawnSync(process.execPath,[RUNNER,'--request',f.requestPath],{encoding:'utf8'});
  expectFailure(f,response);
  fs.chmodSync(f.requestPath,0o600);
  const link = path.join(f.work,'request-link.json'); fs.symlinkSync(f.requestPath,link);
  response = spawnSync(process.execPath,[RUNNER,'--request',link],{encoding:'utf8'});
  expectFailure(f,response);
  assert.equal((response.stdout+response.stderr).includes(f.request.signing.keyPassword),false);
});
test('offline preflight rejects registry dependencies before touching profile', t => {
  const f = fixture(t);
  writeJSON(path.join(f.project,'hvigor/hvigor-config.json5'), {dependencies:{'@ohos/hvigor-ohos-plugin':'6.26.4'}});
  expectFailure(f,f.run());
  assert.equal(fs.existsSync(f.invocationPath),false);
});
test('previous success is removed on a later build failure; stale artifacts cannot succeed', t => {
  const f = fixture(t);
  assert.equal(f.run().status,0);
  writeJSON(path.join(f.project,'fixture-control.json'),{missing:true});
  expectFailure(f,f.run());
});
for (const scope of ['work','project']) {
  test(`an existing ${scope} lock is never removed by a rejected concurrent build`, t => {
    const f = fixture(t);
    const lock = scope === 'work' ? path.join(f.work,'.runner-lock') : path.join(f.project,'.godot-build-runner.lock');
    fs.mkdirSync(lock,{mode:0o700});
    expectFailure(f,f.run());
    assert.equal(fs.existsSync(lock),true);
  });
}
test('interrupted profile backup is not overwritten or deleted', t => {
  const f = fixture(t);
  const backup = path.join(f.project,'.godot-build-runner-original.json5');
  fs.writeFileSync(backup,'CRASH RECOVERY ORIGINAL');
  expectFailure(f,f.run());
  assert.equal(fs.readFileSync(backup,'utf8'),'CRASH RECOVERY ORIGINAL');
});
test('SIGTERM is forwarded and original profile is restored before runner exits', async t => {
  const f = fixture(t, {control:{hold:true}});
  writeJSON(f.requestPath, f.request);
  const child = spawn(process.execPath, [RUNNER,'--request',f.requestPath], {env:process.env,stdio:['ignore','pipe','pipe']});
  let output=''; let errors=''; let signalled=false;
  child.stdout.on('data', bytes => {
    output+=bytes.toString();
    if (!signalled && output.includes('fixture waiting for termination')) {
      signalled=true;
      child.kill('SIGTERM');
    }
  });
  child.stderr.on('data', bytes => {errors+=bytes.toString();});
  const deadline=setTimeout(() => child.kill('SIGTERM'),15000);
  const response=await new Promise((resolve,reject) => {
    child.on('error',reject);
    child.on('close',(status,signal) => resolve({status,signal,stdout:output,stderr:errors}));
  }).finally(() => clearTimeout(deadline));
  assert.equal(signalled,true,'child never reached controlled active build');
  expectFailure(f,response);
  assert.equal(response.signal,null);
  assert.equal(fs.existsSync(path.join(f.project,'.godot-build-runner.lock')),false);
});

test('redactor handles every UTF-8 byte boundary and overlapping secret values', () => {
  const secret = 'Unicode 空格 password with spaces';
  for (let split=0; split<=Buffer.byteLength(secret); ++split) {
    let output=''; const stream = redactedStream({write:text=>{output+=text;}},[secret,'password']);
    const bytes=Buffer.from('before '+secret+' after');
    const cut=Buffer.byteLength('before ')+split;
    stream.write(bytes.subarray(0,cut)); stream.write(bytes.subarray(cut)); stream.end();
    assert.equal(output,'before [REDACTED] after', `byte boundary ${split}`);
  }
});
