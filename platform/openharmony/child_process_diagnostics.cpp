// Godot Engine contributors. SPDX-License-Identifier: MIT
#include "child_process_diagnostics.h"

#ifdef TOOLS_ENABLED
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"

#include <signal.h>
#include <sys/stat.h>

#include <cerrno>

OpenHarmonyChildProcessDiagnostics::Snapshot OpenHarmonyChildProcessDiagnostics::snapshot(const String &p_path) {
	struct stat info {};
	Snapshot result;
	if (::stat(p_path.utf8().get_data(), &info) == 0 && S_ISREG(info.st_mode)) {
		result.exists = true;
		result.inode = info.st_ino;
		result.size = info.st_size;
		result.modified = uint64_t(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec;
	}
	return result;
}

String OpenHarmonyChildProcessDiagnostics::read_tail(const String &p_path, uint64_t p_start) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return "(log unavailable)";
	}
	const uint64_t length = file->get_length();
	const uint64_t offset = MAX(p_start, length > 65536 ? length - 65536 : 0);
	if (offset >= length) {
		return String();
	}
	file->seek(offset);
	Vector<uint8_t> bytes;
	bytes.resize(length - offset);
	const uint64_t count = file->get_buffer(bytes.ptrw(), bytes.size());
	// A bounded tail may start in the middle of a UTF-8 character or log line.
	uint64_t begin = 0;
	if (offset > p_start) {
		while (begin < count && bytes[begin++] != '\n') {
		}
	}
	return String::utf8(reinterpret_cast<const char *>(bytes.ptr()) + begin, count - begin);
}

OpenHarmonyChildProcessDiagnostics::Launch OpenHarmonyChildProcessDiagnostics::prepare(List<String> &r_arguments, const String &p_data_directory) {
	Launch launch;
	String log_path;
	int64_t editor_pid = 0;
	bool editor_instance = false;
	for (const List<String>::Element *argument = r_arguments.front(); argument; argument = argument->next()) {
		const String &value = argument->get();
		if (value == "--" || value == "++") {
			break;
		}
		if ((value == "--editor-pid" || value == "--log-file") && argument->next()) {
			argument = argument->next();
			if (value == "--editor-pid") {
				editor_pid = argument->get().to_int();
			} else {
				log_path = argument->get();
			}
		} else if (value == "--editor" || value == "-e" || value == "--project-manager" || value == "-p") {
			editor_instance = true;
		}
	}
	if (editor_instance || editor_pid != OS::get_singleton()->get_process_id()) {
		return launch;
	}
	launch.enabled = true;
	{
		MutexLock lock(mutex);
		launch.generation = ++generation;
	}
	if (log_path.is_empty()) {
		// Unique engine output avoids presenting a reused PID's old context when
		// a new game fails before its logger is initialized. Crash files retain
		// the established per-PID layout used by the ArkUI fallback diagnostics.
		log_path = p_data_directory.path_join(vformat("godot-run-%d-%d-%d-engine.log", editor_pid, OS::get_singleton()->get_ticks_usec(), launch.generation));
	} else {
		log_path = ProjectSettings::get_singleton()->globalize_path(log_path);
		if (!log_path.is_absolute_path()) {
			log_path = DirAccess::get_full_path(log_path, DirAccess::ACCESS_FILESYSTEM);
		}
	}
	launch.engine_log = log_path.simplify_path();
	launch.crash_directory = launch.engine_log.get_base_dir();
	launch.previous_engine = snapshot(launch.engine_log);
	// Take metadata before the blocking launch callback, never afterwards.
	for (const String &name : DirAccess::get_files_at(launch.crash_directory)) {
		if (name.begins_with("godot-") && name.ends_with("-crash.log")) {
			launch.previous_crashes.insert(name, snapshot(launch.crash_directory.path_join(name)));
		}
	}
	List<String> normalized;
	for (const List<String>::Element *argument = r_arguments.front(); argument; argument = argument->next()) {
		if (argument->get() == "--" || argument->get() == "++") {
			for (; argument; argument = argument->next()) {
				normalized.push_back(argument->get());
			}
			break;
		}
		if (argument->get() == "--log-file" && argument->next()) {
			argument = argument->next();
		} else {
			normalized.push_back(argument->get());
		}
	}
	normalized.push_front(launch.engine_log);
	normalized.push_front("--log-file");
	r_arguments = normalized;
	return launch;
}

