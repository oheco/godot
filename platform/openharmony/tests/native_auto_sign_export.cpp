// Godot Engine contributors. SPDX-License-Identifier: MIT
// Link to the real Editor library; export, process capture and protocol validation
// remain production code. The Node backend creates fake HAP bytes, not signatures.
// clang-format off
#include <native_window/external_window.h>
// clang-format on
#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "editor/export/editor_export.h"
#include "editor/export/editor_export_preset.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/settings/editor_settings.h"
#include "main/main.h"

#include "platform/openharmony/export/export_plugin.h"
#include "platform/openharmony/export/toolchain.h"
#include "platform/openharmony/os_openharmony.h"

#include <dlfcn.h>

#include <clocale>
#include <cstdio>
#include <cstdlib>

namespace {
int failures = 0;
const char *secret_prefix = "AUTO_SIGN_FIXTURE_PRIVATE_";
const char *signing_fields[] = {
	"sign/store_file", "sign/store_password", "sign/key_alias", "sign/key_password",
	"sign/sign_alg", "sign/profile_file", "sign/certpath_file"
};
const char *auth_message = "Run the local DevEco CLI login command, then retry Remote Deploy.";

bool check(bool p_ok, const char *p_message) {
	std::printf("%s %s\n", p_ok ? "PASS" : "FAIL", p_message);
	failures += !p_ok;
	return p_ok;
}

bool number_equals(const Variant &p_value, double p_expected) {
	// JSON::parse() represents JSON numbers as FLOAT, not necessarily INT.
	return (p_value.get_type() == Variant::INT || p_value.get_type() == Variant::FLOAT) && double(p_value) == p_expected;
}

bool read_document(const String &p_path, Dictionary &r_document) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return false;
	}
	Ref<JSON> parser;
	parser.instantiate();
	if (parser->parse(file->get_as_text()) != OK || parser->get_data().get_type() != Variant::DICTIONARY) {
		return false;
	}
	r_document = parser->get_data();
	return true;
}

Dictionary snapshot(const Ref<EditorExportPreset> &p_preset) {
	Dictionary values;
	for (const KeyValue<StringName, Variant> &entry : p_preset->get_values()) {
		values[String(entry.key)] = entry.value;
	}
	return values.duplicate(true);
}

bool unchanged(const Ref<EditorExportPreset> &p_preset, const Dictionary &p_before) {
	if (p_preset->get_values().size() != p_before.size()) {
		return false;
	}
	for (const KeyValue<StringName, Variant> &entry : p_preset->get_values()) {
		if (!p_before.has(String(entry.key)) || entry.value != p_before[String(entry.key)] || p_preset->get(entry.key) != entry.value) {
			return false;
		}
	}
	return !bool(p_preset->get("build/sign")) && !bool(p_preset->get("build/export_project_only"));
}

bool messages_contain(EditorExportPlatformOpenHarmony *p_platform, const String &p_text) {
	for (int i = 0; i < p_platform->get_message_count(); i++) {
		if (p_platform->get_message(i).text.contains(p_text)) {
			return true;
		}
	}
	return false;
}

