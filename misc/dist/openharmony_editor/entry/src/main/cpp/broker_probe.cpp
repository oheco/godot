// Godot Engine contributors. SPDX-License-Identifier: MIT
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime_probe.h"

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>

namespace {
constexpr char REQUEST_PREFIX[] = "OHECO_BROKER_TCP_PROBE_V1 ";
constexpr char RESPONSE_PREFIX[] = "OHECO_BROKER_TCP_PROBE_OK_V1 ";
using Clock = std::chrono::steady_clock;

struct SocketOwner {
	int fd;
	~SocketOwner() {
		if (fd >= 0) {
			close(fd);
		}
	}
};

// One deadline bounds connect, send and receive together, including EINTR and
// partial I/O. Called only from a NAPI worker, never the ArkUI thread.
int wait_ready(int fd, short events, Clock::time_point deadline) {
	for (;;) {
		const auto remaining = deadline - Clock::now();
		if (remaining <= Clock::duration::zero()) {
			return ETIMEDOUT;
		}
		const int milliseconds = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count()) + 1;
		pollfd descriptor { fd, events, 0 };
		const int result = poll(&descriptor, 1, milliseconds);
		if (result < 0) {
			if (errno == EINTR) {
				continue;
			}
			return errno;
		}
		if (result == 0) {
			return ETIMEDOUT;
		}
		if (descriptor.revents & POLLNVAL) {
			return EBADF;
		}
		// Let getsockopt/send/recv report the actual error, or consume buffered
		// data followed by EOF, when HUP/ERR accompanies readiness.
		if (descriptor.revents & (events | POLLERR | POLLHUP)) {
			return 0;
		}
	}
}
} // namespace

std::string probe_broker_tcp(uint16_t port, const std::string &instance_id) {
	std::string report = "broker_probe_version=2\nbroker_transport=tcp\n";
	if (port == 0 || instance_id.size() != 32 || instance_id.find_first_not_of("0123456789abcdef") != std::string::npos) {
		return report + "broker_probe=FAIL step=arguments reason=invalid_endpoint\n";
	}
	report += "broker_address=127.0.0.1:" + std::to_string(port) + "\nbroker_instance_id=" + instance_id + "\n";
	// This nonce only correlates the request and response; it is NOT authentication.
	const std::string nonce = std::to_string(getpid()) + "-" + std::to_string(Clock::now().time_since_epoch().count());
	const std::string request = std::string(REQUEST_PREFIX) + instance_id + " " + nonce + "\n";
	const std::string expected = std::string(RESPONSE_PREFIX) + instance_id + " " + nonce + "\n";
	report += "broker_probe_nonce=" + nonce + "\n";
	report += "broker_client_pid=" + std::to_string(getpid()) + " uid=" + std::to_string(getuid()) + " gid=" + std::to_string(getgid()) + "\n";
	std::ifstream attributes("/proc/self/attr/current");
	std::string domain;
	std::getline(attributes, domain, '\0');
	while (!domain.empty() && (domain.back() == '\n' || domain.back() == '\r')) {
		domain.pop_back();
	}
	report += "broker_client_domain=" + (domain.empty() ? std::string("<unavailable>") : domain) + "\n";
	char namespace_path[128];
	const ssize_t namespace_length = readlink("/proc/self/ns/net", namespace_path, sizeof(namespace_path));
	report += "broker_client_netns=" + (namespace_length >= 0 ? std::string(namespace_path, namespace_length) : std::string("<unavailable>")) + "\n";
	auto failed = [&report](const char *step, int error) {
		return report + "broker_probe=FAIL step=" + step + " errno=" + std::to_string(error) + " (" + strerror(error) + ")\n";
	};

	SocketOwner socket_owner { socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0) };
	const int fd = socket_owner.fd;
	if (fd < 0) {
		return failed("socket", errno);
	}
	const auto deadline = Clock::now() + std::chrono::seconds(3);
	sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	// Never trust a shared discovery file to choose an external host. This native
	// API only takes a port, and always connects directly to IPv4 loopback.
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
		const int error = errno;
		if (error != EINPROGRESS) {
			return failed("connect", error);
		}
		const int ready_error = wait_ready(fd, POLLOUT, deadline);
		if (ready_error != 0) {
			return failed("connect_wait", ready_error);
		}
		int socket_error = 0;
		socklen_t error_length = sizeof(socket_error);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) < 0) {
			return failed("connect_status", errno);
		}
		if (socket_error != 0) {
			return failed("connect", socket_error);
		}
	}
	report += "broker_connect=PASS\nbroker_peer_credentials=unavailable_over_tcp\n";

	size_t sent = 0;
	while (sent < request.size()) {
		const int error = wait_ready(fd, POLLOUT, deadline);
		if (error != 0) {
			return failed("send_wait", error);
		}
		const ssize_t count = send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
		if (count < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
				continue;
			}
			return failed("send", errno);
		}
		if (count == 0) {
			return failed("send", EPIPE);
		}
		sent += count;
	}

	std::string received;
	while (received.size() < 256 && received.find('\n') == std::string::npos) {
		const int error = wait_ready(fd, POLLIN, deadline);
		if (error != 0) {
			return failed("receive_wait", error);
		}
		char buffer[256];
		const ssize_t count = recv(fd, buffer, sizeof(buffer) - received.size(), 0);
		if (count < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
				continue;
			}
			return failed("receive", errno);
		}
		if (count == 0) {
			return failed("receive_eof", ECONNRESET);
		}
		received.append(buffer, count);
	}
	if (received == "ERROR stale_instance\n") {
		return report + "broker_probe=FAIL step=protocol reason=stale_instance\n";
	}
	if (received != expected) {
		return report + "broker_probe=FAIL step=protocol reason=unexpected_response bytes=" + std::to_string(received.size()) + "\n";
	}
	return report + "broker_response=OHECO_BROKER_TCP_PROBE_OK_V1\nbroker_instance_match=PASS\nbroker_nonce_match=PASS\nbroker_probe=PASS\n";
}
