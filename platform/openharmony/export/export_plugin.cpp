/**************************************************************************/
/*  export_plugin.cpp                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "export_plugin.h"

#include "logo_svg.gen.h"
#include "project_config.h"
#include "run_icon_svg.gen.h"
#include "toolchain.h"

#ifdef OPENHARMONY_ENABLED
#include "broker_client.h"
#endif

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/json.h"
#include "core/io/marshalls.h"
#include "core/io/zip_io.h"
#include "core/object/callable_mp.h"
#include "core/os/shared_object.h"
#include "core/version.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_node.h"
#include "editor/export/editor_export.h"
#include "editor/file_system/editor_paths.h"
#include "editor/import/resource_importer_texture.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "main/splash.gen.h"
#include "scene/resources/image_texture.h"

#include "modules/svg/image_loader_svg.h"

#include <string.h>

// OpenHarmony permissions
static const char *OPENHARMONY_PERMISSIONS[] = {
	"ohos.permission.INTERNET",
	"ohos.permission.MICROPHONE",
	nullptr
};

static const char *OPENHARMONY_DEFAULT_SDK_VERSION = "26.0.0";
static const char *OPENHARMONY_DEFAULT_BUNDLE_ID = "org.godotengine.template";
static const char *OPENHARMONY_ORIENTATION_ENUMS = "landscape,landscape_inverted,auto_rotation_landscape,auto_rotation_landscape_restricted,portrait,portrait_inverted,auto_rotation_portrait,auto_rotation_portrait_restricted,auto_rotation_unspecified,auto_rotation_restricted,follow_recent,follow_desktop";

void EditorExportPlatformOpenHarmony::get_preset_features(const Ref<EditorExportPreset> &p_preset, List<String> *r_features) const {
	r_features->push_back("etc2");
	r_features->push_back("astc");
	if (p_preset->get("architectures/arm64")) {
		r_features->push_back("arm64");
	} else if (p_preset->get("architectures/x86_64")) {
		r_features->push_back("x86_64");
	}
}

void EditorExportPlatformOpenHarmony::get_export_options(List<ExportOption> *r_options) const {
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "custom_template/debug", PROPERTY_HINT_GLOBAL_FILE, "*.zip"), ""));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "custom_template/release", PROPERTY_HINT_GLOBAL_FILE, "*.zip"), ""));

	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, vformat("%s/%s", PNAME("architectures"), "arm64")), true, true, true));
	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, vformat("%s/%s", PNAME("architectures"), "x86_64")), false, true, true));

	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, "build/export_project_only"), false, true, true));
	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, "build/override_project_dir"), false, true, true));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "build/sdk_version", PROPERTY_HINT_PLACEHOLDER_TEXT, vformat("%s (default)", OPENHARMONY_DEFAULT_SDK_VERSION)), "", false, true));
	r_options->push_back(ExportOption(PropertyInfo(Variant::INT, "build/compatible_api", PROPERTY_HINT_RANGE, "23,99,1"), 23));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "build/bundle_id", PROPERTY_HINT_PLACEHOLDER_TEXT, vformat("%s (default)", OPENHARMONY_DEFAULT_BUNDLE_ID)), "", false, true));
	r_options->push_back(ExportOption(PropertyInfo(Variant::INT, "build/default_orientation", PROPERTY_HINT_ENUM, OPENHARMONY_ORIENTATION_ENUMS), 0, true, true));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "build/background_image", PROPERTY_HINT_GLOBAL_FILE, "*.png"), "", false, false));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "build/foreground_image", PROPERTY_HINT_GLOBAL_FILE, "*.png"), "", false, false));
	r_options->push_back(ExportOption(PropertyInfo(Variant::INT, "build/version_code", PROPERTY_HINT_RANGE, "1,2147483647,1"), 1000000));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "build/version_name"), "1.0.0"));
	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, "build/expand_into_system_area"), true));
	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, "build/verbose_diagnostics"), false));
	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, "build/sign"), false, true, true));

	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "sign/store_file", PROPERTY_HINT_GLOBAL_FILE, "*.p12", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SECRET), ""));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "sign/store_password", PROPERTY_HINT_PASSWORD, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SECRET), ""));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "sign/key_alias", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SECRET), ""));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "sign/key_password", PROPERTY_HINT_PASSWORD, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SECRET), ""));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "sign/sign_alg", PROPERTY_HINT_ENUM_SUGGESTION, "SHA256withECDSA", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SECRET), "SHA256withECDSA"));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "sign/profile_file", PROPERTY_HINT_GLOBAL_FILE, "*.p7b", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SECRET), ""));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "sign/certpath_file", PROPERTY_HINT_GLOBAL_FILE, "*.cer", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SECRET), ""));

	const char **perms = OPENHARMONY_PERMISSIONS;
	while (*perms) {
		r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, vformat("%s/%s", PNAME("permissions"), String(*perms))), false));
		perms++;
	}
}

bool EditorExportPlatformOpenHarmony::get_export_option_visibility(const EditorExportPreset *p_preset, const String &p_option) const {
	if (p_preset == nullptr) {
		return true;
	}

	bool advanced_options_enabled = p_preset->are_advanced_options_enabled();

	// Hide custom template options unless advanced options are enabled
	if (p_option == "custom_template/debug" || p_option == "custom_template/release") {
		return advanced_options_enabled;
	}

	// Hide architecture options unless advanced options are enabled
	if (p_option.begins_with("architectures/")) {
		return advanced_options_enabled;
	}

	// Hide sign options unless build/sign is enabled
	bool sign_enabled = p_preset->get("build/sign");
	if (p_option.begins_with("sign/")) {
		return sign_enabled;
	}

	return true;
}

String EditorExportPlatformOpenHarmony::get_export_option_warning(const EditorExportPreset *p_preset, const StringName &p_name) const {
	if (p_preset == nullptr) {
		return String();
	}

	// Check architecture selection - only one should be selected
	if (String(p_name).begins_with("architectures/")) {
		bool arm64_selected = p_preset->get("architectures/arm64");
		bool x86_64_selected = p_preset->get("architectures/x86_64");

		int selected_count = 0;
		if (arm64_selected) {
			selected_count++;
		}
		if (x86_64_selected) {
			selected_count++;
		}

		if (selected_count == 0) {
			return TTR("At least one architecture must be selected.");
		} else if (selected_count > 1) {
			return TTR("Only one architecture can be selected at a time.");
		}
	}

	if (p_name == "build/sdk_version") {
		String sdk = p_preset->get(p_name);
		if (!sdk.is_empty() && !OpenHarmonyProjectConfig::valid_sdk(sdk)) {
			return TTR("The shared OpenHarmony host requires API 23 or newer (oo SDK default: 26.0.0).");
		}
	}

	// Check sign options when build/sign is enabled
	bool sign_enabled = p_preset->get("build/sign");
	if (sign_enabled && String(p_name).begins_with("sign/")) {
		String value = p_preset->get(p_name);
		if (value.is_empty()) {
			if (p_name == "sign/store_file") {
				return TTR("Store file path is required when signing is enabled.");
			} else if (p_name == "sign/store_password") {
				return TTR("Store password is required when signing is enabled.");
			} else if (p_name == "sign/key_alias") {
				return TTR("Key alias is required when signing is enabled.");
			} else if (p_name == "sign/key_password") {
				return TTR("Key password is required when signing is enabled.");
			} else if (p_name == "sign/sign_alg") {
				return TTR("Sign algorithm is required when signing is enabled.");
			} else if (p_name == "sign/profile_file") {
				return TTR("Profile file path is required when signing is enabled.");
			} else if (p_name == "sign/certpath_file") {
				return TTR("Certificate path file is required when signing is enabled.");
			}
		}
	}

	return String();
}

String EditorExportPlatformOpenHarmony::get_name() const {
	return "OpenHarmony";
}

String EditorExportPlatformOpenHarmony::get_os_name() const {
	return "OpenHarmony";
}

Ref<Texture2D> EditorExportPlatformOpenHarmony::get_logo() const {
	return logo;
}

Ref<Texture2D> EditorExportPlatformOpenHarmony::get_run_icon() const {
	return run_icon;
}

bool EditorExportPlatformOpenHarmony::poll_export() {
	bool dc = devices_changed.is_set();
	if (dc) {
		// don't clear unless we're reporting true, to avoid race
		devices_changed.clear();
	}
	return dc;
}

int EditorExportPlatformOpenHarmony::get_options_count() const {
	MutexLock lock(device_lock);
	return devices.size();
}

String EditorExportPlatformOpenHarmony::get_options_tooltip() const {
	return TTR("Select device from the list");
}

String EditorExportPlatformOpenHarmony::get_option_label(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, devices.size(), "");
	MutexLock lock(device_lock);
	return devices[p_index];
}

String EditorExportPlatformOpenHarmony::get_option_tooltip(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, devices.size(), "");
	MutexLock lock(device_lock);
	return "Device ID: " + devices[p_index];
}

String EditorExportPlatformOpenHarmony::get_device_architecture(int p_index) const {
	// Only arm64 is supported for now.
	return "arm64";
}

List<String> EditorExportPlatformOpenHarmony::get_binary_extensions(const Ref<EditorExportPreset> &p_preset) const {
	return List<String>{ "hap", "app" };
}

Error EditorExportPlatformOpenHarmony::export_project(const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path, BitField<EditorExportPlatform::DebugFlags> p_flags, bool p_notify) {
	bool should_sign = p_preset->get("build/sign");
	bool export_project_only = p_preset->get("build/export_project_only");
	return export_project_helper(p_preset, p_debug, p_path, should_sign, export_project_only, p_flags, p_notify);
}

Error EditorExportPlatformOpenHarmony::export_project_helper(const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path, bool should_sign, bool export_project_only, BitField<EditorExportPlatform::DebugFlags> p_flags, bool p_notify) {
	ExportNotifier notifier(*this, p_preset, p_debug, p_path, p_flags, p_notify);

	EditorProgress ep("export", TTR("Exporting OpenHarmony Project"), 7, true);

	String requested_sdk = p_preset->get("build/sdk_version");
	if (!requested_sdk.is_empty() && !OpenHarmonyProjectConfig::valid_sdk(requested_sdk)) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), TTR("The shared OpenHarmony host requires API 23 or newer (oo SDK default: 26.0.0)."));
		return ERR_INVALID_PARAMETER;
	}
	bool has_sign = p_preset->get("build/sign");
	if (should_sign && !has_sign) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Code Signing"), TTR("Signing is not enabled in the export preset."));
		return ERR_CANT_CREATE;
	}

	if (ep.step(TTR("Preparing templates..."), 0)) {
		return ERR_SKIP;
	}

	String custom_debug = p_preset->get("custom_template/debug");
	String custom_release = p_preset->get("custom_template/release");
	String template_path = p_debug ? custom_debug : custom_release;
	template_path = template_path.strip_edges();

	if (template_path.is_empty()) {
		String template_file_name = p_debug ? "openharmony_debug_arm64-v8a.zip" : "openharmony_release_arm64-v8a.zip";
		String err;
		template_path = find_game_template(template_file_name, err);
		if (template_path.is_empty()) {
			add_message(EXPORT_MESSAGE_ERROR, TTR("Prepare Templates"), TTR("Export template not found.") + "\n" + err);
			return ERR_FILE_NOT_FOUND;
		}
	}

	if (!FileAccess::exists(template_path)) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Prepare Templates"), vformat(TTR("Template file not found: \"%s\"."), template_path));
		return ERR_FILE_NOT_FOUND;
	}

	if (ep.step(TTR("Creating project directory..."), 1)) {
		return ERR_SKIP;
	}

	String base_dir = p_path.get_base_dir();

	if (base_dir.is_relative_path()) {
		base_dir = OS::get_singleton()->get_resource_dir().path_join(base_dir);
	}
	base_dir = ProjectSettings::get_singleton()->globalize_path(base_dir).simplify_path();
	String project_name = p_path.get_file().get_basename();
	String project_dir = base_dir.path_join(project_name);
	String file_ext = p_path.get_extension();
	if (!project_dir.is_absolute_path() || project_name.is_empty() || project_name == "." || project_name == ".." ||
			(file_ext.to_lower() != "hap" && file_ext.to_lower() != "app")) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Path"), TTR("Select an absolute HAP or APP output path with a nonempty project name."));
		return ERR_FILE_BAD_PATH;
	}

	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (!da->dir_exists(base_dir)) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), vformat(TTR("Target folder does not exist or is inaccessible: \"%s\"."), base_dir));
		return ERR_FILE_BAD_PATH;
	}

	if (da->dir_exists(project_dir)) {
		bool override_project = p_preset->get("build/override_project_dir");
		if (override_project) {
			Error err = da->change_dir(project_dir);
			if (err == OK) {
				da->erase_contents_recursive();
				da->change_dir("..");
				da->remove(project_name);
			}
		} else {
			add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), vformat(TTR("Project dir is already exists (Enable \"Override Project Dir\" to force override): \"%s\"."), project_dir));
			return ERR_ALREADY_EXISTS;
		}
	}

	Error err = da->make_dir_recursive(project_dir);
	if (err != OK) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), vformat(TTR("Could not create project directory: \"%s\"."), project_dir));
		return err;
	}

	if (ep.step(TTR("Extracting template files..."), 2)) {
		return ERR_SKIP;
	}

	Ref<FileAccess> io_fa;
	zlib_filefunc_def io = zipio_create_io(&io_fa);
	unzFile pkg = unzOpen2(template_path.utf8().get_data(), &io);
	if (!pkg) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), vformat(TTR("Could not open template for export: \"%s\"."), template_path));
		return ERR_FILE_NOT_FOUND;
	}

	HashSet<String> extracted_files;
	int ret = unzGoToFirstFile(pkg);
	while (ret == UNZ_OK) {
		unz_file_info info;
		char filename[16384];
		ret = unzGetCurrentFileInfo(pkg, &info, filename, 16384, nullptr, 0, nullptr, 0);
		if (ret != UNZ_OK) {
			break;
		}

		String file = String::utf8(filename);
		if (file.is_empty() || file.is_absolute_path() || file.contains("\\") || file.contains(":") || file.split("/").has("..") ||
				(!file.ends_with("/") && extracted_files.has(file.to_lower()))) {
			unzClose(pkg);
			add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), TTR("The export template contains an unsafe or duplicate file path."));
			return ERR_FILE_CORRUPT;
		}
		if (!file.ends_with("/")) {
			extracted_files.insert(file.to_lower());
		}
		String full_path = project_dir.path_join(file);

		if (file.ends_with("/")) {
			da->make_dir_recursive(full_path);
		} else {
			da->make_dir_recursive(full_path.get_base_dir());

			ret = unzOpenCurrentFile(pkg);
			if (ret == UNZ_OK) {
				Ref<FileAccess> f = FileAccess::open(full_path, FileAccess::WRITE);
				if (f.is_valid()) {
					const int buffer_size = 65536;
					uint8_t buffer[buffer_size];

					while (true) {
						int bytes_read = unzReadCurrentFile(pkg, buffer, buffer_size);
						if (bytes_read == 0) {
							break;
						}
						if (bytes_read < 0) {
							add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), vformat(TTR("Could not read from template: \"%s\"."), template_path));
							unzCloseCurrentFile(pkg);
							unzClose(pkg);
							return ERR_FILE_CORRUPT;
						}
						f->store_buffer(buffer, bytes_read);
					}
				} else {
					add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), vformat(TTR("Could not write file: \"%s\"."), full_path));
					unzCloseCurrentFile(pkg);
					unzClose(pkg);
					return ERR_FILE_CANT_WRITE;
				}
				unzCloseCurrentFile(pkg);
			}
		}

		ret = unzGoToNextFile(pkg);
	}
	unzClose(pkg);

	if (ep.step(TTR("Configuring project files..."), 3)) {
		return ERR_SKIP;
	}

	Vector<String> command_line_flags = gen_export_flags(p_flags);
	if (p_flags.has_flag(DEBUG_FLAG_REMOTE_DEBUG_LOCALHOST)) {
		for (int i = 0; i + 1 < command_line_flags.size(); i++) {
			if (command_line_flags[i] == "--remote-debug") {
				command_line_flags.write[i + 1] = get_debug_protocol() + "127.0.0.1:" + itos(get_debug_port());
			} else if (command_line_flags[i] == "--remote-fs") {
				command_line_flags.write[i + 1] = "127.0.0.1:" + itos(int(EDITOR_GET("filesystem/file_server/port")));
			}
		}
	}
	String cl_file_path = project_dir.path_join("entry/src/main/resources/rawfile/_cl_");
	da->make_dir_recursive(cl_file_path.get_base_dir());

	Ref<FileAccess> cl_file = FileAccess::open(cl_file_path, FileAccess::WRITE);
	if (cl_file.is_null()) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), vformat(TTR("Could not write command line file: \"%s\"."), cl_file_path));
		return ERR_FILE_CANT_WRITE;
	}

	for (const String &flag : command_line_flags) {
		CharString cs = (flag + "\n").utf8();
		cl_file->store_buffer((const uint8_t *)cs.get_data(), cs.length());
	}
	cl_file.unref();

	String sdk_version = p_preset->get("build/sdk_version");
	if (sdk_version.is_empty()) {
		sdk_version = OPENHARMONY_DEFAULT_SDK_VERSION;
	}
	String bundle_id = p_preset->get("build/bundle_id");
	if (bundle_id.is_empty()) {
		bundle_id = OPENHARMONY_DEFAULT_BUNDLE_ID;
	}
	String app_name = GLOBAL_GET("application/config/name");
	if (app_name.is_empty()) {
		app_name = "template";
	}
	const Vector<String> orientations = String(OPENHARMONY_ORIENTATION_ENUMS).split(",");
	const int orientation = p_preset->get("build/default_orientation");
	ERR_FAIL_INDEX_V(orientation, orientations.size(), ERR_INVALID_PARAMETER);
	Dictionary host_options;
	host_options["sdk"] = sdk_version;
	host_options["compatible_api"] = p_preset->get("build/compatible_api");
	const bool managed_runtime = has_export_feature(p_preset, p_debug, "dotnet");
	host_options["dotnet"] = managed_runtime;
	host_options["dotnet_native_aot"] = has_export_feature(p_preset, p_debug, "dotnet_native_aot");
	if (managed_runtime) {
		Dictionary metadata;
		if (OpenHarmonyProjectConfig::read_document(project_dir.path_join("entry/src/main/resources/rawfile/godot_template.json"), metadata) != OK ||
				!bool(metadata.get("monoEnabled", false))) {
			add_message(EXPORT_MESSAGE_ERROR, TTR("Export .NET Project"), TTR("This C# project requires an OpenHarmony template built with module_mono_enabled=yes. Rebuild or install the current templates."));
			return ERR_UNCONFIGURED;
		}
		if (bool(host_options["dotnet_native_aot"]) && !bool(metadata.get("nativeAotEnabled", false))) {
			add_message(EXPORT_MESSAGE_ERROR, TTR("Export .NET Project"), TTR("This C# project requires current OpenHarmony NativeAOT-capable game templates. Rebuild or install the updated templates."));
			return ERR_UNCONFIGURED;
		}
	}
	host_options["bundle"] = bundle_id;
	host_options["name"] = app_name;
	host_options["version_code"] = p_preset->get("build/version_code");
	host_options["version_name"] = p_preset->get("build/version_name");
	host_options["arch"] = bool(p_preset->get("architectures/x86_64")) ? "x86_64" : "arm64-v8a";
	host_options["orientation"] = orientations[orientation];
	host_options["internet"] = bool(p_preset->get("permissions/ohos.permission.INTERNET")) || p_flags.has_flag(DEBUG_FLAG_REMOTE_DEBUG) || p_flags.has_flag(DEBUG_FLAG_DUMB_CLIENT);
	host_options["microphone"] = p_preset->get("permissions/ohos.permission.MICROPHONE");
	host_options["expand_safe_area"] = p_preset->get("build/expand_into_system_area");
	host_options["verbose"] = p_preset->get("build/verbose_diagnostics");
	err = OpenHarmonyProjectConfig::configure_game(project_dir, host_options);
	if (err != OK) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), TTR("Unable to configure the shared host. Use a current OpenHarmony game template and API 23 or newer."));
		return err;
	}

	String background_image = p_preset->get("build/background_image");
	if (!background_image.is_empty() && FileAccess::exists(background_image)) {
		String dest_bg_path = project_dir.path_join("entry/src/main/resources/base/media/background.png");
		da->make_dir_recursive(dest_bg_path.get_base_dir());
		err = da->copy(background_image, dest_bg_path);
		if (err != OK) {
			add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), vformat(TTR("Could not copy background image: \"%s\" to \"%s\"."), background_image, dest_bg_path));
			return err;
		}
		dest_bg_path = project_dir.path_join("AppScope/resources/base/media/background.png");
		da->make_dir_recursive(dest_bg_path.get_base_dir());
		err = da->copy(background_image, dest_bg_path);
		if (err != OK) {
			add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), vformat(TTR("Could not copy background image: \"%s\" to \"%s\"."), background_image, dest_bg_path));
			return err;
		}
	}

	String foreground_image = p_preset->get("build/foreground_image");
	if (!foreground_image.is_empty() && FileAccess::exists(foreground_image)) {
		String dest_fg_path = project_dir.path_join("entry/src/main/resources/base/media/foreground.png");
		da->make_dir_recursive(dest_fg_path.get_base_dir());
		err = da->copy(foreground_image, dest_fg_path);
		if (err != OK) {
			add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), vformat(TTR("Could not copy foreground image: \"%s\" to \"%s\"."), foreground_image, dest_fg_path));
			return err;
		}
		dest_fg_path = project_dir.path_join("AppScope/resources/base/media/foreground.png");
		da->make_dir_recursive(dest_fg_path.get_base_dir());
		err = da->copy(foreground_image, dest_fg_path);
		if (err != OK) {
			add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), vformat(TTR("Could not copy foreground image: \"%s\" to \"%s\"."), foreground_image, dest_fg_path));
			return err;
		}
	}

	if (ep.step(TTR("Saving project data..."), 4)) {
		return ERR_SKIP;
	}

	String pck_path = project_dir.path_join("entry/src/main/resources/rawfile/template.pck");
	Vector<SharedObject> shared_objects;
	err = save_pack(p_preset, p_debug, pck_path, &shared_objects);
	if (err == OK && get_worst_message_type() >= EXPORT_MESSAGE_ERROR) {
		err = ERR_COMPILATION_FAILED;
	}
	if (err != OK) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), TTR("Could not write package file."));
		return err;
	}

	for (const SharedObject &object : shared_objects) {
		const String target = object.target.simplify_path();
		if (target.is_absolute_path() || target.split("/").has("..") || target.contains(":")) {
			add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), TTR("A native dependency has an invalid target directory."));
			return ERR_FILE_BAD_PATH;
		}
		String directory = project_dir.path_join("entry/libs").path_join(host_options["arch"]).path_join(target);
		err = da->make_dir_recursive(directory);
		if (err == OK) {
			err = da->copy(ProjectSettings::get_singleton()->globalize_path(object.path), directory.path_join(object.path.get_file()));
		}
		if (err != OK) {
			add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), TTR("Could not include a native dependency in the exported application."));
			return err;
		}
	}
	print_line(vformat("Project exported pck successfully. %s", pck_path));

	if (export_project_only) {
		print_line("Project exported successfully. Build skipped as requested.");
		return OK;
	}

	if (ep.step(TTR("Building project..."), 5)) {
		return ERR_SKIP;
	}

	return build_project(p_preset, p_debug, project_dir, base_dir.path_join(p_path.get_file()), should_sign);
}

Error EditorExportPlatformOpenHarmony::build_project(const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_project, const String &p_output, bool p_sign) {
	const String node = get_node_path();
	const String hvigor = get_hvigor_path();
	String version = p_preset->get("build/sdk_version");
	if (version.is_empty()) {
		version = OPENHARMONY_DEFAULT_SDK_VERSION;
	}
	const String sdk = get_sdk_path(version);
	const String runner = p_project.path_join("tools/build.cjs");
	if (!FileAccess::exists(node) || !FileAccess::exists(hvigor) || !DirAccess::dir_exists_absolute(sdk) || !FileAccess::exists(runner)) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Build"), TTR("Install oo Node, the @oheco/hvigor adapter and matching SDK view, or configure their paths in Editor Settings. Rebuild outdated game templates."));
		return ERR_UNCONFIGURED;
	}
	// Secrets are never command-line arguments or files on HOME/hmdfs.
	const String work = EditorPaths::get_singleton()->get_temp_dir().path_join("openharmony-build-" + itos(OS::get_singleton()->get_process_id()) + "-" + uitos(OS::get_singleton()->get_ticks_usec()));
	Error err = DirAccess::make_dir_recursive_absolute(work);
	if (err != OK || FileAccess::set_unix_permissions(work, 0700) != OK || (FileAccess::get_unix_permissions(work) & 0777) != 0700) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Build"), TTR("A real private 0700 cache directory is required for the build. Do not place credentials on HOME/hmdfs."));
		_remove_dir_recursive(work);
		return ERR_CANT_CREATE;
	}
	Dictionary request;
	request["schemaVersion"] = 1;
	request["projectDir"] = p_project;
	request["sdkRoot"] = sdk;
	request["hvigorEntry"] = hvigor;
	request["buildMode"] = p_debug ? "debug" : "release";
	request["format"] = p_output.get_extension().to_lower();
	request["workDir"] = work;
	if (p_sign) {
		Dictionary signing;
		signing["certificate"] = p_preset->get("sign/certpath_file");
		signing["profile"] = p_preset->get("sign/profile_file");
		signing["storeFile"] = p_preset->get("sign/store_file");
		signing["keyAlias"] = p_preset->get("sign/key_alias");
		signing["signAlg"] = p_preset->get("sign/sign_alg");
		signing["keyPassword"] = p_preset->get("sign/key_password");
		signing["storePassword"] = p_preset->get("sign/store_password");
		request["signing"] = signing;
	}
	const String request_path = work.path_join("request.json");
	err = OpenHarmonyProjectConfig::write_document(request_path, request);
	if (err == OK) {
		err = FileAccess::set_unix_permissions(request_path, 0600);
		if ((FileAccess::get_unix_permissions(request_path) & 0777) != 0600) {
			err = ERR_CANT_CREATE;
		}
	}
	String output;
	int exit_code = -1;
	if (err == OK) {
		List<String> args{ runner, "--request", request_path };
		err = OS::get_singleton()->execute(node, args, &output, &exit_code, true, nullptr, false);
	}
	// Also redact defensively at the caller boundary, including execution errors.
	for (const char *field : { "sign/key_password", "sign/store_password" }) {
		String secret = p_preset->get(field);
		if (!secret.is_empty()) {
			output = output.replace(secret, "[REDACTED]");
		}
	}
	if (err == OK && exit_code == 0) {
		Dictionary result;
		err = OpenHarmonyProjectConfig::read_document(work.path_join("result.json"), result);
		String artifact = result.get("outputPath", "");
		if (err == OK && (int(result.get("schemaVersion", 0)) != 1 || result.get("mode", "") != request["buildMode"] || result.get("format", "") != request["format"] || bool(result.get("signed", !p_sign)) != p_sign || !artifact.is_absolute_path() || !artifact.begins_with(p_project + "/") || !FileAccess::exists(artifact))) {
			err = ERR_INVALID_DATA;
		}
		if (err == OK) {
			err = DirAccess::copy_absolute(artifact, p_output);
		}
	} else if (err == OK) {
		err = ERR_COMPILATION_FAILED;
	}
	_remove_dir_recursive(work);
	if (err != OK) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Build"), vformat(TTR("Native Hvigor export failed (exit %d):\n%s"), exit_code, output));
	} else {
		print_line("OpenHarmony bundle exported successfully: " + p_output);
	}
	return err;
}

void EditorExportPlatformOpenHarmony::_remove_dir_recursive(const String &p_dir) {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_valid()) {
		Error err = da->erase_contents_recursive();
		ERR_FAIL_COND_MSG(err != OK, "Could not remove directory: " + p_dir);
		err = DirAccess::remove_absolute(p_dir);
		ERR_FAIL_COND_MSG(err != OK, "Could not remove directory: " + p_dir);
	}
}

bool EditorExportPlatformOpenHarmony::use_broker() const {
#ifdef OPENHARMONY_ENABLED
	return bool(EDITOR_GET("export/openharmony/use_broker"));
#else
	return false;
#endif
}

String EditorExportPlatformOpenHarmony::get_broker_endpoint() const {
	// shell serve publishes this under HOME, independently of the tool root.
	String home = OS::get_singleton()->get_environment("HOME");
	if (home.is_empty()) {
		home = "/storage/Users/currentUser";
	}
	return home.path_join(".oheco/broker/endpoint");
}

Error EditorExportPlatformOpenHarmony::execute_tool(const String &p_command, const List<String> &p_arguments, String &r_output, int *r_exitcode, bool p_read_stderr, int p_timeout_ms, const String *p_broker_endpoint) {
	r_output = String();
	if (r_exitcode) {
		*r_exitcode = -1;
	}
#ifdef OPENHARMONY_ENABLED
	const String endpoint = p_broker_endpoint ? *p_broker_endpoint : (use_broker() ? get_broker_endpoint() : String());
	if (!endpoint.is_empty()) {
		std::vector<std::string> arguments;
		for (const String &argument : p_arguments) {
			const CharString encoded = argument.utf8();
			arguments.emplace_back(encoded.get_data(), encoded.length());
		}
		const CharString command = p_command.utf8();
		const auto result = OpenHarmonyBroker::execute(endpoint.utf8().get_data(), std::string(command.get_data(), command.length()), arguments, p_read_stderr, p_timeout_ms);
		r_output = String::utf8(result.output.data(), result.output.size());
		if (r_exitcode) {
			*r_exitcode = result.exit_code;
		}
		if (result.error != 0) {
			r_output += "\noheco broker: " + String::utf8(result.diagnostic.c_str());
			return ERR_CANT_CONNECT;
		}
		return OK;
	}
#endif
	return OS::get_singleton()->execute(p_command, p_arguments, &r_output, r_exitcode, p_read_stderr);
}

Error EditorExportPlatformOpenHarmony::execute_hdc(const String &p_hdc, const List<String> &p_arguments, String &r_output, const String *p_broker_endpoint) {
	int exit_code = -1;
	Error err = execute_tool(p_hdc, p_arguments, r_output, &exit_code, true, 120000, p_broker_endpoint);
	const String lower = r_output.to_lower();
	if (err == OK && (exit_code != 0 || lower.contains("[fail]") || lower.contains("[error]") || lower.contains("error:") || lower.contains("failed to ") || lower.contains("install failed") || lower.contains("ability failed") || lower.contains("process failed"))) {
		err = ERR_CANT_CONNECT;
	}
	return err;
}

void EditorExportPlatformOpenHarmony::clear_remote_run(bool p_stop) {
	if (!remote_run) {
		return;
	}
	// Debugger shutdown can emit a stopped signal while we are stopping. Detach
	// ownership before invoking HDC so that this callback cannot recurse.
	auto session = std::move(remote_run);
	std::string output;
	bool ok = p_stop ? session->stop(output) : session->finish(output);
	if (!ok) {
		add_message(EXPORT_MESSAGE_WARNING, TTR("Run"), "Remote cleanup is pending; it will be retried before the next deployment.\n" + String::utf8(output.c_str()));
		remote_run = std::move(session);
	}
}

void EditorExportPlatformOpenHarmony::stop_remote_run() {
	clear_remote_run(true);
}

void EditorExportPlatformOpenHarmony::remote_debugger_stopped() {
	// A lost debugger connection does not prove the device process exited.
	// Stop our bundle explicitly and retain ownership if HDC is unavailable.
	clear_remote_run(true);
}

Error EditorExportPlatformOpenHarmony::run(const Ref<EditorExportPreset> &p_preset, int p_device, BitField<EditorExportPlatform::DebugFlags> p_debug_flags) {
	String device_id;
	{
		MutexLock lock(device_lock);
		ERR_FAIL_INDEX_V(p_device, devices.size(), ERR_INVALID_PARAMETER);
		device_id = devices[p_device];
	}
	stop_remote_run();
	if (remote_run && remote_run->active()) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Run"), TTR("Previous deployment cleanup failed. Reconnect that device and stop it before deploying again."));
		return ERR_CANT_CONNECT;
	}
	String can_export_error;
	bool missing_templates = false;
	if (!can_export(p_preset, can_export_error, missing_templates, true)) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Run"), can_export_error);
		return ERR_UNCONFIGURED;
	}
	const String hdc = get_hdc_path();
	if (!FileAccess::exists(hdc)) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Run"), TTR("Install oo ohos-sdk-toolchains or configure the HDC path."));
		return ERR_FILE_NOT_FOUND;
	}
	std::vector<int> ports;
	if (p_debug_flags.has_flag(DEBUG_FLAG_REMOTE_DEBUG)) {
		ports.push_back(get_debug_port());
	}
	if (p_debug_flags.has_flag(DEBUG_FLAG_DUMB_CLIENT)) {
		ports.push_back(int(EDITOR_GET("filesystem/file_server/port")));
	}
	if (!ports.empty()) {
		p_debug_flags.set_flag(DEBUG_FLAG_REMOTE_DEBUG_LOCALHOST);
	}
	EditorProgress progress("run", vformat(TTR("Running on %s"), device_id), 2);
	if (progress.step(TTR("Exporting signed debug HAP..."), 0)) {
		return ERR_SKIP;
	}
	const String project_data = ProjectSettings::get_singleton()->globalize_path(ProjectSettings::get_singleton()->get_project_data_path());
	const String work = project_data.path_join("openharmony").path_join("run_" + itos(OS::get_singleton()->get_process_id()) + "_" + uitos(OS::get_singleton()->get_ticks_usec()));
	Error err = DirAccess::make_dir_recursive_absolute(work);
	if (err != OK) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Run"), vformat(TTR("Could not create the project deployment directory: %s"), work));
		return err;
	}
	const String temp = work.path_join("project.hap");
	err = export_project_helper(p_preset, true, temp, true, false, p_debug_flags);
	if (err == OK && !progress.step(TTR("Deploying and starting on device..."), 1)) {
		String bundle = p_preset->get("build/bundle_id");
		if (bundle.is_empty()) {
			bundle = OPENHARMONY_DEFAULT_BUNDLE_ID;
		}
		const String endpoint = use_broker() ? get_broker_endpoint() : String();
		remote_run = std::make_unique<DeviceRunSession>(device_id.utf8().get_data(), bundle.utf8().get_data(),
				[this, hdc, endpoint](const std::vector<std::string> &arguments, std::string &out) {
					List<String> args;
					for (const std::string &argument : arguments) {
						args.push_back(String::utf8(argument.c_str()));
					}
					String output;
					const Error result = execute_hdc(hdc, args, output, &endpoint);
					out = output.utf8().get_data();
					return result == OK;
				});
		std::string output;
		if (!remote_run->start(temp.utf8().get_data(), ports, output)) {
			err = ERR_CANT_CONNECT;
			add_message(EXPORT_MESSAGE_ERROR, TTR("Run"), String::utf8(output.c_str()));
		} else {
			print_line("OpenHarmony game started on " + device_id);
			if (p_debug_flags.has_flag(DEBUG_FLAG_REMOTE_DEBUG) && EditorDebuggerNode::get_singleton()) {
				ScriptEditorDebugger *debugger = EditorDebuggerNode::get_singleton()->get_current_debugger();
				const Callable callback = callable_mp(this, &EditorExportPlatformOpenHarmony::remote_debugger_stopped);
				if (debugger && !debugger->is_connected("stopped", callback)) {
					debugger->connect("stopped", callback);
				}
			}
		}
	} else if (err == OK) {
		err = ERR_SKIP;
	}
	_remove_dir_recursive(work);
	return err;
}

void EditorExportPlatformOpenHarmony::get_platform_features(List<String> *r_features) const {
	r_features->push_back("mobile");
	r_features->push_back("openharmony");
}

bool EditorExportPlatformOpenHarmony::has_valid_export_configuration(const Ref<EditorExportPreset> &p_preset, String &r_error, bool &r_missing_templates, bool p_debug) const {
	String err;
	bool valid = false;

	bool dvalid = false;
	bool rvalid = false;
	bool has_export_templates = false;

	if (p_preset->get("custom_template/debug") != "") {
		dvalid = FileAccess::exists(p_preset->get("custom_template/debug"));
		if (!dvalid) {
			err += TTR("Custom debug template not found.") + "\n";
		}
	} else {
		has_export_templates |= !find_game_template("openharmony_debug_arm64-v8a.zip", err).is_empty();
	}

	if (p_preset->get("custom_template/release") != "") {
		rvalid = FileAccess::exists(p_preset->get("custom_template/release"));
		if (!rvalid) {
			err += TTR("Custom release template not found.") + "\n";
		}
	} else {
		has_export_templates |= !find_game_template("openharmony_release_arm64-v8a.zip", err).is_empty();
	}

	r_missing_templates = !(dvalid || rvalid || has_export_templates);
	valid = dvalid || rvalid || has_export_templates;

	bool sign_enabled = p_preset->get("build/sign");
	if (sign_enabled) {
		String store_file = p_preset->get("sign/store_file");
		String store_password = p_preset->get("sign/store_password");
		String key_alias = p_preset->get("sign/key_alias");
		String key_password = p_preset->get("sign/key_password");
		String sign_alg = p_preset->get("sign/sign_alg");
		String profile_file = p_preset->get("sign/profile_file");
		String certpath_file = p_preset->get("sign/certpath_file");

		if (store_file.is_empty()) {
			valid = false;
			err += TTR("Store file path is required when signing is enabled.") + "\n";
		} else if (!FileAccess::exists(store_file)) {
			valid = false;
			err += TTR("Store file does not exist.") + "\n";
		}

		if (store_password.is_empty()) {
			valid = false;
			err += TTR("Store password is required when signing is enabled.") + "\n";
		}

		if (key_alias.is_empty()) {
			valid = false;
			err += TTR("Key alias is required when signing is enabled.") + "\n";
		}

		if (key_password.is_empty()) {
			valid = false;
			err += TTR("Key password is required when signing is enabled.") + "\n";
		}

		if (sign_alg.is_empty()) {
			valid = false;
			err += TTR("Sign algorithm is required when signing is enabled.") + "\n";
		}

		if (profile_file.is_empty()) {
			valid = false;
			err += TTR("Profile file path is required when signing is enabled.") + "\n";
		} else if (!FileAccess::exists(profile_file)) {
			valid = false;
			err += TTR("Profile file does not exist.") + "\n";
		}

		if (certpath_file.is_empty()) {
			valid = false;
			err += TTR("Certificate path file is required when signing is enabled.") + "\n";
		} else if (!FileAccess::exists(certpath_file)) {
			valid = false;
			err += TTR("Certificate path file does not exist.") + "\n";
		}
	}

	String background_image = p_preset->get("build/background_image");
	String foreground_image = p_preset->get("build/foreground_image");

	if (!background_image.is_empty()) {
		if (!FileAccess::exists(background_image)) {
			valid = false;
			err += TTR("Background image file does not exist.") + "\n";
		} else if (background_image.get_extension().to_lower() != "png") {
			valid = false;
			err += TTR("Background image must be a PNG file.") + "\n";
		} else {
			Ref<Image> img = Image::load_from_file(background_image);
			if (img.is_null()) {
				valid = false;
				err += TTR("Failed to load background image.") + "\n";
			} else if (img->get_width() != 1024 || img->get_height() != 1024) {
				valid = false;
				err += TTR("Background image must be 1024x1024 pixels.") + "\n";
			}
		}
	}

	if (!foreground_image.is_empty()) {
		if (!FileAccess::exists(foreground_image)) {
			valid = false;
			err += TTR("Foreground image file does not exist.") + "\n";
		} else if (foreground_image.get_extension().to_lower() != "png") {
			valid = false;
			err += TTR("Foreground image must be a PNG file.") + "\n";
		} else {
			Ref<Image> img = Image::load_from_file(foreground_image);
			if (img.is_null()) {
				valid = false;
				err += TTR("Failed to load foreground image.") + "\n";
			} else if (img->get_width() != 1024 || img->get_height() != 1024) {
				valid = false;
				err += TTR("Foreground image must be 1024x1024 pixels.") + "\n";
			}
		}
	}

	if (!bool(p_preset->get("build/export_project_only"))) {
		String version = p_preset->get("build/sdk_version");
		if (version.is_empty()) {
			version = OPENHARMONY_DEFAULT_SDK_VERSION;
		}
		if (!FileAccess::exists(get_node_path()) || !FileAccess::exists(get_hvigor_path()) || !DirAccess::dir_exists_absolute(get_sdk_path(version))) {
			valid = false;
			err += TTR("Install oo Node, @oheco/hvigor and a matching SDK view (oo sdk create), or configure explicit OpenHarmony export paths.") + "\n";
		}
	}

	if (!err.is_empty()) {
		r_error = err;
	}

	return valid;
}

bool EditorExportPlatformOpenHarmony::has_valid_project_configuration(const Ref<EditorExportPreset> &p_preset, String &r_error) const {
	String err;
	bool valid = true;

	// Validate preset options using our visibility and warning methods
	List<ExportOption> options;
	get_export_options(&options);
	for (const EditorExportPlatform::ExportOption &E : options) {
		if (get_export_option_visibility(p_preset.ptr(), E.option.name)) {
			String warn = get_export_option_warning(p_preset.ptr(), E.option.name);
			if (!warn.is_empty()) {
				err += warn + "\n";
				if (E.required) {
					valid = false;
				}
			}
		}
	}

	// Check if ETC2/ASTC texture compression is enabled (required for OpenHarmony)
	if (!ResourceImporterTextureSettings::should_import_etc2_astc()) {
		valid = false;
		err += TTR("ETC2/ASTC texture compression must be enabled for OpenHarmony export. Enable it in Project Settings (Rendering > Textures > VRAM Compression > Import ETC2 ASTC).") + "\n";
	}

	// Check if Vulkan renderer is being used (required for OpenHarmony)
	String rendering_method = GLOBAL_GET("rendering/renderer/rendering_method.mobile");
	String rendering_driver = GLOBAL_GET("rendering/rendering_device/driver.openharmony");

	bool uses_vulkan = rendering_driver == "vulkan" && (rendering_method == "forward_plus" || rendering_method == "mobile");
	if (!uses_vulkan) {
		valid = false;
		err += TTR("OpenHarmony export requires Vulkan renderer. Set rendering method to 'Forward+' or 'Mobile' and rendering driver to 'Vulkan' in Project Settings.") + "\n";
	}

	if (!err.is_empty()) {
		r_error = err;
	}

	return valid;
}

String EditorExportPlatformOpenHarmony::find_game_template(const String &p_filename, String &r_error) const {
	const String files = OS::get_singleton()->get_environment("GODOT_OHOS_DATA_DIR");
	const String root = files.path_join("export_templates");
	const String pointer = root.path_join("current.json");
	if (files.is_absolute_path() && FileAccess::exists(pointer)) {
		Ref<FileAccess> file = FileAccess::open(pointer, FileAccess::READ);
		if (file.is_valid() && file->get_length() <= 4096) {
			Variant parsed = JSON::parse_string(file->get_as_text());
			if (parsed.get_type() == Variant::DICTIONARY) {
				Dictionary record = parsed;
				const String hash = record.get("sha256", "");
				if (int(record.get("schemaVersion", 0)) == 1 && hash.length() == 64 && hash.is_valid_hex_number(false) && hash == hash.to_lower()) {
					String directory = root.path_join(hash);
					String template_path = directory.path_join(p_filename);
					if (FileAccess::exists(directory.path_join(".ready")) && FileAccess::exists(template_path)) {
						return template_path;
					}
				}
			}
		}
	}
	return find_export_template(p_filename, &r_error);
}

int EditorExportPlatformOpenHarmony::get_debug_port() const {
	EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
	const String uri = debugger ? debugger->get_server_uri() : String();
	const int separator = uri.rfind(":");
	if (uri.begins_with("tcp://") && separator >= 0) {
		const String value = uri.substr(separator + 1);
		if (value.is_valid_int() && value.to_int() > 0 && value.to_int() <= 65535) {
			return value.to_int();
		}
	}
	return int(EDITOR_GET("network/debug/remote_port"));
}

String EditorExportPlatformOpenHarmony::get_node_path() const {
	return OpenHarmonyToolchain::node(EDITOR_GET("export/openharmony/node_path"));
}

String EditorExportPlatformOpenHarmony::get_sdk_path(const String &p_version) const {
	return OpenHarmonyToolchain::sdk(EDITOR_GET("export/openharmony/sdk_root"), p_version);
}

String EditorExportPlatformOpenHarmony::get_hvigor_path() const {
	return OpenHarmonyToolchain::hvigor(EDITOR_GET("export/openharmony/hvigor_entry"), get_node_path());
}

String EditorExportPlatformOpenHarmony::get_hdc_path() const {
	return OpenHarmonyToolchain::hdc(EDITOR_GET("export/openharmony/hdc_path"), get_sdk_path());
}

EditorExportPlatformOpenHarmony::EditorExportPlatformOpenHarmony() {
	if (EditorNode::get_singleton()) {
		Ref<Image> img;
		img.instantiate();
		const bool upsample = !Math::is_equal_approx(Math::round(EDSCALE), EDSCALE);

		ImageLoaderSVG::create_image_from_string(img, _openharmony_logo_svg, EDSCALE, upsample, false);
		logo = ImageTexture::create_from_image(img);

		ImageLoaderSVG::create_image_from_string(img, _openharmony_run_icon_svg, EDSCALE, upsample, false);
		run_icon = ImageTexture::create_from_image(img);

		devices_changed.set();
		_update_preset_status();
		check_for_changes_thread.start(_check_for_changes_poll_thread, this);
	}
}

void EditorExportPlatformOpenHarmony::_check_for_changes_poll_thread(void *p_ud) {
	EditorExportPlatformOpenHarmony *ea = static_cast<EditorExportPlatformOpenHarmony *>(p_ud);

	while (!ea->quit_request.is_set()) {
		String hdc = ea->get_hdc_path();
		if (ea->has_runnable_preset.is_set() && FileAccess::exists(hdc) && EditorNode::get_singleton()->is_editor_ready()) {
			String devices_output;
			List<String> args{ "list", "targets" };
			int ec = -1;
			Error error = ea->execute_tool(hdc, args, devices_output, &ec, false, 10000);
			if (error != OK || ec != 0) {
				devices_output = "";
			}

			Vector<String> ds = devices_output.split("\n");
			Vector<String> ldevices;

			for (int i = 0; i < ds.size(); i++) {
				String d = ds[i].strip_edges();
				if (d.is_empty() || d == "[Empty]") {
					continue;
				}
				ldevices.push_back(d);
			}

			MutexLock lock(ea->device_lock);

			bool different = false;

			if (ea->devices.size() != ldevices.size()) {
				different = true;
			} else {
				for (int i = 0; i < ea->devices.size(); i++) {
					if (ea->devices[i] != ldevices[i]) {
						different = true;
						break;
					}
				}
			}

			if (different) {
				ea->devices = ldevices;
				ea->devices_changed.set();
			}
		}

		uint64_t sleep = 200;
		uint64_t wait = 3000000;
		uint64_t time = OS::get_singleton()->get_ticks_usec();
		while (OS::get_singleton()->get_ticks_usec() - time < wait) {
			OS::get_singleton()->delay_usec(1000 * sleep);
			if (ea->quit_request.is_set()) {
				break;
			}
		}
	}
}

void EditorExportPlatformOpenHarmony::_update_preset_status() {
	const int preset_count = EditorExport::get_singleton()->get_export_preset_count();
	bool has_runnable = false;

	for (int i = 0; i < preset_count; i++) {
		const Ref<EditorExportPreset> &preset = EditorExport::get_singleton()->get_export_preset(i);
		if (preset->get_platform() == this && preset->is_runnable()) {
			has_runnable = true;
			break;
		}
	}

	if (has_runnable) {
		has_runnable_preset.set();
	} else {
		has_runnable_preset.clear();
	}
	devices_changed.set();
}

void EditorExportPlatformOpenHarmony::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_POSTINITIALIZE: {
			if (EditorExport::get_singleton()) {
				EditorExport::get_singleton()->connect_presets_runnable_updated(callable_mp(this, &EditorExportPlatformOpenHarmony::_update_preset_status));
			}
		} break;
	}
}

EditorExportPlatformOpenHarmony::~EditorExportPlatformOpenHarmony() {
	stop_remote_run();
	quit_request.set();
	if (check_for_changes_thread.is_started()) {
		check_for_changes_thread.wait_to_finish();
	}
}
