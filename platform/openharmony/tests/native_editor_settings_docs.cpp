// Godot Engine contributors. SPDX-License-Identifier: MIT
// Real EditorSettings registration and EditorHelp XML merge used by tooltips.
// clang-format off
#include <native_window/external_window.h>
// clang-format on
#include "core/object/message_queue.h"
#include "editor/doc/editor_help.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/settings/editor_settings.h"
#include "main/main.h"
#include "platform/openharmony/os_openharmony.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>

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
	const uint64_t deadline = os.get_ticks_msec() + 10000;
	do {
		Main::iteration();
	} while (EditorFileSystem::get_singleton()->is_scanning() && os.get_ticks_msec() < deadline);
	MessageQueue::get_main_singleton()->flush();
	DocTools *startup_docs = EditorHelp::get_doc_data();
	DocData::ClassDoc *startup_settings = startup_docs ? startup_docs->class_list.getptr("EditorSettings") : nullptr;
	int startup_missing = 0;
	for (const char *name : { "command_line_tools_root", "sdk_root", "hdc_path", "use_broker", "auto_signing", "always_auto_sign", "devecocli_path", "auto_signing_team_id", "auto_signing_data_dir" }) {
		bool found = false;
		if (startup_settings) {
			for (const DocData::PropertyDoc &property : startup_settings->properties) {
				found |= property.name == String("export/openharmony/") + name && !property.description.strip_edges().is_empty();
			}
		}
		startup_missing += !found;
	}
	std::printf("%s startup exporter setting tooltips: missing=%d\n", startup_missing ? "FAIL" : "PASS", startup_missing);
	MessageQueue::get_main_singleton()->flush();
	EditorHelp::generate_doc(false, false);
	DocTools *docs = EditorHelp::get_doc_data();
	DocData::ClassDoc *settings_doc = docs ? docs->class_list.getptr("EditorSettings") : nullptr;
	int failures = startup_missing;
	bool core_documented = false;
	if (settings_doc) {
		for (const DocData::PropertyDoc &property : settings_doc->properties) {
			if (property.name == "interface/editor/appearance/display_scale" && !property.description.strip_edges().is_empty()) {
				core_documented = true;
			}
		}
	}
	std::printf("%s core EditorSettings tooltip documentation\n", core_documented ? "PASS" : "FAIL");
	failures += !core_documented;
	for (const char *name : { "command_line_tools_root", "sdk_root", "hdc_path", "use_broker", "auto_signing", "always_auto_sign", "devecocli_path", "auto_signing_team_id", "auto_signing_data_dir" }) {
		const String setting = String("export/openharmony/") + name;
		bool documented = false;
		if (settings_doc) {
			for (const DocData::PropertyDoc &property : settings_doc->properties) {
				if (property.name == setting && !property.description.strip_edges().is_empty()) {
					documented = true;
					break;
				}
			}
		}
		const bool registered = EditorSettings::get_singleton()->has_setting(setting);
		std::printf("%s registered setting and nonempty native tooltip: %s\n", documented && registered ? "PASS" : "FAIL", name);
		failures += !(documented && registered);
	}
	for (const char *name : { "auto_signing", "always_auto_sign" }) {
		const String setting = String("export/openharmony/") + name;
		const bool enabled = EditorSettings::get_singleton()->has_setting(setting) && bool(EditorSettings::get_singleton()->get_setting(setting));
		std::printf("%s default-enabled automatic signing: %s\n", enabled ? "PASS" : "FAIL", name);
		failures += !enabled;
	}
	std::printf("EDITOR_SETTINGS_DOCS_%s failures=%d\n", failures ? "FAIL" : "PASS", failures);
	os.main_loop_end();
	Main::cleanup();
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
