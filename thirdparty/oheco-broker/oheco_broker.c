#define _POSIX_C_SOURCE 200809L
#include "oheco_broker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define OB_MAX_FRAME 1048576u
#define OB_MAX_STREAM 65536u
#define OB_DEADLINE 3000

struct ob_process {
    int fd;
    pthread_mutex_t writer;
    atomic_int failure, cancelled, exited;
    int stdin_open;
    unsigned char header[5];
    size_t header_have, payload_have, payload_size;
    unsigned char *payload;
    ob_exit result;
};

static ob_error diag(ob_diagnostic *d, ob_error e, const char *stage,
                     int native, const char *message) {
    if (d) {
        memset(d, 0, sizeof(*d));
        d->code = e;
        d->native_error = native;
        snprintf(d->stage, sizeof(d->stage), "%s", stage);
        snprintf(d->message, sizeof(d->message), "%s", message);
    }
    return e;
}

const char *ob_error_name(ob_error e) {
    static const char *const names[] = {"OK", "UNAVAILABLE", "PROTOCOL",
        "SPAWN_FAILED", "CONNECTION_LOST", "INVALID_ARGUMENT", "TIMEOUT", "IO", "LIMIT"};
    return (unsigned)e < sizeof(names) / sizeof(names[0]) ? names[e] : "UNKNOWN";
}

static int64_t now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int64_t deadline(int timeout) { return timeout < 0 ? -1 : now_ms() + timeout; }
static int remaining(int64_t end) {
    if (end < 0) return -1;
    int64_t n = end - now_ms();
    return n <= 0 ? 0 : n > INT_MAX ? INT_MAX : (int)n;
}

/* Strict Unicode scalar UTF-8: excludes NUL, overlongs and surrogates. */
static int utf8(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint32_t c = s[i++], minimum;
        unsigned more;
        if (c == 0) return 0;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) { more = 1; minimum = 0x80; c &= 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { more = 2; minimum = 0x800; c &= 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { more = 3; minimum = 0x10000; c &= 7; }
        else return 0;
        if (n - i < more) return 0;
        while (more--) {
            unsigned b = s[i++];
            if ((b & 0xc0) != 0x80) return 0;
            c = (c << 6) | (b & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0;
    }
    return 1;
}
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}
static unsigned char *put_string(unsigned char *p, const char *s) {
    size_t n = strlen(s);
    put32(p, (uint32_t)n); memcpy(p + 4, s, n); return p + 4 + n;
}
static ob_error size_string(const char *s, size_t *total) {
    if (!s) return OB_INVALID_ARGUMENT;
    size_t n = strlen(s);
    if (!utf8((const unsigned char *)s, n)) return OB_INVALID_ARGUMENT;
    if (n > OB_MAX_FRAME - 4 || *total > OB_MAX_FRAME - 4 - n) return OB_LIMIT;
    *total += 4 + n;
    return OB_OK;
}
static ob_error encode_start(const ob_options *o, int detached,
                             const char *stdout_file, const char *stderr_file,
                             unsigned char **payload, size_t *size) {
    if (!o || !o->executable || !*o->executable || o->argc > 4096 || o->envc > 4096 ||
        (o->argc && !o->args) || (o->envc && !o->env) ||
        (o->stdin_enabled != 0 && o->stdin_enabled != 1) ||
        (detached && o->stdin_enabled)) return OB_INVALID_ARGUMENT;
    const char *cwd = o->cwd ? o->cwd : "";
    size_t n = 9;
    ob_error e = size_string(o->executable, &n);
    if (!e) e = size_string(cwd, &n);
    for (size_t i = 0; !e && i < o->argc; i++) e = size_string(o->args[i], &n);
    for (size_t i = 0; !e && i < o->envc; i++) {
        if (!o->env[i].key || !*o->env[i].key || strchr(o->env[i].key, '=')) return OB_INVALID_ARGUMENT;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(o->env[i].key, o->env[j].key)) return OB_INVALID_ARGUMENT;
        e = size_string(o->env[i].key, &n);
        if (!e) e = size_string(o->env[i].value, &n);
    }
    if (!e && detached) e = size_string(stdout_file, &n);
    if (!e && detached) e = size_string(stderr_file, &n);
    if (e) return e;
    unsigned char *b = malloc(n), *p;
    if (!b) return OB_IO;
    p = put_string(b, o->executable); p = put_string(p, cwd);
    put32(p, (uint32_t)o->argc); p += 4;
    for (size_t i = 0; i < o->argc; i++) p = put_string(p, o->args[i]);
    put32(p, (uint32_t)o->envc); p += 4;
    for (size_t i = 0; i < o->envc; i++) {
        p = put_string(p, o->env[i].key); p = put_string(p, o->env[i].value);
    }
    *p++ = (unsigned char)o->stdin_enabled;
    if (detached) { p = put_string(p, stdout_file); (void)put_string(p, stderr_file); }
    *payload = b; *size = n;
    return OB_OK;
}

