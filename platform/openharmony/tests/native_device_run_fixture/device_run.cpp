// Godot Engine contributors. SPDX-License-Identifier: MIT
// Execute the production header through an in-memory HDC executor. A loopback
// socket regression also reproduces a local Editor listener and busy candidate.
// No real HDC, processes, installation, forwarding or HAP files are involved.
#include "export/device_run.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using Args = std::vector<std::string>;
using Mapping = DeviceRunSession::PortMapping;
using Mappings = std::vector<Mapping>;
struct Step {
	Args command;
	bool ok = true;
	std::string output;
};
struct Fake {
	std::string target = "fixture-target";
	std::vector<Step> steps;
	unsigned cursor = 0;

	bool execute(const Args &p_args, std::string &r_output) {
		assert(cursor < steps.size());
		Args expected{ "-t", target };
		expected.insert(expected.end(), steps[cursor].command.begin(), steps[cursor].command.end());
		if (p_args != expected) {
			std::cerr << "Unexpected argv at step " << cursor << '\n';
			for (const auto &arg : p_args) {
				std::cerr << '[' << arg << ']';
			}
			std::cerr << '\n';
		}
		assert(p_args == expected); // In particular: no shell, no duplicate -t.
		r_output = steps[cursor].output;
		return steps[cursor++].ok;
	}
	void done() const { assert(cursor == steps.size()); }
	DeviceRunSession::Executor callback() {
		return [this](const Args &args, std::string &output) { return execute(args, output); };
	}
};
static const std::string BUNDLE = "org.oheco.fixture";
static const std::string HAP = "/private/generated app/synthetic-signed.hap";
static Step ls(const std::string &p_output = "", bool p_ok = true) {
	return { { "fport", "ls" }, p_ok, p_output };
}
static Step install(bool p_ok = true) {
	return { { "install", "-r", HAP }, p_ok, p_ok ? "install ok" : "install failed" };
}
static Step forward(int p_device, int p_host, bool p_ok = true) {
	return { { "rport", "tcp:" + std::to_string(p_device), "tcp:" + std::to_string(p_host) }, p_ok, p_ok ? "reverse ok" : "reverse failed" };
}
static Step remove(int p_device, int p_host, bool p_ok = true) {
	return { { "fport", "rm", "tcp:" + std::to_string(p_device), "tcp:" + std::to_string(p_host) }, p_ok, p_ok ? "remove ok" : "remove failed" };
}
static Step start(bool p_ok = true) {
	return { { "shell", "aa", "start", "-b", BUNDLE, "-a", "EntryAbility" }, p_ok, p_ok ? "start ok" : "start failed" };
}
static Step stop(bool p_ok = true) {
	return { { "shell", "aa", "force-stop", "-b", BUNDLE }, p_ok, p_ok ? "stop ok" : "stop failed" };
}
static unsigned scenarios = 0;
static void passed(const std::string &p_name) {
	++scenarios;
	std::cout << "PASS: " << p_name << '\n';
}

