// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

// Kept independent of Godot/ArkUI so the exact host argument handling can be
// exercised by a small native executable in both hardened and regular builds.
namespace GodotOpenHarmony {
enum class ArgumentError {
	OK,
	INVALID_PARAMETER,
	MISSING_DEFAULT_PACK,
	PATH_OVERRIDES_DISABLED,
};

struct ArgumentSegments {
	std::vector<std::string> engine;
	std::vector<std::string> user;
	bool separated = false;
};

inline ArgumentSegments split_arguments(const std::vector<std::string> &arguments) {
	ArgumentSegments result;
	for (const std::string &argument : arguments) {
		if (!result.separated && (argument == "--" || argument == "++")) {
			result.separated = true;
		} else if (result.separated) {
			result.user.push_back(argument);
		} else {
			result.engine.push_back(argument);
		}
	}
	return result;
}

inline std::vector<std::string> read_packaged_command_line(const std::string &content) {
	// The exporter's _cl_ has one argument per line, not shell quoting. Match
	// String::strip_edges() and discard empty lines, including CRLF endings.
	std::vector<std::string> result;
	size_t begin = 0;
	while (begin < content.size()) {
		size_t end = content.find('\n', begin);
		if (end == std::string::npos) {
			end = content.size();
		}
		size_t first = begin;
		size_t last = end;
		while (first < last && static_cast<unsigned char>(content[first]) <= 32) {
			++first;
		}
		while (last > first && static_cast<unsigned char>(content[last - 1]) <= 32) {
			--last;
		}
		if (first != last) {
			result.push_back(content.substr(first, last - first));
		}
		begin = end + 1;
	}
	return result;
}

template <typename RawFileExists>
ArgumentError prepare_packaged_arguments(const std::string &command_line, const std::vector<std::string> &explicit_arguments,
		const std::string &bundle_directory, RawFileExists rawfile_exists, std::vector<std::string> &output) {
	ArgumentSegments packaged = split_arguments(read_packaged_command_line(command_line));
	ArgumentSegments explicit_args = split_arguments(explicit_arguments);
	const std::string bundle_prefix = bundle_directory + (bundle_directory.empty() || bundle_directory.back() != '/' ? "/" : "");
	bool has_main_pack = false;
	// Validate each engine segment separately before concatenating: a dangling
	// _cl_ option must not steal the first explicit argument as its operand.
	for (std::vector<std::string> *segment : { &packaged.engine, &explicit_args.engine }) {
		for (size_t i = 0; i < segment->size(); ++i) {
			const std::string &argument = (*segment)[i];
			if (argument == "--editor" || argument == "-e" || argument == "--project-manager" || argument == "-p") {
				return ArgumentError::INVALID_PARAMETER;
			}
			if (argument == "--main-pack" || argument == "--log-file") {
				const bool main_pack = argument == "--main-pack";
				if (++i == segment->size() || (*segment)[i].empty()) {
					return ArgumentError::INVALID_PARAMETER;
				}
				if (!main_pack) {
					continue;
				}
#ifndef OVERRIDE_PATH_ENABLED
				// Do not silently bypass the upstream hardened-build restriction.
				return ArgumentError::PATH_OVERRIDES_DISABLED;
#else
				has_main_pack = true;
				std::string &pack = (*segment)[i];
				if (pack[0] != '/' && rawfile_exists(pack)) {
					pack = bundle_prefix + pack;
				}
#endif
			}
		}
	}
	std::vector<std::string> combined;
	if (!has_main_pack) {
		// Missing default packs are fatal even in editor builds, where Main
		// would otherwise silently fall back to the project manager.
		if (!rawfile_exists("template.pck")) {
			return ArgumentError::MISSING_DEFAULT_PACK;
		}
#ifdef OVERRIDE_PATH_ENABLED
		combined = { "--main-pack", bundle_prefix + "template.pck" };
#endif
		// Hardened builds discover bundle/template.pck using EXEC_PATH=template
		// instead: their command-line parser rejects --main-pack entirely.
	}
	combined.insert(combined.end(), packaged.engine.begin(), packaged.engine.end());
	combined.insert(combined.end(), explicit_args.engine.begin(), explicit_args.engine.end());
	if (packaged.separated || explicit_args.separated) {
		combined.push_back("--");
		combined.insert(combined.end(), packaged.user.begin(), packaged.user.end());
		combined.insert(combined.end(), explicit_args.user.begin(), explicit_args.user.end());
	}
	output.swap(combined);
	return ArgumentError::OK;
}
} // namespace GodotOpenHarmony