/* Returns errno-style result. cancel-sensitive sends wake at least every 50ms. */
static int ready(ob_process *p, short events, int64_t end, int interruptible) {
    for (;;) {
        if (interruptible && atomic_load(&p->cancelled)) return ECANCELED;
        int timeout = remaining(end);
        if (interruptible && (timeout < 0 || timeout > 50)) timeout = 50;
        struct pollfd f = {p->fd, events, 0};
        int n = poll(&f, 1, timeout);
        if (n > 0) return (f.revents & POLLNVAL) ? EBADF : 0;
        if (n < 0 && errno != EINTR) return errno;
        if (end >= 0 && remaining(end) == 0) return ETIMEDOUT;
    }
}
static ssize_t safe_send(int fd, const void *data, size_t size) {
#ifdef MSG_NOSIGNAL
    return send(fd, data, size, MSG_NOSIGNAL);
#else
    /* Do not change process-wide SIGPIPE handling or consume preexisting signals. */
    sigset_t set, old, pending;
    sigemptyset(&set); sigaddset(&set, SIGPIPE);
    int rc = pthread_sigmask(SIG_BLOCK, &set, &old);
    if (rc) { errno = rc; return -1; }
    if (sigpending(&pending) < 0) {
        int saved = errno; (void)pthread_sigmask(SIG_SETMASK, &old, NULL);
        errno = saved; return -1;
    }
    int existed = sigismember(&pending, SIGPIPE);
    ssize_t n = send(fd, data, size, 0);
    int saved = errno;
    if (n < 0 && saved == EPIPE && !existed) {
        struct timespec zero = {0, 0};
        while (sigtimedwait(&set, NULL, &zero) < 0 && errno == EINTR) {}
    }
    (void)pthread_sigmask(SIG_SETMASK, &old, NULL);
    errno = saved; return n;
#endif
}
static int send_bytes(ob_process *p, const void *data, size_t n,
                      int64_t end, int interruptible) {
    const unsigned char *b = data;
    while (n) {
        if (interruptible && atomic_load(&p->cancelled)) return ECANCELED;
        if (remaining(end) == 0) return ETIMEDOUT;
        ssize_t sent = safe_send(p->fd, b, n);
        if (sent > 0) { b += sent; n -= (size_t)sent; continue; }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int rc = ready(p, POLLOUT, end, interruptible); if (rc) return rc;
        } else return sent == 0 ? EPIPE : errno;
    }
    return 0;
}
static int recv_bytes(ob_process *p, unsigned char *b, size_t n, size_t *have, int64_t end) {
    while (*have < n) {
        ssize_t got = recv(p->fd, b + *have, n - *have, 0);
        if (got > 0) { *have += (size_t)got; continue; }
        if (got == 0) return ECONNRESET;
        if (errno == EINTR) { if (remaining(end) == 0) return ETIMEDOUT; continue; }
        if (errno != EAGAIN && errno != EWOULDBLOCK) return errno;
        int rc = ready(p, POLLIN, end, 0); if (rc) return rc;
    }
    return 0;
}
static int send_frame(ob_process *p, unsigned type, const void *data, size_t n,
                      int64_t end, int interruptible) {
    unsigned char h[5]; h[0] = (unsigned char)type; put32(h + 1, (uint32_t)n);
    int rc = send_bytes(p, h, 5, end, interruptible);
    return rc ? rc : send_bytes(p, data, n, end, interruptible);
}
static ob_error poison(ob_process *p, ob_error e) {
    int expected = OB_OK;
    (void)atomic_compare_exchange_strong(&p->failure, &expected, e);
    (void)shutdown(p->fd, SHUT_RDWR);
    return (ob_error)atomic_load(&p->failure);
}
static ob_error receive_frame(ob_process *p, int64_t end, int starting, ob_diagnostic *d) {
    int rc = recv_bytes(p, p->header, 5, &p->header_have, end);
    if (!rc && !p->payload) {
        p->payload_size = get32(p->header + 1);
        unsigned t = p->header[0]; size_t n = p->payload_size;
        int valid = n <= OB_MAX_FRAME &&
            ((t == 9 && n >= 8) || (starting == 1 && t == 2 && n == 0) ||
             (starting == 2 && t == 11 && n == 4) ||
             (!starting && ((t == 8 && n == 12) || ((t == 5 || t == 6) && n >= 1 && n <= OB_MAX_STREAM))));
        if (!valid) return diag(d, poison(p, OB_PROTOCOL), "frame", 0, "Invalid frame type, order or size");
        p->payload = malloc(n ? n : 1);
        if (!p->payload) return diag(d, poison(p, OB_IO), "frame", ENOMEM, "Allocation failed");
    }
    if (!rc) rc = recv_bytes(p, p->payload, p->payload_size, &p->payload_have, end);
    if (rc == ETIMEDOUT) return diag(d, OB_TIMEOUT, "read", rc, "Read deadline expired; command not cancelled");
    if (rc) {
        ob_error e = (ob_error)atomic_load(&p->failure);
        return diag(d, poison(p, e ? e : OB_CONNECTION_LOST), "read", rc, "Connection lost; command outcome may be unknown");
    }
    if (p->header[0] == 9) {
        uint32_t code = get32(p->payload), n = get32(p->payload + 4);
        if (code < 1 || code > 8 || n != p->payload_size - 8 || !utf8(p->payload + 8, n))
            return diag(d, poison(p, OB_PROTOCOL), "error", 0, "Invalid ERROR payload");
        char message[256]; size_t copy = n < sizeof(message) - 1 ? n : sizeof(message) - 1;
        memcpy(message, p->payload + 8, copy); message[copy] = 0;
        return diag(d, poison(p, (ob_error)code), "server", 0, message);
    }
    return OB_OK;
}
static void clear_frame(ob_process *p) {
    free(p->payload); p->payload = NULL;
    p->header_have = p->payload_have = p->payload_size = 0;
}