void audit_case(EditorExportPlatformOpenHarmony *p_platform, const Dictionary &p_case, const String &p_root, const Dictionary &p_context, const Ref<FileAccess> &p_results) {
	const int initial_failures = failures;
	const String name = p_case["name"];
	const String expected = p_case["expected"];
	const String output = p_root.path_join("exports").path_join(name + ".hap");
	const String project = output.get_basename();
	std::printf("AUTO_SIGN_CASE_BEGIN %s\n", name.utf8().get_data());
	const Ref<EditorExportPreset> preset = p_platform->create_preset();
	preset->set("custom_template/debug", p_case["template"]);
	preset->set("build/export_project_only", false);
	preset->set("build/sign", false);
	preset->set("build/override_project_dir", false);
	preset->set("build/sdk_version", "26.0.0");
	preset->set("build/bundle_id", "org.godotengine.autosignfixture");
	for (int i = 0; i < 7; i++) {
		// Deliberately nonexistent files and invalid manual algorithm/passwords:
		// any attempt to use manual signing should prevent the positive control.
		preset->set(signing_fields[i], String(secret_prefix) + itos(i));
	}
	const Dictionary before = snapshot(preset);
	const Dictionary context_before = p_context.duplicate(true);
	p_platform->clear_messages();
	check(!FileAccess::exists(output), "unique case has no preexisting target HAP");
	const Error result = p_platform->export_project_helper(preset, true, output, true, false, 0, true, nullptr, p_context);
	const Error expected_result = expected == "ok" ? OK : (expected == "auth" ? ERR_CANT_CREATE : (expected == "old_template" ? ERR_FILE_NOT_FOUND : ERR_INVALID_DATA));
	check(result == expected_result, "real export helper returns the expected protocol result");
	check(unchanged(preset, before), "all preset options and disabled manual signing remain unchanged");
	check(p_context.recursive_equal(context_before, 0), "export preserves the caller's public automatic context");
	check(!messages_contain(p_platform, secret_prefix), "Editor messages never expose fixture secrets or raw malformed output");
	check(FileAccess::exists(project.path_join("entry/src/main/resources/rawfile/template.pck")), "real notifier and exporter produce the project pack before protocol handling");

	Dictionary audit;
	const bool helper_ran = read_document(project.path_join("fixture-audit.json"), audit);
	if (expected == "old_template") {
		check(!helper_ran && !FileAccess::exists(project.path_join("tools/auto-sign.cjs")), "old template never executes a signing backend");
		check(messages_contain(p_platform, "template") && messages_contain(p_platform, "helper") && messages_contain(p_platform, "updated"), "missing helper requests updated game templates");
	} else {
		check(helper_ran && number_equals(audit.get("schemaVersion", Variant()), 1) && audit.get("case", "") == name &&
						audit.get("requestValidated", false).get_type() == Variant::BOOL && bool(audit.get("requestValidated", false)) &&
						number_equals(audit.get("requestSchemaVersion", Variant()), 1) && audit.get("projectDir", "") == project &&
						double(audit.get("nodeMajor", 0)) >= 22 && double(audit.get("pid", 0)) > 0,
				"external CLT Node backend ran and accepted only the exact public request metadata");
	}
	if (expected == "ok") {
		Ref<FileAccess> artifact = FileAccess::open(output, FileAccess::READ);
		const String contents = "FAKE HAP protocol fixture: " + name + "\n";
		check(artifact.is_valid() && artifact->get_length() > 0 && artifact->get_as_text() == contents, "C++ copies the backend's nonempty fake HAP bytes to the target");
		check(p_platform->get_message_count() == 0, "successful automatic context bypasses manual signing requirements");
	} else {
		check(!FileAccess::exists(output), "rejected protocol result never produces the target HAP");
		check(p_platform->get_message_count() > 0, "rejected result has an actionable Editor diagnostic");
	}
	if (expected == "auth") {
		check(p_platform->get_message_count() == 1 && p_platform->get_message(0).text == auth_message, "AUTH_REQUIRED exposes exactly the contracted actionable message");
	} else if (expected == "legacy_confirmation") {
		check(messages_contain(p_platform, "old confirmation protocol") && messages_contain(p_platform, "Update"), "old confirmation helper is rejected with a template update diagnostic");
	} else if (name == "invalid_json") {
		check(messages_contain(p_platform, "valid result"), "malformed JSON yields a stable diagnostic without echoing stdout");
	} else if (name.begins_with("schema_")) {
		check(messages_contain(p_platform, "unsupported result"), "unsupported result schema requests updated templates");
	} else if (expected == "invalid") {
		check(messages_contain(p_platform, "validated signed debug HAP"), "invalid artifact contract is explicitly rejected");
	}

	Dictionary evidence;
	evidence["case"] = name;
	evidence["result"] = int(result);
	evidence["expectedResult"] = int(expected_result);
	evidence["helperExecuted"] = helper_ran;
	evidence["messages"] = p_platform->get_message_count();
	evidence["presetPreserved"] = unchanged(preset, before);
	evidence["passed"] = failures == initial_failures;
	p_results->store_line(JSON::stringify(evidence));
	p_results->flush();
	std::printf("AUTO_SIGN_CASE_RESULT %s result=%d expected=%d failures=%d\n", name.utf8().get_data(), int(result), int(expected_result), failures - initial_failures);
}

