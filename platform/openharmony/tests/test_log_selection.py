#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Exercise real OHOS mouse/touch input and log selection on a hybrid display.

Only the display/surface is headless; Input, Viewport, RichTextLabel, timer and
ScrollBar are the actual signed native engine. --expect-original-bug proves the
fixture catches the old competing mouse-pan and selection-scroll paths.
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
    parser.add_argument('--expect-original-bug', action='store_true')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    sdk = args.sdk.resolve(strict=True)
    library = args.library.resolve(strict=True)
    compiler = sdk / 'llvm/bin/clang++'
    signer = shutil.which('binary-sign-tool')
    if not compiler.is_file() or not signer:
        parser.error('Selected native SDK clang++ and PATH binary-sign-tool are required')
    temporary_root = Path(os.environ['TMPDIR']).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='godot-log-selection-', dir=temporary_root) as temporary:
        root = Path(temporary).resolve(strict=True)
        try:
            project = root / 'project'
            project.mkdir()
            (project / 'project.godot').write_text(
                '[application]\nconfig/name="Native log selection fixture"\nrun/main_scene="res://main.tscn"\n')
            (project / 'main.tscn').write_text('[gd_scene format=3]\n[node name="Fixture" type="Node"]\n')
            unsigned, executable = root / 'fixture.unsigned', root / 'fixture'
            command = [str(compiler), '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
                       '-std=c++17', '-DOPENHARMONY_ENABLED', '-DUNIX_ENABLED', '-DDEBUG_ENABLED',
                       '-DDEBUG_METHODS_ENABLED', '-DTOOLS_ENABLED', '-DTHREADS_ENABLED',
                       '-D__OPEN_HARMONY__', '-DVULKAN_ENABLED', '-DRD_ENABLED', '-pthread',
                       '-I' + str(repo), '-I' + str(repo / 'platform/openharmony'),
                       str(repo / 'platform/openharmony/tests/native_log_selection.cpp'),
                       str(repo / 'platform/openharmony/bridge_openharmony.cpp'), str(library),
                       '-Wl,-rpath,' + str(library.parent), '-o', str(unsigned)]
            subprocess.run(command, cwd=repo, check=True)
            subprocess.run([signer, 'sign', '-selfSign', '1', '-inFile', str(unsigned), '-outFile', str(executable)], check=True)
            environment = dict(os.environ)
            environment.update(GODOT_OHOS_DATA_DIR=str(root / 'data'), GODOT_OHOS_CACHE_DIR=str(root / 'cache'))
            if args.expect_original_bug:
                environment['GODOT_EXPECT_LOG_DRAG_BUG'] = '1'
            else:
                environment.pop('GODOT_EXPECT_LOG_DRAG_BUG', None)
            result = subprocess.run([str(executable), '--display-driver', 'hybrid-fixture', '--path', str(project)],
                                    env=environment, text=True, capture_output=True, timeout=60)
            print(result.stdout, end='')
            if result.stderr:
                print(result.stderr, end='')
            if result.returncode or 'LOG_SELECTION_PASS' not in result.stdout:
                raise SystemExit(result.returncode or 1)
        finally:
            if root.parent != temporary_root or root.resolve(strict=True) != Path(temporary).resolve(strict=True):
                raise RuntimeError('Unexpected native fixture cleanup target')


if __name__ == '__main__':
    main()
