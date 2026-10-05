#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Exercise native Editor discovery through an existing real oheco broker.

Uses a disposable GDScript project and fresh EditorSettings. Verifies discovery
from unchanged HOME despite a different OHECO_ROOT with no broker directory, the
broker default, a real runnable preset/Remote Deploy menu, optional broker/direct
routing transitions, and the real C++ project exporter plus headless PCK boot.
It also triggers actual Remote Deploy with synthetic signing inputs/fake Hvigor,
and asks fake HDC to read the HAP directly from the project's .godot directory.
The synthetic install is deliberately rejected before any AA/port commands.
No broker service, real device deployment, port rules, SDK build or signing is changed.
All fixture files live in TMPDIR; persistent evidence contains logs and audits.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import tempfile
import time
import zipfile

sys.dont_write_bytecode = True
from pck_audit import pack_index


EDITOR_SCRIPT = '''extends SceneTree
var remote: MenuButton
var export_node: Node
var platform: EditorExportPlatform
var report: Dictionary = {"status": "FAIL", "phases": []}

func _initialize() -> void:
    call_deferred("audit")

func save_report() -> void:
    var file := FileAccess.open(OS.get_environment("GODOT_BROKER_AUDIT_REPORT"), FileAccess.WRITE)
    if file:
        file.store_string(JSON.stringify(report, "  "))
        file.close()

func require(condition: bool, message: String) -> bool:
    if not condition:
        report["failure"] = message
        save_report()
        push_error("BROKER_EDITOR_FAIL: " + message)
        quit(1)
    return condition

func scan(node: Node) -> void:
    if node.get_class() == "EditorExport":
        export_node = node
    if node is MenuButton and node.tooltip_text == "Remote Deploy":
        remote = node
    for child in node.get_children():
        scan(child)

func snapshot() -> Dictionary:
    var items: Array = []
    var target_items: Array = []
    for index in range(remote.get_popup().item_count):
        var item := {
            "text": remote.get_popup().get_item_text(index),
            "id": remote.get_popup().get_item_id(index),
            "disabled": remote.get_popup().is_item_disabled(index),
        }
        items.append(item.text)
        target_items.append(item)
    return {"visible": remote.is_visible(), "visible_in_tree": remote.is_visible_in_tree(),
            "disabled": remote.disabled, "items": items, "item_details": target_items}

func wait_target(stage: String, expected: String, rejected: String = "") -> bool:
    var began := Time.get_ticks_msec()
    var phase: Dictionary = {"stage": stage, "expected": expected, "rejected": rejected, "samples": []}
    report.phases.append(phase)
    # Allow a complete native polling interval after changing routing settings.
    await create_timer(3.5).timeout
    while Time.get_ticks_msec() - began < 20000:
        var state := snapshot()
        state["elapsed_ms"] = Time.get_ticks_msec() - began
        phase.samples.append(state)
        print("BROKER_EDITOR_MENU ", stage, " ", JSON.stringify(state))
        var enabled_target := false
        for item in state.item_details:
            if item.text == expected and not item.disabled:
                enabled_target = true
        if state.visible and state.visible_in_tree and not state.disabled and enabled_target and not state.items.has(rejected):
            phase["passed"] = true
            save_report()
            return true
        await create_timer(1.0).timeout
    phase["passed"] = false
    return require(false, stage + " did not show enabled target " + expected)

func audit() -> void:
    await process_frame
    while EditorInterface.get_resource_filesystem().is_scanning():
        await process_frame
    var native_home := OS.get_environment("HOME")
    var tool_root := OS.get_environment("OHECO_ROOT")
    report["endpoint_routing"] = {
        "home": native_home, "toolchain_root": tool_root,
        "home_endpoint_exists": FileAccess.file_exists(native_home.path_join(".oheco/broker/endpoint")),
        "toolchain_broker_directory_exists": DirAccess.dir_exists_absolute(tool_root.path_join("broker")),
    }
    if not require(native_home == OS.get_environment("GODOT_BROKER_AUDIT_HOME"), "native HOME must stay unchanged for shell serve discovery"):
        return
    if not require(report.endpoint_routing.home_endpoint_exists and not report.endpoint_routing.toolchain_broker_directory_exists, "fixture must have a HOME endpoint and no toolchain broker directory"):
        return
    var settings := EditorInterface.get_editor_settings()
    report["settings"] = {
        "use_broker_exists": settings.has_setting("export/openharmony/use_broker"),
        "broker_port_exists": settings.has_setting("export/openharmony/broker_port"),
    }
    if not require(report.settings.use_broker_exists, "native use_broker setting missing"):
        return
    var value: Variant = settings.get("export/openharmony/use_broker")
    report.settings["use_broker_type"] = typeof(value)
    report.settings["use_broker_default"] = value
    if not require(typeof(value) == TYPE_BOOL and value == true, "use_broker must default to BOOL true in fresh settings"):
        return
    if not require(not report.settings.broker_port_exists, "broker_port must not be an Editor setting"):
        return
    if not require(OS.get_environment("GODOT_HDC_PROBE_DIRECT") == "1", "direct route marker missing from native Editor"):
        return
    scan(root)
    report["export_node_found"] = export_node != null
    report["remote_button_found"] = remote != null
    if not require(export_node != null and remote != null, "real EditorExport/Remote Deploy nodes missing"):
        return
    for connection in export_node.get_signal_connection_list("export_presets_runnable_updated"):
        var object: Object = connection.callable.get_object()
        if object.get_class() == "EditorExportPlatformOpenHarmony":
            platform = object
    if not require(platform != null, "OpenHarmony platform missing from runnable update signal"):
        return
    var presets := platform.get_current_presets()
    report["presets"] = []
    for item in presets:
        report.presets.append({"name": item.get_preset_name(), "runnable": item.is_runnable()})
    if not require(presets.size() == 1 and presets[0].get_preset_name() == "OpenHarmony" and presets[0].is_runnable(), "fixture preset was not actually loaded as runnable"):
        return
    var preset: EditorExportPreset = presets[0]
    if not require(preset.get("build/export_project_only") == true and preset.get("build/sign") == false, "fixture export settings did not load"):
        return
    if not await wait_target("real-broker-discovery", OS.get_environment("GODOT_BROKER_AUDIT_TARGET")):
        return
    var initial_hdc: String = settings.get("export/openharmony/hdc_path")
    if OS.get_environment("GODOT_BROKER_AUDIT_TOGGLE") == "1":
        settings.set_setting("export/openharmony/hdc_path", OS.get_environment("GODOT_BROKER_AUDIT_FAKE_HDC"))
        if not await wait_target("fake-broker", "godot-broker-probe:5555", "godot-direct-probe:5555"):
            return
        settings.set_setting("export/openharmony/use_broker", false)
        if not await wait_target("fake-direct", "godot-direct-probe:5555", "godot-broker-probe:5555"):
            return
        settings.set_setting("export/openharmony/use_broker", true)
        if not await wait_target("fake-broker-restored", "godot-broker-probe:5555", "godot-direct-probe:5555"):
            return
        settings.set_setting("export/openharmony/hdc_path", initial_hdc)
        if not await wait_target("real-broker-restored", OS.get_environment("GODOT_BROKER_AUDIT_TARGET"), "godot-broker-probe:5555"):
            return
    platform.clear_messages()
    var result := platform.export_project(preset, true, OS.get_environment("GODOT_BROKER_AUDIT_EXPORT"))
    report["export_result"] = result
    report["export_messages"] = []
    for index in range(platform.get_message_count()):
        report.export_messages.append({"type": platform.get_message_type(index),
            "category": platform.get_message_category(index), "text": platform.get_message_text(index)})
    print("BROKER_EDITOR_EXPORT_RESULT=", result)
    if not require(result == OK, "real C++ project export failed"):
        return
    # Exercise the native Run entry, not a copy of its temporary-path algorithm.
    # No real game signing material is used, and fake HDC rejects installation.
    report["run_method_bound"] = platform.has_method("run")
    settings.set_setting("export/openharmony/hdc_path", OS.get_environment("GODOT_BROKER_AUDIT_FAKE_HDC"))
    settings.set_setting("export/openharmony/use_broker", true)
    settings.set_setting("export/openharmony/hvigor_entry", OS.get_environment("GODOT_BROKER_AUDIT_FAKE_HVIGOR"))
    settings.set_setting("export/openharmony/sdk_root", OS.get_environment("GODOT_BROKER_AUDIT_SDK_ROOT"))
    settings.set_project_metadata("debug_options", "run_deploy_remote_debug", false)
    settings.set_project_metadata("debug_options", "run_file_server", false)
    preset.set("build/export_project_only", false)
    preset.set("build/sign", true)
    preset.set("build/bundle_id", "org.oheco.projectwithspaces")
    preset.set("sign/certpath_file", OS.get_environment("GODOT_BROKER_AUDIT_DUMMY_CERTIFICATE"))
    preset.set("sign/profile_file", OS.get_environment("GODOT_BROKER_AUDIT_DUMMY_PROFILE"))
    preset.set("sign/store_file", OS.get_environment("GODOT_BROKER_AUDIT_DUMMY_STORE"))
    preset.set("sign/key_alias", "synthetic-fixture-only")
    preset.set("sign/sign_alg", "SHA256withECDSA")
    preset.set("sign/key_password", "synthetic-key-password")
    preset.set("sign/store_password", "synthetic-store-password")
    if not await wait_target("fake-broker-native-run", "godot-broker-probe:5555", "godot-direct-probe:5555"):
        return
    var selected_id := -1
    for index in range(remote.get_popup().item_count):
        if remote.get_popup().get_item_text(index) == "godot-broker-probe:5555":
            selected_id = remote.get_popup().get_item_id(index)
    if not require(selected_id >= 0, "native Run target has no popup id"):
        return
    report["native_run"] = {"trigger": "RemoteDeploy popup.id_pressed", "popup_id": selected_id,
        "project_data_path": ProjectSettings.globalize_path("res://.godot"),
        "synthetic_signing_inputs": true, "real_signing": false, "real_installation": false}
    platform.clear_messages()
    remote.get_popup().emit_signal("id_pressed", selected_id)
    # The signal invokes the synchronous C++ run; its install is a fake read audit.
    var run_audit_path := OS.get_environment("GODOT_BROKER_AUDIT_RUN_RECORD")
    if not require(FileAccess.file_exists(run_audit_path), "actual Run did not reach fake HDC file inspection"):
        return
    var inspection: Variant = JSON.parse_string(FileAccess.get_file_as_string(run_audit_path))
    if not require(inspection is Dictionary and inspection.get("read_ok", false), "broker fake HDC could not read the HAP"):
        return
    report.native_run["inspection"] = inspection
    report.native_run["hap_removed_after_run"] = not FileAccess.file_exists(inspection.path)
    report.native_run["generated_project_removed_after_run"] = not DirAccess.dir_exists_absolute(inspection.path.get_basename())
    report.native_run["run_directory_removed_after_run"] = not DirAccess.dir_exists_absolute(inspection.path.get_base_dir())
    report.native_run["messages"] = []
    for index in range(platform.get_message_count()):
        report.native_run.messages.append({"type": platform.get_message_type(index),
            "category": platform.get_message_category(index), "text": platform.get_message_text(index)})
    settings.set_setting("export/openharmony/hdc_path", initial_hdc)
    report["status"] = "PASS"
    save_report()
    print("BROKER_EDITOR_PASS")
    quit(0)
'''

