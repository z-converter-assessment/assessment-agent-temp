/**
 * @file worker.h
 * @brief task.install consumer state machine + idempotency / cleanup.
 *
 * One-time API surface:
 *   - worker_init       — validate config, create state dirs, do startup
 *                         scan of `/results` and `/done` (D3 cleanup).
 *   - worker_tick       — called once per main-loop tick. Polls the per-
 *                         machine queue (basic_get), forks a child if a
 *                         task is available and no child is in-flight,
 *                         reaps any finished child, publishes its result,
 *                         acks broker.
 *   - worker_begin_drain — called when SIGTERM is observed. Stops polling
 *                          new tasks; tick still drains the in-flight one.
 *   - worker_idle       — returns 1 when the worker has no in-flight child
 *                         (used by main loop to decide drain completion).
 *   - worker_shutdown   — release resources. Does NOT kill in-flight child;
 *                         drain-then-exit is the supported shutdown path.
 *
 * The worker reuses the existing collector's publish.h for outbound task.result
 * but opens a dedicated AMQP connection (CM2) with the worker credentials.
 */

#ifndef ASSESSMENT_AGENT_WORKER_H
#define ASSESSMENT_AGENT_WORKER_H

#include "publish.h"

#include <stddef.h>

typedef struct worker_ctx_s worker_ctx_t;

typedef struct {
	publish_config_t amqp;                /* AMQP target + worker creds */
	const char      *queue_name;          /* e.g. "agent.tasks.<machine_id>" */
	const char      *result_routing_key;  /* e.g. "task.result" */
	const char      *machine_id;
	const char      *agent_version;
	const char      *state_dir;           /* e.g. "/var/lib/agent-worker" */
	const char      *tmp_dir;             /* e.g. "/tmp" */
	const char      *allowed_hosts_csv;   /* WORKER_DOWNLOAD_ALLOWED_HOSTS */
	int              done_retention_sec;
	int              disk_reserve_mb;
	int              mem_limit_mb;
	int              fsize_limit_mb;
	int              nofile_limit;       /* RLIMIT_NOFILE for install.sh; raw fd count */
} worker_config_t;

/**
 * @brief Initialize the worker subsystem.
 *
 * Creates state directories, scans `/results` for previously-completed
 * tasks whose result was never published (publishes them now + moves to
 * `/done`), purges expired `/done/*.json` files, and connects to the
 * broker. On failure the returned pointer is NULL.
 */
worker_ctx_t *worker_init(const worker_config_t *cfg);

/**
 * @brief One main-loop step. See worker.c for the state machine.
 *
 * Non-blocking. Returns 0 on success, -1 on irrecoverable error (caller
 * should log and continue — worker is best-effort).
 */
int worker_tick(worker_ctx_t *ctx);

/**
 * @brief Keep the worker AMQP connection alive while the main loop sleeps.
 *
 * librabbitmq sends heartbeats from inside `amqp_simple_wait_frame_noblock`,
 * so the connection dies if we sleep longer than the negotiated heartbeat
 * interval without calling into the library (CRITICAL #1). Call this
 * helper from short sleep intervals to keep heartbeats flowing.
 */
void worker_keepalive(worker_ctx_t *ctx);

/**
 * @brief Signal that SIGTERM was received — stop accepting new tasks.
 */
void worker_begin_drain(worker_ctx_t *ctx);

/**
 * @brief Send SIGTERM (or SIGKILL on @p hard) to the in-flight child's
 *        process group. No-op if no child is in-flight. Used by the main
 *        loop to bound drain time when the install script ignores normal
 *        shutdown (CRITICAL #9).
 */
void worker_force_child_term(worker_ctx_t *ctx, int hard);

/**
 * @brief Return 1 when no child is in-flight AND no result publish is pending.
 *
 * Round 4: stricter than just "no live child". After a successful reap,
 * the result file may still be sitting in /results awaiting publish; the
 * drain loop keeps spinning until that publish succeeds (or H3 publish-
 * stuck deadline fires).
 */
int worker_idle(const worker_ctx_t *ctx);

/**
 * @brief Return 1 when an OS child process is still being awaited.
 *
 * Used by the drain loop to detect "child reaped but publish pending"
 * (worker_idle still false but no kill target) so escalation timers
 * don't spin uselessly.
 */
int worker_has_live_child(const worker_ctx_t *ctx);

/**
 * @brief Free resources. Caller must have already waited for the in-flight
 *        child to complete (drain pattern, see C5).
 */
void worker_shutdown(worker_ctx_t *ctx);

#endif