void run_fixture(OS_OpenHarmony &p_os) {
	Dl_info library_info = {};
	const bool found_library = dladdr(reinterpret_cast<const void *>(&Main::start), &library_info) != 0;
	const String loaded_library = found_library && library_info.dli_fname ? OpenHarmonyToolchain::resolve_link(String::utf8(library_info.dli_fname)) : String();
	if (!check(!loaded_library.is_empty() && loaded_library == OS::get_singleton()->get_environment("GODOT_AUTO_SIGN_EXPORT_LIBRARY"), "runtime loads exactly the caller's already signed Editor library")) {
		return;
	}
	for (int i = 0; i < 1000; i++) {
		p_os.main_loop_iterate();
		if (i >= 2 && !EditorFileSystem::get_singleton()->is_scanning()) {
			break;
		}
		p_os.delay_usec(10000);
	}
	if (!check(!EditorFileSystem::get_singleton()->is_scanning(), "real Editor filesystem scan completes before exports")) {
		return;
	}
	EditorSettings *settings = EditorSettings::get_singleton();
	EditorExport *exports = EditorExport::get_singleton();
	if (!check(settings && exports, "Main::start initializes the real Editor settings and export registry")) {
		return;
	}
	const String root = OS::get_singleton()->get_environment("GODOT_AUTO_SIGN_EXPORT_ROOT");
	Dictionary manifest;
	if (!check(root.is_absolute_path() && read_document(root.path_join("manifest.json"), manifest) && number_equals(manifest.get("schemaVersion", Variant()), 1), "runner supplies an isolated public fixture manifest")) {
		return;
	}
	const String tools = manifest["commandLineToolsRoot"];
	const String sdk = root.path_join("sdk");
	settings->set_setting("export/openharmony/command_line_tools_root", tools);
	settings->set_setting("export/openharmony/sdk_root", sdk);
	settings->set_setting("export/openharmony/hdc_path", root.path_join("metadata/hdc-do-not-execute"));
	settings->set_setting("export/openharmony/use_broker", false);
	const String selected_tools = OpenHarmonyToolchain::command_line_tools(tools);
	const String node = OpenHarmonyToolchain::node(selected_tools);
	if (!check(selected_tools == tools && FileAccess::exists(node) && OpenHarmonyToolchain::sdk(sdk, selected_tools, "26.0.0") == sdk, "official CLT Node and isolated complete API26 SDK metadata resolve")) {
		return;
	}
	const int index = exports->get_export_platform_index_by_name("OpenHarmony");
	if (!check(index >= 0, "OpenHarmony export platform is registered")) {
		return;
	}
	const Ref<EditorExportPlatform> platform_ref = exports->get_export_platform(index);
	auto *platform = Object::cast_to<EditorExportPlatformOpenHarmony>(platform_ref.ptr());
	if (!check(platform != nullptr, "registered exporter is the production OpenHarmony implementation")) {
		return;
	}
	Dictionary context;
	context["schemaVersion"] = 1;
	context["projectIdentity"] = ProjectSettings::get_singleton()->globalize_path("res://project.godot").simplify_path();
	context["nodePath"] = node;
	context["commandLineToolsRoot"] = tools;
	context["sdkRoot"] = sdk;
	context["hdcPath"] = root.path_join("metadata/hdc-do-not-execute");
	context["target"] = "native-auto-sign-fake-serial";
	context["format"] = "hap";
	context["buildMode"] = "debug";
	context["cliPath"] = root.path_join("metadata/cli-do-not-execute.cjs");
	context["teamId"] = "public-fixture-team";
	context["loginDataDir"] = root.path_join("login-unused");
	Ref<FileAccess> results = FileAccess::open(root.path_join("case-results.jsonl"), FileAccess::WRITE);
	if (!check(results.is_valid() && manifest.get("cases", Variant()).get_type() == Variant::ARRAY, "case result journal is writable")) {
		return;
	}
	const Array cases = manifest["cases"];
	if (!check(cases.size() >= 15, "manifest covers success without confirmation, diagnostics, legacy protocol rejection and old templates")) {
		return;
	}
	for (const Variant &value : cases) {
		audit_case(platform, value, root, context, results);
	}
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
	// Drain callbacks queued by the final notifier before tearing down Editor.
	for (int i = 0; i < 3; i++) {
		os.main_loop_iterate();
	}
	os.main_loop_end();
	Main::cleanup();
	std::printf("AUTO_SIGN_EXPORT_%s failures=%d\n", failures ? "FAIL" : "PASS", failures);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
