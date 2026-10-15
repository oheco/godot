// Godot Engine contributors. SPDX-License-Identifier: MIT
// Exercise the policy used by run() with real Editor settings and presets.
// clang-format off
#include <native_window/external_window.h>
// clang-format on
#include "core/io/file_access.h"
#include "editor/export/editor_export.h"
#include "editor/export/editor_export_preset.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/settings/editor_settings.h"
#include "main/main.h"
#include "platform/openharmony/export/export_plugin.h"
#include "platform/openharmony/os_openharmony.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>

namespace {
int failures = 0;
const char *signing_fields[] = {
	"sign/store_file", "sign/store_password", "sign/key_alias", "sign/key_password",
	"sign/sign_alg", "sign/profile_file", "sign/certpath_file"
};

bool check(bool p_ok, const char *p_message) {
	std::printf("%s %s\n", p_ok ? "PASS" : "FAIL", p_message);
	failures += !p_ok;
	return p_ok;
}

Dictionary signing_snapshot(const Ref<EditorExportPreset> &p_preset) {
	Dictionary result;
	result["build/sign"] = p_preset->get("build/sign");
	for (const char *field : signing_fields) {
		result[field] = p_preset->get(field);
	}
	return result;
}

bool unchanged(const Ref<EditorExportPreset> &p_preset, const Dictionary &p_snapshot) {
	if (p_preset->get("build/sign") != p_snapshot["build/sign"]) {
		return false;
	}
	for (const char *field : signing_fields) {
		if (p_preset->get(field) != p_snapshot[field]) {
			return false;
		}
	}
	return true;
}

void audit_policy(const Ref<EditorExportPreset> &p_preset, bool p_manual_complete) {
	const Dictionary snapshot = signing_snapshot(p_preset);
	EditorSettings *settings = EditorSettings::get_singleton();
	check(EditorExportPlatformOpenHarmony::has_complete_manual_signing(p_preset) == p_manual_complete, "production completeness check matches real preset fields");
	for (bool automatic : { false, true }) {
		for (bool always : { false, true }) {
			settings->set_setting("export/openharmony/auto_signing", automatic);
			settings->set_setting("export/openharmony/always_auto_sign", always);
			const bool selected = EditorExportPlatformOpenHarmony::should_auto_sign_remote_run(p_preset,
					bool(settings->get_setting("export/openharmony/auto_signing")),
					bool(settings->get_setting("export/openharmony/always_auto_sign")));
			const bool expected = automatic && (always || !p_manual_complete);
			check(selected == expected, "production run policy honors auto/always/manual precedence");
		}
	}
	check(unchanged(p_preset, snapshot), "policy does not mutate or save preset signing fields");
}

void run_fixture(OS_OpenHarmony &p_os) {
	for (int i = 0; i < 1000; i++) {
		p_os.main_loop_iterate();
		if (i >= 2 && !EditorFileSystem::get_singleton()->is_scanning()) {
			break;
		}
		p_os.delay_usec(10000);
	}
	if (!check(!EditorFileSystem::get_singleton()->is_scanning(), "real Editor filesystem scan completes before policy validation")) {
		return;
	}
	EditorExport *exports = EditorExport::get_singleton();
	if (!check(exports && EditorSettings::get_singleton(), "real Editor export registry and settings initialized")) {
		return;
	}
	const int index = exports->get_export_platform_index_by_name("OpenHarmony");
	if (!check(index >= 0, "real OpenHarmony export platform registered")) {
		return;
	}
	const Ref<EditorExportPlatform> platform = exports->get_export_platform(index);
	const Ref<EditorExportPreset> preset = platform->create_preset();
	const String root = OS::get_singleton()->get_environment("GODOT_AUTO_SIGN_POLICY_ROOT");
	if (!check(root.is_absolute_path(), "runner supplies isolated fixture root")) {
		return;
	}
	const String template_path = root.path_join("configuration-template.zip");
	{
		Ref<FileAccess> file = FileAccess::open(template_path, FileAccess::WRITE);
		if (!check(file.is_valid(), "create template existence marker without a build")) {
			return;
		}
		file->store_string("Configuration validation only; never extracted or executed.\n");
	}
	preset->set("custom_template/debug", template_path);
	preset->set("custom_template/release", template_path);
	preset->set("build/export_project_only", true);
	preset->set("build/sign", false);
	for (const char *field : signing_fields) {
		preset->set(field, "");
	}
	audit_policy(preset, false);
	String error;
	bool missing_templates = false;
	check(platform->has_valid_export_configuration(preset, error, missing_templates, true), "ordinary project export still accepts signing disabled");
	preset->set("build/sign", true);
	audit_policy(preset, false);
	error = String();
	check(!platform->has_valid_export_configuration(preset, error, missing_templates, true) && error.contains("required when signing is enabled"), "ordinary export still rejects missing manual signing fields");

	preset->set("sign/store_file", root.path_join("missing-store.p12"));
	preset->set("sign/store_password", "fixture encrypted store password");
	preset->set("sign/key_alias", "fixture alias");
	preset->set("sign/key_password", "fixture encrypted key password");
	preset->set("sign/sign_alg", "SHA256withECDSA");
	preset->set("sign/profile_file", root.path_join("missing-profile.p7b"));
	preset->set("sign/certpath_file", root.path_join("missing-certificate.cer"));
	const Dictionary complete = signing_snapshot(preset);
	audit_policy(preset, true);
	error = String();
	check(!platform->has_valid_export_configuration(preset, error, missing_templates, true) &&
			error.contains("Store file does not exist") && error.contains("Profile file does not exist") && error.contains("Certificate path file does not exist"),
			"complete manual identity with missing files reports manual errors instead of changing identity");
	check(unchanged(preset, complete), "ordinary export validation preserves manual identity");
	for (const char *field : signing_fields) {
		preset->set(field, "");
		audit_policy(preset, false);
		preset->set(field, complete[field]);
	}
	preset->set("build/sign", false);
	audit_policy(preset, false);
	preset->set("build/sign", true);
	check(unchanged(preset, complete), "all individual field omissions restore the original real preset");
	check(!EditorExportPlatformOpenHarmony::has_complete_manual_signing(Ref<EditorExportPreset>()), "null preset has no complete manual identity");
}
} // namespace

int main(int argc, char **argv) {
	OS_OpenHarmony::EXEC_PATH = argv[0];
	OS_OpenHarmony os(false);
	setlocale(LC_CTYPE, "");
	if (Main::setup(argv[0], argc - 1, argv + 1) != OK) {
		return EXIT_FAILURE;
	}
	if (Main::start() != EXIT_SUCCESS) {
		Main::cleanup();
		return EXIT_FAILURE;
	}
	os.main_loop_begin();
	run_fixture(os);
	os.main_loop_end();
	Main::cleanup();
	std::printf("AUTO_SIGN_POLICY_%s failures=%d\n", failures ? "FAIL" : "PASS", failures);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
