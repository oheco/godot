#!/usr/bin/env python3
"""Run the real Godot export pipeline and boot its PCK on native OpenHarmony.

This verifies export/configuration and headless PCK execution, not an installed
HAP, Vulkan presentation or UIAbility lifecycle. Requires a freshly built signed
editor CLI and a real template_debug ZIP from generate_bundle=yes.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import zipfile

from pck_audit import pack_index

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--godot', type=Path, required=True)
    parser.add_argument('--template', type=Path, required=True)
    parser.add_argument('--runtime-godot', type=Path, help='Optional signed template CLI from build-cli.py; test hardened adjacent-PCK discovery')
    parser.add_argument('--output', type=Path, required=True, help='New persistent directory for evidence/project')
    parser.add_argument('--build-bundles', action='store_true', help='Also run real oo Hvigor unsigned HAP/APP builds')
    parser.add_argument('--release-template', type=Path, help='Complete template_release ZIP for bundle validation')
    args = parser.parse_args()
    if sys.platform != 'ohos':
        raise SystemExit('Run on native OpenHarmony')
    args.godot = args.godot.resolve(strict=True)
    args.template = args.template.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    with tempfile.TemporaryDirectory(prefix='godot-export-test-', dir=os.environ['TMPDIR']) as temporary:
        root = Path(temporary)
        project = root / 'project with spaces'
        project.mkdir()
        display_name = 'Game "quoted" \\ host'
        (project / 'project.godot').write_text(textwrap.dedent('''    config_version=5
    [application]
    config/name=%s
    run/main_scene="res://Main.tscn"
    [rendering]
    renderer/rendering_method="mobile"
    renderer/rendering_method.mobile="mobile"
    rendering_device/driver.openharmony="vulkan"
    textures/vram_compression/import_etc2_astc=true
    ''') % json.dumps(display_name))
        (project / 'Main.tscn').write_text(textwrap.dedent('''    [gd_scene load_steps=2 format=3]
    [ext_resource type="Script" path="res://Main.gd" id="1"]
    [node name="Main" type="Node"]
    script = ExtResource("1")
    '''))
        (project / 'Main.gd').write_text(
            'extends Node\n'
            'func _ready():\n'
            '    assert(ProjectSettings.get_setting("application/config/name") == %s)\n'
            '    print("SHARED_HOST_PCK_PASS")\n'
            '    get_tree().quit(0)\n' % json.dumps(display_name))
        preset = textwrap.dedent('''    [preset.0]
    name="OpenHarmony"
    platform="OpenHarmony"
    runnable=true
    advanced_options=true
    export_filter="all_resources"
    include_filter=""
    exclude_filter=""
    export_path=""
    [preset.0.options]
    custom_template/debug=%s
    custom_template/release=%s
    architectures/arm64=true
    architectures/x86_64=false
    build/export_project_only=true
    build/override_project_dir=false
    build/sdk_version=""
    build/bundle_id="org.oheco.godothosttest"
    build/version_code=7
    build/version_name="1.0.7"
    build/default_orientation=4
    build/expand_into_system_area=false
    build/verbose_diagnostics=true
    build/sign=false
    permissions/ohos.permission.INTERNET=true
    permissions/ohos.permission.MICROPHONE=true
    ''') % (json.dumps(str(args.template)), json.dumps(str(args.template)))
        (project / 'export_presets.cfg').write_text(preset)
        env = dict(os.environ)
        for key in ('HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'GODOT_OHOS_DATA_DIR', 'GODOT_OHOS_CACHE_DIR'):
            directory = root / key.lower()
            directory.mkdir()
            env[key] = str(directory)
        records = []

        def run(name, command, expected=0, cwd=None):
            result = subprocess.run(command, cwd=cwd or project, env=env, capture_output=True, text=True, timeout=900)
            text = result.stdout + result.stderr
            (output / (name + '.log')).write_text(text)
            records.append({'stage': name, 'exit_code': result.returncode})
            if expected == 0 and (result.returncode != 0 or 'SCRIPT ERROR:' in text):
                raise RuntimeError(f'{name} failed ({result.returncode}); see {output / (name + ".log")}')
            if expected != 0 and result.returncode != expected:
                raise RuntimeError(f'{name} returned {result.returncode}, expected {expected}')
            return text

        (project / 'ExitCode.gd').write_text('extends SceneTree\nfunc _initialize():\n    quit(7)\n')
        run('exit-code', [str(args.godot), '--headless', '--path', str(project), '--script', 'res://ExitCode.gd'], expected=7)
        run('import', [str(args.godot), '--headless', '--editor', '--path', str(project), '--import'])
        run('export', [str(args.godot), '--headless', '--path', str(project), '--export-debug', 'OpenHarmony', str(root / 'Game.hap')])
        generated = root / 'Game'
        read = lambda name: json.loads((generated / name).read_text())
        assert read('AppScope/app.json5')['app']['bundleName'] == 'org.oheco.godothosttest'
        assert read('AppScope/app.json5')['app']['versionCode'] == 7
        assert read('AppScope/app.json5')['app']['versionName'] == '1.0.7'
        strings = {v['name']: v['value'] for v in read('AppScope/resources/base/element/string.json')['string']}
        assert strings['app_name'] == display_name
        product = read('build-profile.json5')['app']['products'][0]
        assert product['compileSdkVersion'] == '26.0.0' and product['targetSdkVersion'] == '26.0.0', product
        assert product['compatibleSdkVersion'] == 23 and product['runtimeOS'] == 'OpenHarmony', product
        module = read('entry/src/main/module.json5')['module']
        assert module['mainElement'] == 'EntryAbility'
        assert module['deviceTypes'] == ['default']
        assert module['abilities'][0]['orientation'] == 'portrait'
        permissions = {p['name']: p for p in module['requestPermissions']}
        assert set(permissions) == {'ohos.permission.INTERNET', 'ohos.permission.MICROPHONE'}
        assert permissions['ohos.permission.MICROPHONE']['usedScene']['abilities'] == ['EntryAbility']
        host = read('entry/src/main/resources/rawfile/godot_host.json')
        assert host['host']['role'] == 'game' and host['managed']['mode'] == 'none'
        assert host['window']['expandIntoSystemArea'] is False
        assert host['diagnostics']['level'] == 'verbose'
        assert not (generated / 'entry/src/main/resources/rawfile/runtime.zip').exists()
        with zipfile.ZipFile(args.template) as template:
            # The exporter must not fork or rewrite the shared shell source.
            for name in template.namelist():
                if name.endswith(('.ets', '.cpp', '.h', '.ts')):
                    assert (generated / name).read_bytes() == template.read(name), name
        pck = generated / 'entry/src/main/resources/rawfile/template.pck'
        pack_index(pck)  # Fail even when the engine hides duplicate paths while mounting.
        text = run('pck', [str(args.godot), '--headless', '--main-pack', str(pck)])
        assert 'SHARED_HOST_PCK_PASS' in text
        if args.runtime_godot:
            # Game templates disable command-line path overrides by default.
            # Do not weaken that build for a test: use a relocated signed CLI
            # and its matching adjacent pack, with no source project in cwd.
            runtime = args.runtime_godot.resolve(strict=True)
            runner = root / 'runtime'
            runner.mkdir()
            shutil.copy2(runtime, runner / 'godot')
            shutil.copy2(runtime.parent / 'libgodot.so', runner / 'libgodot.so')
            shutil.copyfile(pck, runner / 'godot.pck')
            text = run('template-runtime', [str(runner / 'godot'), '--headless'], cwd=runner)
            assert 'SHARED_HOST_PCK_PASS' in text
        (project / 'export_presets.cfg').write_text(preset.replace('build/sdk_version=""', 'build/sdk_version="5.1.0(18)"'))
        run('reject-old-api', [str(args.godot), '--headless', '--path', str(project), '--export-debug', 'OpenHarmony', str(root / 'Old.hap')], expected=1)
        assert not (root / 'Old').exists()
        # Real C++ export must reject nested Variant types and incomplete host
        # contracts, rather than successfully mutating a temporary Dictionary.
        corruptions = [
            ('AppScope/app.json5', 'app', []),
            ('entry/src/main/resources/rawfile/godot_host.json', 'window', []),
            ('entry/src/main/resources/rawfile/godot_host.json', 'managed', {}),
            ('build-profile.json5', 'app', {'products': [None]}),
        ]
        for index, (path, key, bad_value) in enumerate(corruptions):
            broken = root / f'broken-{index}.zip'
            with zipfile.ZipFile(args.template) as original, zipfile.ZipFile(broken, 'w') as altered:
                for entry in original.infolist():
                    content = original.read(entry.filename)
                    if entry.filename == path:
                        document = json.loads(content)
                        document[key] = bad_value
                        content = json.dumps(document).encode()
                    altered.writestr(entry, content)
            (project / 'export_presets.cfg').write_text(preset.replace(json.dumps(str(args.template)), json.dumps(str(broken))))
            run(f'reject-bad-schema-{index}', [str(args.godot), '--headless', '--path', str(project),
                '--export-debug', 'OpenHarmony', str(root / f'Broken{index}.hap')], expected=1)
        if args.build_bundles:
            import hashlib
            bundle_preset = preset.replace('build/export_project_only=true', 'build/export_project_only=false')
            if args.release_template:
                bundle_preset = bundle_preset.replace('custom_template/release=' + json.dumps(str(args.template)),
                                                       'custom_template/release=' + json.dumps(str(args.release_template.resolve(strict=True))))
            (project / 'export_presets.cfg').write_text(bundle_preset)
            for name, switch, suffix in [('debug-hap', '--export-debug', 'hap'),
                                         ('release-hap', '--export-release', 'hap'),
                                         ('release-app', '--export-release', 'app')]:
                target = root / f'{name} with spaces.{suffix}'
                run(name, [str(args.godot), '--headless', '--path', str(project), switch, 'OpenHarmony', str(target)])
                assert target.is_file() and target.stat().st_size > 0, target
                with zipfile.ZipFile(target) as archive:
                    assert archive.testzip() is None
                    if suffix == 'hap':
                        assert any(entry.endswith('/template.pck') for entry in archive.namelist())
                with target.open('rb') as stream:
                    records[-1].update(size=target.stat().st_size, sha256=hashlib.file_digest(stream, 'sha256').hexdigest())
        shutil.copytree(generated, output / 'Game')
        (output / 'result.json').write_text(json.dumps({'scope': 'real native export and headless PCK, not HAP', 'stages': records}, indent=2) + '\n')
    print('PASS real native game export, oo SDK26/default metadata, permissions, shared sources, PCK boot and API 18 rejection')


if __name__ == "__main__":
    main()
