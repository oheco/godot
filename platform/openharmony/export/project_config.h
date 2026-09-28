// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

#include "core/io/file_access.h"
#include "core/io/json.h"

#include <initializer_list>

namespace OpenHarmonyProjectConfig {

inline bool valid_sdk(const String &p_version) {
	const int open = p_version.find("(");
	if (open < 0 || !p_version.ends_with(")")) {
		return false;
	}
	const String api = p_version.substr(open + 1, p_version.length() - open - 2);
	const Vector<String> version = p_version.substr(0, open).split(".");
	if (version.size() != 3 || !api.is_valid_int() || api.to_int() < 23) {
		return false;
	}
	for (const String &part : version) {
		if (!part.is_valid_int() || part.to_int() < 0) {
			return false;
		}
	}
	return true;
}

inline Error read_document(const String &p_path, Dictionary &r_document) {
	Error error;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &error);
	ERR_FAIL_COND_V_MSG(file.is_null(), error, "Cannot read shared host configuration: " + p_path);
	Ref<JSON> parser;
	parser.instantiate();
	error = parser->parse(file->get_as_text());
	ERR_FAIL_COND_V_MSG(error != OK || parser->get_data().get_type() != Variant::DICTIONARY,
			ERR_PARSE_ERROR, "Shared host templates must use JSON-compatible configuration: " + p_path);
	r_document = parser->get_data();
	return OK;
}

inline Error write_document(const String &p_path, const Dictionary &p_document) {
	Error error;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &error);
	ERR_FAIL_COND_V(file.is_null(), error);
	const String content = JSON::stringify(p_document, "  ", false) + "\n";
	file->store_string(content);
	file.unref(); // Close before checking: stdio may defer write errors until flush.
	file = FileAccess::open(p_path, FileAccess::READ, &error);
	ERR_FAIL_COND_V(file.is_null(), error);
	ERR_FAIL_COND_V_MSG(file->get_as_text() != content, ERR_FILE_CANT_WRITE,
			"Unable to persist shared host configuration: " + p_path);
	return OK;
}

inline void set_string(Dictionary &r_document, const String &p_name, const String &p_value) {
	Array strings = r_document["string"];
	for (int i = 0; i < strings.size(); i++) {
		Dictionary entry = strings[i];
		if (entry.get("name", "") == p_name) {
			entry["value"] = p_value;
			return;
		}
	}
	Dictionary entry;
	entry["name"] = p_name;
	entry["value"] = p_value;
	strings.push_back(entry);
}

struct Field {
	const char *name;
	Variant::Type type;
};

inline bool has_shape(const Dictionary &p_value, std::initializer_list<Field> p_fields, bool p_exact = false) {
	if (p_exact && p_value.size() != int(p_fields.size())) {
		return false;
	}
	for (const Field &field : p_fields) {
		if (!p_value.has(field.name)) {
			return false;
		}
		const Variant value = p_value[field.name];
		if (value.get_type() != field.type && !(field.type == Variant::FLOAT && value.get_type() == Variant::INT)) {
			return false;
		}
	}
	return true;
}

inline bool valid_game_host(const Dictionary &p_host) {
	if (!has_shape(p_host, { { "schemaVersion", Variant::FLOAT }, { "host", Variant::DICTIONARY },
			{ "launch", Variant::DICTIONARY }, { "instances", Variant::DICTIONARY }, { "managed", Variant::DICTIONARY },
			{ "window", Variant::DICTIONARY }, { "diagnostics", Variant::DICTIONARY } }, true)) {
		return false;
	}
	Dictionary host = p_host["host"], launch = p_host["launch"], instances = p_host["instances"],
			managed = p_host["managed"], window = p_host["window"], diagnostics = p_host["diagnostics"];
	if (!has_shape(host, { { "role", Variant::STRING } }, true) ||
			!has_shape(launch, { { "defaultMode", Variant::STRING }, { "defaultArguments", Variant::ARRAY }, { "acceptProjectRequests", Variant::BOOL } }, true) ||
			!has_shape(instances, { { "policy", Variant::STRING }, { "maxCount", Variant::FLOAT } }, true) ||
			!has_shape(managed, { { "mode", Variant::STRING }, { "sdkSource", Variant::STRING } }, true) ||
			!has_shape(window, { { "expandIntoSystemArea", Variant::BOOL } }, true) ||
			!has_shape(diagnostics, { { "level", Variant::STRING } }, true)) {
		return false;
	}
	const double count = instances["maxCount"];
	const String level = diagnostics["level"];
	if (double(p_host["schemaVersion"]) != 1 || host["role"] != "game" || launch["defaultMode"] != "packaged-game" ||
			bool(launch["acceptProjectRequests"]) || instances["policy"] != "disabled" || count < 1 || count > 5 || count != int(count) ||
			managed["mode"] != "none" || managed["sdkSource"] != "oheco" || (level != "normal" && level != "verbose")) {
		return false;
	}
	Array arguments = launch["defaultArguments"];
	for (const Variant &argument : arguments) {
		if (argument.get_type() != Variant::STRING || String(argument).find_char(0) != -1) {
			return false;
		}
	}
	return true;
}

