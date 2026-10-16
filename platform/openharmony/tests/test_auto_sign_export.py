#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Exercise the real C++ automatic signing bridge with a local fake JSON backend.

Requires a newly rebuilt, already signed Editor library with the Dictionary
export_project_helper signature. Only the disposable fixture/bridge executable
and native fake backend are self-signed; --library is read and reused unchanged.
The native fake backend is compiled and self-signed once. Each case copies a real
debug template ZIP, removes tools/auto-sign.cjs, and adds the same tools/auto-sign
ELF plus tools/auto-sign-fixture.json (both omitted for old_template). All other
entries retain their bytes and ZIP provenance. The real Editor exports a
project/PCK, invokes the fake ELF directly, validates its JSON and copies or
rejects fake HAP bytes. This does not prove real signing or device installation.
No SCons, real CLI/HDC invocation, login, cloud issuance, certificates or release.

--compile-only compiles fixture and bridge objects, prepares/checks the backend
fixtures, and never links or runs the Editor. It is suitable for the old library
while Lead prepares the final signed Editor build. It never prints a run PASS.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile
import zipfile


SECRET_PREFIX = 'AUTO_SIGN_FIXTURE_PRIVATE_'
AUTH_MESSAGE = 'Run the local DevEco CLI login command, then retry Remote Deploy.'
CASES = (
    ('ok', 'ok'),
    ('auth_required', 'auth'),
    ('confirmation_required', 'legacy_confirmation'),
    ('invalid_json', 'invalid_json'),
    ('schema_number', 'invalid'),
    ('schema_string', 'invalid'),
    ('signed_false', 'invalid'),
    ('signed_string', 'invalid'),
    ('profile_false', 'invalid'),
    ('profile_string', 'invalid'),
    ('wrong_format', 'invalid'),
    ('wrong_mode', 'invalid'),
    ('outside_project', 'invalid'),
    ('path_traversal', 'invalid'),
    ('nonzero_exit', 'invalid'),
    ('missing_artifact', 'invalid'),
    ('old_template', 'old_template'),
)

# Case selection and expected public metadata live beside the ELF, never in a
# private field smuggled into the production request schema. No Node is executed
# by this frontend fixture; real Node version enforcement belongs to backend tests.
NATIVE_HELPER = 'tools/auto-sign'
FIXTURE_CONFIG = 'tools/auto-sign-fixture.json'
HELPER_PATHS = frozenset(('tools/auto-sign.cjs', NATIVE_HELPER, FIXTURE_CONFIG))


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def safe_log(text):
    return re.sub(re.escape(SECRET_PREFIX) + r'[^\s"\x27,}]*', '[REDACTED_FIXTURE_VALUE]', text)


def text_output(value):
    return value.decode('utf-8', errors='replace') if isinstance(value, bytes) else (value or '')


def collect_cases(root, summary):
    journal = root / 'case-results.jsonl'
    if not journal.is_file():
        return
    cases = []
    for number, line in enumerate(journal.read_text().splitlines(), start=1):
        if not line:
            continue
        try:
            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError('Case result is not an object')
            cases.append(value)
        except ValueError:
            summary.setdefault('journalErrors', []).append('Invalid case result at line %d' % number)
    summary['cases'] = cases


def run_native(command, root, environment):
    # Own a distinct session/process group so a hung backend descendant cannot
    # outlive a timed-out fixture and continue touching its deleted directory.
    with subprocess.Popen(command, cwd=root, env=environment, text=True,
                          stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, start_new_session=True) as process:
        def stop_group():
            if process.pid <= 1 or process.pid == os.getpgrp():
                raise RuntimeError('Unexpected native fixture process group')
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

        timed_out = False
        try:
            stdout, stderr = process.communicate(timeout=300)
        except subprocess.TimeoutExpired:
            timed_out = True
            stop_group()
            stdout, stderr = process.communicate(timeout=10)
        except BaseException:
            stop_group()
            process.communicate(timeout=10)
            raise
        return subprocess.CompletedProcess(command, process.returncode, stdout, stderr), timed_out