static int discover(const char *path, unsigned *port) {
    char *allocated = NULL;
    if (!path) {
        const char *home = getenv("HOME");
        if (!home || !*home) return ENOENT;
        size_t n = strlen(home);
        if (n > SIZE_MAX - 25) return ENAMETOOLONG;
        allocated = malloc(n + 25);
        if (!allocated) return ENOMEM;
        snprintf(allocated, n + 25, "%s/.oheco/broker/endpoint", home); path = allocated;
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC), saved = errno;
    free(allocated);
    if (fd < 0) return saved;
    struct stat st;
    if (fstat(fd, &st) != 0) { saved = errno; close(fd); return saved; }
    if (!S_ISREG(st.st_mode)) { close(fd); return EINVAL; }
    unsigned char b[65]; size_t n = 0;
    for (;;) {
        ssize_t got = read(fd, b + n, sizeof(b) - n);
        if (got > 0) { n += (size_t)got; if (n == sizeof(b)) break; }
        else if (got == 0) break;
        else if (errno != EINTR) { saved = errno; close(fd); return saved; }
    }
    close(fd);
    if (!n || n > 64) return EINVAL;
    if (b[n - 1] == '\n') { n--; if (n && b[n - 1] == '\r') n--; }
    if (n <= 10 || memcmp(b, "127.0.0.1:", 10)) return EINVAL;
    unsigned value = 0;
    for (size_t i = 10; i < n; i++) {
        if (b[i] < '0' || b[i] > '9') return EINVAL;
        value = value * 10 + b[i] - '0'; if (value > 65535) return EINVAL;
    }
    if (!value) return EINVAL;
    *port = value; return 0;
}