void OpenHarmonyChildProcessDiagnostics::started(ProcessID p_pid, const Launch &p_launch) {
	if (!p_launch.enabled || p_pid <= 0) {
		return;
	}
	Child child;
	child.launch = p_launch;
	const String name = vformat("godot-%d-crash.log", p_pid);
	child.crash_log = p_launch.crash_directory.path_join(name);
	if (const Snapshot *previous = p_launch.previous_crashes.getptr(name)) {
		child.previous_crash = *previous;
	}
	child.launch.previous_crashes.clear();
	MutexLock lock(mutex);
	children.insert(p_pid, child);
}

void OpenHarmonyChildProcessDiagnostics::report(ProcessID p_pid, const Child &p_child, const String &p_crash) {
	const String heading = vformat("Game process %d crashed. The editor is still running.", p_pid);
	String context = "Engine log: " + p_child.launch.engine_log + "\n";
	const Snapshot engine = snapshot(p_child.launch.engine_log);
	const Snapshot &before = p_child.launch.previous_engine;
	if (engine.exists && (!before.exists || engine.inode != before.inode || engine.modified != before.modified || engine.size != before.size)) {
		context += read_tail(p_child.launch.engine_log);
	} else {
		context += "(no engine output recorded for this run)";
	}
	context += "\nNative crash log: " + p_child.crash_log + "\n" + p_crash;
	// Go directly through EditorLog so --quiet/disable_stdout cannot hide a
	// crash report. The engine loop owns both the editor and this poller.
	if (EditorNode::get_singleton() && EditorNode::get_log()) {
		EditorNode::get_log()->add_message(heading, EditorLog::MSG_TYPE_ERROR);
		EditorNode::get_log()->add_message(context, EditorLog::MSG_TYPE_STD);
		// Also persist the report in the parent's log without printing it to
		// EditorLog twice through EditorNode's global print handler.
		OS::get_singleton()->print("%s\n", (heading + "\n" + context).utf8().get_data());
	} else {
		print_error(heading);
		print_line(context);
	}
}

void OpenHarmonyChildProcessDiagnostics::poll() {
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	HashMap<ProcessID, Child> pending;
	{
		MutexLock lock(mutex);
		if (children.is_empty() || now < next_poll) {
			return;
		}
		next_poll = now + 250000;
		pending = children;
	}
	for (KeyValue<ProcessID, Child> &entry : pending) {
		Child &child = entry.value;
		const Snapshot current = snapshot(child.crash_log);
		const Snapshot &before = child.previous_crash;
		const bool replaced = !before.exists || current.inode != before.inode || current.size < before.size;
		const uint64_t start = replaced ? 0 : before.size;
		const bool has_crash = current.exists && current.size > start;
		const bool running = ::kill(entry.key, 0) == 0 || errno == EPERM;
		bool finished = false;
		if (has_crash) {
			if (!child.first_crash) {
				child.first_crash = now;
			}
			const String crash = read_tail(child.crash_log, start);
			if (crash.contains("=== end of OpenHarmony crash backtrace ===") || !running || now - child.first_crash >= 1000000) {
				report(entry.key, child, crash);
				finished = true;
			}
		} else if (!running) {
			if (!child.first_exit) {
				child.first_exit = now;
			}
			// Final checks allow a short delayed file publication after exit.
			finished = now - child.first_exit >= 1000000;
		}
		MutexLock lock(mutex);
		Child *tracked = children.getptr(entry.key);
		if (tracked && tracked->launch.generation == child.launch.generation) {
			if (finished) {
				children.erase(entry.key);
			} else {
				tracked->first_crash = child.first_crash;
				tracked->first_exit = child.first_exit;
			}
		}
	}
}
#endif