def isolated_environment(root):
    environment = dict(os.environ)
    # Remove inherited selection/injection knobs; select the tools explicitly.
    for key in tuple(environment):
        if (key.startswith(('GODOT_OHOS_', 'GODOT_AUTO_SIGN_', 'DEVECO_', 'OHOS_'))
                or key in ('NODE_OPTIONS', 'NODE_PATH', 'XDG_CONFIG_DIRS', 'XDG_DATA_DIRS',
                           'LD_LIBRARY_PATH', 'LD_PRELOAD', 'LD_AUDIT')):
            environment.pop(key)
    for variable in ('HOME', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME',
                     'XDG_STATE_HOME', 'XDG_RUNTIME_DIR', 'GODOT_OHOS_DATA_DIR',
                     'GODOT_OHOS_CACHE_DIR', 'TMPDIR', 'TMP', 'TEMP'):
        location = root / variable.lower()
        location.mkdir(mode=0o700)
        environment[variable] = str(location)
    environment.update(GODOT_AUTO_SIGN_EXPORT_ROOT=str(root), LANG='C.UTF-8', LANGUAGE='en', LC_ALL='C.UTF-8')
    return environment


def make_metadata(root):
    sdk = root / 'sdk'
    for component in ('native', 'ets', 'js', 'toolchains', 'previewer'):
        target = sdk / '26.0.0' / component / 'oh-uni-package.json'
        target.parent.mkdir(parents=True)
        target.write_text(json.dumps(dict(apiVersion=26, platformVersion='26.0.0',
                                         version='26.0.0.105', releaseType='Release', path=component)))
    metadata = root / 'metadata'
    metadata.mkdir()
    # Public placeholders must never be executed. The native fake uses neither.
    (metadata / 'hdc-do-not-execute').write_text('Fixture public HDC placeholder. Never execute.\n')
    (metadata / 'cli-do-not-execute.cjs').write_text('throw new Error("Fixture CLI must never execute");\n')
    (root / 'login-unused').mkdir(mode=0o700)


def zip_provenance(archive, item):
    with archive.open(item) as stream:
        sha256 = hashlib.file_digest(stream, 'sha256').hexdigest()
    metadata = tuple(getattr(item, key) for key in (
        'date_time', 'compress_type', 'comment', 'extra', 'create_system',
        'create_version', 'extract_version', 'external_attr', 'internal_attr'))
    return sha256, metadata


def fixture_entry(name, data, executable=False):
    item = zipfile.ZipInfo(name)
    item.create_system = 3
    item.external_attr = (0o100755 if executable else 0o100644) << 16
    item.compress_type = zipfile.ZIP_DEFLATED
    return item, data


def prepare_fixtures(root, template, tools, backend):
    project = root / 'project'
    project.mkdir()
    (project / 'project.godot').write_text(
        'config_version=5\n[application]\nconfig/name="Native automatic signing protocol fixture"\n'
        'run/main_scene="res://main.tscn"\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n')
    (project / 'main.tscn').write_text('[gd_scene format=3]\n[node name="Fixture" type="Node"]\n')
    (root / 'exports').mkdir()
    (root / 'templates').mkdir()
    make_metadata(root)
    expected = dict(schemaVersion=1, projectIdentity=str(project / 'project.godot'),
                    nodePath=str(tools / 'node/bin/node'), commandLineToolsRoot=str(tools),
                    sdkRoot=str(root / 'sdk'), hdcPath=str(root / 'metadata/hdc-do-not-execute'),
                    target='native-auto-sign-fake-serial', format='hap', buildMode='debug',
                    cliPath=str(root / 'metadata/cli-do-not-execute.cjs'), teamId='public-fixture-team',
                    loginDataDir=str(root / 'login-unused'))
    cases = []
    binary = backend.read_bytes()
    binary_sha256 = hashlib.sha256(binary).hexdigest()
    with zipfile.ZipFile(template) as source:
        names = source.namelist()
        if len(set(names)) != len(names):
            raise ValueError('The source debug template has duplicate entries')
        required = ('AppScope/app.json5', 'build-profile.json5', 'entry/src/main/module.json5',
                    'entry/src/main/resources/rawfile/godot_template.json', 'tools/build.cjs')
        if any(name not in names for name in required):
            raise ValueError('Select a real current OpenHarmony DebugTemplate ZIP')
        baseline = {item.filename: zip_provenance(source, item) for item in source.infolist()
                    if item.filename not in HELPER_PATHS}
        for name, expected_result in CASES:
            case_expected = dict(expected, projectDir=str(root / 'exports' / name))
            config = dict(schemaVersion=1, case=name, expected=case_expected)
            destination = root / 'templates' / (name + '.zip')
            with zipfile.ZipFile(destination, 'w') as target:
                target.comment = source.comment
                for item in source.infolist():
                    if item.filename not in HELPER_PATHS:
                        # Preserve non-helper bytes and ZIP provenance while
                        # streaming the real template. Never alter its source.
                        with source.open(item) as before, target.open(copy.copy(item), 'w') as after:
                            shutil.copyfileobj(before, after, length=1024 * 1024)
                if name != 'old_template':
                    target.writestr(*fixture_entry(NATIVE_HELPER, binary, executable=True))
                    target.writestr(*fixture_entry(FIXTURE_CONFIG, json.dumps(config) + '\n'))
            with zipfile.ZipFile(destination) as copied:
                copied_names = copied.namelist()
                copied_info = {item.filename: zip_provenance(copied, item) for item in copied.infolist()
                               if item.filename not in HELPER_PATHS}
                if copied_info != baseline:
                    raise ValueError('Template copy unexpectedly changed non-helper bytes or provenance')
                if ('tools/auto-sign.cjs' in copied_names or
                        (NATIVE_HELPER in copied_names) != (name != 'old_template') or
                        (FIXTURE_CONFIG in copied_names) != (name != 'old_template')):
                    raise ValueError('Template native helper replacement failed')
                if name != 'old_template':
                    if hashlib.sha256(copied.read(NATIVE_HELPER)).hexdigest() != binary_sha256:
                        raise ValueError('Cases must contain the same signed native fake backend')
                    if not copied.getinfo(NATIVE_HELPER).external_attr >> 16 & 0o111:
                        raise ValueError('Native fake backend ZIP entry is not executable')
                    if json.loads(copied.read(FIXTURE_CONFIG)) != config:
                        raise ValueError('Native fake backend public configuration changed')
            cases.append(dict(name=name, expected=expected_result, template=str(destination)))
    (root / 'manifest.json').write_text(json.dumps(dict(schemaVersion=1, commandLineToolsRoot=str(tools), cases=cases)))
    return project


