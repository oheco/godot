#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Validate real native-window rendering with an existing signed OpenHarmony library.

Builds/signs only the small ConsumerSurface runner, then boots the selected signed
engine through its normal OS/Main C++ entry. Editor cases use a loose project;
hardened templates require --editor-cli and --template-zip to export that same
scene through the actual OpenHarmony exporter, then automatically load runner.pck
beside runner.signed, with no --path/--main-pack or signed-ELF modification.
Project settings select the renderer; no rendering-method/driver command-line
override is used. Verifies 20 post-draw frames, 2D/3D/depth pixels, a real
SubViewport's readback and composited texture, vsync requests/getters, and >=10 acquired/released real
presentation buffers. --resize additionally requires 640x480 buffers after the
public display resize boundary. It does not measure display scanout synchronization
or cover ArkTS, HAP installation, UIAbility or visible ArkUI windows.

HOME is preserved. Project, isolated XDG/GODOT/OHECO paths, intermediates and the
signed runner live in TMPDIR and are removed. All persistent logs/PNG/JSON go to
--output, which must be a new directory with an existing parent. Neither the
selected library nor production sources are modified. ELF .codesign presence is
checked before loading; signature authentication remains the OS loader's job.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

from pck_audit import pack_index


SOURCE = Path(__file__).resolve().parents[3]
TESTS = Path(__file__).resolve().parent
MINIMUM_BUFFERS = 10
MINIMUM_RESIZED_BUFFERS = 5


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def signed_elf(path):
    """Check bounded ELF64/aarch64 section metadata, without executing the file."""
    size = path.stat().st_size
    with path.open('rb') as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', header, 18)[0] != 183:
            raise RuntimeError('Expected aarch64 ELF64: ' + str(path))
        section_offset = struct.unpack_from('<Q', header, 40)[0]
        section_size, section_count, names_index = struct.unpack_from('<HHH', header, 58)
        if section_size != 64 or not 0 < section_count < 65535 or names_index >= section_count:
            raise RuntimeError('Unsupported ELF section table: ' + str(path))
        if section_offset + section_count * section_size > size:
            raise RuntimeError('ELF section table exceeds file size: ' + str(path))
        stream.seek(section_offset)
        sections = [struct.unpack('<IIQQQQIIQQ', stream.read(64)) for _ in range(section_count)]
        names_section = sections[names_index]
        if names_section[5] > 1024 * 1024 or names_section[4] + names_section[5] > size:
            raise RuntimeError('Invalid ELF section-name table: ' + str(path))
        stream.seek(names_section[4])
        names = stream.read(names_section[5])
        for section in sections:
            if section[0] >= len(names):
                raise RuntimeError('Invalid ELF section-name offset: ' + str(path))
            name = names[section[0]:].split(b'\0', 1)[0]
            if name == b'.codesign' and section[5] > 0 and section[4] + section[5] <= size:
                return True
    return False


def isolated_environment(root):
    # Clear inherited engine/XDG overrides rather than passing user's settings to
    # the fixture. Keep the real HOME, including for native signing tools.
    environment = {name: value for name, value in os.environ.items()
                   if not name.startswith(('GODOT_', 'XDG_', 'DOTNET_', 'NUGET_', 'COREHOST_', 'COMPlus_', 'LD_', 'NATIVE_WINDOW_PROBE_'))}
    variables = ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'XDG_STATE_HOME',
                 'XDG_RUNTIME_DIR', 'XDG_CONFIG_DIRS', 'XDG_DATA_DIRS',
                 'GODOT_OHOS_DATA_DIR', 'GODOT_OHOS_CACHE_DIR', 'OHECO_ROOT', 'TMPDIR',
                 'DOTNET_CLI_HOME', 'NUGET_PACKAGES', 'NUGET_HTTP_CACHE_PATH',
                 'NUGET_PLUGINS_CACHE_PATH', 'DOTNET_BUNDLE_EXTRACT_BASE_DIR')
    for variable in variables:
        directory = root / variable.lower()
        directory.mkdir(mode=0o700)
        environment[variable] = str(directory)
    environment['TMP'] = environment['TMPDIR']
    environment['TEMP'] = environment['TMPDIR']
    environment.pop('LD_PRELOAD', None)
    environment['PYTHONDONTWRITEBYTECODE'] = '1'
    environment['DOTNET_CLI_TELEMETRY_OPTOUT'] = '1'
    return environment


