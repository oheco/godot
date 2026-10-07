#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Exercise the real OpenHarmony Editor's filesystem defaults headlessly.

This verifies the engine and embedded file dialog, not installed ArkUI scaling,
clipboard permission, or external-application presentation.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--godot', type=Path, required=True)
    parser.add_argument('--godotsharp', type=Path, required=True)
    parser.add_argument('--dotnet-sdk', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='godot-desktop-defaults-', dir=os.environ['TMPDIR']) as temporary:
        root = Path(temporary)
        (root / 'project.godot').write_text('[application]\nconfig/name="Desktop defaults fixture"\n')
        (root / 'verify.gd').write_text('''extends SceneTree

func _initialize():
    call_deferred("verify")

func verify():
    var expected = "/storage/Users/currentUser"
    if OS.get_system_dir(OS.SYSTEM_DIR_DESKTOP) != expected:
        push_error("Incorrect system desktop directory")
        quit(1)
        return
    var dialog = FileDialog.new()
    dialog.access = FileDialog.ACCESS_FILESYSTEM
    if dialog.current_dir != expected:
        push_error("Incorrect embedded filesystem dialog default: " + dialog.current_dir)
        dialog.free()
        quit(2)
        return
    dialog.current_dir = OS.get_environment("GODOT_TEST_EXPLICIT_DIRECTORY")
    if dialog.current_dir != OS.get_environment("GODOT_TEST_EXPLICIT_DIRECTORY"):
        push_error("An explicitly chosen directory was overridden")
        dialog.free()
        quit(3)
        return
    dialog.free()
    var settings = EditorInterface.get_editor_settings()
    if settings.get_setting("filesystem/directories/default_project_path") != expected:
        push_error("Incorrect initial project directory")
        quit(4)
        return
    print("PASS real Editor: user-home filesystem dialog and initial project path; explicit path preserved")
    quit(0)
''')
        environment = dict(os.environ)
        environment.update(
            GODOT_OHOS_DATA_DIR=str(root / 'data'), GODOT_OHOS_CACHE_DIR=str(root / 'cache'),
            GODOT_TEST_EXPLICIT_DIRECTORY=str(root), GODOT_SHARP_ROOT=str(args.godotsharp.resolve()),
            DOTNET_ROOT=str(args.dotnet_sdk.resolve()), DOTNET_CLI_HOME=str(root / 'dotnet'),
            NUGET_PACKAGES=str(root / 'nuget'), DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1',
            DOTNET_CLI_TELEMETRY_OPTOUT='1')
        command = [str(args.godot.resolve()), '--headless', '--editor', '--path', str(root),
                   '--script', str(root / 'verify.gd')]
        result = subprocess.run(command, env=environment, text=True, capture_output=True, timeout=120)
        print(result.stdout)
        if result.stderr:
            print(result.stderr)
        if result.returncode or 'PASS real Editor:' not in result.stdout:
            raise SystemExit(result.returncode or 1)


if __name__ == '__main__':
    main()
