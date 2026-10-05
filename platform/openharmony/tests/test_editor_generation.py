#!/usr/bin/env python3
"""Project-generation integration tests with explicit non-executable ELF fixtures."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

PLATFORM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PLATFORM))
from project_config import HEADERS, HOST_PATH, HOST_ABI_VERSION, read_document, write_document


class EditorGenerationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='godot-generation-test-', dir=os.environ['TMPDIR'])
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.library = self.root / 'libgodot.so'
        header = bytearray(64)
        header[:6] = b'\x7fELF\x02\x01'
        header[18:20] = (183).to_bytes(2, 'little')
        self.library.write_bytes(header)
        write_document(self.root / 'build-info.json', {
            'libgodot.so_sha256': hashlib.sha256(header).hexdigest(),
            'openharmony_host_abi': HOST_ABI_VERSION, 'fixture': True})
        self.config = self.root / 'config.json'
        write_document(self.config, {'managed': {'mode': 'none'}})
        self.output = self.root / 'Generated Editor With Spaces'

    def generate(self, *extra, success=True):
        result = subprocess.run([sys.executable, str(PLATFORM / 'export-editor-project.py'),
                                 '--library', str(self.library), '--config', str(self.config),
                                 '--output', str(self.output), *extra], capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def test_native_only_editor_and_update_migrate_old_layout(self):
        self.generate()
        self.assertEqual(read_document(self.output / HOST_PATH)['managed']['mode'], 'none')
        self.assertFalse((self.output / 'entry/src/main/resources/rawfile/runtime.zip').exists())
        self.assertEqual((self.output / 'entry/libs/arm64-v8a/libgodot.so').read_bytes(), self.library.read_bytes())
        for name in HEADERS:
            self.assertEqual((self.output / 'entry/src/main/cpp/include' / name).read_bytes(), (PLATFORM / name).read_bytes())
        legacy = self.output / 'entry/src/main/ets/editorability/EditorAbility.ets'
        legacy.parent.mkdir(parents=True)
        legacy.write_text('// stale editor-only entry')
        old_header = self.output / 'entry/src/main/cpp/include/editor_bridge_openharmony.h'
        old_header.write_text('// stale ABI')
        raw = self.output / 'entry/src/main/resources/rawfile'
        (raw / 'runtime.zip').write_bytes(b'stale')
        (raw / 'runtime-manifest.json').write_text('{}')
        profile_path = self.output / 'build-profile.json5'
        profile = read_document(profile_path)
        profile['app']['signingConfigs'] = [{'name': 'personal', 'material': {'password': 'test//fixture'}}]
        profile['app']['products'][0]['signingConfig'] = 'personal'
        write_document(profile_path, profile)
        local = self.output / 'local.properties'
        local.write_text('sdk.dir=/keep/this/local/path\n')
        lock = self.output / 'entry/oh-package-lock.json5'
        lock.write_text('{"localResolvedLock":true}\n')
        self.generate('--update')
        self.assertFalse(legacy.exists())
        self.assertFalse(old_header.exists())
        self.assertFalse((raw / 'runtime.zip').exists())
        self.assertFalse((raw / 'runtime-manifest.json').exists())
        self.assertEqual(read_document(profile_path)['app']['signingConfigs'], profile['app']['signingConfigs'])
        self.assertEqual(read_document(profile_path)['app']['products'][0]['signingConfig'], 'personal')
        self.assertEqual(local.read_text(), 'sdk.dir=/keep/this/local/path\n')
        self.assertEqual(lock.read_text(), '{"localResolvedLock":true}\n')
        self.assertTrue((self.output / 'entry/src/main/ets/entryability/EntryAbility.ets').exists())
        self.generate(success=False)

    def template_zip(self, kind, mono=True):
        path = self.root / (kind + '.zip')
        with zipfile.ZipFile(path, 'w') as archive:
            archive.writestr('entry/src/main/resources/rawfile/godot_template.json', json.dumps({
                'hostAbi': HOST_ABI_VERSION, 'monoEnabled': mono,
                'architecture': 'arm64-v8a', 'target': 'template_' + kind}))
            archive.writestr('tools/build.cjs', '// configuration-only fixture, not executable\n')
        return path

    def test_bundled_template_pair_digest_and_explicit_removal(self):
        debug, release = self.template_zip('debug'), self.template_zip('release')
        self.generate('--game-template-debug', str(debug), '--game-template-release', str(release))
        raw = self.output / 'entry/src/main/resources/rawfile'
        manifest = read_document(raw / 'export-templates.json')
        self.assertEqual(manifest['schemaVersion'], 1)
        self.assertEqual(len(manifest['files']), 2)
        for entry in manifest['files']:
            payload = (raw / 'export-templates' / entry['name']).read_bytes()
            self.assertEqual(entry['size'], len(payload))
            self.assertEqual(entry['sha256'], hashlib.sha256(payload).hexdigest())
        identity = hashlib.sha256(json.dumps(manifest['files'], sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        self.assertEqual(manifest['sha256'], identity)
        self.generate('--update')
        self.assertFalse((raw / 'export-templates.json').exists())
        self.assertFalse((raw / 'export-templates').exists())

    def test_incomplete_or_non_mono_templates_do_not_touch_output(self):
        debug, release = self.template_zip('debug'), self.template_zip('release', mono=False)
        self.generate('--game-template-debug', str(debug), success=False)
        self.assertFalse(self.output.exists())
        self.generate('--game-template-debug', str(debug), '--game-template-release', str(release), success=False)
        self.assertFalse(self.output.exists())

    def test_legacy_native_cache_is_rejected_before_update(self):
        self.generate()
        before = {p: p.read_bytes() for p in self.output.rglob('*') if p.is_file()}
        path = self.root / 'build-info.json'
        info = read_document(path)
        info.pop('openharmony_host_abi')
        write_document(path, info)
        result = self.generate('--update', success=False)
        self.assertIn('shared host ABI', result.stderr)
        self.assertEqual(before, {p: p.read_bytes() for p in before})

    def test_invalid_config_fails_before_touching_existing_project(self):
        self.generate()
        before = {p: p.read_bytes() for p in self.output.rglob('*') if p.is_file()}
        write_document(self.config, {'build': {'sdkVersion': '5.1.0(18)'}})
        self.generate('--update', success=False)
        self.assertEqual(before, {p: p.read_bytes() for p in before})

    def test_semantic_profile_error_is_preserved_before_copy(self):
        self.generate()
        profile = read_document(self.output / 'build-profile.json5')
        profile['app']['products'] = None
        write_document(self.output / 'build-profile.json5', profile)
        legacy = self.output / 'entry/src/main/ets/editorability/EditorAbility.ets'
        legacy.parent.mkdir(parents=True)
        legacy.write_text('// must not be deleted by a failed update')
        before = {p: p.read_bytes() for p in self.output.rglob('*') if p.is_file()}
        self.generate('--update', success=False)
        self.assertEqual(before, {p: p.read_bytes() for p in self.output.rglob('*') if p.is_file()})

    def test_personal_signing_cannot_be_archived(self):
        self.generate()
        path = self.output / 'build-profile.json5'
        profile = read_document(path)
        profile['app']['signingConfigs'] = [{'name': 'personal', 'material': {'password': 'fixture'}}]
        write_document(path, profile)
        before = {p: p.read_bytes() for p in self.output.rglob('*') if p.is_file()}
        self.generate('--update', '--archive', str(self.root / 'unsafe.zip'), success=False)
        self.assertFalse((self.root / 'unsafe.zip').exists())
        self.assertEqual(before, {p: p.read_bytes() for p in before})

    def test_unsupported_local_json5_is_preserved_before_copy(self):
        self.generate()
        path = self.output / 'build-profile.json5'
        path.write_text("{app:{signingConfigs:[]}}")
        before = {p: p.read_bytes() for p in self.output.rglob('*') if p.is_file()}
        self.generate('--update', success=False)
        self.assertEqual(before, {p: p.read_bytes() for p in before})


if __name__ == '__main__':
    unittest.main(verbosity=2)