def run(command, *, cwd, environment, log, timeout):
    with log.open('x', encoding='utf-8') as output:
        output.write('argv=' + json.dumps([str(value) for value in command]) + '\n')
        output.flush()
        result = subprocess.run([str(value) for value in command], cwd=cwd, env=environment,
                                stdout=output, stderr=subprocess.STDOUT, timeout=timeout)
    return result.returncode


def project_settings(method):
    return ('''config_version=5
[application]
config/name="Native window isolated graphics acceptance"
run/main_scene="res://rendering_window_scene.tscn"
config/use_custom_user_dir=true
config/custom_user_dir_name="native-window-fixture"
[display]
window/size/viewport_width=320
window/size/viewport_height=240
window/size/window_width_override=320
window/size/window_height_override=240
window/vsync/vsync_mode=0
[rendering]
renderer/rendering_method="''' + method + '''"
renderer/rendering_method.mobile="''' + method + '''"
textures/vram_compression/import_etc2_astc=true
''').encode()


def export_sdk_root(args):
    selected = args.sdk_root or os.environ.get('GODOT_OHOS_SDK_ROOT')
    if selected:
        return Path(selected).expanduser().resolve(strict=True)
    packages = Path(os.environ.get('OHECO_ROOT') or Path.home() / '.oheco')
    launcher = (packages / 'bin/hvigorw').resolve(strict=True)
    tools = next((parent for parent in launcher.parents if (parent / 'hvigor/bin/hvigorw.js').is_file()), None)
    if tools is None:
        raise RuntimeError('Select a real export SDK with --sdk-root or the active Command Line Tools')
    return (tools / 'sdk').resolve(strict=True)


def native_build(library, launcher=None):
    metadata = json.loads((library.parent / 'build-info.json').read_text(encoding='utf-8'))
    if metadata.get('libgodot.so_sha256') != sha256(library) or metadata.get('openharmony_host_abi') != 4:
        raise RuntimeError('Signed library hash/ABI differs from its build-info.json')
    if launcher and (metadata.get('godot_sha256') != sha256(launcher) or not signed_elf(launcher) or not signed_elf(library)):
        raise RuntimeError('Export Editor must match its signed launcher/library build-info.json')
    return metadata


