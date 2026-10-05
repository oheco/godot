#!/usr/bin/env python3
"""Verify an aborted export cannot contribute stale payload to the next export.

Use a native editor executable. This test creates only a private GDScript project;
it does not invoke .NET, signing, SDK builds or application installation.
"""
import argparse
from pathlib import Path
import os
import subprocess
import tempfile

from pck_audit import pack_index


SCRIPT = '''extends SceneTree

class PayloadPlugin extends EditorExportPlugin:
    var calls := 0
    func _get_name() -> String:
        return "Retry audit payload"
    func _export_begin(_features: PackedStringArray, _debug: bool, _path: String, _flags: int) -> void:
        calls += 1
        add_file("res://.godot/mono/publish/arm64/Marker.dll", ("attempt-%d" % calls).to_utf8_buffer(), false)

class AuditPlatform extends EditorExportPlatformExtension:
    func _get_name() -> String: return "Leak Audit"
    func _get_os_name() -> String: return "Linux"
    func _get_platform_features() -> PackedStringArray: return PackedStringArray(["pc", "Linux"])
    func _get_preset_features(_preset: EditorExportPreset) -> PackedStringArray: return PackedStringArray(["x86_64"])
    func _get_export_options() -> Array[Dictionary]: return []

func _initialize() -> void:
    call_deferred("audit")

func audit() -> void:
    while EditorInterface.get_resource_filesystem().is_scanning():
        await process_frame
    var owner := EditorPlugin.new()
    var plugin := PayloadPlugin.new()
    owner.add_export_plugin(plugin)
    var platform := AuditPlatform.new()
    var preset := platform.create_preset()
    var failed := platform.export_pack(preset, true, ProjectSettings.globalize_path("res://missing-dir/Failed.pck"))
    print("AUDIT_FIRST_RESULT=", failed, " BEGINS=", plugin.calls)
    var retried := platform.export_pack(preset, true, ProjectSettings.globalize_path("res://Retry.pck"))
    print("AUDIT_SECOND_RESULT=", retried, " BEGINS=", plugin.calls)
    owner.remove_export_plugin(plugin)
    owner.free()
    quit(0 if failed != OK and retried == OK and plugin.calls == 2 else 1)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--editor', type=Path, required=True)
    args = parser.parse_args()
    editor = args.editor.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='godot-export-retry-', dir=os.environ['TMPDIR']) as directory:
        root = Path(directory)
        project = root / 'project'
        project.mkdir()
        (project / 'project.godot').write_text('''config_version=5
[application]
config/name="RetryLeakAudit"
[rendering]
renderer/rendering_method="gl_compatibility"
''')
        (project / 'audit.gd').write_text(SCRIPT)
        env = dict(os.environ)
        for key in ('GODOT_OHOS_DATA_DIR', 'GODOT_OHOS_CACHE_DIR', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME'):
            location = root / key.lower()
            location.mkdir()
            env[key] = str(location)
        command = [str(editor), '--headless', '--editor', '--path', str(project),
                   '--script', str(project / 'audit.gd')]
        try:
            result = subprocess.run(command, env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, timeout=120)
        except subprocess.TimeoutExpired as error:
            output = error.stdout or b''
            if isinstance(output, bytes):
                output = output.decode('utf-8', errors='replace')
            raise RuntimeError(f'Native retry scenario timed out:\n{output}') from error
        if result.returncode or 'AUDIT_SECOND_RESULT=0 BEGINS=2' not in result.stdout:
            raise RuntimeError(f'Native retry scenario failed (exit {result.returncode}):\n{result.stdout}')
        pack = project / 'Retry.pck'
        entries = pack_index(pack)  # A stale first attempt must fail, even with identical bytes.
        marker = [entry for entry in entries if entry.path == '.godot/mono/publish/arm64/Marker.dll']
        if len(marker) != 1:
            raise AssertionError(f'Expected one current marker, got {len(marker)}')
        with pack.open('rb') as stream:
            stream.seek(marker[0].offset)
            content = stream.read(marker[0].size)
        if content != b'attempt-2':
            raise AssertionError(f'The successful retry must contain its own payload, got {content!r}')
        print('PASS: failed export discarded its payload; retry stored only attempt-2')


if __name__ == '__main__':
    main()
