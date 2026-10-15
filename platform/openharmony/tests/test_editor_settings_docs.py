#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Verify real EditorHelp tooltips for exporter-registered EditorSettings.

Compiles a signed native fixture and starts the real headless editor. The old
engine can be checked with --expect-missing-docs to prove the XML merge defect.
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
    parser.add_argument('--expect-missing-docs', action='store_true')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    sdk = args.sdk.resolve(strict=True)
    library = args.library.resolve(strict=True)
    compiler = sdk / 'llvm/bin/clang++'
    signer = shutil.which('binary-sign-tool')
    if not compiler.is_file() or not signer:
        parser.error('Selected native SDK clang++ and PATH binary-sign-tool are required')
    temporary_root = Path(os.environ['TMPDIR']).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='godot-settings-docs-', dir=temporary_root) as temporary:
        root = Path(temporary).resolve(strict=True)
        try:
            project = root / 'project with spaces'
            project.mkdir()
            (project / 'project.godot').write_text('config_version=5\n[application]\nconfig/name="Settings documentation fixture"\n')
            unsigned, executable = root / 'fixture.unsigned', root / 'fixture'
            command = [str(compiler), '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
                       '-std=c++17', '-DOPENHARMONY_ENABLED', '-DUNIX_ENABLED', '-DDEBUG_ENABLED',
                       '-DDEBUG_METHODS_ENABLED', '-DTOOLS_ENABLED', '-DTHREADS_ENABLED',
                       '-D__OPEN_HARMONY__', '-DVULKAN_ENABLED', '-DRD_ENABLED', '-pthread',
                       '-I' + str(repo), '-I' + str(repo / 'platform/openharmony'),
                       str(repo / 'platform/openharmony/tests/native_editor_settings_docs.cpp'),
                       str(repo / 'platform/openharmony/bridge_openharmony.cpp'), str(library),
                       '-Wl,-rpath,' + str(library.parent), '-o', str(unsigned)]
            subprocess.run(command, cwd=repo, check=True)
            subprocess.run([signer, 'sign', '-selfSign', '1', '-inFile', str(unsigned), '-outFile', str(executable)], check=True)
            environment = dict(os.environ)
            for name in ('HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'GODOT_OHOS_DATA_DIR', 'GODOT_OHOS_CACHE_DIR'):
                directory = root / name.lower()
                directory.mkdir()
                environment[name] = str(directory)
            result = subprocess.run([str(executable), '--headless', '--editor', '--path', str(project)],
                                    env=environment, text=True, capture_output=True, timeout=120)
            print(result.stdout, end='')
            if result.stderr:
                print(result.stderr, end='')
            if args.expect_missing_docs:
                if (result.returncode != 1 or 'PASS core EditorSettings tooltip documentation' not in result.stdout
                        or 'FAIL registered setting and nonempty native tooltip: hdc_path' not in result.stdout):
                    raise SystemExit('The old engine did not demonstrate the expected missing exporter tooltip')
            elif result.returncode or 'EDITOR_SETTINGS_DOCS_PASS failures=0' not in result.stdout:
                raise SystemExit(result.returncode or 1)
        finally:
            if root.parent != temporary_root or root.resolve(strict=True) != Path(temporary).resolve(strict=True):
                raise RuntimeError('Unexpected native fixture cleanup target')


if __name__ == '__main__':
    main()
