// Godot Engine contributors. SPDX-License-Identifier: MIT
#include "export/broker_client.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char **argv) {
	if (argc < 6) {
		return 2;
	}
	const std::string mode = argv[1];
	const std::string endpoint = argv[2];
	const int timeout = std::stoi(argv[3]);
	const bool read_stderr = std::string(argv[4]) == "1";
	std::string command = argv[5];
	std::vector<std::string> args;
	for (int i = 6; i < argc; ++i) {
		args.emplace_back(argv[i]);
	}
	if (mode == "nul-command") {
		command.append("\0suffix", 7);
	} else if (mode == "nul-arg") {
		args.emplace_back("prefix\0suffix", 13);
	} else if (mode == "bad-utf8") {
		args.emplace_back("\xed\xa0\x80", 3);
	}
	const auto begin = std::chrono::steady_clock::now();
	const auto result = OpenHarmonyBroker::execute(endpoint, command, args, read_stderr, timeout);
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
	// Keep the host alive so peer EOF must come from late SDK release, rather
	// than process teardown hiding a dangling startup handle.
	if (mode == "startup-keepalive") {
		std::this_thread::sleep_for(std::chrono::milliseconds(600));
	}
	std::cout << result.error << '\n' << result.exit_code << '\n' << elapsed << '\n'
			  << result.output.size() << '\n' << result.diagnostic.size() << '\n';
	std::cout.write(result.output.data(), result.output.size());
	std::cout.write(result.diagnostic.data(), result.diagnostic.size());
	return 0;
}
