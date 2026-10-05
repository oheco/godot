#ifndef OHECO_BROKER_H
#define OHECO_BROKER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ob_error {
    OB_OK = 0, OB_UNAVAILABLE = 1, OB_PROTOCOL = 2, OB_SPAWN_FAILED = 3,
    OB_CONNECTION_LOST = 4, OB_INVALID_ARGUMENT = 5, OB_TIMEOUT = 6,
    OB_IO = 7, OB_LIMIT = 8
} ob_error;

typedef struct ob_diagnostic {
    ob_error code;
    int native_error;
    char stage[24];
    char message[256];
} ob_diagnostic;

typedef struct ob_env { const char *key; const char *value; } ob_env;
typedef struct ob_options {
    const char *executable;
    const char *cwd;               /* NULL or empty: broker startup cwd */
    const char *const *args;       /* Arguments EXCLUDING argv[0]. */
    size_t argc;
    const ob_env *env;             /* Inherited environment overrides. */
    size_t envc;
    int stdin_enabled;            /* Exactly 0 or 1. */
} ob_options;

typedef struct ob_process ob_process;
typedef struct ob_exit {
    uint32_t reason;               /* 0 normal, 1 signal, 2 cancelled */
    int32_t exit_code;
    uint32_t signal;
} ob_exit;
typedef enum ob_event_type { OB_STDOUT = 5, OB_STDERR = 6, OB_EXIT = 8 } ob_event_type;
typedef struct ob_event {
    ob_event_type type;
    const unsigned char *data;     /* Borrowed until next read/wait/release. */
    size_t size;
    ob_exit result;
} ob_event;

/* NULL endpoint_file selects $HOME/.oheco/broker/endpoint. No automatic retry.
 * Connect, handshake, and START exchange each have a 3-second deadline.
 * On error *out is NULL; diagnostics are optional and caller-owned. */
ob_error ob_start(const char *endpoint_file, const ob_options *options,
                  ob_process **out, ob_diagnostic *diagnostic);
/* Detached startup uses v2 only; managed ob_start remains wire v1.
 * stdin_enabled MUST be 0. NULL/empty log paths use the null sink; nonempty
 * paths append/create regular files, relative to the effective remote cwd.
 * Returns a diagnostic PID (1..INT32_MAX), NOT a local child or wait/cancel
 * handle. *pid is 0 on error; pid is required, diagnostic is optional.
 * Closes transport after ACK. A successful OS spawn survives disconnect and
 * normal broker shutdown, but terminal/OS force-stop survival is not promised.
 * After attempting START, loss/timeout has UNKNOWN outcome: never retry or
 * fall back to managed mode. Old servers reject v2 with PROTOCOL safely.
 * Discovery/connect failures are UNAVAILABLE; phase deadlines are 3 seconds.
 * All option/path strings are borrowed only for this call. */
ob_error ob_spawn_detached(const char *endpoint_file, const ob_options *options,
                           const char *stdout_file, const char *stderr_file,
                           uint32_t *pid, ob_diagnostic *diagnostic);
/* At most one event reader/waiter, and one stdin producer, per process.
 * cancel may run concurrently with either; release must not run concurrently.
 * A write has a 3-second total I/O budget. Failure may have sent partial input.
 * A failed/partial frame poisons the connection rather than risking corruption.
 * Cancellation takes priority over further stdin writes; these then return IO. */
ob_error ob_write_stdin(ob_process *process, const void *data, size_t size,
                        ob_diagnostic *diagnostic);
ob_error ob_close_stdin(ob_process *process, ob_diagnostic *diagnostic);
/* timeout_ms: -1 infinite, 0 poll, positive total call budget. TIMEOUT leaves
 * the command running and preserves partial frames. EXIT is repeatable.
 * Continuously drain events, or use wait (which discards unread output). */
ob_error ob_read_event(ob_process *process, int timeout_ms, ob_event *event,
                       ob_diagnostic *diagnostic);
ob_error ob_wait(ob_process *process, int timeout_ms, ob_exit *result,
                 ob_diagnostic *diagnostic);
/* Requests remote cancellation; does not wait for EXIT. Idempotent. */
ob_error ob_cancel(ob_process *process, ob_diagnostic *diagnostic);
/* Closes connection; active remote command is cancelled by the broker. */
void ob_release(ob_process *process);
const char *ob_error_name(ob_error error);

/* Stable descriptive public spellings; short OB_/ob_ names remain aliases.
 * This SDK is source-embedded, so these introduce no extra binary ABI. */
typedef ob_error oheco_broker_error;
typedef ob_diagnostic oheco_broker_diagnostic;
typedef ob_env oheco_broker_env;
typedef ob_options oheco_broker_options;
typedef ob_process oheco_broker_process;
typedef ob_exit oheco_broker_exit;
typedef ob_event_type oheco_broker_event_type;
typedef ob_event oheco_broker_event;
#define OHECO_BROKER_OK OB_OK
#define OHECO_BROKER_ERR_UNAVAILABLE OB_UNAVAILABLE
#define OHECO_BROKER_ERR_PROTOCOL OB_PROTOCOL
#define OHECO_BROKER_ERR_SPAWN_FAILED OB_SPAWN_FAILED
#define OHECO_BROKER_ERR_CONNECTION_LOST OB_CONNECTION_LOST
#define OHECO_BROKER_ERR_INVALID_ARGUMENT OB_INVALID_ARGUMENT
#define OHECO_BROKER_ERR_TIMEOUT OB_TIMEOUT
#define OHECO_BROKER_ERR_IO OB_IO
#define OHECO_BROKER_ERR_LIMIT OB_LIMIT
#define OHECO_BROKER_EVENT_STDOUT OB_STDOUT
#define OHECO_BROKER_EVENT_STDERR OB_STDERR
#define OHECO_BROKER_EVENT_EXIT OB_EXIT
#define oheco_broker_start ob_start
#define oheco_broker_spawn_detached ob_spawn_detached
#define oheco_broker_write_stdin ob_write_stdin
#define oheco_broker_close_stdin ob_close_stdin
#define oheco_broker_read_event ob_read_event
#define oheco_broker_wait ob_wait
#define oheco_broker_cancel ob_cancel
#define oheco_broker_release ob_release
#define oheco_broker_error_name ob_error_name

#ifdef __cplusplus
}
#endif
#endif
