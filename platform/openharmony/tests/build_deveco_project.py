#!/usr/bin/env python3
"""Build an isolated generated project with preinstalled native Hvigor/SDK inputs.

No dependency downloads, signing or installation are performed. By default the
project's API 23 HarmonyOS configuration is preserved. --openharmony-sdk-version
explicitly selects a compatibility build with the available OpenHarmony SDK;
that result is NOT a HarmonyOS API 23 HAP acceptance result.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, required=True)
    parser.add_argument('--node-modules', type=Path, required=True, help='Offline prepared Hvigor dependencies')
    parser.add_argument('--sdk', type=Path, required=True, help='Prepared SDK root for Hvigor')
    parser.add_argument('--node', default=shutil.which('node'))
    parser.add_argument('--output', type=Path, required=True, help='New persistent evidence directory')
    parser.add_argument('--openharmony-sdk-version', help='Explicit alternate SDK build, e.g. 26.0.0')
    parser.add_argument('--arkts-only', action='store_true')
    args = parser.parse_args()
    if sys.platform != 'ohos':
        parser.error('Run with native OpenHarmony Python/Node and SDK tools')
    args.project = args.project.resolve(strict=True)
    args.node_modules = args.node_modules.resolve(strict=True)
    args.sdk = args.sdk.resolve(strict=True)
    hvigor = args.node_modules / '@ohos/hvigor/bin/hvigor.js'
    if not hvigor.is_file() or not args.node:
        parser.error('Native Node and prepared @ohos/hvigor are required')
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from project_config import read_document, write_document
    with tempfile.TemporaryDirectory(prefix='godot-hvigor-test-', dir=os.environ['TMPDIR']) as temporary:
        root = Path(temporary)
        project = root / 'project'
        shutil.copytree(args.project, project, ignore=shutil.ignore_patterns(
            'node_modules', 'oh_modules', '.hvigor', '.cxx', 'build', '.git', '.bitfun', '.idea', '.godot-config-history',
            '*.p12', '*.p7b', '*.cer', 'local.properties'))
        profile = read_document(project / 'build-profile.json5')
        profile['app']['signingConfigs'] = []
        for product in profile['app']['products']:
            product.pop('signingConfig', None)
            if args.openharmony_sdk_version:
                product.update(compileSdkVersion=args.openharmony_sdk_version,
                               targetSdkVersion=args.openharmony_sdk_version,
                               compatibleSdkVersion=23, runtimeOS='OpenHarmony')
        write_document(project / 'build-profile.json5', profile)
        module_path = project / 'entry/src/main/module.json5'
        module = read_document(module_path)
        original_devices = list(module['module']['deviceTypes'])
        if args.openharmony_sdk_version:
            # HarmonyOS calls this device class 'phone'; the actual OpenHarmony
            # SDK syscap definitions call it 'default'. Normalize only this
            # explicit alternate-SDK test copy, never the generated source or SDK.
            module['module']['deviceTypes'] = [
                'default' if device == 'phone' else device for device in original_devices]
            write_document(module_path, module)
        (project / 'node_modules').symlink_to(args.node_modules, target_is_directory=True)
        env = dict(os.environ)
        env.update(NODE_PATH=str(args.node_modules), HVIGOR_USER_HOME=str(root / 'hvigor-home'),
                   DEVECO_SDK_HOME=str(args.sdk), OHOS_SDK_HOME=str(args.sdk), OHOS_BASE_SDK_HOME=str(args.sdk),
                   SHELL=shutil.which('sh') or '/usr/bin/sh')
        # Inherit explicitly configured proxy variables; never change transport
        # or install dependencies as a side effect of a validation run.
        command = [args.node, str(hvigor), '--mode', 'module', '-p', 'module=entry@default',
                   '-p', 'product=default', '-p', 'buildMode=debug',
                   'default@CompileArkTS' if args.arkts_only else 'assembleHap', '--no-daemon', '--stacktrace']
        with (output / 'build.log').open('w') as log:
            result = subprocess.run(command, cwd=project, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=900)
        artifacts = []
        if result.returncode == 0 and not args.arkts_only:
            for hap in (project / 'entry/build').rglob('*.hap'):
                target = output / hap.name
                shutil.copyfile(hap, target)
                artifacts.append(target.name)
            if not artifacts:
                raise RuntimeError('assembleHap succeeded without producing a HAP')
        write_document(output / 'result.json', {
            'scope': 'OpenHarmony alternate SDK compatibility build' if args.openharmony_sdk_version else 'HarmonyOS project build',
            'installed_or_ui_tested': False, 'source_project': str(args.project),
            'build_profile': profile, 'original_device_types': original_devices,
            'test_device_types': module['module']['deviceTypes'],
            'exit_code': result.returncode, 'artifacts': artifacts})
        print(output)
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
