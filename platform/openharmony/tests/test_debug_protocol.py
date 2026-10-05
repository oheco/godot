#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Regress real native GDScript remote debugging through Godot's Variant codec.

The native editor imports/exports a real debug project and debug PCK. A relocated
signed template_debug CLI boots its adjacent godot.pck without path overrides.
Another real editor process runs a temporary GDScript TCPServer/PacketPeerStream
peer, which asserts breakpoint/stack/local values, next/continue and scene tree.
Only temporary localhost peers are used: no GUI, HDC, device install, C# source
debugging, HAP signing, downloads or persistent app/service dependency.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import sys
import tempfile
import time


MAIN = '''extends Node
func _ready():
    var answer: int = 40
    answer += 2
    print("DEBUG_PROTOCOL_VALUE=", answer)
    get_tree().quit(0)
'''
SCENE = '''[gd_scene load_steps=2 format=3]
[ext_resource type="Script" path="res://Main.gd" id="1"]
[node name="Main" type="Node"]
script = ExtResource("1")
'''
PROJECT = '''config_version=5
[application]
config/name="Native GDScript Debug Protocol Regression"
run/main_scene="res://Main.tscn"
run/flush_stdout_on_print=true
[rendering]
renderer/rendering_method="mobile"
renderer/rendering_method.mobile="mobile"
rendering_device/driver.openharmony="vulkan"
textures/vram_compression/import_etc2_astc=true
'''