FAKE_HDC = '''#!/system/bin/sh
# Never forward to real HDC. Install only reads a synthetic file, then fails.
if [ "$#" -eq 5 ] && [ "$1" = "-t" ] && [ "$3" = "install" ] && [ "$4" = "-r" ]; then
    exec @@NODE@@ @@INSPECT@@ "$@"
fi
if [ "$#" -ne 2 ] || [ "$1" != "list" ] || [ "$2" != "targets" ]; then
    printf '[Fail] unexpected fake HDC command\\n'
    exit 64
fi
if [ "${GODOT_HDC_PROBE_DIRECT:-}" = "1" ]; then
    printf 'direct\\n' >> "${0%/*}/route.log"
    printf 'godot-direct-probe:5555\\n'
else
    printf 'broker\\n' >> "${0%/*}/route.log"
    printf 'godot-broker-probe:5555\\n'
fi
'''

# The build runner and C++ export are real; this adapter produces JSON bytes with
# a -signed filename only to exercise result selection. It performs no signing.
FAKE_HVIGOR = r'''const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const args=process.argv.slice(2),root=process.cwd();
fs.appendFileSync(@@BUILD_LOG@@,JSON.stringify({args,cwd:root,synthetic:true})+'\n');
if(args.length===1&&args[0]==='--adapter-info'){
  console.log(JSON.stringify({adapter:'6.26.4-ohos.1',host:'openharmony',arch:'arm64',upstream:'6.26.4',pinnedInterfacesUnmodified:true}));
}else{
  if(!args.includes('assembleHap'))process.exit(64);
  const pck=fs.readFileSync(path.join(root,'entry/src/main/resources/rawfile/template.pck'));
  const out=path.join(root,'entry/build/default/outputs/default');fs.mkdirSync(out,{recursive:true});
  fs.writeFileSync(path.join(out,'fixture-default-signed.hap'),JSON.stringify({fixture:'SYNTHETIC_NOT_A_SIGNED_HAP',pck_sha256:crypto.createHash('sha256').update(pck).digest('hex')})+'\n');
}
'''

