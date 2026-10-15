#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Run the real native Editor exporter after its debugger falls back to another port.

Compiles only the fixture against an existing signed Editor libgodot.so. All
project generation, settings, native binaries and HOME/XDG state live in TMPDIR.
No SCons, SDK/Hvigor build, library execution from the template, HDC, installation
or application signing occurs. --expect-original-bug requires the positive
control to pass and the fallback export to reject with ERR_INVALID_DATA.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--template', type=Path, required=True,
                        help='Existing current OpenHarmony debug game template zip')
    parser.add_argument('--expect-original-bug', action='store_true')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    sdk = args.sdk.resolve(strict=True)
    library = args.library.resolve(strict=True)
    template = args.template.resolve(strict=True)
    compiler = sdk / 'llvm/bin/clang++'
    signer = shutil.which('binary-sign-tool')
    if not compiler.is_file() or not signer:
        parser.error('Selected native SDK llvm/bin/clang++ and PATH binary-sign-tool are required')
    temporary_root = Path(os.environ['TMPDIR']).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='godot-debug-port-export-', dir=temporary_root) as temporary:
        root = Path(temporary).resolve(strict=True)
        if root.parent != temporary_root or not root.name.startswith('godot-debug-port-export-'):
            raise RuntimeError('Unexpected native fixture directory')
        try:
            project = root / 'project'
            project.mkdir()
            (project / 'project.godot').write_text(
                'config_version=5\n[application]\nconfig/name="Native debug port export fixture"\n'
                'run/main_scene="res://main.tscn"\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n')
            (project / 'main.tscn').write_text('[gd_scene format=3]\n[node name="Fixture" type="Node"]\n')
            output = root / 'exports'
            output.mkdir()
            unsigned, executable = root / 'fixture.unsigned', root / 'fixture'
            command = [str(compiler), '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
                       '-std=c++17', '-DOPENHARMONY_ENABLED', '-DUNIX_ENABLED', '-DDEBUG_ENABLED',
                       '-DDEBUG_METHODS_ENABLED', '-DTOOLS_ENABLED', '-DTHREADS_ENABLED',
                       '-D__OPEN_HARMONY__', '-DVULKAN_ENABLED', '-DRD_ENABLED', '-pthread',
                       '-I' + str(repo), '-I' + str(repo / 'platform/openharmony')]
            if args.expect_original_bug:
                command.append('-DGODOT_EXPECT_ORIGINAL_DEBUG_PORT_BUG')
            command += [str(repo / 'platform/openharmony/tests/native_debug_port_export.cpp'),
                        str(repo / 'platform/openharmony/bridge_openharmony.cpp'), str(library),
                        '-Wl,-rpath,' + str(library.parent), '-o', str(unsigned)]
            environment = dict(os.environ)
            for variable, directory in {
                'HOME': 'home', 'XDG_CONFIG_HOME': 'config', 'XDG_DATA_HOME': 'data',
                'XDG_CACHE_HOME': 'cache', 'XDG_STATE_HOME': 'state', 'XDG_RUNTIME_DIR': 'runtime',
                'GODOT_OHOS_DATA_DIR': 'godot-data', 'GODOT_OHOS_CACHE_DIR': 'godot-cache',
                'TMPDIR': 'tmp',
            }.items():
                location = root / directory
                location.mkdir(mode=0o700)
                environment[variable] = str(location)
            environment.update(GODOT_DEBUG_PORT_TEMPLATE=str(template), GODOT_DEBUG_PORT_OUTPUT=str(output))
            # Isolate compile/sign helpers too; PATH keeps the explicitly selected
            # signer available without reading the real user's configuration.
            subprocess.run(command, cwd=root, env=environment, check=True)
            subprocess.run([signer, 'sign', '-selfSign', '1', '-inFile', str(unsigned), '-outFile', str(executable)],
                           cwd=root, env=environment, check=True)
            result = subprocess.run([str(executable), '--editor', '--headless', '--path', str(project)],
                                    cwd=root, env=environment, text=True, capture_output=True, timeout=120)
            print(result.stdout, end='')
            if result.stderr:
                print(result.stderr, end='')
            if result.returncode or 'DEBUG_PORT_EXPORT_PASS failures=0' not in result.stdout:
                raise SystemExit(result.returncode or 1)
            print('DEBUG_PORT_EXPORT_MODE ' + ('expected-original-bug' if args.expect_original_bug else 'fixed-engine'))
        finally:
            # TemporaryDirectory removes this exact previously checked absolute
            # target only; generated projects and settings are never reused.
            if root.parent != temporary_root or root.resolve(strict=True) != Path(temporary).resolve(strict=True):
                raise RuntimeError('Unexpected native fixture cleanup target')


if __name__ == '__main__':
    main()
