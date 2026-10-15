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
from sdk_fixture import make_sdk


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
        product = read_document(self.output / 'build-profile.json5')['app']['products'][0]
        self.assertEqual(product['compatibleSdkVersion'], '26.0.0')
        self.assertEqual(product['runtimeOS'], 'OpenHarmony')
        permissions = {item['name']: item for item in read_document(self.output / 'entry/src/main/module.json5')['module']['requestPermissions']}
        self.assertEqual(permissions['ohos.permission.READ_PASTEBOARD']['reason'], '$string:reason_pasteboard')
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
        profile['app']['products'][0]['runtimeOS'] = 'HarmonyOS'
        profile['app']['products'][0]['compatibleSdkVersion'] = '7.0.0(26)'
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
        self.assertEqual(read_document(profile_path)['app']['products'][0]['runtimeOS'], 'OpenHarmony')
        self.assertEqual(read_document(profile_path)['app']['products'][0]['compatibleSdkVersion'], '26.0.0')
        self.assertEqual(local.read_text(), 'sdk.dir=/keep/this/local/path\n')
        self.assertEqual(lock.read_text(), '{"localResolvedLock":true}\n')
        self.assertTrue((self.output / 'entry/src/main/ets/entryability/EntryAbility.ets').exists())
        self.generate(success=False)

    def test_editor_sdk_selection_uses_harmonyos_metadata(self):
        sdk = make_sdk(self.root / 'sdk with spaces', 'HarmonyOS')
        write_document(self.config, {'managed': {'mode': 'none'}, 'build': {'sdkRoot': str(sdk / 'default')}})
        self.generate()
        product = read_document(self.output / 'build-profile.json5')['app']['products'][0]
        self.assertEqual(product['runtimeOS'], 'HarmonyOS')
        self.assertEqual(product['compatibleSdkVersion'], '26.0.0')
        self.assertEqual(read_document(self.output / 'entry/src/main/module.json5')['module']['deviceTypes'], ['2in1'])

    def template_zip(self, kind):
        path = self.root / f'openharmony_{kind}_arm64-v8a.zip'
        with zipfile.ZipFile(path, 'w') as archive:
            archive.writestr('entry/src/main/resources/rawfile/godot_template.json', json.dumps({
                'hostAbi': HOST_ABI_VERSION, 'monoEnabled': True,
                'architecture': 'arm64-v8a', 'target': 'template_' + kind}))
            archive.writestr('tools/build.cjs', '// configuration-only fixture, not executable\n')
        return path

    def test_editor_and_archive_exclude_separately_downloaded_templates(self):
        downloads = [self.template_zip(kind) for kind in ('debug', 'release')]
        before = {path: path.read_bytes() for path in downloads}
        archive = self.root / 'Editor.zip'
        self.generate('--archive', str(archive))
        raw = self.output / 'entry/src/main/resources/rawfile'
        self.assertFalse((raw / 'export-templates.json').exists())
        self.assertFalse((raw / 'export-templates').exists())
        self.assertIsNone(read_document(self.output / 'build-inputs.json')['export_templates'])
        with zipfile.ZipFile(archive) as packaged:
            self.assertFalse(any('export-templates' in name or
                                 name.endswith(tuple(path.name for path in downloads))
                                 for name in packaged.namelist()))
        self.assertEqual(before, {path: path.read_bytes() for path in downloads})

    def test_update_removes_former_bundled_resources_and_preserves_other_files(self):
        self.generate()
        raw = self.output / 'entry/src/main/resources/rawfile'
        bundled = raw / 'export-templates'
        bundled.mkdir()
        for kind in ('debug', 'release'):
            path = self.template_zip(kind)
            (bundled / path.name).write_bytes(path.read_bytes())
        (raw / 'export-templates.json').write_text('{"oldBundledManifest":true}\n')
        unrelated = raw / 'user-resource.txt'
        unrelated.write_text('Keep unrelated project resources.\n')
        downloads = {path: path.read_bytes() for path in self.root.glob('openharmony_*.zip')}
        archive = self.root / 'Updated Editor.zip'
        self.generate('--update', '--archive', str(archive))
        self.assertFalse((raw / 'export-templates.json').exists())
        self.assertFalse(bundled.exists())
        self.assertEqual(unrelated.read_text(), 'Keep unrelated project resources.\n')
        self.assertEqual(downloads, {path: path.read_bytes() for path in downloads})
        self.assertIsNone(read_document(self.output / 'build-inputs.json')['export_templates'])
        with zipfile.ZipFile(archive) as packaged:
            self.assertFalse(any('export-templates' in name for name in packaged.namelist()))

    def test_removed_bundling_options_fail_before_touching_project(self):
        debug, release = self.template_zip('debug'), self.template_zip('release')
        options = ('--game-template-debug', str(debug), '--game-template-release', str(release))
        result = self.generate(*options, success=False)
        self.assertIn('unrecognized arguments', result.stderr)
        self.assertFalse(self.output.exists())
        self.generate()
        before = {p: p.read_bytes() for p in self.output.rglob('*') if p.is_file()}
        result = self.generate('--update', *options, success=False)
        self.assertIn('unrecognized arguments', result.stderr)
        self.assertEqual(before, {p: p.read_bytes() for p in self.output.rglob('*') if p.is_file()})

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