static int listener(int p_port) {
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	assert(fd >= 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(p_port);
	if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(fd, 1) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}
static int listener_port(int p_fd) {
	sockaddr_in address{};
	socklen_t size = sizeof(address);
	assert(getsockname(p_fd, reinterpret_cast<sockaddr *>(&address), &size) == 0);
	return ntohs(address.sin_port);
}
static void local_listener_regression() {
	const int host_fd = listener(0);
	assert(host_fd >= 0);
	const int host_port = listener_port(host_fd);
	const int first_candidate = host_port == 65535 ? 1 : host_port + 1;
	// If another process already owns this port, it is already a busy candidate.
	const int busy_fd = listener(first_candidate);
	std::vector<int> attempted;
	int device_fd = -1;
	int device_port = 0;
	DeviceRunSession session("local-target", BUNDLE, [&](const Args &args, std::string &out) {
		assert(args.size() >= 4 && args[0] == "-t" && args[1] == "local-target");
		if (args[2] == "rport") {
			assert(args.size() == 5 && args[4] == "tcp:" + std::to_string(host_port));
			const int candidate = std::stoi(args[3].substr(4));
			attempted.push_back(candidate);
			assert(candidate != host_port);
			const int fd = listener(candidate);
			if (fd < 0) {
				out = "[Fail] Address already in use";
				return false;
			}
			device_fd = fd;
			device_port = candidate;
		} else if (args[2] == "fport" && args[3] == "rm") {
			assert(args.size() == 6 && args[4] == "tcp:" + std::to_string(device_port) && args[5] == "tcp:" + std::to_string(host_port));
			assert(device_fd >= 0);
			close(device_fd);
			device_fd = -1;
		} else if (args[2] == "fport") {
			assert(args.size() == 4 && args[3] == "ls");
			out = "[Empty]\n"; // Ordinary listeners do not appear in HDC's rule list.
		}
		return true;
	});
	std::string output;
	assert(session.prepare_ports({ host_port }, output));
	assert(attempted.size() >= 2 && attempted.front() == first_candidate);
	assert(device_port != host_port && device_port != first_candidate);
	Args arguments{ "--remote-debug", "tcp://localhost:" + std::to_string(host_port) };
	assert(session.remap_arguments(arguments));
	assert(arguments[1] == "tcp://127.0.0.1:" + std::to_string(device_port));
	assert(session.start(HAP, output));
	assert(session.stop(output));
	assert(device_fd == -1 && listener_port(host_fd) == host_port);
	if (busy_fd >= 0) {
		assert(listener_port(busy_fd) == first_candidate);
		close(busy_fd);
	}
	close(host_fd);
	passed("real local Editor listener and busy device candidate select a different usable port");
}

int main() {
	std::string output;
	{
		Fake fake;
		fake.target = "fixture-target with spaces";
		fake.steps = { ls(), forward(6008, 6007), forward(6011, 6010), install(), start(), stop(), remove(6011, 6010), remove(6008, 6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.prepare_ports({ 6007, 6010 }, output));
		assert((session.mappings() == Mappings{ { 6008, 6007 }, { 6011, 6010 } }));
		assert(session.owned_mappings() == session.mappings());
		Args arguments{ "--remote-fs", "localhost:6010", "--remote-fs-password", "--remote-debug", "--remote-debug", "tcp://localhost:6007", "--breakpoints", "res://script.gd:23", "--debug-collisions" };
		assert(session.remap_arguments(arguments));
		assert((arguments == Args{ "--remote-fs", "127.0.0.1:6011", "--remote-fs-password", "--remote-debug", "--remote-debug", "tcp://127.0.0.1:6008", "--breakpoints", "res://script.gd:23", "--debug-collisions" }));
		assert(session.start(HAP, output));
		assert(output.find("start ok") != std::string::npos);
		const auto before = fake.cursor;
		assert(!session.prepare_ports({}, output));
		assert(!session.start(HAP, output));
		assert(fake.cursor == before);
		assert(session.stop(output));
		assert(!session.active() && session.owned_mappings().empty());
		assert(session.stop(output) && session.finish(output));
		fake.done();
		passed("exact target argv, distinct mappings, real debug/fs arguments and idempotent cleanup");
	}
	{
		Fake fake;
		fake.steps = { install(), start(), stop(), install(), start() };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.start(HAP, output));
		assert(session.prepare_ports({}, output));
		assert(session.start(HAP, output));
		assert(session.owned_mappings().empty());
		assert(session.stop(output));
		assert(session.prepare_ports({}, output));
		assert(session.start(HAP, output));
		assert(session.finish(output));
		assert(!session.active());
		fake.done();
		passed("run without debug ports still stops owned bundle and can restart after cleanup");
	}
	{
		Fake fake;
		fake.steps = { ls(), forward(6008, 6007), forward(6011, 6010), install(false), remove(6011, 6010), remove(6008, 6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.prepare_ports({ 6007, 6010 }, output));
		assert(!session.start(HAP, output));
		assert(output.find("install failed") != std::string::npos);
		assert(!session.active());
		assert(session.stop(output));
		fake.done();
		passed("install failure releases prepared mappings without force-stopping a bundle");
	}
	{
		Fake fake;
		fake.steps = { ls(), forward(6008, 6007), forward(6011, 6010, false), ls("disconnected", false), remove(6008, 6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.prepare_ports({ 6007, 6010 }, output));
		assert(!session.active());
		fake.done();
		passed("second mapping failure removes only first confirmed owned endpoint pair");
	}
	{
		Fake fake;
		fake.steps = { ls(), forward(6008, 6007), install(), start(false), stop(), remove(6008, 6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.prepare_ports({ 6007 }, output));
		assert(!session.start(HAP, output));
		assert(!session.active());
		assert(output.find("start failed") != std::string::npos);
		fake.done();
		passed("failed ability start force-stops partial start and rolls back own endpoint pair");
	}
	for (const std::string &listing : {
				 "fixture-target tcp:8007 tcp:6007 [Reverse]\n",
				 "[Reverse] target=fixture-target tcp:0008007 tcp:0006007\r\n",
				 "fixture-target Reverse tcp:8007 tcp:6007\n" }) {
		Fake fake;
		fake.steps = { ls(listing), install(), start(), stop() };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.prepare_ports({ 6007 }, output));
		assert((session.mappings() == Mappings{ { 8007, 6007 } }));
		assert(session.owned_mappings().empty());
		assert(session.start(HAP, output));
		assert(session.stop(output));
		fake.done();
		passed("compatible target reverse mapping is borrowed and never deleted");
	}
	{
		Fake fake;
		fake.steps = { ls("fixture-target tcp:8007 tcp:6007 [Reverse]\n"), forward(6011, 6010),
			install(false), remove(6011, 6010, false), remove(6011, 6010) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.prepare_ports({ 6007, 6010 }, output));
		assert((session.mappings() == Mappings{ { 8007, 6007 }, { 6011, 6010 } }));
		assert((session.owned_mappings() == Mappings{ { 6011, 6010 } }));
		assert(!session.start(HAP, output));
		assert(session.active());
		assert(session.stop(output));
		assert(!session.active());
		fake.done();
		passed("mixed borrowed/owned mappings preserve existing rule even after install rollback retry");
	}
	for (const std::string &listing : {
				 "fixture-target tcp:6007 tcp:6007 [Reverse]\n", // Same-port rule must not be reused.
				 "unrelated-target tcp:8007 tcp:6007 [Reverse]\n",
				 "fixture-target tcp:8007 tcp:6007 [Forward]\n",
				 "tcp:8007 tcp:6007 [Reverse]\n", // Cannot positively identify its target.
				 "fixture-target tcp:8007 tcp:6007\n", // Cannot positively identify its direction.
				 "fixture-target localabstract:debug tcp:6007 [Reverse]\n" }) {
		Fake fake;
		fake.steps = { ls(listing), forward(6008, 6007), remove(6008, 6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.prepare_ports({ 6007 }, output));
		assert((session.mappings() == Mappings{ { 6008, 6007 } }));
		assert(session.finish(output));
		assert(!session.active());
		fake.done();
		passed("same-port, foreign or ambiguous existing rule is preserved and not reused");
	}
	{
		Fake fake;
		fake.steps = { ls("fixture-target tcp:0006008 tcp:9001 [Reverse]\nother-target tcp:9002 tcp:6009 [Forward]\n"),
			forward(6011, 6007), forward(6012, 6010), remove(6012, 6010), remove(6011, 6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.prepare_ports({ 6007, 6010 }, output));
		assert((session.mappings() == Mappings{ { 6011, 6007 }, { 6012, 6010 } }));
		assert(session.stop(output)); // Export cancellation: no install or force-stop.
		fake.done();
		passed("candidate selection avoids all host listeners, mapped devices and existing rule endpoints");
	}
	{
		Fake fake;
		fake.steps = { ls(), forward(6008, 6007, false), ls("fixture-target tcp:6009 tcp:9009 [Reverse]\n"),
			forward(6010, 6007), remove(6010, 6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.prepare_ports({ 6007 }, output));
		assert((session.mappings() == Mappings{ { 6010, 6007 } }));
		assert(session.finish(output));
		fake.done();
		passed("failed bind refreshes rules and skips newly occupied candidates without deleting them");
	}
	for (const std::string &listing : { "unrecognized listing banner", "[Bogus] tcp:6007 tcp:6007\n", "tcp:6007\n",
				 "tcp:not-a-port tcp:6007 [Reverse]\n", "[Reverse] [Forward] tcp:6007 tcp:6007\n" }) {
		Fake fake;
		fake.steps = { ls(listing) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.prepare_ports({ 6007 }, output));
		assert(!session.active());
		assert(output.find("Unrecognized") != std::string::npos);
		fake.done();
		passed("unknown nonempty listing format fails closed before any mutation");
	}
	{
		Fake fake;
		fake.steps = { ls("device unavailable", false) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.prepare_ports({ 6007 }, output));
		assert(!session.active());
		fake.done();
		passed("failed list preflight never installs or creates forwarding");
	}
	{
		Fake fake;
		fake.steps = { ls("[Empty]\n"), forward(2, 65535), forward(3, 1), remove(3, 1), remove(2, 65535) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.prepare_ports({ 65535, 1 }, output));
		assert(session.stop(output));
		fake.done();
		passed("TCP boundary ports wrap safely and avoid the other requested host port");
	}
	for (const std::vector<int> &ports : { std::vector<int>{ 0 }, std::vector<int>{ -1 }, std::vector<int>{ 65536 },
				 std::vector<int>{ 6007, 6007 }, std::vector<int>(17, 6007) }) {
		Fake fake;
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.prepare_ports(ports, output));
		assert(!session.active());
		fake.done();
		passed("invalid, duplicate or unbounded host ports rejected before executor");
	}
	{
		Fake fake;
		DeviceRunSession invalid_target("", BUNDLE, fake.callback());
		assert(!invalid_target.prepare_ports({}, output));
		DeviceRunSession invalid_bundle(fake.target, "bad; shell injection", fake.callback());
		assert(!invalid_bundle.prepare_ports({}, output));
		DeviceRunSession no_executor(fake.target, BUNDLE, {});
		assert(!no_executor.prepare_ports({}, output));
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.prepare_ports({}, output));
		assert(!session.start("relative.hap", output));
		assert(!session.start(std::string("/private/a\0b.hap", 16), output));
		assert(session.finish(output));
		fake.done();
		passed("invalid paths, target, bundle and callback rejected before executor");
	}
	{
		Fake fake;
		fake.steps = { ls(), forward(6008, 6007), forward(6011, 6010), install(), start(),
			stop(), remove(6011, 6010, false), remove(6008, 6007), remove(6011, 6010) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.prepare_ports({ 6007, 6010 }, output));
		assert(session.start(HAP, output));
		assert(!session.stop(output));
		assert(session.active());
		assert((session.owned_mappings() == Mappings{ { 6011, 6010 } }));
		const auto before = fake.cursor;
		assert(!session.prepare_ports({}, output));
		assert(fake.cursor == before);
		assert(session.stop(output));
		assert(!session.active());
		fake.done();
		passed("failed cleanup retains only failed full endpoint pair for stop retry");
	}
	{
		Fake fake;
		fake.steps = { ls(), forward(6008, 6007), install(), start(), stop(false), remove(6008, 6007), stop() };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.prepare_ports({ 6007 }, output));
		assert(session.start(HAP, output));
		assert(!session.stop(output));
		assert(session.active() && session.owned_mappings().empty());
		assert(output.find("stop failed") != std::string::npos);
		assert(session.stop(output));
		assert(!session.active());
		fake.done();
		passed("failed force-stop retains running state while still cleaning all owned ports");
	}
	{
		Fake fake;
		fake.steps = { ls(), forward(6008, 6007), forward(6011, 6010, false), ls("offline", false),
			remove(6008, 6007, false), remove(6008, 6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.prepare_ports({ 6007, 6010 }, output));
		assert(session.active());
		assert((session.owned_mappings() == Mappings{ { 6008, 6007 } }));
		assert(session.stop(output));
		assert(!session.active());
		fake.done();
		passed("failed reservation rollback retains owned mapping and retries without force-stop");
	}
	{
		Fake fake;
		fake.steps = { ls(), forward(6008, 6007), install(), start(false), stop(false), remove(6008, 6007, false), stop(), remove(6008, 6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.prepare_ports({ 6007 }, output));
		assert(!session.start(HAP, output));
		assert(session.active());
		assert((session.owned_mappings() == Mappings{ { 6008, 6007 } }));
		assert(session.stop(output));
		assert(!session.active());
		fake.done();
		passed("partial-start rollback failures retain running and owned state for retry");
	}
	{
		Fake fake;
		fake.steps = { ls(), forward(6008, 6007), install(), start(), remove(6008, 6007, false), remove(6008, 6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.prepare_ports({ 6007 }, output));
		assert(session.start(HAP, output));
		assert(!session.finish(output));
		assert(session.active());
		assert(session.finish(output));
		assert(!session.active());
		fake.done();
		passed("natural completion retries failed endpoint cleanup without force-stop");
	}
	{
		Fake fake;
		fake.steps = { ls() };
		for (unsigned i = 0; i < DeviceRunSession::MAX_PORT_ATTEMPTS; ++i) {
			fake.steps.push_back(forward(6008 + i, 6007, false));
			fake.steps.push_back(ls());
		}
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.prepare_ports({ 6007 }, output));
		assert(!session.active());
		fake.done();
		passed("candidate bind retries are bounded and never claim unsuccessful rules");
	}
	{
		Fake fake;
		fake.steps = { ls(), forward(6008, 6007), remove(6008, 6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		Args arguments{ "--remote-debug", "tcp://localhost:6007" };
		assert(!session.remap_arguments(arguments));
		assert(session.prepare_ports({ 6007 }, output));
		for (Args invalid : { Args{ "--remote-debug" }, Args{ "--remote-debug", "tcp://localhost:6010" }, Args{ "--remote-fs", "localhost:0" } }) {
			assert(!session.remap_arguments(invalid));
		}
		arguments = { "--remote-debug", "ws://localhost:6007" };
		assert(session.remap_arguments(arguments));
		assert(arguments[1] == "ws://127.0.0.1:6008");
		assert(session.finish(output));
		fake.done();
		passed("command line rejects missing mappings and preserves the actual debug protocol");
	}
	local_listener_regression();
	std::cout << "PASS: " << scenarios << " native production device-run scenarios; no real HDC/device actions\n";
}
