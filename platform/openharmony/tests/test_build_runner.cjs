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
const e = {}; e.PNPM_VERSION = "10.28.2";
const fs = require('node:fs');
const path = require('node:path');
const root = process.cwd();
const control = JSON.parse(fs.readFileSync(path.join(root, 'fixture-control.json')));
const args = process.argv.slice(2);
const profile = fs.readFileSync(path.join(root, 'build-profile.json5'));
const doc = JSON.parse(profile);
const material = doc.app.signingConfigs[0]?.material;
const work = path.dirname(process.env.HOME);
fs.writeFileSync(path.join(work, 'generated-profile.json'), profile);
fs.writeFileSync(path.join(work, 'invocation.json'), JSON.stringify({
  args, wrapper:process.argv[1], execPath:process.execPath, cwd:root,
  env:Object.fromEntries(['JAVA_HOME','DEVECO_SDK_HOME','OHOS_SDK_HOME','OHOS_BASE_SDK_HOME',
    'HVIGOR_USER_HOME','HVIGOR_WRAPPER_HOME','TMPDIR','TMP','TEMP','HOME','NODE_PATH','NODE_OPTIONS','NODE_OPTS',
    'NODE_HOME','CANGJIE_BUILD_SUPPORT_PATH','NPM_CONFIG_OFFLINE'].map(k => [k,process.env[k]])),
  secretEnvAbsent:!Object.keys(process.env).some(k=>/^OHECO_HVIGOR_/.test(k)),
}));
const format = args.includes('assembleHap') ? 'hap' : 'app';
const out = format === 'hap' ? path.join(root, 'entry/build/default/outputs/default') : path.join(root, 'build/outputs/default');
const signed = Boolean(material);
const all = format === 'app' && doc.app.products[0].buildOption?.packOptions?.appWithSignedPkg === true;
const suffix = signed ? (all ? '-all-signed' : '-signed') : '-unsigned';
if (control.failure) {
  const secrets = JSON.parse(fs.readFileSync(path.join(path.dirname(material.storeFile),'fixture-secrets.json')));
  const a = 'wrong key password: ' + secrets.plaintext[0] + '\ncipher: ' + material.keyPassword + '\n';
  const b = 'wrong store password: ' + secrets.plaintext[1] + '\ncipher: ' + material.storePassword + '\n';
  const cutA=Math.floor(a.length/2),cutB=Math.floor(b.length/2);
  process.stdout.write(a.slice(0,cutA)); process.stderr.write(b.slice(0,cutB));
  setTimeout(()=>{process.stdout.write(a.slice(cutA));process.stderr.write(b.slice(cutB));
    console.error('material '+[material.certpath,material.profile,material.storeFile,material.keyAlias].join(' '));process.exitCode=7;
  },15);
} else {
  fs.mkdirSync(out,{recursive:true});
  if (!control.missing) {
    const name='fixture-default'+(control.wrongSuffix?'-unsigned':suffix)+'.'+format;
    const file=path.join(out,name);
    if(control.symlink) fs.symlinkSync(path.join(root,'fixture-control.json'),file);
    else fs.writeFileSync(file,control.empty?'':'SYNTHETIC PACKAGE, NOT A REAL HAP/APP');
    if(control.ambiguous) fs.writeFileSync(path.join(out,'other-default'+suffix+'.'+format),'OTHER SYNTHETIC PACKAGE');
  }
  fs.mkdirSync(path.join(root,'entry/build/other/outputs/default'),{recursive:true});
  fs.writeFileSync(path.join(root,'entry/build/other/outputs/default/distractor-signed.hap'),'DISTRACTOR');
  fs.mkdirSync(path.join(out,'nested'),{recursive:true});
  fs.writeFileSync(path.join(out,'nested/distractor'+suffix+'.'+format),'DISTRACTOR');
  console.log('fixture build stdout');console.error('fixture build stderr');
  if(control.hold) {
    // Model the official wrapper's forked engine, which must also receive signals.
    const {spawn}=require('node:child_process');
    const source="const fs=require('node:fs');process.on('SIGTERM',()=>{fs.writeFileSync(process.argv[1],'terminated');process.exit(0)});process.on('SIGINT',()=>{fs.writeFileSync(process.argv[1],'interrupted');process.exit(0)});console.log('fixture waiting for termination'+'.'.repeat(2048));setInterval(()=>{},1000)";
    const engine=spawn(process.execPath,['--eval',source,path.join(work,'engine-signal')],{stdio:'inherit'});
    process.on('SIGTERM',()=>engine.on('close',()=>process.exit(0)));
    process.on('SIGINT',()=>engine.on('close',()=>process.exit(0)));
  }
}
`;
const FAKE_DECIPHER = String.raw`'use strict';
// The installed plugin logger imports this peer before exposing decryptPwd.
require('@ohos/hvigor');
const fs=require('node:fs'),path=require('node:path');
exports.DecipherUtil={decryptPwd(materialDir,encrypted,profilePath){
  const input=JSON.parse(fs.readFileSync(path.join(materialDir,'fixture-secrets.json')));
  const calls=path.join(materialDir,'decrypt-calls.json');
  const history=fs.existsSync(calls)?JSON.parse(fs.readFileSync(calls)):[];
  history.push({materialDir,encrypted,profilePath});fs.writeFileSync(calls,JSON.stringify(history));
  if(input.probeNoise||input.probeFailure){console.log(input.plaintext.join(' '));console.error(input.encrypted.join(' '));}
  if(input.probeFailure==='exit') process.exit(0);
  if(input.probeFailure==='throw') throw new Error(input.plaintext[0]);
  if(input.probeFailure==='response') return 1;
  const index=input.encrypted.indexOf(encrypted);if(index<0) throw new Error('invalid fixture token');
  return input.plaintext[index];
}};
`;
function encryptedToken(id) {
  const content=Buffer.from('opaque fixture token '+id);
  const frame=Buffer.alloc(16+content.length+16,0xab);
  frame.writeUInt32BE(content.length+16);content.copy(frame,16);
  return frame.toString('hex');
}

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
  const toolchain = path.join(root,'command line tools with spaces; no shell');
  const home = path.join(root,'persistent-hvigor');
  const entry = path.join(toolchain,'hvigor/bin/hvigorw.js');
  fs.mkdirSync(path.dirname(entry),{recursive:true,mode:0o700});
  fs.writeFileSync(entry, FAKE_HVIGOR);
  fs.writeFileSync(path.join(path.dirname(entry),'hvigorw'),'SYNTHETIC SHELL LAUNCHER; MUST NOT EXECUTE');
  const selectedNode = path.join(toolchain,'node/bin/node');
  fs.mkdirSync(path.dirname(selectedNode),{recursive:true,mode:0o700});
  fs.symlinkSync(process.execPath,selectedNode);
  writeJSON(path.join(toolchain,'hvigor/hvigor/package.json'),{name:'@ohos/hvigor',version:'7.26.8',main:'index.cjs'});
  fs.writeFileSync(path.join(toolchain,'hvigor/hvigor/index.cjs'),'module.exports = {};');
  writeJSON(path.join(toolchain,'hvigor/hvigor-ohos-plugin/package.json'),{name:'@ohos/hvigor-ohos-plugin',version:'7.26.8',peerDependencies:{'@ohos/hvigor':'7.26.8'}});
  const decipher = path.join(toolchain,'hvigor/hvigor-ohos-plugin/src/utils/decipher-util.js');
  fs.mkdirSync(path.dirname(decipher),{recursive:true,mode:0o700});fs.writeFileSync(decipher,FAKE_DECIPHER);
  const toolsDir = path.join(home,'wrapper/tools/10.28.2');
  writeJSON(path.join(toolsDir,'node_modules/pnpm/package.json'),{name:'pnpm',version:'10.28.2',main:'index.cjs'});
  fs.writeFileSync(path.join(toolsDir,'node_modules/pnpm/index.cjs'),'throw new Error("pnpm must not execute");');
  fs.mkdirSync(path.join(toolsDir,'node_modules/.bin'),{recursive:true,mode:0o700});
  fs.writeFileSync(path.join(toolsDir,'node_modules/.bin/pnpm'),'SYNTHETIC PNPM LAUNCHER; MUST NOT EXECUTE');
  const family = options.sdkFamily || 'OpenHarmony';
  const foundation = family === 'HarmonyOS' ? path.join(sdk,'default/openharmony') : path.join(sdk,'26.0.0');
  for (const component of ['native','ets','js','toolchains','previewer']) {
    writeJSON(path.join(foundation,component,'oh-uni-package.json'),
      {apiVersion:'26',platformVersion:'26.0.0',path:component,version:'26.0.0.105',releaseType:'Release'});
  }
  if (family === 'HarmonyOS') {
    writeJSON(path.join(sdk,'default/sdk-pkg.json'), {data:{apiVersion:'26',platformVersion:'26.0.0',version:'26.0.0.105',releaseType:'Release'}});
    for (const component of ['native','ets','toolchains']) {
      writeJSON(path.join(sdk,'default/hms',component,'uni-package.json'),
        {apiVersion:'26',platformVersion:'26.0.0',path:component,version:'26.0.0.105',releaseType:'Release'});
    }
  }
  const profile = { app: { signingConfigs: [{name:'original-encrypted-signing'}], products: [{
    name:'default', signingConfig:'original-encrypted-signing',
    runtimeOS:Object.hasOwn(options,'runtimeOS') ? options.runtimeOS : family,
    compileSdkVersion:'26.0.0', targetSdkVersion:'26.0.0', compatibleSdkVersion:'26.0.0',
    ...(Object.hasOwn(options,'packOptions') ? {buildOption:{packOptions:options.packOptions}} : {}),
  }] }, modules:[{name:'entry', srcPath:'./entry', targets:[{name:'default'}]}] };
  // Comments, CRLF, trailing commas and original formatting must return byte-for-byte.
  const original = Buffer.from('// preserve this formatting and CRLF\r\n' + JSON.stringify(profile, null, 3).replace(/\n/g, '\r\n').replace(/\r\n}$/, ',\r\n}') + '\r\n');
  fs.writeFileSync(path.join(project, 'build-profile.json5'), original);
  const originalMode=fs.statSync(path.join(project,'build-profile.json5')).mode & 0o777;
  writeJSON(path.join(project, 'oh-package.json5'), {dependencies:{}, devDependencies:{}});
  writeJSON(path.join(project, 'entry/oh-package.json5'), {dependencies:{'libentry.so':'file:./src/main/cpp/types/libentry'}});
  fs.mkdirSync(path.join(project, 'entry/src/main/cpp/types/libentry'), {recursive:true, mode:0o700});
  writeJSON(path.join(project, 'hvigor/hvigor-config.json5'), {dependencies:{}, execution:{daemon:false, incremental:true,parallel:false}});
  for(const [relative,task] of [['hvigorfile.ts','appTasks'],['entry/hvigorfile.ts','hapTasks']]) {
    fs.writeFileSync(path.join(project,relative),`import { ${task} } from '@ohos/hvigor-ohos-plugin';\nexport default { system: ${task}, /* stock template */ plugins: [] }\n`);
  }
  writeJSON(path.join(project, 'fixture-control.json'), options.control || {});
  const request = { schemaVersion:2, projectDir:project, sdkRoot:sdk, commandLineToolsRoot:toolchain,
    buildMode:options.mode || 'debug', format:options.format || 'hap', workDir:work, hvigorUserHome:home };
  const plaintext=['wrong key with 空格 and spaces !!','wrong store with spaces " / \\ !!'];
  if (options.signed) {
    const material = path.join(root, 'private signing');
    fs.mkdirSync(material, {mode:0o700});
    for (const name of ['certificate.cer','profile.p7b','store.p12']) fs.writeFileSync(path.join(material,name),'SYNTHETIC MATERIAL', {mode:0o600});
    request.signing = {certificate:path.join(material,'certificate.cer'), profile:path.join(material,'profile.p7b'),
      storeFile:path.join(material,'store.p12'), keyAlias:'fixture private alias', signAlg:'SHA256withECDSA',
      keyPasswordEncrypted:encryptedToken('key').toUpperCase(), storePasswordEncrypted:encryptedToken('store')};
    for(const name of ['fd','ac','ce']) {
      fs.mkdirSync(path.join(material,'material',name),{recursive:true,mode:0o700});
      fs.writeFileSync(path.join(material,'material',name,'component'),'SYNTHETIC COMPANION MATERIAL');
    }
    writeJSON(path.join(material,'fixture-secrets.json'),{plaintext,
      encrypted:[request.signing.keyPasswordEncrypted,request.signing.storePasswordEncrypted],...options.probe});
  }
  const requestPath = path.join(work, 'request.json');
  function run(changes = {}, env = {}) {
    writeJSON(requestPath, {...request, ...changes});
    return spawnSync(process.execPath, [RUNNER, '--request', requestPath], {
      env:{...process.env, JAVA_HOME:'/no/java/should/be/used', DEVECO_SDK_HOME:'/unrelated/sdk',
        NODE_PATH:'fixture inherited node lookup',NODE_OPTS:'unrelated launcher override',
        HVIGOR_WRAPPER_HOME:'/unrelated/wrapper',CANGJIE_BUILD_SUPPORT_PATH:'/unrelated/plugin',
        OHECO_HVIGOR_KEY_PASSWORD:'old key secret',OHECO_HVIGOR_STORE_PASSWORD:'old store secret', ...env}, encoding:'utf8', timeout:20000,
    });
  }
  function restored() {
    assert.deepEqual(fs.readFileSync(path.join(project,'build-profile.json5')), original);
    assert.equal(fs.statSync(path.join(project,'build-profile.json5')).mode & 0o777,originalMode);
  }
  return { root, project, sdk, work, home, toolchain, toolsDir, selectedNode, decipher, plaintext, entry, request, requestPath, run, restored,
    resultPath:path.join(work,'result.json'), invocationPath:path.join(work,'invocation.json') };
}
function expectFailure(f, response) {
  assert.equal(response.error, undefined);
  assert.notEqual(response.status, 0);
  assert.equal(fs.existsSync(f.resultPath), false);
  f.restored();
}

for (const runtimeOS of ['HarmonyOS', undefined]) {
  test(`reject SDK/profile mode mismatch ${runtimeOS} before building and preserve original profile`, t => {
    const f = fixture(t, {runtimeOS});
    const response = f.run();
    expectFailure(f, response);
    assert.match(response.stderr, /runtimeOS does not match the selected SDK metadata/);
    assert.equal(fs.existsSync(f.invocationPath), false);
  });
}

for (const signed of [false,true]) {
  test(`HarmonyOS SDK selects DEVECO root and preserves profile (${signed ? 'signed' : 'unsigned'})`, t => {
    const f = fixture(t, {sdkFamily:'HarmonyOS', signed});
    const response = f.run({sdkRoot:path.join(f.sdk,'default')});
    assert.equal(response.status, 0, response.stderr);
    const invoked = JSON.parse(fs.readFileSync(f.invocationPath));
    assert.equal(invoked.env.DEVECO_SDK_HOME, f.sdk);
    assert.equal(invoked.env.OHOS_BASE_SDK_HOME, undefined);
    const generated = JSON.parse(fs.readFileSync(path.join(f.work,'generated-profile.json')));
    assert.equal(generated.app.products[0].runtimeOS, 'HarmonyOS');
    assert.equal(generated.app.products[0].compatibleSdkVersion, '26.0.0');
    if (signed) assert.equal(generated.app.signingConfigs[0].type, 'HarmonyOS');
    f.restored();
  });
}
test('stale sdk.dir is rejected before building; matching escaped paths are accepted', t => {
  const f = fixture(t);
  const properties = path.join(f.project,'local.properties');
  fs.writeFileSync(properties,'sdk.dir=/unrelated/SDK\n');
  const response = f.run();
  expectFailure(f,response);
  assert.match(response.stderr,/sdk.dir conflicts/);
  assert.equal(fs.existsSync(f.invocationPath),false);
  fs.writeFileSync(properties,'sdk.dir=' + f.sdk.replaceAll(' ','\\ ') + '\n');
  assert.equal(f.run().status,0);
  f.restored();
});
test('SDK directory aliases and numeric API metadata are accepted; null identities are rejected', t => {
  const f = fixture(t, {sdkFamily:'HarmonyOS'});
  const paths = [path.join(f.sdk,'default/sdk-pkg.json'),
    ...['native','ets','js','toolchains','previewer'].map(component => path.join(f.sdk,'default/openharmony',component,'oh-uni-package.json')),
    ...['native','ets','toolchains'].map(component => path.join(f.sdk,'default/hms',component,'uni-package.json'))];
  for (const filename of paths) {
    const document = JSON.parse(fs.readFileSync(filename));
    (document.data || document).apiVersion = 26;
    writeJSON(filename,document);
  }
  const alias = path.join(f.root,'sdk-directory-alias');
  fs.symlinkSync(path.join(f.sdk,'default'),alias,'dir');
  assert.equal(f.run({sdkRoot:alias}).status,0);
  f.restored();
  for (const filename of paths) {
    const document = JSON.parse(fs.readFileSync(filename));
    const info = document.data || document;
    info.version = info.releaseType = null;
    writeJSON(filename,document);
  }
  expectFailure(f,f.run({sdkRoot:alias}));
});
test('incoherent SDK metadata is rejected before invoking the build', t => {
  const f = fixture(t);
  writeJSON(path.join(f.sdk,'26.0.0/native/oh-uni-package.json'),
    {apiVersion:'25',platformVersion:'26.0.0',path:'native',version:'26.0.0.105',releaseType:'Release'});
  expectFailure(f,f.run());
  assert.equal(fs.existsSync(f.invocationPath),false);
});

for (const format of ['hap','app']) for (const mode of ['debug','release']) for (const signed of [false,true]) {
  test(`${format} ${mode} ${signed ? 'signed' : 'unsigned'}: node spawn, isolated env, exact profile restoration`, t => {
    const f = fixture(t, {format, mode, signed});
    const response = f.run();
    assert.equal(response.error, undefined);
    assert.equal(response.status, 0, response.stderr);
    const invoked = JSON.parse(fs.readFileSync(f.invocationPath));
    assert.deepEqual(invoked.args, buildArguments(f.request));
    assert.equal(invoked.args.includes('--parallel'),false);
    assert.equal(invoked.wrapper,f.entry);
    assert.equal(invoked.args.includes('--no-daemon'),true);
    assert.equal(invoked.args.includes('--no-incremental'),true);
    assert.equal(invoked.execPath, process.execPath);
    assert.equal(invoked.cwd, f.project);
    assert.equal(invoked.env.JAVA_HOME, undefined);
    assert.equal(invoked.env.DEVECO_SDK_HOME, undefined);
    assert.equal(invoked.env.OHOS_SDK_HOME, f.sdk);
    assert.equal(invoked.env.OHOS_BASE_SDK_HOME, f.sdk);
    assert.equal(invoked.env.HVIGOR_USER_HOME, f.home);
    assert.equal(invoked.env.NODE_HOME,path.dirname(f.selectedNode));
    assert.equal(invoked.env.HVIGOR_WRAPPER_HOME,undefined);
    assert.equal(invoked.env.CANGJIE_BUILD_SUPPORT_PATH,undefined);
    assert.equal(invoked.env.NODE_OPTS,undefined);
    assert.equal(invoked.env.NODE_OPTIONS,undefined);
    assert.equal(invoked.env.TMP,path.join(f.work,'tmp'));
    assert.equal(invoked.env.TEMP,path.join(f.work,'tmp'));
    assert.equal(invoked.env.TMPDIR, path.join(f.work,'tmp'));
    assert.equal(invoked.env.HOME, path.join(f.work,'home'));
    assert.equal(invoked.env.NODE_PATH, undefined);
    assert.equal(invoked.env.NPM_CONFIG_OFFLINE, 'true');
    assert.equal(invoked.secretEnvAbsent,true);
    const generatedText = fs.readFileSync(path.join(f.work,'generated-profile.json'),'utf8');
    const generated = JSON.parse(generatedText);
    assert.equal(generated.app.products[0].runtimeOS, 'OpenHarmony');
    assert.equal(generated.app.signingConfigs.length, signed ? 1 : 0);
    if (signed) {
      assert.equal(generated.app.signingConfigs[0].type, 'OpenHarmony');
      assert.equal(generated.app.signingConfigs[0].material.certpath, f.request.signing.certificate);
      assert.equal(generated.app.signingConfigs[0].material.keyPassword,f.request.signing.keyPasswordEncrypted);
      assert.equal(generated.app.signingConfigs[0].material.storePassword,f.request.signing.storePasswordEncrypted);
      for(const secret of f.plaintext) assert.equal(generatedText.includes(secret),false);
      const calls=JSON.parse(fs.readFileSync(path.join(path.dirname(f.request.signing.storeFile),'decrypt-calls.json')));
      assert.deepEqual(calls.map(call=>call.encrypted),[f.request.signing.keyPasswordEncrypted,f.request.signing.storePasswordEncrypted]);
      assert.ok(calls.every(call=>call.materialDir===path.dirname(f.request.signing.storeFile)&&call.profilePath===path.join(f.project,'build-profile.json5')));
    }
    if (signed && format === 'app') {
      assert.equal(generated.app.products[0].buildOption.packOptions.appWithSignedPkg,true);
      assert.equal(generated.app.products[0].buildOption.packOptions.buildAppSkipSignHap,false);
    }
    const expectedName = `fixture-default-${signed?(format==='app'?'all-signed':'signed'):'unsigned'}.${format}`;
    const outDir = format === 'hap' ? 'entry/build/default/outputs/default' : 'build/outputs/default';
    assert.deepEqual(JSON.parse(fs.readFileSync(f.resultPath)), {schemaVersion:2,
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
  for(const secret of f.plaintext) assert.equal(combined.includes(secret),false,'leaked plaintext');
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
for(const [label,engine,plugin,peer] of [
  ['old adapter','6.26.4','6.26.4','6.26.4'], ['unsupported major','8.26.8','8.26.8','8.26.8'],
  ['below minimum','7.26.7','7.26.7','7.26.7'], ['incoherent versions','7.26.8','7.26.9','7.26.8'],
  ['bad peer','7.26.8','7.26.8','7.26.9'],['prerelease','7.26.8-beta.1','7.26.8-beta.1','7.26.8-beta.1'],
]) {
  test(`official toolchain rejects ${label} before wrapper launch`,t=>{
    const f=fixture(t);
    writeJSON(path.join(f.toolchain,'hvigor/hvigor/package.json'),{name:'@ohos/hvigor',version:engine});
    writeJSON(path.join(f.toolchain,'hvigor/hvigor-ohos-plugin/package.json'),{name:'@ohos/hvigor-ohos-plugin',version:plugin,peerDependencies:{'@ohos/hvigor':peer}});
    expectFailure(f,f.run());assert.equal(fs.existsSync(f.invocationPath),false);
  });
}
test('compatible newer coherent major 7 is accepted',t=>{
  const f=fixture(t);
  writeJSON(path.join(f.toolchain,'hvigor/hvigor/package.json'),{name:'@ohos/hvigor',version:'7.27.0'});
  writeJSON(path.join(f.toolchain,'hvigor/hvigor-ohos-plugin/package.json'),{name:'@ohos/hvigor-ohos-plugin',version:'7.27.0',peerDependencies:{'@ohos/hvigor':'7.27.0'}});
  assert.equal(f.run().status,0);f.restored();
});
for (const [label,changes] of Object.entries({unknown:{unknown:true}, version:{schemaVersion:1},
  'numeric version string':{schemaVersion:'2'}, mode:{buildMode:'profile'}, format:{format:'zip'},
  'relative project':{projectDir:'relative'}, 'relative SDK':{sdkRoot:'relative'},
  'relative toolchain':{commandLineToolsRoot:'relative'}, 'relative user home':{hvigorUserHome:'relative'},
  'obsolete adapter field':{hvigorEntry:'/unrelated/adapter.cjs'},
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
  assert.equal((response.stdout+response.stderr).includes(f.request.signing.keyPasswordEncrypted),false);
});
test('offline preflight rejects registry dependencies before touching profile', t => {
  const f = fixture(t);
  writeJSON(path.join(f.project,'hvigor/hvigor-config.json5'), {dependencies:{'@ohos/hvigor-ohos-plugin':'7.26.8'}});
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
for(const signal of ['SIGTERM','SIGINT']) test(`${signal} reaches wrapper and engine; signed profile is restored before runner exits`, async t => {
  const f = fixture(t, {signed:true,control:{hold:true}});
  writeJSON(f.requestPath, f.request);
  const child = spawn(process.execPath, [RUNNER,'--request',f.requestPath], {env:process.env,stdio:['ignore','pipe','pipe']});
  let output=''; let errors=''; let signalled=false;
  child.stdout.on('data', bytes => {
    output+=bytes.toString();
    if (!signalled && output.includes('fixture waiting for termination')) {
      signalled=true;
      child.kill(signal);
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
  assert.equal(fs.readFileSync(path.join(f.work,'engine-signal'),'utf8'),signal==='SIGTERM'?'terminated':'interrupted');
  assert.equal(fs.existsSync(path.join(f.project,'.godot-build-runner.lock')),false);
});

for(const [label,relative] of [
  ['missing bundled Node','node/bin/node'], ['missing wrapper','hvigor/bin/hvigorw.js'],
  ['missing shell sibling','hvigor/bin/hvigorw'], ['missing engine manifest','hvigor/hvigor/package.json'],
  ['missing plugin manifest','hvigor/hvigor-ohos-plugin/package.json'],
]) test(`${label} fails before wrapper launch`,t=>{
  const f=fixture(t);fs.unlinkSync(path.join(f.toolchain,relative));
  expectFailure(f,f.run());assert.equal(fs.existsSync(f.invocationPath),false);
});
test('real pnpm root export accepts its manifest without forbidden package.json subpath resolution',t=>{
  const f=fixture(t);
  writeJSON(path.join(f.toolsDir,'node_modules/pnpm/package.json'),
    {name:'pnpm',version:'10.28.2',main:'index.cjs',exports:{'.':'./package.json'}});
  assert.equal(f.run().status,0);
  f.restored();
});
for(const [label,relative] of [['launcher','node_modules/.bin/pnpm'],['entry','node_modules/pnpm/index.cjs'],['manifest','node_modules/pnpm/package.json']]) {
  test(`missing prepared pnpm ${label} fails without initializing or invoking wrapper`,t=>{
    const f=fixture(t);const filename=path.join(f.toolsDir,relative);fs.unlinkSync(filename);
    const response=f.run();expectFailure(f,response);
    assert.equal(fs.existsSync(filename),false);assert.equal(fs.existsSync(f.invocationPath),false);
    assert.equal(fs.existsSync(path.join(f.home,'project_caches')),false);
  });
}
test('prepared pnpm version must match the actual wrapper assignment',t=>{
  const f=fixture(t);writeJSON(path.join(f.toolsDir,'node_modules/pnpm/package.json'),{name:'pnpm',version:'10.27.0',main:'index.cjs'});
  const response=f.run();expectFailure(f,response);assert.match(response.stderr,/PNPM_VERSION/);
});
for(const [label,source] of [['missing','const e={};'],['conflicting','const e={}; e.PNPM_VERSION="10.28.2"; e.PNPM_VERSION="10.29.0";']]) {
  test(`${label} wrapper PNPM_VERSION assignment fails closed`,t=>{
    const f=fixture(t);fs.writeFileSync(f.entry,source);expectFailure(f,f.run());assert.equal(fs.existsSync(f.invocationPath),false);
  });
}
test('wrapper cache cannot fall back to an ancestor pnpm package',t=>{
  const f=fixture(t);
  fs.renameSync(path.join(f.toolsDir,'node_modules/pnpm'),path.join(f.home,'pnpm-ancestor'));
  fs.mkdirSync(path.join(f.home,'node_modules'),{recursive:true});
  fs.renameSync(path.join(f.home,'pnpm-ancestor'),path.join(f.home,'node_modules/pnpm'));
  const response=f.run();expectFailure(f,response);assert.match(response.stderr,/inside the selected/);
});
for(const [label,doc] of [['wrong name',{name:'@oheco/hvigor',version:'7.26.8'}],['malformed version',{name:'@ohos/hvigor',version:'7.26'}]]) {
  test(`toolchain metadata rejects ${label}`,t=>{
    const f=fixture(t);writeJSON(path.join(f.toolchain,'hvigor/hvigor/package.json'),doc);
    expectFailure(f,f.run());assert.equal(fs.existsSync(f.invocationPath),false);
  });
}
test('corrupt installed metadata emits a generic diagnostic',t=>{
  const f=fixture(t);fs.writeFileSync(path.join(f.toolchain,'hvigor/hvigor/package.json'),'{"private junk');
  const response=f.run();expectFailure(f,response);assert.equal(response.stderr.includes('private junk'),false);
});
for(const nodeVersion of ['24.14.0','22.21.0','24.14.1-beta.1']) test(`selected bundled Node ${nodeVersion} is rejected`,t=>{
  const f=fixture(t);fs.unlinkSync(f.selectedNode);
  // A Node executable fixture changes only the private version probe response.
  fs.writeFileSync(f.selectedNode,`#!${process.execPath}\nconst fs=require('node:fs'),path=require('node:path');const input=JSON.parse(fs.readFileSync(0,'utf8'));fs.writeSync(3,JSON.stringify({version:${JSON.stringify(nodeVersion)},pnpm:path.join(input.toolsDir,'node_modules/pnpm/index.cjs')}));\n`,{mode:0o700});
  expectFailure(f,f.run());assert.equal(fs.existsSync(f.invocationPath),false);
});
test('outer spawn selects the bundled Node path and passes -- before the official wrapper',t=>{
  const f=fixture(t);fs.unlinkSync(f.selectedNode);
  const trace=path.join(f.work,'selected-node-argv.json');
  fs.writeFileSync(f.selectedNode,`#!${process.execPath}\nconst fs=require('node:fs'),cp=require('node:child_process');const args=process.argv.slice(2);if(args[0]==='--')fs.writeFileSync(${JSON.stringify(trace)},JSON.stringify(args));const response=cp.spawnSync(${JSON.stringify(process.execPath)},args,{stdio:args[0]==='--eval'?[0,1,2,3]:[0,1,2]});process.exitCode=response.status??1;\n`,{mode:0o700});
  const response=f.run({}, {NODE_OPTIONS:'--no-warnings'});assert.equal(response.status,0,response.stderr);
  assert.deepEqual(JSON.parse(fs.readFileSync(trace)),['--',f.entry,...buildArguments(f.request)]);f.restored();
});
test('persistent home selection respects request, inherited env, and native OHECO_ROOT fallback',t=>{
  const {hvigorUserHome,DEFAULT_OHECO_ROOT}=require(path.join(path.dirname(RUNNER),'toolchain-config.cjs'));
  assert.equal(hvigorUserHome({},{}),path.join(path.dirname(DEFAULT_OHECO_ROOT),'.hvigor'));
  assert.equal(hvigorUserHome({},{OHECO_ROOT:'/fixture/oheco'}),'/fixture/.hvigor');
  assert.equal(hvigorUserHome({},{HVIGOR_USER_HOME:'/fixture/inherited',OHECO_ROOT:'/ignored/root'}),'/fixture/inherited');
  assert.equal(hvigorUserHome({hvigorUserHome:'/fixture/request'},{HVIGOR_USER_HOME:'/ignored/inherited'}),'/fixture/request');
  const f=fixture(t);assert.equal(f.run({hvigorUserHome:undefined},{HVIGOR_USER_HOME:f.home}).status,0);
  assert.equal(JSON.parse(fs.readFileSync(f.invocationPath)).env.HVIGOR_USER_HOME,f.home);f.restored();
  const selected=path.join(f.root,'.hvigor');fs.renameSync(f.home,selected);
  assert.equal(f.run({hvigorUserHome:undefined},{HVIGOR_USER_HOME:undefined,OHECO_ROOT:path.join(f.root,'.oheco')}).status,0);
  assert.equal(JSON.parse(fs.readFileSync(f.invocationPath)).env.HVIGOR_USER_HOME,selected);f.restored();
});
for(const probeFailure of ['throw','exit','response']) test(`decrypt helper ${probeFailure} fails privately before profile mutation`,t=>{
  const f=fixture(t,{signed:true,probe:{probeFailure}});const response=f.run();expectFailure(f,response);
  const output=response.stdout+response.stderr;
  for(const secret of [...f.plaintext,f.request.signing.keyPasswordEncrypted,f.request.signing.storePasswordEncrypted]) assert.equal(output.includes(secret),false);
  assert.match(output,/Official signing password preflight failed/);assert.equal(fs.existsSync(f.invocationPath),false);
});
test('successful noisy public decrypt probe never forwards helper stdout or stderr',t=>{
  const f=fixture(t,{signed:true,probe:{probeNoise:true}});const response=f.run();assert.equal(response.status,0,response.stderr);
  for(const secret of [...f.plaintext,f.request.signing.keyPasswordEncrypted,f.request.signing.storePasswordEncrypted]) assert.equal((response.stdout+response.stderr).includes(secret),false);
  f.restored();
});
for(const exported of ['module.exports={};','exports.DecipherUtil={getKey(){throw new Error("private method must not run")}};']) {
  test('only documented DecipherUtil.decryptPwd is accepted',t=>{
    const f=fixture(t,{signed:true});fs.writeFileSync(f.decipher,exported);
    expectFailure(f,f.run());assert.equal(fs.existsSync(f.invocationPath),false);
  });
}
for(const [label,token] of [['env reference','env:OLD_SECRET'],['plaintext','plain password'],['odd hex','abc'],
  ['nonhex','x'.repeat(100)],['short frame','ab'.repeat(16)],['invalid content length','00'.repeat(40)],
  ['short auth tag','00000008'+'ab'.repeat(20)]]) {
  test(`signing rejects ${label} representation before public decrypt`,t=>{
    const f=fixture(t,{signed:true});const response=f.run({signing:{...f.request.signing,keyPasswordEncrypted:token}});
    expectFailure(f,response);assert.equal(fs.existsSync(f.invocationPath),false);
    assert.equal(fs.existsSync(path.join(path.dirname(f.request.signing.storeFile),'decrypt-calls.json')),false);
  });
}
for(const name of ['fd','ac','ce']) test(`missing signing companion ${name} directory fails before decrypt`,t=>{
  const f=fixture(t,{signed:true});const target=path.join(path.dirname(f.request.signing.storeFile),'material',name);
  fs.unlinkSync(path.join(target,'component'));fs.rmdirSync(target);
  expectFailure(f,f.run());assert.equal(fs.existsSync(f.invocationPath),false);
});
for(const [label,relative,doc] of [
  ['root registry','oh-package.json5',{dependencies:{remote:'1.0.0'}}],
  ['missing local entry','entry/oh-package.json5',{dependencies:{missing:'file:./missing'}}],
  ['dynamic deps','oh-package.json5',{dependencies:{},dynamicDependencies:{}}],
  ['package scripts','oh-package.json5',{dependencies:{},scripts:{build:'untrusted'}}],
  ['Hvigor local plugin','hvigor/hvigor-config.json5',{dependencies:{extra:'file:./extra'}}],
  ['parameterFile','hvigor/hvigor-config.json5',{dependencies:{},parameterFile:'./params.json'}],
  ['nodeOptions','hvigor/hvigor-config.json5',{dependencies:{},nodeOptions:{exposeGC:true}}],
  ['custom properties','hvigor/hvigor-config.json5',{dependencies:{},properties:{'hvigor.dependency.useNpm':true}}],
  ['build cache dir','hvigor/hvigor-config.json5',{dependencies:{},buildCacheDir:'./other'}],
]) test(`static project contract rejects ${label}`,t=>{
  const f=fixture(t);writeJSON(path.join(f.project,relative),doc);
  expectFailure(f,f.run());assert.equal(fs.existsSync(f.invocationPath),false);
});
for(const relative of ['hvigorfile.ts','entry/hvigorfile.ts']) test(`dynamic tasks in ${relative} fail before wrapper`,t=>{
  const f=fixture(t);fs.appendFileSync(path.join(f.project,relative),'\nprocess.exit(0);');
  expectFailure(f,f.run());assert.equal(fs.existsSync(f.invocationPath),false);
});
test('persistent Hvigor custom parameter file fails without modifying cache',t=>{
  const f=fixture(t);const filename=path.join(f.home,'hvigor-config.json5');writeJSON(filename,{parameterFile:'./untrusted.json'});
  const original=fs.readFileSync(filename);expectFailure(f,f.run());assert.deepEqual(fs.readFileSync(filename),original);
  assert.equal(fs.existsSync(f.invocationPath),false);
});
test('nonempty stale workspace dependencies fail before wrapper installer path',t=>{
  const f=fixture(t),{createHash}=require('node:crypto');
  const hash=createHash('sha256').update(f.project).digest('hex').slice(0,32);
  const filename=path.join(f.home,'project_caches',hash,'workspace/package.json');writeJSON(filename,{dependencies:{previous:'1.0.0'}});
  const original=fs.readFileSync(filename);expectFailure(f,f.run());assert.deepEqual(fs.readFileSync(filename),original);
  assert.equal(fs.existsSync(f.invocationPath),false);
});
test('signal during final profile restoration cancels success without leaving handlers or locks',t=>{
  const f=fixture(t,{signed:true});writeJSON(f.requestPath,f.request);
  const bootstrap=path.join(f.root,'restoration-signal.cjs');
  fs.writeFileSync(bootstrap,`const fs=require('node:fs');const rename=fs.renameSync;fs.renameSync=function(from,to){const result=rename.apply(this,arguments);if(from.endsWith('.godot-build-runner-original.json5'))process.emit('SIGTERM');return result;};require(${JSON.stringify(RUNNER)}).run(${JSON.stringify(f.requestPath)}).then(()=>{process.exitCode=0},()=>{if(process.listenerCount('SIGTERM')!==0||process.listenerCount('SIGINT')!==0)throw new Error('leaked signal handlers');process.exitCode=1;});\n`);
  const response=spawnSync(process.execPath,[bootstrap],{env:process.env,encoding:'utf8',timeout:20000});
  expectFailure(f,response);assert.match(response.stderr,/interrupted/);
  assert.equal(fs.existsSync(path.join(f.project,'.godot-build-runner.lock')),false);
  assert.equal(fs.existsSync(path.join(f.work,'.runner-lock')),false);
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
