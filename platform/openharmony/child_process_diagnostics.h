// Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

#ifdef TOOLS_ENABLED
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"

// All reports are delivered by the parent's engine loop. A child cannot be
// expected to flush its debugger connection from a fatal signal handler.
class OpenHarmonyChildProcessDiagnostics {
	struct Snapshot {
		bool exists = false;
		uint64_t inode = 0;
		uint64_t size = 0;
		uint64_t modified = 0;
	};

public:
	struct Launch {
		bool enabled = false;
		uint64_t generation = 0;
		String engine_log;
		String crash_directory;
		HashMap<String, Snapshot> previous_crashes;
		Snapshot previous_engine;
	};

private:
	struct Child {
		Launch launch;
		String crash_log;
		Snapshot previous_crash;
		uint64_t first_crash = 0;
		uint64_t first_exit = 0;
	};
	Mutex mutex;
	HashMap<ProcessID, Child> children;
	uint64_t generation = 0;
	uint64_t next_poll = 0;

	static Snapshot snapshot(const String &p_path);
	static String read_tail(const String &p_path, uint64_t p_start = 0);
	static void report(ProcessID p_pid, const Child &p_child, const String &p_crash);

public:
	// Called BEFORE spawn: the child may crash before the launcher returns its PID.
	// Normalize the requested log path and override host defaults through the
	// existing argument channel, without changing the native host ABI.
	Launch prepare(List<String> &r_arguments, const String &p_data_directory);
	void started(ProcessID p_pid, const Launch &p_launch);
	void poll();
};
#endif
