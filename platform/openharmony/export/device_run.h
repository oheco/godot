// Godot Engine contributors. SPDX-License-Identifier: MIT
#ifndef OPENHARMONY_DEVICE_RUN_H
#define OPENHARMONY_DEVICE_RUN_H

#include <functional>
#include <string>
#include <vector>

// Standalone HDC transaction helper. Executor receives arguments INCLUDING
// -t/target but NOT the executable. It must invoke HDC directly, not via a shell.
// No destructor cleanup: the caller explicitly retains failures for retry.
class DeviceRunSession {
public:
	using Executor = std::function<bool(const std::vector<std::string> &, std::string &)>;
	struct PortMapping {
		int device_port;
		int host_port;
		bool operator==(const PortMapping &p_other) const {
			return device_port == p_other.device_port && host_port == p_other.host_port;
		}
	};
	static constexpr unsigned MAX_PORTS = 16;
	static constexpr unsigned MAX_PORT_ATTEMPTS = 32;

	DeviceRunSession(const std::string &p_target, const std::string &p_bundle, Executor p_executor) :
			target_(p_target), bundle_(p_bundle), executor_(p_executor) {}
	DeviceRunSession(const DeviceRunSession &) = delete;
	DeviceRunSession &operator=(const DeviceRunSession &) = delete;

	bool active() const { return prepared_ || running_ || !owned_mappings_.empty(); }
	const std::vector<PortMapping> &mappings() const { return mappings_; }
	const std::vector<PortMapping> &owned_mappings() const { return owned_mappings_; }

	// Reserve the actual device listeners BEFORE exporting _cl_. Host ports are
	// Editor listeners and must never also become device listeners on a local
	// HDC target. A successful rport, rather than a host-only socket probe, proves
	// that the device endpoint is usable (including on a different machine).
	bool prepare_ports(const std::vector<int> &p_host_ports, std::string &r_output) {
		r_output.clear();
		if (active()) {
			r_output = "Device session is active or has cleanup pending; stop/finish it before preparing again.\n";
			return false;
		}
		if (!executor_ || !valid_text(target_) || !valid_bundle(bundle_)) {
			r_output = "A valid executor, target and bundle are required.\n";
			return false;
		}
		if (p_host_ports.size() > MAX_PORTS) {
			r_output = "Too many debug ports (maximum 16).\n";
			return false;
		}
		for (auto i = p_host_ports.begin(); i != p_host_ports.end(); ++i) {
			if (*i < 1 || *i > 65535) {
				r_output = "Debug ports must be TCP ports in 1..65535.\n";
				return false;
			}
			for (auto j = p_host_ports.begin(); j != i; ++j) {
				if (*i == *j) {
					r_output = "Debug ports must be unique.\n";
					return false;
				}
			}
		}
		std::vector<Rule> rules;
		if (!p_host_ports.empty() && !list_rules(rules, r_output)) {
			return false;
		}
		for (int host_port : p_host_ports) {
			bool mapped = false;
			for (const Rule &rule : rules) {
				// Reuse only a positively identified reverse rule on this target.
				// Borrowed rules are never added to owned_mappings_ or removed.
				if (rule.target == target_ && rule.reverse && tcp_port(rule.second) == host_port) {
					const int device_port = tcp_port(rule.first);
					if (device_port && !reserved(device_port, p_host_ports, {})) {
						mappings_.push_back({ device_port, host_port });
						mapped = true;
						break;
					}
				}
			}
			int candidate = host_port;
			for (unsigned attempts = 0, scanned = 0; !mapped && attempts < MAX_PORT_ATTEMPTS && scanned < 65535; ++scanned) {
				candidate = candidate == 65535 ? 1 : candidate + 1;
				if (reserved(candidate, p_host_ports, rules)) {
					continue;
				}
				++attempts;
				std::string output;
				if (execute({ "rport", endpoint(candidate), endpoint(host_port) }, output, r_output)) {
					const PortMapping mapping{ candidate, host_port };
					mappings_.push_back(mapping);
					owned_mappings_.push_back(mapping); // Own only confirmed creations.
					mapped = true;
				} else if (!list_rules(rules, r_output)) {
					break; // Disconnected device / unknown listing: do not keep retrying.
				}
				// An unlisted listener or racing HDC client may occupy the candidate.
				// Keep its rule intact and try the next available device endpoint.
			}
			if (!mapped) {
				r_output += "Unable to reserve a distinct device debug port.\n";
				mappings_.clear();
				cleanup_ports(r_output);
				return false;
			}
		}
		prepared_ = true;
		return true;
	}

