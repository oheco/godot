#!/usr/bin/env python3
"""Focused native/managed NuGet-config migration tests; no restore or network.

Uses the installed native compiler and .NET compiler only. All fixtures and
compiler outputs live under TMPDIR; production source is compiled unchanged.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
RUNTIME_CPP = ROOT / 'misc/dist/openharmony_template/entry/src/main/cpp/runtime_paths.cpp'
RUNTIME_INCLUDE = RUNTIME_CPP.parent
PROJECT_GENERATOR = ROOT / 'modules/mono/editor/GodotTools/GodotTools.ProjectEditor/ProjectGenerator.cs'
LEGACY = (b'<configuration><packageSources><clear/>'
          b'<add key="godot-bundled" value="%GODOT_NUGET_SOURCE%"/>'
          b'</packageSources></configuration>\n')

NATIVE_PROBE = r'''
#include "runtime_paths.h"
#include <cstdlib>
#include <iostream>
#include <string>
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    const std::string root = argv[1], mode = argv[2];
    setenv("OHECO_ROOT", (root + "/absent-oheco").c_str(), 1);
    setenv("GODOT_OHOS_DOTNET_ROOT", (root + "/absent-sdk").c_str(), 1);
    for (const char *key : {"GODOT_NUGET_SOURCE", "GODOT_SHARP_ROOT", "DOTNET_ROOT", "NUGET_PACKAGES", "DOTNET_CLI_HOME"})
        setenv(key, "managed-none-sentinel", 1);
    std::string message;
    const bool ok = configure_runtime_paths(root + "/files", root + "/cache",
            mode == "none" ? "" : root + "/runtime", mode, message);
    std::cout << "ok=" << ok << '\n' << "message=" << message << '\n';
    for (const char *key : {"GODOT_NUGET_SOURCE", "GODOT_SHARP_ROOT", "DOTNET_ROOT", "NUGET_PACKAGES", "DOTNET_CLI_HOME"})
        std::cout << key << '=' << (getenv(key) ? getenv(key) : "") << '\n';
    return ok ? 0 : 1;
}
'''

MANAGED_PROBE = r'''
using System;
using System.IO;
using System.Reflection;
using GodotTools.ProjectEditor;
namespace GodotTools.Shared {
    // Unused project-generation dependencies. The actual production source and
    // its NuGet writer compile unchanged; this probe invokes only that writer.
    public static class GeneratedGodotNupkgsVersions { public const string GodotNETSdk = "fixture-unused"; }
    public static class IdentifierUtils {
        public static string SanitizeQualifiedIdentifier(string value, bool allowEmptyIdentifiers) => value;
    }
}
public static class Probe {
    public static int Main(string[] args) {
        if (args.Length != 2) return 2;
        AppDomain.CurrentDomain.AssemblyResolve += (sender, eventArgs) => {
            string path = Path.Combine(args[1], new AssemblyName(eventArgs.Name).Name + ".dll");
            return File.Exists(path) ? Assembly.LoadFrom(path) : null;
        };
        typeof(ProjectGenerator).GetMethod("EnsureOpenHarmonyNuGetConfig",
                BindingFlags.NonPublic | BindingFlags.Static).Invoke(null, new object[] { args[0] });
        return 0;
    }
}
'''


def run(command, cwd, env=None):
    result = subprocess.run([str(value) for value in command], cwd=cwd, env=env,
                            capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise AssertionError(f'Fixture compiler/probe failed ({result.returncode}):\n{result.stdout}\n{result.stderr}')
    return result


class ManagedPackageSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        native = Path(os.environ.get('GODOT_OHOS_TEST_NATIVE_SDK',
            '/storage/Users/currentUser/.oheco/packages/ohos-sdk-native/26.0.0.35-Beta')).resolve(strict=True)
        cls.dotnet_root = Path(os.environ.get('GODOT_OHOS_TEST_DOTNET_ROOT',
            '/storage/Users/currentUser/.oheco/packages/dotnet-sdk/10.0.401-ohos.2')).resolve(strict=True)
        cls.dotnet = cls.dotnet_root / 'dotnet'
        if not cls.dotnet.is_file():
            cls.dotnet = cls.dotnet_root / 'bin/dotnet'
        csc = sorted((cls.dotnet_root / 'sdk').glob('*/Roslyn/bincore/csc.dll'))[-1]
        cls.sdk = csc.parents[2]
        refs = sorted((cls.dotnet_root / 'packs/Microsoft.NETCore.App.Ref').glob('*/ref/net10.0'))[-1]
        runtime = sorted((cls.dotnet_root / 'shared/Microsoft.NETCore.App').iterdir())[-1].name
        cls.source_hashes = {path: hashlib.sha256(path.read_bytes()).hexdigest()
                             for path in [RUNTIME_CPP, PROJECT_GENERATOR]}
        temporary = Path(os.environ['TMPDIR']).resolve(strict=True)
        cls.temporary = tempfile.TemporaryDirectory(prefix='managed-package-sources-', dir=temporary)
        cls.work = Path(cls.temporary.name).resolve(strict=True)
        assert cls.work.parent == temporary
        try:
            native_source = cls.work / 'native_probe.cpp'
            native_source.write_text(NATIVE_PROBE)
            cls.native_probe = cls.work / 'native-probe'
            run([native / 'llvm/bin/clang++', '--target=aarch64-linux-ohos',
                 '--sysroot=' + str(native / 'sysroot'), '-std=c++17', '-O0',
                 '-static-libstdc++', '-static-libgcc', '-Wl,--no-code-sign',
                 '-I' + str(RUNTIME_INCLUDE), RUNTIME_CPP, native_source,
                 '-o', cls.work / 'native-probe.unsigned'], cls.work)
            signer = shutil.which('binary-sign-tool')
            if not signer:
                raise RuntimeError('binary-sign-tool must be available on PATH')
            run([signer, 'sign', '-selfSign', '1', '-inFile', cls.work / 'native-probe.unsigned',
                 '-outFile', cls.native_probe], cls.work)
            cls.native_probe.chmod(0o755)  # Executable bit only; no data-permission policy.

            managed_source = cls.work / 'managed_probe.cs'
            managed_source.write_text(MANAGED_PROBE)
            cls.managed_probe = cls.work / 'managed-probe.dll'
            cls.dotnet_env = dict(os.environ, DOTNET_ROOT=str(cls.dotnet_root),
                DOTNET_CLI_HOME=str(cls.work / 'dotnet-cli'), DOTNET_NOLOGO='1',
                DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_GENERATE_ASPNET_CERTIFICATE='false')
            references = sorted(refs.glob('*.dll')) + [cls.sdk / 'Microsoft.Build.dll',
                                                       cls.sdk / 'Microsoft.Build.Framework.dll']
            command = [cls.dotnet, csc, '/nologo', '/noconfig', '/nostdlib+', '/target:exe',
                       '/out:' + str(cls.managed_probe),
                       *['/reference:' + str(path) for path in references],
                       PROJECT_GENERATOR, managed_source]
            run(command, cls.work, cls.dotnet_env)
            (cls.work / 'managed-probe.runtimeconfig.json').write_text(json.dumps({
                'runtimeOptions': {'tfm': 'net10.0', 'framework': {
                    'name': 'Microsoft.NETCore.App', 'version': runtime}}}))
        except BaseException:
            cls.temporary.cleanup()
            raise

    @classmethod
    def tearDownClass(cls):
        for path, expected in cls.source_hashes.items():
            assert hashlib.sha256(path.read_bytes()).hexdigest() == expected
        assert cls.work.parent == Path(os.environ['TMPDIR']).resolve(strict=True)
        cls.temporary.cleanup()

    def native(self, root, mode='sdk'):
        if mode == 'sdk':
            api = root / 'runtime/GodotSharp/Api/Debug/GodotSharp.dll'
            api.parent.mkdir(parents=True, exist_ok=True)
            api.write_bytes(b'fixture: native setup checks presence only')
        result = run([self.native_probe, root, mode], self.work)
        return dict(line.split('=', 1) for line in result.stdout.splitlines())

    def check_sources(self, path):
        sources = ET.fromstring(path.read_bytes()).find('packageSources')
        self.assertIsNotNone(sources.find('clear'))
        self.assertEqual({item.attrib['key']: item.attrib['value'] for item in sources.findall('add')}, {
            'godot-bundled': '%GODOT_NUGET_SOURCE%',
            'nuget.org': 'https://api.nuget.org/v3/index.json'})
        self.assertEqual(sources.find("add[@key='nuget.org']").attrib['protocolVersion'], '3')

    def source_cases(self, kind):
        for name, initial, migrate in [
            ('new', None, True), ('legacy', LEGACY, True),
            ('custom', b'<configuration><packageSources><add key="company" value="https://example.invalid/v3/index.json"/></packageSources></configuration>\n', False),
            ('edited', LEGACY + b'<!-- user edit -->\n', False),
            ('bom', b'\xef\xbb\xbf' + LEGACY, False),
            ('crlf', LEGACY.replace(b'\n', b'\r\n'), False),
            ('symlink', LEGACY, False),
        ]:
            with self.subTest(implementation=kind, case=name):
                root = self.work / (kind + '-' + name)
                directory = root / 'files/Projects' if kind == 'native' else root / 'project'
                directory.mkdir(parents=True)
                config = directory / 'NuGet.Config'
                if initial is not None:
                    if name == 'symlink':
                        target = root / 'user-config'; target.write_bytes(initial); config.symlink_to(target)
                    else:
                        config.write_bytes(initial)
                    before = (config.read_bytes(), config.stat().st_mtime_ns)
                if kind == 'native':
                    environment = self.native(root)
                    self.assertEqual(environment['GODOT_NUGET_SOURCE'], str(root / 'runtime/GodotSharp/Tools/nupkgs'))
                    self.assertFalse((directory / 'CSharpSmoke-4.7.2-ohos.3').exists())
                else:
                    run([self.dotnet, self.managed_probe, directory, self.sdk], self.work, self.dotnet_env)
                if migrate:
                    self.check_sources(config)
                else:
                    self.assertEqual((config.read_bytes(), config.stat().st_mtime_ns), before)
                    if name == 'symlink':
                        self.assertTrue(config.is_symlink())

    def test_native_sources_and_exact_legacy_migration(self):
        self.source_cases('native')

    def test_managed_project_sources_and_exact_legacy_migration(self):
        self.source_cases('managed')

    def test_existing_sample_is_preserved_without_auto_population(self):
        root = self.work / 'existing-sample'
        sample = root / 'files/Projects/CSharpSmoke-4.7.2-ohos.3/user-file.txt'
        sample.parent.mkdir(parents=True)
        sample.write_bytes(b'user-owned existing example')
        before = (sample.read_bytes(), sample.stat().st_mtime_ns)
        self.native(root)
        self.assertEqual((sample.read_bytes(), sample.stat().st_mtime_ns), before)
        self.assertFalse((root / 'runtime/Examples').exists())

    def test_managed_none_does_not_touch_sources_or_managed_environment(self):
        root = self.work / 'managed-none'; root.mkdir()
        environment = self.native(root, 'none')
        self.assertEqual(environment['ok'], '1')
        for key in ['GODOT_NUGET_SOURCE', 'GODOT_SHARP_ROOT', 'DOTNET_ROOT', 'NUGET_PACKAGES', 'DOTNET_CLI_HOME']:
            self.assertEqual(environment[key], 'managed-none-sentinel')
        self.assertFalse((root / 'files/Projects').exists())
        self.assertFalse((root / 'runtime').exists())


if __name__ == '__main__':
    unittest.main()
