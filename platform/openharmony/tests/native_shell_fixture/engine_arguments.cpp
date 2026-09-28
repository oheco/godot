// Godot Engine contributors. SPDX-License-Identifier: MIT
// Include the real implementation used by engine_host_openharmony.cpp; do not
// reimplement argument splitting, pack validation or normalization in the test.
#include "engine_arguments_openharmony.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using GodotOpenHarmony::ArgumentError;
using GodotOpenHarmony::prepare_packaged_arguments;
using Arguments = std::vector<std::string>;

int main() {
	const std::string bundle = "/bundle/rawfile/";
	auto exists = [](const std::string &name) { return name == "template.pck" || name == "custom.pck"; };
	auto missing = [](const std::string &) { return false; };
	assert((GodotOpenHarmony::read_packaged_command_line(" \r\n --verbose\r\n test project \n\t--\n player one\n") ==
			Arguments{ "--verbose", "test project", "--", "player one" }));

	Arguments output;
	assert(prepare_packaged_arguments("", {}, bundle, exists, output) == ArgumentError::OK);
#ifdef OVERRIDE_PATH_ENABLED
	const Arguments defaults{ "--main-pack", "/bundle/rawfile/template.pck" };
#else
	// No forbidden --main-pack injection in disable_path_overrides=yes builds.
	const Arguments defaults{};
#endif
	assert(output == defaults);
	output = { "unchanged" };
	assert(prepare_packaged_arguments("", {}, bundle, missing, output) == ArgumentError::MISSING_DEFAULT_PACK);
	assert((output == Arguments{ "unchanged" }));

	// Every combination of exporter/explicit separator, including ++, leaves
	// explicit engine options ahead of the combined user arguments.
	for (const std::string &raw_separator : { "--", "++" }) {
		for (const std::string &explicit_separator : { "--", "++" }) {
			const std::string raw = "--verbose\n" + raw_separator + "\nraw-user\n--editor\n++\n";
			Arguments explicit_args{ "--log-file", "/private/engine.log", explicit_separator, "explicit-user", "--main-pack", "user-value" };
			assert(prepare_packaged_arguments(raw, explicit_args, bundle, exists, output) == ArgumentError::OK);
			Arguments expected = defaults;
			expected.insert(expected.end(), { "--verbose", "--log-file", "/private/engine.log", "--", "raw-user", "--editor", "++", "explicit-user", "--main-pack", "user-value" });
			assert(output == expected);
		}
	}
	assert(prepare_packaged_arguments("--\nraw-user\n", { "--verbose" }, bundle, exists, output) == ArgumentError::OK);
	Arguments expected = defaults;
	expected.insert(expected.end(), { "--verbose", "--", "raw-user" });
	assert(output == expected);
	assert(prepare_packaged_arguments("--verbose\n", { "++", "user" }, bundle, exists, output) == ArgumentError::OK);
	expected = defaults;
	expected.insert(expected.end(), { "--verbose", "--", "user" });
	assert(output == expected);
	assert(prepare_packaged_arguments("--\n", { "++" }, bundle, exists, output) == ArgumentError::OK);
	expected = defaults;
	expected.push_back("--");
	assert(output == expected);

	// An option before one stream's delimiter cannot consume an operand from
	// the other stream or from its user segment.
	assert(prepare_packaged_arguments("--main-pack\n", { "custom.pck" }, bundle, exists, output) == ArgumentError::INVALID_PARAMETER);
	assert(prepare_packaged_arguments("--main-pack\n--\ncustom.pck\n", {}, bundle, exists, output) == ArgumentError::INVALID_PARAMETER);
	assert(prepare_packaged_arguments("", { "--main-pack", "" }, bundle, exists, output) == ArgumentError::INVALID_PARAMETER);
	assert(prepare_packaged_arguments("--log-file\n++\n", { "/private/engine.log" }, bundle, exists, output) == ArgumentError::INVALID_PARAMETER);
	for (const std::string &editor_flag : { "--editor", "-e", "--project-manager", "-p" }) {
		assert(prepare_packaged_arguments(editor_flag, {}, bundle, exists, output) == ArgumentError::INVALID_PARAMETER);
		assert(prepare_packaged_arguments("", { editor_flag }, bundle, exists, output) == ArgumentError::INVALID_PARAMETER);
	}
	// A flag-looking log file operand is an operand, not an editor request.
	assert(prepare_packaged_arguments("", { "--log-file", "--editor" }, bundle, exists, output) == ArgumentError::OK);

#ifdef OVERRIDE_PATH_ENABLED
	// Relative rawfile packs use the virtual bundle path. Explicit filesystem
	// paths remain unchanged, and explicit args override an exported pack last.
	assert(prepare_packaged_arguments("--main-pack\ntemplate.pck\n--\nraw-user\n",
			{ "--main-pack", "custom.pck", "++", "explicit-user" }, "/bundle/rawfile", exists, output) == ArgumentError::OK);
	assert((output == Arguments{ "--main-pack", "/bundle/rawfile/template.pck", "--main-pack", "/bundle/rawfile/custom.pck", "--", "raw-user", "explicit-user" }));
	assert(prepare_packaged_arguments("", { "--main-pack", "/private/custom.pck" }, bundle, missing, output) == ArgumentError::OK);
	assert((output == Arguments{ "--main-pack", "/private/custom.pck" }));
	assert(prepare_packaged_arguments("", { "--main-pack", "filesystem.pck" }, bundle, missing, output) == ArgumentError::OK);
	assert((output == Arguments{ "--main-pack", "filesystem.pck" }));
#else
	// Explicit path overrides stay forbidden, even for the default pack. The
	// hardened default succeeds via EXEC_PATH=template, never a CLI bypass.
	assert(prepare_packaged_arguments("", { "--main-pack", "template.pck" }, bundle, exists, output) == ArgumentError::PATH_OVERRIDES_DISABLED);
	assert(prepare_packaged_arguments("--main-pack\n/private/custom.pck\n", {}, bundle, exists, output) == ArgumentError::PATH_OVERRIDES_DISABLED);
#endif
	// Main-pack tokens after a separator are game arguments, not a substitute
	// for the required bundled default pack.
	assert(prepare_packaged_arguments("--\n--main-pack\ncustom.pck\n", {}, bundle, missing, output) == ArgumentError::MISSING_DEFAULT_PACK);
	// Exercise the same input/output aliasing used by the host.
	output = { "--verbose", "++", "user" };
	assert(prepare_packaged_arguments("", output, bundle, exists, output) == ArgumentError::OK);
	expected = defaults;
	expected.insert(expected.end(), { "--verbose", "--", "user" });
	assert(output == expected);
#ifdef OVERRIDE_PATH_ENABLED
	std::cout << "PASS: real engine argument helper (path overrides enabled)\n";
#else
	std::cout << "PASS: real engine argument helper (hardened, path overrides disabled)\n";
#endif
}
