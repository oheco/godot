// Godot Engine contributors. SPDX-License-Identifier: MIT
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime_probe.h"

#include <poll.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>

namespace {
constexpr char SOCKET_NAME[] = "oheco.broker.v1";
constexpr char REQUEST[] = "OHECO_BROKER_PROBE_V1\n";
constexpr char RESPONSE[] = "OHECO_BROKER_PROBE_OK_V1\n";
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

std::string probe_broker_socket() {
	std::string report = "broker_probe_version=1\nbroker_address=@oheco.broker.v1\n";
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

	SocketOwner socket_owner { socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0) };
	const int fd = socket_owner.fd;
	if (fd < 0) {
		return failed("socket", errno);
	}
	const auto deadline = Clock::now() + std::chrono::seconds(3);
	sockaddr_un address {};
	address.sun_family = AF_UNIX;
	static_assert(sizeof(SOCKET_NAME) <= sizeof(address.sun_path));
	// Leading NUL selects the abstract namespace. Do not include a trailing NUL
	// or sizeof(sockaddr_un): those bytes would name a different abstract socket.
	memcpy(address.sun_path + 1, SOCKET_NAME, sizeof(SOCKET_NAME) - 1);
	const socklen_t address_length = offsetof(sockaddr_un, sun_path) + sizeof(SOCKET_NAME);
	if (connect(fd, reinterpret_cast<sockaddr *>(&address), address_length) < 0) {
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
	report += "broker_connect=PASS\n";
	ucred peer {};
	socklen_t peer_length = sizeof(peer);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_length) == 0 && peer_length == sizeof(peer)) {
		report += "broker_server_pid=" + std::to_string(peer.pid) + " uid=" + std::to_string(peer.uid) + " gid=" + std::to_string(peer.gid) + "\n";
	} else {
		report += "broker_server_credentials=<unavailable>\n";
	}

	size_t sent = 0;
	while (sent < sizeof(REQUEST) - 1) {
		const int error = wait_ready(fd, POLLOUT, deadline);
		if (error != 0) {
			return failed("send_wait", error);
		}
		const ssize_t count = send(fd, REQUEST + sent, sizeof(REQUEST) - 1 - sent, MSG_NOSIGNAL);
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
	while (received.size() < 128 && received.find('\n') == std::string::npos) {
		const int error = wait_ready(fd, POLLIN, deadline);
		if (error != 0) {
			return failed("receive_wait", error);
		}
		char buffer[128];
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
	if (received != RESPONSE) {
		return report + "broker_probe=FAIL step=protocol reason=unexpected_response bytes=" + std::to_string(received.size()) + "\n";
	}
	return report + "broker_response=OHECO_BROKER_PROBE_OK_V1\nbroker_probe=PASS\n";
}
