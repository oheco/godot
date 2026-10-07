#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Run actual OHOS mouse conversion against real embedded PopupMenu input.

The fixture links a signed native Editor engine and compiles the production
bridge. It uses real Input/Viewport/Button/PopupMenu classes under the headless
renderer; it does not operate an installed application or test clipboard APIs.
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
    parser.add_argument('--baseline-ref', help='Optionally reproduce the original bridge from this Git commit')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    fixture = Path(__file__).resolve().parent / 'native_popup_mouse_fixture/check.cpp'
    library = args.library.resolve()
    sdk = args.sdk.resolve()
    compiler = sdk / 'llvm/bin/clang++'
    signer = shutil.which('binary-sign-tool')
    if not compiler.is_file() or not signer or not library.is_file():
        raise SystemExit('Native SDK compiler, binary-sign-tool and signed engine library are required')
    with tempfile.TemporaryDirectory(prefix='godot-popup-motion-', dir=os.environ['TMPDIR']) as temporary:
        root = Path(temporary)
        project = root / 'project'
        project.mkdir()
        (project / 'project.godot').write_text(
            '[application]\nconfig/name="Native popup mouse fixture"\nrun/main_scene="res://main.tscn"\n')
        (project / 'main.tscn').write_text('[gd_scene format=3]\n[node name="Fixture" type="Node"]\n')
        environment = dict(os.environ)
        environment.update(GODOT_OHOS_DATA_DIR=str(root / 'data'), GODOT_OHOS_CACHE_DIR=str(root / 'cache'))
        sources = [('fixed', repo / 'platform/openharmony/bridge_openharmony.cpp')]
        if args.baseline_ref:
            original = subprocess.run(['git', 'show', args.baseline_ref + ':platform/openharmony/bridge_openharmony.cpp'],
                                      cwd=repo, check=True, text=True, capture_output=True).stdout
            baseline = root / 'bridge-original.cpp'
            baseline.write_text(original)
            sources.insert(0, ('original', baseline))
        for mode, bridge in sources:
            unsigned = root / (mode + '.unsigned')
            executable = root / mode
            command = [str(compiler), '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
                       '-std=c++17', '-DOPENHARMONY_ENABLED', '-DUNIX_ENABLED', '-DDEBUG_ENABLED',
                       '-DDEBUG_METHODS_ENABLED', '-DTOOLS_ENABLED', '-DTHREADS_ENABLED',
                       '-D__OPEN_HARMONY__', '-DVULKAN_ENABLED', '-DRD_ENABLED',
                       '-pthread', '-I' + str(repo),
                       '-I' + str(repo / 'platform/openharmony'), str(fixture), str(bridge), str(library),
                       '-Wl,-rpath,' + str(library.parent), '-o', str(unsigned)]
            subprocess.run(command, cwd=repo, check=True)
            subprocess.run([signer, 'sign', '-selfSign', '1', '-inFile', str(unsigned), '-outFile', str(executable)], check=True)
            child_environment = dict(environment)
            if mode == 'original':
                child_environment['GODOT_EXPECT_ORIGINAL_HOVER_BUG'] = '1'
            else:
                child_environment.pop('GODOT_EXPECT_ORIGINAL_HOVER_BUG', None)
            result = subprocess.run([str(executable), '--headless', '--path', str(project)],
                                    env=child_environment, text=True, capture_output=True, timeout=90)
            print(result.stdout)
            if result.stderr:
                print(result.stderr)
            if result.returncode or 'PASS ' not in result.stdout:
                raise SystemExit(result.returncode or 1)
    print('PASS native popup mouse-motion regression; installed-device hover acceptance remains separate')


if __name__ == '__main__':
    main()
