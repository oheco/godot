#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Check actual LineEdit/TextEdit IME submissions against embedded rendering.

The signed native engine provides real Windows, viewports, fonts and controls;
only the display output is captured. No application, device GUI, clipboard or
system input method is operated. Temporary executables/configuration are isolated.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-sdk', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--baseline', action='store_true', help='Expect and record the old embedded-control offsets')
    parser.add_argument('--compile-controls', action='store_true', help='Compile current production controls instead of using the supplied library implementations')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    sdk = args.native_sdk.resolve()
    library = args.library.resolve()
    compiler = sdk / 'llvm/bin/clang++'
    signer = shutil.which('binary-sign-tool')
    if not compiler.is_file() or not signer or not library.is_file():
        raise SystemExit('Native SDK compiler, signer and signed native engine library are required')
    with tempfile.TemporaryDirectory(prefix='godot-ime-controls-', dir=os.environ['TMPDIR']) as temporary:
        root = Path(temporary).resolve()
        if root.parent != Path(os.environ['TMPDIR']).resolve():
            raise SystemExit('Unexpected temporary fixture parent')
        project = root / 'project'
        project.mkdir()
        (project / 'project.godot').write_text('[application]\nconfig/name="Native IME controls"\nrun/main_scene="res://main.tscn"\n')
        (project / 'main.tscn').write_text('[gd_scene format=3]\n[node name="Fixture" type="Node"]\n')
        unsigned = root / 'probe.unsigned'
        executable = root / 'probe'
        command = [str(compiler), '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
                   '-std=c++17', '-DOPENHARMONY_ENABLED', '-DUNIX_ENABLED', '-DDEBUG_ENABLED',
                   '-DDEBUG_METHODS_ENABLED', '-DTOOLS_ENABLED', '-DTHREADS_ENABLED', '-D__OPEN_HARMONY__',
                   '-DVULKAN_ENABLED', '-DRD_ENABLED', '-pthread', '-I' + str(repo),
                   '-I' + str(repo / 'platform/openharmony'),
                   str(repo / 'platform/openharmony/tests/native_ime_controls.cpp')]
        if args.compile_controls:
            command += [str(repo / 'scene/gui/line_edit.cpp'), str(repo / 'scene/gui/text_edit.cpp')]
        command += [str(library), '-Wl,-rpath,' + str(library.parent), '-o', str(unsigned)]
        subprocess.run(command, cwd=repo, check=True)
        subprocess.run([signer, 'sign', '-selfSign', '1', '-inFile', str(unsigned), '-outFile', str(executable)], check=True)
        environment = dict(os.environ)
        environment.update(GODOT_OHOS_DATA_DIR=str(root / 'data'), GODOT_OHOS_CACHE_DIR=str(root / 'cache'))
        if args.baseline:
            environment['GODOT_EXPECT_ORIGINAL_IME_BUG'] = '1'
        else:
            environment.pop('GODOT_EXPECT_ORIGINAL_IME_BUG', None)
        result = subprocess.run([str(executable), '--display-driver', 'ime-capture', '--rendering-driver', 'dummy',
                                 '--path', str(project)], env=environment, capture_output=True, text=True, timeout=90)
        print(result.stdout)
        print(result.stderr)
        if result.returncode or 'PASS ' not in result.stdout:
            raise SystemExit(result.returncode or 1)
    print('PASS native controls IME regression; installed popup candidate placement remains separate')


if __name__ == '__main__':
    main()
