// Godot Engine contributors. SPDX-License-Identifier: MIT
// Native CLI entry for headless imports, binding generation and C# validation.
#include "os_openharmony.h"

#include "main/main.h"

#include <clocale>
#include <cstdlib>

extern "C" __attribute__((visibility("default"))) int godot_cli_main(int argc, char **argv) {
	if (argc < 1) {
		return EXIT_FAILURE;
	}
	OS_OpenHarmony::EXEC_PATH = argv[0];
	OS_OpenHarmony os(false);
	setlocale(LC_CTYPE, "");
	Error error = Main::setup(argv[0], argc - 1, argv + 1);
	if (error != OK) {
		return error == ERR_HELP ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	if (Main::start() == EXIT_SUCCESS) {
		if (os.get_main_loop()) {
			os.main_loop_begin();
			while (!os.main_loop_iterate()) {
			}
			os.main_loop_end();
		}
	} else {
		os.set_exit_code(EXIT_FAILURE);
	}
	Main::cleanup();
	return os.get_exit_code();
}
