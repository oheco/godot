// Godot Engine contributors. SPDX-License-Identifier: MIT
// Real EditorNode/EditorLog and real OS_OpenHarmony::create_instance. The launch
// callback controls early-reply races, then execs this signed fixture as a real
// game process to exercise the production fatal-signal handler end to end.
// clang-format off
#include <native_window/external_window.h>
// clang-format on
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "main/main.h"
#include "scene/gui/rich_text_label.h"

#include "platform/openharmony/crash_handler_openharmony.h"
#include "platform/openharmony/engine_host_openharmony.h"
#include "platform/openharmony/os_openharmony.h"

#include <spawn.h>
#include <sys/wait.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern char **environ;
namespace {
String executable;
String custom_log;
int failures = 0;
int fake_pid = 0;
int launches = 0;
bool split_crash = false;
bool large_context = false;
enum Mode { EARLY,
	OLD_ONLY,
	EMPTY,
	FAILURE,
	REAL_CRASH,
	NORMAL } mode = EARLY;
const char *end_marker = "\n=== end of OpenHarmony crash backtrace ===\n";

void check(bool p_ok, const char *p_message) {
	std::printf("%s %s\n", p_ok ? "PASS" : "FAIL", p_message);
	failures += !p_ok;
}
void write(const String &p_path, const String &p_text, bool p_append = false) {
	Ref<FileAccess> file = FileAccess::open(p_path, p_append ? FileAccess::READ_WRITE : FileAccess::WRITE);
	if (file.is_valid()) {
		if (p_append) {
			file->seek_end();
		}
		file->store_string(p_text);
	}
}
String output(Node *p_node) {
	String result;
	if (RichTextLabel *label = Object::cast_to<RichTextLabel>(p_node)) {
		result += label->get_parsed_text();
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		result += output(p_node->get_child(i));
	}
	return result;
}
String output() {
	return output(EditorNode::get_log());
}
int reports() {
	return output().count("The editor is still running.");
}
void pump(OS_OpenHarmony &p_os, int p_frames = 40) {
	for (int i = 0; i < p_frames; i++) {
		p_os.delay_usec(10000);
		p_os.main_loop_iterate();
	}
}
String log_path(int p_argc, const char *const *p_argv) {
	for (int i = 0; i + 1 < p_argc; i++) {
		if (std::strcmp(p_argv[i], "--log-file") == 0) {
			return String::utf8(p_argv[i + 1]);
		}
	}
	return OS::get_singleton()->get_data_path().path_join("untracked.log");
}
int32_t spawn(int p_argc, const char *const *p_argv) {
	launches++;
	if (mode == FAILURE) {
		return -1;
	}
	const String engine = log_path(p_argc, p_argv);
	if (mode == REAL_CRASH || mode == NORMAL) {
		std::vector<std::string> arguments = { executable.utf8().get_data(), "--fixture-child", mode == REAL_CRASH ? "crash" : "normal" };
		for (int i = 0; i < p_argc; i++) {
			arguments.push_back(p_argv[i]);
		}
		std::vector<char *> argv;
		for (std::string &argument : arguments) {
			argv.push_back(argument.data());
		}
		argv.push_back(nullptr);
		pid_t pid = 0;
		return posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ) == 0 ? pid : -1;
	}
	const String crash = engine.get_base_dir().path_join(vformat("godot-%d-crash.log", fake_pid));
	if (mode == EARLY) {
		write(engine, large_context ? "DROP_LARGE_CONTEXT_PREFIX\n" + String("padding \u4e2d\u6587 context\n").repeat(10000) + "BOUNDED_FINAL_UTF8_\u4e2d\u6587\n" : "CURRENT RUN: final resource load error before fatal signal\n");
		if (!FileAccess::exists(crash)) {
			write(crash, String());
		}
		write(crash, "=== Godot (OpenHarmony) fatal signal === signal=11\nEARLY RACE" + (split_crash ? String() : String(end_marker)), true);
	} else if (mode == EMPTY) {
		write(engine, "NORMAL RUN\n");
		write(crash, String());
	}
	return fake_pid;
}
List<String> arguments(const String &p_editor_pid = String(), bool p_custom = true) {
	List<String> result;
	result.push_back("--headless");
	result.push_back("--path");
	result.push_back(OS::get_singleton()->get_resource_dir());
	result.push_back("--editor-pid");
	result.push_back(p_editor_pid.is_empty() ? itos(OS::get_singleton()->get_process_id()) : p_editor_pid);
	if (p_custom) {
		result.push_back("--log-file");
		result.push_back(custom_log);
	}
	return result;
}
void synthetic(OS_OpenHarmony &p_os) {
	const String crash = custom_log.get_base_dir().path_join(vformat("godot-%d-crash.log", fake_pid));
	write(crash, "HISTORICAL PID CRASH" + String(end_marker));
	write(custom_log, String("OLD ENGINE CONTEXT\n").repeat(100));
	mode = OLD_ONLY;
	ProcessID pid = 0;
	check(p_os.create_instance(arguments(), &pid) == OK && pid == fake_pid, "attach a previously used PID through the real OS boundary");
	pump(p_os);
	check(reports() == 0, "old nonempty crash in the same second is not a new crash");
	mode = EARLY;
	check(p_os.create_instance(arguments(), &pid) == OK, "new run returns after its crash log has already been appended");
	pump(p_os);
	check(reports() == 1, "crash before launcher reply reaches the real EditorLog once");
	check(output().contains("CURRENT RUN") && output().contains("EARLY RACE"), "Output includes the last engine error and native backtrace");
	check(!output().contains("HISTORICAL PID CRASH") && !output().contains("OLD ENGINE CONTEXT"), "report excludes reused-PID crash prefix and old longer engine output");
	pump(p_os);
	check(reports() == 1, "repeated polls do not replay a consumed crash");
	check(p_os.create_instance(arguments(), &pid) == OK, "same PID can be registered for a later run");
	pump(p_os);
	check(reports() == 2, "a later run reports a new crash from the reused PID");
	mode = EMPTY;
	check(p_os.create_instance(arguments(), &pid) == OK, "normal game creates an empty crash log");
	pump(p_os, 140);
	check(reports() == 2, "empty crash and ordinary exit do not create false reports");
	write(crash, "LATE HISTORY" + String(end_marker), true);
	pump(p_os);
	check(reports() == 2, "ordinary exited tracker is removed after its final file checks");
	mode = EARLY;
	p_os.create_instance(arguments(itos(p_os.get_process_id() + 1)), &pid);
	pump(p_os);
	check(reports() == 2, "foreign editor PID is not monitored");
	List<String> user_arguments;
	user_arguments.push_back("--");
	user_arguments.push_back("--editor-pid");
	user_arguments.push_back(itos(p_os.get_process_id()));
	p_os.create_instance(user_arguments, &pid);
	pump(p_os);
	check(reports() == 2, "game user arguments cannot claim an editor attachment");
	mode = FAILURE;
	check(p_os.create_instance(arguments(), &pid) != OK, "failed launch is returned as an error");
	pump(p_os);
	check(reports() == 2, "failed launch is not reported as a native crash");
	mode = EARLY;
	fake_pid = p_os.get_process_id(); // A live PID keeps the split writer alive.
	split_crash = true;
	large_context = true;
	p_os.create_instance(arguments(), &pid);
	pump(p_os, 20);
	check(reports() == 2, "partial fatal report waits for the live writer to finish");
	write(custom_log.get_base_dir().path_join(vformat("godot-%d-crash.log", fake_pid)), end_marker, true);
	pump(p_os);
	check(reports() == 3, "completed split crash is published exactly once");
	check(output().contains("BOUNDED_FINAL_UTF8_\u4e2d\u6587") && !output().contains("DROP_LARGE_CONTEXT_PREFIX"), "large UTF-8 engine output is limited to the final 64 KiB");
	split_crash = false;
	large_context = false;
}
int child(int p_argc, char **p_argv) {
	const bool crash = std::strcmp(p_argv[2], "crash") == 0;
	OS_OpenHarmony os(false);
	const String engine = log_path(p_argc - 3, p_argv + 3);
	const String path = engine.get_base_dir().path_join(vformat("godot-%d-crash.log", os.get_process_id()));
	ohos_crash_handler_install(path.utf8().get_data());
	if (Main::setup(p_argv[0], p_argc - 3, p_argv + 3) != OK || Main::start() != EXIT_SUCCESS) {
		return 80;
	}
	os.main_loop_begin();
	pump(os, 4);
	print_error("REAL_GAME_LAST_ERROR_\u4e2d\u6587");
	if (crash) {
		::raise(SIGSEGV);
		return 81;
	}
	os.main_loop_end();
	Main::cleanup();
	return 0;
}
} // namespace

