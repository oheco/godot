// Godot Engine contributors. SPDX-License-Identifier: MIT
// Transpile the real Templates.ets with an existing SDK compiler; run controlled
// resource/FileIO mocks on a real private TMPDIR. No network/HAP/GUI/signing.
'use strict';
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');

const args = process.argv.slice(2);
assert.ok(args.length === 0 || (args.length === 2 && args[0] === '--compiler'), 'Use --compiler /path/to/typescript.js');
const compiler = args[1] || process.env.GODOT_ARKTS_TYPESCRIPT;
assert.ok(compiler, 'Provide --compiler or GODOT_ARKTS_TYPESCRIPT; no compiler is downloaded');
const ts = require(path.resolve(compiler));
const runtime = path.resolve(__dirname, '../../../misc/dist/openharmony_template/entry/src/main/ets/runtime');
const NAMES = ['openharmony_debug_arm64-v8a.zip', 'openharmony_release_arm64-v8a.zip'];
const sha = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const plain = value => JSON.parse(JSON.stringify(value));
function load(name, dependencies) {
  const result = ts.transpileModule(fs.readFileSync(path.join(runtime,name),'utf8'), {
    fileName:name.replace(/\.ets$/,'.ts'), reportDiagnostics:true,
    compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS},
  });
  const errors=(result.diagnostics||[]).filter(item=>item.category===ts.DiagnosticCategory.Error);
  assert.equal(errors.length,0,errors.map(item=>ts.flattenDiagnosticMessageText(item.messageText,'\n')).join('\n'));
  const sandbox={exports:{},console,Date,ArrayBuffer,Uint8Array,Promise,TextDecoder,TextEncoder,
    require:id=>{assert.ok(Object.hasOwn(dependencies,id),`Unexpected dependency ${id}`);return dependencies[id];}};
  vm.runInNewContext(result.outputText,sandbox,{filename:name});
  return sandbox.exports;
}
function sdkError(error) {
  if(error.code==='ENOENT') error.code=13900002;
  else if(error.code==='EEXIST') error.code=13900015;
  else if(error.code==='ENOTDIR') error.code=13900018;
  return error;
}
function call(operation) {try{return operation();}catch(error){throw sdkError(error);}}
function fixture(t, options={}) {
  assert.ok(process.env.TMPDIR && fs.statSync(process.env.TMPDIR).isDirectory(),'TMPDIR must exist');
  const root=fs.mkdtempSync(path.join(os.tmpdir(),'godot-templates-test-'));
  t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
  const files=path.join(root,'files'); fs.mkdirSync(files,{mode:0o700});
  const payloads=options.large ? [Buffer.alloc(1024*1024+257,0x5a),Buffer.alloc(131121,0x34)] :
    [Buffer.from('SYNTHETIC DEBUG ZIP\0with binary data'),Buffer.from('SYNTHETIC RELEASE ZIP\xffwith binary data')];
  const manifest={schemaVersion:1,sha256:sha(Buffer.concat(payloads)),files:payloads.map((payload,index)=>({name:NAMES[index],sha256:sha(payload),size:payload.length}))};
  const sourceFiles=payloads.map((payload,index)=>{
    const filename=path.join(root,`raw-${index}`);
    fs.writeFileSync(filename,Buffer.concat([Buffer.from('HEADER-NOT-COPIED!'),payload,Buffer.from('SUFFIX-NOT-COPIED')]));
    return filename;
  });
  const events=[]; const logs=[]; const offsets=new Map(); const contexts=[];
  const control={...options};
  const parent=path.join(files,'export_templates');
  const destination=()=>path.join(parent,manifest.sha256);
  let rawOpens=0,rawCloses=0,hashes=0;
  let releaseHash;
  const hashGate=new Promise(resolve=>{releaseHash=resolve;});
  let gatedHashes=0;
  const io={
    OpenMode:{CREATE:fs.constants.O_CREAT,WRITE_ONLY:fs.constants.O_WRONLY,TRUNC:fs.constants.O_TRUNC,NOFOLLOW:fs.constants.O_NOFOLLOW},
    WhenceType:{SEEK_SET:0},
    accessSync:filename=>fs.existsSync(filename),
    statSync:filename=>call(()=>fs.statSync(filename)),
    lstatSync:filename=>call(()=>fs.lstatSync(filename)),
    mkdirSync:(filename,recursive)=>call(()=>{
      events.push(['mkdir',filename]);
      if(control.stageCollision && filename.includes('.preparing-')) {
        fs.mkdirSync(filename);fs.writeFileSync(path.join(filename,'FOREIGN'),'KEEP');
      }
      fs.mkdirSync(filename,{recursive:recursive===true,mode:0o700});
    }),
    listFileSync:filename=>call(()=>fs.readdirSync(filename)),
    rmdirSync:filename=>call(()=>{events.push(['rmdir',filename]);fs.rmdirSync(filename);}),
    unlinkSync:filename=>call(()=>{events.push(['unlink',filename]);fs.unlinkSync(filename);}),
    renameSync:(from,to)=>call(()=>{
      events.push(['rename',from,to]);
      if(control.renameFailure && to===destination()) throw new Error('synthetic publication failure');
      if(control.pointerFailure && to===path.join(parent,'current.json')) throw new Error('synthetic pointer failure');
      if(control.invalidRace && to===destination()) {fs.mkdirSync(to);fs.writeFileSync(path.join(to,'FOREIGN'),'KEEP');throw new Error('synthetic invalid race');}
      fs.renameSync(from,to);
    }),
    openSync:(filename,flags)=>call(()=>({fd:fs.openSync(filename,flags)})),
    closeSync:file=>call(()=>fs.closeSync(file.fd)),
    lseek:(fd,offset)=>{events.push(['lseek',offset]);offsets.set(fd,offset);return offset;},
    read:async(fd,buffer,settings)=>call(()=>{
      assert.equal(buffer.byteLength,1024*1024);
      assert.ok(settings.length<=1024*1024);
      if(control.readZero) return 0;
      const count=fs.readSync(fd,Buffer.from(buffer),0,Math.min(settings.length,100003),offsets.get(fd));
      offsets.set(fd,offsets.get(fd)+count);events.push(['read',count]);return count;
    }),
    write:async(fd,buffer)=>call(()=>{
      if(control.writeZero) return 0;
      const limit=control.large?65537:7;
      const count=fs.writeSync(fd,Buffer.from(buffer).subarray(0,limit));events.push(['write',count]);return count;
    }),
    writeSync:(fd,buffer)=>call(()=>{
      if(control.documentZero) return 0;
      const count=fs.writeSync(fd,Buffer.from(buffer).subarray(0,11));events.push(['writeDocument',count]);return count;
    }),
    readTextSync:(filename,settings)=>call(()=>{
      const buffer=fs.readFileSync(filename);return buffer.subarray(0,settings.length).toString();
    }),
  };
  const helpers=load('FileSystem.ets',{'@kit.CoreFileKit':{fileIo:io}});
  const modules=pid=>load('Templates.ets',{
    '@kit.AbilityKit':{},'@kit.BasicServicesKit':{},
    '@kit.ArkTS':{util:{
      TextDecoder:class{decodeWithStream(data){return new TextDecoder().decode(data);}},
      TextEncoder:class{encodeInto(text){return new TextEncoder().encode(text);}},
      generateRandomUUID:()=>crypto.randomUUID(),
    }},
    '@kit.CoreFileKit':{fileIo:io,hash:{hash:async(filename,algorithm)=>{
      assert.equal(algorithm,'sha256');hashes++;events.push(['hash',filename]);
      if(control.cleanupLink) {
        const other=path.join(root,'unrelated');fs.mkdirSync(other,{recursive:true});fs.writeFileSync(path.join(other,'keep'),'FOREIGN DATA');
        const link=path.join(path.dirname(filename),'foreign-link');if(!fs.existsSync(link)) fs.symlinkSync(other,link);
      }
      if(control.race && filename.endsWith(NAMES[1])) {
        gatedHashes++;if(gatedHashes===2) releaseHash();await hashGate;
      }
      return control.hashFailure?'0'.repeat(64):sha(fs.readFileSync(filename)).toUpperCase();
    }}},
    'libentry.so':{default:{processId:()=>pid}},
    './Diagnostics':{record:message=>logs.push(message),describe:error=>String(error),setStep:step=>events.push(['step',step])},
    './FileSystem':helpers,
  });
  function context() {
    const active=new Map();
    const result={filesDir:files,resourceManager:{
      getRawFileContent:async name=>{
        assert.equal(name,'export-templates.json');events.push(['manifest']);
        if(control.absent) throw Object.assign(new Error('manifest absent'),{code:9001005});
        if(control.getFailure) throw Object.assign(new Error('generic resource read failure'),{code:13900012});
        return Buffer.from(control.document===undefined?JSON.stringify(manifest):control.document);
      },
      getRawFd:async name=>{
        const index=NAMES.indexOf(name.slice('export-templates/'.length));
        assert.ok(index>=0);assert.equal(name,`export-templates/${NAMES[index]}`);
        assert.equal(active.has(name),false,'coalescing must prevent shared resource cursor reads');
        if(control.rawMissing) throw Object.assign(new Error('raw zip missing'),{code:9001005});
        const fd=fs.openSync(sourceFiles[index],'r');active.set(name,fd);rawOpens++;
        return {fd,offset:18,length:manifest.files.find(file=>file.name===NAMES[index]).size+(control.sizeMismatch?1:0)};
      },
      closeRawFd:async name=>{
        assert.ok(active.has(name));fs.closeSync(active.get(name));active.delete(name);rawCloses++;
        if(control.closeFailure) throw new Error('raw close failure');
      },
    }};
    contexts.push(active);return result;
  }
  t.after(()=>{for(const active of contexts)for(const fd of active.values()){try{fs.closeSync(fd);}catch{}}});
  function assertClean() {
    if(!fs.existsSync(parent)) return;
    assert.equal(fs.readdirSync(parent).some(name=>name.includes('.preparing-')||name.startsWith('.pointer-')),false,'own temporary trees must be cleaned');
  }
  return {root,files,parent,manifest,payloads,control,events,logs,modules,context,destination,assertClean,
    counts:()=>({rawOpens,rawCloses,hashes})};
}

