'use strict';
// Runs the actual helper through execFile-compatible local executables. No real
// auth, cloud signing, SDK build, installation, or network requests are made.
// GODOT_NATIVE_AUTO_SIGNER selects the signed production C++ executable.
// Official tools retain the caller environment and selected original CLT.
// GODOT_NATIVE_NODE_RUNTIME selects a real signed CLT libc++_shared.so copied as
// fixture data; the installed CLI, HDC and SDK launchers are never executed.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync, spawn } = require('node:child_process');
const NATIVE = process.env.GODOT_NATIVE_AUTO_SIGNER || '';
assert.ok(path.isAbsolute(NATIVE),'set GODOT_NATIVE_AUTO_SIGNER to the signed production native worker');
const NODE_RUNTIME = process.env.GODOT_NATIVE_NODE_RUNTIME || '/storage/Users/currentUser/.oheco/packages/command-line-tools/26.0.0.201/sdk/default/openharmony/native/llvm/lib/aarch64-linux-ohos/libc++_shared.so';
function invocation(args) { return {file:NATIVE,args}; }
function receipt(child, expectedExit = 0) {
  assert.equal(child.error,undefined,child.error&&child.error.message);
  assert.equal(child.status,expectedExit,child.stderr+'\n'+child.stdout);assert.equal(child.signal,null);assert.equal(child.stderr,'');
  assert.ok(!child.stdout.includes(SECRET),'raw child diagnostics escaped');
  assert.match(child.stdout,/^\{[^\r\n]*\}\n?$/,'stdout must contain exactly one complete JSON line');
  const result=JSON.parse(child.stdout);assert.equal(result.schemaVersion,1);
  assert.ok(['ok','error'].includes(result.status),'only automatic success/error receipts are supported');
  assert.equal(Object.hasOwn(result,'confirmationToken'),false);
  if(result.status==='error'){assert.equal(typeof result.code,'string');assert.ok(result.code.length);assert.equal(typeof result.message,'string');}
  else{assert.equal(result.signed,true);assert.equal(result.profileValidated,true);assert.equal(result.format,'hap');assert.equal(result.mode,'debug');}
  return result;
}
const UDID_A = 'a'.repeat(64), UDID_B = 'b'.repeat(64);
const SECRET = 'SENTINEL_secret_token_and_password_never_print';
// Public SDK sample certificate: local fixture data, valid through 2049. Nothing
// in these tests is capable of signing an actual package.
const CERT = `-----BEGIN CERTIFICATE-----
MIICMzCCAbegAwIBAgIEaOC/zDAMBggqhkjOPQQDAwUAMGMxCzAJBgNVBAYTAkNO
MRQwEgYDVQQKEwtPcGVuSGFybW9ueTEZMBcGA1UECxMQT3Blbkhhcm1vbnkgVGVh
bTEjMCEGA1UEAxMaT3Blbkhhcm1vbnkgQXBwbGljYXRpb24gQ0EwHhcNMjEwMjAy
MTIxOTMxWhcNNDkxMjMxMTIxOTMxWjBoMQswCQYDVQQGEwJDTjEUMBIGA1UEChML
T3Blbkhhcm1vbnkxGTAXBgNVBAsTEE9wZW5IYXJtb255IFRlYW0xKDAmBgNVBAMT
H09wZW5IYXJtb255IEFwcGxpY2F0aW9uIFJlbGVhc2UwWTATBgcqhkjOPQIBBggq
hkjOPQMBBwNCAATbYOCQQpW5fdkYHN45v0X3AHax12jPBdEDosFRIZ1eXmxOYzSG
JwMfsHhUU90E8lI0TXYZnNmgM1sovubeQqATo1IwUDAfBgNVHSMEGDAWgBTbhrci
FtULoUu33SV7ufEFfaItRzAOBgNVHQ8BAf8EBAMCB4AwHQYDVR0OBBYEFPtxruhl
cRBQsJdwcZqLu9oNUVgaMAwGCCqGSM49BAMDBQADaAAwZQIxAJta0PQ2p4DIu/ps
LMdLCDgQ5UH1l0B4PGhBlMgdi2zf8nk9spazEQI/0XNwpft8QAIwHSuA2WelVi/o
zAlF08DnbJrOOtOnQq5wHOPlDYB4OtUzOYJk9scotrEnJxJzGsh/
-----END CERTIFICATE-----
`;
const COMMON = String.raw`
const fs=require('node:fs'),path=require('node:path');
const control=JSON.parse(fs.readFileSync(process.env.FIXTURE_CONTROL));
const root=path.dirname(process.env.FIXTURE_CONTROL);
function log(kind,details={}){fs.appendFileSync(path.join(root,'events.jsonl'),JSON.stringify({kind,...details})+'\n');}
function write(file,data){fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,data);}
function json(file,data){write(file,JSON.stringify(data));}
function auditToolEnvironment(phase){
  if(!control.normalEnvironmentProbe)return;
  const assert=require('node:assert/strict');
  assert.equal(process.env.HOME,process.env.FIXTURE_ORIGINAL_HOME,'official tools must retain caller HOME');
  assert.equal(process.env.XDG_CONFIG_HOME,process.env.FIXTURE_ORIGINAL_CONFIG,'official tools must retain caller XDG config');
  assert.equal(process.umask(),Number(process.env.FIXTURE_CALLER_UMASK),'official tools must inherit caller umask');
  if(phase==='cli'){
    assert.equal(process.env.COMMAND_LINE_TOOL_PATH,process.env.FIXTURE_ORIGINAL_CLT,'selected original CLT must be used');
  }
  log('normal-tool-environment',{phase,home:process.env.HOME,config:process.env.XDG_CONFIG_HOME,umask:process.umask()});
}
function auditRuntime(phase,nodeChild){
  if(!control.runtimeIsolation)return;
  const assert=require('node:assert/strict'),ld=process.env.LD_LIBRARY_PATH||'',original=process.env.FIXTURE_ORIGINAL_LD_LIBRARY_PATH||'';
  const isolated=!!process.env.FIXTURE_NATIVE_WORKER&&nodeChild&&!control.runtimeAbsent;
  let runtime=null,target=null;
  if(isolated){
    const suffix=original?':'+original:'';
    assert.ok(ld.endsWith(suffix));runtime=ld.slice(0,ld.length-suffix.length);
    assert.equal(ld,runtime+suffix);assert.equal(path.basename(runtime),'node-runtime');
    assert.equal(path.dirname(path.dirname(runtime)),path.join(root,'private tmp'));
    assert.match(path.basename(path.dirname(runtime)),/^godot-auto-sign-[^/]+$/);
    const dir=fs.lstatSync(runtime);assert.equal(dir.isDirectory(),true);assert.equal(dir.isSymbolicLink(),false);assert.equal(fs.realpathSync(runtime),runtime);
    assert.deepEqual(fs.readdirSync(runtime),['libc++_shared.so']);
    const library=path.join(runtime,'libc++_shared.so'),link=fs.lstatSync(library);
    target=fs.readlinkSync(library);assert.equal(link.isSymbolicLink(),true);assert.equal(target,fs.realpathSync(process.env.FIXTURE_SELECTED_NODE_RUNTIME));
    assert.ok(fs.readFileSync(library).equals(fs.readFileSync(process.env.FIXTURE_SELECTED_NODE_RUNTIME)));
    assert.equal(fs.existsSync(path.join(runtime,'libc++.so')),false);
  }else assert.equal(ld,original,'a non-Node process or absent runtime must retain the broker loader environment');
  for(const entry of ld.split(':').filter(Boolean)){
    assert.ok(!entry.startsWith(process.env.FIXTURE_ORIGINAL_CLT+path.sep),'compiler library directory leaked to a descendant');
    assert.ok(!entry.startsWith(path.join(root,'unselected SDK')+path.sep),'unselected SDK leaked to a descendant');
  }
  log('runtime-environment',{phase,ld,original,isolated,runtime,target});
}
console.log('SENTINEL_secret_token_and_password_never_print');console.error('SENTINEL_secret_token_and_password_never_print');
`;
const CLI = COMMON + String.raw`
(async()=>{
auditRuntime('cli',true);auditToolEnvironment('cli');
if(control.runtimeIsolation){
  const child=require('node:child_process').spawnSync(process.execPath,[path.join(root,'sdk-child.cjs')],{encoding:'utf8'});
  if(child.error||child.status!==0||!child.stdout.includes('SDK_CHILD_OK'))throw new Error('SDK child inherited an unsafe loader environment');
}
const args=process.argv.slice(2),loginData=process.env.DEVECO_CLI_DATA_DIR||path.join(process.env.HOME,'.config/deveco-cli');
if(control.normalEnvironmentProbe)require('node:assert/strict').equal(loginData,process.env.FIXTURE_EXPECTED_LOGIN_DATA,'official CLI must read the selected existing login directory');
log('cli',{args,home:process.env.HOME,data:loginData,config:process.env.XDG_CONFIG_HOME,cwd:process.cwd(),clt:process.env.COMMAND_LINE_TOOL_PATH,umask:process.umask(),authSource:process.env.DEVECO_CLI_AUTH_SOURCE,codeAuthDir:process.env.DEVECO_CODE_AUTH_DIR,nodeOptions:process.env.NODE_OPTIONS,nodePath:process.env.NODE_PATH,nodeOpts:process.env.NODE_OPTS,execArgv:process.execArgv});
async function probeChildren(){
  const cp=require('node:child_process'),bin=path.join(process.env.COMMAND_LINE_TOOL_PATH,'sdk/default/openharmony/toolchains/hdc');
  const childArgs=['-t','serial-is-not-udid','shell','bm','get','-u'];
  const env=Object.freeze({...process.env,HDC_SERVER:'cli-override-must-not-replace-host',OHOS_HDC_SERVER_PORT:'1'}),options=Object.freeze({encoding:'utf8',env});
  const collect=child=>new Promise((resolve,reject)=>{let stdout='';child.stdout.on('data',bytes=>stdout+=bytes);child.on('error',reject);child.on('close',code=>code===0?resolve(stdout):reject(new Error('fake HDC spawn failed')));});
  for(const mode of ['spawn','spawnSync','execFile','execFileSync','promisify','esm']){
    let stdout;
    if(mode==='spawn')stdout=await collect(cp.spawn(bin,childArgs,options));
    if(mode==='spawnSync'){const result=cp.spawnSync(bin,childArgs,options);if(result.status!==0)throw new Error('fake HDC spawnSync failed');stdout=result.stdout;}
    if(mode==='execFile')stdout=await new Promise((resolve,reject)=>cp.execFile(bin,childArgs,(error,out)=>error?reject(error):resolve(out)));
    if(mode==='execFileSync')stdout=cp.execFileSync(bin,childArgs,options);
    if(mode==='promisify')stdout=(await require('node:util').promisify(cp.execFile)(bin,childArgs,options)).stdout;
    if(mode==='esm')stdout=(await import('node:child_process')).execFileSync(bin,childArgs,options);
    if(!String(stdout).includes('a'.repeat(64)))throw new Error('CLI HDC did not return the actual UDID');
    log('cli-hdc-probe',{mode});
  }
  if(!cp.execFileSync(bin,['list','targets'],options).includes('serial-is-not-udid'))throw new Error('CLI HDC target list failed');
  const other=cp.execFileSync(path.join(root,'unrelated tools/hdc'),[],options);
  if(!other.includes('OTHER_CHILD_OK'))throw new Error('non-HDC child changed');
  if(env.HOME!==process.env.HOME||env.HDC_SERVER!=='cli-override-must-not-replace-host')throw new Error('caller environment was mutated');
}
if(args.join(' ')==='auth status'){
  const calls=fs.readFileSync(path.join(root,'events.jsonl'),'utf8').trim().split('\n').map(line=>JSON.parse(line)).filter(event=>event.kind==='cli'&&event.args.join(' ')==='auth status').length;
  if(calls===2){
    if(control.permissionsOnRecheck){const file=path.join(process.cwd(),'entry/src/main/module.json5');const doc=JSON.parse(fs.readFileSync(file));doc.module.requestPermissions=control.permissionsOnRecheck.map(name=>({name}));json(file,doc);}
    if(control.bundleOnRecheck){const file=path.join(process.cwd(),'AppScope/app.json5');const doc=JSON.parse(fs.readFileSync(file));doc.app.bundleName=control.bundleOnRecheck;json(file,doc);}
    if(control.otherCLIStateOnRecheck)write(path.join(process.env.HOME,'.ohos/config/other-project-state'),'unrelated application state');
  }
  if(control.notLoggedIn){console.log('Not logged in');process.exit(0);}
  const data=loginData;
  if(!fs.existsSync(path.join(data,'token.enc'))||!fs.existsSync(path.join(data,'token.dek'))){console.log('Not logged in');process.exit(0);}
  const token=fs.readFileSync(path.join(data,'token.enc'),'utf8'),dek=fs.readFileSync(path.join(data,'token.dek'),'utf8');
  if(!token.endsWith('original-token')||!dek.endsWith('original-dek'))throw new Error('existing CLI login data changed');
  for(const version of [1,2,3])if(fs.readFileSync(path.join(process.env.HOME,'.local/share/deveco-cli/keys','kek-v'+version+'.bin'),'utf8')!=='original-kek-'+version)throw new Error('CLI did not read original KEK data');
  // Fake CLI only reads fixture login data. Worker-issued copies or rewrites
  // would change the observed path/content and fail the environment regression.
  console.log('Current user: local-fixture');
  if(process.env.FIXTURE_NATIVE_WORKER&&control.runtimeTamper){
    const runtime=(process.env.LD_LIBRARY_PATH||'').split(':')[0],library=path.join(runtime,'libc++_shared.so');
    if(path.dirname(path.dirname(runtime))!==path.join(root,'private tmp'))throw new Error('unsafe fixture mutation target');
    if(control.runtimeTamper==='wrong-target'){fs.unlinkSync(library);fs.symlinkSync(process.env.FIXTURE_UNSELECTED_NODE_RUNTIME,library);}
    else if(control.runtimeTamper==='extra-library')write(path.join(runtime,'libc++.so'),'POISON compiler linker script');
    else if(control.runtimeTamper==='symlink-dir'){fs.unlinkSync(library);fs.rmdirSync(runtime);fs.symlinkSync(path.dirname(process.env.FIXTURE_SELECTED_NODE_RUNTIME),runtime);}
    else throw new Error('unknown runtime mutation');
    log('runtime-tamper',{mode:control.runtimeTamper});
  }
}else if(args.join(' ')==='auth team list'){
  const calls=fs.readFileSync(path.join(root,'events.jsonl'),'utf8').trim().split('\n').map(line=>JSON.parse(line)).filter(event=>event.kind==='cli'&&event.args.join(' ')==='auth team list').length;
  const teams=(calls>=2&&control.teamsOnRecheck)||control.teams||['team-one'],width=Math.max(2,...teams.map(id=>id.length));
  console.log('Id'.padEnd(width)+'  Name');console.log('-'.repeat(width)+'  ----');
  for(const id of teams)console.log(id.padEnd(width)+'  Local Team Name With Spaces');
}else if(args[0]==='signature'&&args[1]==='generate'){
  // Official CLI 1.3.5 buildOpenHarmonyProvider calls this before any HDC or
  // generation. Its OpenHarmony emulatorPath/javaPath are empty (no guard).
  const clt=process.env.COMMAND_LINE_TOOL_PATH,toolchainRoot=fs.realpathSync(clt);
  const tools={node:'node/bin/node',ohpm:'ohpm/bin/pm-cli.js',hvigor:'hvigor/bin/hvigorw.js',sdk:'sdk',hdc:'sdk/default/openharmony/toolchains/hdc'};
  const checked=[];
  for(const [name,relative] of Object.entries(tools)){
    const file=path.join(clt,relative),resolved=fs.realpathSync(file),within=path.relative(toolchainRoot,resolved);
    if(path.isAbsolute(within)||within==='..'||within.startsWith('..'+path.sep))throw new Error('Unsafe toolchain path: '+name+' resolves outside the toolchain root');
    const stat=fs.lstatSync(file),original=path.join(process.env.FIXTURE_ORIGINAL_CLT,relative);
    checked.push({name,path:file,resolved,isFile:stat.isFile(),isDirectory:stat.isDirectory(),isSymlink:stat.isSymbolicLink(),links:stat.nlink,owner:stat.uid,mode:stat.mode&0o7777,...(['node','ohpm','hvigor'].includes(name)?{sameBytes:fs.readFileSync(file).equals(fs.readFileSync(original)),distinctOriginal:resolved!==fs.realpathSync(original)}:{})});
  }
  log('toolchain-containment',{root:toolchainRoot,checked});
  if(control.cliLauncherProbe){
    const cp=require('node:child_process');
    for(const name of ['ohpm','hvigor']){
      const result=cp.spawnSync(path.join(clt,tools.node),[path.join(clt,tools[name])],{encoding:'utf8'});
      if(result.error||result.status!==0||result.stdout.trim()!=='FAKE_'+name.toUpperCase()+'_OK')throw new Error('selected launcher dependency resolution failed: '+name);
      log('cli-launcher-probe',{name});
    }
    const links=['node/lib','ohpm/lib','ohpm/node_modules','hvigor/engine','hvigor/plugins','hvigor/node_modules'];
    log('toolchain-dependency-links',{links:links.map(relative=>({relative,isSymlink:fs.lstatSync(path.join(clt,relative)).isSymbolicLink(),resolved:fs.realpathSync(path.join(clt,relative))})),binSiblings:['ohpm/bin/launcher.cjs','hvigor/bin/launcher.cjs'].map(relative=>{const file=path.join(clt,relative),original=path.join(process.env.FIXTURE_ORIGINAL_CLT,relative);return{relative,isFile:fs.lstatSync(file).isFile(),isSymlink:fs.lstatSync(file).isSymbolicLink(),sameBytes:fs.readFileSync(file).equals(fs.readFileSync(original))};})});
  }
  if(control.cliHdcProbe)await probeChildren();
  log('generate',{locks:fs.readdirSync(path.join(process.env.FIXTURE_SIGNING_ROOT,'locks')).sort()});
  if(!args.includes('--team-id')||args.includes('--force'))throw new Error('invalid generate arguments');
  const materialDir=path.join(process.env.HOME,'.ohos/config');
  const app=JSON.parse(fs.readFileSync(path.join(process.cwd(),'AppScope/app.json5'))).app;
  const mod=JSON.parse(fs.readFileSync(path.join(process.cwd(),'entry/src/main/module.json5'))).module;
  const now=Math.floor(Date.now()/1000);
  const profile={type:'debug','bundle-info':{'bundle-name':control.wrongBundle?'org.wrong.app':app.bundleName,'development-certificate':control.cert},
    validity:{'not-before':now-60,'not-after':control.expired?now-1:now+86400},
    'debug-info':{'device-id-type':'udid','device-ids':control.generatedDevices||['a'.repeat(64)]},
    acls:{'allowed-acls':control.generatedACL||mod.requestPermissions.map(p=>p.name)}};
  if(control.omitProfileACL)delete profile.acls;
  else if(Object.hasOwn(control,'profileACLPolicy'))profile.acls=control.profileACLPolicy;
  write(path.join(materialDir,'cert.cer'),control.certBundle||control.cert);json(path.join(materialDir,'profile.p7b'),profile);write(path.join(materialDir,'store.p12'),'old-key-material');
  for(const name of ['fd','ac','ce'])write(path.join(materialDir,'material',name,'component'),name+' local component');
  if(control.generateFail){
    write(path.join(materialDir,'store.p12'),'damaged-key');
    if(control.blockCacheRollback){
      const projects=path.join(process.env.FIXTURE_SIGNING_ROOT,'projects'),outside=path.join(root,'unrelated-cache-root');
      fs.mkdirSync(outside,{mode:0o700});fs.renameSync(projects,path.join(root,'projects-before-rollback'));fs.symlinkSync(outside,projects);
    }
    console.error(control.network?'ECONNRESET network proxy':'generation failed');process.exit(9);
  }
  const frame=Buffer.alloc(64,0xab);frame.writeUInt32BE(48);
  const config={name:'fixture-signing',type:'HarmonyOS',material:{certpath:path.join(materialDir,'cert.cer'),profile:path.join(materialDir,'profile.p7b'),storeFile:path.join(materialDir,'store.p12'),keyAlias:'fixture-private-alias',signAlg:'SHA256withECDSA',keyPassword:frame.toString('hex'),storePassword:frame.toString('hex')}};
  const file=path.join(process.cwd(),'build-profile.json5'),doc=JSON.parse(fs.readFileSync(file));doc.app.signingConfigs=[config];doc.app.products.find(p=>p.name==='default').signingConfig=config.name;json(file,control.malformedGenerated?{app:{products:{},signingConfigs:{}}}:doc);
}else throw new Error('unexpected command: '+args.join(' '));
})().catch(error=>{console.error(error.message);process.exitCode=12;});
`;
const HDC = COMMON + String.raw`
const args=process.argv.slice(2),runtime=(process.env.LD_LIBRARY_PATH||'').split(':')[0];auditRuntime('hdc',path.basename(runtime)==='node-runtime');auditToolEnvironment('hdc');log('hdc',{args,file:process.argv[1],home:process.env.HOME,server:process.env.HDC_SERVER,port:process.env.OHOS_HDC_SERVER_PORT,umask:process.umask()});
if(process.env.HOME!==process.env.FIXTURE_ORIGINAL_HOME)throw new Error('caller HDC HOME changed');
if(fs.readFileSync(path.join(process.env.HOME,'.harmony/hdckey'),'utf8')!=='original-hdc-private'||fs.readFileSync(path.join(process.env.HOME,'.harmony/hdckey.pub'),'utf8')!=='original-hdc-public')throw new Error('original HDC identity changed');
if(args.join(' ')==='list targets'){console.log('serial-is-not-udid');process.exit(0);}
if(control.unauthorized){console.error('device unauthorized');process.exit(0);}
if(control.deviceFail){console.error('device disconnected');process.exit(3);}
if(args.slice(2).join(' ')!=='shell bm get -u')throw new Error('unexpected hdc command');
let udid=(control.udids||{})[args[1]]||'a'.repeat(64);
if(control.changeUDIDBeforeGeneration){const calls=fs.readFileSync(path.join(root,'events.jsonl'),'utf8').trim().split('\n').map(line=>JSON.parse(line)).filter(event=>event.kind==='hdc').length;if(calls>=2)udid='b'.repeat(64);}
console.log('udid: '+udid);
`;
const VERIFIER = COMMON + String.raw`
const args=process.argv.slice(2);auditRuntime('verify',false);auditToolEnvironment('verify');log('verify',{args});
if(control.verifyFailOnce){json(process.env.FIXTURE_CONTROL,{...control,verifyFailOnce:false});process.exit(4);}
if(control.verifyFail){process.exit(4);}
const input=args[args.indexOf('-inFile')+1],output=args[args.indexOf('-outFile')+1];
const doc=JSON.parse(fs.readFileSync(input));
if(control.invalidVerifiedJSON)write(output,'not JSON');else json(output,doc);
`;
const BUILD = COMMON + String.raw`
auditRuntime('build',true);auditToolEnvironment('build');
const args=process.argv.slice(2),file=args[1],request=JSON.parse(fs.readFileSync(file));
const stat=fs.statSync(file),parent=fs.statSync(path.dirname(file));
log('build',{args,requestMode:stat.mode&0o777,parentMode:parent.mode&0o777,request,requestFile:file});
if(args[0]!=='--request'||request.schemaVersion!==2)throw new Error('invalid build contract');
if(control.normalEnvironmentProbe){
  const assert=require('node:assert/strict'),home=request.hvigorUserHome||process.env.HVIGOR_USER_HOME;
  assert.equal(home,process.env.HVIGOR_USER_HOME,'ordinary Hvigor home must not be replaced');
  const workspace=path.join(home,'project_caches/fixture/workspace/node_modules/@ohos'),file=path.join(workspace,'created-with-caller-umask');
  fs.mkdirSync(workspace,{recursive:true});fs.writeFileSync(file,'ordinary SDK cache');
  log('normal-hvigor-cache',{home,umask:process.umask(),directoryMode:fs.statSync(workspace).mode&0o777,fileMode:fs.statSync(file).mode&0o777});
}
const profile=path.join(request.projectDir,'build-profile.json5'),original=fs.readFileSync(profile);
try{
  write(profile,'temporary signing bytes: '+request.signing.keyPasswordEncrypted);
  if(control.buildFail)process.exitCode=7;
  else{
    const artifact=control.outsideArtifact?path.join(root,'outside-signed.hap'):path.join(request.projectDir,'entry/build/default/outputs/default/demo-signed.hap');
    write(artifact,'SYNTHETIC SIGNED HAP');
    json(path.join(request.workDir,'result.json'),{schemaVersion:2,signed:!control.unsigned,format:control.wrongFormat?'app':'hap',mode:control.wrongMode?'release':'debug',outputPath:artifact});
  }
}finally{if(!control.noRestore)fs.writeFileSync(profile,original);}
if(control.holdBuild){
  {
    // The grandchild closes its stdio and ignores SIGTERM. The worker must wait
    // for group termination even after the direct build process exits.
    const marker=path.join(root,'grandchild-ready');
    const code="const fs=require('node:fs');process.on('SIGTERM',()=>{});fs.writeFileSync(process.argv[1],String(process.pid));setInterval(()=>{},1000);";
    const child=require('node:child_process').spawn(process.execPath,['-e',code,marker],{stdio:'ignore'});
    child.on('error',()=>process.exit(12));
    process.on('SIGTERM',()=>{log('build-exiting');process.exit(0);});
    const ready=setInterval(()=>{if(fs.existsSync(marker)){clearInterval(ready);log('build-held',{grandchild:Number(fs.readFileSync(marker,'utf8'))});}},10);
  }
  setInterval(()=>{},1000);
}
`;
function write(file, content, mode = 0o600) {
  fs.mkdirSync(path.dirname(file), {recursive:true,mode:0o700});
  fs.writeFileSync(file, content, {mode});
}
function json(file, value) { write(file, JSON.stringify(value)); }
function executable(file, source) { write(file, `#!${process.execPath}\n'use strict';\n${source}`, 0o700); fs.chmodSync(file,0o700); }
function fixture(t, initial = {}) {
  assert.ok(path.isAbsolute(process.env.TMPDIR||''),'fixtures require an explicit absolute TMPDIR');
  const tempRoot=fs.realpathSync(process.env.TMPDIR),root=fs.mkdtempSync(path.join(tempRoot,'godot-auto-sign-test-'));
  t.after(() => { assert.equal(path.dirname(root),tempRoot); assert.ok(path.isAbsolute(root) && path.basename(root).startsWith('godot-auto-sign-test-')); fs.rmSync(root,{recursive:true,force:true}); });
  const home=path.join(root,'original home'),config=path.join(root,'private config'),tmp=path.join(root,'private tmp');
  const project=path.join(root,'export with spaces; $ no shell'),clt=path.join(root,'CLT with spaces'),sdk=path.join(clt,'sdk');
  for(const dir of [home,config,tmp,project,sdk])fs.mkdirSync(dir,{recursive:true,mode:0o700});
  // Every fake CLT has the real provider's explicit paths and runnable launcher
  // dependencies. The fixed-SDK case uses the actual signed Node ELF (>4 MiB).
  const cltNode=path.join(clt,'node/bin/node');
  if(initial.cliLauncherProbe){fs.mkdirSync(path.dirname(cltNode),{recursive:true,mode:0o700});fs.copyFileSync(process.execPath,cltNode);fs.chmodSync(cltNode,0o500);}
  else executable(cltNode,"const result=require('node:child_process').spawnSync(process.execPath,process.argv.slice(2),{stdio:'inherit'});if(result.error)throw result.error;process.exit(result.status===null?1:result.status);");
  write(path.join(clt,'node/lib/fixture-runtime.txt'),'selected CLT runtime');
  write(path.join(clt,'ohpm/bin/pm-cli.js'),"require('./launcher.cjs')();\n");
  write(path.join(clt,'ohpm/bin/launcher.cjs'),"module.exports=()=>{if(require('../lib/index.cjs')!=='ohpm'||require('fixture-dependency')!=='dependency')throw new Error('ohpm dependencies');console.log('FAKE_OHPM_OK');};\n");
  write(path.join(clt,'ohpm/lib/index.cjs'),"module.exports='ohpm';\n");
  write(path.join(clt,'hvigor/bin/hvigorw.js'),"require('./launcher.cjs')();\n");
  write(path.join(clt,'hvigor/bin/launcher.cjs'),"module.exports=()=>{if(require('../engine/index.cjs')!=='hvigor'||require('../plugins/plugin.cjs')!=='plugin'||require('fixture-dependency')!=='dependency')throw new Error('hvigor dependencies');console.log('FAKE_HVIGOR_OK');};\n");
  write(path.join(clt,'hvigor/engine/index.cjs'),"module.exports='hvigor';\n");
  write(path.join(clt,'hvigor/plugins/plugin.cjs'),"module.exports='plugin';\n");
  for(const tool of ['ohpm','hvigor'])write(path.join(clt,tool,'node_modules/fixture-dependency/index.js'),"module.exports='dependency';\n");
  write(path.join(clt,'version.txt'),'Version: 26.0.0\n');
  const runtimeDir=path.join(sdk,'default/openharmony/native/llvm/lib/aarch64-linux-ohos'),runtime=path.join(runtimeDir,'libc++_shared.so'),unselectedRuntime=path.join(root,'unselected SDK/lib/libc++_shared.so');
  if(initial.runtimeIsolation){
    write(path.join(runtimeDir,'libc++.so'),'POISON compiler linker script; never a system runtime');
    write(path.join(runtimeDir,'unrelated-sdk-library.so'),'must not enter the loader search path');
    if(!initial.runtimeAbsent){
      assert.equal(fs.readFileSync(NODE_RUNTIME).subarray(0,4).toString('hex'),'7f454c46','supply a real signed CLT runtime via GODOT_NATIVE_NODE_RUNTIME');
      fs.copyFileSync(NODE_RUNTIME,runtime);fs.chmodSync(runtime,0o500);
      fs.mkdirSync(path.dirname(unselectedRuntime),{recursive:true,mode:0o700});fs.copyFileSync(runtime,unselectedRuntime);fs.chmodSync(unselectedRuntime,0o500);
    }
  }
  const permissionDefinitions={definePermissions:[
    ...['ohos.permission.TEST_ACL','ohos.permission.NEW_ACL','ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY','ohos.permission.kernel.LOAD_INDEPENDENT_LIBRARY'].map(name=>({name,availableLevel:'system_basic',availableType:'NORMAL',provisionEnable:true,since:14})),
    ...['ohos.permission.INTERNET','ohos.permission.MICROPHONE','ohos.permission.FILE_ACCESS_PERSIST','ohos.permission.READ_WRITE_DOWNLOAD_DIRECTORY','ohos.permission.READ_WRITE_DOCUMENTS_DIRECTORY'].map(name=>({name,availableLevel:'normal',availableType:'NORMAL',provisionEnable:false,since:9})),
    ...['ohos.permission.READ_DOCUMENT','ohos.permission.WRITE_DOCUMENT'].map(name=>({name,availableLevel:'system_basic',availableType:'NORMAL',provisionEnable:true,since:9})),
    {name:'ohos.permission.NEW_API_ACL',availableLevel:'system_basic',availableType:'NORMAL',provisionEnable:true,since:'26.0.0'},
    {name:'ohos.permission.FUTURE_ACL',availableLevel:'system_basic',availableType:'NORMAL',provisionEnable:true,since:27},
  ]};
  json(path.join(sdk,'default/openharmony/toolchains/lib/permissionDefinitions.json'),permissionDefinitions);
  json(path.join(sdk,'default/sdk-pkg.json'),{data:{apiVersion:'26',platformVersion:'26.0.0'}});
  fs.mkdirSync(path.join(home,'.hvigor/wrapper'),{recursive:true,mode:0o700});
  const controlFile=path.join(root,'control.json');
  let control={cert:CERT,...initial};json(controlFile,control);
  const cli=path.join(root,'official fake CLI.cjs'),hdc=path.join(root,'hdc with spaces');
  write(cli,CLI);executable(hdc,HDC);executable(path.join(sdk,'default/openharmony/toolchains/hdc'),HDC);
  write(path.join(root,'sdk-child.cjs'),COMMON+String.raw`
auditRuntime(process.argv[2]?'sdk-grandchild':'sdk-child',true);
if(!process.argv[2]){
  const child=require('node:child_process').spawnSync(process.execPath,[__filename,'leaf'],{encoding:'utf8'});
  if(child.error||child.status!==0||!child.stdout.includes('SDK_CHILD_OK'))throw new Error('SDK grandchild inherited an unsafe loader environment');
}
console.log('SDK_CHILD_OK');
`);
  executable(path.join(root,'unrelated tools/hdc'),COMMON+String.raw`
auditRuntime('other-child',true);
auditToolEnvironment('other-child');
log('other-child',{home:process.env.HOME,server:process.env.HDC_SERVER,umask:process.umask()});
if(process.env.HOME!==process.env.FIXTURE_ORIGINAL_HOME)throw new Error('unrelated child HOME changed');
console.log('OTHER_CHILD_OK');
`);
  executable(path.join(sdk,'default/openharmony/toolchains/lib/hap-sign-tool'),VERIFIER);
  write(path.join(project,'tools/build.cjs'),BUILD);
  const original=Buffer.from('// preserve exported bytes\r\n'+JSON.stringify({app:{signingConfigs:[],products:[{name:'default',runtimeOS:'OpenHarmony'}]},modules:[{name:'entry',srcPath:'./entry'}]},null,3)+'\r\n');
  write(path.join(project,'build-profile.json5'),original);
  json(path.join(project,'AppScope/app.json5'),{app:{bundleName:'org.oheco.fixture'}});
  const module={module:{name:'entry',requestPermissions:[{name:'ohos.permission.TEST_ACL'}]}};
  json(path.join(project,'entry/src/main/module.json5'),module);
  json(path.join(project,'entry/build-profile.json5'),{apiType:'stageMode'});
  const login=path.join(home,'.config/deveco-cli');
  write(path.join(login,'token.enc'),'original-token');write(path.join(login,'token.dek'),'original-dek');
  for(const n of [1,2,3])write(path.join(home,`.local/share/deveco-cli/keys/kek-v${n}.bin`),'original-kek-'+n);
  write(path.join(home,'.harmony/hdckey'),'original-hdc-private');write(path.join(home,'.harmony/hdckey.key'),'original-hdc-component');write(path.join(home,'.harmony/hdckey.pub'),'original-hdc-public');
  const identity=path.join(home,'source project/project.godot'),preset=path.join(path.dirname(identity),'export_presets.cfg');
  const originalPreset='[preset.0.options]\nbuild/sign=false\nsign/key_alias="manual-unchanged"\n';
  write(identity,'[application]\nconfig/name="Original source"\n');write(preset,originalPreset);
  const request={schemaVersion:1,projectDir:project,projectIdentity:identity,nodePath:process.execPath,
    commandLineToolsRoot:clt,sdkRoot:sdk,hdcPath:control.cliUseSDK?path.join(sdk,'default/openharmony/toolchains/hdc'):hdc,target:'serial-is-not-udid',format:'hap',buildMode:'debug',cliPath:cli,teamId:'',loginDataDir:''};
  const env={...process.env,HOME:home,XDG_CONFIG_HOME:config,TMPDIR:tmp,HVIGOR_USER_HOME:path.join(home,'.hvigor'),FIXTURE_CONTROL:controlFile,FIXTURE_SIGNING_ROOT:path.join(config,'godot-openharmony-signing'),DEVECO_CLI_DATA_DIR:login,HDC_SERVER:'127.0.0.1:39999',OHOS_HDC_SERVER_PORT:'8710',FIXTURE_ORIGINAL_HOME:home,FIXTURE_ORIGINAL_CONFIG:config,FIXTURE_CALLER_UMASK:String(process.umask()),FIXTURE_ORIGINAL_CLT:clt,FIXTURE_HDC_PATH:hdc,FIXTURE_NATIVE_WORKER:NATIVE,FIXTURE_SELECTED_NODE_RUNTIME:runtime,FIXTURE_UNSELECTED_NODE_RUNTIME:unselectedRuntime};
  if(initial.runtimeIsolation){
    const brokerLibraries=[path.join(root,'broker runtime first'),path.join(root,'broker runtime second')];
    for(const dir of brokerLibraries)fs.mkdirSync(dir,{mode:0o700});
    env.LD_LIBRARY_PATH=brokerLibraries.join(':')+(process.env.LD_LIBRARY_PATH?':'+process.env.LD_LIBRARY_PATH:'');
    env.FIXTURE_ORIGINAL_LD_LIBRARY_PATH=env.LD_LIBRARY_PATH;env.COMMAND_LINE_TOOL_PATH='';
  }
  function run(changes={}, envChanges={}){
    const selected={...request,...changes},childEnv={...env,...envChanges};
    childEnv.FIXTURE_EXPECTED_LOGIN_DATA=selected.loginDataDir||childEnv.DEVECO_CLI_DATA_DIR||login;
    const command=invocation(['--request-json',JSON.stringify(selected)]);
    return receipt(spawnSync(command.file,command.args,{encoding:'utf8',env:childEnv,timeout:20000,maxBuffer:1024*1024}));
  }
  function events(){const file=path.join(root,'events.jsonl');return fs.existsSync(file)?fs.readFileSync(file,'utf8').trim().split('\n').filter(Boolean).map(line=>JSON.parse(line)):[];}
  function update(value){control={...control,...value};json(controlFile,control);}
  function cache(){const projects=path.join(config,'godot-openharmony-signing/projects');const keys=fs.readdirSync(projects);assert.equal(keys.length,1);return path.join(projects,keys[0]);}
  function profileEdit(change){const file=path.join(cache(),'materials/profile.p7b'),doc=JSON.parse(fs.readFileSync(file));change(doc);json(file,doc);}
  function assertClean(){
    assert.deepEqual(fs.readdirSync(tmp),[],'temporary files leaked');
    const locks=path.join(config,'godot-openharmony-signing/locks');if(fs.existsSync(locks))assert.deepEqual(fs.readdirSync(locks),[],'locks leaked');
    assert.deepEqual(fs.readFileSync(path.join(project,'build-profile.json5')),original,'public build profile changed');
    assert.equal(fs.readFileSync(preset,'utf8'),originalPreset,'source export preset changed');
    assert.equal(fs.readFileSync(path.join(login,'token.enc'),'utf8'),'original-token');
    assert.equal(fs.readFileSync(path.join(login,'token.dek'),'utf8'),'original-dek');
    for(const n of [1,2,3])assert.equal(fs.readFileSync(path.join(home,`.local/share/deveco-cli/keys/kek-v${n}.bin`),'utf8'),'original-kek-'+n);
    for(const [name,value] of [['hdckey','original-hdc-private'],['hdckey.key','original-hdc-component'],['hdckey.pub','original-hdc-public']])assert.equal(fs.readFileSync(path.join(home,'.harmony',name),'utf8'),value);
  }
  return {root,home,config,tmp,project,clt,sdk,cli,hdc,login,request,env,run,events,update,cache,profileEdit,assertClean,original,module};
}

