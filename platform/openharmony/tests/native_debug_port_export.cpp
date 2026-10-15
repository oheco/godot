// Godot Engine contributors. SPDX-License-Identifier: MIT
// Link to the real Editor library: no exporter/debugger implementation copies.
// clang-format off
#include <native_window/external_window.h>
// clang-format on
#include "core/io/file_access.h"
#include "core/io/tcp_server.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/export/editor_export.h"
#include "editor/export/editor_export_preset.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/settings/editor_settings.h"
#include "main/main.h"
#include "platform/openharmony/export/export_plugin.h"
#include "platform/openharmony/os_openharmony.h"

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
int failures = 0;
bool check(bool p_ok, const char *p_message) {
	std::printf("%s %s\n", p_ok ? "PASS" : "FAIL", p_message);
	failures += !p_ok;
	return p_ok;
}

// Older templates used little-endian count/length-prefixed strings; current
// exporters write one UTF-8 argument per line. Validate either actual artifact.
bool read_arguments(const String &p_path, std::vector<std::string> &r_arguments) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null() || file->get_length() < 4 || file->get_length() > 65536) {
		return false;
	}
	const uint64_t size = file->get_length();
	if (file->get_8() == '-') {
		file->seek(0);
		const String text = file->get_as_text();
		if (!text.ends_with("\n")) {
			return false;
		}
		for (const String &argument : text.trim_suffix("\n").split("\n")) {
			r_arguments.emplace_back(argument.utf8().get_data());
		}
		std::printf("CL_FORMAT utf8-lines\n");
		return true;
	}
	file->seek(0);
	file->set_big_endian(false);
	const uint32_t count = file->get_32();
	if (count == 0 || count > 128) {
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		if (size - file->get_position() < 4) {
			return false;
		}
		const uint32_t length = file->get_32();
		if (length > size - file->get_position()) {
			return false;
		}
		std::string argument(length, '\0');
		if (file->get_buffer(reinterpret_cast<uint8_t *>(argument.data()), length) != length || argument.find('\0') != std::string::npos) {
			return false;
		}
		r_arguments.push_back(argument);
	}
	std::printf("CL_FORMAT binary-count-length\n");
	return file->get_position() == size;
}

bool argument_equals(const std::vector<std::string> &p_arguments, const std::string &p_flag, const std::string &p_value) {
	unsigned found = 0;
	for (size_t i = 0; i + 1 < p_arguments.size(); i++) {
		if (p_arguments[i] == p_flag) {
			if (p_arguments[++i] != p_value) {
				return false;
			}
			found++;
		}
	}
	return found == 1;
}