def build_backend(repo, sdk, root, signer, run):
    source = repo / 'platform/openharmony/tests/native_auto_sign_fixture/backend.cpp'
    jsoncpp = repo / 'thirdparty/openxr/src/external/jsoncpp'
    dependencies = sorted((jsoncpp / 'src/lib_json').glob('*.cpp'))
    if not dependencies:
        raise RuntimeError('Vendored JsonCpp sources are required for the native fake backend')
    unsigned, binary = root / 'auto-sign.unsigned', root / 'auto-sign'
    run([sdk / 'llvm/bin/clang++', '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
         '-std=c++17', '-O2', '-pthread', '-static-libstdc++', '-I' + str(jsoncpp / 'include'),
         source, *dependencies, '-Wl,--no-code-sign', '-o', unsigned], timeout=120)
    header = run([sdk / 'llvm/bin/llvm-readelf', '--file-header', unsigned]).stdout
    dynamic = run([sdk / 'llvm/bin/llvm-readelf', '--dynamic', unsigned]).stdout
    if 'AArch64' not in header or 'libc++_shared.so' in dynamic:
        raise RuntimeError('Native fake backend must be ARM64 ELF with a static C++ runtime')
    run([sdk / 'llvm/bin/llvm-objcopy', '--remove-section=.codesign', unsigned])
    run([signer, 'sign', '-selfSign', '1', '-inFile', unsigned, '-outFile', binary])
    binary.chmod(0o755)
    sections = run([sdk / 'llvm/bin/llvm-readelf', '--sections', binary]).stdout
    if not re.search(r'\s\.codesign\s', sections):
        raise RuntimeError('Self-signed native fake backend has no .codesign section')
    metadata = dict(source=str(source.relative_to(repo)), sourceSha256=digest(source),
                    binarySha256=digest(binary), signed=True, staticCppRuntime=True, machine='AArch64',
                    dependencies=[dict(source=str(path.relative_to(repo)), sha256=digest(path))
                                  for path in dependencies])
    return binary, metadata