test('native signing preserves the caller tool environment and umask',t=>{
  const previous=process.umask(0o022);
  try{
    const f=fixture(t,{normalEnvironmentProbe:true,cliHdcProbe:true});
    const originals=[path.join(f.login,'token.enc'),path.join(f.login,'token.dek'),path.join(f.home,'.harmony/hdckey')];
    const before=originals.map(file=>({file,bytes:fs.readFileSync(file),mtime:fs.statSync(file).mtimeMs}));
    assert.equal(f.run().status,'ok');
    const events=f.events(),tools=events.filter(row=>row.kind==='normal-tool-environment');
    for(const phase of ['cli','hdc','other-child','verify','build'])assert.ok(tools.some(row=>row.phase===phase),'missing caller environment probe: '+phase);
    for(const row of tools){assert.equal(row.home,f.home);assert.equal(row.config,f.config);assert.equal(row.umask,0o022);}
    for(const row of events.filter(row=>row.kind==='cli')){assert.equal(row.data,f.login);assert.equal(row.clt,f.clt);assert.deepEqual(row.execArgv,[]);}
    const cache=events.find(row=>row.kind==='normal-hvigor-cache');assert.ok(cache);
    assert.equal(cache.home,f.env.HVIGOR_USER_HOME);assert.equal(cache.umask,0o022);assert.equal(cache.directoryMode,0o755);assert.equal(cache.fileMode,0o644);
    for(const row of before){assert.deepEqual(fs.readFileSync(row.file),row.bytes);assert.equal(fs.statSync(row.file).mtimeMs,row.mtime);}
    assert.equal(fs.existsSync(path.join(f.config,'godot-openharmony-signing/cli-home')),false,'worker must not clone CLI credentials');
    assert.equal(fs.existsSync(path.join(f.clt,'sdk/default/openharmony/toolchains/.godot-hdc-host.json')),false,'worker must not install an HDC proxy');
    f.assertClean();
  }finally{process.umask(previous);}
});