# Python never serializes a Godot Variant packet. The real engine's GDScript
# PacketPeerStream performs both packet framing and Variant encoding/decoding.
PEER = r'''extends SceneTree
var server := TCPServer.new()
var stream: StreamPeerTCP
var peer := PacketPeerStream.new()
var stage: String = "break4"
var thread_id: int = 0
var deadline: int = 0
var result_path: String = ""
var finished: bool = false
var expected_vars: int = -1
var found_answer: bool = false
var snapshots: Array = []
var current_snapshot: Dictionary = {}
var scene_nodes: Array = []
var commands: Array = []
var debug_exits: int = 0
var packet_count: int = 0

func _initialize():
    var arguments := OS.get_cmdline_user_args()
    if arguments.size() != 1:
        push_error("Expected one private result path")
        quit(1)
        return
    result_path = arguments[0]
    deadline = Time.get_ticks_msec() + 45000
    var error := server.listen(0, "127.0.0.1")
    if error != OK:
        fail_test("TCPServer localhost listen failed: " + str(error))
        return
    print("PEER_READY " + JSON.stringify({"port": server.get_local_port()}))

func write_result(status: String, reason: String):
    var document := {"schemaVersion": 1, "status": status, "reason": reason,
        "codec": "native Godot PacketPeerStream Variant", "stage": stage,
        "snapshots": snapshots, "scene_nodes": scene_nodes,
        "commands": commands, "debug_exits": debug_exits, "packet_count": packet_count}
    var file := FileAccess.open(result_path, FileAccess.WRITE)
    if file == null:
        push_error("Unable to write private protocol result")
        return
    file.store_string(JSON.stringify(document, "  ") + "\n")
    file.flush()
    file.close()

func fail_test(reason: String):
    if finished:
        return
    finished = true
    write_result("FAIL", reason)
    push_error(reason)
    server.stop()
    if stream != null:
        stream.disconnect_from_host()
    quit(1)

func complete_test():
    if snapshots.size() != 2 or snapshots[0].get("line", 0) != 4 or snapshots[1].get("line", 0) != 5:
        fail_test("Incomplete actual stack transitions")
        return
    if snapshots[0].get("answer", -1) != 40 or snapshots[1].get("answer", -1) != 42 or scene_nodes.is_empty() or debug_exits != 2:
        fail_test("Incomplete actual local/scene assertions")
        return
    finished = true
    write_result("PASS", "Actual native breakpoint, stack, locals, next, continue and scene tree verified")
    server.stop()
    print("PEER_COMPLETE")
    quit(0)

func send_command(command: String, arguments: Array):
    var error := peer.put_var([command, thread_id, arguments])
    if error != OK:
        fail_test("Native PacketPeerStream put_var failed: " + str(error))
        return
    commands.append({"command": command, "thread": thread_id, "args": arguments})

func _process(_delta) -> bool:
    if finished:
        return false
    if Time.get_ticks_msec() > deadline:
        fail_test("Protocol deadline exceeded in " + stage)
        return false
    if stream == null:
        if not server.is_connection_available():
            return false
        stream = server.take_connection()
        peer.stream_peer = stream
    stream.poll()
    while peer.get_available_packet_count() > 0 and not finished:
        var packet: Variant = peer.get_var()
        if peer.get_packet_error() != OK or typeof(packet) != TYPE_ARRAY or packet.size() != 3:
            fail_test("Malformed real remote debugger packet")
            return false
        if typeof(packet[0]) != TYPE_STRING or typeof(packet[1]) != TYPE_INT or typeof(packet[2]) != TYPE_ARRAY:
            fail_test("Malformed remote packet envelope types")
            return false
        packet_count += 1
        handle_packet(packet[0], packet[1], packet[2])
    if finished:
        return false
    if stream.get_status() != StreamPeerTCP.STATUS_CONNECTED:
        if stage == "exit":
            complete_test()
        else:
            fail_test("Game disconnected before protocol completion in " + stage)
    return false

func handle_packet(message: String, message_thread: int, data: Array):
    if message == "debug_exit":
        debug_exits += 1
        return
    if message == "error":
        if data.size() < 10 or data[9] != true:
            fail_test("Remote engine error: " + JSON.stringify(data))
        return
    if not ["debug_enter", "stack_dump", "stack_frame_vars", "stack_frame_var", "scene:scene_tree"].has(message):
        return
    print("PACKET " + message + " stage=" + stage)
    if message == "debug_enter":
        if not ["break4", "break5"].has(stage) or data.size() != 4 or data[0] != true or data[1] != "Breakpoint" or data[2] != true or data[3] != message_thread:
            fail_test("Unexpected native debug_enter state/payload: " + JSON.stringify(data))
            return
        if thread_id != 0 and message_thread != thread_id:
            fail_test("Next broke in an unexpected thread")
            return
        thread_id = message_thread
        stage = "stack4" if stage == "break4" else "stack5"
        send_command("get_stack_dump", [])
        return
    if message_thread != thread_id:
        fail_test("Reply arrived for an unexpected debug thread")
        return
    if message == "stack_dump":
        if not ["stack4", "stack5"].has(stage) or data.size() < 4 or typeof(data[0]) != TYPE_INT or data[0] < 3 or data[0] % 3 != 0 or data.size() != data[0] + 1:
            fail_test("Invalid actual stack_dump shape")
            return
        var expected_line: int = 4 if stage == "stack4" else 5
        if data[1] != "res://Main.gd" or data[2] != expected_line or data[3] != "_ready":
            fail_test("Actual top stack frame did not match Main.gd:_ready line " + str(expected_line) + ": " + JSON.stringify(data))
            return
        current_snapshot = {"file": data[1], "line": data[2], "function": data[3], "thread": thread_id,
            "frame_count": int(data[0] / 3), "answer": null}
        snapshots.append(current_snapshot)
        expected_vars = -1
        found_answer = false
        stage = "vars4" if expected_line == 4 else "vars5"
        send_command("get_stack_frame_vars", [0])
        return
    if message == "stack_frame_vars":
        if not ["vars4", "vars5"].has(stage) or expected_vars != -1 or data.size() != 1 or typeof(data[0]) != TYPE_INT or data[0] <= 0 or data[0] > 100000:
            fail_test("Invalid actual stack_frame_vars count")
            return
        expected_vars = data[0]
        return
    if message == "stack_frame_var":
        if not ["vars4", "vars5"].has(stage) or expected_vars <= 0 or data.size() != 5 or typeof(data[0]) != TYPE_STRING or typeof(data[1]) != TYPE_INT or typeof(data[2]) != TYPE_INT:
            fail_test("Invalid actual stack_frame_var record")
            return
        if data[0] == "answer" and data[1] == 0:
            var expected_answer: int = 40 if stage == "vars4" else 42
            if found_answer or data[2] != TYPE_INT or typeof(data[3]) != TYPE_INT or data[3] != expected_answer:
                fail_test("Actual local answer was not integer " + str(expected_answer))
                return
            found_answer = true
            current_snapshot["answer"] = data[3]
            current_snapshot["local_scope"] = data[1]
            current_snapshot["variant_type"] = data[2]
        expected_vars -= 1
        if expected_vars == 0:
            if not found_answer:
                fail_test("Actual stack locals lacked answer")
                return
            if stage == "vars4":
                stage = "scene"
                send_command("scene:request_scene_tree", [])
            else:
                stage = "exit"
                send_command("continue", [])
        return
    if message == "scene:scene_tree":
        if stage != "scene" or data.is_empty() or data.size() % 6 != 0:
            fail_test("Invalid actual remote scene tree shape")
            return
        var found_main: bool = false
        for index in range(0, data.size(), 6):
            if typeof(data[index]) != TYPE_INT or typeof(data[index + 1]) != TYPE_STRING or typeof(data[index + 2]) != TYPE_STRING or typeof(data[index + 3]) != TYPE_INT or typeof(data[index + 4]) != TYPE_STRING or typeof(data[index + 5]) != TYPE_INT:
                fail_test("Invalid remote node field types")
                return
            var node := {"child_count": data[index], "name": data[index + 1], "type": data[index + 2],
                "id": data[index + 3], "scene_path": data[index + 4], "view_flags": data[index + 5]}
            scene_nodes.append(node)
            if node["name"] == "Main":
                if found_main or node["id"] <= 0 or node["scene_path"] != "res://Main.tscn" or node["child_count"] != 0:
                    fail_test("Actual Main remote node identity/path/children mismatch")
                    return
                found_main = true
        if not found_main:
            fail_test("Actual RemoteSceneTree did not contain Main")
            return
        stage = "break5"
        send_command("next", [])
'''


