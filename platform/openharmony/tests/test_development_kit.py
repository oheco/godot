#!/usr/bin/env python3
"""Verify complete optional-kit layout without an APP or general offline feed."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

PLATFORM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PLATFORM))
from development_kit import GODOT_PACKAGES, MANAGED_VERSION, managed_inputs
from project_config import HOST_ABI_VERSION, read_document, write_document


class DevelopmentKitPackagingTest(unittest.TestCase):
    def test_sdk_provenance_accepts_standard_and_aspnetcore_builds(self):
        for filename in ('BUILDINFO.json', 'BUILDINFO.aspnetcore.json'):
            with self.subTest(filename=filename), tempfile.TemporaryDirectory(prefix='godot-kit-sdk-', dir=os.environ['TMPDIR']) as directory:
                root = Path(directory)
                sharp, sdk = root / 'GodotSharp', root / 'sdk'
                for name in ('Api/Debug/GodotSharp.dll', 'Api/Release/GodotSharp.dll', 'Tools/GodotTools.dll',
                             *('Tools/nupkgs/' + package + '.' + MANAGED_VERSION + '.nupkg' for package in GODOT_PACKAGES)):
                    path = sharp / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(b'input fixture')
                for name in ('sdk', 'host/fxr', 'shared/Microsoft.NETCore.App',
                             'packs/Microsoft.NETCore.App.Ref', 'packs/Microsoft.NETCore.App.Runtime.openharmony-arm64'):
                    (sdk / name).mkdir(parents=True, exist_ok=True)
                (sdk / 'dotnet').write_bytes(b'input fixture')
                write_document(sdk / filename, {'rid': 'openharmony-arm64', 'sdk_version': '10.0.401'})
                packages, provenance = managed_inputs(sharp, sdk)
                self.assertEqual(len(packages), 4)
                self.assertEqual(provenance['sdk_version'], '10.0.401')

    def test_kit_keeps_complete_development_assets_and_only_four_godot_packages(self):
        with tempfile.TemporaryDirectory(prefix='godot-kit-package-test-', dir=os.environ['TMPDIR']) as directory:
            root = Path(directory)
            cli = root / 'signed cli fixtures'
            cli.mkdir()
            header = bytearray(64)
            header[:6] = b'\x7fELF\x02\x01'
            header[18:20] = (183).to_bytes(2, 'little')
            digest = hashlib.sha256(header).hexdigest()
            for name in ('godot', 'libgodot.so'):
                (cli / name).write_bytes(header)
            (cli / 'godot').chmod(0o755)
            write_document(cli / 'build-info.json', {
                'openharmony_host_abi': HOST_ABI_VERSION, 'godot_sha256': digest,
                'libgodot.so_sha256': digest, 'fixture': True})
            sdk = root / 'external sdk'
            sdk.mkdir()
            (sdk / 'dotnet').write_bytes(header)
            write_document(sdk / 'BUILDINFO.json', {'rid': 'openharmony-arm64'})
            for name in ('sdk', 'host/fxr', 'shared/Microsoft.NETCore.App',
                         'packs/Microsoft.NETCore.App.Ref', 'packs/Microsoft.NETCore.App.Runtime.openharmony-arm64'):
                (sdk / name).mkdir(parents=True, exist_ok=True)
            sharp = root / 'GodotSharp'
            for name in ('Api/Debug/GodotSharp.dll', 'Api/Release/GodotSharp.dll',
                         'Tools/GodotTools.dll', 'Tools/Required.Dependency.dll'):
                path = sharp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(('fixture ' + name).encode())
            packages = sharp / 'Tools/nupkgs'
            packages.mkdir()
            own = [f'{name}.{MANAGED_VERSION}.nupkg' for name in GODOT_PACKAGES]
            for name in own:
                (packages / name).write_bytes(('fixture ' + name).encode())
            (packages / 'Unrelated.ThirdParty.1.0.0.nupkg').write_bytes(b'excluded general offline package')
            templates = []
            for kind in ('debug', 'release'):
                path = root / (kind + '.zip')
                with zipfile.ZipFile(path, 'w') as archive:
                    archive.writestr('entry/src/main/resources/rawfile/godot_template.json',
                                     json.dumps({'hostAbi': HOST_ABI_VERSION}))
                    archive.writestr('tools/auto-sign', b'fixture native backend')
                    archive.writestr('entry/libs/arm64-v8a/libgodot.so', header)
                templates.append(path)
            output = root / 'development kit'
            packed = root / 'development kit.tar.gz'
            result = subprocess.run([sys.executable, '-B', str(PLATFORM / 'package-development-kit.py'),
                '--cli', str(cli), '--godotsharp', str(sharp), '--dotnet-sdk', str(sdk),
                '--debug-template', str(templates[0]), '--release-template', str(templates[1]),
                '--output', str(output), '--archive', str(packed)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            manifest = read_document(output / 'godot-devkit.json')
            self.assertEqual(manifest['engineLibrarySha256'], digest)
            self.assertEqual(manifest['hostAbiVersion'], HOST_ABI_VERSION)
            self.assertEqual(manifest['dependencies'][0]['name'], 'dotnet-sdk')
            self.assertEqual((output / 'bin/godot').read_bytes(), header)
            self.assertEqual((output / 'bin/libgodot.so').read_bytes(), header)
            managed = output / 'share/godot'
            self.assertEqual(sorted(path.name for path in managed.rglob('*.nupkg')), sorted(own))
            self.assertEqual((managed / 'GodotSharp/Tools/Required.Dependency.dll').read_bytes(),
                             (sharp / 'Tools/Required.Dependency.dll').read_bytes())
            self.assertFalse((managed / 'nuget').exists())
            self.assertFalse((managed / 'Examples').exists())
            self.assertFalse((output / 'dotnet').exists())
            self.assertEqual((managed / 'templates/.ready').read_text().strip(), manifest['templateSetSha256'])
            for kind, path in zip(('debug', 'release'), templates):
                self.assertEqual((managed / f'templates/openharmony_{kind}_arm64-v8a.zip').read_bytes(), path.read_bytes())
            with tarfile.open(packed) as archive:
                self.assertEqual(archive.extractfile(output.name + '/bin/libgodot.so').read(), header)
                self.assertEqual(archive.getmember(output.name + '/bin/godot').mode & 0o111, 0o111)


if __name__ == '__main__':
    unittest.main(verbosity=2)