def export_template_pack(args, project, runtime, environment, destination, receipt, export_sdk):
    editor = args.editor_cli.expanduser().resolve(strict=True)
    archive_path = args.template_zip.expanduser().resolve(strict=True)
    library = Path(receipt['library'])
    editor_library = editor.parent / 'libgodot.so'
    native = native_build(library)
    editor_native = native_build(editor_library, editor)
    if native['source_commit'] != editor_native['source_commit'] or native['engine_version'] != editor_native['engine_version']:
        raise RuntimeError('Export Editor and selected template library must be built from the same engine version/source')
    with zipfile.ZipFile(archive_path) as archive:
        metadata = json.loads(archive.read('entry/src/main/resources/rawfile/godot_template.json'))
        with archive.open('entry/libs/arm64-v8a/libgodot.so') as payload:
            payload_hash = hashlib.file_digest(payload, 'sha256').hexdigest()
    if (metadata.get('hostAbi') != native['openharmony_host_abi'] or metadata.get('target') != args.engine_target
            or metadata.get('architecture') != 'arm64-v8a' or receipt['driver'] not in metadata.get('renderingDrivers', ['vulkan'])
            or payload_hash != receipt['library_sha256']):
        raise RuntimeError('Actual template ZIP metadata/library must match the selected signed native engine')
    protected = (editor, editor_library, archive_path, editor.parent / 'build-info.json', library.parent / 'build-info.json')
    receipt.update(editor_cli=str(editor), editor_native_build=editor_native, native_build=native,
                   template_zip=str(archive_path), template_zip_sha256=sha256(archive_path), template_metadata=metadata,
                   template_library_sha256=payload_hash, export_sdk_root=str(export_sdk),
                   launch_input_sha256={str(path): sha256(path) for path in protected})
    # Only the Editor can accept --path. Its loader must not pick up the selected
    # template library from the native runner's LD_LIBRARY_PATH.
    export_environment = dict(environment, LD_LIBRARY_PATH=str(editor.parent), GODOT_OHOS_SDK_ROOT=str(export_sdk))
    version_command = [editor, '--version']
    receipt['editor_version_exit'] = run(version_command, cwd=project, environment=export_environment,
                                       log=destination / 'editor-version.log', timeout=60)
    version_lines = (destination / 'editor-version.log').read_text(encoding='utf-8').splitlines()
    if receipt['editor_version_exit'] or editor_native['engine_version'] not in version_lines:
        raise RuntimeError('Actual signed export Editor version differs from build-info.json')
    receipt['editor_version'] = editor_native['engine_version']
    preset = ('[preset.0]\nname="OpenHarmony"\nplatform="OpenHarmony"\nrunnable=true\n'
              'advanced_options=true\nexport_filter="all_resources"\ninclude_filter=""\nexclude_filter=""\n[preset.0.options]\n'
              'architectures/arm64=true\narchitectures/x86_64=false\n'
              'build/export_project_only=true\nbuild/override_project_dir=false\nbuild/sign=false\n'
              'build/bundle_id="org.oheco.renderingwindowfixture"\n')
    mode = 'debug' if args.engine_target == 'template_debug' else 'release'
    preset += 'custom_template/' + mode + '=' + json.dumps(str(archive_path)) + '\n'
    (project / 'export_presets.cfg').write_text(preset, encoding='utf-8')
    export_target = project.parent / 'Window.hap'
    command = [editor, '--headless', '--path', project, '--export-' + mode, 'OpenHarmony', export_target]
    receipt.update(export_argv=[str(value) for value in command], export_cwd=str(project),
                   export_project_only=True, product_build_requested=False, product_sign_requested=False)
    receipt['export_exit'] = run(command, cwd=project, environment=export_environment, log=destination / 'export.log', timeout=300)
    export_log = (destination / 'export.log').read_text(encoding='utf-8', errors='replace')
    if receipt['export_exit'] or 'SCRIPT ERROR:' in export_log or 'Build skipped as requested.' not in export_log or export_target.exists():
        raise RuntimeError('Actual project-only export failed or unexpectedly built a HAP; see export.log')
    generated = export_target.with_suffix('')
    pck = generated / 'entry/src/main/resources/rawfile/template.pck'
    entries = pack_index(pck)
    exported_library_hash = sha256(generated / 'entry/libs/arm64-v8a/libgodot.so')
    if exported_library_hash != payload_hash:
        raise RuntimeError('Actual exporter changed the signed template native payload')
    adjacent = runtime / 'runner.pck'  # Main strips the .signed extension from EXEC_PATH.
    shutil.copyfile(pck, adjacent)
    if sha256(adjacent) != sha256(pck):
        raise RuntimeError('Adjacent exported PCK copy differs from the actual export')
    receipt.update(exported_library_sha256=exported_library_hash, exported_pck=str(pck),
                   pck_sha256=sha256(pck), pck_size=pck.stat().st_size, sidecar_pck=str(adjacent),
                   sidecar_pck_sha256=sha256(adjacent),
                   pck_index=[dict(path=entry.path, size=entry.size, md5=entry.md5) for entry in entries])


def log_errors(text, engine_target):
    """Allow two exact, once-per-log diagnostics of this GDScript-only host."""
    lines = re.sub(r'\x1b\[[0-9;]*m', '', text).splitlines()
    errors, allowed = [], set()
    for index, line in enumerate(lines):
        message = line.strip()
        if not re.match(r'^(?:ERROR|SCRIPT ERROR|USER ERROR|FATAL(?: ERROR)?):', message):
            continue
        origin = lines[index + 1].strip() if index + 1 < len(lines) else ''
        policy = None
        # ConsumerSurface has no UIAbility to queue a window-mode request to.
        if (message == 'ERROR: Cannot queue window mode change.'
                and re.fullmatch(r'at: window_set_mode \(platform/openharmony/display_server_openharmony\.cpp:\d+\)', origin)):
            policy = 'consumer-without-uiability'
        # The isolated Editor runs only GDScript and deliberately has no .NET root.
        if (engine_target == 'editor' and message == 'ERROR: The host fxr folder does not exist: /usr/share/dotnet/host/fxr.'
                and re.fullmatch(r'at: try_get_path_from_dotnet_root \(modules/mono/editor/hostfxr_resolver\.cpp:\d+\)', origin)):
            policy = 'gdscript-editor-without-dotnet'
        expected = policy is not None and policy not in allowed
        if expected:
            allowed.add(policy)
        errors.append(dict(line=index + 1, message=message, origin=origin,
                           expected=expected, fixture_allowance=policy))
    return errors


