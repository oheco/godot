// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Godot Engine contributors.
// Standalone terminal-namespace automatic debug signing worker. No Godot API.

#include <json/json.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
namespace auto_signer {
namespace fs = std::filesystem;
using JSON = Json::Value;
using Env = std::map<std::string, std::string>;
constexpr size_t LIMIT = 4 * 1024 * 1024;
constexpr size_t TREE_LIMIT = 32 * LIMIT;
constexpr size_t EXECUTABLE_LIMIT = 128 * 1024 * 1024;
volatile sig_atomic_t interrupted = 0;

struct SafeError : std::runtime_error {
	std::string code;
	SafeError(std::string p_code, const std::string &message) : std::runtime_error(message), code(std::move(p_code)) {}
};
[[noreturn]] void fail(const std::string &code, const std::string &message) { throw SafeError(code, message); }
void need(bool ok, const std::string &code, const std::string &message) { if (!ok) fail(code, message); }
void checkInterrupted() { if (interrupted) fail("INTERRUPTED", "Automatic signing was interrupted; retry when ready."); }
void handleSignal(int) { interrupted = 1; }

struct FD {
	int fd = -1;
	explicit FD(int p_fd = -1) : fd(p_fd) {}
	~FD() { if (fd >= 0) ::close(fd); }
	FD(const FD &) = delete;
	FD &operator=(const FD &) = delete;
	FD(FD &&other) noexcept : fd(other.fd) { other.fd = -1; }
	void reset(int replacement = -1) { if (fd >= 0) ::close(fd); fd = replacement; }
};
std::string normalized(const std::string &path) {
	std::string result = fs::path(path).lexically_normal().string();
	while (result.size() > 1 && result.back() == '/') result.pop_back();
	return result;
}
bool text(const JSON &value) { return value.isString() && value.asString().size() <= 32768 && value.asString().find('\0') == std::string::npos; }
bool absolute(const std::string &path) { return !path.empty() && path[0] == '/' && path.size() <= 32768 && path.find('\0') == std::string::npos; }
bool inside(const std::string &root, const std::string &path) {
	const std::string a = normalized(root), b = normalized(path);
	return b.size() > a.size() && b.compare(0, a.size(), a) == 0 && (a == "/" || b[a.size()] == '/');
}
std::string join(const std::string &a, const std::string &b) { return (fs::path(a) / b).string(); }
std::string parent(const std::string &path) { return fs::path(path).parent_path().string(); }
bool exists(const std::string &path) {
	struct stat st{};
	if (::lstat(path.c_str(), &st) == 0) return true;
	if (errno == ENOENT) return false;
	fail("CACHE_INVALID", "A required signing path is inaccessible.");
}
std::string real(const std::string &path, const std::string &code = "CACHE_INVALID") {
	char *value = ::realpath(path.c_str(), nullptr);
	need(value != nullptr, code, "A required path is missing or inaccessible.");
	std::string result(value);
	::free(value);
	return result;
}
struct stat directory(const std::string &path, const std::string &code) {
	struct stat st{};
	need(absolute(path) && ::lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode), code, "A required directory is missing or is not a real directory.");
	need(real(path, code) == normalized(path), code, "A required directory must not traverse symbolic links.");
	return st;
}
struct stat regular(const std::string &path, const std::string &code = "CACHE_INVALID", size_t maximum = LIMIT) {
	struct stat st{};
	need(::lstat(path.c_str(), &st) == 0, code, "A required file is missing or inaccessible.");
	need(S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode) && st.st_nlink == 1 && st.st_size >= 0 && uint64_t(st.st_size) <= maximum, code, "A required file must be regular, bounded, and have exactly one link.");
	return st;
}
bool sameFile(const struct stat &a, const struct stat &b) { return a.st_dev == b.st_dev && a.st_ino == b.st_ino; }
void privateMode(const std::string &path, mode_t bits, bool dir, size_t maximum = LIMIT) {
	const std::string code = "PRIVATE_STORAGE_REQUIRED";
	struct stat before = dir ? directory(path, code) : regular(path, code, maximum);
	need(before.st_uid == ::getuid() && real(path, code) == normalized(path), code, "Signing storage must be caller-owned and must not traverse symbolic links.");
	FD fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (dir ? O_DIRECTORY : 0)));
	struct stat checked{};
	need(fd.fd >= 0 && ::fstat(fd.fd, &checked) == 0 && sameFile(before, checked) && ::fchmod(fd.fd, bits) == 0 && ::fstat(fd.fd, &checked) == 0 &&
		(checked.st_mode & 07777) == bits && checked.st_uid == ::getuid() && (dir || checked.st_nlink == 1), code,
		"Use permission-capable XDG_CONFIG_HOME and TMPDIR outside HOME (0700 directories, 0600 files).");
}
std::string privateDir(const std::string &path) {
	need(absolute(path), "PRIVATE_STORAGE_REQUIRED", "Private storage paths must be absolute.");
	if (!exists(path)) {
		if (!exists(parent(path))) privateDir(parent(path));
		directory(parent(path), "PRIVATE_STORAGE_REQUIRED");
		need(::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST, "PRIVATE_STORAGE_REQUIRED", "Cannot create a private signing directory.");
	}
	privateMode(path, 0700, true);
	return path;
}
std::string readBytes(const std::string &path, const std::string &code = "CACHE_INVALID", size_t maximum = LIMIT) {
	const struct stat before = regular(path, code, maximum);
	FD fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
	struct stat st{};
	need(fd.fd >= 0 && ::fstat(fd.fd, &st) == 0 && sameFile(before, st) && S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_size >= 0 && uint64_t(st.st_size) <= maximum,
		code, "A required file changed or could not be read safely.");
	std::string data;
	std::array<char, 16384> buffer{};
	for (;;) {
		ssize_t n = ::read(fd.fd, buffer.data(), buffer.size());
		if (n < 0 && errno == EINTR) continue;
		need(n >= 0, code, "A required file could not be read.");
		if (!n) break;
		need(data.size() + size_t(n) <= maximum, code, "A required file exceeds the supported size.");
		data.append(buffer.data(), size_t(n));
	}
	need(::fstat(fd.fd, &st) == 0 && sameFile(before, st) && st.st_nlink == 1 && st.st_size == before.st_size && data.size() == size_t(st.st_size), code, "A required file changed while it was being read.");
	return data;
}
void privateWrite(const std::string &path, const std::string &data) {
	privateDir(parent(path));
	if (exists(path)) {
		regular(path, "PRIVATE_STORAGE_REQUIRED");
		privateMode(path, 0600, false);
	}
	FD fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
	struct stat st{};
	need(fd.fd >= 0 && ::fstat(fd.fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_uid == ::getuid() &&
		::fchmod(fd.fd, 0600) == 0 && ::fstat(fd.fd, &st) == 0 && (st.st_mode & 07777) == 0600 && ::ftruncate(fd.fd, 0) == 0,
		"PRIVATE_STORAGE_REQUIRED", "Cannot write a caller-owned private signing file with working Unix permissions.");
	size_t offset = 0;
	while (offset < data.size()) {
		ssize_t n = ::write(fd.fd, data.data() + offset, data.size() - offset);
		if (n < 0 && errno == EINTR) continue;
		need(n > 0, "PRIVATE_STORAGE_REQUIRED", "Cannot complete a private signing file write.");
		offset += size_t(n);
	}
	need(::fsync(fd.fd) == 0, "PRIVATE_STORAGE_REQUIRED", "Cannot persist a private signing file.");
}
std::string jsonString(const JSON &value) {
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	builder["emitUTF8"] = true;
	return Json::writeString(builder, value);
}
JSON parseJSON(const std::string &data, bool comments = false) {
	need(data.size() <= LIMIT, "INVALID_PROJECT", "A JSON input exceeds the supported size.");
	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;
	builder["allowComments"] = comments;
	builder["allowTrailingCommas"] = comments;
	builder["strictRoot"] = true;
	builder["failIfExtra"] = true;
	builder["rejectDupKeys"] = true;
	builder["allowSpecialFloats"] = false;
	builder["allowSingleQuotes"] = false;
	builder["allowNumericKeys"] = false;
	builder["stackLimit"] = 128;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	JSON value;
	std::string errors;
	need(reader->parse(data.data(), data.data() + data.size(), &value, &errors) && value.isObject(), "INVALID_JSON", "Expected a supported JSON object.");
	return value;
}
JSON readJSON(const std::string &path, bool comments = false) { return parseJSON(readBytes(path), comments); }
void writeJSON(const std::string &path, const JSON &value) { privateWrite(path, jsonString(value) + "\n"); }
std::string digest(const std::string &data) {
	unsigned char hash[32];
	need(mbedtls_sha256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), hash, 0) == 0, "INTERNAL_ERROR", "Cannot hash signing metadata.");
	static const char hex[] = "0123456789abcdef";
	std::string result;
	for (unsigned char byte : hash) { result += hex[byte >> 4]; result += hex[byte & 15]; }
	return result;
}
std::vector<std::string> entries(const std::string &path) {
	std::vector<std::string> result;
	for (const auto &entry : fs::directory_iterator(path)) result.push_back(entry.path().filename().string());
	std::sort(result.begin(), result.end());
	return result;
}
void removeOwned(const std::string &root, const std::string &path) {
	need(absolute(root) && absolute(path) && inside(root, path) && real(root) == normalized(root), "CACHE_INVALID", "Refusing to remove a path outside the private workspace.");
	// The root and all live writable parents are caller-owned. remove_all unlinks
	// symlinks themselves; it never follows links from an untrusted CLI output.
	if (!exists(path)) return;
	directory(parent(path), "CACHE_INVALID");
	fs::remove_all(path);
}
void unlinkRegular(const std::string &path, const std::string &code = "CACHE_INVALID") {
	regular(path, code);
	need(::unlink(path.c_str()) == 0, code, "Cannot remove an obsolete private file.");
}
void copyFilePrivate(const std::string &source, const std::string &destination) { privateWrite(destination, readBytes(source)); }
struct TreeState { size_t count = 0; uint64_t bytes = 0; };
void copyTree(const std::string &source, const std::string &destination, TreeState &state) {
	directory(source, "CACHE_INVALID");
	privateDir(destination);
	for (const std::string &name : entries(source)) {
		need(++state.count <= 4096, "CACHE_INVALID", "Signing cache contains too many files.");
		const std::string from = join(source, name), to = join(destination, name);
		struct stat st{};
		need(::lstat(from.c_str(), &st) == 0 && !S_ISLNK(st.st_mode), "CACHE_INVALID", "Signing material must not contain symbolic links.");
		if (S_ISDIR(st.st_mode)) copyTree(from, to, state);
		else {
			need(st.st_size >= 0, "CACHE_INVALID", "Signing material has an invalid size.");
			state.bytes += uint64_t(st.st_size);
			need(state.bytes <= TREE_LIMIT, "CACHE_INVALID", "Signing material exceeds the supported cache size.");
			copyFilePrivate(from, to);
		}
	}
}
void copyTree(const std::string &source, const std::string &destination) { TreeState state; copyTree(source, destination, state); }
void hardenTree(const std::string &root, size_t &count) {
	privateMode(root, 0700, true);
	for (const auto &name : entries(root)) {
		need(++count <= 4096, "PRIVATE_STORAGE_REQUIRED", "The private CLI home exceeds the supported file count.");
		const std::string path = join(root, name);
		struct stat st{};
		need(::lstat(path.c_str(), &st) == 0, "PRIVATE_STORAGE_REQUIRED", "A private CLI file is inaccessible.");
		if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) hardenTree(path, count);
		else privateMode(path, 0600, false);
	}
}
void hardenTree(const std::string &root) { size_t count = 0; hardenTree(root, count); }
struct Lock {
	std::string path;
	struct stat identity{};
	bool owned = false;
	explicit Lock(const std::string &p_path) : path(p_path) {
		if (::mkdir(path.c_str(), 0700) != 0) {
			if (errno == EEXIST) fail("CACHE_BUSY", "Another signing operation is active; retry after it finishes. Remove a stale private lock only after checking no helper is running.");
			fail("PRIVATE_STORAGE_REQUIRED", "Cannot lock the private signing cache.");
		}
		owned = ::lstat(path.c_str(), &identity) == 0;
		try { privateMode(path, 0700, true); } catch (...) { release(); throw; }
	}
	Lock(const Lock &) = delete;
	Lock &operator=(const Lock &) = delete;
	~Lock() { release(); }
	void release() noexcept {
		struct stat st{};
		if (owned && ::lstat(path.c_str(), &st) == 0 && sameFile(identity, st) && S_ISDIR(st.st_mode) && st.st_uid == ::getuid()) ::rmdir(path.c_str());
		owned = false;
	}
};
struct Temporary {
	std::string root, path;
	bool retainedForRecovery = false;
	explicit Temporary(const std::string &p_root) : root(p_root) {
		std::string pattern = join(root, "godot-auto-sign-XXXXXX");
		std::vector<char> bytes(pattern.begin(), pattern.end()); bytes.push_back(0);
		char *created = ::mkdtemp(bytes.data());
		need(created != nullptr, "PRIVATE_STORAGE_REQUIRED", "Cannot create a private temporary signing directory.");
		path = created;
		try { privateMode(path, 0700, true); } catch (...) { cleanup(); throw; }
	}
	void finish() { if (!path.empty()) { removeOwned(root, path); path.clear(); } }
	void cleanup() noexcept { if (!retainedForRecovery) { try { finish(); } catch (...) {} } }
	~Temporary() { cleanup(); }
};
Env environment() {
	Env result;
	for (char **item = environ; item && *item; ++item) {
		std::string entry(*item); const size_t pos = entry.find('=');
		if (pos != std::string::npos) result.emplace(entry.substr(0, pos), entry.substr(pos + 1));
	}
	return result;
}
std::string envValue(const Env &env, const std::string &key) { auto it = env.find(key); return it == env.end() ? "" : it->second; }
Env cleanEnvironment(Env env) {
	for (const char *name : {"NODE_OPTIONS", "NODE_PATH", "NODE_OPTS", "DEVECO_CLI_AUTH_SOURCE"}) env.erase(name);
	for (auto it = env.begin(); it != env.end();) { if (it->first.compare(0, 16, "DEVECO_CODE_AUTH") == 0 || it->first == "DEVECO_CODE_AUTH") it = env.erase(it); else ++it; }
	env["NO_COLOR"] = "1"; env["FORCE_COLOR"] = "0"; env["TERM"] = "dumb";
	return env;
}
struct Result { bool ok = false, missing = false, timedOut = false, overflow = false; int execError = 0, exitCode = -1; std::string out, err; };
using Clock = std::chrono::steady_clock;
struct ChildReaper {
	pid_t pid;
	bool reaped = false;
	explicit ChildReaper(pid_t p_pid) : pid(p_pid) {}
	~ChildReaper() {
		if (reaped) return;
		::kill(-pid, SIGTERM); ::kill(pid, SIGTERM);
		const auto deadline = Clock::now() + std::chrono::seconds(5);
		int status;
		// Keep the direct PID reserved until the entire group has been signalled;
		// its early exit says nothing about grandchildren with redirected stdio.
		while (Clock::now() < deadline) (void)::poll(nullptr, 0, 20);
		::kill(-pid, SIGKILL); ::kill(pid, SIGKILL);
		const auto finalDrain = Clock::now() + std::chrono::seconds(1);
		while (Clock::now() < finalDrain) (void)::poll(nullptr, 0, 20);
		while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	}
};
void pipePair(int (&pipefd)[2]) {
	need(::pipe(pipefd) == 0, "INTERNAL_ERROR", "Cannot create a child process channel.");
	for (int &fd : pipefd) {
		if (fd < 3) {
			const int replacement = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
			if (replacement < 0) { ::close(pipefd[0]); ::close(pipefd[1]); fail("INTERNAL_ERROR", "Cannot protect a child process channel."); }
			::close(fd); fd = replacement;
		}
		if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) { ::close(pipefd[0]); ::close(pipefd[1]); fail("INTERNAL_ERROR", "Cannot protect a child process channel."); }
	}
}
Result execute(const std::string &file, const std::vector<std::string> &args, const Env &env, const std::string &cwd = "", int timeoutMs = 20000) {
	checkInterrupted();
	std::vector<std::string> argvStorage; argvStorage.push_back(file); argvStorage.insert(argvStorage.end(), args.begin(), args.end());
	std::vector<char *> argv; for (auto &arg : argvStorage) argv.push_back(arg.data()); argv.push_back(nullptr);
	std::vector<std::string> envStorage; for (const auto &entry : env) envStorage.push_back(entry.first + "=" + entry.second);
	std::vector<char *> envp; for (auto &entry : envStorage) envp.push_back(entry.data()); envp.push_back(nullptr);
	int outPipe[2], errPipe[2], execPipe[2];
	pipePair(outPipe); FD outRead(outPipe[0]), outWrite(outPipe[1]);
	pipePair(errPipe); FD errRead(errPipe[0]), errWrite(errPipe[1]);
	pipePair(execPipe); FD execRead(execPipe[0]), execWrite(execPipe[1]);
	const pid_t pid = ::fork();
	need(pid >= 0, "INTERNAL_ERROR", "Cannot start a signing subprocess.");
	if (pid == 0) {
		::signal(SIGINT, SIG_DFL); ::signal(SIGTERM, SIG_DFL); ::signal(SIGPIPE, SIG_DFL);
		::setpgid(0, 0);
		int nullfd = ::open("/dev/null", O_RDONLY);
		int error = 0;
		if (nullfd < 0 || ::dup2(nullfd, STDIN_FILENO) < 0 || ::dup2(outWrite.fd, STDOUT_FILENO) < 0 || ::dup2(errWrite.fd, STDERR_FILENO) < 0) error = errno;
		if (nullfd > STDERR_FILENO) ::close(nullfd);
		::close(outRead.fd); ::close(outWrite.fd); ::close(errRead.fd); ::close(errWrite.fd); ::close(execRead.fd);
		if (!error && !cwd.empty() && ::chdir(cwd.c_str()) != 0) error = errno;
		if (!error) { ::execve(file.c_str(), argv.data(), envp.data()); error = errno; }
		(void)::write(execWrite.fd, &error, sizeof(error));
		::_exit(127);
	}
	ChildReaper child(pid);
	::setpgid(pid, pid);
	outWrite.reset(); errWrite.reset(); execWrite.reset();
	for (int fd : {outRead.fd, errRead.fd, execRead.fd}) {
		const int flags = ::fcntl(fd, F_GETFL);
		need(flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0, "INTERNAL_ERROR", "Cannot configure bounded child process capture.");
	}
	Result result;
	const auto start = Clock::now();
	bool terminated = false, killed = false, exited = false;
	auto terminationTime = start;
	int status = 0;
	std::array<char, 16384> buffer{};
	for (;;) {
		const auto now = Clock::now();
		if (!terminated && (interrupted || now - start >= std::chrono::milliseconds(timeoutMs) || result.overflow)) {
			result.timedOut = !interrupted && !result.overflow;
			::kill(-pid, SIGTERM); ::kill(pid, SIGTERM);
			terminated = true; terminationTime = now;
		}
		if (terminated && !killed && now - terminationTime >= std::chrono::seconds(5)) { ::kill(-pid, SIGKILL); ::kill(pid, SIGKILL); killed = true; }
		// A descendant retaining stdout must not keep a finished helper alive.
		if (exited && !terminated && (outRead.fd >= 0 || errRead.fd >= 0) && now - start >= std::chrono::milliseconds(timeoutMs)) {
			::kill(-pid, SIGTERM); terminated = true; terminationTime = now; result.timedOut = true;
		}
		struct pollfd polls[3] = {{outRead.fd, POLLIN, 0}, {errRead.fd, POLLIN, 0}, {execRead.fd, POLLIN, 0}};
		(void)::poll(polls, 3, 50);
		for (size_t i = 0; i < 3; ++i) {
			FD *fd = i == 0 ? &outRead : i == 1 ? &errRead : &execRead;
			if (fd->fd < 0) continue;
			// Bound each drain so continuous output cannot starve signal/timeouts.
			for (size_t reads = 0; reads < 16; ++reads) {
				ssize_t n = ::read(fd->fd, buffer.data(), buffer.size());
				if (n < 0 && errno == EINTR) continue;
				if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
				if (n <= 0) { fd->reset(); break; }
				if (i == 2) { if (size_t(n) == sizeof(int)) std::memcpy(&result.execError, buffer.data(), sizeof(int)); }
				else {
					std::string &target = i == 0 ? result.out : result.err;
					if (target.size() + size_t(n) > LIMIT) result.overflow = true;
					else if (!result.overflow) target.append(buffer.data(), size_t(n));
				}
			}
		}
		if (!exited) {
			siginfo_t info{};
			const int observed = ::waitid(P_PID, pid, &info, WEXITED | WNOHANG | WNOWAIT);
			need(observed == 0 || errno == EINTR, "INTERNAL_ERROR", "Cannot observe a signing subprocess safely.");
			if (observed == 0 && info.si_pid == pid) exited = true;
		}
		const bool closed = outRead.fd < 0 && errRead.fd < 0 && execRead.fd < 0;
		if (exited && closed && !terminated) break;
		// Cancellation must still kill remaining group members even when the
		// direct child has already exited and nobody holds its pipes open.
		if (exited && killed && Clock::now() - terminationTime >= std::chrono::seconds(6)) break;
	}
	while (::waitpid(pid, &status, 0) < 0) {
		if (errno == EINTR) continue;
		fail("INTERNAL_ERROR", "Cannot reap a signing subprocess safely.");
	}
	child.reaped = true;
	checkInterrupted();
	result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	result.missing = result.execError == ENOENT;
	result.ok = result.exitCode == 0 && !result.execError && !result.timedOut && !result.overflow;
	return result;
}

