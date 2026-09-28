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

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--godot', type=Path, required=True)
    parser.add_argument('--template', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New persistent directory for evidence/project')
    args = parser.parse_args()
    if sys.platform != 'ohos':
        raise SystemExit('Run on native OpenHarmony')
    args.godot = args.godot.resolve(strict=True)
    args.template = args.template.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    with tempfile.TemporaryDirectory(prefix='godot-export-test-', dir=os.environ['TMPDIR']) as temporary:
        root = Path(temporary)
        project = root / 'project'
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
    build/version_name="test.7"
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

        def run(name, command, expected=0):
            result = subprocess.run(command, cwd=project, env=env, capture_output=True, text=True, timeout=300)
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
        assert read('AppScope/app.json5')['app']['versionName'] == 'test.7'
        strings = {v['name']: v['value'] for v in read('AppScope/resources/base/element/string.json')['string']}
        assert strings['app_name'] == display_name
        product = read('build-profile.json5')['app']['products'][0]
        for key in ('compileSdkVersion', 'targetSdkVersion', 'compatibleSdkVersion'):
            assert product[key] == '6.1.0(23)', (key, product)
        module = read('entry/src/main/module.json5')['module']
        assert module['mainElement'] == 'EntryAbility'
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
        text = run('pck', [str(args.godot), '--headless', '--main-pack', str(pck)])
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
        shutil.copytree(generated, output / 'Game')
        (output / 'result.json').write_text(json.dumps({'scope': 'real native export and headless PCK, not HAP', 'stages': records}, indent=2) + '\n')
    print('PASS real native game export, API 23 metadata, permissions, shared sources, PCK boot and API 18 rejection')


if __name__ == "__main__":
    main()