def collect_result(destination, args, receipt):
    log_path = destination / 'runner.log'
    log = log_path.read_text(encoding='utf-8', errors='replace') if log_path.exists() else ''
    engine_log_path = destination / 'engine.log'
    engine_log = engine_log_path.read_text(encoding='utf-8', errors='replace') if engine_log_path.exists() else ''
    receipt['engine_log_present'] = bool(engine_log.strip())
    receipt['engine_log_sha256'] = sha256(engine_log_path) if engine_log_path.is_file() else None
    receipt['engine_errors'] = log_errors(engine_log, args.engine_target)
    receipt['runner_errors'] = log_errors(log, args.engine_target)
    receipt['unexpected_engine_errors'] = [error for error in receipt['engine_errors'] if not error['expected']]
    receipt['unexpected_runner_errors'] = [error for error in receipt['runner_errors'] if not error['expected']]
    binding_error = re.compile(r'gl_window_make_current|MAIN_WINDOW_ID|make.?current|(?:bind|release).*context|context.*(?:current|bind)', re.IGNORECASE)
    receipt['gl_context_binding_errors'] = [dict(log=source, **error) for source, errors in
                                          (('engine.log', receipt['engine_errors']), ('runner.log', receipt['runner_errors']))
                                          for error in errors if binding_error.search(error['message'] + ' ' + error['origin'])]
    expected_library = receipt['library']
    receipt['loaded_library_matches'] = bool(re.search(
        r'^ENGINE_LIBRARY path=' + re.escape(expected_library) + r' expected=' + re.escape(expected_library) + r' match=1$', log, re.MULTILINE))
    consumer = re.search(r'^CONSUMER_FINAL callbacks=(\d+) acquired=(\d+) released=(\d+) errors=(\d+) destroyed=(\d+)$', log, re.MULTILINE)
    if consumer:
        receipt['consumer'] = dict(zip(('callbacks', 'acquired', 'released', 'errors', 'destroyed'), map(int, consumer.groups())))
    sizes = re.search(r'^CONSUMER_SIZES initial=(\d+) resized=(\d+) invalid=(\d+)$', log, re.MULTILINE)
    if sizes:
        receipt['consumer_sizes'] = dict(zip(('initial', 'resized', 'invalid'), map(int, sizes.groups())))
    receipt['native_resize_observed'] = ('CONSUMER_RESIZE width=640 height=480 consumer=0 producer=0' in log
                                         and 'ENGINE_RESIZE iteration=8 width=640 height=480 public_display_resize=1' in log)
    # Both old and new EGL managers use these words in their diagnostics. VSync
    # acceptance checks requests/getters and absence of interval failures only.
    interval_error = re.compile(r'(?:cannot[^\n]*swap.?interval|eglSwapInterval[^\n]*(?:error|fail)|error[^\n]*swap.?interval)', re.IGNORECASE)
    receipt['egl_swap_interval_errors'] = interval_error.findall(log + '\n' + engine_log)
    graphics_path = destination / 'viewport-receipt.json'
    if graphics_path.exists():
        receipt['graphics'] = json.loads(graphics_path.read_text(encoding='utf-8'))
    expected_size = [640, 480] if args.resize else [320, 240]
    for prefix, filename in (('png', 'viewport.png'), ('offscreen_png', 'offscreen.png')):
        png = destination / filename
        if png.exists():
            with png.open('rb') as stream:
                header = stream.read(24)
            if len(header) == 24 and header[:8] == b'\x89PNG\r\n\x1a\n' and header[12:16] == b'IHDR':
                receipt[prefix + '_size'] = list(struct.unpack('>II', header[16:24]))
            receipt[prefix + '_sha256'] = sha256(png)
    receipt['crash_logs'] = sorted(path.name for path in destination.glob('*crash.log'))
    consumer_data = receipt.get('consumer', {})
    size_data = receipt.get('consumer_sizes', {})
    graphics = receipt.get('graphics', {})
    driver = 'opengl3' if args.method == 'gl_compatibility' else 'vulkan'
    resize_passed = (receipt['native_resize_observed'] and size_data.get('initial', 0) > 0
                     and size_data.get('resized', 0) >= MINIMUM_RESIZED_BUFFERS) if args.resize else size_data.get('initial', 0) >= MINIMUM_BUFFERS
    packed_ok = args.engine_target == 'editor' or (
        receipt.get('export_exit') == 0 and receipt.get('noPathOverrides') and receipt.get('runtime_no_loose_project')
        and receipt.get('pck_sha256') == receipt.get('sidecar_pck_sha256')
        and receipt.get('exported_library_sha256') == receipt['library_sha256']
        and receipt.get('template_library_sha256') == receipt['library_sha256'])
    receipt['pass'] = bool(
        packed_ok and receipt.get('launch_inputs_unchanged') and receipt.get('runner_unchanged')
        and receipt.get('run_exit') == 0 and receipt['loaded_library_matches'] and receipt.get('temporary_cleaned')
        and receipt.get('project_unchanged') and receipt.get('sources_unchanged') and receipt.get('library_unchanged')
        and not receipt['egl_swap_interval_errors'] and not receipt['crash_logs']
        and receipt['engine_log_present'] and not receipt['unexpected_engine_errors']
        and not receipt['unexpected_runner_errors'] and not receipt['gl_context_binding_errors']
        and graphics.get('pass') is True and graphics.get('method') == args.method and graphics.get('driver') == driver
        and graphics.get('viewport_size') == expected_size and graphics.get('frame_post_draw_waits') == 20
        and graphics.get('checks', {}).get('vsync_requests_and_getters') is True
        and graphics.get('offscreen_viewport_size') == [64, 64] and graphics.get('offscreen_update_always') is True
        and receipt.get('offscreen_png_size') == [64, 64]
        and all(graphics.get('checks', {}).get(check) is True for check in (
            'offscreen_image_valid', 'offscreen_size', 'offscreen_white', 'offscreen_magenta',
            'offscreen_composite_white', 'offscreen_composite_magenta', 'offscreen_png_saved'))
        and graphics.get('display_scanout_sync_measured') is False and receipt.get('png_size') == expected_size
        and consumer_data.get('acquired', 0) >= MINIMUM_BUFFERS
        and consumer_data.get('acquired') == consumer_data.get('released')
        and consumer_data.get('errors') == 0 and consumer_data.get('destroyed') == 1
        and size_data.get('invalid') == 0 and resize_passed)


