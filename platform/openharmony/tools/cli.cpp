// Godot Engine contributors. SPDX-License-Identifier: MIT
#include "runtime_paths.h"

#include <cstdio>

extern "C" int godot_cli_main(int argc, char **argv);
int main(int argc, char **argv) {
	std::string notice;
	configure_cli_development_kit(argc > 0 ? argv[0] : "", notice);
	if (!notice.empty()) { std::fprintf(stderr, "Godot development kit: %s\n", notice.c_str()); }
	return godot_cli_main(argc, argv);
}