def build_objects(repo, sdk, root, environment, run):
    compiler = sdk / 'llvm/bin/clang++'
    flags = [str(compiler), '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
             '-std=c++17', '-DOPENHARMONY_ENABLED', '-DUNIX_ENABLED', '-DDEBUG_ENABLED',
             '-DDEBUG_METHODS_ENABLED', '-DTOOLS_ENABLED', '-DTHREADS_ENABLED',
             '-D__OPEN_HARMONY__', '-DVULKAN_ENABLED', '-DRD_ENABLED', '-pthread',
             '-I' + str(repo), '-I' + str(repo / 'platform/openharmony')]
    objects = []
    for source, name in ((repo / 'platform/openharmony/tests/native_auto_sign_export.cpp', 'fixture.o'),
                         (repo / 'platform/openharmony/bridge_openharmony.cpp', 'bridge.o')):
        output = root / name
        run(flags + ['-c', str(source), '-o', str(output)], timeout=120)
        objects.append(output)
    return flags, objects


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--sdk', type=Path, required=True, help='Independent native SDK; no automatic linker signing')
    parser.add_argument('--library', type=Path, required=True, help='Final already signed Editor libgodot.so; never modified')
    parser.add_argument('--template', type=Path, required=True, help='Real current OpenHarmony debug game template ZIP')
    parser.add_argument('--command-line-tools-root', type=Path, required=True, help='Official API26 CLT root with real native Node')
    parser.add_argument('--output', type=Path, help='New directory inside TMPDIR; records source, binary, logs and case results')
    parser.add_argument('--compile-only', action='store_true', help='Compile/check fixtures without linking or running the Editor')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    sdk = args.sdk.resolve(strict=True)
    library = args.library.resolve(strict=True)
    template = args.template.resolve(strict=True)
    tools = args.command_line_tools_root.resolve(strict=True)
    signer = shutil.which('binary-sign-tool')
    for tool in ('clang++', 'llvm-objcopy', 'llvm-readelf'):
        if not (sdk / 'llvm/bin' / tool).is_file():
            parser.error('Selected independent native SDK must provide clang++, llvm-objcopy and llvm-readelf')
    if not signer:
        parser.error('PATH binary-sign-tool is required to sign the disposable fixture/bridge and fake backend')
    if not (tools / 'node/bin/node').is_file() or not (tools / 'hvigor/bin/hvigorw.js').is_file():
        parser.error('Select the official API26 command-line-tools root with native Node and Hvigor metadata')
    if not library.is_file() or not template.is_file():
        parser.error('--library and --template must be existing files')
    temporary_root = Path(os.environ['TMPDIR']).resolve(strict=True)
    if args.output:
        evidence = args.output.resolve()
        if evidence == temporary_root or not evidence.is_relative_to(temporary_root):
            parser.error('--output evidence must be a new directory inside TMPDIR')
        if evidence.exists():
            parser.error('--output must be a new directory to avoid overwriting prior evidence')
        evidence.mkdir(parents=True)
    else:
        evidence = None
    summary = dict(schemaVersion=1, compileOnly=args.compile_only, runtimeExecuted=False,
                   library=str(library), librarySha256=digest(library), template=str(template),
                   templateSha256=digest(template), sdk=str(sdk), commandLineToolsRoot=str(tools),
                   cases=[], passed=False, buildCommands=[])
    build_log = []
    native_stdout = native_stderr = ''
    exit_code = 1
    try:
        with tempfile.TemporaryDirectory(prefix='godot-auto-sign-export-', dir=temporary_root) as temporary:
            root = Path(temporary).resolve(strict=True)
            if root.parent != temporary_root or not root.name.startswith('godot-auto-sign-export-'):
                raise RuntimeError('Unexpected native export fixture directory')
            try:
                environment = isolated_environment(root)
                environment['GODOT_AUTO_SIGN_EXPORT_LIBRARY'] = str(library)

                def run(command, timeout=60):
                    command = [str(part) for part in command]
                    record = dict(command=command, timeoutSeconds=timeout)
                    summary['buildCommands'].append(record)
                    build_log.append(json.dumps(command) + '\n')
                    try:
                        result = subprocess.run(command, cwd=root, env=environment,
                                                text=True, capture_output=True, timeout=timeout)
                    except subprocess.TimeoutExpired as error:
                        record['timedOut'] = True
                        build_log.append(safe_log(text_output(error.stdout) + text_output(error.stderr)))
                        raise RuntimeError('Native fixture build/check timed out') from None
                    record['exitCode'] = result.returncode
                    build_log.append(safe_log(result.stdout + result.stderr))
                    if result.returncode:
                        raise RuntimeError('Native fixture build/check failed (exit %d)' % result.returncode)
                    return result

                # Check the supplied library has signing data, without invoking
                # any signer/objcopy on it. The caller provides a valid signature.
                sections = run([sdk / 'llvm/bin/llvm-readelf', '--sections', library]).stdout
                if not re.search(r'\s\.codesign\s', sections):
                    raise RuntimeError('--library must already be signed (.codesign section missing)')
                flags, objects = build_objects(repo, sdk, root, environment, run)
                backend, summary['fakeBinary'] = build_backend(repo, sdk, root, signer, run)
                if evidence:
                    shutil.copyfile(repo / summary['fakeBinary']['source'], evidence / 'fake-backend.cpp')
                    shutil.copyfile(backend, evidence / 'fake-auto-sign')
                    summary['fakeBinary']['evidenceBinary'] = str(evidence / 'fake-auto-sign')
                    summary['fakeBinary']['evidenceSource'] = str(evidence / 'fake-backend.cpp')
                project = prepare_fixtures(root, template, tools, backend)
                summary['fixturesPrepared'] = len(CASES)
                summary['objectsCompiled'] = len(objects)
                if args.compile_only:
                    summary['passed'] = True
                    print('AUTO_SIGN_EXPORT_COMPILE_ONLY objects=2 native-backend=signed fixtures=%d runtime=not-executed' % len(CASES))
                    exit_code = 0
                else:
                    unsigned, executable = root / 'fixture.unsigned', root / 'fixture'
                    # The Editor SONAME is libgodot.so even when its build output
                    # has a longer filename. Resolve that name only in this
                    # isolated directory, without copying or resigning the input.
                    (root / 'libgodot.so').symlink_to(library)
                    run(flags + [str(part) for part in objects] + [str(library), '-Wl,--no-code-sign',
                        '-Wl,-rpath,' + str(root), '-o', str(unsigned)], timeout=120)
                    # Strip only the disposable executable's reserved section,
                    # then self-sign fixture and bridge together with PATH tool.
                    run([sdk / 'llvm/bin/llvm-objcopy', '--remove-section=.codesign', unsigned])
                    run([signer, 'sign', '-selfSign', '1', '-inFile', unsigned, '-outFile', executable])
                    executable.chmod(0o755)
                    summary['runtimeExecuted'] = True
                    result, timed_out = run_native([str(executable), '--editor', '--headless', '--path', str(project)],
                                                   root, environment)
                    summary['runtimeTimedOut'] = timed_out
                    leaked = SECRET_PREFIX in result.stdout or SECRET_PREFIX in result.stderr
                    native_stdout, native_stderr = safe_log(result.stdout), safe_log(result.stderr)
                    print(native_stdout, end='')
                    if native_stderr:
                        print(native_stderr, end='')
                    collect_cases(root, summary)
                    summary['processExitCode'] = result.returncode
                    summary['privateMarkerLeaked'] = leaked
                    summary['passed'] = (result.returncode == 0 and not leaked and not timed_out
                                         and not summary.get('journalErrors')
                                         and 'AUTO_SIGN_EXPORT_PASS failures=0' in result.stdout
                                         and [case['case'] for case in summary['cases']] == [name for name, _ in CASES]
                                         and all(case['passed'] for case in summary['cases']))
                    if not summary['passed']:
                        raise RuntimeError('Native automatic signing protocol regression did not pass')
                    exit_code = 0
            finally:
                # Preserve flushed case evidence even if a later build/run step
                # raises, before TemporaryDirectory removes the fixture root.
                if not summary['cases']:
                    collect_cases(root, summary)
                # TemporaryDirectory deletes only this exact checked absolute
                # target on the private temporary filesystem.
                if root.parent != temporary_root or root.resolve(strict=True) != Path(temporary).resolve(strict=True):
                    raise RuntimeError('Unexpected native fixture cleanup target')
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        summary['error'] = safe_log(str(error))
        print('AUTO_SIGN_EXPORT_RUNNER_FAIL ' + summary['error'])
        if build_log:
            print(''.join(build_log[-2:]), end='')
    finally:
        # Hash comparison detects accidental writes without ever signing or
        # rewriting either caller-owned input artifact.
        summary['libraryPreserved'] = digest(library) == summary['librarySha256']
        summary['templatePreserved'] = digest(template) == summary['templateSha256']
        if not summary['libraryPreserved'] or not summary['templatePreserved']:
            summary['passed'] = False
            exit_code = 1
        if evidence:
            (evidence / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
            (evidence / 'build.log').write_text(''.join(build_log))
            (evidence / 'native.stdout.log').write_text(native_stdout)
            (evidence / 'native.stderr.log').write_text(native_stderr)
            (evidence / 'case-results.jsonl').write_text(''.join(json.dumps(case) + '\n' for case in summary['cases']))
            print('AUTO_SIGN_EXPORT_EVIDENCE ' + str(evidence))
    raise SystemExit(exit_code)


if __name__ == '__main__':
    main()
