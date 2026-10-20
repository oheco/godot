// Godot Engine contributors. SPDX-License-Identifier: MIT
// The runner includes the byte-exact current production validation method.
// ProjectSettings, feature resolution, presets and other checks use the Editor.
#include "core/config/project_settings.h"
#include "editor/export/editor_export.h"
#include "editor/export/editor_export_preset.h"
#include "editor/file_system/editor_file_system.h"
#include "main/main.h"

#include "platform/openharmony/export/export_plugin.h"
#include "platform/openharmony/os_openharmony.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>

#ifndef GODOT_RENDERING_EXPORT_LIBRARY_ONLY
#include "rendering_export_method.inc"
#endif

namespace {
// Real ARM hosts automatically import ETC2/ASTC even when its project setting
// is false. Override only the hardware preference to exercise the unchanged
// missing-import rejection that also matters when exporting from an x86 host.
class FixtureOS : public OS_OpenHarmony {
public:
	FixtureOS() :
			OS_OpenHarmony(false) {}

	PreferredTextureFormat get_preferred_texture_format() const override {
		if (get_environment("GODOT_RENDERING_TEXTURE_PREFERENCE") == "s3tc_bptc") {
			return PREFERRED_TEXTURE_FORMAT_S3TC_BPTC;
		}
		return OS_OpenHarmony::get_preferred_texture_format();
	}
};

int failures = 0;
bool check(bool p_ok, const char *p_message) {
	std::printf("%s %s\n", p_ok ? "PASS" : "FAIL", p_message);
	failures += !p_ok;
	return p_ok;
}

Dictionary rendering_snapshot() {
	Dictionary result;
	List<PropertyInfo> properties;
	ProjectSettings *settings = ProjectSettings::get_singleton();
	settings->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if (property.name.begins_with("rendering/")) {
			result[property.name] = settings->get_setting(property.name);
		}
	}
	return result;
}

Dictionary preset_snapshot(const Ref<EditorExportPreset> &p_preset) {
	Dictionary result;
	for (const KeyValue<StringName, Variant> &entry : p_preset->get_values()) {
		result[entry.key] = entry.value;
	}
	return result;
}

void run_fixture(OS_OpenHarmony &p_os) {
	for (int i = 0; i < 1000; i++) {
		p_os.main_loop_iterate();
		if (i >= 2 && !EditorFileSystem::get_singleton()->is_scanning()) {
			break;
		}
		p_os.delay_usec(10000);
	}
	if (!check(!EditorFileSystem::get_singleton()->is_scanning(), "real Editor filesystem scan completes")) {
		return;
	}
	EditorExport *exports = EditorExport::get_singleton();
	if (!check(exports != nullptr, "real Editor export registry initialized")) {
		return;
	}
	const int index = exports->get_export_platform_index_by_name("OpenHarmony");
	if (!check(index >= 0, "OpenHarmony export platform registered")) {
		return;
	}
	const Ref<EditorExportPlatform> platform_ref = exports->get_export_platform(index);
	auto *platform = Object::cast_to<EditorExportPlatformOpenHarmony>(platform_ref.ptr());
	if (!check(platform != nullptr, "registered platform is EditorExportPlatformOpenHarmony")) {
		return;
	}
	const Ref<EditorExportPreset> preset = platform->create_preset();
	preset->set("build/export_project_only", true);
	preset->set("build/sign", false);
	OS *os = OS::get_singleton();
	preset->set_custom_features(os->get_environment("GODOT_RENDERING_CUSTOM_FEATURES"));
	const String expected_method = os->get_environment("GODOT_RENDERING_EXPECT_METHOD");
	const String expected_driver = os->get_environment("GODOT_RENDERING_EXPECT_DRIVER");
	const String driver_key = expected_method == "gl_compatibility" ? "rendering/gl_compatibility/driver" : "rendering/rendering_device/driver";
	check(String(preset->get_project_setting("rendering/renderer/rendering_method")) == expected_method, "real export features resolve the expected rendering method");
	check(String(preset->get_project_setting(driver_key)) == expected_driver, "real export features resolve the selected driver's setting");
	const bool expected_valid = os->get_environment("GODOT_RENDERING_EXPECT_VALID") == "1";
	const String expected_error = os->get_environment("GODOT_RENDERING_EXPECT_ERROR");
	const Dictionary settings_before = rendering_snapshot();
	const Dictionary preset_before = preset_snapshot(preset);
	const String features_before = preset->get_custom_features();
	// Explicit qualification guarantees the method compiled by this fixture is
	// called even when the linked library's vtable still contains the old method.
	for (int i = 0; i < 2; i++) {
		String error;
		const bool valid = platform->EditorExportPlatformOpenHarmony::has_valid_project_configuration(preset, error);
		std::printf("VALIDATION valid=%d error=%s\n", int(valid), error.utf8().get_data());
		check(valid == expected_valid, "production project validation matches expected result");
		check(expected_error.is_empty() ? error.is_empty() : error.contains(expected_error), "validation reports the selected renderer's actionable diagnostic");
		check(rendering_snapshot() == settings_before && preset_snapshot(preset) == preset_before && preset->get_custom_features() == features_before,
				"validation preserves rendering settings and export preset");
	}
}
} // namespace

int main(int argc, char **argv) {
	OS_OpenHarmony::EXEC_PATH = argv[0];
	FixtureOS os;
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
	std::printf("RENDERING_EXPORT_%s failures=%d\n", failures ? "FAIL" : "PASS", failures);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
