#!/usr/bin/env python3
"""Offline tests for shared-host profiles and reproducible template staging."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import subprocess
import tempfile
import unittest
from unittest import mock

PLATFORM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PLATFORM))
import project_config as project
from sdk_fixture import make_sdk


class ProjectConfigurationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='godot-host-test-', dir=os.environ['TMPDIR'])
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        project.stage_template(self.root)

    def document(self, name):
        return project.read_document(self.root / name)

    def test_default_game_has_no_editor_dependencies(self):
        project.configure_project(self.root, project.configuration())
        host = self.document(project.HOST_PATH)
        self.assertEqual(host['host']['role'], 'game')
        self.assertEqual(host['launch']['defaultMode'], 'packaged-game')
        self.assertEqual(host['managed']['mode'], 'none')
        self.assertEqual(host['instances']['policy'], 'disabled')
        self.assertFalse((self.root / 'entry/src/main/resources/rawfile/runtime.zip').exists())
        self.assertEqual(self.document('entry/src/main/module.json5')['module']['requestPermissions'], [])
        self.assertNotIn('multiAppMode', self.document('AppScope/app.json5')['app'])
        product = self.document('build-profile.json5')['app']['products'][0]
        self.assertEqual(product['compileSdkVersion'], '26.0.0')
        self.assertEqual(product['targetSdkVersion'], '26.0.0')
        self.assertEqual(product['compatibleSdkVersion'], '26.0.0')
        self.assertEqual(product['runtimeOS'], 'OpenHarmony')
        self.assertEqual(self.document('entry/src/main/module.json5')['module']['deviceTypes'], ['default'])

    def test_legacy_sdk_syntax_is_rejected_without_modifying_project(self):
        before = {p: p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        for profile in ('editor', 'game'):
            for sdk in ('7.0.0(26)', '26.0.0(26)'):
                with self.subTest(profile=profile, sdk=sdk), self.assertRaisesRegex(ValueError, 'SDK version'):
                    project.configure_project(self.root, project.configuration(profile, {'build': {'sdkVersion': sdk}}))
        self.assertEqual(before, {p: p.read_bytes() for p in before})
        for compatible in (25, 27):
            with self.assertRaises(ValueError):
                project.configuration(overrides={'build': {'compatibleApi': compatible}})

    def test_api_26_floor_applies_to_editor_and_game(self):
        for profile in ('editor', 'game'):
            for sdk in ('23.0.0', '24.0.0', '25.0.0', '6.1.0(23)'):
                with self.subTest(profile=profile, sdk=sdk), self.assertRaisesRegex(ValueError, 'API 26'):
                    project.configuration(profile, {'build': {'sdkVersion': sdk}})
            for compatible in (23, 24, 25):
                with self.subTest(profile=profile, compatible=compatible), self.assertRaisesRegex(ValueError, 'between 26'):
                    project.configuration(profile, {'build': {'compatibleApi': compatible}})
            config = project.configuration(profile, {'build': {'sdkVersion': '27.0.0', 'compatibleApi': 27}})
            project.configure_project(self.root, config)
            product = self.document('build-profile.json5')['app']['products'][0]
            self.assertEqual(product['compileSdkVersion'], '27.0.0')
            self.assertEqual(product['compatibleSdkVersion'], '27.0.0')
            self.assertEqual(product['runtimeOS'], 'OpenHarmony')

    def test_selected_sdk_metadata_decides_mode_for_both_profiles(self):
        for family in ('OpenHarmony', 'HarmonyOS'):
            sdk = make_sdk(self.root / ('fixture-' + family), family)
            for profile in ('game', 'editor'):
                with self.subTest(family=family, profile=profile):
                    config = project.configuration(profile, {'build': {'sdkRoot': str(sdk)}})
                    project.configure_project(self.root, config)
                    product = self.document('build-profile.json5')['app']['products'][0]
                    self.assertEqual(product['runtimeOS'], family)
                    for field in ('compileSdkVersion', 'targetSdkVersion', 'compatibleSdkVersion'):
                        self.assertEqual(product[field], '26.0.0')
                    devices = ['2in1'] if profile == 'editor' else ['phone' if family == 'HarmonyOS' else 'default']
                    self.assertEqual(self.document('entry/src/main/module.json5')['module']['deviceTypes'], devices)
            alias = sdk / ('default' if family == 'HarmonyOS' else '26.0.0')
            selected = project.resolve_sdk('26.0.0', 26, str(alias))
            self.assertEqual(selected['sdkRoot'], str(sdk))
            self.assertEqual(selected['runtimeOS'], family)

    def test_python_generator_preserves_game_selection_and_editor_2in1_boundary(self):
        # Editor exports use C++ project_config.h. This generator also owns the
        # Editor application itself, so its 2in1-only contract must stay intact.
        for family in ('OpenHarmony', 'HarmonyOS'):
            sdk = make_sdk(self.root / ('devices-' + family), family)
            for devices in (['phone'], ['phone', 'tablet', '2in1'], ['tablet', '2in1']):
                with self.subTest(family=family, devices=devices):
                    config = project.configuration('game', {
                        'build': {'sdkRoot': str(sdk)}, 'application': {'deviceTypes': devices}})
                    project.configure_project(self.root, config)
                    expected = [('default' if family == 'OpenHarmony' else 'phone')
                                if device == 'phone' else device for device in devices]
                    self.assertEqual(self.document('entry/src/main/module.json5')['module']['deviceTypes'], expected)
            project.configure_project(self.root, project.configuration('editor', {'build': {'sdkRoot': str(sdk)}}))
            self.assertEqual(self.document('entry/src/main/module.json5')['module']['deviceTypes'], ['2in1'])
            before = {p: p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
            for devices in ([], ['phone'], ['phone', '2in1'], ['tablet', '2in1']):
                with self.subTest(family=family, editor_devices=devices), self.assertRaises(ValueError):
                    project.configure_project(self.root, project.configuration('editor', {
                        'build': {'sdkRoot': str(sdk)}, 'application': {'deviceTypes': devices}}))
            self.assertEqual(before, {p: p.read_bytes() for p in before})

    @unittest.skipUnless(os.environ.get('GODOT_OHOS_TEST_NATIVE_SDK') and os.environ.get('GODOT_OHOS_TEST_LIBRARY'),
                         'Set GODOT_OHOS_TEST_NATIVE_SDK and GODOT_OHOS_TEST_LIBRARY for the small native C++ configuration fixture')
    def test_native_game_device_masks_and_atomic_rejection(self):
        # Compile the current header against an existing engine library; this
        # exercises production Variant/JSON/configure_game without rebuilding Godot.
        sdk = Path(os.environ['GODOT_OHOS_TEST_NATIVE_SDK']).resolve(strict=True)
        source_library = Path(os.environ['GODOT_OHOS_TEST_LIBRARY']).resolve(strict=True)
        library = self.root / 'libgodot.so'  # Match the engine's SONAME.
        signer = shutil.which('binary-sign-tool')
        self.assertIsNotNone(signer)
        (self.root / 'project.godot').write_text(
            '[application]\nconfig/name="Device type configuration fixture"\nrun/main_scene="res://fixture.tscn"\n')
        (self.root / 'fixture.tscn').write_text('[gd_scene format=3]\n[node name="Fixture" type="Node"]\n')
        source = self.root / 'device_types.cpp'
        source.write_text(r'''
#include <native_window/external_window.h>
#include "main/main.h"
#include "platform/openharmony/os_openharmony.h"
#include "platform/openharmony/export/project_config.h"
#include <cstdio>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return false; } } while (false)

static bool check_devices(const String &root, const char *oh_sdk, const char *hos_sdk) {
    using namespace OpenHarmonyProjectConfig;
    Dictionary options;
    options["sdk"] = "26.0.0";
    options["bundle"] = "org.oheco.devicetypesfixture";
    options["name"] = "Devices";
    options["version_code"] = 1;
    options["version_name"] = "1.0";
    options["arch"] = "arm64-v8a";
    options["orientation"] = "portrait";
    options["internet"] = false;
    options["microphone"] = false;
    options["expand_safe_area"] = true;
    options["verbose"] = false;
    for (int family = 0; family < 2; family++) {
        options["sdk_root"] = family == 0 ? oh_sdk : hos_sdk;
        for (int64_t mask = 1; mask <= 63; mask++) {
            options["device_types"] = mask;
            CHECK(configure_game(root, options) == OK);
            Dictionary document;
            CHECK(read_document(root.path_join("entry/src/main/module.json5"), document) == OK);
            Dictionary module = document["module"];
            Array expected;
            if (mask & 1) { expected.push_back(family == 0 ? "default" : "phone"); }
            if (mask & 2) { expected.push_back("tablet"); }
            if (mask & 4) { expected.push_back("2in1"); }
            if (mask & 8) { expected.push_back("tv"); }
            if (mask & 16) { expected.push_back("wearable"); }
            if (mask & 32) { expected.push_back("car"); }
            CHECK(Array(module["deviceTypes"]) == expected);
        }
        options.erase("device_types");
        CHECK(configure_game(root, options) == OK);
        Dictionary document;
        CHECK(read_document(root.path_join("entry/src/main/module.json5"), document) == OK);
        Dictionary module = document["module"];
        CHECK(Array(module["deviceTypes"]) == Array{family == 0 ? "default" : "phone"});
        const char *paths[] = {"AppScope/app.json5", "AppScope/resources/base/element/string.json",
            "entry/src/main/resources/base/element/string.json", "build-profile.json5",
            "entry/build-profile.json5", "entry/src/main/module.json5",
            "entry/src/main/resources/rawfile/godot_host.json"};
        String before[7];
        for (int i = 0; i < 7; i++) { before[i] = FileAccess::get_file_as_string(root.path_join(paths[i])); }
        for (const Variant &invalid : { Variant(), Variant(false), Variant(true), Variant(1.5), Variant("phone"),
                Variant(int64_t(0)), Variant(int64_t(-1)), Variant(int64_t(64)), Variant(int64_t(65)),
                Variant(int64_t(1) << 32), Variant(int64_t(1) << 62) }) {
            options["device_types"] = invalid;
            CHECK(configure_game(root, options) == ERR_INVALID_PARAMETER);
            for (int i = 0; i < 7; i++) {
                CHECK(FileAccess::get_file_as_string(root.path_join(paths[i])) == before[i]);
            }
        }
    }
    std::puts("DEVICE_TYPES_PASS: 126 masks, both SDK families, defaults and 22 invalid inputs without writes");
    return true;
}
int main(int argc, char **argv) {
    if (argc != 4) { return 2; }
    OS_OpenHarmony::EXEC_PATH = argv[0];
    OS_OpenHarmony os(false);
    char headless[] = "--headless", path_option[] = "--path";
    char *arguments[] = {headless, path_option, argv[1]};
    if (Main::setup(argv[0], 3, arguments) != OK) { return 3; }
    const bool passed = check_devices(argv[1], argv[2], argv[3]);
    Main::cleanup();
    return passed ? 0 : 1;
}
''')
        unsigned, executable = self.root / 'device-types.unsigned', self.root / 'device-types'
        repo = PLATFORM.parents[1]
        command = [str(sdk / 'llvm/bin/clang++'), '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
                   '-std=c++17', '-DOPENHARMONY_ENABLED', '-DUNIX_ENABLED', '-DDEBUG_ENABLED',
                   '-DDEBUG_METHODS_ENABLED', '-DTOOLS_ENABLED', '-DTHREADS_ENABLED', '-D__OPEN_HARMONY__',
                   '-DVULKAN_ENABLED', '-DRD_ENABLED', '-pthread', '-I' + str(repo), '-I' + str(PLATFORM),
                   str(source), str(PLATFORM / 'bridge_openharmony.cpp'), str(library),
                   '-Wl,--no-code-sign', '-Wl,-rpath,' + str(self.root), '-o', str(unsigned)]
        environment = dict(os.environ)
        environment.update(GODOT_OHOS_DATA_DIR=str(self.root / 'data'), GODOT_OHOS_CACHE_DIR=str(self.root / 'cache'))
        for key in ('HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME'):
            directory = self.root / ('isolated-' + key.lower())
            directory.mkdir()
            environment[key] = str(directory)
        def run_native(command):
            result = subprocess.run(command, cwd=repo, env=environment, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result

        # Sign an isolated copy on TMPDIR's native filesystem. Never strip or
        # re-sign the user's build output, which may live on hmdfs.
        staging_library = self.root / 'libgodot.staging.so'
        run_native([str(sdk / 'llvm/bin/llvm-objcopy'), '--remove-section=.codesign',
                    str(source_library), str(staging_library)])
        run_native([signer, 'sign', '-selfSign', '1', '-inFile', str(staging_library), '-outFile', str(library)])
        run_native(command)
        sections = run_native([str(sdk / 'llvm/bin/llvm-readelf'), '--sections', str(unsigned)]).stdout
        if '.codesign' in sections:
            # Some installed native toolchains reserve this section even with
            # --no-code-sign. Remove it before signing the final fixture inode.
            run_native([str(sdk / 'llvm/bin/llvm-objcopy'), '--remove-section=.codesign', str(unsigned)])
        run_native([signer, 'sign', '-selfSign', '1', '-inFile', str(unsigned), '-outFile', str(executable)])
        executable.chmod(0o755)
        result = run_native([str(executable), str(self.root),
                             str(make_sdk(self.root / 'native-oh-sdk', 'OpenHarmony')),
                             str(make_sdk(self.root / 'native-hos-sdk', 'HarmonyOS'))])
        self.assertIn('DEVICE_TYPES_PASS', result.stdout)
        print(result.stdout, end='')

    def test_sdk_alias_numeric_api_and_null_identity(self):
        sdk = make_sdk(self.root / 'typed-sdk', 'HarmonyOS')
        for path in sdk.rglob('*.json'):
            document = project.read_document(path)
            info = document['data'] if 'data' in document else document
            info['apiVersion'] = 26
            project.write_document(path, document)
        alias = self.root / 'sdk-directory-alias'
        alias.symlink_to(sdk / 'default', target_is_directory=True)
        self.assertEqual(project.resolve_sdk('26.0.0', 26, str(alias))['sdkRoot'], str(sdk))
        for path in sdk.rglob('*.json'):
            document = project.read_document(path)
            info = document['data'] if 'data' in document else document
            info['version'], info['releaseType'] = None, None
            project.write_document(path, document)
        with self.assertRaises(ValueError):
            project.resolve_sdk('26.0.0', 26, str(alias))

    def test_toolchain_default_sdk_and_external_sdk_precedence(self):
        tools = self.root / 'command-line-tools'
        make_sdk(tools / 'sdk', 'HarmonyOS')
        for name in ('hvigor/bin/hvigorw', 'node/bin/node'):
            path = tools / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('SYNTHETIC NON-EXECUTABLE TOOL PLACEHOLDER')
        mapping_path = tools / 'hvigor/hvigor-ohos-plugin/node_modules/@ohos/hos-sdkmanager-common/build/res/hos-config.json'
        project.write_document(mapping_path, {'osVersionMapper': {'26.0.0': '26.0.0'}})
        default = project.resolve_sdk('26.0.0', 26, command_line_tools_root=str(tools))
        self.assertEqual(default['runtimeOS'], 'HarmonyOS')
        external = make_sdk(self.root / 'external-sdk', 'OpenHarmony')
        selected = project.resolve_sdk('26.0.0', 26, str(external), str(tools))
        self.assertEqual(selected['runtimeOS'], 'OpenHarmony')
        self.assertEqual(selected['sdkRoot'], str(external))
        with self.assertRaises(ValueError):
            project.resolve_sdk('26.0.0', 26, str(self.root / 'missing-sdk'), str(tools))

    def test_partial_or_wrong_sdk_fails_before_modifying_project(self):
        sdk = make_sdk(self.root / 'partial-sdk', 'HarmonyOS')
        path = sdk / 'default/hms/native/uni-package.json'
        metadata = project.read_document(path)
        metadata['apiVersion'] = '25'
        project.write_document(path, metadata)
        before = {p: p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        for root in (sdk, self.root / 'nonexistent-sdk', sdk / 'default/openharmony'):
            with self.subTest(root=root), self.assertRaises(ValueError):
                project.configure_project(self.root, project.configuration('editor', {'build': {'sdkRoot': str(root)}}))
        self.assertEqual(before, {p: p.read_bytes() for p in before})

    def test_editor_only_changes_configuration(self):
        sources = {p.relative_to(self.root): p.read_bytes() for p in self.root.rglob('*')
                   if p.is_file() and p.suffix in ('.ets', '.cpp', '.h', '.ts')}
        project.configure_project(self.root, project.configuration('editor'))
        self.assertTrue(sources)
        self.assertEqual(sources, {p: (self.root / p).read_bytes() for p in sources})
        host = self.document(project.HOST_PATH)
        self.assertEqual(host['host']['role'], 'editor')
        self.assertEqual(host['launch']['defaultArguments'], ['--single-window'])
        self.assertEqual(host['managed']['mode'], 'sdk')
        module = self.document('entry/src/main/module.json5')['module']
        self.assertEqual(module['mainElement'], 'EntryAbility')
        self.assertEqual(module['abilities'][0]['launchType'], 'multiton')
        self.assertEqual(module['deviceTypes'], ['2in1'])
        self.assertEqual(self.document('AppScope/app.json5')['app']['multiAppMode']['maxCount'], 5)
        permissions = {p['name']: p for p in module['requestPermissions']}
        self.assertIn('ohos.permission.ALLOW_EXTERNAL_NATIVE_CODE', permissions)
        self.assertNotIn('ohos.permission.MOUNT_HDCDEBUG_PATH', permissions)
        self.assertEqual(permissions['ohos.permission.READ_WRITE_USER_FILE']['usedScene']['abilities'], ['EntryAbility'])
        self.assertEqual(permissions['ohos.permission.READ_PASTEBOARD'], {
            'name': 'ohos.permission.READ_PASTEBOARD', 'reason': '$string:reason_pasteboard',
            'usedScene': {'abilities': ['EntryAbility'], 'when': 'inuse'},
        })
        strings = {item['name']: item['value'] for item in self.document('entry/src/main/resources/base/element/string.json')['string']}
        self.assertIn('ohos.permission.READ_PASTEBOARD', strings['user_permissions'].split(','))
        self.assertTrue(strings['reason_pasteboard'])
        self.assertTrue(project.RESTRICTED.isdisjoint(permissions))

    def test_no_managed_editor_is_supported(self):
        config = project.configuration('editor', {'managed': {'mode': 'none'}})
        project.configure_project(self.root, config)
        permissions = {p['name'] for p in self.document('entry/src/main/module.json5')['module']['requestPermissions']}
        self.assertIn('ohos.permission.READ_WRITE_USER_FILE', permissions)
        self.assertIn('ohos.permission.ALLOW_EXTERNAL_NATIVE_CODE', permissions)
        self.assertNotIn('ohos.permission.MOUNT_HDCDEBUG_PATH', permissions)
        self.assertEqual(self.document(project.HOST_PATH)['managed']['mode'], 'none')

    def test_preserve_signing_but_migrate_required_build_fields(self):
        path = self.root / 'build-profile.json5'
        build = self.document('build-profile.json5')
        signing = [{'name': 'personal', 'material': {'storePassword': 'x//y/*z*/\\"w', 'storeFile': '/my/private/path'}}]
        build['app']['signingConfigs'] = signing
        product = build['app']['products'][0]
        product['signingConfig'] = 'personal'
        product['compatibleSdkVersion'] = '5.1.0(18)'
        product['runtimeOS'] = 'HarmonyOS'
        product['customBuildField'] = {'keep': True}
        path.write_text('// local DevEco settings\n' + json.dumps(build)[:-1] + ',\n}\n')
        entry = self.document('entry/build-profile.json5')
        entry['buildOption']['externalNativeOptions']['arguments'] = '-DUSER_OPTION=ON'
        entry['buildOption']['nativeLib']['debugSymbol']['strip'] = True
        project.write_document(self.root / 'entry/build-profile.json5', entry)
        config = project.configuration('editor')
        project.configure_project(self.root, config)
        after = self.document('build-profile.json5')['app']
        self.assertEqual(after['signingConfigs'], signing)
        self.assertEqual(after['products'][0]['signingConfig'], 'personal')
        self.assertEqual(after['products'][0]['customBuildField'], {'keep': True})
        self.assertEqual(after['products'][0]['compatibleSdkVersion'], '26.0.0')
        self.assertEqual(after['products'][0]['runtimeOS'], 'OpenHarmony')
        entry = self.document('entry/build-profile.json5')['buildOption']
        self.assertEqual(entry['externalNativeOptions']['arguments'], '-DUSER_OPTION=ON')
        self.assertTrue(entry['nativeLib']['debugSymbol']['strip'])
        before = {p: p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        project.configure_project(self.root, config)
        self.assertEqual(before, {p: p.read_bytes() for p in before})

    def test_json_escaping_and_permission_ability(self):
        name = '游戏 "Quoted" \\ Test\nLine'
        project.configure_project(self.root, project.configuration(overrides={
            'application': {'displayName': name}, 'permissions': ['ohos.permission.MICROPHONE']}))
        strings = {s['name']: s['value'] for s in self.document('entry/src/main/resources/base/element/string.json')['string']}
        self.assertEqual(strings['EntryAbility_label'], name)
        self.assertEqual(strings['user_permissions'], 'ohos.permission.MICROPHONE')
        request = self.document('entry/src/main/module.json5')['module']['requestPermissions'][0]
        self.assertEqual(request['usedScene']['abilities'], ['EntryAbility'])

    def test_restricted_permissions_require_explicit_flags(self):
        permission = 'ohos.permission.CUSTOM_SANDBOX'
        with self.assertRaises(ValueError):
            project.configuration('editor', {'permissions': [permission]})
        project.configure_project(self.root, project.configuration('editor'), [permission])
        permissions = {p['name'] for p in self.document('entry/src/main/module.json5')['module']['requestPermissions']}
        self.assertIn(permission, permissions)
        project.configure_project(self.root, project.configuration('editor'))
        permissions = {p['name'] for p in self.document('entry/src/main/module.json5')['module']['requestPermissions']}
        self.assertNotIn(permission, permissions)

    def test_invalid_options_fail_explicitly(self):
        invalid = [
            {'schemaVersion': 2}, {'unknown': True}, {'host': {'role': 'invalid'}},
            {'launch': {'defaultArguments': [1]}}, {'launch': {'acceptProjectRequests': True}},
            {'managed': {'mode': 'runtime'}}, {'managed': {'mode': 'sdk'}},
            {'instances': {'policy': 'editor'}}, {'instances': {'maxCount': 6}},
            {'instances': {'maxCount': True}}, {'window': {'expandIntoSystemArea': 'yes'}},
            {'build': {'sdkVersion': '5.1.0(18)'}}, {'build': {'sdkVersion': 'not-an-api'}},
            {'build': {'architectures': ['riscv64']}}, {'application': {'bundleId': 'not a bundle'}},
            {'permissions': ['ohos.permission.CAMERA']},
            {'permissions': ['ohos.permission.READ_WRITE_USER_FILE']},
            {'permissions': ['ohos.permission.ALLOW_EXTERNAL_NATIVE_CODE']},
        ]
        for overrides in invalid:
            with self.subTest(overrides=overrides), self.assertRaises(ValueError):
                project.configuration(overrides=overrides)

    def test_unparseable_preserved_profile_does_not_modify_documents(self):
        path = self.root / 'entry/build-profile.json5'
        path.write_text("{unquoted: 'unsupported'}")
        before = {p: p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        with self.assertRaises(ValueError):
            project.configure_project(self.root, project.configuration('editor'))
        self.assertEqual(before, {p: p.read_bytes() for p in before})

    def test_file_quota_failure_preserves_original_signing_bytes(self):
        profile = self.document('build-profile.json5')
        profile['app']['signingConfigs'] = [{'name': 'personal', 'material': {'password': 'fixture' * 100}}]
        profile['app']['products'][0]['targetSdkVersion'] = '5.1.0(18)'
        project.write_document(self.root / 'build-profile.json5', profile)
        before = {p: p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        code = '''import resource, signal, sys
sys.path.insert(0, sys.argv[1])
import project_config as p
config = p.configuration('editor')
signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
resource.setrlimit(resource.RLIMIT_FSIZE, (512, 512))
p.configure_project(sys.argv[2], config)
'''
        result = subprocess.run([sys.executable, '-B', '-c', code, str(PLATFORM), str(self.root)], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(before, {p: p.read_bytes() for p in self.root.rglob('*') if p.is_file()})

    def test_install_failure_restores_both_profiles_without_rewriting(self):
        profile = self.document('build-profile.json5')
        profile['app']['signingConfigs'] = [{'name': 'personal', 'material': {'password': 'fixture'}}]
        profile['app']['products'][0]['targetSdkVersion'] = '5.1.0(18)'
        project.write_document(self.root / 'build-profile.json5', profile)
        entry = self.document('entry/build-profile.json5')
        entry['buildOption']['nativeLib']['debugSymbol']['strip'] = True
        project.write_document(self.root / 'entry/build-profile.json5', entry)
        protected = {self.root / name: (self.root / name).read_bytes()
                     for name in ('build-profile.json5', 'entry/build-profile.json5')}
        replace = os.replace
        failed = False

        def fail_install(source, destination):
            nonlocal failed
            if Path(destination) == self.root / 'build-profile.json5' and '.godot-config-history' not in Path(source).parts:
                failed = True
                raise OSError(28, 'simulated destination full after staging')
            return replace(source, destination)

        with mock.patch.object(project.os, 'replace', side_effect=fail_install), self.assertRaises(OSError):
            project.configure_project(self.root, project.configuration('editor'))
        self.assertTrue(failed)
        self.assertEqual(protected, {p: p.read_bytes() for p in protected})

    def test_missing_profile_with_recovery_history_is_not_reset(self):
        saved = self.root / '.godot-config-history/interrupted/build-profile.json5'
        saved.parent.mkdir(parents=True)
        os.replace(self.root / 'build-profile.json5', saved)
        with self.assertRaisesRegex(ValueError, 'restore its durable'):
            project.configure_project(project.TEMPLATE, project.configuration('editor'),
                                      dry_run=True, preserved_profiles=self.root)
        self.assertTrue(saved.is_file())
        self.assertFalse((self.root / 'build-profile.json5').exists())

    def test_no_stale_generated_inputs_in_staging(self):
        self.assertFalse((self.root / 'entry/src/main/cpp/libs').exists())
        self.assertFalse((self.root / 'entry/libs').exists())
        self.assertFalse((self.root / 'local.properties').exists())
        self.assertFalse((self.root / 'oh-package-lock.json5').exists())
        self.assertEqual({p.name for p in (self.root / 'entry/src/main/resources/rawfile').iterdir()}, {'godot_host.json'})


if __name__ == '__main__':
    unittest.main(verbosity=2)