	// Apply the confirmed mappings to the command line embedded in the HAP.
	// Keep the debugger protocol and all unrelated values, including passwords.
	bool remap_arguments(std::vector<std::string> &r_arguments) const {
		if (!prepared_) {
			return false;
		}
		for (auto i = r_arguments.begin(); i != r_arguments.end(); ++i) {
			const bool debug = *i == "--remote-debug";
			if (!debug && *i != "--remote-fs") {
				if ((*i == "--remote-fs-password" || *i == "--breakpoints") && i + 1 != r_arguments.end()) {
					++i;
				}
				continue;
			}
			if (++i == r_arguments.end()) {
				return false;
			}
			const auto colon = i->rfind(':');
			const int host_port = colon == std::string::npos ? 0 : tcp_port("tcp:" + i->substr(colon + 1));
			int device_port = 0;
			for (const PortMapping &mapping : mappings_) {
				if (mapping.host_port == host_port) {
					device_port = mapping.device_port;
					break;
				}
			}
			if (!device_port) {
				return false;
			}
			const auto scheme = i->find("://");
			const std::string protocol = debug && scheme != std::string::npos ? i->substr(0, scheme + 3) : "";
			*i = protocol + "127.0.0.1:" + std::to_string(device_port);
		}
		return true;
	}

	bool start(const std::string &p_absolute_hap, std::string &r_output) {
		r_output.clear();
		if (!prepared_ || running_ || !valid_text(p_absolute_hap) || p_absolute_hap[0] != '/') {
			r_output = "Prepare ports before starting with a valid absolute HAP path.\n";
			return false;
		}
		std::string output;
		if (!execute({ "install", "-r", p_absolute_hap }, output, r_output)) {
			prepared_ = false;
			mappings_.clear();
			cleanup_ports(r_output);
			return false;
		}
		// A failed aa start can still partially start an ability. Keep this state
		// until force-stop succeeds, including after a failed rollback.
		running_ = true;
		if (!execute({ "shell", "aa", "start", "-b", bundle_, "-a", "EntryAbility" }, output, r_output)) {
			prepared_ = false;
			mappings_.clear();
			stop_running(r_output);
			cleanup_ports(r_output);
			return false;
		}
		return true;
	}

	bool stop(std::string &r_output) {
		r_output.clear();
		prepared_ = false;
		mappings_.clear();
		const bool stopped = stop_running(r_output);
		const bool cleaned = cleanup_ports(r_output);
		return stopped && cleaned; // Always try port cleanup even if stopping fails.
	}

	bool finish(std::string &r_output) {
		r_output.clear();
		running_ = false; // Caller reports a natural exit; never force-stop here.
		prepared_ = false;
		mappings_.clear();
		return cleanup_ports(r_output);
	}

private:
	struct Rule {
		std::string target;
		std::string first;
		std::string second;
		bool reverse = false;
	};
	std::string target_;
	std::string bundle_;
	Executor executor_;
	std::vector<PortMapping> mappings_;
	std::vector<PortMapping> owned_mappings_;
	bool prepared_ = false;
	bool running_ = false;

