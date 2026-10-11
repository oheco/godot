// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"

#include <initializer_list>

namespace OpenHarmonyProjectConfig {

inline int sdk_api(const String &p_version) {
	const Vector<String> parts = p_version.split(".");
	if (parts.size() != 3) {
		return 0;
	}
	for (const String &part : parts) {
		if (part.is_empty() || part[0] < '0' || part[0] > '9' || !part.is_valid_int()) {
			return 0;
		}
	}
	return parts[0].to_int();
}

inline bool valid_sdk(const String &p_version) {
	return sdk_api(p_version) >= 26;
}

inline bool read_sdk_metadata(const String &p_path, Dictionary &r_metadata) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return false;
	}
	Variant data = JSON::parse_string(file->get_as_text());
	if (data.get_type() != Variant::DICTIONARY) {
		return false;
	}
	r_metadata = data;
	return true;
}

inline bool sdk_metadata_api_matches(const Variant &p_api, int p_expected) {
	if (p_api.get_type() == Variant::STRING) {
		return String(p_api) == itos(p_expected);
	}
	return (p_api.get_type() == Variant::INT || p_api.get_type() == Variant::FLOAT) && double(p_api) == p_expected;
}

inline String canonical_sdk_path(String p_path) {
	Ref<DirAccess> directory = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	for (int links = 0; links < 32; links++) {
		const Vector<String> parts = p_path.split("/", false);
		String prefix = p_path.begins_with("//") ? "//" : (p_path.begins_with("/") ? "/" : "");
		bool changed = false;
		for (int i = 0; i < parts.size(); i++) {
			prefix = prefix.path_join(parts[i]);
			if (!directory->is_link(prefix)) {
				continue;
			}
			const String target = directory->read_link(prefix);
			if (target.is_empty()) {
				return "";
			}
			p_path = target.is_absolute_path() ? target : prefix.get_base_dir().path_join(target);
			for (int j = i + 1; j < parts.size(); j++) {
				p_path = p_path.path_join(parts[j]);
			}
			changed = true;
			break;
		}
		if (!changed) {
			return p_path.simplify_path();
		}
	}
	return "";
}

inline bool sdk_components_match(const String &p_root, const String &p_filename, const String &p_version, bool p_hms = false,
		String p_package_version = "", String p_release_type = "") {
	for (const char *component : { "native", "ets", "js", "toolchains", "previewer" }) {
		if (p_hms && (String(component) == "js" || String(component) == "previewer")) {
			continue;
		}
		Dictionary metadata;
		if (!read_sdk_metadata(p_root.path_join(component).path_join(p_filename), metadata) ||
				!sdk_metadata_api_matches(metadata.get("apiVersion", Variant()), sdk_api(p_version)) ||
				metadata.get("platformVersion", "") != p_version || metadata.get("path", "") != component) {
			return false;
		}
		if (metadata.get("version", Variant()).get_type() != Variant::STRING || metadata.get("releaseType", Variant()).get_type() != Variant::STRING) {
			return false;
		}
		const String package_version = metadata.get("version", ""), release_type = metadata.get("releaseType", "");
		if (package_version.is_empty() || release_type.is_empty()) {
			return false;
		}
		if (p_package_version.is_empty() && p_release_type.is_empty()) {
			p_package_version = package_version;
			p_release_type = release_type;
		}
		if (package_version != p_package_version || release_type != p_release_type) {
			return false;
		}
	}
	return true;
}