def run_fixture(args, destination, receipt):
    if sys.platform != 'ohos' or os.uname().machine != 'aarch64':
        raise RuntimeError('Run on native aarch64 HarmonyOS')
    sdk = args.native_sdk.expanduser().resolve(strict=True)
    library = args.library.expanduser().resolve(strict=True)
    export_sdk = export_sdk_root(args) if args.engine_target != 'editor' else None
    if library.name.endswith(('.raw', '.unsigned')) or not signed_elf(library):
        raise RuntimeError('Refusing an unsigned/raw engine library; a pre-signed ELF with .codesign is required')
    compiler = sdk / 'llvm/bin/clang++'
    objcopy = sdk / 'llvm/bin/llvm-objcopy'
    signer = shutil.which('binary-sign-tool')
    if not compiler.is_file() or not objcopy.is_file() or not (sdk / 'sysroot').is_dir() or not signer:
        raise RuntimeError('Native SDK clang++, llvm-objcopy, sysroot and PATH binary-sign-tool are required')
    snapshots = {path: path.read_bytes() for path in (
        Path(__file__).resolve(), TESTS / 'native_rendering_window.cpp', TESTS / 'rendering_window_scene.gd',
        SOURCE / 'platform/openharmony/os_openharmony.h', SOURCE / 'platform/openharmony/display_server_openharmony.h',
        SOURCE / 'platform/openharmony/engine_host_openharmony.h')}
    receipt.update(timestamp_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                   library=str(library), library_sha256=sha256(library), library_codesign_present=True,
                   native_sdk=str(sdk), source=str(SOURCE), engine_target=args.engine_target,
                   method=args.method, driver='opengl3' if args.method == 'gl_compatibility' else 'vulkan',
                   renderer_selection='project.godot', renderer_cli_overrides=False,
                   project_launch='editor-loose-project' if args.engine_target == 'editor' else 'exported-adjacent-pck',
                   signed_elf_pack_appended=False, launch_input_sha256={},
                   render_thread=args.render_thread or 'engine-default', resize_requested=args.resize,
                   minimum_buffers=MINIMUM_BUFFERS, minimum_resized_buffers=MINIMUM_RESIZED_BUFFERS if args.resize else 0,
                   signed_execution_only=True, display_scanout_sync_measured=False,
                   scope='Real ConsumerSurface/native window and unmodified signed engine OS/Main; no ArkTS/HAP/UIAbility/visible ArkUI window coverage.',
                   source_sha256={str(path.relative_to(SOURCE)): hashlib.sha256(data).hexdigest() for path, data in snapshots.items()})
    temporary_root = Path(os.environ['TMPDIR']).resolve(strict=True)
    work = None
    try:
        with tempfile.TemporaryDirectory(prefix='godot-rendering-window-', dir=temporary_root) as temporary:
            work = Path(temporary).resolve(strict=True)
            if work.parent != temporary_root or not work.name.startswith('godot-rendering-window-'):
                raise RuntimeError('Unexpected native rendering fixture directory')
            receipt['temporary_root'] = str(work)
            try:
                environment = isolated_environment(work)
                environment['LD_LIBRARY_PATH'] = str(library.parent)
                environment['GODOT_RENDERING_WINDOW_LIBRARY'] = str(library)
                environment['GODOT_RENDERING_WINDOW_OUTPUT'] = str(destination)
                environment['GODOT_RENDERING_WINDOW_METHOD'] = args.method
                environment['GODOT_RENDERING_WINDOW_DRIVER'] = receipt['driver']
                environment['GODOT_RENDERING_WINDOW_RESIZE'] = '1' if args.resize else '0'
                receipt['home_preserved'] = environment.get('HOME') == os.environ.get('HOME')
                runtime = work / 'runtime'
                runtime.mkdir()
                unsigned, executable = work / 'runner.unsigned', runtime / 'runner.signed'
                defines = ['OPENHARMONY_ENABLED', 'UNIX_ENABLED', 'THREADS_ENABLED', '__OPEN_HARMONY__', 'VULKAN_ENABLED', 'RD_ENABLED', 'NDEBUG']
                if args.engine_target == 'editor':
                    defines.append('TOOLS_ENABLED')
                if args.engine_target != 'template_release':
                    defines += ['DEBUG_ENABLED', 'DEBUG_METHODS_ENABLED']
                command = [compiler, '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'), '-std=c++17', '-pthread']
                command += ['-D' + define for define in defines]
                command += ['-I' + str(SOURCE), '-I' + str(SOURCE / 'platform/openharmony'),
                            TESTS / 'native_rendering_window.cpp', library, '-lnative_image', '-lnative_window',
                            '-Wl,--no-code-sign', '-Xlinker', '-rpath', '-Xlinker', str(library.parent), '-o', unsigned]
                receipt['compile_defines'] = defines
                receipt['build_exit'] = run(command, cwd=work, environment=environment, log=destination / 'build.log', timeout=180)
                if receipt['build_exit']:
                    raise RuntimeError('Runner compilation failed; see build.log')
                receipt['strip_signature_exit'] = run([objcopy, '--remove-section=.codesign', unsigned], cwd=work,
                                                       environment=environment, log=destination / 'strip-signature.log', timeout=60)
                if receipt['strip_signature_exit']:
                    raise RuntimeError('Cannot remove automatic linker signature before final signing')
                receipt['sign_exit'] = run([signer, 'sign', '-inFile', unsigned, '-outFile', executable, '-selfSign', '1'],
                                           cwd=work, environment=environment, log=destination / 'sign.log', timeout=60)
                if receipt['sign_exit'] or not signed_elf(executable):
                    raise RuntimeError('Runner signing failed or signed ELF section is missing; see sign.log')
                executable.chmod(0o755)
                receipt['runner_sha256'] = sha256(executable)
                project = work / 'project'
                project.mkdir()
                project_bytes = project_settings(args.method)
                project_path = project / 'project.godot'
                project_path.write_bytes(project_bytes)
                (project / 'rendering_window_scene.tscn').write_text(
                    '[gd_scene load_steps=2 format=3]\n[ext_resource type="Script" path="res://rendering_window_scene.gd" id="1"]\n'
                    '[node name="NativeWindowFixture" type="Node3D"]\nscript = ExtResource("1")\n', encoding='utf-8')
                shutil.copyfile(TESTS / 'rendering_window_scene.gd', project / 'rendering_window_scene.gd')
                receipt['project_settings_sha256'] = hashlib.sha256(project_bytes).hexdigest()
                command = [executable]
                if args.engine_target == 'editor':
                    command += ['--path', project]
                else:
                    export_template_pack(args, project, runtime, environment, destination, receipt, export_sdk)
                receipt['runtime_no_loose_project'] = not (runtime / 'project.godot').exists() and not project.is_relative_to(runtime)
                receipt['runtime_entries_before_start'] = sorted(path.name for path in runtime.iterdir())
                receipt['noPathOverrides'] = '--path' not in command and '--main-pack' not in command
                if args.engine_target != 'editor' and (not receipt['noPathOverrides'] or not receipt['runtime_no_loose_project']
                        or receipt['runtime_entries_before_start'] != ['runner.pck', 'runner.signed']):
                    raise RuntimeError('Hardened template must use only its signed runner and adjacent real exported pack')
                command += ['--audio-driver', 'Dummy', '--resolution', '320x240',
                            '--log-file', destination / 'engine.log', '--verbose']
                if args.render_thread:
                    command += ['--render-thread', args.render_thread]
                receipt['run_argv'] = [str(value) for value in command]
                receipt['run_cwd'] = str(runtime)
                try:
                    receipt['run_exit'] = run(command, cwd=runtime, environment=environment, log=destination / 'runner.log', timeout=100)
                except subprocess.TimeoutExpired:
                    receipt['run_exit'] = 'timeout'
                receipt['project_unchanged'] = project_path.read_bytes() == project_bytes
                receipt['runner_unchanged'] = sha256(executable) == receipt['runner_sha256']
            finally:
                for index, crash in enumerate(work.rglob('godot-*-crash.log')):
                    shutil.copyfile(crash, destination / ('native-' + str(index) + '-' + crash.name))
                if work.resolve(strict=True) != work or work.parent != temporary_root:
                    raise RuntimeError('Native fixture cleanup path changed')
    finally:
        if work is not None:
            receipt['temporary_cleaned'] = not work.exists()
        receipt['sources_unchanged'] = all(path.read_bytes() == data for path, data in snapshots.items())
        receipt['library_unchanged'] = sha256(library) == receipt['library_sha256']
        receipt['launch_inputs_unchanged'] = all(sha256(Path(path)) == digest for path, digest in receipt['launch_input_sha256'].items())
    collect_result(destination, args, receipt)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-sdk', type=Path, required=True, help='Prepared native SDK with llvm/ and sysroot/')
    parser.add_argument('--library', type=Path, required=True, help='Existing signed libgodot.so for the selected target')
    parser.add_argument('--engine-target', choices=('editor', 'template_debug', 'template_release'), required=True)
    parser.add_argument('--editor-cli', type=Path, help='Signed Editor CLI for actual template project-only export')
    parser.add_argument('--template-zip', type=Path, help='Actual template ZIP containing exactly the selected signed library')
    parser.add_argument('--sdk-root', type=Path, help='Real export SDK root; defaults to GODOT_OHOS_SDK_ROOT or the active tools bundled SDK')
    parser.add_argument('--method', choices=('mobile', 'forward_plus', 'gl_compatibility'), required=True)
    parser.add_argument('--output', type=Path, required=True, help='NEW output directory; parent must already exist')
    parser.add_argument('--render-thread', choices=('safe', 'separate'))
    parser.add_argument('--resize', action='store_true', help='Resize the real producer/display to 640x480 at loop iteration 8')
    args = parser.parse_args()
    if args.engine_target != 'editor' and (not args.editor_cli or not args.template_zip):
        parser.error('Template cases require --editor-cli and --template-zip for a real exported adjacent PCK')
    if args.engine_target == 'editor' and any((args.editor_cli, args.template_zip, args.sdk_root)):
        parser.error('--editor-cli, --template-zip and --sdk-root are for template cases')
    requested = args.output.expanduser()
    if requested.exists() or requested.is_symlink():
        parser.error('--output must be a new directory; existing evidence is never overwritten')
    destination = requested.parent.resolve(strict=True) / requested.name
    destination.mkdir(mode=0o700, exist_ok=False)
    receipt = {'test': 'rendering_window', 'pass': False, 'output': str(destination)}
    try:
        run_fixture(args, destination, receipt)
    except (OSError, ValueError, RuntimeError, KeyError, struct.error, subprocess.TimeoutExpired) as error:
        receipt.update({'pass': False, 'error': str(error)})
    with (destination / 'runner-receipt.json').open('x', encoding='utf-8') as output:
        json.dump(receipt, output, indent=2, sort_keys=True)
        output.write('\n')
    print(json.dumps(receipt, indent=2, sort_keys=True), flush=True)
    return 0 if receipt['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