	static bool valid_text(const std::string &p_text) {
		if (p_text.empty()) {
			return false;
		}
		for (unsigned char character : p_text) {
			if (character < 32 || character == 127) {
				return false;
			}
		}
		return true;
	}
	static bool valid_bundle(const std::string &p_bundle) {
		if (!valid_text(p_bundle)) {
			return false;
		}
		for (char character : p_bundle) {
			if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
						(character >= '0' && character <= '9') || character == '_' || character == '.')) {
				return false;
			}
		}
		return true;
	}
	static std::string endpoint(int p_port) { return "tcp:" + std::to_string(p_port); }
	static bool whitespace(char p_character) { return p_character == ' ' || p_character == '\t' || p_character == '\r'; }
	static std::vector<std::string> words(const std::string &p_line) {
		std::vector<std::string> result;
		std::string::size_type cursor = 0;
		while (cursor < p_line.size()) {
			while (cursor < p_line.size() && whitespace(p_line[cursor])) {
				++cursor;
			}
			const auto begin = cursor;
			while (cursor < p_line.size() && !whitespace(p_line[cursor])) {
				++cursor;
			}
			if (cursor > begin) {
				result.push_back(p_line.substr(begin, cursor - begin));
			}
		}
		return result;
	}
	static bool direction(const std::string &p_word) {
		return p_word == "[Forward]" || p_word == "[Reverse]" || p_word == "Forward" || p_word == "Reverse";
	}
	static int tcp_port(const std::string &p_word) {
		if (p_word.compare(0, 4, "tcp:") != 0 || p_word.size() == 4) {
			return 0;
		}
		unsigned value = 0;
		for (auto i = std::string::size_type(4); i < p_word.size(); ++i) {
			if (p_word[i] < '0' || p_word[i] > '9' || value > 65535) {
				return 0;
			}
			value = value * 10 + static_cast<unsigned>(p_word[i] - '0');
		}
		return value >= 1 && value <= 65535 ? int(value) : 0;
	}
	static bool port_endpoint(const std::string &p_word) {
		const auto colon = p_word.find(':');
		if (colon == std::string::npos || colon == 0 || colon + 1 == p_word.size()) {
			return false;
		}
		const std::string protocol = p_word.substr(0, colon);
		if (protocol == "tcp") {
			return tcp_port(p_word) != 0;
		}
		return protocol == "localabstract" || protocol == "localfilesystem" || protocol == "localreserved" ||
				protocol == "dev" || protocol == "jdwp" || protocol == "ark";
	}
	bool reserved(int p_port, const std::vector<int> &p_host_ports, const std::vector<Rule> &p_rules) const {
		for (int host_port : p_host_ports) {
			if (p_port == host_port) {
				return true;
			}
		}
		for (const PortMapping &mapping : mappings_) {
			if (p_port == mapping.device_port) {
				return true;
			}
		}
		// Avoid both endpoints, even on other targets: HDC's host listeners can
		// share the local machine with the selected device and the Editor.
		for (const Rule &rule : p_rules) {
			if (p_port == tcp_port(rule.first) || p_port == tcp_port(rule.second)) {
				return true;
			}
		}
		return false;
	}
	static bool parse_listing(const std::string &p_listing, std::vector<Rule> &r_rules) {
		r_rules.clear();
		std::string::size_type begin = 0;
		while (begin < p_listing.size()) {
			auto end = p_listing.find('\n', begin);
			if (end == std::string::npos) {
				end = p_listing.size();
			}
			const auto row = words(p_listing.substr(begin, end - begin));
			begin = end + 1;
			if (row.empty() || (row.size() == 1 && (row[0] == "Empty" || row[0] == "[Empty]"))) {
				continue;
			}
			// fport ls can include ALL targets even with -t. Accept adjacent
			// endpoints, an optional direction marker and an optional connect key.
			std::vector<std::string>::size_type first = row.size();
			unsigned count = 0;
			bool has_direction = false;
			bool has_target = false;
			Rule rule;
			for (auto i = row.begin(); i != row.end(); ++i) {
				if (port_endpoint(*i)) {
					if (first == row.size()) {
						first = static_cast<std::vector<std::string>::size_type>(i - row.begin());
					}
					++count;
				} else if (direction(*i)) {
					if (has_direction) {
						return false;
					}
					has_direction = true;
					rule.reverse = *i == "[Reverse]" || *i == "Reverse";
				} else if (!has_target && valid_text(*i) && !(i->front() == '[' && i->back() == ']')) {
					has_target = true;
					rule.target = i->compare(0, 7, "target=") == 0 ? i->substr(7) : *i;
				} else {
					return false;
				}
			}
			if (count != 2 || first + 1 >= row.size() || !port_endpoint(row[first + 1]) || (has_target && rule.target.empty())) {
				return false;
			}
			rule.first = row[first];
			rule.second = row[first + 1];
			r_rules.push_back(rule);
		}
		return true;
	}
	bool list_rules(std::vector<Rule> &r_rules, std::string &r_log) {
		std::string listing;
		if (!execute({ "fport", "ls" }, listing, r_log)) {
			return false;
		}
		if (!parse_listing(listing, r_rules)) {
			r_log += "Unrecognized nonempty HDC port listing; refusing to claim ownership.\n";
			return false;
		}
		return true;
	}
	bool execute(const std::vector<std::string> &p_command, std::string &r_output, std::string &r_log) {
		std::vector<std::string> args{ "-t", target_ };
		args.insert(args.end(), p_command.begin(), p_command.end());
		r_output.clear();
		const bool ok = executor_(args, r_output);
		r_log += ok ? "HDC step succeeded:" : "HDC step failed:";
		for (const std::string &arg : args) {
			r_log += " " + arg;
		}
		r_log += "\n";
		if (!r_output.empty()) {
			r_log += r_output;
			if (r_output.back() != '\n') {
				r_log += "\n";
			}
		}
		return ok;
	}
	bool stop_running(std::string &r_output) {
		if (!running_) {
			return true;
		}
		std::string output;
		if (!execute({ "shell", "aa", "force-stop", "-b", bundle_ }, output, r_output)) {
			return false;
		}
		running_ = false;
		return true;
	}
	bool cleanup_ports(std::string &r_output) {
		bool ok = true;
		// HDC's fport rm removes forward AND reverse rules. rport rm is not
		// supported. Keep the full device/host pair if removal needs retry.
		for (auto i = owned_mappings_.size(); i > 0; --i) {
			const PortMapping mapping = owned_mappings_[i - 1];
			std::string output;
			if (execute({ "fport", "rm", endpoint(mapping.device_port), endpoint(mapping.host_port) }, output, r_output)) {
				owned_mappings_.erase(owned_mappings_.begin() + (i - 1));
			} else {
				ok = false;
			}
		}
		return ok;
	}
};

#endif // OPENHARMONY_DEVICE_RUN_H