test('strict public request protocol and official team table parser',t=>{
  const f=fixture(t,{teams:['12345','67890']});
  for(const args of [[],['--request-json'],['--request-json','{}','extra'],['--request-json',JSON.stringify({...f.request,password:SECRET})]]){
    const command=invocation(args),result=receipt(spawnSync(command.file,command.args,{encoding:'utf8',env:f.env,timeout:20000}));
    assert.equal(result.status,'error');assert.equal(result.code,'INVALID_REQUEST');
  }
  assert.equal(f.events().length,0);
  assert.equal(f.run().code,'TEAM_REQUIRED');assert.equal(f.events().filter(e=>e.kind==='generate').length,0);
  assert.equal(f.run({teamId:'67890'}).status,'ok');
  assert.ok(f.events().find(e=>e.kind==='cli'&&e.args[0]==='signature').args.includes('67890'));f.assertClean();
});
test('first request without a token generates once automatically; matching cache builds without generation',t=>{
  const f=fixture(t);assert.equal(Object.hasOwn(f.request,'confirmationToken'),false);
  const result=f.run();assert.equal(result.status,'ok',JSON.stringify(result));assert.equal(result.signed,true);assert.equal(result.profileValidated,true);
  assert.equal(fs.existsSync(path.join(f.cache(),'pending-confirmation.json')),false);
  assert.equal(fs.existsSync(path.join(f.config,'godot-openharmony-signing/confirmation.key')),false);
  assert.ok(result.outputPath.startsWith(f.project+path.sep));
  const cached=f.run();assert.equal(cached.status,'ok',JSON.stringify(cached));
  assert.equal(f.events().filter(e=>e.kind==='generate').length,1);
  const generate=f.events().filter(e=>e.kind==='cli'&&e.args[0]==='signature')[0];
  assert.deepEqual(generate.args,['signature','generate','--team-id','team-one','--product','default']);
  assert.equal(generate.home,f.home);assert.equal(generate.data,f.login);assert.equal(generate.clt,f.clt);
  const held=f.events().find(event=>event.kind==='generate').locks;
  assert.equal(held.length,3);assert.ok(held.includes('cli'));assert.ok(held.some(name=>name.startsWith('project-')));assert.ok(held.some(name=>name.startsWith('team-')));
  const hdc=f.events().find(e=>e.kind==='hdc');assert.deepEqual(hdc.args,['-t','serial-is-not-udid','shell','bm','get','-u']);assert.equal(hdc.home,f.home);assert.equal(hdc.home,generate.home);assert.equal(hdc.server,'127.0.0.1:39999');
  const build=f.events().find(e=>e.kind==='build');assert.ok(build.requestFile.startsWith(f.tmp+path.sep));assert.equal(build.request.hvigorUserHome||f.env.HVIGOR_USER_HOME,f.env.HVIGOR_USER_HOME);
  for(const key of ['certificate','profile','storeFile'])assert.ok(build.request.signing[key].startsWith(f.cache()+path.sep));
  assert.ok(!JSON.stringify(build.args).includes(build.request.signing.keyPasswordEncrypted));assert.ok(!fs.existsSync(build.requestFile));
  f.assertClean();
});
test('official CLI HDC subprocesses use original tools and ordinary caller environments',t=>{
  const f=fixture(t,{cliHdcProbe:true});assert.equal(f.run().status,'ok');
  const events=f.events();
  assert.deepEqual(events.filter(e=>e.kind==='cli-hdc-probe').map(e=>e.mode),['spawn','spawnSync','execFile','execFileSync','promisify','esm']);
  const hdcs=events.filter(e=>e.kind==='hdc');assert.equal(hdcs.length,9);
  for(const event of hdcs)assert.equal(event.home,f.home);
  const defaults=hdcs.filter(event=>event.server===f.env.HDC_SERVER);assert.equal(defaults.length,3);
  for(const event of defaults)assert.equal(event.port,f.env.OHOS_HDC_SERVER_PORT);
  const selected=hdcs.filter(event=>event.file===path.join(f.sdk,'default/openharmony/toolchains/hdc'));assert.equal(selected.length,7);
  const overridden=hdcs.filter(event=>event.server==='cli-override-must-not-replace-host');assert.equal(overridden.length,6);
  for(const event of overridden)assert.equal(event.port,'1','explicit CLI child options must not be intercepted');
  for(const event of events.filter(e=>e.kind==='cli')){assert.equal(event.home,f.home);assert.equal(event.data,f.login);assert.equal(event.clt,f.clt);assert.deepEqual(event.execArgv,[]);}
  const other=events.find(e=>e.kind==='other-child');assert.equal(other.home,f.home);assert.equal(other.server,'cli-override-must-not-replace-host');
  assert.equal(fs.existsSync(path.join(f.config,'godot-openharmony-signing/cli-home')),false);f.assertClean();
});
test('selected original SDK launchers keep their paths and Node runtime isolation',t=>{
  const f=fixture(t,{cliHdcProbe:true,cliUseSDK:true,cliLauncherProbe:true,runtimeIsolation:true}),key=path.join(f.home,'.harmony/hdckey.key');
  f.request.nodePath=path.join(f.clt,'node/bin/node');
  const runtimeFile=path.join(f.sdk,'default/openharmony/native/llvm/lib/aarch64-linux-ohos/libc++_shared.so'),runtimeBytes=fs.readFileSync(runtimeFile),runtimeStat=fs.statSync(runtimeFile);
  fs.unlinkSync(key);
  assert.equal(f.run().status,'ok');
  const events=f.events(),guard=events.find(e=>e.kind==='toolchain-containment');
  assert.ok(guard);assert.equal(guard.root,fs.realpathSync(f.clt));assert.deepEqual(guard.checked.map(e=>e.name),['node','ohpm','hvigor','sdk','hdc']);
  for(const entry of guard.checked){
    const relative=path.relative(guard.root,entry.resolved);
    assert.equal(path.isAbsolute(relative),false);assert.notEqual(relative,'..');assert.equal(relative.startsWith('..'+path.sep),false);
    assert.equal(entry.isSymlink,false);assert.equal(entry.name==='sdk'?entry.isDirectory:entry.isFile,true);
    if(['node','ohpm','hvigor'].includes(entry.name)){
      assert.equal(entry.sameBytes,true);assert.equal(entry.distinctOriginal,false);
    }
  }
  assert.deepEqual(events.filter(e=>e.kind==='cli-launcher-probe').map(e=>e.name),['ohpm','hvigor']);
  const dependencies=events.find(e=>e.kind==='toolchain-dependency-links');
  for(const entry of dependencies.links){assert.equal(entry.isSymlink,false);assert.equal(entry.resolved,fs.realpathSync(path.join(f.clt,entry.relative)));}
  for(const entry of dependencies.binSiblings){assert.equal(entry.isFile,true);assert.equal(entry.isSymlink,false);assert.equal(entry.sameBytes,true);}
  const node=path.join(f.clt,'node/bin/node');assert.ok(fs.statSync(node).size>4*1024*1024);assert.deepEqual(fs.readFileSync(node),fs.readFileSync(process.execPath),'original signed Node bytes changed');assert.equal(fs.statSync(node).mode&0o7777,0o500,'original fixture Node mode changed');
  assert.equal(fs.existsSync(guard.root),true,'selected original toolchain must remain in place');
  const hdcs=events.filter(e=>e.kind==='hdc');
  assert.equal(hdcs.length,9);
  assert.equal(hdcs.filter(e=>e.file===path.join(f.sdk,'default/openharmony/toolchains/hdc')).length,9);
  assert.equal(hdcs.filter(e=>e.file===f.hdc).length,0);
  for(const event of hdcs)assert.equal(event.home,f.home);
  assert.equal(hdcs.filter(event=>event.server===f.env.HDC_SERVER&&event.port===f.env.OHOS_HDC_SERVER_PORT).length,3);
  assert.equal(hdcs.filter(event=>event.server==='cli-override-must-not-replace-host'&&event.port==='1').length,6);
  assert.equal(fs.existsSync(key),false);
  assert.equal(fs.existsSync(path.join(f.config,'godot-openharmony-signing/cli-home/.harmony/hdckey.key')),false);
  write(key,'original-hdc-component');
  const runtimes=events.filter(e=>e.kind==='runtime-environment');
  for(const phase of ['cli','sdk-child','sdk-grandchild','hdc','other-child','verify','build'])assert.ok(runtimes.some(e=>e.phase===phase),'missing loader inheritance probe: '+phase);
  const isolated=runtimes.filter(e=>e.isolated);assert.ok(isolated.length);
  assert.equal(new Set(isolated.map(e=>e.runtime)).size,1,'one work directory must reuse exactly one runtime');
  for(const event of isolated){assert.equal(event.ld,event.runtime+':'+f.env.LD_LIBRARY_PATH);assert.equal(event.target,fs.realpathSync(runtimeFile));assert.equal(fs.existsSync(event.runtime),false,'Node runtime must be cleaned');}
  assert.equal(runtimes.filter(e=>e.phase==='hdc'&&!e.isolated).length,2,'direct HDC calls retain the original broker loader environment');
  assert.equal(runtimes.filter(e=>e.phase==='hdc'&&e.isolated).length,7,'CLI children inherit the same one-SONAME directory');
  assert.ok(runtimes.filter(e=>e.phase==='verify').every(e=>!e.isolated));
  // A second request with no LD variable must not gain an empty/trailing search
  // component. Cache reuse still exercises CLI, HDC, verifier, and build children.
  const before=f.events().length;
  assert.equal(f.run({}, {LD_LIBRARY_PATH:undefined,FIXTURE_ORIGINAL_LD_LIBRARY_PATH:undefined}).status,'ok');
  const noLD=f.events().slice(before).filter(e=>e.kind==='runtime-environment');assert.ok(noLD.length);
  for(const event of noLD){assert.equal(event.original,'');assert.equal(event.ld,event.isolated?event.runtime:'');}
  assert.equal(f.events().filter(e=>e.kind==='generate').length,1);
  assert.deepEqual(fs.readFileSync(runtimeFile),runtimeBytes);assert.equal(fs.statSync(runtimeFile).mode,runtimeStat.mode);assert.equal(fs.statSync(runtimeFile).mtimeMs,runtimeStat.mtimeMs,'source runtime must remain read-only');
  f.assertClean();
  const absent=fixture(t,{runtimeIsolation:true,runtimeAbsent:true,cliHdcProbe:true});
  assert.equal(absent.run().status,'ok');const absentEvents=absent.events().filter(e=>e.kind==='runtime-environment');assert.ok(absentEvents.length);assert.ok(absentEvents.every(e=>!e.isolated&&e.ld===absent.env.LD_LIBRARY_PATH));absent.assertClean();
  for(const mode of ['wrong-target','extra-library','symlink-dir']){
    const stale=fixture(t,{runtimeIsolation:true,runtimeTamper:mode});
    assert.equal(stale.run().status,'error',mode);
    assert.equal(stale.events().filter(e=>e.kind==='runtime-tamper').length,1);
    assert.equal(stale.events().filter(e=>e.kind==='generate').length,0);assert.equal(stale.events().filter(e=>e.kind==='build').length,0);stale.assertClean();
  }
});
test('CER bundles match the developer certificate without assuming the first entry',t=>{
  // A different parseable public certificate models an unrelated chain entry.
  // The fake native verifier covers trust; this case covers entry selection.
  const bytes=Buffer.from(new (require('node:crypto').X509Certificate)(CERT).raw);bytes[bytes.length-1]^=1;
  const other='-----BEGIN CERTIFICATE-----\n'+bytes.toString('base64').match(/.{1,64}/g).join('\n')+'\n-----END CERTIFICATE-----\n';
  const f=fixture(t,{certBundle:other+CERT});
  assert.equal(f.run().status,'ok');
  assert.equal(f.run().status,'ok');
  assert.equal(f.events().filter(event=>event.kind==='generate').length,1);
  f.assertClean();
});
test('unauthenticated CLI exit zero never triggers login or signing',t=>{
  const f=fixture(t,{notLoggedIn:true});assert.equal(f.run().code,'LOGIN_REQUIRED');
  assert.deepEqual(f.events().filter(e=>e.kind==='cli').map(e=>e.args),[['auth','status']]);f.assertClean();
});
test('multiple teams require an explicit editor team; single team is always passed explicitly',t=>{
  const f=fixture(t,{teams:['first-team','second-team']});assert.equal(f.run().code,'TEAM_REQUIRED');assert.equal(f.events().filter(e=>e.kind==='generate').length,0);
  assert.equal(f.run({teamId:'second-team'}).status,'ok');
  assert.ok(f.events().find(e=>e.kind==='cli'&&e.args[0]==='signature').args.includes('second-team'));f.assertClean();
});
test('device unavailable and unauthorized are distinct; serial is never a substitute for UDID',t=>{
  const f=fixture(t,{deviceFail:true});assert.equal(f.run().code,'DEVICE_UNAVAILABLE');
  f.update({deviceFail:false,unauthorized:true});assert.equal(f.run().code,'DEVICE_UNAUTHORIZED');
  assert.equal(f.events().filter(e=>e.kind==='generate').length,0);f.assertClean();
});
test('a new target automatically regenerates for its actual UDID in the same application cache',t=>{
  const f=fixture(t,{udids:{'second-serial':UDID_B}});assert.equal(f.run().status,'ok');const cache=f.cache();
  f.update({generatedDevices:[UDID_B]});assert.equal(f.run({target:'second-serial'}).status,'ok');
  assert.equal(f.cache(),cache);assert.equal(f.events().filter(e=>e.kind==='generate').length,2);
  assert.equal(f.run({target:'second-serial'}).status,'ok');assert.equal(f.events().filter(e=>e.kind==='generate').length,2);f.assertClean();
});
test('expired profiles update once; mismatching generated devices are rejected without an internal retry',t=>{
  const f=fixture(t,{udids:{'second-serial':UDID_B}});assert.equal(f.run().status,'ok');
  f.profileEdit(p=>{p.validity['not-after']=1;});assert.equal(f.run().status,'ok');assert.equal(f.events().filter(e=>e.kind==='generate').length,2);
  const old=fs.readFileSync(path.join(f.cache(),'materials/profile.p7b'));
  const rejected=f.run({target:'second-serial'});assert.equal(rejected.code,'PROFILE_DEVICE_MISMATCH');
  assert.deepEqual(fs.readFileSync(path.join(f.cache(),'materials/profile.p7b')),old,'failed generation must restore previous app material');
  assert.equal(f.events().filter(e=>e.kind==='generate').length,3);f.assertClean();
});
const OPTIONAL_ACL_FORMS = [
  ['missing', undefined], ['null', null], ['empty object', {}], ['empty array', []],
  ['null allowed-acls', {'allowed-acls':null}], ['empty allowed-acls', {'allowed-acls':[]}],
  ['unknown policy', {policy:'all',account:SECRET}],
  ['object allowed-acls', {'allowed-acls':{all:true}}],
  ['non-string grant', {'allowed-acls':['ohos.permission.TEST_ACL',null]}],
  ['unknown grant', {'allowed-acls':[SECRET]}],
];
function setProfileACL(profile, value) {
  if(value===undefined)delete profile.acls;
  else profile.acls=structuredClone(value);
}
test('normal API26 INTERNET accepts optional ACL forms and reuses cache without granting permissions or tokens',t=>{
  const f=fixture(t,{profileACLPolicy:null});
  f.module.module.requestPermissions=[{name:'ohos.permission.INTERNET'}];
  json(path.join(f.project,'entry/src/main/module.json5'),f.module);
  const metadataFile=path.join(f.sdk,'default/openharmony/toolchains/lib/permissionDefinitions.json');
  const definitions=JSON.parse(fs.readFileSync(metadataFile));
  definitions.definePermissions.find(p=>p.name==='ohos.permission.INTERNET').provisionEnable=true;
  json(metadataFile,definitions);
  assert.equal(f.run().status,'ok','generated null ACL representation must be accepted');
  assert.deepEqual(JSON.parse(fs.readFileSync(path.join(f.cache(),'signing-context.json'))).requiredACL,[]);
  for(const [label,value] of OPTIONAL_ACL_FORMS){
    f.profileEdit(profile=>setProfileACL(profile,value));
    assert.equal(f.run().status,'ok',label);
  }
  assert.equal(f.events().filter(e=>e.kind==='generate').length,1,'ACL form changes must not cause cloud regeneration');
  assert.deepEqual(JSON.parse(fs.readFileSync(path.join(f.project,'entry/src/main/module.json5'))).module.requestPermissions,[{name:'ohos.permission.INTERNET'}]);
  assert.equal(fs.existsSync(path.join(f.cache(),'pending-confirmation.json')),false);
  f.module.module.requestPermissions.push({name:'ohos.permission.UNKNOWN_PERMISSION'});
  json(path.join(f.project,'entry/src/main/module.json5'),f.module);
  assert.equal(f.run().code,'ACL_METADATA_INVALID','optional ACL forms cannot authorize an unknown requested permission');
  assert.equal(f.events().filter(e=>e.kind==='generate').length,1);
  f.assertClean();
});
test('actual ACL requirements reject every absent or malformed policy and require explicit string grants',t=>{
  const f=fixture(t);assert.equal(f.run().status,'ok');
  let generations=1;
  for(const [label,value] of OPTIONAL_ACL_FORMS){
    f.profileEdit(profile=>setProfileACL(profile,value));
    f.update({omitProfileACL:value===undefined,profileACLPolicy:value===undefined?null:value});
    const result=f.run();
    assert.equal(result.code,'PROFILE_INVALID',label);
    assert.ok(result.message.includes('Validation reason: PROFILE_ACL_MISMATCH.'),label);
    assert.ok(!JSON.stringify(result).includes(f.root),'safe reasons must not expose private paths');
    assert.equal(f.events().filter(e=>e.kind==='generate').length,++generations,'one attempt per invalid policy');
  }
  f.update({omitProfileACL:false,profileACLPolicy:{'allowed-acls':['ohos.permission.TEST_ACL']}});
  assert.equal(f.run().status,'ok','explicit required grant must succeed');
  assert.equal(f.events().filter(e=>e.kind==='generate').length,++generations);
  assert.equal(f.run().status,'ok');
  assert.equal(f.events().filter(e=>e.kind==='generate').length,generations,'valid granted cache must be reused');
  f.assertClean();
});
test('ACL-free profiles still require native trust, bundle, device, validity and matching certificate',t=>{
  const bytes=Buffer.from(new (require('node:crypto').X509Certificate)(CERT).raw);bytes[bytes.length-1]^=1;
  const other='-----BEGIN CERTIFICATE-----\n'+bytes.toString('base64').match(/.{1,64}/g).join('\n')+'\n-----END CERTIFICATE-----\n';
  for(const [control,code,reason] of [
    [{verifyFail:true},'PROFILE_INVALID','PROFILE_UNVERIFIED'],
    [{wrongBundle:true},'PROFILE_INVALID','PROFILE_BUNDLE_MISMATCH'],
    [{generatedDevices:[UDID_B]},'PROFILE_DEVICE_MISMATCH','PROFILE_DEVICE_MISMATCH'],
    [{expired:true},'PROFILE_INVALID','PROFILE_EXPIRED'],
    [{certBundle:other},'PROFILE_INVALID','CERTIFICATE_MISMATCH'],
  ]){
    const f=fixture(t,{profileACLPolicy:null,...control});
    f.module.module.requestPermissions=[{name:'ohos.permission.INTERNET'}];
    json(path.join(f.project,'entry/src/main/module.json5'),f.module);
    const result=f.run();assert.equal(result.code,code);
    assert.ok(result.message.includes('Validation reason: '+reason+'.'));
    assert.ok(!JSON.stringify(result).includes(f.root));
    assert.equal(f.events().filter(e=>e.kind==='generate').length,1);
    assert.equal(f.events().filter(e=>e.kind==='build').length,0);
    f.assertClean();
  }
});
test('new ACL permissions and missing profile ACLs update automatically exactly once per request',t=>{
  const f=fixture(t);assert.equal(f.run().status,'ok');f.module.module.requestPermissions.push({name:'ohos.permission.NEW_ACL'});json(path.join(f.project,'entry/src/main/module.json5'),f.module);
  assert.equal(f.run().status,'ok');assert.equal(f.events().filter(e=>e.kind==='generate').length,2);
  assert.equal(f.run().status,'ok');assert.equal(f.events().filter(e=>e.kind==='generate').length,2);
  f.profileEdit(p=>{p.acls['allowed-acls']=[];});assert.equal(f.run().status,'ok');assert.equal(f.events().filter(e=>e.kind==='generate').length,3);f.assertClean();
});
test('generation fails once without retry and restores CLI team files, application keys and configuration',t=>{
  const f=fixture(t);assert.equal(f.run().status,'ok');
  f.profileEdit(p=>{p.validity['not-after']=1;});const oldConfig=fs.readFileSync(path.join(f.cache(),'signing-config.json'));
  const oldProfile=fs.readFileSync(path.join(f.cache(),'materials/profile.p7b'));
  const cliStore=path.join(f.home,'.ohos/config/store.p12');const oldStore=fs.readFileSync(cliStore);
  f.update({generateFail:true,network:true});assert.equal(f.run().code,'NETWORK_ERROR');assert.equal(f.events().filter(e=>e.kind==='generate').length,2);
  assert.deepEqual(fs.readFileSync(cliStore),oldStore);assert.deepEqual(fs.readFileSync(path.join(f.cache(),'signing-config.json')),oldConfig);
  assert.deepEqual(fs.readFileSync(path.join(f.cache(),'materials/profile.p7b')),oldProfile);f.assertClean();
  f.update({generateFail:false,network:false});assert.equal(f.run().status,'ok');assert.equal(f.events().filter(e=>e.kind==='generate').length,3);f.assertClean();
  {
    const broken=fixture(t);assert.equal(broken.run().status,'ok');broken.profileEdit(profile=>{profile.validity['not-after']=1;});
    const cache=broken.cache(),config=fs.readFileSync(path.join(cache,'signing-config.json')),profile=fs.readFileSync(path.join(cache,'materials/profile.p7b'));
    const store=path.join(broken.home,'.ohos/config/store.p12'),storeBytes=fs.readFileSync(store);
    broken.update({generateFail:true,blockCacheRollback:true});
    const result=broken.run();assert.equal(result.status,'error');assert.equal(result.code,'ROLLBACK_FAILED');
    assert.equal(broken.events().filter(event=>event.kind==='generate').length,2);
    const signingRoot=path.join(broken.config,'godot-openharmony-signing'),projects=path.join(signingRoot,'projects');
    try{
      const recoveryRoot=path.join(signingRoot,'recovery'),names=fs.readdirSync(recoveryRoot);assert.equal(names.length,1);
      const recovery=path.join(recoveryRoot,names[0]);
      assert.deepEqual(fs.readFileSync(path.join(recovery,'cache-backup/signing-config.json')),config);
      assert.deepEqual(fs.readFileSync(path.join(recovery,'cache-backup/materials/profile.p7b')),profile);
      assert.deepEqual(fs.readFileSync(path.join(recovery,'cli-backup/config/store.p12')),storeBytes);
      assert.deepEqual(fs.readFileSync(store),storeBytes,'CLI restoration must still run when cache restoration fails');
      function recoveryTree(dir){assert.equal(fs.lstatSync(dir).isDirectory(),true);for(const entry of fs.readdirSync(dir,{withFileTypes:true})){const file=path.join(dir,entry.name);assert.equal(entry.isSymbolicLink(),false);if(entry.isDirectory())recoveryTree(file);else assert.equal(fs.lstatSync(file).isFile(),true);}}
      recoveryTree(recoveryRoot);assert.deepEqual(fs.readdirSync(broken.tmp),[],'durable recovery must allow temporary cleanup');
    }finally{assert.equal(fs.readlinkSync(projects),path.join(broken.root,'unrelated-cache-root'));fs.unlinkSync(projects);fs.renameSync(path.join(broken.root,'projects-before-rollback'),projects);}
    broken.assertClean();
  }
});
test('build failure restores public profile; later build reuses cache; invalid receipts never succeed',t=>{
  const f=fixture(t,{buildFail:true});assert.equal(f.run().code,'BUILD_FAILED');f.assertClean();
  f.update({buildFail:false});assert.equal(f.run().status,'ok');assert.equal(f.events().filter(e=>e.kind==='generate').length,1);
  for(const field of ['outsideArtifact','unsigned','wrongFormat','wrongMode']){f.update({[field]:true});assert.equal(f.run().code,'BUILD_FAILED');f.update({[field]:false});}
  f.assertClean();
});
test('profile verification failures are conservative; unknown all-device policy never becomes ready',t=>{
  const f=fixture(t);assert.equal(f.run().status,'ok');f.update({verifyFail:true});assert.equal(f.run().code,'PROFILE_INVALID');
  assert.equal(f.events().filter(e=>e.kind==='generate').length,2);
  f.update({verifyFail:false,generatedDevices:[]});f.profileEdit(p=>{p['debug-info']['device-ids']=[];});assert.equal(f.run().code,'PROFILE_DEVICE_MISMATCH');
  assert.equal(f.events().filter(e=>e.kind==='generate').length,3);f.assertClean();
});
test('native signing selects existing login data without copying credentials',t=>{
  for(const selection of ['explicit request','inherited CLI directory','HOME fallback']){
    const f=fixture(t,{normalEnvironmentProbe:true}),selected=path.join(f.root,'selected existing CLI login');
    write(path.join(selected,'token.enc'),'selected-original-token');write(path.join(selected,'token.dek'),'selected-original-dek');
    const login=selection==='HOME fallback'?f.login:selected;
    const before=['token.enc','token.dek'].map(name=>{const file=path.join(login,name);return{file,bytes:fs.readFileSync(file),mtime:fs.statSync(file).mtimeMs};});
    const request=selection==='explicit request'?{loginDataDir:login}:{};
    const environment=selection==='inherited CLI directory'?{DEVECO_CLI_DATA_DIR:login}:selection==='HOME fallback'?{DEVECO_CLI_DATA_DIR:undefined}:{};
    assert.equal(f.run(request,environment).status,'ok',selection);
    for(const row of f.events().filter(row=>row.kind==='cli')){assert.equal(row.home,f.home,selection);assert.equal(row.data,login,selection);assert.equal(row.clt,f.clt,selection);}
    for(const row of before){assert.deepEqual(fs.readFileSync(row.file),row.bytes,selection);assert.equal(fs.statSync(row.file).mtimeMs,row.mtime,selection);}
    assert.equal(fs.existsSync(path.join(f.config,'godot-openharmony-signing/cli-home')),false,selection);
    f.assertClean();
  }
});
test('CLI discovery uses PATH, selected Node prefix, and oo current Node without shell or installation',t=>{
  const f=fixture(t),selectedPrefix=path.join(f.root,'selected CLT Node'),selectedNode=path.join(selectedPrefix,'bin/node'),bin=path.join(f.root,'fixture PATH bin');
  fs.mkdirSync(path.dirname(selectedNode),{recursive:true,mode:0o700});fs.symlinkSync(process.execPath,selectedNode);fs.mkdirSync(bin,{mode:0o700});fs.symlinkSync(f.cli,path.join(bin,'devecocli'));
  assert.equal(f.run({cliPath:'',nodePath:selectedNode},{PATH:bin}).status,'ok');
  const entry=path.join(selectedPrefix,'lib/node_modules/@deveco/deveco-cli/cli.js');write(entry,CLI);
  assert.equal(f.run({cliPath:'',nodePath:selectedNode},{PATH:''}).status,'ok');
  assert.ok(entry.startsWith(f.root+path.sep));fs.unlinkSync(entry);
  const ooNode=path.join(f.root,'oo current Node/bin/node');write(ooNode,'public discovery marker; never executed');
  fs.symlinkSync(f.cli,path.join(path.dirname(ooNode),'devecocli'));fs.mkdirSync(path.join(f.home,'.oheco/bin'),{recursive:true,mode:0o700});fs.symlinkSync(ooNode,path.join(f.home,'.oheco/bin/node'));
  assert.equal(f.run({cliPath:'',nodePath:selectedNode},{PATH:''}).status,'ok');assert.equal(f.events().filter(e=>e.kind==='generate').length,1);f.assertClean();
});
test('missing CLI and old/missing Node produce stable actionable errors',t=>{
  const f=fixture(t);assert.equal(f.run({cliPath:path.join(f.root,'missing CLI')}).code,'CLI_MISSING');
  assert.equal(f.run({nodePath:path.join(f.root,'missing Node')}).code,'NODE_MISSING');
  const old=path.join(f.root,'old-node');executable(old,"console.log('v20.19.0');");assert.equal(f.run({nodePath:old}).code,'NODE_VERSION_TOO_OLD');
  f.assertClean();
});
test('malformed project/CLI JSON and inaccessible verifier produce expected safe errors',t=>{
  const f=fixture(t,{malformedGenerated:true});
  json(path.join(f.project,'build-profile.json5'),{app:{products:[null]},modules:[null]});assert.equal(f.run().code,'INVALID_PROJECT');write(path.join(f.project,'build-profile.json5'),f.original);
  f.module.module.requestPermissions=null;json(path.join(f.project,'entry/src/main/module.json5'),f.module);assert.equal(f.run().code,'INVALID_PROJECT');
  f.module.module.requestPermissions=[{name:'ohos.permission.TEST_ACL'}];json(path.join(f.project,'entry/src/main/module.json5'),f.module);
  assert.equal(f.run().code,'SIGNING_FAILED');f.assertClean();f.update({malformedGenerated:false});assert.equal(f.run().status,'ok');
  fs.chmodSync(path.join(f.sdk,'default/openharmony/toolchains/lib/hap-sign-tool'),0o600);assert.equal(f.run().code,'CLT_MISSING');f.assertClean();
});
test('SDK permission metadata filters ACL exceptions, version strings, and unsupported permissions',t=>{
  const f=fixture(t,{generatedACL:['ohos.permission.FILE_ACCESS_PERSIST','ohos.permission.NEW_API_ACL']});
  f.module.module.requestPermissions=['ohos.permission.INTERNET','ohos.permission.READ_DOCUMENT','ohos.permission.WRITE_DOCUMENT','ohos.permission.FILE_ACCESS_PERSIST','ohos.permission.NEW_API_ACL'].map(name=>({name}));
  json(path.join(f.project,'entry/src/main/module.json5'),f.module);assert.equal(f.run().status,'ok');
  const stored=JSON.parse(fs.readFileSync(path.join(f.cache(),'signing-context.json')));
  assert.deepEqual(stored.requiredACL,['ohos.permission.FILE_ACCESS_PERSIST','ohos.permission.NEW_API_ACL']);
  f.module.module.requestPermissions.push({name:'ohos.permission.FUTURE_ACL'});json(path.join(f.project,'entry/src/main/module.json5'),f.module);assert.equal(f.run().code,'ACL_UNSUPPORTED');
  f.module.module.requestPermissions=[{name:'ohos.permission.UNKNOWN_PERMISSION'}];json(path.join(f.project,'entry/src/main/module.json5'),f.module);assert.equal(f.run().code,'ACL_METADATA_INVALID');f.assertClean();
});
test('JSON schema errors and unexpected fatal failures each emit one safe JSON receipt',t=>{
  const f=fixture(t);assert.equal(f.run({password:SECRET}).code,'INVALID_REQUEST');assert.equal(f.run({schemaVersion:2}).code,'INVALID_REQUEST');
  // JsonCpp throws when its bounded parser stack is exceeded, exercising the
  // executable's unexpected-exception receipt without a product test hook.
  const raw='{"password":'+JSON.stringify(SECRET)+',"nested":'+'['.repeat(256)+'0'+']'.repeat(256)+'}';
  const child=spawnSync(NATIVE,['--request-json',raw],{encoding:'utf8',env:f.env,timeout:20000});
  const result=receipt(child,1);assert.equal(result.status,'error');assert.equal(result.code,'INTERNAL_ERROR');assert.equal(f.events().length,0);f.assertClean();
});
test('failure to restore the public build profile is reported explicitly',t=>{
  const f=fixture(t,{noRestore:true});assert.equal(f.run().code,'BUILD_PROFILE_NOT_RESTORED');
  write(path.join(f.project,'build-profile.json5'),f.original);f.assertClean();
});
test('normal permissions reuse matching ACL-free material and removed ACLs automatically update once',t=>{
  const f=fixture(t,{generatedACL:[]});
  f.module.module.requestPermissions=[{name:'ohos.permission.INTERNET'},{name:'ohos.permission.MICROPHONE'}];
  json(path.join(f.project,'entry/src/main/module.json5'),f.module);assert.equal(f.run().status,'ok');
  f.module.module.requestPermissions.push({name:'ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY'});
  json(path.join(f.project,'entry/src/main/module.json5'),f.module);f.update({generatedACL:['ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY']});
  assert.equal(f.run().status,'ok');
  f.module.module.requestPermissions.pop();json(path.join(f.project,'entry/src/main/module.json5'),f.module);
  f.update({generatedACL:[]});assert.equal(f.run().status,'ok');assert.equal(f.events().filter(e=>e.kind==='generate').length,3);f.assertClean();
});
test('stable projectIdentity reuses cache across temporary export directories without tokens',t=>{
  const f=fixture(t);assert.equal(f.run().status,'ok');const other=path.join(f.root,'second exported project');fs.cpSync(f.project,other,{recursive:true});
  const result=f.run({projectDir:other});assert.equal(result.status,'ok',JSON.stringify(result));assert.ok(result.outputPath.startsWith(other+path.sep));
  assert.equal(f.run().status,'ok');assert.equal(f.events().filter(e=>e.kind==='generate').length,1);f.assertClean();
});
test('device identity is checked again under the team lock and current UDID is generated once',t=>{
  const f=fixture(t,{changeUDIDBeforeGeneration:true,generatedDevices:[UDID_B]});assert.equal(f.run().status,'ok');
  const profile=JSON.parse(fs.readFileSync(path.join(f.cache(),'materials/profile.p7b')));
  assert.deepEqual(profile['debug-info']['device-ids'],[UDID_B]);assert.equal(f.events().filter(e=>e.kind==='generate').length,1);f.assertClean();
});
test('legacy pending files and HMAC keys remain unused and untouched during automatic updates',t=>{
  const f=fixture(t);assert.equal(f.run().status,'ok');
  const pending=path.join(f.cache(),'pending-confirmation.json'),key=path.join(f.config,'godot-openharmony-signing/confirmation.key');
  write(pending,'legacy-invalid-json-is-not-read');write(key,'legacy-key-is-not-used');
  f.profileEdit(profile=>{profile.validity['not-after']=1;});assert.equal(f.run().status,'ok');
  assert.equal(fs.readFileSync(pending,'utf8'),'legacy-invalid-json-is-not-read');assert.equal(fs.readFileSync(key,'utf8'),'legacy-key-is-not-used');
  assert.equal(f.run({confirmationToken:'a'.repeat(64)}).code,'INVALID_REQUEST');
  assert.equal(f.events().filter(event=>event.kind==='generate').length,2);f.assertClean();
});
test('cache becoming valid at the locked recheck is reused without generation',t=>{
  const f=fixture(t);assert.equal(f.run().status,'ok');f.update({verifyFailOnce:true});assert.equal(f.run().status,'ok');
  assert.equal(f.events().filter(event=>event.kind==='generate').length,1);f.assertClean();
});
test('live permissions and unrelated CLI state are re-read before one generation',t=>{
  const f=fixture(t,{permissionsOnRecheck:['ohos.permission.TEST_ACL','ohos.permission.NEW_ACL'],otherCLIStateOnRecheck:true});
  assert.equal(f.run().status,'ok');const profile=JSON.parse(fs.readFileSync(path.join(f.cache(),'materials/profile.p7b')));
  assert.deepEqual(profile.acls['allowed-acls'].slice().sort(),['ohos.permission.NEW_ACL','ohos.permission.TEST_ACL']);
  assert.equal(f.events().filter(event=>event.kind==='generate').length,1);f.assertClean();
});
test('bundle or automatically selected team changing under locks does not generate for stale cache identity',t=>{
  const changedBundle=fixture(t,{bundleOnRecheck:'org.oheco.changed'});assert.equal(changedBundle.run().code,'PROJECT_CHANGED');
  assert.equal(changedBundle.events().filter(event=>event.kind==='generate').length,0);changedBundle.assertClean();
  const changedTeam=fixture(t,{teamsOnRecheck:['different-team']});assert.equal(changedTeam.run().code,'TEAM_REQUIRED');
  assert.equal(changedTeam.events().filter(event=>event.kind==='generate').length,0);changedTeam.assertClean();
});
test('malformed cached configuration regenerates safely without forwarding stale signing paths',t=>{
  const f=fixture(t);assert.equal(f.run().status,'ok');write(path.join(f.cache(),'signing-config.json'),'invalid old config');
  assert.equal(f.run().status,'ok');assert.equal(f.events().filter(event=>event.kind==='generate').length,2);f.assertClean();
});
test('SIGTERM waits for child exit, releases locks, and cleans owned temporary requests',async t=>{
  const f=fixture(t);assert.equal(f.run().status,'ok');f.update({holdBuild:true});
  let watcher,timer;
  const held=new Promise((resolve,reject)=>{
    const check=()=>{if(f.events().some(event=>event.kind==='build-held'))resolve();};
    watcher=fs.watch(f.root,check);timer=setTimeout(()=>reject(new Error('fixture build did not start')),10000);
  });
  const command=invocation(['--request-json',JSON.stringify(f.request)]);
  const child=spawn(command.file,command.args,{env:f.env,stdio:['ignore','pipe','pipe']});
  let stdout='',stderr='';child.stdout.on('data',bytes=>stdout+=bytes);child.stderr.on('data',bytes=>stderr+=bytes);
  const closed=new Promise(resolve=>child.on('close',(code,signal)=>resolve({code,signal})));
  let grandchild;
  function alive(pid){
    try{process.kill(pid,0);}catch(error){if(error.code==='ESRCH')return false;throw error;}
    // A reparented zombie has exited; the system reaper owns its final wait.
    try{const stat=fs.readFileSync('/proc/'+pid+'/stat','utf8');return stat.slice(stat.lastIndexOf(')')+2)[0]!=='Z';}catch(error){if(error.code==='ENOENT')return false;throw error;}
  }
  try{
    await held;grandchild=f.events().find(event=>event.kind==='build-held').grandchild;
    assert.ok(Number.isSafeInteger(grandchild)&&grandchild>1);assert.equal(alive(grandchild),true);
    child.kill('SIGTERM');const result=await closed;const response=receipt({status:result.code,signal:result.signal,stdout,stderr});assert.equal(response.status,'error');assert.equal(response.code,'INTERRUPTED');
    assert.ok(f.events().some(event=>event.kind==='build-exiting'));assert.equal(alive(grandchild),false,'SIGTERM receipt must wait for the redirected-stdio, SIGTERM-ignoring grandchild to exit');
  }
  finally{
    watcher.close();clearTimeout(timer);if(child.exitCode===null)child.kill('SIGKILL');
    if(grandchild&&alive(grandchild))process.kill(grandchild,'SIGKILL');
  }
  f.assertClean();
});
test('project lock prevents concurrent cache mutation and leaves another owners lock intact',t=>{
  const f=fixture(t);assert.equal(f.run().status,'ok');
  const cache=f.cache(),key=path.basename(cache),lock=path.join(f.config,'godot-openharmony-signing/locks','project-'+key);fs.mkdirSync(lock,{mode:0o700});
  try{assert.equal(f.run().code,'CACHE_BUSY');assert.ok(fs.existsSync(lock));}finally{fs.rmdirSync(lock);}f.assertClean();
});