/* A detached exchange borrows the same transport/framing, never exposes a handle. */
static ob_error start_exchange(const char *endpoint_file, const ob_options *o,
                               int detached, const char *stdout_file, const char *stderr_file,
                               ob_process **out, ob_diagnostic *d) {
    *out = NULL;
    unsigned char *payload = NULL; size_t size = 0;
    ob_error e = encode_start(o, detached, stdout_file, stderr_file, &payload, &size);
    if (e) return diag(d, e, "options", 0, "Invalid options, invalid UTF-8 or oversized START");
    unsigned port = 0; int rc = discover(endpoint_file, &port);
    if (rc) { free(payload); return diag(d, OB_UNAVAILABLE, "discovery", rc, "Endpoint file unavailable or malformed"); }
    ob_process *p = calloc(1, sizeof(*p));
    if (!p) { free(payload); return diag(d, OB_IO, "start", ENOMEM, "Allocation failed"); }
    p->fd = -1; atomic_init(&p->failure, 0); atomic_init(&p->cancelled, 0); atomic_init(&p->exited, 0);
    rc = pthread_mutex_init(&p->writer, NULL);
    if (rc) { free(p); free(payload); return diag(d, OB_IO, "start", rc, "Mutex creation failed"); }
    p->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (p->fd < 0 || fcntl(p->fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(p->fd, F_SETFD, FD_CLOEXEC) < 0) {
        e = diag(d, OB_UNAVAILABLE, "connect", errno, "Cannot create nonblocking socket"); goto fail;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address)); address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int64_t end = deadline(OB_DEADLINE);
    rc = connect(p->fd, (struct sockaddr *)&address, sizeof(address));
    if (rc < 0) {
        rc = errno;
        if (rc == EINPROGRESS || rc == EINTR) {
            rc = ready(p, POLLOUT, end, 0);
            if (!rc) { socklen_t len = sizeof(rc); if (getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &rc, &len) < 0) rc = errno; }
        }
        if (rc) { e = diag(d, OB_UNAVAILABLE, "connect", rc, "Broker connection failed"); goto fail; }
    }
    end = deadline(OB_DEADLINE);
    const unsigned char *magic = (const unsigned char *)(detached ? "OHECOB2\n" : "OHECOB1\n");
    unsigned char response[8]; size_t have = 0;
    rc = send_bytes(p, magic, 8, end, 0);
    if (!rc) rc = recv_bytes(p, response, 8, &have, end);
    if (rc || memcmp(magic, response, 8)) { e = diag(d, OB_PROTOCOL, "handshake", rc, "Handshake failed"); goto fail; }
    end = deadline(OB_DEADLINE);
    rc = send_frame(p, detached ? 10 : 1, payload, size, end, 0);
    if (rc) { e = diag(d, rc == ETIMEDOUT ? OB_TIMEOUT : OB_CONNECTION_LOST, "start", rc, "START send failed; outcome may be unknown"); goto fail; }
    e = receive_frame(p, end, detached ? 2 : 1, d);
    if (e) {
        if (detached && e == OB_TIMEOUT && !atomic_load(&p->failure))
            diag(d, e, "start", ETIMEDOUT, "Detached START deadline expired; outcome unknown; do not retry");
        goto fail;
    }
    if (!detached) clear_frame(p);
    p->stdin_open = o->stdin_enabled;
    free(payload); *out = p; return diag(d, OB_OK, "start", 0, "");
fail:
    free(payload); ob_release(p); return e;
}

ob_error ob_start(const char *endpoint_file, const ob_options *o,
                  ob_process **out, ob_diagnostic *d) {
    if (!out) return diag(d, OB_INVALID_ARGUMENT, "start", 0, "Missing result pointer");
    return start_exchange(endpoint_file, o, 0, NULL, NULL, out, d);
}

ob_error ob_spawn_detached(const char *endpoint_file, const ob_options *o,
                           const char *stdout_file, const char *stderr_file,
                           uint32_t *pid, ob_diagnostic *d) {
    if (!pid) return diag(d, OB_INVALID_ARGUMENT, "start", 0, "Missing PID pointer");
    *pid = 0;
    ob_process *p = NULL;
    ob_error e = start_exchange(endpoint_file, o, 1,
                                stdout_file ? stdout_file : "",
                                stderr_file ? stderr_file : "", &p, d);
    if (e) return e;
    uint32_t value = get32(p->payload);
    if (!value || value > INT32_MAX)
        e = diag(d, OB_PROTOCOL, "start", 0, "Invalid detached PID");
    else *pid = value;
    /* Close immediately after ACK; no EOF wait, CANCEL, or managed fallback. */
    ob_release(p);
    return e;
}

