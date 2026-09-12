// Godot Engine contributors. SPDX-License-Identifier: MIT
extern "C" int godot_cli_main(int argc, char **argv);
int main(int argc, char **argv) {
	return godot_cli_main(argc, argv);
}