def digest(filename):
    with filename.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def terminate_owned(process):
    if process is None:
        return
    if process.poll() is None:
        process.terminate()
        try:
            process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=5)


def wait_ready(process, log, timeout=30):
    deadline = time.monotonic() + timeout
    pending = bytearray()
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('Native GDScript peer did not become ready')
            if not selector.select(remaining):
                raise TimeoutError('Native GDScript peer readiness timed out')
            chunk = os.read(process.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError('Native GDScript peer exited before readiness')
            log.write(chunk)
            log.flush()
            pending.extend(chunk)
            while b'\n' in pending:
                line, _, tail = pending.partition(b'\n')
                pending = bytearray(tail)
                if line.startswith(b'PEER_READY '):
                    value = json.loads(line[len(b'PEER_READY '):])
                    port = value['port']
                    assert isinstance(port, int) and 1 <= port <= 65535, value
                    return port


def drain_until_exit(peer, game, log, timeout=60):
    deadline = time.monotonic() + timeout
    with selectors.DefaultSelector() as selector:
        selector.register(peer.stdout, selectors.EVENT_READ)
        while peer.poll() is None or game.poll() is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('Native game/peer protocol did not complete')
            if not selector.get_map():
                # EOF can precede waitpid visibility by a few scheduler ticks;
                # use bounded process waits, not a polling/sleep loop.
                peer.wait(timeout=remaining)
                game.wait(timeout=max(0.001, deadline - time.monotonic()))
                break
            events = selector.select(remaining)
            if not events:
                if peer.poll() is not None and game.poll() is not None:
                    break
                raise TimeoutError('Native game/peer protocol completion timed out')
            for key, _ in events:
                chunk = os.read(key.fileobj.fileno(), 65536)
                if not chunk:
                    selector.unregister(key.fileobj)
                else:
                    log.write(chunk)
                    log.flush()
        return peer.returncode, game.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--editor', type=Path, required=True)
    parser.add_argument('--template-debug', type=Path, required=True)
    parser.add_argument('--template-release', type=Path)
    parser.add_argument('--template-zip', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New application-private evidence directory')
    args = parser.parse_args()
    if sys.platform != 'ohos':
        parser.error('Run with native OpenHarmony Python and native signed Godot CLIs')
    editor = args.editor.resolve(strict=True)
    template_debug = args.template_debug.resolve(strict=True)
    template_zip = args.template_zip.resolve(strict=True)
    template_release = args.template_release.resolve(strict=True) if args.template_release else None
    for entry in [editor, template_debug] + ([template_release] if template_release else []):
        if not entry.is_file() or not (entry.parent / 'libgodot.so').is_file():
            parser.error(f'CLI and adjacent libgodot.so required: {entry}')
    if not os.environ.get('TMPDIR') or not Path(os.environ['TMPDIR']).is_dir():
        parser.error('TMPDIR must be an existing application-private temporary directory')
    args.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    output = args.output.resolve()
    if output.stat().st_mode & 0o777 != 0o700:
        parser.error('Evidence output must have effective 0700 permissions, not HOME/hmdfs chmod')
    records = []
    result = {
        'schemaVersion': 1, 'status': 'FAIL',
        'scope': 'Real native editor export + hardened mono template_debug CLI GDScript TCP debugger protocol; not GUI/HAP/HDC/device or C# source debugging',
        'codec': 'Native editor GDScript TCPServer + PacketPeerStream; no Python Variant codec or mocked debugger',
        'inputs': {name: {'path': str(entry), 'sha256': digest(entry)} for name, entry in
                   [('editor', editor), ('template_debug', template_debug), ('template_zip', template_zip)]},
        'stages': records,
        'release_debug_rejection': {'status': 'not requested'},
    }
    if template_release:
        result['inputs']['template_release'] = {'path': str(template_release), 'sha256': digest(template_release)}
    for name, entry in [('editor', editor), ('template_debug', template_debug)] + ([('template_release', template_release)] if template_release else []):
        library = entry.parent / 'libgodot.so'
        result['inputs'][name]['library'] = {'path': str(library), 'size': library.stat().st_size, 'sha256': digest(library)}
    try:
        with tempfile.TemporaryDirectory(prefix='godot-debug-protocol-', dir=os.environ['TMPDIR']) as temporary:
            root = Path(temporary)
            project = root / 'game project with spaces'
            peer_project = root / 'protocol peer'
            project.mkdir(mode=0o700)
            peer_project.mkdir(mode=0o700)
            (project / 'project.godot').write_text(PROJECT)
            (project / 'Main.gd').write_text(MAIN)
            (project / 'Main.tscn').write_text(SCENE)
            (peer_project / 'project.godot').write_text('config_version=5\n[application]\nrun/flush_stdout_on_print=true\n')
            (peer_project / 'Peer.gd').write_text(PEER)
            env = dict(os.environ)
            for key in ('HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'GODOT_OHOS_DATA_DIR', 'GODOT_OHOS_CACHE_DIR', 'TMPDIR'):
                folder = root / ('env-' + key.lower())
                folder.mkdir(mode=0o700)
                env[key] = str(folder)
            # Use the supplied CLI's matching library, never an inherited path
            # that happens to contain another target's libgodot.so.
            inherited_libraries = os.environ.get('LD_LIBRARY_PATH', '')
            env.pop('LD_PRELOAD', None)
            env['LD_LIBRARY_PATH'] = str(editor.parent) + (os.pathsep + inherited_libraries if inherited_libraries else '')
            preset = '''[preset.0]
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
build/bundle_id="org.oheco.debugprotocolfixture"
build/sign=false
permissions/ohos.permission.INTERNET=true
''' % (json.dumps(str(template_zip)), json.dumps(str(template_zip)))
            (project / 'export_presets.cfg').write_text(preset)

            def run(stage, command, cwd=project, expected_zero=True, timeout=180, child_env=None):
                try:
                    completed = subprocess.run(command, cwd=cwd, env=child_env or env, capture_output=True, timeout=timeout)
                except subprocess.TimeoutExpired as error:
                    (output / (stage + '.log')).write_bytes((error.stdout or b'') + (error.stderr or b''))
                    records.append({'stage': stage, 'argv': command, 'exit_code': None, 'timeout_seconds': timeout})
                    raise
                text = completed.stdout + completed.stderr
                (output / (stage + '.log')).write_bytes(text)
                records.append({'stage': stage, 'argv': command, 'exit_code': completed.returncode})
                if expected_zero:
                    assert completed.returncode == 0, f'{stage} exited {completed.returncode}'
                    assert b'SCRIPT ERROR:' not in text, f'{stage} had a real script error'
                else:
                    assert completed.returncode != 0, f'{stage} unexpectedly accepted remote debugging'
                return completed

            run('editor-version', [str(editor), '--version'])
            run('import', [str(editor), '--headless', '--editor', '--path', str(project), '--import'])
            run('export-debug-project', [str(editor), '--headless', '--path', str(project), '--export-debug', 'OpenHarmony', str(root / 'Game.hap')])
            generated = root / 'Game'
            host = json.loads((generated / 'entry/src/main/resources/rawfile/godot_host.json').read_text())
            assert host['host']['role'] == 'game' and host['managed']['mode'] == 'none', host
            assert json.loads((generated / 'entry/src/main/module.json5').read_text())['module']['deviceTypes'] == ['default']
            assert json.loads((generated / 'build-profile.json5').read_text())['app']['signingConfigs'] == []
            runner = root / 'debug runner'
            runner.mkdir(mode=0o700)
            shutil.copy2(template_debug, runner / 'godot')
            shutil.copy2(template_debug.parent / 'libgodot.so', runner / 'libgodot.so')
            pck = runner / 'godot.pck'
            # Both switches are deliberate: export-pack alone defaults to release
            # and strips the stack locals required by this real debugger test.
            run('export-debug-pack', [str(editor), '--headless', '--path', str(project), '--export-debug', 'OpenHarmony', '--export-pack', 'OpenHarmony', str(pck)])
            assert pck.is_file() and pck.stat().st_size > 0
            result['pack'] = {'size': pck.stat().st_size, 'sha256': digest(pck), 'debug': True, 'adjacent': 'godot.pck'}
            peer_command = [str(editor), '--headless', '--max-fps', '60', '--path', str(peer_project), '--script', 'res://Peer.gd', '--', str(output / 'native-peer.json')]
            peer_process = None
            game_process = None
            with (output / 'native-peer.log').open('wb') as peer_log, (output / 'native-template-debug.log').open('wb') as game_log:
                try:
                    peer_process = subprocess.Popen(peer_command, cwd=peer_project, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                    port = wait_ready(peer_process, peer_log)
                    game_command = [str(runner / 'godot'), '--headless', '--remote-debug', f'tcp://127.0.0.1:{port}', '--breakpoints', 'res://Main.gd:4']
                    assert '--main-pack' not in game_command and '--path' not in game_command
                    game_env = dict(env)
                    game_env['LD_LIBRARY_PATH'] = str(runner) + (os.pathsep + inherited_libraries if inherited_libraries else '')
                    game_process = subprocess.Popen(game_command, cwd=runner, env=game_env, stdout=game_log, stderr=subprocess.STDOUT)
                    peer_code, game_code = drain_until_exit(peer_process, game_process, peer_log)
                    records.extend([
                        {'stage': 'native-peer', 'argv': peer_command, 'exit_code': peer_code},
                        {'stage': 'native-template-debug', 'argv': game_command, 'exit_code': game_code},
                    ])
                    assert peer_code == 0 and game_code == 0, (peer_code, game_code)
                finally:
                    terminate_owned(game_process)
                    terminate_owned(peer_process)
                    if peer_process is not None and peer_process.stdout is not None:
                        peer_process.stdout.close()
            protocol = json.loads((output / 'native-peer.json').read_text())
            assert protocol['status'] == 'PASS', protocol
            assert [(item['file'], item['line'], item['function'], item['answer'], item['local_scope'], item['variant_type']) for item in protocol['snapshots']] == [
                ('res://Main.gd', 4, '_ready', 40, 0, 2), ('res://Main.gd', 5, '_ready', 42, 0, 2)]
            assert any(node['name'] == 'Main' and node['id'] > 0 and node['scene_path'] == 'res://Main.tscn' for node in protocol['scene_nodes'])
            assert [item['command'] for item in protocol['commands']] == ['get_stack_dump', 'get_stack_frame_vars', 'scene:request_scene_tree', 'next', 'get_stack_dump', 'get_stack_frame_vars', 'continue']
            game_text = (output / 'native-template-debug.log').read_text(errors='replace')
            assert 'SCRIPT ERROR:' not in game_text
            assert 'DEBUG_PROTOCOL_VALUE=42' in game_text
            result['protocol'] = protocol
            if template_release:
                release_runner = root / 'release runner'
                release_runner.mkdir(mode=0o700)
                shutil.copy2(template_release, release_runner / 'godot')
                shutil.copy2(template_release.parent / 'libgodot.so', release_runner / 'libgodot.so')
                shutil.copyfile(pck, release_runner / 'godot.pck')
                release_env = dict(env)
                release_env['LD_LIBRARY_PATH'] = str(release_runner) + (os.pathsep + inherited_libraries if inherited_libraries else '')
                baseline = run('release-adjacent-pack-baseline', [str(release_runner / 'godot'), '--headless'], cwd=release_runner, child_env=release_env)
                assert b'DEBUG_PROTOCOL_VALUE=42' in baseline.stdout
                rejected = run('release-reject-remote-debug', [str(release_runner / 'godot'), '--headless', '--remote-debug', f'tcp://127.0.0.1:{port}'], cwd=release_runner, expected_zero=False, child_env=release_env)
                assert rejected.returncode > 0 and b'compiled without debug' in rejected.stdout + rejected.stderr
                result['release_debug_rejection'] = {'status': 'PASS', 'exit_code': rejected.returncode, 'baseline_exit_code': baseline.returncode, 'reason': 'compile-time no-debug guard'}
            (output / 'fixture-Main.gd').write_text(MAIN)
            (output / 'fixture-Peer.gd').write_text(PEER)
            result['temporary_processes_drained'] = True
        result['temporary_projects_removed'] = True
        result['status'] = 'PASS'
        print('PASS real native GDScript breakpoint line4/answer40 -> next line5/answer42, stack/locals, RemoteSceneTree, continue/exit0' + ('; release remote-debug rejected' if template_release else ''))
    except BaseException as error:
        result['failure'] = str(error)
        raise
    finally:
        (output / 'result.json').write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n')


if __name__ == '__main__':
    main()