inline Error resolve_sdk(const String &p_version, int p_compatible_api, const String &p_sdk_root, Dictionary &r_selection) {
	ERR_FAIL_COND_V_MSG(!valid_sdk(p_version) || p_compatible_api < 26 || p_compatible_api > sdk_api(p_version),
			ERR_INVALID_PARAMETER, "Use a HarmonyOS/OpenHarmony SDK version such as 26.0.0 and a compatible API between 26 and the compile API.");
	r_selection["compileSdkVersion"] = p_version;
	r_selection["targetSdkVersion"] = p_version;
	r_selection["compatibleSdkVersion"] = itos(p_compatible_api) + ".0.0";
	r_selection["runtimeOS"] = "OpenHarmony";
	r_selection["sdkRoot"] = "";
	r_selection["openharmonySdkRoot"] = "";
	// Source-only templates have no selected SDK. Actual SDK metadata, when
	// available, determines the mode; API26 versions are dotted in both families.
	if (p_sdk_root.is_empty()) {
		return OK;
	}
	ERR_FAIL_COND_V_MSG(!p_sdk_root.is_absolute_path() || !DirAccess::dir_exists_absolute(p_sdk_root),
			ERR_FILE_NOT_FOUND, "The selected SDK root must be an existing absolute directory.");
	String root = canonical_sdk_path(p_sdk_root);
	ERR_FAIL_COND_V_MSG(root.is_empty(), ERR_INVALID_DATA, "Cannot resolve the selected SDK path or its symbolic links.");
	if (root.length() > 1) {
		root = root.trim_suffix("/");
	}
	const String bundle = FileAccess::exists(root.path_join("default/sdk-pkg.json")) ? root.path_join("default") : root;
	const String descriptor = bundle.path_join("sdk-pkg.json");
	if (FileAccess::exists(descriptor)) {
		Dictionary metadata;
		ERR_FAIL_COND_V_MSG(bundle.get_file() != "default" || !read_sdk_metadata(descriptor, metadata) ||
				metadata.get("data", Variant()).get_type() != Variant::DICTIONARY,
				ERR_INVALID_DATA, "HarmonyOS SDK requires a valid default/sdk-pkg.json bundle.");
		Dictionary data = metadata["data"];
		ERR_FAIL_COND_V_MSG(!sdk_metadata_api_matches(data.get("apiVersion", Variant()), sdk_api(p_version)) || data.get("platformVersion", "") != p_version ||
				data.get("version", Variant()).get_type() != Variant::STRING || data.get("releaseType", Variant()).get_type() != Variant::STRING ||
				String(data.get("version", "")).is_empty() || String(data.get("releaseType", "")).is_empty() ||
				!sdk_components_match(bundle.path_join("openharmony"), "oh-uni-package.json", p_version, false, data.get("version", ""), data.get("releaseType", "")) ||
				!sdk_components_match(bundle.path_join("hms"), "uni-package.json", p_version, true, data.get("version", ""), data.get("releaseType", "")),
				ERR_INVALID_DATA, "HarmonyOS SDK components are incomplete or do not match the selected API/version.");
		r_selection["runtimeOS"] = "HarmonyOS";
		r_selection["sdkRoot"] = bundle.get_base_dir();
		r_selection["openharmonySdkRoot"] = bundle.path_join("openharmony");
	} else {
		ERR_FAIL_COND_V_MSG(DirAccess::dir_exists_absolute(root.path_join("hms")) || DirAccess::dir_exists_absolute(root.path_join("default/hms")),
				ERR_INVALID_DATA, "Incomplete HarmonyOS SDK: missing default/sdk-pkg.json.");
		const bool version_directory = FileAccess::exists(root.path_join("native/oh-uni-package.json"));
		ERR_FAIL_COND_V_MSG(version_directory && root.get_file() != p_version,
				ERR_INVALID_DATA, "OpenHarmony SDK requires a versioned root: <SDK home>/<full API>/<component>.");
		const String foundation = version_directory ? root : root.path_join(p_version);
		ERR_FAIL_COND_V_MSG(!sdk_components_match(foundation, "oh-uni-package.json", p_version),
				ERR_INVALID_DATA, "OpenHarmony SDK components are incomplete or do not match the selected API/version.");
		r_selection["sdkRoot"] = version_directory ? root.get_base_dir() : root;
		r_selection["openharmonySdkRoot"] = foundation;
	}
	return OK;
}

} // namespace OpenHarmonyProjectConfig