int main(int argc, char **argv) {
	if (argc > 2 && std::strcmp(argv[1], "--fixture-child") == 0) {
		return child(argc, argv);
	}
	executable = String::utf8(argv[0]);
	OS_OpenHarmony os(true); // false would bypass UIAbility create_instance.
	if (Main::setup(argv[0], argc - 1, argv + 1) != OK || Main::start() != EXIT_SUCCESS) {
		return 70;
	}
	os.main_loop_begin();
	pump(os, 4);
	EditorNode *editor = EditorNode::get_singleton();
	check(editor && EditorNode::get_log(), "real EditorNode and Output dock are initialized");
	fake_pid = os.get_process_id() + 10000000;
	custom_log = os.get_data_path().path_join("custom logs \u4e2d\u6587/engine with spaces.log");
	check(DirAccess::make_dir_recursive_absolute(custom_log.get_base_dir()) == OK, "create a custom log directory separate from the default data directory");
	godot_host_set_create_instance_callback(spawn);
	synthetic(os);
	const int before = reports();
	for (Mode next : { REAL_CRASH, NORMAL, REAL_CRASH }) {
		mode = next;
		ProcessID pid = 0;
		check(os.create_instance(arguments(String(), false), &pid) == OK, "launch an actual signed game subprocess from the same editor");
		int status = 0;
		bool exited = false;
		for (int i = 0; i < 500; i++) {
			pump(os, 1);
			if (::waitpid(pid, &status, WNOHANG) == pid) {
				exited = true;
				break;
			}
		}
		if (!exited && pid > 0) {
			::kill(pid, SIGKILL);
			::waitpid(pid, &status, 0);
		}
		check(exited && (next == NORMAL ? WIFEXITED(status) && WEXITSTATUS(status) == 0 : WIFSIGNALED(status)), "actual game terminates with the expected normal or fatal status");
		pump(os);
		check(EditorNode::get_singleton() == editor && os.get_main_loop(), "the same parent editor remains usable after the child exits");
	}
	check(reports() == before + 2, "two real native crashes reported, intervening normal run stayed clean");
	check(output().contains("REAL_GAME_LAST_ERROR_\u4e2d\u6587") && output().contains("signal=11"), "real crash report and final UTF-8 game error reach Output");
	godot_host_set_create_instance_callback(nullptr);
	std::printf("%s launches=%d failures=%d\n", failures ? "CHILD_CRASH_FAIL" : "CHILD_CRASH_PASS", launches, failures);
	os.main_loop_end();
	Main::cleanup();
	return failures ? 1 : 0;
}