FAKE_HDC_INSPECT = r'''const fs=require('node:fs'),crypto=require('node:crypto');
const argv=process.argv.slice(2),filename=argv[4];
const record={argv,path:filename,read_ok:false,broker_route:process.env.GODOT_HDC_PROBE_DIRECT!=='1',real_installation:false};
try{
  const bytes=fs.readFileSync(filename),data=JSON.parse(bytes.toString('utf8'));
  record.read_ok=data.fixture==='SYNTHETIC_NOT_A_SIGNED_HAP';
  record.bytes=bytes.length;record.sha256=crypto.createHash('sha256').update(bytes).digest('hex');record.payload=data;
}catch(e){record.error=String(e);}
fs.writeFileSync(@@RUN_RECORD@@,JSON.stringify(record,null,2)+'\n');
console.log('[Fail] SYNTHETIC_HAP_INSPECTION_ONLY; no package was installed');
'''

MAIN_SCRIPT = '''extends Node
func _ready() -> void:
    if ProjectSettings.get_setting("application/config/name") != "Broker Editor Regression":
        push_error("Exported application metadata mismatch")
        get_tree().quit(1)
        return
    if FileAccess.get_file_as_string("res://regression.txt") != "BROKER_EXPORT_RESOURCE\\n":
        push_error("Exported resource did not survive the PCK")
        get_tree().quit(1)
        return
    print("BROKER_EDITOR_PCK_PASS")
    get_tree().quit(0)
'''


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--editor', type=Path, required=True, help='Signed native Editor CLI with broker support')
    parser.add_argument('--template', type=Path, required=True, help='Complete native debug template ZIP')
    parser.add_argument('--oheco-root', type=Path, default=Path('/storage/Users/currentUser/.oheco'),
                        help='Installed tool root to expose through a temporary alias; broker discovery uses unchanged HOME')
    parser.add_argument('--target', default='192.168.1.160:34835', help='Target expected from read-only real broker discovery')
    parser.add_argument('--output', type=Path, required=True, help='New persistent directory for logs and audits')
    parser.add_argument('--skip-routing-toggle', action='store_true', help='Omit the fake routing transitions; still exercise native Run with mock tools')
    parser.add_argument('--sdk-root', type=Path, help='Canonical SDK view root for the real build runner; defaults to installed API26 view')
    args = parser.parse_args()
    if sys.platform != 'ohos':
        parser.error('Run on native OpenHarmony')
    editor = args.editor.resolve(strict=True)
    library = editor.parent / 'libgodot.so'
    template = args.template.resolve(strict=True)
    oheco_root = args.oheco_root.resolve(strict=True)
    inherited_home = os.environ.get('HOME', '')
    if not inherited_home or not Path(inherited_home).is_absolute():
        parser.error('Keep an absolute HOME pointing to the existing shell serve discovery file')
    for name in ('bin', 'packages', 'sdk'):
        if not (oheco_root / name).is_dir():
            parser.error(f'Tool root must supply {name} for the temporary alternate-root fixture')
    sdk_root = args.sdk_root
    if sdk_root is None:
        candidates = []
        for view in (oheco_root / 'sdk').iterdir():
            record_path = view / 'view.json'
            if record_path.is_file() and json.loads(record_path.read_text()).get('sdk_version') == '26.0.0':
                candidates.append(view / 'root')
        if not candidates:
            parser.error('Provide --sdk-root or an installed API26 SDK view for the native Run build runner')
        sdk_root = sorted(candidates)[-1]
    sdk_root = sdk_root.resolve(strict=True)
    if not sdk_root.is_dir():
        parser.error('--sdk-root must be a real SDK view directory')
    endpoint = Path(inherited_home) / '.oheco/broker/endpoint'
    endpoint_text = endpoint.read_text().strip()
    if not endpoint_text.startswith('127.0.0.1:') or not library.is_file():
        parser.error('Require an existing loopback broker endpoint and adjacent signed libgodot.so')
    tmpdir = os.environ.get('TMPDIR')
    if not tmpdir or not Path(tmpdir).is_dir():
        parser.error('TMPDIR must be an existing writable native temporary directory')
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    records = []
    report = {'schemaVersion': 1, 'status': 'FAIL', 'platform': sys.platform,
              'scope': 'Real native headless Editor/discovery/routing/project export/PCK; actual native Run with synthetic signing inputs and fake Hvigor/HDC direct file inspection. No real HAP build/sign/install, UIAbility or device AA/port changes.',
              'endpoint': endpoint_text, 'endpoint_file': str(endpoint), 'expected_target': args.target,
              'routing_toggle': not args.skip_routing_toggle, 'broker_environment_overrides': False,
              'inputs': {name: {'path': str(path), 'sha256': sha256(path)} for name, path in [
                  ('editor', editor), ('editor_library', library), ('debug_template', template), ('test', Path(__file__).resolve())]},
              'stages': records}
    temporary_root = None
    failure = None
    try:
        with tempfile.TemporaryDirectory(prefix='godot-broker-editor-', dir=tmpdir) as temporary:
            root = Path(temporary)
            temporary_root = root
            toolchain_alias = root / 'alternate tools with spaces'
            toolchain_alias.mkdir()
            for name in ('bin', 'packages', 'sdk'):
                (toolchain_alias / name).symlink_to(oheco_root / name, target_is_directory=True)
            # Tools stay real, but this root deliberately cannot supply discovery.
            # Changing SDK/tool roots must not redirect the shell serve endpoint.
            report['alternate_toolchain_root'] = {
                'path': str(toolchain_alias), 'source': str(oheco_root),
                'linked_directories': ['bin', 'packages', 'sdk'],
                'broker_directory_exists': (toolchain_alias / 'broker').exists(),
                'home_unchanged': True, 'home': inherited_home,
            }
            if (toolchain_alias / 'broker').exists():
                raise AssertionError('Alternate toolchain root must not contain a broker directory')
            project = root / 'project with spaces'
            project.mkdir()
            (project / 'project.godot').write_text('''config_version=5
[application]
config/name="Broker Editor Regression"
run/main_scene="res://Main.tscn"
[rendering]
renderer/rendering_method="mobile"
renderer/rendering_method.mobile="mobile"
rendering_device/driver.openharmony="vulkan"
textures/vram_compression/import_etc2_astc=true
''')
            (project / 'Main.tscn').write_text('''[gd_scene load_steps=2 format=3]
[ext_resource type="Script" path="res://Main.gd" id="1"]
[node name="Main" type="Node"]
script = ExtResource("1")
''')
            (project / 'Main.gd').write_text(MAIN_SCRIPT)
            (project / 'regression.txt').write_text('BROKER_EXPORT_RESOURCE\n')
            (project / 'export_presets.cfg').write_text('''[runnable_presets]
OpenHarmony="OpenHarmony"
[preset.0]
name="OpenHarmony"
platform="OpenHarmony"
advanced_options=true
export_filter="all_resources"
include_filter="regression.txt"
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
build/bundle_id="org.oheco.godoteditor"
build/sign=false
''' % (json.dumps(str(template)), json.dumps(str(template))))
            audit_script = root / 'EditorAudit.gd'
            audit_script.write_text(EDITOR_SCRIPT)
            exit_script = root / 'ExitCode.gd'
            exit_script.write_text('extends SceneTree\nfunc _initialize() -> void:\n    quit(7)\n')
            run_record = root / 'native-run-inspection.json'
            build_log = root / 'fake-hvigor.jsonl'
            inspect_script = root / 'FakeHdcInspect.cjs'
            inspect_script.write_text(FAKE_HDC_INSPECT.replace('@@RUN_RECORD@@', json.dumps(str(run_record))))
            fake_hvigor = root / 'FakeHvigor.cjs'
            fake_hvigor.write_text(FAKE_HVIGOR.replace('@@BUILD_LOG@@', json.dumps(str(build_log))))
            dummy_material = {}
            for name in ('certificate', 'profile', 'store'):
                path = root / ('synthetic-' + name + '.fixture')
                path.write_text('NOT REAL SIGNING MATERIAL; DISPOSABLE TEST FIXTURE ONLY\n')
                path.chmod(0o600)
                dummy_material[name] = path
            fake = root / 'fake-hdc'
            fake.write_text(FAKE_HDC.replace('@@NODE@@', shlex.quote(str(oheco_root / 'bin/node')))
                            .replace('@@INSPECT@@', shlex.quote(str(inspect_script))))
            fake.chmod(0o700)
            env = dict(os.environ)
            for key in ('XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'XDG_DATA_HOME',
                        'GODOT_OHOS_DATA_DIR', 'GODOT_OHOS_CACHE_DIR', 'TMPDIR'):
                directory = root / ('env-' + key.lower())
                directory.mkdir()
                env[key] = str(directory)
            env.pop('LD_PRELOAD', None)
            env['OHECO_ROOT'] = str(toolchain_alias)
            env['GODOT_BROKER_AUDIT_HOME'] = inherited_home
            env['GODOT_HDC_PROBE_DIRECT'] = '1'
            env['GODOT_BROKER_AUDIT_REPORT'] = str(root / 'editor-report.json')
            env['GODOT_BROKER_AUDIT_TARGET'] = args.target
            env['GODOT_BROKER_AUDIT_TOGGLE'] = '0' if args.skip_routing_toggle else '1'
            env['GODOT_BROKER_AUDIT_FAKE_HDC'] = str(fake)
            env['GODOT_BROKER_AUDIT_FAKE_HVIGOR'] = str(fake_hvigor)
            env['GODOT_BROKER_AUDIT_SDK_ROOT'] = str(sdk_root)
            env['GODOT_BROKER_AUDIT_RUN_RECORD'] = str(run_record)
            for name, path in dummy_material.items():
                env['GODOT_BROKER_AUDIT_DUMMY_' + name.upper()] = str(path)
            env['GODOT_BROKER_AUDIT_EXPORT'] = str(root / 'Broker Export.hap')

            def run(name, command, expected=0, timeout=180):
                began = time.monotonic()
                try:
                    result = subprocess.run(command, cwd=project, env=env, stdin=subprocess.DEVNULL,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                            encoding='utf-8', errors='replace', timeout=timeout)
                    text, code = result.stdout, result.returncode
                except subprocess.TimeoutExpired as error:
                    text = error.stdout or b''
                    if isinstance(text, bytes):
                        text = text.decode('utf-8', 'replace')
                    code = 124
                (output / (name + '.log')).write_text(text)
                records.append({'stage': name, 'command': command, 'exit_code': code,
                                'expected_exit_code': expected, 'elapsed_seconds': round(time.monotonic() - began, 3)})
                print(f'{name}: exit={code}, log={output / (name + ".log")}', flush=True)
                if code != expected or 'SCRIPT ERROR:' in text:
                    raise RuntimeError(f'{name} failed (exit {code}, expected {expected}); see its stdout log')
                return text

            try:
                run('exit-code', [str(editor), '--headless', '--path', str(project), '--script', str(exit_script)], expected=7)
                text = run('editor', [str(editor), '--headless', '--editor', '--path', str(project), '--script', str(audit_script)])
                if 'BROKER_EDITOR_PASS' not in text:
                    raise AssertionError('Editor audit did not complete')
                generated = root / 'Broker Export'
                if (root / 'Broker Export.hap').exists():
                    raise AssertionError('Project-only export unexpectedly produced a HAP')
                app = json.loads((generated / 'AppScope/app.json5').read_text())
                module = json.loads((generated / 'entry/src/main/module.json5').read_text())
                host = json.loads((generated / 'entry/src/main/resources/rawfile/godot_host.json').read_text())
                permissions = [item['name'] for item in module['module'].get('requestPermissions', [])]
                if permissions:
                    raise AssertionError(f'Disposable exported game must have empty permissions, got {permissions}')
                if app['app']['bundleName'] != 'org.oheco.godoteditor' or host['managed']['mode'] != 'none' or host['host']['role'] != 'game':
                    raise AssertionError('Exported GDScript project metadata differs from the preset')
                with zipfile.ZipFile(template) as archive:
                    for name in archive.namelist():
                        if name.endswith(('.ets', '.cpp', '.h', '.ts')) and (generated / name).read_bytes() != archive.read(name):
                            raise AssertionError(f'Exporter altered shared template source: {name}')
                pck = generated / 'entry/src/main/resources/rawfile/template.pck'
                entries = pack_index(pck)
                paths = {entry.path for entry in entries}
                if 'project.binary' not in paths or 'regression.txt' not in paths or not paths.intersection({'Main.tscn', 'Main.tscn.remap'}):
                    raise AssertionError(f'Missing exported PCK resource paths: {sorted(paths)}')
                if any(path.startswith('.godot/mono/') for path in paths):
                    raise AssertionError('GDScript fixture must not acquire a managed payload')
                (output / 'pck-index.json').write_text(json.dumps([{'path': entry.path, 'size': entry.size, 'md5': entry.md5} for entry in entries], indent=2) + '\n')
                report['export_audit'] = {'bundle_name': app['app']['bundleName'], 'permissions': permissions,
                    'MOUNT_HDCDEBUG_PATH': False, 'managed_mode': host['managed']['mode'],
                    'pck_entries': len(entries), 'pck_bytes': pck.stat().st_size, 'pck_sha256': sha256(pck),
                    'shared_template_sources_unchanged': True}
                for name in ('AppScope/app.json5', 'entry/src/main/module.json5', 'entry/src/main/resources/rawfile/godot_host.json'):
                    shutil.copyfile(generated / name, output / Path(name).name)
                editor_audit = json.loads((root / 'editor-report.json').read_text())
                native_run = editor_audit['native_run']
                inspection = json.loads(run_record.read_text())
                installed_path = Path(inspection['path'])
                relative = installed_path.relative_to(project / '.godot' / 'openharmony')
                if (not installed_path.is_absolute() or len(relative.parts) != 2
                        or not relative.parts[0].startswith('run_') or relative.parts[1] != 'project.hap'):
                    raise AssertionError(f'Actual Run install did not read project .godot HAP: {installed_path}')
                if inspection.get('broker_route') is not True or inspection.get('read_ok') is not True:
                    raise AssertionError('Fake HDC must successfully read the same HAP through the broker route')
                if inspection.get('payload', {}).get('fixture') != 'SYNTHETIC_NOT_A_SIGNED_HAP':
                    raise AssertionError('Run inspection must read the explicitly synthetic package')
                if inspection['argv'] != ['-t', 'godot-broker-probe:5555', 'install', '-r', str(installed_path)]:
                    raise AssertionError('Actual Run install argv differs from the selected fake target and inspected path')
                build_invocations = [json.loads(line) for line in build_log.read_text().splitlines()]
                if (len(build_invocations) != 2 or build_invocations[0]['args'] != ['--adapter-info']
                        or 'assembleHap' not in build_invocations[1]['args']
                        or any(Path(item['cwd']) != installed_path.with_suffix('') for item in build_invocations)):
                    raise AssertionError('Actual Run did not invoke the shared build runner and fake Hvigor in its generated project')
                if not all(native_run.get(key) is True for key in ('hap_removed_after_run',
                        'generated_project_removed_after_run', 'run_directory_removed_after_run')):
                    raise AssertionError('Native Run did not clean its owned HAP/project/run directory after fake install rejection')
                if installed_path.exists() or installed_path.parent.exists():
                    raise AssertionError('Native Run left its owned shared-project staging files')
                if not any('SYNTHETIC_HAP_INSPECTION_ONLY' in message['text'] for message in native_run['messages']):
                    raise AssertionError('Native Run did not expose the deliberately rejected synthetic installation')
                report['native_run_audit'] = {
                    'trigger': native_run['trigger'], 'run_method_bound': editor_audit['run_method_bound'],
                    'hdc_read_path': str(installed_path), 'project_godot_path': str(project / '.godot'),
                    'read_from_broker_route': True, 'synthetic_bytes': inspection['bytes'],
                    'synthetic_sha256': inspection['sha256'], 'synthetic_payload': inspection['payload'],
                    'actual_cpp_run_and_build_runner': True, 'mock_hvigor': True, 'mock_hdc': True,
                    'real_hap_build': False, 'real_signing': False, 'real_installation': False,
                    'owned_run_directory_removed': True,
                }
                text = run('pck', [str(editor), '--headless', '--main-pack', str(pck)])
                if 'BROKER_EDITOR_PCK_PASS' not in text:
                    raise AssertionError('Exported GDScript PCK did not execute its resource check')
                report['status'] = 'PASS'
            finally:
                editor_report = root / 'editor-report.json'
                if editor_report.is_file():
                    shutil.copyfile(editor_report, output / 'editor-report.json')
                for fixture_log in (run_record, build_log):
                    if fixture_log.is_file():
                        shutil.copyfile(fixture_log, output / fixture_log.name)
                route_log = root / 'route.log'
                if route_log.is_file():
                    routing = route_log.read_text().splitlines()
                    (output / 'routing.log').write_text(route_log.read_text())
                    report['route_executions'] = routing
                    if not args.skip_routing_toggle and (routing.count('broker') < 2 or routing.count('direct') < 1):
                        raise AssertionError(f'Missing real routing executions: {routing}')
    except Exception as error:
        failure = error
        report['status'] = 'FAIL'
        report['failure'] = f'{type(error).__name__}: {error}'
    finally:
        report['temporary_fixture_cleaned'] = temporary_root is not None and not temporary_root.exists()
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    if failure:
        raise RuntimeError(f'Native broker Editor regression failed; see {output / "result.json"}') from failure
    print('PASS real broker Editor discovery, runnable Remote Deploy menu, project export, empty permissions and GDScript PCK boot')


if __name__ == '__main__':
    main()