inline bool valid_documents(const Dictionary *p_documents) {
	if (!has_shape(p_documents[0], { { "app", Variant::DICTIONARY } }) ||
			!has_shape(p_documents[1], { { "string", Variant::ARRAY } }) || !has_shape(p_documents[2], { { "string", Variant::ARRAY } }) ||
			!has_shape(p_documents[3], { { "app", Variant::DICTIONARY } }) || !has_shape(p_documents[4], { { "buildOption", Variant::DICTIONARY } }) ||
			!has_shape(p_documents[5], { { "module", Variant::DICTIONARY } }) || !valid_game_host(p_documents[6])) {
		return false;
	}
	for (int i : { 1, 2 }) {
		Array strings = p_documents[i]["string"];
		for (const Variant &entry : strings) {
			if (entry.get_type() != Variant::DICTIONARY || !has_shape(entry, { { "name", Variant::STRING }, { "value", Variant::STRING } })) {
				return false;
			}
		}
	}
	Dictionary app = p_documents[3]["app"], build = p_documents[4]["buildOption"], module = p_documents[5]["module"];
	if (!has_shape(app, { { "products", Variant::ARRAY } }) || !has_shape(build, { { "externalNativeOptions", Variant::DICTIONARY } }) ||
			!has_shape(module, { { "abilities", Variant::ARRAY }, { "mainElement", Variant::STRING } }) || module["mainElement"] != "EntryAbility") {
		return false;
	}
	Array products = app["products"], abilities = module["abilities"];
	if (products.is_empty() || abilities.is_empty()) {
		return false;
	}
	for (const Variant &product : products) {
		if (product.get_type() != Variant::DICTIONARY || !has_shape(product, { { "name", Variant::STRING } })) {
			return false;
		}
	}
	for (const Variant &ability : abilities) {
		if (ability.get_type() != Variant::DICTIONARY || !has_shape(ability, { { "name", Variant::STRING } })) {
			return false;
		}
	}
	return true;
}

inline Error configure_game(const String &p_root, const Dictionary &p_options) {
	const String paths[] = {
		"AppScope/app.json5", "AppScope/resources/base/element/string.json",
		"entry/src/main/resources/base/element/string.json", "build-profile.json5",
		"entry/build-profile.json5", "entry/src/main/module.json5",
		"entry/src/main/resources/rawfile/godot_host.json"
	};
	Dictionary documents[7];
	for (int i = 0; i < 7; i++) {
		Error error = read_document(p_root.path_join(paths[i]), documents[i]);
		ERR_FAIL_COND_V(error != OK, error);
	}
	ERR_FAIL_COND_V_MSG(!valid_documents(documents), ERR_INVALID_DATA,
			"Invalid shared game host schema. Rebuild or repair the OpenHarmony template.");
	ERR_FAIL_COND_V(!valid_sdk(p_options["sdk"]), ERR_INVALID_PARAMETER);
	const int64_t version_code = p_options["version_code"];
	ERR_FAIL_COND_V(version_code < 1 || version_code > 2147483647 || String(p_options["version_name"]).is_empty(), ERR_INVALID_PARAMETER);
	Dictionary app = documents[0]["app"];
	app["bundleName"] = p_options["bundle"];
	app["versionCode"] = p_options["version_code"];
	app["versionName"] = p_options["version_name"];
	app.erase("multiAppMode");
	set_string(documents[1], "app_name", p_options["name"]);
	set_string(documents[2], "EntryAbility_label", p_options["name"]);
	set_string(documents[2], "EntryAbility_desc", p_options["name"]);
	set_string(documents[2], "user_permissions", bool(p_options["microphone"]) ? "ohos.permission.MICROPHONE" : "");
	Dictionary build_app = documents[3]["app"];
	Array products = build_app.get("products", Array());
	ERR_FAIL_COND_V(products.is_empty(), ERR_INVALID_DATA);
	for (int i = 0; i < products.size(); i++) {
		Dictionary product = products[i];
		product["compileSdkVersion"] = p_options["sdk"];
		product["targetSdkVersion"] = p_options["sdk"];
		product["compatibleSdkVersion"] = p_options["sdk"];
	}
	Dictionary build = documents[4]["buildOption"];
	Dictionary native = build.get("externalNativeOptions", Dictionary());
	Array architectures;
	architectures.push_back(p_options["arch"]);
	native["abiFilters"] = architectures;
	build["externalNativeOptions"] = native;
	Dictionary module = documents[5]["module"];
	Array abilities = module.get("abilities", Array());
	ERR_FAIL_COND_V(abilities.is_empty(), ERR_INVALID_DATA);
	Dictionary ability = abilities[0];
	ERR_FAIL_COND_V(ability.get("name", "") != "EntryAbility", ERR_INVALID_DATA);
	ability["orientation"] = p_options["orientation"];
	ability.erase("launchType");
	Array permissions;
	if (p_options["internet"]) {
		Dictionary permission;
		permission["name"] = "ohos.permission.INTERNET";
		permissions.push_back(permission);
	}
	if (p_options["microphone"]) {
		Dictionary permission, scene;
		Array users;
		users.push_back("EntryAbility");
		scene["abilities"] = users;
		scene["when"] = "inuse";
		permission["name"] = "ohos.permission.MICROPHONE";
		permission["reason"] = "$string:MICROPHONE_reason";
		permission["usedScene"] = scene;
		permissions.push_back(permission);
	}
	module["requestPermissions"] = permissions;
	Dictionary window = documents[6]["window"];
	window["expandIntoSystemArea"] = p_options["expand_safe_area"];
	Dictionary diagnostics = documents[6]["diagnostics"];
	diagnostics["level"] = bool(p_options["verbose"]) ? "verbose" : "normal";
	for (int i = 0; i < 7; i++) {
		Error error = write_document(p_root.path_join(paths[i]), documents[i]);
		ERR_FAIL_COND_V(error != OK, error);
	}
	return OK;
}
} // namespace OpenHarmonyProjectConfig
