// Godot Engine contributors. SPDX-License-Identifier: MIT
// Transpile the real Templates.ets with an existing SDK compiler. Verify the
// startup compatibility entry point leaves resources and manual templates alone.
// No network/HAP/GUI/signing; all filesystem fixtures live in private TMPDIR.
'use strict';
const assert = require('node:assert/strict');
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

function load() {
  const result = ts.transpileModule(fs.readFileSync(path.join(runtime, 'Templates.ets'), 'utf8'), {
    fileName: 'Templates.ts', reportDiagnostics: true,
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
  });
  const errors = (result.diagnostics || []).filter(item => item.category === ts.DiagnosticCategory.Error);
  assert.equal(errors.length, 0, errors.map(item => ts.flattenDiagnosticMessageText(item.messageText, '\n')).join('\n'));
  const sandbox = {
    exports: {}, Promise,
    require: id => {
      assert.equal(id, '@kit.AbilityKit', `Startup must not depend on template resource/file APIs: ${id}`);
      return {};
    },
  };
  vm.runInNewContext(result.outputText, sandbox, { filename: 'Templates.ets' });
  return sandbox.exports;
}

function fixture(t) {
  assert.ok(process.env.TMPDIR && fs.statSync(process.env.TMPDIR).isDirectory(), 'TMPDIR must exist');
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'godot-templates-test-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const files = path.join(root, 'files');
  fs.mkdirSync(files);
  const parent = path.join(files, 'export_templates');
  fs.mkdirSync(parent);
  // An old cached installation may even have invalid metadata. Startup must
  // ignore it, preserving it and separately downloaded custom templates.
  const pointer = path.join(parent, 'current.json');
  fs.writeFileSync(pointer, 'LEGACY POINTER, NOT VALID JSON');
  const downloads = path.join(root, 'manual downloads with spaces');
  fs.mkdirSync(downloads);
  const originals = new Map([[pointer, fs.readFileSync(pointer)]]);
  for (const name of NAMES) {
    for (const directory of [parent, downloads]) {
      const filename = path.join(directory, name);
      fs.writeFileSync(filename, Buffer.from(`SYNTHETIC MANUAL ZIP ${name}\0`));
      originals.set(filename, fs.readFileSync(filename));
    }
  }
  return { files, parent, downloads, originals };
}

for (const scenario of ['absent resources', 'legacy bundled resources', 'resource manager I/O failure']) {
  test(`startup ignores ${scenario} and preserves manual/legacy templates`, async t => {
    const f = fixture(t);
    const events = [];
    const context = { resourceManager: {
      getRawFileContent: async name => {
        events.push(name);
        if (scenario === 'absent resources') { throw Object.assign(new Error('absent'), { code: 9001005 }); }
        if (scenario === 'resource manager I/O failure') { throw new Error('resource manager unavailable'); }
        return Buffer.from('{"schemaVersion":1,"files":[]}');
      },
      getRawFd: async name => { events.push(name); throw new Error('No archive should be opened'); },
    } };
    const module = load();
    await module.prepareTemplates(context, f.files);
    await module.prepareTemplates(context, f.files);
    assert.deepEqual(events, [], 'Template availability must not affect Editor startup');
    for (const [filename, bytes] of f.originals) { assert.deepEqual(fs.readFileSync(filename), bytes, filename); }
    assert.deepEqual(fs.readdirSync(f.parent).sort(), [...NAMES, 'current.json'].sort());
    assert.deepEqual(fs.readdirSync(f.files), ['export_templates']);
    assert.deepEqual(fs.readdirSync(f.downloads).sort(), [...NAMES].sort());
  });
}

test('concurrent startup never inspects a context or creates a template root', async t => {
  const f = fixture(t);
  const absent = path.join(f.files, 'not created');
  const context = new Proxy({}, { get: () => assert.fail('Template startup must not inspect the application context') });
  const module = load();
  await Promise.all([
    module.prepareTemplates(context, absent),
    module.prepareTemplates(context, f.files),
    module.prepareTemplates(context, absent),
  ]);
  assert.equal(fs.existsSync(absent), false);
  for (const [filename, bytes] of f.originals) { assert.deepEqual(fs.readFileSync(filename), bytes, filename); }
});
