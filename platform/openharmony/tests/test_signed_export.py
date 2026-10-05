#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Native disposable-signing gate for the real C++ OpenHarmony exporter.

Uses existing signed Godot editor, complete debug/release template ZIPs, SDK26
native hap-sign-tool and offline oo Hvigor dependencies. Generates one-time CA,
app/profile certificates and PKCS12 material only in private TMPDIR. Exports and
verifies signed debug HAP and release APP including EACH inner HAP. Optional
wrong-password rejection is enabled by default. No personal material, device
trust claim, HDC/install, Java, SDK download, publish or persistent signed package.
Evidence contains only sanitized logs, artifact hashes and verification checks.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import secrets
import signal
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import uuid
import zipfile

BUNDLE = 'org.oheco.signedexportfixture'
PASSWORD_KEYS = {'keystorePwd', 'keyPwd', 'issuerKeyPwd'}
PEM = re.compile(r'-----BEGIN [^-]+-----[\s\S]*?-----END [^-]+-----')


def sha(filename):
    with filename.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def private_write(filename, text):
    filename.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd = os.open(filename, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w', encoding='utf-8') as stream:
        stream.write(text)
    if stat.S_IMODE(filename.stat().st_mode) != 0o600:
        raise RuntimeError('Temporary credential/document requires effective 0600')


def safe_app_haps(archive, destination):
    """Validate ZIP names/types/limits before copying only inner HAP members."""
    output = []
    names = set()
    total = 0
    with zipfile.ZipFile(archive) as package:
        for member in package.infolist():
            name = member.filename
            path = PurePosixPath(name)
            if not name or path.is_absolute() or '\\' in name or ':' in name or '\0' in name or '..' in path.parts:
                raise RuntimeError('Unsafe APP ZIP member path')
            normalized = name.casefold().rstrip('/')
            if normalized in names:
                raise RuntimeError('Duplicate/case-conflicting APP ZIP member')
            names.add(normalized)
            kind = stat.S_IFMT(member.external_attr >> 16)
            if kind not in (0, stat.S_IFREG, stat.S_IFDIR) or member.flag_bits & 1:
                raise RuntimeError('Unsafe/encrypted APP ZIP member')
            total += member.file_size
            if member.file_size > 1024 * 1024 * 1024 or total > 2 * 1024 * 1024 * 1024:
                raise RuntimeError('APP ZIP size limit exceeded')
            if not member.is_dir() and name.lower().endswith('.hap'):
                target = destination / ('inner-' + str(len(output)) + '.hap')
                with package.open(member) as source, target.open('xb') as sink:
                    shutil.copyfileobj(source, sink, 1024 * 1024)
                output.append((name, target))
    if not output:
        raise RuntimeError('Signed APP has no inner HAP to verify')
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--editor', type=Path, required=True)
    parser.add_argument('--template-debug-zip', type=Path, required=True)
    parser.add_argument('--template-release-zip', type=Path, required=True)
    parser.add_argument('--sdk-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New application-private evidence directory; no material/packages saved')
    parser.add_argument('--skip-wrong-password', action='store_true', help='Skip optional negative native export (positive signing remains mandatory)')
    parser.add_argument('--build-timeout', type=int, default=600, help='Per-export native deadline in seconds (default 600)')
    args = parser.parse_args()
    if sys.platform != 'ohos':
        parser.error('Run on native OpenHarmony')
    editor = args.editor.resolve(strict=True)
    debug_zip = args.template_debug_zip.resolve(strict=True)
    release_zip = args.template_release_zip.resolve(strict=True)
    sdk = args.sdk_root.resolve(strict=True)
    signer = sdk / '26.0.0/toolchains/lib/hap-sign-tool'
    if not signer.is_file():
        parser.error('SDK root must provide SDK26 native toolchains/lib/hap-sign-tool')
    if not (editor.parent / 'libgodot.so').is_file():
        parser.error('Editor needs its adjacent signed libgodot.so')
    tmpdir = os.environ.get('TMPDIR')
    if not tmpdir or not Path(tmpdir).is_dir():
        parser.error('TMPDIR must be an existing permission-capable private temporary directory')
    runner_source = Path(__file__).resolve().parents[3] / 'misc/dist/openharmony_template/tools/build.cjs'
    runner_bytes = runner_source.read_bytes()
    for template in (debug_zip, release_zip):
        with zipfile.ZipFile(template) as archive:
            if archive.read('tools/build.cjs') != runner_bytes:
                parser.error('Template ZIP runner differs from current source; repackage final templates before this gate')
    args.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    output = args.output.resolve()
    if stat.S_IMODE(output.stat().st_mode) != 0o700:
        parser.error('Evidence must have effective 0700 permissions; do not use HOME/hmdfs chmod')
    records = []
    result = {'schemaVersion': 1, 'status': 'FAIL',
              'scope': 'Real native C++ exporter -> offline oo Hvigor -> disposable native signed HAP/APP verification; file-signing only, NOT device installation trust',
              'personal_material': False, 'network_downloads': False, 'installation': False,
              'inputs': {name: {'sha256': sha(filename)} for name, filename in [
                  ('editor', editor), ('editor_library', editor.parent / 'libgodot.so'),
                  ('debug_template', debug_zip), ('release_template', release_zip), ('native_signer', signer), ('runner', runner_source)]},
              'stages': records, 'artifacts': [], 'wrong_password': {'status': 'not requested'}}
    password = 'Disposable Export With Spaces ' + secrets.token_hex(24)
    wrong_password = 'Incorrect Export With Spaces ' + secrets.token_hex(24)
    secret_values = [password, wrong_password, password.split()[-1], wrong_password.split()[-1]]
    private_root = None

    def sanitize(text):
        for value in secret_values:
            text = text.replace(value, '[REDACTED]')
        # Some native tools alter the first byte of password argv in /proc;
        # matching only the complete original value is not sufficient there.
        text = re.sub(r'(-(?:keystorePwd|keyPwd|issuerKeyPwd)\s+).*?(?=\s+-[A-Za-z]|$)',
                      r'\1[REDACTED]', text, flags=re.DOTALL)
        text = PEM.sub('[REDACTED CERTIFICATE/KEY MATERIAL]', text)
        if '"bundle-info"' in text or '"development-certificate"' in text:
            text = '[Native signer profile output omitted; exit/check recorded]\n'
        return text

    try:
        with tempfile.TemporaryDirectory(prefix='godot-signed-export-', dir=tmpdir) as temporary:
            root = Path(temporary)
            private_root = root
            if stat.S_IMODE(root.stat().st_mode) != 0o700:
                raise RuntimeError('Disposable signing root is not private 0700')
            secret_values.append(str(root))
            material = root / 'disposable signing material'
            material.mkdir(mode=0o700)
            store = material / 'fixture.p12'
            project = root / 'source project with spaces'
            project.mkdir(mode=0o700)
            # Freeze all inputs before invoking the real exporter. No ZIP is
            # rewritten by this test; copied archive bytes remain exact.
            frozen = root / 'frozen template inputs with spaces'
            frozen.mkdir(mode=0o700)
            shutil.copyfile(debug_zip, frozen / 'debug.zip')
            shutil.copyfile(release_zip, frozen / 'release.zip')
            debug_zip = frozen / 'debug.zip'
            release_zip = frozen / 'release.zip'
            editor_runner = root / 'signed editor runner'
            editor_runner.mkdir(mode=0o700)
            shutil.copy2(editor, editor_runner / 'godot')
            shutil.copy2(editor.parent / 'libgodot.so', editor_runner / 'libgodot.so')
            editor = editor_runner / 'godot'
            env = dict(os.environ)
            for key in ('HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'GODOT_OHOS_DATA_DIR', 'GODOT_OHOS_CACHE_DIR', 'TMPDIR'):
                folder = root / ('env-' + key.lower())
                folder.mkdir(mode=0o700)
                env[key] = str(folder)
            env.pop('JAVA_HOME', None)
            env.pop('DEVECO_SDK_HOME', None)
            env.pop('LD_PRELOAD', None)
            env['GODOT_OHOS_SDK_ROOT'] = env['OHOS_SDK_HOME'] = env['OHOS_BASE_SDK_HOME'] = str(sdk)
            env['NPM_CONFIG_OFFLINE'] = env['npm_config_offline'] = 'true'
            inherited = os.environ.get('LD_LIBRARY_PATH', '')
            env['LD_LIBRARY_PATH'] = str(editor.parent) + (os.pathsep + inherited if inherited else '')

            def owned_members(session_id):
                members = []
                for entry in Path('/proc').iterdir():
                    if not entry.name.isdigit():
                        continue
                    pid = int(entry.name)
                    try:
                        if entry.stat().st_uid == os.getuid() and os.getsid(pid) == session_id and os.getpgid(pid) == session_id:
                            members.append(pid)
                    except (FileNotFoundError, ProcessLookupError, PermissionError):
                        continue
                return members

            def signal_owned_group(process, sig):
                if owned_members(process.pid):
                    try:
                        os.killpg(process.pid, sig)
                    except ProcessLookupError:
                        pass

            def stop_owned_group(process):
                # Each native command starts a NEW session. Only same-UID
                # members of that explicitly owned SID/PGID may be signaled.
                signal_owned_group(process, signal.SIGTERM)
                try:
                    process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    signal_owned_group(process, signal.SIGKILL)
                    process.communicate(timeout=5)
                signal_owned_group(process, signal.SIGKILL)

            def diagnose(label, process):
                summary = []
                for pid in owned_members(process.pid):
                    try:
                        command_line = Path('/proc/' + str(pid) + '/cmdline').read_bytes().replace(b'\0', b' ').decode(errors='replace')
                        summary.append({'pid': pid, 'argv': sanitize(command_line)})
                    except (FileNotFoundError, ProcessLookupError, PermissionError):
                        continue
                (output / (label + '-owned-processes.json')).write_text(json.dumps(summary, indent=2))
                for index, filename in enumerate(root.rglob('*.log')):
                    if filename.is_symlink() or not filename.is_file():
                        continue
                    # Diagnostic logs only, never credential/profile/PKCS12
                    # files. Bound each tail and redact BEFORE persisting.
                    with filename.open('rb') as stream:
                        stream.seek(max(0, filename.stat().st_size - 256 * 1024))
                        text = stream.read().decode(errors='replace')
                    (output / (label + '-diagnostic-' + str(index) + '.log')).write_text(sanitize(text))

            def run(label, command, *, expected_zero=True, exporter=False, timeout=None):
                # Never check=True/CalledProcessError: it would embed signer argv
                # passwords. Kill the OWNED process group before deleting keys.
                timeout = args.build_timeout if timeout is None else timeout
                started = time.monotonic()
                process = subprocess.Popen(command, cwd=project, env=env, stdout=subprocess.PIPE,
                                           stderr=subprocess.PIPE, umask=0o077, start_new_session=True)
                try:
                    try:
                        stdout, stderr = process.communicate(timeout=timeout)
                    except subprocess.TimeoutExpired as error:
                        diagnose(label, process)
                        raw = (error.stdout or b'') + (error.stderr or b'')
                        (output / (label + '.log')).write_text(sanitize(raw.decode(errors='replace')))
                        records.append({'stage': label, 'exit_code': None, 'timeout': True, 'elapsed_seconds': time.monotonic() - started})
                        raise RuntimeError('Native task exceeded timeout: ' + label) from None
                    completed = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
                finally:
                    stop_owned_group(process)
                raw = (completed.stdout + completed.stderr).decode(errors='replace')
                (output / (label + '.log')).write_text(sanitize(raw))
                leaked = any(value in raw for value in (password, wrong_password))
                records.append({'stage': label, 'exit_code': completed.returncode,
                                'complete_password_in_raw_exporter_log': leaked if exporter else False})
                if exporter and leaked:
                    raise RuntimeError('Exporter disclosed a complete password; sanitized evidence retained')
                if expected_zero and (completed.returncode != 0 or (exporter and 'SCRIPT ERROR:' in raw)):
                    raise RuntimeError('Native task failed: ' + label + ' (exit ' + str(completed.returncode) + ')')
                if not expected_zero and completed.returncode == 0:
                    raise RuntimeError('Wrong-password export unexpectedly succeeded')
                return completed

            def sign(label, action, **options):
                command = [str(signer), action]
                for key, value in options.items():
                    command.extend(['-' + key, str(value)])
                return run(label, command, timeout=180)

            common = {'keystoreFile': store, 'keystorePwd': password, 'keyPwd': password}
            root_dn = 'CN=Godot Disposable Root,O=oheco,C=CN'
            sub_dn = 'CN=Godot Disposable Intermediate,O=oheco,C=CN'
            ca = {'keyAlg': 'ECC', 'keySize': 'NIST-P-256', 'validity': 2, 'signAlg': 'SHA256withECDSA', **common}
            sign('root-ca', 'generate-ca', keyAlias='root', subject=root_dn, outFile=material / 'root.cer', **ca)
            sign('intermediate-ca', 'generate-ca', keyAlias='intermediate', subject=sub_dn, issuer=root_dn,
                 issuerKeyAlias='root', issuerKeyPwd=password, basicConstraintsPathLen=0, outFile=material / 'sub.cer', **ca)
            for alias, action in [('app', 'generate-app-cert'), ('profile', 'generate-profile-cert')]:
                sign(alias + '-key', 'generate-keypair', keyAlias=alias, keyAlg='ECC', keySize='NIST-P-256', **common)
                sign(alias + '-cert', action, keyAlias=alias, subject='CN=Godot Disposable ' + alias + ',O=oheco,C=CN',
                     issuer=sub_dn, issuerKeyAlias='intermediate', issuerKeyPwd=password, validity=2,
                     signAlg='SHA256withECDSA', outForm='certChain', rootCaCertFile=material / 'root.cer',
                     subCaCertFile=material / 'sub.cer', outFile=material / (alias + '-chain.cer'), **common)
            profile = json.loads((signer.parent / 'UnsgnedDebugProfileTemplate.json').read_text())
            profile['uuid'] = str(uuid.uuid4())
            now = int(time.time())
            profile['validity'] = {'not-before': now - 60, 'not-after': now + 86400}
            leaf = (material / 'app-chain.cer').read_text().split('-----END CERTIFICATE-----')[0] + '-----END CERTIFICATE-----\n'
            profile['bundle-info'].update({'bundle-name': BUNDLE, 'developer-id': 'godot-disposable-export-test', 'development-certificate': leaf})
            profile['acls']['allowed-acls'] = []
            profile['permissions']['restricted-permissions'] = []
            profile['debug-info']['device-ids'] = ['0' * 64]
            private_write(material / 'profile.json', json.dumps(profile, indent=2))
            sign('profile-sign', 'sign-profile', mode='localSign', keyAlias='profile', signAlg='SHA256withECDSA',
                 profileCertFile=material / 'profile-chain.cer', inFile=material / 'profile.json', outFile=material / 'profile.p7b', **common)
            (project / 'project.godot').write_text('''config_version=5
[application]
config/name="Disposable Signed Export Fixture"
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
            (project / 'Main.gd').write_text('extends Node\nfunc _ready():\n    print("DISPOSABLE_SIGNED_EXPORT_FIXTURE")\n    get_tree().quit(0)\n')
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
build/export_project_only=false
build/override_project_dir=false
build/sdk_version="26.0.0"
build/compatible_api=23
build/bundle_id="org.oheco.signedexportfixture"
build/sign=true
permissions/ohos.permission.INTERNET=true
''' % (json.dumps(str(debug_zip)), json.dumps(str(release_zip)))
            (project / 'export_presets.cfg').write_text(preset)
            run('import', [str(editor), '--headless', '--editor', '--path', str(project), '--import'], exporter=True)
            credential_dir = project / '.godot'
            credential_file = credential_dir / 'export_credentials.cfg'
            # Existing .godot was generated inside private TMPDIR. Its direct
            # credential parent is secured on that real FS, never HOME/hmdfs.
            credential_dir.chmod(0o700)
            if stat.S_IMODE(credential_dir.stat().st_mode) != 0o700:
                raise RuntimeError('Credential directory is not effective 0700')
            def credentials(store_password):
                if credential_file.exists():
                    credential_file.unlink()
                fields = {'sign/store_file': str(store), 'sign/store_password': store_password, 'sign/key_alias': 'app',
                          'sign/key_password': password, 'sign/sign_alg': 'SHA256withECDSA',
                          'sign/profile_file': str(material / 'profile.p7b'), 'sign/certpath_file': str(material / 'app-chain.cer')}
                private_write(credential_file, '[preset.0.options]\n' + ''.join(key + '=' + json.dumps(value) + '\n' for key, value in fields.items()))

            def check_restored(generated, template):
                root_build = generated / 'build-profile.json5'
                parsed = json.loads(root_build.read_text())
                assert parsed['app']['signingConfigs'] == []
                assert all('signingConfig' not in item for item in parsed['app']['products'])
                with zipfile.ZipFile(template) as archive:
                    before = json.loads(archive.read('build-profile.json5'))
                expected_options = next(item for item in before['app']['products'] if item['name'] == 'default').get('buildOption')
                restored_options = next(item for item in parsed['app']['products'] if item['name'] == 'default').get('buildOption')
                assert restored_options == expected_options, 'Temporary signed APP pack options were not restored'
                assert not (generated / '.godot-build-runner-original.json5').exists()
                assert not (generated / '.godot-build-runner.lock').exists()
                # Credentials should occur only in the intended temporary
                # .godot file; generated profiles/cache requests must not retain
                # either actual password after C++ finally cleanup.
                for directory in (generated, root / 'env-xdg_cache_home'):
                    for filename in directory.rglob('*'):
                        if not filename.is_file() or filename.is_symlink():
                            continue
                        if filename.name == 'request.json':
                            raise RuntimeError('C++ private signing request was not removed')
                        if filename.suffix in ('.json', '.json5', '.cfg', '.log', '.txt', '.js', '.cjs', '.ts', '.ets'):
                            data = filename.read_bytes()
                            if password.encode() in data or wrong_password.encode() in data:
                                raise RuntimeError('Generated configuration/cache retained a complete password')
                assert 'sign/store_password' not in (project / 'export_presets.cfg').read_text()
                return {'profile_signing_material_removed': True, 'private_request_removed': True,
                        'password_not_in_generated_config_or_cache': True, 'original_pack_options_restored': True}

            def verify(label, artifact):
                sign(label, 'verify-app', inFile=artifact, outCertChain=material / (label + '-verify.cer'), outProfile=material / (label + '-verify.p7b'))

            credentials(password)
            for label, switch, suffix in [('signed-debug-hap', '--export-debug', 'hap'), ('signed-release-app', '--export-release', 'app')]:
                target = root / (label + ' with spaces.' + suffix)
                run(label, [str(editor), '--headless', '--path', str(project), switch, 'OpenHarmony', str(target)], exporter=True)
                if not target.is_file() or target.stat().st_size <= 0:
                    raise RuntimeError('Signed export did not produce a nonempty artifact')
                generated = target.with_suffix('')
                checks = check_restored(generated, release_zip if suffix == 'app' else debug_zip)
                verify(label + '-verify-outer', target)
                artifact_record = {'kind': suffix, 'mode': 'debug' if suffix == 'hap' else 'release',
                                   'size': target.stat().st_size, 'sha256': sha(target), 'outer_native_verified': True, **checks}
                if suffix == 'app':
                    extraction = root / 'app inner packages'
                    extraction.mkdir(mode=0o700)
                    inner = []
                    for index, (member, filename) in enumerate(safe_app_haps(target, extraction)):
                        verify(label + '-verify-inner-' + str(index), filename)
                        inner.append({'name': member, 'size': filename.stat().st_size, 'sha256': sha(filename), 'native_verified': True})
                    artifact_record['inner_haps'] = inner
                result['artifacts'].append(artifact_record)
                (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
            if not args.skip_wrong_password:
                credentials(wrong_password)
                target = root / 'wrong password with spaces.hap'
                failed = run('wrong-password', [str(editor), '--headless', '--path', str(project), '--export-debug', 'OpenHarmony', str(target)], expected_zero=False, exporter=True)
                assert failed.returncode > 0 and not target.exists()
                checks = check_restored(target.with_suffix(''), debug_zip)
                result['wrong_password'] = {'status': 'PASS', 'exit_code': failed.returncode,
                                           'no_artifact': True, 'complete_password_not_logged': True, **checks}
            credential_file.unlink()
            result['temporary_credentials_removed'] = not credential_file.exists()
        result['private_temporary_tree_removed'] = not private_root.exists()
        result['status'] = 'PASS'
        print('PASS native disposable signed debug HAP + release APP outer/all inner HAP verification; credentials/private requests cleaned; no personal/device/install trust')
    except BaseException as error:
        # Do not propagate an exception that embeds native signing argv/secrets.
        result['failure'] = sanitize(str(error))
        raise RuntimeError(result['failure']) from None
    finally:
        if private_root is not None:
            result['private_temporary_tree_removed'] = not private_root.exists()
        result_text = json.dumps(result, indent=2, ensure_ascii=False) + '\n'
        for value in (password, wrong_password):
            if value in result_text:
                raise RuntimeError('Evidence unexpectedly contains a password')
        (output / 'result.json').write_text(result_text)


if __name__ == '__main__':
    main()