void audit_export(EditorExportPlatformOpenHarmony *p_platform, const Ref<EditorExportPreset> &p_preset, const String &p_output, int p_debug_port, int p_file_port, bool p_expect_bug) {
	const std::string target = "native-debug-port-fixture";
	std::vector<std::vector<std::string>> calls;
	std::vector<DeviceRunSession::PortMapping> created;
	std::vector<DeviceRunSession::PortMapping> removed;
	DeviceRunSession session(target, "org.godotengine.debugportfixture", [&](const std::vector<std::string> &p_args, std::string &r_output) {
		calls.push_back(p_args);
		if (p_args.size() < 4 || p_args[0] != "-t" || p_args[1] != target) {
			check(false, "fake executor received selected target");
			return false;
		}
		if (p_args == std::vector<std::string>{ "-t", target, "fport", "ls" }) {
			r_output = "Empty\n";
			return true;
		}
		const auto parse_endpoint = [](const std::string &p_text) {
			return p_text.compare(0, 4, "tcp:") == 0 ? std::atoi(p_text.c_str() + 4) : 0;
		};
		if (p_args.size() == 5 && p_args[2] == "rport") {
			created.push_back({ parse_endpoint(p_args[3]), parse_endpoint(p_args[4]) });
			r_output = "Forwardport result:OK\n";
			return true;
		}
		if (p_args.size() == 6 && p_args[2] == "fport" && p_args[3] == "rm") {
			const DeviceRunSession::PortMapping mapping{ parse_endpoint(p_args[4]), parse_endpoint(p_args[5]) };
			if (std::find(created.begin(), created.end(), mapping) != created.end() && std::find(removed.begin(), removed.end(), mapping) == removed.end()) {
				removed.push_back(mapping);
				r_output = "Remove forward ruler success\n";
				return true;
			}
		}
		check(false, "executor rejects unexpected HDC command or unowned removal");
		return false;
	});
	std::string output;
	if (!check(session.prepare_ports({ p_debug_port, p_file_port }, output), "prepare real DeviceRunSession with actual debugger and file-server host ports")) {
		session.finish(output);
		return;
	}
	const auto mappings = session.mappings();
	check(mappings.size() == 2 && session.owned_mappings() == mappings && mappings == created, "two confirmed reverse mappings are owned");
	if (mappings.size() != 2) {
		session.finish(output);
		return;
	}
	check(mappings[0].host_port == p_debug_port && mappings[1].host_port == p_file_port && mappings[0].device_port != mappings[1].device_port &&
			mappings[0].device_port != p_debug_port && mappings[0].device_port != p_file_port && mappings[1].device_port != p_debug_port && mappings[1].device_port != p_file_port,
			"device endpoints differ from both live host listeners");
	std::printf("MAPPINGS debug=%d->%d files=%d->%d\n", mappings[0].device_port, p_debug_port, mappings[1].device_port, p_file_port);

	const BitField<EditorExportPlatform::DebugFlags> flags = EditorExportPlatform::DEBUG_FLAG_REMOTE_DEBUG | EditorExportPlatform::DEBUG_FLAG_DUMB_CLIENT | EditorExportPlatform::DEBUG_FLAG_REMOTE_DEBUG_LOCALHOST;
	p_platform->clear_messages();
	const Error result = p_platform->export_project_helper(p_preset, true, p_output, false, true, flags, true, &session);
	std::printf("EXPORT_RESULT %d expected=%d\n", int(result), int(p_expect_bug ? ERR_INVALID_DATA : OK));
	bool mismatch_message = false;
	for (int i = 0; i < p_platform->get_message_count(); i++) {
		const String message = p_platform->get_message(i).text;
		std::printf("EXPORT_MESSAGE %s\n", message.utf8().get_data());
		mismatch_message |= message.contains("device debug port mappings do not match");
	}
	const String cl_path = p_output.get_basename().path_join("entry/src/main/resources/rawfile/_cl_");
	if (p_expect_bug) {
		check(result == ERR_INVALID_DATA && mismatch_message, "reproduced original exporter mismatch rejection after debugger fallback");
		check(!FileAccess::exists(cl_path), "original exporter rejects stale configured debug port before writing _cl_");
	} else if (check(result == OK, "real export_project_helper completes project-only export")) {
		std::vector<std::string> arguments;
		if (check(read_arguments(cl_path, arguments), "decode actual generated _cl_ artifact")) {
			for (const std::string &argument : arguments) {
				std::printf("CL_ARGUMENT %s\n", argument.c_str());
			}
			check(argument_equals(arguments, "--remote-debug", "tcp://127.0.0.1:" + std::to_string(mappings[0].device_port)), "_cl_ debugger endpoint uses mapped device port and actual tcp protocol");
			check(argument_equals(arguments, "--remote-fs", "127.0.0.1:" + std::to_string(mappings[1].device_port)), "_cl_ file-server endpoint uses its own mapped device port");
			check(argument_equals(arguments, "--remote-fs-password", "fixture password tcp://unchanged:49000"), "unrelated file-server password survives export unchanged");
		}
		check(FileAccess::exists(p_output.get_basename().path_join("entry/src/main/resources/rawfile/template.pck")), "real project-only export writes project pack");
	}
	check(session.mappings() == mappings && session.owned_mappings() == created, "export retains mapping reservations");
	check(session.finish(output), "finish cleans owned reverse rules without install or ability commands");
	std::vector<DeviceRunSession::PortMapping> reversed(created.rbegin(), created.rend());
	check(removed == reversed && calls.size() == 5 && !session.active() && session.owned_mappings().empty(), "cleanup removes exactly the two owned endpoint pairs in reverse order");
	const size_t previous_calls = calls.size();
	check(session.finish(output) && calls.size() == previous_calls, "repeated cleanup performs no extra HDC commands");
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
	EditorSettings *settings = EditorSettings::get_singleton();
	EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
	EditorExport *exports = EditorExport::get_singleton();
	if (!check(settings && debugger && exports, "Main::start initialized real Editor settings, debugger and export registry")) {
		return;
	}
	const int platform_index = exports->get_export_platform_index_by_name("OpenHarmony");
	if (!check(platform_index >= 0, "OpenHarmony export platform is registered")) {
		return;
	}
	Ref<EditorExportPlatform> platform_ref = exports->get_export_platform(platform_index);
	auto *platform = Object::cast_to<EditorExportPlatformOpenHarmony>(platform_ref.ptr());
	if (!check(platform != nullptr, "registered platform is real EditorExportPlatformOpenHarmony")) {
		return;
	}
	Ref<TCPServer> occupied;
	occupied.instantiate();
	if (!check(occupied->listen(0, IPAddress("127.0.0.1")) == OK, "occupy configured debugger port with real TCP listener")) {
		return;
	}
	const int configured_port = occupied->get_local_port();
	if (!check(configured_port > 0 && configured_port < 65531, "ephemeral configured debugger port allows fallback")) {
		return;
	}
	settings->set_setting("network/debug/remote_host", "127.0.0.1");
	settings->set_setting("network/debug/remote_port", configured_port);
	if (!check(debugger->start("tcp://") == OK, "real debugger starts while configured port is occupied")) {
		return;
	}
	const String uri = debugger->get_server_uri();
	String scheme, host, path, fragment;
	int actual_port = 0;
	if (!check(uri.parse_url(scheme, host, actual_port, path, fragment) == OK && uri.begins_with("tcp://127.0.0.1:") && actual_port > 0 && actual_port != configured_port,
			"debugger URI proves actual listener differs from configured port")) {
		return;
	}
	Ref<TCPServer> file_server;
	file_server.instantiate();
	if (!check(file_server->listen(0, IPAddress("127.0.0.1")) == OK, "reserve distinct real file-server host listener")) {
		return;
	}
	const int file_port = file_server->get_local_port();
	settings->set_setting("filesystem/file_server/port", file_port);
	settings->set_setting("filesystem/file_server/password", "fixture password tcp://unchanged:49000");
	const String template_path = OS::get_singleton()->get_environment("GODOT_DEBUG_PORT_TEMPLATE");
	const String output_root = OS::get_singleton()->get_environment("GODOT_DEBUG_PORT_OUTPUT");
	if (!check(!template_path.is_empty() && FileAccess::exists(template_path) && output_root.is_absolute_path(), "runner supplies isolated output and existing game template")) {
		return;
	}
	Ref<EditorExportPreset> preset = platform->create_preset();
	preset->set("custom_template/debug", template_path);
	preset->set("build/export_project_only", true);
	preset->set("build/sign", false);
	preset->set("build/bundle_id", "org.godotengine.debugportfixture");

	// Positive control must succeed even on the old library, proving that the
	// template, project packing, session layout and _cl_ assertions are usable.
	settings->set_setting("network/debug/remote_port", actual_port);
	audit_export(platform, preset, output_root.path_join("control.hap"), actual_port, file_port, false);
	check(int(settings->get_setting("network/debug/remote_port")) == actual_port, "control export preserves its host-port setting");
	settings->set_setting("network/debug/remote_port", configured_port);
	std::printf("DEBUGGER configured=%d actual=%d uri=%s\n", configured_port, actual_port, uri.utf8().get_data());
#ifdef GODOT_EXPECT_ORIGINAL_DEBUG_PORT_BUG
	audit_export(platform, preset, output_root.path_join("fallback.hap"), actual_port, file_port, true);
#else
	audit_export(platform, preset, output_root.path_join("fallback.hap"), actual_port, file_port, false);
#endif
	check(String(settings->get_setting("network/debug/remote_host")) == "127.0.0.1" && int(settings->get_setting("network/debug/remote_port")) == configured_port &&
			int(settings->get_setting("filesystem/file_server/port")) == file_port && String(settings->get_setting("filesystem/file_server/password")) == "fixture password tcp://unchanged:49000",
			"fallback export preserves all configured host values");
	check(debugger->get_server_uri() == uri && occupied->is_listening() && file_server->is_listening(), "export and session cleanup preserve live host listeners");
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
	if (EditorDebuggerNode::get_singleton()) {
		EditorDebuggerNode::get_singleton()->stop(true);
	}
	os.main_loop_end();
	Main::cleanup();
	std::printf("DEBUG_PORT_EXPORT_%s failures=%d\n", failures ? "FAIL" : "PASS", failures);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
