#!/usr/bin/env python3
"""Focused native fixture; no SDK host, HAP, network or device is executed.

Run once after source freeze:
  GODOT_TEST_NATIVE_SDK=/path/to/native python3 platform/openharmony/tests/test_development_kit_discovery.py
The signed fixture and every synthetic installation live under the caller TMPDIR.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[3]
CPP = SOURCE / 'misc/dist/openharmony_template/entry/src/main/cpp'
VERSION = '4.7.2-ohos.3'
PACKAGES = ('Godot.NET.Sdk', 'Godot.SourceGenerators', 'GodotSharp', 'GodotSharpEditor')
KEYS = ('GODOT_SHARP_ROOT', 'GODOT_NUGET_SOURCE', 'DOTNET_ROOT', 'DOTNET_ROOT_ARM64',
        'GODOT_OHOS_DATA_DIR', 'GODOT_OHOS_CACHE_DIR', 'NUGET_PACKAGES', 'DOTNET_CLI_HOME',
        'GODOT_OHOS_DEVKIT_ROOT', 'GODOT_OHOS_DOTNET_ROOT')
DRIVER = r'''
#include "runtime_paths.h"
#include <cstdlib>
#include <iostream>
int main(int argc, char **argv) {
  if (argc < 2) return 2;
  const std::string command = argv[1]; std::string notice; bool ok = true;
  if (command == "list") {
    for (const auto &item : development_kit_candidates()) std::cout << item.root << '\t' << item.manifest.size() << '\n';
    return 0;
  }
  if (command == "publish" && argc == 4) ok = publish_development_kit_templates(argv[2], argv[3], notice);
  else if (command == "configure" && argc == 7) ok = configure_runtime_paths(argv[2], argv[3], argv[4], argv[5], notice, argv[6]);
  else if (command == "cli" && argc == 3) ok = configure_cli_development_kit(argv[2], notice);
  else return 3;
  std::cout << "ok=" << ok << "\nnotice=" << notice << '\n';
  for (const char *key : {"GODOT_SHARP_ROOT", "GODOT_NUGET_SOURCE", "DOTNET_ROOT", "DOTNET_ROOT_ARM64", "GODOT_OHOS_DATA_DIR", "GODOT_OHOS_CACHE_DIR", "NUGET_PACKAGES", "DOTNET_CLI_HOME", "TMPDIR", "HOME"}) {
    const char *value = getenv(key); if (value) std::cout << key << '=' << value << '\n';
  }
  return 0;
}
'''


class DevelopmentKitDiscovery(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = Path(os.environ['TMPDIR']).resolve(strict=True)
        cls.owner = tempfile.TemporaryDirectory(prefix='godot-devkit-discovery-', dir=cls.temp)
        cls.work = Path(cls.owner.name).resolve(strict=True)
        assert cls.work.parent == cls.temp
        sdk = Path(os.environ.get('GODOT_TEST_NATIVE_SDK', '/storage/Users/currentUser/.oheco/packages/ohos-sdk-native/26.0.0.35-Beta')).resolve(strict=True)
        compiler = sdk / 'llvm/bin/clang++'
        signer = shutil.which('binary-sign-tool')
        if not compiler.is_file() or not signer:
            cls.owner.cleanup()
            raise RuntimeError('Selected native SDK clang++ and PATH binary-sign-tool are required')
        driver = cls.work / 'probe.cpp'
        driver.write_text(DRIVER)
        unsigned = cls.work / 'probe.unsigned'
        cls.probe = cls.work / 'probe'
        commands = [
            [str(compiler), '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
             '-std=c++17', '-static-libstdc++', '-I' + str(CPP), str(driver), str(CPP / 'runtime_paths.cpp'),
             '-ldl', '-o', str(unsigned)],
            [signer, 'sign', '-inFile', str(unsigned), '-outFile', str(cls.probe), '-selfSign', '1'],
        ]
        try:
            for command in commands:
                subprocess.run(command, check=True, capture_output=True, text=True)
            cls.probe.chmod(0o755)
        except BaseException:
            cls.owner.cleanup()
            raise

    @classmethod
    def tearDownClass(cls):
        assert cls.work.resolve(strict=True).parent == cls.temp
        assert cls.work.name.startswith('godot-devkit-discovery-')
        cls.owner.cleanup()

    def setUp(self):
        self.root = self.work / self._testMethodName
        self.root.mkdir()
        self.env = {key: value for key, value in os.environ.items() if key not in KEYS}
        self.env.update(OHECO_ROOT=str(self.root / 'oo'), XDG_CONFIG_HOME=str(self.root / 'config'),
                        XDG_CACHE_HOME=str(self.root / 'cache'), TMPDIR=str(self.temp))

    def run_probe(self, *arguments, env=None):
        output = subprocess.run([str(self.probe), *map(str, arguments)], env=env or self.env,
                                capture_output=True, text=True, check=True)
        return dict(line.split('=', 1) for line in output.stdout.splitlines())

    def kit(self, root, token='a'):
        (root / 'bin').mkdir(parents=True)
        (root / 'bin/godot').write_text('fixture executable marker\n')
        (root / 'bin/libgodot.so').write_text('fixture library marker\n')
        managed = root / 'share/godot/GodotSharp'
        (managed / 'Api/Debug').mkdir(parents=True)
        (managed / 'Api/Debug/GodotSharp.dll').write_text('fixture API marker\n')
        (managed / 'Tools/nupkgs').mkdir(parents=True)
        for name in PACKAGES:
            (managed / 'Tools/nupkgs' / f'{name}.{VERSION}.nupkg').write_text(name)
        templates = root / 'share/godot/templates'
        templates.mkdir()
        for name in ('openharmony_debug_arm64-v8a.zip', 'openharmony_release_arm64-v8a.zip'):
            (templates / name).write_text('fixture ZIP marker ' + token)
        (templates / '.ready').write_text(token * 64 + '\n')
        (root / 'godot-devkit.json').write_text(json.dumps({
            'schemaVersion': 1, 'engineVersion': '4.7.2', 'hostAbiVersion': 4,
            'managedVersion': VERSION, 'architecture': 'arm64-v8a',
            'engineLibrarySha256': hashlib.sha256((root / 'bin/libgodot.so').read_bytes()).hexdigest(),
            'managedRoot': 'share/godot', 'templatesRoot': 'share/godot/templates',
            'templateSetSha256': token * 64,
        }))
        return root

    def candidates(self, env=None):
        output = subprocess.run([str(self.probe), 'list'], env=env or self.env,
                                check=True, capture_output=True, text=True).stdout
        return [(Path(line.rsplit('\t', 1)[0]), int(line.rsplit('\t', 1)[1])) for line in output.splitlines()]

    def test_active_version_then_fallback_and_explicit_selection(self):
        versions = Path(self.env['OHECO_ROOT']) / 'packages/godot-editor'
        active = self.kit(versions / '4.7.2-ohos.1')
        later = self.kit(versions / '4.7.2-ohos.2')
        latest = self.kit(versions / '4.7.2-ohos.10')
        (Path(self.env['OHECO_ROOT']) / 'bin').mkdir()
        (Path(self.env['OHECO_ROOT']) / 'bin/godot').symlink_to(active / 'bin/godot')
        self.assertEqual([p for p, _ in self.candidates()], [active, latest, later])
        chosen = dict(self.env, GODOT_OHOS_DEVKIT_ROOT=str(self.root / 'missing explicit kit'))
        self.assertEqual(self.candidates(chosen), [(self.root / 'missing explicit kit', 0)])
        chosen['GODOT_OHOS_DEVKIT_ROOT'] = str(later)
        self.assertEqual([p for p, _ in self.candidates(chosen)], [later])

    def test_manifest_read_is_bounded(self):
        kit = self.kit(self.root / 'kit')
        (kit / 'godot-devkit.json').write_text('x' * 65537)
        self.assertEqual(self.candidates(dict(self.env, GODOT_OHOS_DEVKIT_ROOT=str(kit))), [(kit, 0)])

    def test_template_pointer_links_and_updates_only_own_pointer(self):
        a = self.kit(self.root / 'kit a', 'a')
        b = self.kit(self.root / 'kit b', 'b')
        files = self.root / 'data'
        self.assertEqual(self.run_probe('publish', files, a / 'share/godot')['ok'], '1')
        link = files / 'export_templates' / ('a' * 64)
        self.assertTrue(link.is_symlink())
        self.assertEqual(link.resolve(), a / 'share/godot/templates')
        self.assertEqual(self.run_probe('publish', files, b / 'share/godot')['ok'], '1')
        pointer = files / 'export_templates/current.json'
        self.assertEqual(json.loads(pointer.read_text()), {'schemaVersion': 1, 'sha256': 'b' * 64, 'source': 'oheco-devkit'})
        self.assertTrue(link.is_symlink())
        custom = '{"schemaVersion":1,"sha256":"' + 'a' * 64 + '","source":"manual"}\n'
        pointer.write_text(custom)
        self.assertEqual(self.run_probe('publish', files, a / 'share/godot')['ok'], '1')
        self.assertEqual(pointer.read_text(), custom)
        pointer.write_text(json.dumps({'schemaVersion': 1, 'sha256': 'a' * 64, 'source': 'oheco-devkit'}, indent=2))
        preserved = pointer.read_bytes()
        self.run_probe('publish', files, b / 'share/godot')
        self.assertEqual(pointer.read_bytes(), preserved)

    def test_existing_wrong_link_and_real_directory_are_not_replaced(self):
        kit = self.kit(self.root / 'kit')
        files = self.root / 'data'
        link = files / 'export_templates' / ('a' * 64)
        link.mkdir(parents=True)
        (link / 'user-data').write_text('preserved')
        self.assertEqual(self.run_probe('publish', files, kit / 'share/godot')['ok'], '0')
        self.assertEqual((link / 'user-data').read_text(), 'preserved')
        other = self.root / 'other'
        other.mkdir()
        files2 = self.root / 'data2'
        (files2 / 'export_templates').mkdir(parents=True)
        wrong = files2 / 'export_templates' / ('a' * 64)
        wrong.symlink_to(other, target_is_directory=True)
        self.assertEqual(self.run_probe('publish', files2, kit / 'share/godot')['ok'], '0')
        self.assertEqual(wrong.resolve(), other)

    def test_none_mode_and_default_sdk_missing(self):
        files, cache = self.root / 'data', self.root / 'cache'
        result = self.run_probe('configure', files, cache, '', 'none', self.temp)
        self.assertEqual(result['ok'], '1')
        self.assertNotIn('GODOT_SHARP_ROOT', result)
        self.assertNotIn('NUGET_PACKAGES', result)
        self.assertFalse((files / 'Projects').exists())
        kit = self.kit(self.root / 'kit')
        result = self.run_probe('configure', files, cache, kit / 'share/godot', 'sdk', self.temp)
        self.assertEqual(result['ok'], '1')
        self.assertIn('No installed .NET SDK', result['notice'])
        self.assertNotIn('DOTNET_ROOT', result)
        self.assertEqual(result['TMPDIR'], str(self.temp))
        self.assertEqual(result['GODOT_NUGET_SOURCE'], str(kit / 'share/godot/GodotSharp/Tools/nupkgs'))
        self.assertIn('https://api.nuget.org/v3/index.json', (files / 'Projects/NuGet.Config').read_text())

    def test_cli_self_location_and_caller_overrides(self):
        kit = self.kit(self.root / 'kit with spaces')
        result = self.run_probe('cli', kit / 'bin/godot')
        self.assertEqual(result['ok'], '1')
        self.assertEqual(result['GODOT_SHARP_ROOT'], str(kit / 'share/godot/GodotSharp'))
        self.assertEqual(result['GODOT_OHOS_DATA_DIR'], self.env['XDG_CONFIG_HOME'] + '/godot-editor')
        self.assertEqual(result['GODOT_OHOS_CACHE_DIR'], self.env['XDG_CACHE_HOME'] + '/godot-editor')
        custom = dict(self.env, GODOT_SHARP_ROOT='/explicit/managed', GODOT_NUGET_SOURCE='/explicit/source',
                      GODOT_OHOS_DATA_DIR=str(self.root / 'explicit-data'), GODOT_OHOS_CACHE_DIR='/explicit/cache',
                      DOTNET_ROOT='/explicit/sdk', NUGET_PACKAGES='/explicit/nuget', DOTNET_CLI_HOME='/explicit/cli')
        result = self.run_probe('cli', kit / 'bin/godot', env=custom)
        for key in ('GODOT_SHARP_ROOT', 'GODOT_NUGET_SOURCE', 'GODOT_OHOS_DATA_DIR', 'GODOT_OHOS_CACHE_DIR',
                    'DOTNET_ROOT', 'NUGET_PACKAGES', 'DOTNET_CLI_HOME'):
            self.assertEqual(result[key], custom[key])
        self.assertEqual(result['HOME'], self.env['HOME'])
        self.assertEqual(result['TMPDIR'], self.env['TMPDIR'])
        legacy = self.run_probe('cli', self.root / 'legacy-launcher')
        self.assertNotIn('GODOT_SHARP_ROOT', legacy)
        self.assertNotIn('GODOT_OHOS_DATA_DIR', legacy)


if __name__ == '__main__':
    unittest.main(verbosity=2)
