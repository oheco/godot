// Godot Engine contributors. SPDX-License-Identifier: MIT
// Execute the production header through an in-memory HDC executor. No devices,
// processes, installation, port forwarding or real HAP files are involved.
#include "export/device_run.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using Args = std::vector<std::string>;
struct Step {
	Args command;
	bool ok = true;
	std::string output;
};
struct Fake {
	std::string target = "fixture-target with spaces";
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
static Step forward(int p_port, bool p_ok = true) {
	const std::string endpoint = "tcp:" + std::to_string(p_port);
	return { { "rport", endpoint, endpoint }, p_ok, p_ok ? "reverse ok" : "reverse failed" };
}
static Step remove(int p_port, bool p_ok = true) {
	const std::string endpoint = "tcp:" + std::to_string(p_port);
	return { { "fport", "rm", endpoint, endpoint }, p_ok, p_ok ? "remove ok" : "remove failed" };
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

int main() {
	std::string output;
	{
		Fake fake;
		fake.steps = { ls(), ls(), install(), forward(6007), forward(6010), start(), stop(), remove(6010), remove(6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.start(HAP, { 6007, 6010 }, output));
		assert(session.active());
		assert((session.owned_ports() == std::vector<int>{ 6007, 6010 }));
		assert(output.find("start ok") != std::string::npos);
		const auto before = fake.cursor;
		assert(!session.start(HAP, {}, output));
		assert(fake.cursor == before); // No overlapping session on this object.
		assert(session.stop(output));
		assert(!session.active());
		assert(session.owned_ports().empty());
		assert(session.stop(output));
		assert(session.finish(output));
		fake.done();
		passed("exact target argv, two ports, start/stop and idempotence");
	}
	{
		Fake fake;
		fake.steps = { install(), start(), stop() };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.start(HAP, {}, output));
		assert(session.active());
		assert(session.owned_ports().empty());
		assert(session.stop(output));
		assert(!session.active());
		fake.done();
		passed("run without debug ports still force-stops owned bundle");
	}
	{
		Fake fake;
		fake.steps = { ls(), ls(), install(false) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.start(HAP, { 6007, 6010 }, output));
		assert(output.find("install failed") != std::string::npos);
		assert(!session.active());
		assert(session.stop(output));
		fake.done();
		passed("install failure never creates or deletes forwarding");
	}
	{
		Fake fake;
		fake.steps = { ls(), ls(), install(), forward(6007), forward(6010, false), remove(6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.start(HAP, { 6007, 6010 }, output));
		assert(!session.active());
		assert(session.owned_ports().empty());
		fake.done();
		passed("second forward failure removes only first confirmed owned endpoint");
	}
	{
		Fake fake;
		fake.steps = { ls(), ls(), install(), forward(6007), forward(6010), start(false), stop(), remove(6010), remove(6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.start(HAP, { 6007, 6010 }, output));
		assert(!session.active());
		assert(output.find("start failed") != std::string::npos);
		fake.done();
		passed("failed ability start force-stops partial start then rolls back own endpoints");
	}
	{
		Fake fake;
		fake.steps = { ls(), install(), forward(6007), start(), remove(6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.start(HAP, { 6007 }, output));
		assert(session.finish(output));
		assert(!session.active());
		assert(session.finish(output));
		assert(session.stop(output));
		fake.done();
		passed("natural completion removes owned endpoints without force-stop");
	}
	for (const std::string &listing : {
				 "tcp:6007 tcp:6007 [Forward]\n",
				 "[Reverse] tcp:6007 tcp:6007\r\n",
				 "fixture-target tcp:6007 tcp:6007 [Reverse]\n",
				 "[Forward] target=fixture-target tcp:6007 tcp:6007\n",
				 "fixture-target tcp:6007 tcp:7007 [Reverse]\n",
				 "[Forward] fixture-target tcp:7007 tcp:6007\n",
				 "fixture-target tcp:0006007 tcp:7007 [Reverse]\n" }) {
		Fake fake;
		fake.target = "fixture-target";
		fake.steps = { ls(listing) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.start(HAP, { 6007 }, output));
		assert(!session.active());
		assert(session.stop(output)); // MUST NOT delete collision owned by others.
		fake.done();
		passed("collision rejects either exact endpoint without deleting existing rule");
	}
	{
		Fake fake;
		fake.target = "fixture-target";
		fake.steps = { ls("unrelated-target tcp:6007 tcp:6007 [Reverse]\nfixture-target tcp:60070 tcp:60070 [Forward]\nfixture-target tcp:6017 tcp:6010 [Reverse]\n"),
			install(), forward(6007), start(), stop(), remove(6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.start(HAP, { 6007 }, output));
		assert(session.stop(output));
		fake.done();
		passed("unrelated target and different exact TCP ports do not collide");
	}
	for (const std::string &listing : { "unrecognized listing banner", "[Bogus] tcp:6007 tcp:6007\n", "tcp:6007\n",
				 "tcp:not-a-port tcp:6007 [Reverse]\n", "[Reverse] [Forward] tcp:6007 tcp:6007\n" }) {
		Fake fake;
		fake.steps = { ls(listing) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.start(HAP, { 6007 }, output));
		assert(!session.active());
		assert(output.find("Unrecognized") != std::string::npos);
		fake.done();
		passed("unknown nonempty listing format fails closed");
	}
	{
		Fake fake;
		fake.steps = { ls("device unavailable", false) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.start(HAP, { 6007 }, output));
		assert(!session.active());
		fake.done();
		passed("failed list preflight never installs or creates forwarding");
	}
	{
		Fake fake;
		fake.steps = { ls("[Empty]\n"), ls("Empty\r\n"), install(), forward(1), forward(65535), start(), stop(), remove(65535), remove(1) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.start(HAP, { 1, 65535 }, output));
		assert(session.stop(output));
		fake.done();
		passed("TCP boundary ports and recognized empty listings");
	}
	for (const std::vector<int> &ports : { std::vector<int>{ 0 }, std::vector<int>{ -1 }, std::vector<int>{ 65536 },
				 std::vector<int>{ 6007, 6007 }, std::vector<int>(17, 6007) }) {
		Fake fake;
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.start(HAP, ports, output));
		assert(!session.active());
		fake.done();
		passed("invalid, duplicate or unbounded ports rejected before executor");
	}
	{
		Fake fake;
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.start("relative.hap", {}, output));
		assert(!session.start(std::string("/private/a\0b.hap", 16), {}, output));
		DeviceRunSession invalid_target("", BUNDLE, fake.callback());
		assert(!invalid_target.start(HAP, {}, output));
		DeviceRunSession invalid_bundle(fake.target, "bad; shell injection", fake.callback());
		assert(!invalid_bundle.start(HAP, {}, output));
		DeviceRunSession no_executor(fake.target, BUNDLE, {});
		assert(!no_executor.start(HAP, {}, output));
		fake.done();
		passed("invalid paths, target, bundle and missing callback rejected before executor");
	}
	{
		Fake fake;
		fake.steps = { ls(), ls(), install(), forward(6007), forward(6010), start(),
			stop(), remove(6010, false), remove(6007), remove(6010) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.start(HAP, { 6007, 6010 }, output));
		assert(!session.stop(output));
		assert(session.active());
		assert((session.owned_ports() == std::vector<int>{ 6010 }));
		const auto before = fake.cursor;
		assert(!session.start(HAP, {}, output));
		assert(fake.cursor == before);
		assert(session.stop(output)); // No redundant force-stop of an already stopped ability.
		assert(!session.active());
		fake.done();
		passed("failed port cleanup retains only failed owned endpoint for stop retry");
	}
	{
		Fake fake;
		fake.steps = { ls(), install(), forward(6007), start(), stop(false), remove(6007), stop() };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.start(HAP, { 6007 }, output));
		assert(!session.stop(output));
		assert(session.active());
		assert(session.owned_ports().empty());
		assert(output.find("stop failed") != std::string::npos);
		assert(session.stop(output));
		assert(!session.active());
		fake.done();
		passed("failed force-stop retains running state while still cleaning all ports");
	}
	{
		Fake fake;
		fake.steps = { ls(), ls(), install(), forward(6007), forward(6010, false), remove(6007, false), remove(6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.start(HAP, { 6007, 6010 }, output));
		assert(session.active());
		assert((session.owned_ports() == std::vector<int>{ 6007 }));
		assert(session.stop(output));
		assert(!session.active());
		fake.done();
		passed("failed forward rollback retains own endpoint and retries without force-stop");
	}
	{
		Fake fake;
		fake.steps = { ls(), install(), forward(6007), start(false), stop(false), remove(6007, false), stop(), remove(6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(!session.start(HAP, { 6007 }, output));
		assert(session.active());
		assert((session.owned_ports() == std::vector<int>{ 6007 }));
		assert(session.stop(output));
		assert(!session.active());
		fake.done();
		passed("half-start rollback failures retain both running and owned state for retry");
	}
	{
		Fake fake;
		fake.steps = { ls(), install(), forward(6007), start(), remove(6007, false), remove(6007) };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.start(HAP, { 6007 }, output));
		assert(!session.finish(output));
		assert(session.active());
		assert((session.owned_ports() == std::vector<int>{ 6007 }));
		assert(session.finish(output));
		assert(!session.active());
		fake.done();
		passed("natural finish retains failed cleanup endpoint and retries without force-stop");
	}
	{
		Fake fake;
		fake.steps = { install(), start(), stop(), install(), start() };
		DeviceRunSession session(fake.target, BUNDLE, fake.callback());
		assert(session.start(HAP, {}, output));
		assert(session.stop(output));
		assert(session.start(HAP, {}, output));
		assert(session.finish(output));
		assert(!session.active());
		fake.done();
		passed("fully cleaned session may start again safely");
	}
	std::cout << "PASS: " << scenarios << " native production device-run scenarios; no real HDC/device actions\n";
}
