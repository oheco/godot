// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

#include "sdk_config.h"
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

inline String resolve_link(const String &p_path) {
	return OpenHarmonyProjectConfig::canonical_sdk_path(p_path);
}

inline String command_line_tools(const String &p_override = "") {
	String selected = p_override;
	if (selected.is_empty()) {
		selected = OS::get_singleton()->get_environment("GODOT_OHOS_COMMAND_LINE_TOOLS_ROOT");
	}
	if (selected.is_empty()) {
		// oo's active launcher resides in <selected package>/.oo-launchers.
		String launcher = resolve_link(root().path_join("bin/hvigorw"));
		if (FileAccess::exists(launcher)) {
			selected = launcher.get_base_dir().get_base_dir();
		}
	}
	if (selected.is_empty()) {
		return "";
	}
	selected = resolve_link(selected);
	if (!FileAccess::exists(selected.path_join("hvigor/bin/hvigorw.js")) || !FileAccess::exists(selected.path_join("node/bin/node"))) {
		return "";
	}
	return selected;
}

inline String node(const String &p_tools) {
	return p_tools.is_empty() ? String() : p_tools.path_join("node/bin/node");
}

inline String hvigor(const String &p_tools) {
	// hvigorw's documented shell launcher executes this official JS sibling.
	return p_tools.is_empty() ? String() : p_tools.path_join("hvigor/bin/hvigorw.js");
}

inline String sdk(const String &p_override, const String &p_tools, const String &p_version = "26.0.0") {
	String selected = p_override;
	if (selected.is_empty()) {
		selected = OS::get_singleton()->get_environment("GODOT_OHOS_SDK_ROOT");
	}
	if (selected.is_empty() && !p_tools.is_empty()) {
		selected = p_tools.path_join("sdk");
	}
	if (selected.is_empty()) {
		return "";
	}
	Dictionary selection;
	if (OpenHarmonyProjectConfig::resolve_sdk(p_version, 26, selected, selection) != OK) {
		return "";
	}
	return selection["sdkRoot"];
}

inline String hdc(const String &p_override, const String &p_sdk, const String &p_version = "26.0.0") {
	if (!p_override.is_empty()) {
		return resolve_link(p_override);
	}
	String command = root().path_join("bin/hdc");
	if (FileAccess::exists(command)) {
		return resolve_link(command);
	}
	Dictionary selection;
	if (p_sdk.is_empty() || OpenHarmonyProjectConfig::resolve_sdk(p_version, 26, p_sdk, selection) != OK) {
		return "";
	}
	String foundation = selection["openharmonySdkRoot"];
	return foundation.path_join("toolchains/hdc");
}

} // namespace OpenHarmonyToolchain