function verifyPublished(f) {
  const dest=f.destination();
  assert.deepEqual(JSON.parse(fs.readFileSync(path.join(dest,'.ready'))),f.manifest);
  assert.deepEqual(JSON.parse(fs.readFileSync(path.join(f.parent,'current.json'))),{schemaVersion:1,sha256:f.manifest.sha256});
  for(let i=0;i<NAMES.length;i++)assert.deepEqual(fs.readFileSync(path.join(dest,NAMES[i])),f.payloads[i]);
  f.assertClean();
}

test('cold copies raw fd ranges with 1MiB buffer and partial writes; warm does not rehash',async t=>{
  const f=fixture(t,{large:true});const module=f.modules(101);const context=f.context();
  await module.prepareTemplates(context,f.files);verifyPublished(f);
  assert.deepEqual(f.counts(),{rawOpens:2,rawCloses:2,hashes:2});
  assert.ok(f.events.filter(event=>event[0]==='read').length>2);
  assert.ok(f.events.filter(event=>event[0]==='writeDocument').length>4);
  await module.prepareTemplates(context,f.files);verifyPublished(f);
  assert.deepEqual(f.counts(),{rawOpens:2,rawCloses:2,hashes:2});
});
test('same-process calls coalesce, independent process publication race validates ready',async t=>{
  const f=fixture(t,{race:true});const a=f.modules(101),b=f.modules(202),ca=f.context(),cb=f.context();
  fs.mkdirSync(f.parent,{recursive:true});const foreign=path.join(f.parent,'foreign.preparing-not-owned');
  fs.mkdirSync(foreign);fs.writeFileSync(path.join(foreign,'keep'),'KEEP');
  let timer;
  try{await Promise.race([
    Promise.all([a.prepareTemplates(ca,f.files),a.prepareTemplates(ca,f.files),b.prepareTemplates(cb,f.files)]),
    new Promise((resolve,reject)=>{timer=setTimeout(()=>reject(new Error('race timed out')),5000);}),
  ]);}finally{clearTimeout(timer);}
  assert.deepEqual(f.counts(),{rawOpens:4,rawCloses:4,hashes:4});
  assert.equal(fs.readFileSync(path.join(foreign,'keep'),'utf8'),'KEEP');
  assert.ok(f.logs.some(message=>message.includes('won by another process')));
  fs.rmSync(foreign,{recursive:true});verifyPublished(f);
});
test('known absent manifest leaves old pointer/cache untouched and returns',async t=>{
  const f=fixture(t,{absent:true});fs.mkdirSync(f.parent);
  const pointer=path.join(f.parent,'current.json');fs.writeFileSync(pointer,'OLD POINTER');
  await f.modules(101).prepareTemplates(f.context(),f.files);
  assert.equal(fs.readFileSync(pointer,'utf8'),'OLD POINTER');
  assert.deepEqual(f.counts(),{rawOpens:0,rawCloses:0,hashes:0});
  assert.ok(f.logs.some(message=>message.includes('manifest absent')));f.assertClean();
});
test('generic manifest GET failure is not swallowed as absent',async t=>{
  const f=fixture(t,{getFailure:true});await assert.rejects(f.modules(101).prepareTemplates(f.context(),f.files),/generic resource read failure/);
  assert.equal(fs.existsSync(f.parent),false);
});
for(const [name,control,expected]of[
  ['hash mismatch',{hashFailure:true,cleanupLink:true},/checksum mismatch/],
  ['zero read',{readZero:true},/Unexpected end/],
  ['zero write',{writeZero:true},/Unable to write export template archive/],
  ['descriptor length mismatch',{sizeMismatch:true},/descriptor\/size mismatch/],
  ['missing archive',{rawMissing:true},/raw zip missing/],
  ['raw close failure',{closeFailure:true},/raw close failure/],
  ['document write failure',{documentZero:true},/Unable to write export template document/],
  ['publication failure',{renameFailure:true},/publication failure/],
])test(`${name} rejects, preserves unrelated caches, and cleans only own staging`,async t=>{
  const f=fixture(t,control);fs.mkdirSync(f.parent);const old=path.join(f.parent,'a'.repeat(64));
  fs.mkdirSync(old);fs.writeFileSync(path.join(old,'keep'),'OLD');fs.writeFileSync(path.join(f.parent,'current.json'),'OLD POINTER');
  await assert.rejects(f.modules(101).prepareTemplates(f.context(),f.files),expected);
  assert.equal(fs.readFileSync(path.join(old,'keep'),'utf8'),'OLD');
  assert.equal(fs.readFileSync(path.join(f.parent,'current.json'),'utf8'),'OLD POINTER');
  assert.equal(fs.existsSync(f.destination()),false);f.assertClean();
  const counts=f.counts();assert.equal(counts.rawOpens,counts.rawCloses);
  if(control.cleanupLink)assert.equal(fs.readFileSync(path.join(f.root,'unrelated/keep'),'utf8'),'FOREIGN DATA');
});
test('pointer rename failure preserves complete published cache and old pointer, retry succeeds warm',async t=>{
  const f=fixture(t,{pointerFailure:true});fs.mkdirSync(f.parent);fs.writeFileSync(path.join(f.parent,'current.json'),'OLD POINTER');
  const module=f.modules(101),context=f.context();await assert.rejects(module.prepareTemplates(context,f.files),/pointer failure/);
  assert.equal(fs.readFileSync(path.join(f.parent,'current.json'),'utf8'),'OLD POINTER');f.assertClean();
  assert.equal(fs.existsSync(path.join(f.destination(),'.ready')),true);
  f.control.pointerFailure=false;await module.prepareTemplates(context,f.files);verifyPublished(f);
  assert.deepEqual(f.counts(),{rawOpens:2,rawCloses:2,hashes:2});
});
for(const kind of ['marker','size','archive symlink','destination symlink'])test(`warm ${kind} mismatch refuses old destination without removing it`,async t=>{
  const f=fixture(t);const module=f.modules(101),context=f.context();await module.prepareTemplates(context,f.files);
  const dest=f.destination(),marker=path.join(dest,'.ready');
  if(kind==='marker')fs.writeFileSync(marker,'{}');
  if(kind==='size')fs.writeFileSync(path.join(dest,NAMES[0]),'BAD SIZE');
  if(kind==='archive symlink'){fs.unlinkSync(path.join(dest,NAMES[0]));fs.symlinkSync(path.join(f.root,'raw-0'),path.join(dest,NAMES[0]));}
  if(kind==='destination symlink'){fs.renameSync(dest,dest+'-keep');fs.symlinkSync(dest+'-keep',dest);}
  await assert.rejects(module.prepareTemplates(context,f.files),/Invalid published/);
  assert.equal(fs.existsSync(dest),true);assert.deepEqual(f.counts(),{rawOpens:2,rawCloses:2,hashes:2});f.assertClean();
});
test('foreign staging creation collision is never adopted or removed',async t=>{
  const f=fixture(t,{stageCollision:true});await assert.rejects(f.modules(101).prepareTemplates(f.context(),f.files));
  const foreign=fs.readdirSync(f.parent).find(name=>name.includes('.preparing-'));
  assert.ok(foreign);assert.equal(fs.readFileSync(path.join(f.parent,foreign,'FOREIGN'),'utf8'),'KEEP');
  assert.deepEqual(f.counts(),{rawOpens:0,rawCloses:0,hashes:0});
});
test('invalid raced destination is preserved and causes failure, not successful publication',async t=>{
  const f=fixture(t,{invalidRace:true});await assert.rejects(f.modules(101).prepareTemplates(f.context(),f.files),/invalid race/);
  assert.equal(fs.readFileSync(path.join(f.destination(),'FOREIGN'),'utf8'),'KEEP');
  assert.equal(fs.existsSync(path.join(f.parent,'current.json')),false);f.assertClean();
});
for(const kind of ['parent','anchor','pointer'])test(`private ${kind} symlink is not followed`,async t=>{
  const f=fixture(t);const other=path.join(f.root,'other');fs.mkdirSync(other);fs.writeFileSync(path.join(other,'keep'),'KEEP');
  if(kind==='parent')fs.symlinkSync(other,f.parent);
  if(kind==='anchor'){fs.rmdirSync(f.files);fs.symlinkSync(other,f.files);}
  if(kind==='pointer'){fs.mkdirSync(f.parent);fs.symlinkSync(path.join(other,'keep'),path.join(f.parent,'current.json'));}
  await assert.rejects(f.modules(101).prepareTemplates(f.context(),f.files));
  assert.equal(fs.readFileSync(path.join(other,'keep'),'utf8'),'KEEP');
  if(kind!=='anchor')assert.equal(fs.lstatSync(kind==='parent'?f.parent:path.join(f.parent,'current.json')).isSymbolicLink(),true);
});
test('symlink ancestor and unsafe filesDir components are rejected before copying',async t=>{
  const f=fixture(t);const real=path.join(f.root,'real');fs.mkdirSync(real);fs.mkdirSync(path.join(real,'files'));
  const link=path.join(f.root,'link');fs.symlinkSync(real,link);
  const module=f.modules(101),context=f.context();
  for(const filesDir of [path.join(link,'files'),'relative',f.files+'/',f.files+'/../files',f.files+'//child']){
    await assert.rejects(module.prepareTemplates(context,filesDir));
  }
  assert.deepEqual(f.counts(),{rawOpens:0,rawCloses:0,hashes:0});
  assert.deepEqual(fs.readdirSync(path.join(real,'files')),[]);
});
for(const [name,mutate]of[
  ['null',()=>null],['array',()=>[]],['unknown root key',manifest=>({...manifest,extra:true})],
  ['version',manifest=>({...manifest,schemaVersion:2})],['nonhex combined digest',manifest=>({...manifest,sha256:'../unsafe'})],
  ['missing release',manifest=>({...manifest,files:[manifest.files[0]]})],
  ['duplicate debug',manifest=>({...manifest,files:[manifest.files[0],manifest.files[0]]})],
  ['unsafe filename',manifest=>({...manifest,files:[{...manifest.files[0],name:'../evil.zip'},manifest.files[1]]})],
  ['unknown file key',manifest=>({...manifest,files:[{...manifest.files[0],extra:1},manifest.files[1]]})],
  ['zero size',manifest=>({...manifest,files:[{...manifest.files[0],size:0},manifest.files[1]]})],
  ['unsafe size',manifest=>({...manifest,files:[{...manifest.files[0],size:Number.MAX_SAFE_INTEGER+1},manifest.files[1]]})],
  ['bad file hash',manifest=>({...manifest,files:[{...manifest.files[0],sha256:'F'.repeat(64)},manifest.files[1]]})],
])test(`exact manifest rejects ${name} before any raw fd/copy`,async t=>{
  const f=fixture(t);f.control.document=JSON.stringify(mutate(plain(f.manifest)));
  await assert.rejects(f.modules(101).prepareTemplates(f.context(),f.files));
  assert.equal(fs.existsSync(f.parent),false);assert.deepEqual(f.counts(),{rawOpens:0,rawCloses:0,hashes:0});
});
test('invalid JSON, empty/oversized manifest are errors, not absent fallback',async t=>{
  const f=fixture(t);const module=f.modules(101),context=f.context();
  for(const document of ['{','',' '.repeat(16385)]){
    f.control.document=document;await assert.rejects(module.prepareTemplates(context,f.files));
  }
  assert.equal(fs.existsSync(f.parent),false);assert.equal(f.logs.some(message=>message.includes('manifest absent')),false);
});
test('reordered file records are accepted and semantically identical warm ready marker remains valid',async t=>{
  const f=fixture(t);const module=f.modules(101),context=f.context();await module.prepareTemplates(context,f.files);
  f.manifest.files.reverse();await module.prepareTemplates(context,f.files);
  assert.deepEqual(f.counts(),{rawOpens:2,rawCloses:2,hashes:2});f.assertClean();
});
test('distinct private roots serialize rather than coalesce to the wrong directory',async t=>{
  const f=fixture(t);const other=path.join(f.root,'other-files');fs.mkdirSync(other);
  const module=f.modules(101),context=f.context();
  await Promise.all([module.prepareTemplates(context,f.files),module.prepareTemplates(context,other)]);
  verifyPublished(f);assert.deepEqual(JSON.parse(fs.readFileSync(path.join(other,'export_templates/current.json'))),{schemaVersion:1,sha256:f.manifest.sha256});
  assert.deepEqual(f.counts(),{rawOpens:4,rawCloses:4,hashes:4});
});