static ob_error input(ob_process *p, const void *data, size_t size, int eof, ob_diagnostic *d) {
    if (!p || (!data && size)) return diag(d, OB_INVALID_ARGUMENT, "stdin", 0, "Invalid input argument");
    pthread_mutex_lock(&p->writer);
    ob_error e = (ob_error)atomic_load(&p->failure);
    int rc = 0;
    if (!e && (atomic_load(&p->cancelled) || atomic_load(&p->exited))) e = OB_IO;
    if (!e && !p->stdin_open) e = OB_INVALID_ARGUMENT;
    int64_t end = deadline(OB_DEADLINE);
    const unsigned char *b = data;
    if (!e && eof) rc = send_frame(p, 4, NULL, 0, end, 0);
    while (!e && !rc && size) {
        /* Never interrupt an individual frame: CANCEL must not split STDIN. */
        if (atomic_load(&p->cancelled)) { e = OB_IO; break; }
        size_t n = size > OB_MAX_STREAM ? OB_MAX_STREAM : size;
        rc = send_frame(p, 3, b, n, end, 0); b += n; size -= n;
    }
    if (rc) e = poison(p, rc == ETIMEDOUT ? OB_TIMEOUT : rc == ECANCELED ? OB_IO : OB_CONNECTION_LOST);
    if (!e && eof) p->stdin_open = 0;
    pthread_mutex_unlock(&p->writer);
    return diag(d, e, "stdin", rc, e ? "Input failed or closed; transport failures close connection" : "");
}
ob_error ob_write_stdin(ob_process *p, const void *data, size_t n, ob_diagnostic *d) { return input(p, data, n, 0, d); }
ob_error ob_close_stdin(ob_process *p, ob_diagnostic *d) { return input(p, NULL, 0, 1, d); }

ob_error ob_cancel(ob_process *p, ob_diagnostic *d) {
    if (!p) return diag(d, OB_INVALID_ARGUMENT, "cancel", 0, "Missing process");
    atomic_store(&p->cancelled, 1);
    pthread_mutex_lock(&p->writer);
    ob_error e = (ob_error)atomic_load(&p->failure);
    int rc = 0;
    if (!e && !atomic_load(&p->exited)) rc = send_frame(p, 7, NULL, 0, deadline(OB_DEADLINE), 0);
    if (rc) e = poison(p, rc == ETIMEDOUT ? OB_TIMEOUT : OB_CONNECTION_LOST);
    pthread_mutex_unlock(&p->writer);
    return diag(d, e, "cancel", rc, e ? "Cancellation transport failed; connection closed" : "");
}

static ob_error read_event_until(ob_process *p, int64_t end, ob_event *event, ob_diagnostic *d) {
    memset(event, 0, sizeof(*event));
    if (atomic_load(&p->exited)) { event->type = OB_EXIT; event->result = p->result; return diag(d, OB_OK, "read", 0, ""); }
    ob_error e = (ob_error)atomic_load(&p->failure);
    if (e) return diag(d, e, "read", 0, "Connection is terminal");
    if (p->header_have == 5 && p->payload && p->payload_have == p->payload_size) clear_frame(p);
    e = receive_frame(p, end, 0, d);
    if (e) return e;
    event->type = (ob_event_type)p->header[0];
    if (event->type == OB_EXIT) {
        uint32_t reason = get32(p->payload), code = get32(p->payload + 4), signal_number = get32(p->payload + 8);
        int normal = code <= 255 && signal_number == 0;
        int signalled = code == UINT32_MAX && signal_number > 0;
        if (!((reason == 0 && normal) || (reason == 1 && signalled) || (reason == 2 && (normal || signalled))))
            return diag(d, poison(p, OB_PROTOCOL), "exit", 0, "Invalid EXIT payload");
        p->result.reason = reason; p->result.exit_code = code == UINT32_MAX ? -1 : (int32_t)code; p->result.signal = signal_number;
        event->result = p->result; atomic_store(&p->exited, 1);
    } else { event->data = p->payload; event->size = p->payload_size; }
    return diag(d, OB_OK, "read", 0, "");
}
ob_error ob_read_event(ob_process *p, int timeout, ob_event *event, ob_diagnostic *d) {
    if (!p || !event || timeout < -1) return diag(d, OB_INVALID_ARGUMENT, "read", 0, "Invalid reader argument");
    return read_event_until(p, deadline(timeout), event, d);
}
ob_error ob_wait(ob_process *p, int timeout, ob_exit *result, ob_diagnostic *d) {
    if (!p || !result || timeout < -1) return diag(d, OB_INVALID_ARGUMENT, "wait", 0, "Invalid wait argument");
    int64_t end = deadline(timeout);
    for (;;) {
        ob_event event; ob_error e = read_event_until(p, end, &event, d);
        if (e) return e;
        if (event.type == OB_EXIT) { *result = event.result; return OB_OK; }
        if (end >= 0 && remaining(end) == 0) return diag(d, OB_TIMEOUT, "wait", 0, "Wait deadline expired; command not cancelled");
    }
}
void ob_release(ob_process *p) {
    if (!p) return;
    if (p->fd >= 0) close(p->fd);
    pthread_mutex_destroy(&p->writer); free(p->payload); free(p);
}
