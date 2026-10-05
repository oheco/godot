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
	static constexpr unsigned MAX_PORTS = 16;

	DeviceRunSession(const std::string &p_target, const std::string &p_bundle, Executor p_executor) :
			target_(p_target), bundle_(p_bundle), executor_(p_executor) {}
	DeviceRunSession(const DeviceRunSession &) = delete;
	DeviceRunSession &operator=(const DeviceRunSession &) = delete;

	bool active() const { return running_ || !owned_ports_.empty(); }
	const std::vector<int> &owned_ports() const { return owned_ports_; }

	bool start(const std::string &p_absolute_hap, const std::vector<int> &p_ports, std::string &r_output) {
		r_output.clear();
		if (active()) {
			r_output = "Device session is active or has cleanup pending; stop/finish it before starting again.\n";
			return false;
		}
		if (!executor_ || !valid_text(target_) || !valid_bundle(bundle_) || !valid_text(p_absolute_hap) || p_absolute_hap[0] != '/') {
			r_output = "A valid executor, target, bundle and absolute HAP path are required.\n";
			return false;
		}
		if (p_ports.size() > MAX_PORTS) {
			r_output = "Too many debug ports (maximum 16).\n";
			return false;
		}
		for (auto i = p_ports.begin(); i != p_ports.end(); ++i) {
			if (*i < 1 || *i > 65535) {
				r_output = "Debug ports must be TCP ports in 1..65535.\n";
				return false;
			}
			for (auto j = p_ports.begin(); j != i; ++j) {
				if (*i == *j) {
					r_output = "Debug ports must be unique.\n";
					return false;
				}
			}
		}
		// Check every requested endpoint before installing or creating any rule.
		for (int port : p_ports) {
			std::string listing;
			if (!execute({ "fport", "ls" }, listing, r_output)) {
				return false;
			}
			bool collision = false;
			if (!inspect_listing(listing, endpoint(port), collision)) {
				r_output += "Unrecognized nonempty HDC port listing; refusing to claim ownership.\n";
				return false;
			}
			if (collision) {
				r_output += "Requested endpoint already belongs to an existing HDC forward/reverse rule.\n";
				return false;
			}
		}
		std::string output;
		if (!execute({ "install", "-r", p_absolute_hap }, output, r_output)) {
			return false; // No forwarding happened, so there is nothing to remove.
		}
		for (int port : p_ports) {
			const std::string tcp = endpoint(port);
			if (!execute({ "rport", tcp, tcp }, output, r_output)) {
				cleanup_ports(r_output);
				return false;
			}
			owned_ports_.push_back(port); // Own only confirmed successful creations.
		}
		// A failed aa start can still partially start an ability. Keep this state
		// until force-stop succeeds, including after a failed rollback.
		running_ = true;
		if (!execute({ "shell", "aa", "start", "-b", bundle_, "-a", "EntryAbility" }, output, r_output)) {
			stop_running(r_output);
			cleanup_ports(r_output);
			return false;
		}
		return true;
	}

	bool stop(std::string &r_output) {
		r_output.clear();
		const bool stopped = stop_running(r_output);
		const bool cleaned = cleanup_ports(r_output);
		return stopped && cleaned; // Always try port cleanup even if stopping fails.
	}

	bool finish(std::string &r_output) {
		r_output.clear();
		running_ = false; // Caller reports a natural exit; never force-stop here.
		return cleanup_ports(r_output);
	}

private:
	std::string target_;
	std::string bundle_;
	Executor executor_;
	std::vector<int> owned_ports_;
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
	static bool port_endpoint(const std::string &p_word) {
		const auto colon = p_word.find(':');
		if (colon == std::string::npos || colon == 0 || colon + 1 == p_word.size()) {
			return false;
		}
		const std::string protocol = p_word.substr(0, colon);
		if (protocol == "tcp") {
			unsigned value = 0;
			for (auto i = colon + 1; i < p_word.size(); ++i) {
				if (p_word[i] < '0' || p_word[i] > '9' || value > 65535) {
					return false;
				}
				value = value * 10 + static_cast<unsigned>(p_word[i] - '0');
			}
			return value >= 1 && value <= 65535;
		}
		return protocol == "localabstract" || protocol == "localfilesystem" || protocol == "localreserved" ||
				protocol == "dev" || protocol == "jdwp" || protocol == "ark";
	}
	static bool endpoint_matches(const std::string &p_existing, const std::string &p_requested) {
		if (p_existing.compare(0, 4, "tcp:") != 0) {
			return false;
		}
		// HDC can retain a spelling supplied by another client. Leading zeros
		// must not disguise ownership of the same numerical TCP port.
		std::string::size_type first_digit = 4;
		while (first_digit + 1 < p_existing.size() && p_existing[first_digit] == '0') {
			++first_digit;
		}
		return p_existing.substr(first_digit) == p_requested.substr(4);
	}
	bool inspect_listing(const std::string &p_listing, const std::string &p_endpoint, bool &r_collision) const {
		r_collision = false;
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
			// Accept adjacent endpoints, optional direction marker, and optional
			// connect key: e.g. target tcp:6007 tcp:6007 [Reverse]. fport ls
			// may list ALL targets even with -t; never claim another target's row.
			std::vector<std::string>::size_type first = row.size();
			unsigned count = 0;
			bool has_direction = false;
			bool has_target = false;
			std::string row_target;
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
				} else if (!has_target && valid_text(*i) && !(i->front() == '[' && i->back() == ']')) {
					has_target = true;
					row_target = i->compare(0, 7, "target=") == 0 ? i->substr(7) : *i;
				} else {
					return false;
				}
			}
			if (count != 2 || first + 1 >= row.size() || !port_endpoint(row[first + 1]) || (has_target && row_target.empty())) {
				return false;
			}
			// Either endpoint is sufficient: an existing reverse rule can already
			// own the requested device TCP port while mapping to a different host
			// port. Be conservative rather than replacing another owner's rule.
			if ((!has_target || row_target == target_) && (endpoint_matches(row[first], p_endpoint) || endpoint_matches(row[first + 1], p_endpoint))) {
				r_collision = true;
			}
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
		// supported. Keep only failed removals, so subsequent cleanup can retry.
		for (auto i = owned_ports_.size(); i > 0; --i) {
			const std::string tcp = endpoint(owned_ports_[i - 1]);
			std::string output;
			if (execute({ "fport", "rm", tcp, tcp }, output, r_output)) {
				owned_ports_.erase(owned_ports_.begin() + (i - 1));
			} else {
				ok = false;
			}
		}
		return ok;
	}
};

#endif // OPENHARMONY_DEVICE_RUN_H
