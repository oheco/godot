#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Native Editor Output acceptance, including real signed SIGSEGV subprocesses.

This links the production editor library and exercises UIAbility create_instance
with a controlled launcher callback. It does not install a HAP or replace any
running editor. ArkUI mount behavior is verified separately by the host tests
and the generated application's CompileArkTS build.
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
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    sdk = args.sdk.resolve(strict=True)
    library = args.library.resolve(strict=True)
    compiler = sdk / 'llvm/bin/clang++'
    signer = shutil.which('binary-sign-tool')
    if not compiler.is_file() or not signer:
        parser.error('Selected native SDK clang++ and PATH binary-sign-tool are required')
    temporary_root = Path(os.environ['TMPDIR']).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='godot child crash ', dir=temporary_root) as temporary:
        root = Path(temporary).resolve(strict=True)
        try:
            project = root / 'project'
            project.mkdir()
            (project / 'project.godot').write_text(
                '[application]\nconfig/name="Child crash acceptance"\n'
                'run/main_scene="res://main.tscn"\n'
                '[rendering]\nrenderer/rendering_method="gl_compatibility"\n')
            (project / 'main.tscn').write_text('[gd_scene format=3]\n[node name="Fixture" type="Node"]\n')
            data = root / 'data'
            data.mkdir()
            (root / 'libgodot.so').symlink_to(library)
            unsigned, executable = root / 'fixture.unsigned', root / 'fixture'
            command = [str(compiler), '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
                       '-std=c++17', '-DOPENHARMONY_ENABLED', '-DUNIX_ENABLED', '-DDEBUG_ENABLED',
                       '-DDEBUG_METHODS_ENABLED', '-DTOOLS_ENABLED', '-DTHREADS_ENABLED',
                       '-D__OPEN_HARMONY__', '-DVULKAN_ENABLED', '-DRD_ENABLED', '-pthread',
                       '-I' + str(repo), '-I' + str(repo / 'platform/openharmony'),
                       str(repo / 'platform/openharmony/tests/native_child_crash.cpp'),
                       str(repo / 'platform/openharmony/bridge_openharmony.cpp'), str(library),
                       '-Wl,-rpath,' + str(root), '-Wl,-rpath,' + str(library.parent), '-o', str(unsigned)]
            subprocess.run(command, cwd=repo, check=True)
            subprocess.run([signer, 'sign', '-selfSign', '1', '-inFile', str(unsigned), '-outFile', str(executable)], check=True)
            environment = dict(os.environ)
            environment.update(GODOT_OHOS_DATA_DIR=str(data), GODOT_OHOS_CACHE_DIR=str(root / 'cache'),
                               GODOT_OHOS_CONFIG_DIR=str(root / 'config'))
            result = subprocess.run([str(executable), '--headless', '--editor', '--quiet',
                                     '--path', str(project), '--log-file', str(root / 'parent.log')],
                                    env=environment, text=True, capture_output=True, timeout=120)
            print(result.stdout, end='')
            if result.stderr:
                print(result.stderr, end='')
            if result.returncode or 'CHILD_CRASH_PASS' not in result.stdout:
                raise SystemExit(result.returncode or 1)
            persisted = (root / 'parent.log').read_text()
            if 'The editor is still running.' not in persisted or 'REAL_GAME_LAST_ERROR_中文' not in persisted:
                raise SystemExit('Quiet-mode crash diagnostics were not persisted in the parent engine log')
            print('PASS quiet-mode parent log preserves the native crash report and final game error')
        finally:
            if root.parent != temporary_root or root.resolve(strict=True) != Path(temporary).resolve(strict=True):
                raise RuntimeError('Unexpected child-crash fixture cleanup target')


if __name__ == '__main__':
    main()
