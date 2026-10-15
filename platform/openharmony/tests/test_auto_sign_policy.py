#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Exercise the real run signing policy with native Editor settings/presets.

Compile only the fixture and bridge against an existing signed Editor library.
The policy helpers are the same inline production methods used by run(), so this
can run before the final Editor rebuild. Ordinary public export validation also
runs against the real Editor. No SCons, CLI login, certificate generation, HAP
signing, HDC, installation or publishing. Only the disposable fixture executable
and a temporary copy of the existing Editor library are self-signed to permit
local native execution; the original library and its signature stay intact.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True, help='Native SDK with llvm/bin/clang++ and sysroot')
    parser.add_argument('--library', type=Path, required=True, help='Existing signed Editor libgodot.so')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    sdk = args.sdk.resolve(strict=True)
    source_library = args.library.resolve(strict=True)
    compiler = sdk / 'llvm/bin/clang++'
    signer = shutil.which('binary-sign-tool')
    if not compiler.is_file() or not signer:
        parser.error('Selected native SDK clang++ and PATH binary-sign-tool are required')
    temporary_root = Path(os.environ['TMPDIR']).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='godot-auto-sign-policy-', dir=temporary_root) as temporary:
        root = Path(temporary).resolve(strict=True)
        if root.parent != temporary_root or not root.name.startswith('godot-auto-sign-policy-'):
            raise RuntimeError('Unexpected native policy fixture directory')
        try:
            environment = dict(os.environ)
            for variable in ('HOME', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME',
                             'XDG_STATE_HOME', 'XDG_RUNTIME_DIR', 'GODOT_OHOS_DATA_DIR',
                             'GODOT_OHOS_CACHE_DIR', 'TMPDIR'):
                location = root / variable.lower()
                location.mkdir(mode=0o700)
                environment[variable] = str(location)
            environment['GODOT_AUTO_SIGN_POLICY_ROOT'] = str(root)
            project = root / 'project'
            project.mkdir()
            (project / 'project.godot').write_text(
                'config_version=5\n[application]\nconfig/name="Native automatic signing policy fixture"\n'
                '[rendering]\nrenderer/rendering_method="gl_compatibility"\n')
            # Sign an isolated copy on the native temporary filesystem; preserve
            # the original build output and its signature on HOME/hmdfs.
            library = root / 'libgodot.so'
            staging_library = root / 'libgodot.staging.so'
            subprocess.run([str(sdk / 'llvm/bin/llvm-objcopy'), '--remove-section=.codesign',
                            str(source_library), str(staging_library)],
                           cwd=root, env=environment, check=True, timeout=60)
            subprocess.run([signer, 'sign', '-selfSign', '1', '-inFile', str(staging_library), '-outFile', str(library)],
                           cwd=root, env=environment, check=True, timeout=30)
            unsigned, executable = root / 'fixture.unsigned', root / 'fixture'
            command = [str(compiler), '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
                       '-std=c++17', '-DOPENHARMONY_ENABLED', '-DUNIX_ENABLED', '-DDEBUG_ENABLED',
                       '-DDEBUG_METHODS_ENABLED', '-DTOOLS_ENABLED', '-DTHREADS_ENABLED',
                       '-D__OPEN_HARMONY__', '-DVULKAN_ENABLED', '-DRD_ENABLED', '-pthread',
                       '-I' + str(repo), '-I' + str(repo / 'platform/openharmony'),
                       str(repo / 'platform/openharmony/tests/native_auto_sign_policy.cpp'),
                       str(repo / 'platform/openharmony/bridge_openharmony.cpp'), str(library),
                       '-Wl,--no-code-sign', '-Wl,-rpath,' + str(library.parent), '-o', str(unsigned)]
            subprocess.run(command, cwd=root, env=environment, check=True, timeout=120)
            # Some native linkers reserve .codesign even with --no-code-sign.
            subprocess.run([str(sdk / 'llvm/bin/llvm-objcopy'), '--remove-section=.codesign', str(unsigned)],
                           cwd=root, env=environment, check=True, timeout=30)
            subprocess.run([signer, 'sign', '-selfSign', '1', '-inFile', str(unsigned), '-outFile', str(executable)],
                           cwd=root, env=environment, check=True, timeout=30)
            executable.chmod(0o755)
            result = subprocess.run([str(executable), '--editor', '--headless', '--path', str(project)],
                                    cwd=root, env=environment, text=True, capture_output=True, timeout=120)
            print(result.stdout, end='')
            if result.stderr:
                print(result.stderr, end='')
            if result.returncode or 'AUTO_SIGN_POLICY_PASS failures=0' not in result.stdout:
                raise SystemExit(result.returncode or 1)
        finally:
            # TemporaryDirectory removes only this exact, already checked root.
            if root.parent != temporary_root or root.resolve(strict=True) != Path(temporary).resolve(strict=True):
                raise RuntimeError('Unexpected native policy fixture cleanup target')


if __name__ == '__main__':
    main()