const std::array<const char *, 13> REQUEST_KEYS = {{"schemaVersion", "projectDir", "projectIdentity", "nodePath", "commandLineToolsRoot", "sdkRoot", "hdcPath", "target", "format", "buildMode", "cliPath", "teamId", "loginDataDir"}};
std::string field(const JSON &request, const char *key) { return request[key].asString(); }
void validateRequest(const JSON &request) {
	need(request.isObject() && request.size() == REQUEST_KEYS.size(), "INVALID_REQUEST", "Missing or unknown automatic signing request fields.");
	for (const char *key : REQUEST_KEYS) {
		need(request.isMember(key), "INVALID_REQUEST", "Missing or unknown automatic signing request fields.");
		if (std::strcmp(key, "schemaVersion") != 0) need(text(request[key]), "INVALID_REQUEST", "Request fields must be bounded strings without NUL.");
	}
	need(request["schemaVersion"].isInt() && request["schemaVersion"].asInt() == 1, "INVALID_REQUEST", "Automatic signing requires schemaVersion 1.");
	for (const char *key : {"projectDir", "projectIdentity", "nodePath", "commandLineToolsRoot", "sdkRoot", "hdcPath"}) need(absolute(field(request, key)), "INVALID_REQUEST", "Project and tool paths must be absolute.");
	for (const char *key : {"cliPath", "loginDataDir"}) need(field(request, key).empty() || absolute(field(request, key)), "INVALID_REQUEST", "Optional tool and login paths must be absolute.");
	const std::string target = field(request, "target"), team = field(request, "teamId");
	need(!target.empty() && target.find_first_of("\r\n") == std::string::npos, "INVALID_REQUEST", "Select a target device.");
	need(field(request, "format") == "hap" && field(request, "buildMode") == "debug", "INVALID_REQUEST", "Automatic signing supports debug HAP builds only.");
	need(team.empty() || std::regex_match(team, std::regex("[A-Za-z0-9_-]{1,256}")), "INVALID_REQUEST", "The selected team ID is invalid.");
	directory(field(request, "projectDir"), "INVALID_PROJECT");
	directory(field(request, "commandLineToolsRoot"), "CLT_MISSING");
	directory(real(field(request, "sdkRoot"), "CLT_MISSING"), "CLT_MISSING");
}
Env nodeEnvironment(const JSON &request, Env env, const std::string &workDir) {
	const std::string source = join(field(request, "commandLineToolsRoot"), "sdk/default/openharmony/native/llvm/lib/aarch64-linux-ohos/libc++_shared.so");
	struct stat st{};
	if (::stat(source.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
		// Node needs only this SONAME. Exposing the compiler library directory also
		// shadows the system libc++.so used by HDC and other SDK descendants.
		const std::string target = real(source, "CLT_MISSING");
		const std::string runtime = privateDir(join(workDir, "node-runtime"));
		const std::string link = join(runtime, "libc++_shared.so");
		if (!exists(link)) need(::symlink(target.c_str(), link.c_str()) == 0, "PRIVATE_STORAGE_REQUIRED", "Cannot prepare the private Node runtime.");
		std::vector<char> linked(target.size() + 1);
		const ssize_t length = ::readlink(link.c_str(), linked.data(), linked.size());
		need(::lstat(link.c_str(), &st) == 0 && S_ISLNK(st.st_mode) && st.st_uid == ::getuid() && length >= 0 && size_t(length) == target.size() &&
			std::string(linked.data(), size_t(length)) == target && entries(runtime) == std::vector<std::string>{"libc++_shared.so"},
			"PRIVATE_STORAGE_REQUIRED", "The private Node runtime must contain only the selected Command Line Tools runtime link.");
		const std::string old = envValue(env, "LD_LIBRARY_PATH");
		env["LD_LIBRARY_PATH"] = runtime + (old.empty() ? "" : ":" + old);
	}
	return env;
}
void nodeCheck(const JSON &request, const Env &env, const std::string &workDir) {
	const Result result = execute(field(request, "nodePath"), {"--version"}, nodeEnvironment(request, env, workDir));
	need(!result.missing, "NODE_MISSING", "Select an installed Node.js 22 or newer in the editor toolchain settings.");
	std::smatch match;
	need(result.ok && result.out.size() <= 128 && std::regex_match(result.out, match, std::regex("v([0-9]+)\\.[0-9]+\\.[0-9]+[\\r\\n\\t ]*")), "NODE_MISSING", "The selected Node.js executable cannot report its version.");
	need(match[1].str().size() <= 8 && std::stoul(match[1].str()) >= 22, "NODE_VERSION_TOO_OLD", "Automatic signing requires Node.js 22 or newer.");
}
std::string selfPath() {
	std::vector<char> buffer(32769);
	const ssize_t n = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
	need(n > 0 && size_t(n) < buffer.size() - 1, "INTERNAL_ERROR", "Cannot locate the native signing worker executable.");
	return real(std::string(buffer.data(), size_t(n)), "INTERNAL_ERROR");
}
std::string discoverCLI(const JSON &request, const Env &env) {
	std::vector<std::string> candidates;
	if (!field(request, "cliPath").empty()) candidates.push_back(field(request, "cliPath"));
	else {
		const std::string path = envValue(env, "PATH");
		size_t offset = 0;
		while (offset <= path.size()) {
			const size_t end = path.find(':', offset);
			const std::string part = path.substr(offset, end == std::string::npos ? end : end - offset);
			if (!part.empty()) candidates.push_back(fs::absolute(join(part, "devecocli")).string());
			if (end == std::string::npos) break;
			offset = end + 1;
		}
		candidates.push_back(join(parent(parent(field(request, "nodePath"))), "lib/node_modules/@deveco/deveco-cli/cli.js"));
		try { candidates.push_back(join(parent(real(join(envValue(env, "HOME"), ".oheco/bin/node"))), "devecocli")); } catch (const SafeError &) {}
		// In the JS helper process.execPath was the selected Node, not the game
		// template directory containing this native worker.
		try { candidates.push_back(join(parent(real(field(request, "nodePath"))), "devecocli")); } catch (const SafeError &) {}
	}
	for (const auto &candidate : candidates) {
		try { const std::string resolved = real(candidate); struct stat st{}; if (::stat(resolved.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return resolved; } catch (const SafeError &) {}
	}
	fail("CLI_MISSING", "Install the official CLI with npm install -g @deveco/deveco-cli, or set its entry file in EditorSettings.");
}
std::string discoverVerifier(const JSON &request) {
	std::vector<std::string> candidates;
	for (const auto &root : {join(field(request, "commandLineToolsRoot"), "sdk"), field(request, "sdkRoot")}) {
		candidates.push_back(join(root, "default/openharmony/toolchains/lib/hap-sign-tool"));
		candidates.push_back(join(root, "openharmony/toolchains/lib/hap-sign-tool"));
		candidates.push_back(join(root, "toolchains/lib/hap-sign-tool"));
		try { for (const auto &name : entries(root)) if (std::regex_match(name, std::regex("[0-9.]+"))) candidates.push_back(join(join(root, name), "toolchains/lib/hap-sign-tool")); } catch (const fs::filesystem_error &) {}
	}
	for (const auto &candidate : candidates) {
		struct stat st{};
		if (::stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return real(candidate, "CLT_MISSING");
	}
	fail("CLT_MISSING", "The selected SDK must provide the native toolchains/lib/hap-sign-tool profile verifier.");
}

const JSON &member(const JSON &value, const char *key) {
	static const JSON empty;
	return value.isObject() ? value[key] : empty;
}
bool equals(const JSON &value, const char *expected) { return value.isString() && value.asString() == expected; }
JSON stringArray(const std::vector<std::string> &values) {
	JSON result(Json::arrayValue); for (const auto &value : values) result.append(value); return result;
}
int64_t apiLevel(const JSON &value) {
	if (value.isIntegral() && value.isInt64() && value.asInt64() > 0 && value.asInt64() <= 9007199254740991LL) return value.asInt64();
	if (value.isString() && value.asString().size() <= 64 && std::regex_match(value.asString(), std::regex("[0-9]+(\\.[0-9]+){0,3}"))) {
		const std::string major = value.asString().substr(0, value.asString().find('.'));
		try { size_t consumed = 0; const uint64_t result = std::stoull(major, &consumed); if (consumed == major.size() && result > 0 && result <= 9007199254740991ULL) return int64_t(result); } catch (const std::exception &) {}
	}
	fail("ACL_METADATA_INVALID", "The SDK permission API metadata is unsupported. Select a complete compatible Command Line Tools SDK.");
}
struct Metadata {
	std::string bundle, aclMetadata;
	std::vector<std::string> permissions, requiredACL;
	JSON rootProfile, app, module;
};
void requiredACL(const JSON &request, Metadata &metadata) {
	JSON definitions, descriptor;
	const std::string sdk = join(field(request, "commandLineToolsRoot"), "sdk/default");
	try {
		definitions = readJSON(join(sdk, "openharmony/toolchains/lib/permissionDefinitions.json"));
		descriptor = readJSON(join(sdk, "sdk-pkg.json"));
	} catch (const SafeError &) { fail("ACL_METADATA_INVALID", "The selected Command Line Tools SDK is missing valid permissionDefinitions.json or sdk-pkg.json."); }
	const int64_t api = apiLevel(member(member(descriptor, "data"), "apiVersion"));
	const JSON &permissions = definitions["definePermissions"];
	need(permissions.isArray() && permissions.size() <= 20000, "ACL_METADATA_INVALID", "The SDK permission definitions are invalid. Select a complete compatible SDK.");
	std::map<std::string, JSON> index;
	for (const auto &definition : permissions) {
		const JSON &name = member(definition, "name");
		need(definition.isObject() && name.isString() && index.emplace(name.asString(), definition).second, "ACL_METADATA_INVALID", "The SDK permission definitions contain malformed or duplicate names.");
	}
	const std::set<std::string> includes = {"ohos.permission.FILE_ACCESS_PERSIST", "ohos.permission.READ_WRITE_DOWNLOAD_DIRECTORY", "ohos.permission.READ_WRITE_DOCUMENTS_DIRECTORY"};
	const std::set<std::string> excludes = {"ohos.permission.READ_DOCUMENT", "ohos.permission.WRITE_DOCUMENT"};
	for (const auto &name : metadata.permissions) {
		auto it = index.find(name);
		need(it != index.end(), "ACL_METADATA_INVALID", "A requested permission has no complete SDK definition. Check the selected SDK.");
		const JSON &definition = it->second;
		need(definition["availableLevel"].isString() && definition["availableType"].isString() && definition["provisionEnable"].isBool(), "ACL_METADATA_INVALID", "A requested permission has no complete SDK definition. Check the selected SDK.");
		const int64_t since = apiLevel(definition.isMember("since") && !definition["since"].isNull() ? definition["since"] : definition["minSupportApiLevel"]);
		const int64_t minimum = definition.isMember("minSupportApiLevel") ? apiLevel(definition["minSupportApiLevel"]) : since;
		need(std::max(since, minimum) <= api, "ACL_UNSUPPORTED", "A requested permission requires a newer SDK API. Update the selected SDK or remove that permission.");
		if (!excludes.count(name) && (includes.count(name) || (equals(definition["availableLevel"], "system_basic") && equals(definition["availableType"], "NORMAL") && definition["provisionEnable"].asBool()))) metadata.requiredACL.push_back(name);
	}
	JSON fingerprint(Json::arrayValue); fingerprint.append(descriptor); fingerprint.append(definitions);
	metadata.aclMetadata = digest(jsonString(fingerprint));
}
bool bundleName(const std::string &value) {
	bool first = true, dotted = false;
	for (unsigned char ch : value) {
		if (ch == '.') { if (first) return false; first = true; dotted = true; continue; }
		const bool letter = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
		if (!(letter || (!first && ((ch >= '0' && ch <= '9') || ch == '_')))) return false;
		first = false;
	}
	return dotted && !first;
}
Metadata projectMetadata(const JSON &request) {
	Metadata metadata;
	const std::string project = field(request, "projectDir");
	metadata.rootProfile = readJSON(join(project, "build-profile.json5"), true);
	metadata.app = readJSON(join(project, "AppScope/app.json5"), true);
	const JSON &app = member(metadata.app, "app"), &bundle = member(app, "bundleName");
	need(app.isObject() && bundle.isString() && bundleName(bundle.asString()), "INVALID_PROJECT", "The exported project has no supported bundle name.");
	metadata.bundle = bundle.asString();
	const JSON &profileApp = member(metadata.rootProfile, "app"), &products = member(profileApp, "products"), &modules = metadata.rootProfile["modules"];
	need(profileApp.isObject() && products.isArray(), "INVALID_PROJECT", "Automatic signing requires the generated default product and entry module.");
	size_t defaults = 0;
	for (const auto &product : products) {
		need(product.isObject() && member(product, "name").isString(), "INVALID_PROJECT", "Automatic signing requires the generated default product and entry module.");
		if (equals(product["name"], "default")) ++defaults;
	}
	need(defaults == 1 && modules.isArray() && modules.size() == 1 && modules[0].isObject() && equals(modules[0]["name"], "entry") && (equals(modules[0]["srcPath"], "entry") || equals(modules[0]["srcPath"], "./entry")), "INVALID_PROJECT", "Automatic signing requires the generated default product and entry module.");
	metadata.module = readJSON(join(project, "entry/src/main/module.json5"), true);
	const JSON &module = member(metadata.module, "module"), &permissions = member(module, "requestPermissions");
	need(module.isObject() && (!module.isMember("requestPermissions") || permissions.isArray()), "INVALID_PROJECT", "The entry permission configuration is invalid.");
	std::set<std::string> names;
	if (permissions.isArray()) for (const auto &permission : permissions) {
		const JSON &name = member(permission, "name");
		need(permission.isObject() && name.isString() && !name.asString().empty() && name.asString().find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.") == std::string::npos, "INVALID_PROJECT", "A requested permission is invalid.");
		names.insert(name.asString());
	}
	metadata.permissions.assign(names.begin(), names.end());
	requiredACL(request, metadata);
	return metadata;
}
struct Context {
	JSON request;
	Env originalEnv, env;
	std::string originalHome, root, cliHome, cliData, work, cli, verifier, team, udid, cache, minimal, overlay;
	Metadata metadata;
	Temporary *temporary = nullptr;
};
JSON sourceFingerprint(const std::string &path) {
	if (!exists(path)) return JSON();
	const struct stat st = regular(path, "LOGIN_DATA_INVALID");
	JSON result(Json::objectValue);
	result["size"] = Json::Int64(st.st_size);
	result["mtime"] = double(st.st_mtim.tv_sec) * 1000 + double(st.st_mtim.tv_nsec) / 1000000;
	result["sha256"] = digest(readBytes(path, "LOGIN_DATA_INVALID"));
	return result;
}
void syncLogin(Context &ctx) {
	std::string login = field(ctx.request, "loginDataDir");
	if (login.empty()) login = envValue(ctx.originalEnv, "DEVECO_CLI_DATA_DIR");
	if (login.empty()) login = join(ctx.originalHome, ".config/deveco-cli");
	need(absolute(login), "LOGIN_DATA_INVALID", "The original CLI login data directory must be absolute.");
	need(normalized(login) != ctx.root && !inside(ctx.root, login), "LOGIN_DATA_INVALID", "The source CLI login must be outside the worker's private signing cache.");
	std::vector<std::pair<std::string, std::string>> files;
	for (const char *name : {"token.enc", "token.dek"}) files.emplace_back(join(login, name), join(ctx.cliData, name));
	for (int n = 1; n <= 3; ++n) {
		const std::string relative = ".local/share/deveco-cli/keys/kek-v" + std::to_string(n) + ".bin";
		files.emplace_back(join(ctx.originalHome, relative), join(ctx.cliHome, relative));
	}
	for (const char *name : {"hdckey", "hdckey.key", "hdckey.pub"}) files.emplace_back(join(ctx.originalHome, std::string(".harmony/") + name), join(ctx.cliHome, std::string(".harmony/") + name));
	JSON metadata(Json::objectValue); metadata["source"] = login; metadata["originalHome"] = ctx.originalHome; metadata["files"] = JSON(Json::arrayValue);
	for (const auto &file : files) metadata["files"].append(sourceFingerprint(file.first));
	const std::string metaPath = join(ctx.root, "login-source.json");
	if (exists(metaPath)) { privateMode(metaPath, 0600, false); if (readJSON(metaPath) == metadata) return; }
	for (size_t index = 0; index < files.size(); ++index) {
		if (!metadata["files"][Json::ArrayIndex(index)].isNull()) copyFilePrivate(files[index].first, files[index].second);
		else if (exists(files[index].second)) unlinkRegular(files[index].second);
	}
	writeJSON(metaPath, metadata);
}
void linkSiblings(const std::string &source, const std::string &destination, const std::set<std::string> &except) {
	directory(real(source), "CLT_MISSING"); privateDir(destination);
	for (const auto &name : entries(source)) if (!except.count(name)) {
		const std::string from = join(source, name), to = join(destination, name);
		need(::symlink(from.c_str(), to.c_str()) == 0, "PRIVATE_STORAGE_REQUIRED", "Cannot prepare the private Command Line Tools overlay.");
	}
}
void copyToolBin(const std::string &source, const std::string &destination, const std::string &entry) {
	linkSiblings(source, destination, {"bin"});
	const std::string fromBin = join(source, "bin"), toBin = privateDir(join(destination, "bin"));
	directory(real(fromBin), "CLT_MISSING");
	regular(join(fromBin, entry), "CLT_MISSING", EXECUTABLE_LIMIT);
	for (const auto &name : entries(fromBin)) {
		checkInterrupted();
		const std::string from = join(fromBin, name), to = join(toBin, name);
		struct stat st{};
		need(::lstat(from.c_str(), &st) == 0, "CLT_MISSING", "A required Command Line Tools launcher is inaccessible.");
		if (S_ISREG(st.st_mode)) {
			// The official CLI realpaths its explicit entries. Copy bin siblings
			// too, preserving relative imports and signed ELF bytes unchanged.
			privateWrite(to, readBytes(from, "CLT_MISSING", EXECUTABLE_LIMIT));
			privateMode(to, (entry == "node" && name == entry) || (st.st_mode & 0111) ? 0700 : 0600, false, EXECUTABLE_LIMIT);
		} else {
			need(::symlink(from.c_str(), to.c_str()) == 0, "PRIVATE_STORAGE_REQUIRED", "Cannot prepare the private Command Line Tools launcher siblings.");
		}
	}
}
std::string prepareOverlay(Context &ctx) {
	if (!ctx.overlay.empty()) return ctx.overlay;
	const std::string original = field(ctx.request, "commandLineToolsRoot");
	const std::string overlay = privateDir(join(ctx.work, "clt-overlay"));
	linkSiblings(original, overlay, {"sdk", "node", "ohpm", "hvigor"});
	for (const auto &tool : {std::make_pair("node", "node"), std::make_pair("ohpm", "pm-cli.js"), std::make_pair("hvigor", "hvigorw.js")}) {
		copyToolBin(join(original, tool.first), join(overlay, tool.first), tool.second);
	}
	linkSiblings(join(original, "sdk"), join(overlay, "sdk"), {"default"});
	linkSiblings(join(original, "sdk/default"), join(overlay, "sdk/default"), {"openharmony"});
	linkSiblings(join(original, "sdk/default/openharmony"), join(overlay, "sdk/default/openharmony"), {"toolchains"});
	const std::string toolchains = join(overlay, "sdk/default/openharmony/toolchains");
	linkSiblings(join(original, "sdk/default/openharmony/toolchains"), toolchains, {"hdc"});
	// Copy the already signed executable without modifying any bytes. A native
	// executable cannot be replaced with a script under the broker's namespace.
	const std::string proxy = join(toolchains, "hdc");
	privateWrite(proxy, readBytes(selfPath(), "INTERNAL_ERROR", EXECUTABLE_LIMIT));
	struct stat st{};
	need(::chmod(proxy.c_str(), 0700) == 0 && ::lstat(proxy.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_uid == ::getuid() && (st.st_mode & 07777) == 0700, "PRIVATE_STORAGE_REQUIRED", "Cannot prepare the private native HDC proxy executable.");
	JSON config(Json::objectValue); config["schemaVersion"] = 1; config["hdcPath"] = real(field(ctx.request, "hdcPath"), "DEVICE_UNAVAILABLE");
	config["hostEnvironment"] = JSON(Json::objectValue); config["hostEnvironment"]["HOME"] = ctx.originalHome;
	for (const char *key : {"HDC_SERVER", "OHOS_HDC_SERVER_PORT"}) {
		auto it = ctx.originalEnv.find(key); if (it != ctx.originalEnv.end()) config["hostEnvironment"][key] = it->second;
	}
	need(real(proxy) != config["hdcPath"].asString(), "DEVICE_UNAVAILABLE", "The selected HDC executable must not be the private proxy.");
	writeJSON(join(toolchains, ".godot-hdc-host.json"), config);
	ctx.overlay = overlay;
	return overlay;
}
Env cliEnvironment(Context &ctx) {
	Env env = ctx.env;
	env["HOME"] = ctx.cliHome;
	env["XDG_CONFIG_HOME"] = privateDir(join(ctx.cliHome, ".config")); env["XDG_DATA_HOME"] = privateDir(join(ctx.cliHome, ".local/share"));
	env["XDG_CACHE_HOME"] = privateDir(join(ctx.cliHome, ".cache")); env["XDG_STATE_HOME"] = privateDir(join(ctx.cliHome, ".local/state"));
	env["DEVECO_CLI_DATA_DIR"] = ctx.cliData;
	env["TMPDIR"] = ctx.work; env["TMP"] = ctx.work; env["TEMP"] = ctx.work;
	env["COMMAND_LINE_TOOL_PATH"] = prepareOverlay(ctx);
	const std::string oldPath = envValue(ctx.env, "PATH");
	env["PATH"] = parent(field(ctx.request, "nodePath")) + (oldPath.empty() ? "" : ":" + oldPath);
	return env;
}
Result cli(Context &ctx, const std::vector<std::string> &args) {
	std::vector<std::string> argv{ctx.cli}; argv.insert(argv.end(), args.begin(), args.end());
	try {
		Result result = execute(field(ctx.request, "nodePath"), argv, nodeEnvironment(ctx.request, cliEnvironment(ctx), ctx.work), ctx.minimal.empty() ? field(ctx.request, "projectDir") : ctx.minimal, !args.empty() && args[0] == "signature" ? 120000 : 20000);
		hardenTree(ctx.cliHome);
		return result;
	} catch (...) { hardenTree(ctx.cliHome); throw; }
}
bool networkFailure(const Result &result) { return std::regex_search(result.out + result.err, std::regex("ENOTFOUND|ECONNREFUSED|ECONNRESET|ETIMEDOUT|network|fetch failed|proxy|timeout", std::regex::icase)); }
void authenticate(Context &ctx) {
	const Result result = cli(ctx, {"auth", "status"});
	need(!std::regex_search(result.out + result.err, std::regex("Not logged in", std::regex::icase)), "LOGIN_REQUIRED", "Run devecocli auth login outside the editor, then retry. The helper never logs in.");
	if (!result.ok && networkFailure(result)) fail("NETWORK_ERROR", "The CLI cannot reach its service; check the network/proxy and retry.");
	bool verified = false;
	std::istringstream lines(result.out); std::string line;
	while (std::getline(lines, line)) {
		const size_t start = line.find_first_not_of(" \t\r\v\f");
		if (start == std::string::npos || line.compare(start, 13, "Current user:") != 0) continue;
		const size_t user = line.find_first_not_of(" \t\r\v\f", start + 13);
		if (user != std::string::npos) { verified = true; break; }
	}
	need(result.ok && verified, "LOGIN_REQUIRED", "No verified CLI login was found. Run devecocli auth login outside the editor, then retry.");
}
std::vector<std::string> parseTeams(const std::string &rawOutput) {
	std::string output; output.reserve(rawOutput.size());
	for (size_t at = 0; at < rawOutput.size();) {
		if (rawOutput[at] == '\x1b' && at + 1 < rawOutput.size() && rawOutput[at + 1] == '[') {
			size_t end = at + 2;
			while (end < rawOutput.size() && rawOutput[end] >= '0' && rawOutput[end] <= '?') ++end;
			while (end < rawOutput.size() && rawOutput[end] >= ' ' && rawOutput[end] <= '/') ++end;
			if (end < rawOutput.size() && rawOutput[end] >= '@' && rawOutput[end] <= '~') { at = end + 1; continue; }
		}
		output.push_back(rawOutput[at++]);
	}
	std::vector<std::string> ids; bool table = false;
	std::istringstream lines(output); std::string line;
	while (std::getline(lines, line)) {
		if (line.find("└") != std::string::npos || line.find("╰") != std::string::npos) { table = false; continue; }
		for (const std::string &delimiter : {std::string("│"), std::string("┃"), std::string("|")}) {
			size_t at = 0; while ((at = line.find(delimiter, at)) != std::string::npos) line.replace(at, delimiter.size(), " ");
		}
		if (std::all_of(line.begin(), line.end(), [](unsigned char ch) { return std::isspace(ch) || ch == '-' || ch == '+' || ch == '=' || ch >= 0x80; })) continue;
		std::istringstream fields(line); std::string id, name; fields >> id >> name;
		if (std::regex_match(id, std::regex("Id", std::regex::icase)) && std::regex_match(name, std::regex("Name", std::regex::icase))) { table = true; continue; }
		if (table && !name.empty() && std::regex_match(id, std::regex("[A-Za-z0-9_-]{1,256}")) && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
	}
	return ids;
}
std::string selectTeam(Context &ctx) {
	if (!field(ctx.request, "teamId").empty()) return field(ctx.request, "teamId");
	const Result result = cli(ctx, {"auth", "team", "list"});
	if (!result.ok && networkFailure(result)) fail("NETWORK_ERROR", "Cannot list teams; check the network/proxy and retry.");
	need(result.ok, "TEAM_REQUIRED", "Cannot list teams. Set the signing team ID in EditorSettings.");
	const auto teams = parseTeams(result.out);
	need(teams.size() == 1, "TEAM_REQUIRED", "Set the signing team ID in EditorSettings; automatic selection requires exactly one team.");
	return teams[0];
}
std::string deviceUDID(Context &ctx) {
	const Result result = execute(field(ctx.request, "hdcPath"), {"-t", field(ctx.request, "target"), "shell", "bm", "get", "-u"}, ctx.env);
	if (std::regex_search(result.out + result.err, std::regex("unauthori[sz]ed|not authori[sz]ed|authorize|authentication fail", std::regex::icase))) fail("DEVICE_UNAUTHORIZED", "Authorize the selected HDC device and retry.");
	std::set<std::string> matches;
	const std::regex pattern("\\b[0-9a-fA-F]{64}\\b");
	for (auto it = std::sregex_iterator(result.out.begin(), result.out.end(), pattern); it != std::sregex_iterator(); ++it) {
		std::string value = it->str(); std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return char(std::tolower(ch)); }); matches.insert(value);
	}
	need(result.ok && matches.size() == 1, "DEVICE_UNAVAILABLE", "Cannot read the selected device UDID with hdc shell bm get -u; check its connection and authorization.");
	return *matches.begin();
}
bool frame(const JSON &value) {
	if (!value.isString()) return false;
	const std::string hex = value.asString();
	if (hex.size() < 64 || hex.size() > LIMIT || hex.size() % 2 || !std::all_of(hex.begin(), hex.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; })) return false;
	uint32_t length = 0;
	for (size_t i = 0; i < 8; ++i) { const char ch = hex[i]; length = (length << 4) | uint32_t(ch <= '9' ? ch - '0' : (ch >= 'a' ? ch - 'a' : ch - 'A') + 10); }
	return length >= 16 && length == hex.size() / 2 - 16;
}
JSON configMaterial(const JSON &config) {
	const JSON &material = member(config, "material");
	need(config.isObject() && member(config, "name").isString() && !config["name"].asString().empty() && material.isObject(), "CACHE_INVALID", "The CLI signing configuration is incomplete.");
	for (const char *key : {"certpath", "profile", "storeFile", "keyAlias", "signAlg", "keyPassword", "storePassword"}) need(material[key].isString() && !material[key].asString().empty() && material[key].asString().find('\0') == std::string::npos, "CACHE_INVALID", "The CLI signing configuration contains invalid signing material.");
	need(frame(material["keyPassword"]) && frame(material["storePassword"]) && (equals(material["signAlg"], "SHA256withECDSA") || equals(material["signAlg"], "SHA384withECDSA")), "CACHE_INVALID", "The CLI signing configuration does not contain supported encrypted signing material.");
	return material;
}
// Convert an already parsed X.509 UTC civil time without local timezone state.
int64_t certificateTime(const mbedtls_x509_time &time) {
	int year = time.year; const unsigned month = unsigned(time.mon), day = unsigned(time.day);
	year -= month <= 2;
	const int era = (year >= 0 ? year : year - 399) / 400;
	const unsigned yoe = unsigned(year - era * 400);
	const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return (int64_t(era) * 146097 + doe - 719468) * 86400 + time.hour * 3600 + time.min * 60 + time.sec;
}
struct Certificate {
	mbedtls_x509_crt crt;
	Certificate() { mbedtls_x509_crt_init(&crt); }
	~Certificate() { mbedtls_x509_crt_free(&crt); }
	Certificate(const Certificate &) = delete;
	Certificate &operator=(const Certificate &) = delete;
	bool parse(const std::string &bytes) {
		if (bytes.find("-----BEGIN CERTIFICATE-----") != std::string::npos) return mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char *>(bytes.c_str()), bytes.size() + 1) == 0;
		return mbedtls_x509_crt_parse_der(&crt, reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size()) == 0;
	}
};
std::string certificateCheck(const JSON &material, const JSON &profile, int64_t now) {
	try {
		const JSON &embedded = member(member(profile, "bundle-info"), "development-certificate");
		if (!embedded.isString() || embedded.asString().size() > LIMIT) return "CERTIFICATE_INVALID";
		Certificate chain, certificate;
		if (!chain.parse(readBytes(material["certpath"].asString())) || !certificate.parse(embedded.asString())) return "CERTIFICATE_INVALID";
		const mbedtls_x509_crt *matched = nullptr;
		for (const mbedtls_x509_crt *candidate = &chain.crt; candidate; candidate = candidate->next) {
			if (candidate->raw.len == certificate.crt.raw.len && candidate->raw.p && certificate.crt.raw.p && std::memcmp(candidate->raw.p, certificate.crt.raw.p, candidate->raw.len) == 0) { matched = candidate; break; }
		}
		if (!matched) return "CERTIFICATE_MISMATCH";
		if (certificateTime(matched->valid_from) > now || certificateTime(matched->valid_to) <= now) return "CERTIFICATE_EXPIRED";
		return "";
	} catch (const SafeError &) { return "CERTIFICATE_INVALID"; }
}
struct Inspection { bool ready = false; std::string reason = "CACHE_INVALID"; JSON material; };
Inspection inspectSigning(Context &ctx, const JSON &config) {
	JSON material;
	try {
		const JSON context = readJSON(join(ctx.cache, "signing-context.json"));
		if (member(context, "requiredACL") != stringArray(ctx.metadata.requiredACL)) return {false, "PROFILE_ACL_MISMATCH", JSON()};
		material = configMaterial(config);
		for (const char *key : {"certpath", "profile", "storeFile"}) {
			const std::string path = material[key].asString();
			need(absolute(path) && inside(join(ctx.cache, "materials"), path) && real(path) == path, "CACHE_INVALID", "Signing paths must remain in this private application cache without symbolic links.");
			privateMode(path, 0600, false);
		}
		for (const char *name : {"fd", "ac", "ce"}) {
			const std::string dir = join(parent(material["storeFile"].asString()), std::string("material/") + name);
			directory(dir, "CACHE_INVALID");
			privateMode(dir, 0700, true);
			need(!entries(dir).empty(), "CACHE_INVALID", "Signing companion material is incomplete.");
		}
	} catch (const SafeError &error) {
		if (error.code == "PRIVATE_STORAGE_REQUIRED") throw;
		return {false, "CACHE_INVALID", JSON()};
	}
	const std::string output = join(ctx.work, "verified-profile.json");
	if (exists(output)) unlinkRegular(output);
	const Result result = execute(ctx.verifier, {"verify-profile", "-inFile", material["profile"].asString(), "-outFile", output}, cliEnvironment(ctx));
	if (result.missing || result.execError == EACCES || result.execError == ENOEXEC) fail("CLT_MISSING", "The selected SDK native profile verifier is not executable. Check the Command Line Tools installation.");
	if (!result.ok || !exists(output)) return {false, "PROFILE_UNVERIFIED", JSON()};
	JSON profile;
	try { privateMode(output, 0600, false); profile = readJSON(output); }
	catch (const SafeError &error) { if (error.code == "PRIVATE_STORAGE_REQUIRED") throw; return {false, "PROFILE_UNVERIFIED", JSON()}; }
	const int64_t now = int64_t(std::time(nullptr));
	if (!equals(member(profile, "type"), "debug") || !equals(member(member(profile, "bundle-info"), "bundle-name"), ctx.metadata.bundle.c_str())) return {false, "PROFILE_BUNDLE_MISMATCH", JSON()};
	const JSON &validity = member(profile, "validity"), &before = member(validity, "not-before"), &after = member(validity, "not-after");
	if (!validity.isObject() || !before.isIntegral() || !before.isInt64() || !after.isIntegral() || !after.isInt64() || before.asInt64() < -9007199254740991LL || before.asInt64() > 9007199254740991LL || after.asInt64() < -9007199254740991LL || after.asInt64() > 9007199254740991LL || before.asInt64() > now || after.asInt64() <= now) return {false, "PROFILE_EXPIRED", JSON()};
	const JSON &debug = member(profile, "debug-info"), &deviceType = member(debug, "device-id-type"), &devices = member(debug, "device-ids");
	std::string type = deviceType.isString() ? deviceType.asString() : "";
	std::transform(type.begin(), type.end(), type.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
	bool selected = false;
	if (!debug.isObject() || type != "udid" || !devices.isArray()) return {false, "PROFILE_DEVICE_MISMATCH", JSON()};
	for (const auto &device : devices) {
		if (!device.isString() || !std::regex_match(device.asString(), std::regex("[0-9a-fA-F]{64}"))) return {false, "PROFILE_DEVICE_MISMATCH", JSON()};
		std::string id = device.asString(); std::transform(id.begin(), id.end(), id.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
		if (id == ctx.udid) selected = true;
	}
	if (!selected) return {false, "PROFILE_DEVICE_MISMATCH", JSON()};
	// ACL-free requests impose no shape on the SDK's optional ACL representation.
	if (!ctx.metadata.requiredACL.empty()) {
		const JSON &acls = member(member(profile, "acls"), "allowed-acls");
		if (!acls.isArray()) return {false, "PROFILE_ACL_MISMATCH", JSON()};
		std::set<std::string> allowed;
		for (const auto &acl : acls) { if (!acl.isString()) return {false, "PROFILE_ACL_MISMATCH", JSON()}; allowed.insert(acl.asString()); }
		for (const auto &acl : ctx.metadata.requiredACL) if (!allowed.count(acl)) return {false, "PROFILE_ACL_MISMATCH", JSON()};
	}
	const std::string certificateError = certificateCheck(material, profile, now);
	if (!certificateError.empty()) return {false, certificateError, JSON()};
	return {true, "", material};
}
void refreshMetadata(Context &ctx) {
	Metadata current = projectMetadata(ctx.request);
	need(current.bundle == ctx.metadata.bundle, "PROJECT_CHANGED", "The project bundle changed during automatic signing. Retry with the current project configuration.");
	ctx.metadata = std::move(current);
}
Inspection cachedSigning(Context &ctx) {
	const std::string filename = join(ctx.cache, "signing-config.json");
	if (!exists(filename)) return {false, "CACHE_MISSING", JSON()};
	try { return inspectSigning(ctx, readJSON(filename)); }
	catch (const SafeError &error) {
		if (interrupted || error.code == "PRIVATE_STORAGE_REQUIRED" || error.code == "CLT_MISSING") throw;
		return {false, "CACHE_INVALID", JSON()};
	}
}
void prepareMinimal(Context &ctx) {
	const std::string target = privateDir(join(ctx.cache, "project"));
	JSON profile = ctx.metadata.rootProfile;
	profile["app"]["signingConfigs"] = JSON(Json::arrayValue);
	for (auto &product : profile["app"]["products"]) product.removeMember("signingConfig");
	writeJSON(join(target, "build-profile.json5"), profile);
	writeJSON(join(target, "AppScope/app.json5"), ctx.metadata.app);
	writeJSON(join(target, "entry/src/main/module.json5"), ctx.metadata.module);
	const std::string entryBuild = join(field(ctx.request, "projectDir"), "entry/build-profile.json5");
	if (exists(entryBuild)) writeJSON(join(target, "entry/build-profile.json5"), readJSON(entryBuild, true));
	ctx.minimal = target;
}
JSON importGenerated(Context &ctx) {
	const JSON profile = readJSON(join(ctx.minimal, "build-profile.json5"), true);
	const JSON &app = member(profile, "app"), &products = member(app, "products"), &configs = member(app, "signingConfigs");
	need(app.isObject() && products.isArray() && configs.isArray(), "SIGNING_FAILED", "The CLI returned a malformed signing project configuration.");
	JSON selectedProduct;
	for (const auto &product : products) if (equals(member(product, "name"), "default")) { selectedProduct = product; break; }
	std::vector<JSON> selectedConfigs;
	for (const auto &config : configs) if (config.isObject() && member(config, "name") == member(selectedProduct, "signingConfig")) selectedConfigs.push_back(config);
	need(selectedConfigs.size() == 1, "SIGNING_FAILED", "The CLI did not write one selected signing configuration.");
	JSON config = selectedConfigs[0];
	const JSON material = configMaterial(config);
	const std::string stage = privateDir(join(ctx.work, "new-materials"));
	for (const auto &item : std::array<std::pair<const char *, const char *>, 3>{{{"certpath", "certificate.cer"}, {"profile", "profile.p7b"}, {"storeFile", "store.p12"}}}) {
		need(absolute(material[item.first].asString()), "SIGNING_FAILED", "The CLI returned a nonabsolute signing path.");
		const std::string source = real(material[item.first].asString());
		need(inside(ctx.cliHome, source) || inside(ctx.cache, source), "SIGNING_FAILED", "The CLI returned signing files outside its private namespace.");
		copyFilePrivate(source, join(stage, item.second));
	}
	copyTree(join(parent(real(material["storeFile"].asString())), "material"), join(stage, "material"));
	const std::string destination = join(ctx.cache, "materials");
	if (exists(destination)) removeOwned(ctx.cache, destination);
	// TMPDIR and XDG_CONFIG_HOME need not be on the same filesystem.
	copyTree(stage, destination);
	config["material"]["certpath"] = join(destination, "certificate.cer");
	config["material"]["profile"] = join(destination, "profile.p7b");
	config["material"]["storeFile"] = join(destination, "store.p12");
	writeJSON(join(ctx.cache, "signing-config.json"), config);
	JSON context(Json::objectValue); context["requiredACL"] = stringArray(ctx.metadata.requiredACL);
	writeJSON(join(ctx.cache, "signing-context.json"), context);
	return config;
}
void retainRecovery(Context &ctx, const std::string &cacheBackup, const std::string &ohosBackup, bool hadOhos) noexcept {
	// A failed restore (e.g. full configuration filesystem) must not destroy its
	// sole complete backups. Prefer durable private recovery storage; retain the
	// original verified private TMPDIR only if even that transfer is impossible.
	if (ctx.temporary) ctx.temporary->retainedForRecovery = true;
	try {
		const std::string recoveryRoot = privateDir(join(ctx.root, "recovery"));
		std::string pattern = join(recoveryRoot, "recovery-XXXXXX");
		std::vector<char> bytes(pattern.begin(), pattern.end()); bytes.push_back(0);
		char *created = ::mkdtemp(bytes.data());
		need(created != nullptr, "PRIVATE_STORAGE_REQUIRED", "Cannot retain private recovery material.");
		const std::string destination = created; privateMode(destination, 0700, true);
		copyTree(cacheBackup, join(destination, "cache-backup"));
		if (hadOhos) copyTree(ohosBackup, join(destination, "cli-backup"));
		if (ctx.temporary) ctx.temporary->retainedForRecovery = false;
	} catch (...) {}
}
Inspection generate(Context &ctx) {
	Lock teamLock(join(ctx.root, "locks/team-" + digest(ctx.team)));
	Lock cliLock(join(ctx.root, "locks/cli"));
	const std::string cacheBackup = join(ctx.work, "cache-backup"), ohos = join(ctx.cliHome, ".ohos"), ohosBackup = join(ctx.work, "cli-backup");
	bool backedUp = false, hadOhos = false;
	try {
		hardenTree(ctx.cliHome);
		syncLogin(ctx);
		authenticate(ctx);
		need(selectTeam(ctx) == ctx.team, "TEAM_REQUIRED", "The available signing team changed. Select the intended team ID in EditorSettings and retry.");
		ctx.udid = deviceUDID(ctx);
		refreshMetadata(ctx);
		const Inspection current = cachedSigning(ctx);
		if (current.ready) return current;
		copyTree(ctx.cache, cacheBackup);
		hadOhos = exists(ohos);
		if (hadOhos) copyTree(ohos, ohosBackup);
		backedUp = true;
		prepareMinimal(ctx);
		const Result result = cli(ctx, {"signature", "generate", "--team-id", ctx.team, "--product", "default"});
		if (!result.ok) {
			if (networkFailure(result)) fail("NETWORK_ERROR", "Signing service request failed. Check the network/proxy; old local signing materials were retained.");
			fail("SIGNING_FAILED", "The official CLI failed to generate signing materials. Old local files were retained; a cloud certificate change cannot be rolled back locally.");
		}
		const JSON config = importGenerated(ctx);
		const Inspection checked = inspectSigning(ctx, config);
		need(checked.ready, checked.reason == "PROFILE_DEVICE_MISMATCH" ? "PROFILE_DEVICE_MISMATCH" : "PROFILE_INVALID", "The generated profile did not pass bundle, device, permission, validity and certificate checks. Validation reason: " + checked.reason + ". Old local files were retained; check the official CLI account/device setup.");
		return checked;
	} catch (...) {
		if (backedUp) {
			bool restoreFailed = false;
			try { removeOwned(ctx.root, ctx.cache); copyTree(cacheBackup, ctx.cache); } catch (...) { restoreFailed = true; }
			try {
				if (exists(ohos)) removeOwned(ctx.cliHome, ohos);
				if (hadOhos) copyTree(ohosBackup, ohos);
			} catch (...) { restoreFailed = true; }
			if (restoreFailed) {
				retainRecovery(ctx, cacheBackup, ohosBackup, hadOhos);
				fail("ROLLBACK_FAILED", "Local signing files could not be fully restored. Stop signing and inspect the retained private recovery backups before retrying; cloud certificate changes cannot be rolled back locally.");
			}
		}
		throw;
	}
}
std::string buildSdkRoot(const JSON &request) {
	// sdk-config.cjs accepts an SDK home or a version/default bundle. Normalize
	// a direct HarmonyOS component root for that public build-runner contract.
	const std::string selected = real(field(request, "sdkRoot"), "CLT_MISSING");
	if (fs::path(selected).filename() == "openharmony" && fs::path(parent(selected)).filename() == "default" && exists(join(parent(selected), "sdk-pkg.json"))) return parent(parent(selected));
	return field(request, "sdkRoot");
}
std::string buildHvigorHome(Context &ctx) {
	// Keep the private umask: HOME/hmdfs maps directory ownership and cannot
	// safely host owner-only workspaces. Reuse only the prepared wrapper tree;
	// all writable Hvigor state belongs to this permission-capable operation.
	const auto selected = ctx.originalEnv.find("HVIGOR_USER_HOME");
	const std::string source = selected == ctx.originalEnv.end() ? join(ctx.originalHome, ".hvigor") : selected->second;
	need(absolute(source), "CLT_MISSING", "The prepared Hvigor home must be an absolute path.");
	const std::string wrapper = real(join(source, "wrapper"), "CLT_MISSING");
	directory(wrapper, "CLT_MISSING");
	const std::string home = privateDir(join(ctx.work, "hvigor-home"));
	need(entries(home).empty(), "PRIVATE_STORAGE_REQUIRED", "The private Hvigor workspace must be empty before initialization.");
	need(::symlink(wrapper.c_str(), join(home, "wrapper").c_str()) == 0, "PRIVATE_STORAGE_REQUIRED", "Cannot reuse the prepared Hvigor wrapper dependencies.");
	const std::string config = join(source, "hvigor-config.json5");
	if (exists(config)) {
		// Preserve the public runner's validation of global dependencies/options
		// without allowing this build to rewrite the caller's configuration.
		privateWrite(join(home, "hvigor-config.json5"), readBytes(real(config, "CLT_MISSING"), "CLT_MISSING"));
	}
	return home;
}
JSON build(Context &ctx, const JSON &material) {
	const std::string project = field(ctx.request, "projectDir"), runner = join(project, "tools/build.cjs"), profilePath = join(project, "build-profile.json5");
	regular(runner, "BUILD_FAILED");
	const std::string original = readBytes(profilePath, "BUILD_FAILED"), workDir = privateDir(join(ctx.work, "build"));
	JSON request(Json::objectValue); request["schemaVersion"] = 2; request["projectDir"] = project;
	request["sdkRoot"] = buildSdkRoot(ctx.request); request["commandLineToolsRoot"] = ctx.request["commandLineToolsRoot"];
	request["buildMode"] = "debug"; request["format"] = "hap"; request["workDir"] = workDir;
	request["hvigorUserHome"] = buildHvigorHome(ctx);
	JSON signing(Json::objectValue);
	for (const auto &item : std::array<std::pair<const char *, const char *>, 7>{{{"certificate", "certpath"}, {"profile", "profile"}, {"storeFile", "storeFile"}, {"keyAlias", "keyAlias"}, {"signAlg", "signAlg"}, {"keyPasswordEncrypted", "keyPassword"}, {"storePasswordEncrypted", "storePassword"}}}) signing[item.first] = material[item.second];
	request["signing"] = signing;
	const std::string filename = join(workDir, "request.json"); writeJSON(filename, request);
	Result result; std::exception_ptr executionError;
	Env buildEnv = nodeEnvironment(ctx.request, ctx.env, ctx.work);
	buildEnv["COMMAND_LINE_TOOL_PATH"] = field(ctx.request, "commandLineToolsRoot");
	try { result = execute(field(ctx.request, "nodePath"), {runner, "--request", filename}, buildEnv, project, 10 * 60 * 1000); }
	catch (...) { executionError = std::current_exception(); }
	bool restored = false;
	try { restored = readBytes(profilePath, "BUILD_PROFILE_NOT_RESTORED") == original; } catch (...) {}
	need(restored, "BUILD_PROFILE_NOT_RESTORED", "The build profile was not restored. Recover the build runner backup before retrying.");
	if (executionError) std::rethrow_exception(executionError);
	need(result.ok, "BUILD_FAILED", "Signed HAP build failed. Check the prepared offline toolchain and build inputs.");
	JSON receipt;
	try { receipt = readJSON(join(workDir, "result.json")); } catch (const SafeError &) { fail("BUILD_FAILED", "The build runner did not produce a valid result."); }
	const JSON &output = member(receipt, "outputPath");
	need(member(receipt, "schemaVersion").isInt() && receipt["schemaVersion"].asInt() == 2 && member(receipt, "signed").isBool() && receipt["signed"].asBool() && equals(member(receipt, "format"), "hap") && equals(member(receipt, "mode"), "debug") && output.isString() && absolute(output.asString()) && inside(project, output.asString()), "BUILD_FAILED", "The build runner returned an invalid signed artifact receipt.");
	struct stat st{};
	const std::string outputPath = output.asString();
	need(::lstat(outputPath.c_str(), &st) == 0 && S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode) && st.st_size > 0 && real(outputPath, "BUILD_FAILED") == outputPath && fs::path(outputPath).extension() == ".hap", "BUILD_FAILED", "The signed HAP artifact is missing, empty, or traverses symbolic links.");
	JSON response(Json::objectValue); response["schemaVersion"] = 1; response["status"] = "ok"; response["signed"] = true; response["format"] = "hap"; response["mode"] = "debug"; response["outputPath"] = outputPath; response["profileValidated"] = true;
	return response;
}
JSON perform(const JSON &request, const Env &originalEnv) {
	validateRequest(request);
	Context ctx; ctx.request = request; ctx.originalEnv = originalEnv; ctx.env = cleanEnvironment(originalEnv);
	ctx.originalHome = envValue(originalEnv, "HOME");
	need(absolute(ctx.originalHome), "PRIVATE_STORAGE_REQUIRED", "An absolute original HOME is required to copy the existing CLI login.");
	ctx.originalHome = normalized(ctx.originalHome);
	const std::string configHome = normalized(envValue(originalEnv, "XDG_CONFIG_HOME")), tempHome = normalized(envValue(originalEnv, "TMPDIR"));
	for (const auto &root : {configHome, tempHome}) {
		need(absolute(root) && root != ctx.originalHome && !inside(ctx.originalHome, root) && !std::regex_search(root, std::regex("(^|/)hmdfs(/|$)", std::regex::icase)), "PRIVATE_STORAGE_REQUIRED", "Set XDG_CONFIG_HOME and TMPDIR to permission-capable private storage outside HOME/hmdfs.");
		directory(root, "PRIVATE_STORAGE_REQUIRED");
	}
	const std::string signingRoot = join(configHome, "godot-openharmony-signing");
	need(signingRoot != ctx.originalHome && !inside(signingRoot, ctx.originalHome) && normalized(field(request, "projectDir")) != signingRoot && !inside(signingRoot, field(request, "projectDir")) && !inside(field(request, "projectDir"), signingRoot) && normalized(field(request, "projectDir")) != tempHome && !inside(field(request, "projectDir"), tempHome) && tempHome != signingRoot && !inside(signingRoot, tempHome), "PRIVATE_STORAGE_REQUIRED", "Private signing storage and temporary storage must be separate from the source HOME and exported project.");
	ctx.root = privateDir(signingRoot); privateDir(join(ctx.root, "locks"));
	ctx.cliHome = privateDir(join(ctx.root, "cli-home")); ctx.cliData = privateDir(join(ctx.cliHome, ".config/deveco-cli"));
	Temporary temporary(tempHome); ctx.work = temporary.path; ctx.temporary = &temporary;
	nodeCheck(request, ctx.env, ctx.work);
	ctx.cli = discoverCLI(request, ctx.env); ctx.verifier = discoverVerifier(request); ctx.metadata = projectMetadata(request);
	{
		Lock cliLock(join(ctx.root, "locks/cli"));
		hardenTree(ctx.cliHome);
		syncLogin(ctx); ctx.udid = deviceUDID(ctx); authenticate(ctx); ctx.team = selectTeam(ctx);
	}
	JSON identity(Json::arrayValue); identity.append(request["projectIdentity"]); identity.append(ctx.metadata.bundle); identity.append(ctx.team);
	const std::string key = digest(jsonString(identity));
	Lock projectLock(join(ctx.root, "locks/project-" + key));
	ctx.cache = privateDir(join(ctx.root, "projects/" + key)); hardenTree(ctx.cache); refreshMetadata(ctx);
	Inspection checked = cachedSigning(ctx);
	if (!checked.ready) checked = generate(ctx);
	JSON response = build(ctx, checked.material);
	temporary.finish();
	checkInterrupted();
	return response;
}

// Only the generated private overlay invokes this path. Proxy stdout/stderr are
// HDC's own streams, directed to the CLI's bounded captured pipes, never Editor.
int proxyMain(const std::string &configPath, const std::vector<std::string> &args) {
	try {
		const std::string dir = parent(configPath);
		const struct stat parentStat = directory(dir, "PRIVATE_STORAGE_REQUIRED"), fileStat = regular(configPath, "PRIVATE_STORAGE_REQUIRED");
		need(parentStat.st_uid == ::getuid() && (parentStat.st_mode & 07777) == 0700 && fileStat.st_uid == ::getuid() && (fileStat.st_mode & 07777) == 0600 && real(configPath) == normalized(configPath), "PRIVATE_STORAGE_REQUIRED", "Invalid native HDC proxy configuration.");
		const JSON config = readJSON(configPath);
		need(config.size() == 3 && config.isMember("schemaVersion") && config["schemaVersion"].isInt() && config["schemaVersion"].asInt() == 1 && text(member(config, "hdcPath")) && absolute(config["hdcPath"].asString()) && member(config, "hostEnvironment").isObject(), "INVALID_REQUEST", "Invalid native HDC proxy configuration.");
		const std::string hdc = real(config["hdcPath"].asString());
		need(hdc != selfPath(), "INVALID_REQUEST", "The native HDC proxy cannot invoke itself.");
		const JSON &host = config["hostEnvironment"];
		need(text(member(host, "HOME")) && absolute(host["HOME"].asString()), "INVALID_REQUEST", "Invalid original HDC HOME.");
		for (const auto &key : host.getMemberNames()) need((key == "HOME" || key == "HDC_SERVER" || key == "OHOS_HDC_SERVER_PORT") && text(host[key]), "INVALID_REQUEST", "Invalid native HDC host environment.");
		Env env = cleanEnvironment(environment());
		for (const char *key : {"HOME", "HDC_SERVER", "OHOS_HDC_SERVER_PORT"}) { if (host.isMember(key)) env[key] = host[key].asString(); else env.erase(key); }
		std::vector<std::string> storage{hdc}; storage.insert(storage.end(), args.begin(), args.end());
		std::vector<char *> argv; for (auto &arg : storage) argv.push_back(arg.data()); argv.push_back(nullptr);
		std::vector<std::string> envStorage; for (const auto &entry : env) envStorage.push_back(entry.first + "=" + entry.second);
		std::vector<char *> envp; for (auto &entry : envStorage) envp.push_back(entry.data()); envp.push_back(nullptr);
		::execve(hdc.c_str(), argv.data(), envp.data());
		return 127;
	} catch (...) { return 126; }
}
JSON errorResponse(const std::string &code, const std::string &message) {
	JSON response(Json::objectValue); response["schemaVersion"] = 1; response["status"] = "error"; response["code"] = code; response["message"] = message; return response;
}
} // namespace auto_signer

int main(int argc, char **argv) {
	using namespace auto_signer;
	if (argc > 0 && fs::path(argv[0]).filename() == "hdc") {
		std::vector<std::string> args; for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
		try { return proxyMain(join(parent(selfPath()), ".godot-hdc-host.json"), args); } catch (...) { return 126; }
	}
	if (argc >= 4 && std::string(argv[1]) == "--hdc-host" && std::string(argv[3]) == "--") {
		std::vector<std::string> args; for (int i = 4; i < argc; ++i) args.emplace_back(argv[i]);
		return proxyMain(argv[2], args);
	}
	::umask(0077);
	struct sigaction action{}; action.sa_handler = handleSignal; ::sigemptyset(&action.sa_mask);
	::sigaction(SIGINT, &action, nullptr); ::sigaction(SIGTERM, &action, nullptr); ::signal(SIGPIPE, SIG_IGN); ::signal(SIGCHLD, SIG_DFL);
	JSON response; int exitCode = 0;
	try {
		need(argc == 3 && std::string(argv[1]) == "--request-json", "INVALID_REQUEST", "Use auto-sign --request-json with one JSON metadata object.");
		response = perform(parseJSON(argv[2]), environment());
	} catch (const SafeError &error) { response = errorResponse(error.code, error.what()); if (error.code == "INTERNAL_ERROR") exitCode = 1; }
	catch (const fs::filesystem_error &error) {
		if (error.code().value() == EACCES || error.code().value() == EPERM || error.code().value() == EROFS) response = errorResponse("PRIVATE_STORAGE_REQUIRED", "Signing storage or tools are inaccessible. Check private storage permissions and tool paths.");
		else if (error.code().value() == ENOENT) response = errorResponse("CACHE_INVALID", "A required signing/build file disappeared. Check the selected toolchain and retry.");
		else { response = errorResponse("INTERNAL_ERROR", "Automatic signing failed internally. Check the private cache and selected tools."); exitCode = 1; }
	} catch (...) { response = errorResponse("INTERNAL_ERROR", "Automatic signing failed internally. Check the private cache and selected tools."); exitCode = 1; }
	std::cout << jsonString(response) << '\n';
	return exitCode;
}
