// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"

namespace OpenHarmonyToolchain {

inline String root() {
	String configured = OS::get_singleton()->get_environment("OHECO_ROOT");
	if (!configured.is_empty()) {
		return configured;
	}
#ifdef OPENHARMONY_ENABLED
	return "/storage/Users/currentUser/.oheco";
#else
	return OS::get_singleton()->get_environment("HOME").path_join(".oheco");
#endif
}

inline String resolve_link(String p_path) {
	Ref<DirAccess> directory = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	for (int i = 0; i < 16 && directory->is_link(p_path); i++) {
		String target = directory->read_link(p_path);
		if (target.is_empty()) {
			return String();
		}
		p_path = (target.is_absolute_path() ? target : p_path.get_base_dir().path_join(target)).simplify_path();
	}
	return p_path;
}

inline String node(const String &p_override) {
	return resolve_link(p_override.is_empty() ? root().path_join("bin/node") : p_override);
}

inline String hvigor(const String &p_override, const String &p_node) {
	if (!p_override.is_empty()) {
		return resolve_link(p_override);
	}
	return p_node.get_base_dir().get_base_dir().path_join("lib/node_modules/@oheco/hvigor/bin/hvigor.cjs");
}

inline String sdk(const String &p_override, const String &p_version = "26.0.0") {
	if (!p_override.is_empty()) {
		return p_override;
	}
	for (const char *name : { "GODOT_OHOS_SDK_ROOT", "OHOS_SDK_HOME", "OHOS_BASE_SDK_HOME" }) {
		String configured = OS::get_singleton()->get_environment(name);
		if (!configured.is_empty()) {
			return configured;
		}
	}
	Ref<DirAccess> views = DirAccess::open(root().path_join("sdk"));
	if (views.is_null()) {
		return String();
	}
	String selected;
	views->list_dir_begin();
	for (String name = views->get_next(); !name.is_empty(); name = views->get_next()) {
		if (!views->current_is_dir() || name.begins_with(".")) {
			continue;
		}
		String view = views->get_current_dir().path_join(name);
		Ref<FileAccess> file = FileAccess::open(view.path_join("view.json"), FileAccess::READ);
		if (file.is_null()) {
			continue;
		}
		Variant data = JSON::parse_string(file->get_as_text());
		if (data.get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary record = data;
		if (record.get("sdk_version", "") == p_version && DirAccess::dir_exists_absolute(view.path_join("root").path_join(p_version)) &&
				(selected.is_empty() || name.naturalnocasecmp_to(selected) > 0)) {
			selected = name;
		}
	}
	views->list_dir_end();
	return selected.is_empty() ? String() : views->get_current_dir().path_join(selected).path_join("root");
}

inline String hdc(const String &p_override, const String &p_sdk) {
	if (!p_override.is_empty()) {
		return resolve_link(p_override);
	}
	String command = root().path_join("bin/hdc");
	if (FileAccess::exists(command)) {
		return resolve_link(command);
	}
	return p_sdk.is_empty() ? String() : p_sdk.path_join("26.0.0/toolchains/hdc");
}

} // namespace OpenHarmonyToolchain
