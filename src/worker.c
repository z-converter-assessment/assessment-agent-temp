/**
 * @file worker.c
 * @brief task.install consumer + fork/reap + idempotency state machine.
 *
 * State (per ctx):
 *   IDLE  — no child in flight. tick polls basic_get on the per-machine queue.
 *           If empty → return. If message → fork child → enter BUSY.
 *   BUSY  — child PID set. tick reaps non-blocking; on reap → read result
 *           file (or synthesize on abnormal exit) → publish task.result →
 *           ack broker → move result file to `/done` → enter IDLE.
 *   DRAIN — SIGTERM observed. No new basic_get. tick reaps the in-flight
 *           child if any, then waits idle.
 *
 * Idempotency:
 *   /var/lib/agent-worker/done/<task_id>.json exists  ⇒  this task already
 *     completed in a prior run. Worker emits a synthesized result with
 *     failure_reason="already_done" and acks immediately (no fork).
 *
 * Durability:
 *   The child writes its result JSON to /var/lib/agent-worker/results/
 *   <task_id>.json *before* exiting. The parent — and only the parent —
 *   reads, publishes, acks, then moves the file to /done. A parent
 *   crash between child completion and broker ack leaves the file in
 *   /results; on next startup the worker_init scan replays it.
 */

#define _POSIX_C_SOURCE 200809L

#include "worker.h"
#include "download.h"
#include "extract.h"
#include "exec.h"
#include "util.h"

#include <cJSON.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef AGENT_VERSION
#define AGENT_VERSION "0.0.0"
#endif

/* ============================================================
 * Context
 * ============================================================ */

struct worker_ctx_s {
	worker_config_t cfg;
	publish_conn_t *conn;

	int  drain;                    /* set by worker_begin_drain */
	int  conn_dead;                /* CRITICAL #2: set on transport failure; reconnect on next tick */
	/* Round 3: monotonic clock to avoid NTP step skew. */
	struct timespec last_reconnect_attempt;
	int  reconnect_backoff_sec;    /* current backoff window */

	/* IDLE when child_pid == 0, BUSY otherwise. */
	pid_t    child_pid;
	uint64_t inflight_delivery_tag;
	char     inflight_task_id[128];

	/* Cached dir paths so we don't re-derive on every tick. */
	char results_dir[512];
	char done_dir[512];
	char running_dir[512];   /* CRITICAL #10: in-flight task markers */
};

/*
 * Backoff caps: start at 1s, double each consecutive failure, max 60s.
 * Reset to 0 on a successful reconnect.
 */
#define WORKER_RECONNECT_BACKOFF_MAX 60

/* ============================================================
 * Small filesystem helpers
 * ============================================================ */

static int mkdir_p(const char *path, mode_t mode)
{
	if (!path || !*path) return -1;
	char tmp[1024];
	size_t n = strnlen(path, sizeof tmp);
	if (n >= sizeof tmp) return -1;
	memcpy(tmp, path, n + 1);

	for (size_t i = 1; i < n; i++) {
		if (tmp[i] == '/') {
			tmp[i] = '\0';
			if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
			tmp[i] = '/';
		}
	}
	if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
	return 0;
}

static int file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/*
 * Atomic write with full durability:
 *   1. write tmp file, fsync the file fd
 *   2. rename(tmp, path) — atomic on POSIX filesystems
 *   3. fsync the parent directory so the rename is durable across power loss
 *
 * Without the dir fsync, a rename can be lost on crash even though the file
 * data is durable. /done idempotency markers must survive crash so we don't
 * silently double-execute redelivered tasks (CRITICAL #13).
 */
static int fsync_parent_dir(const char *path)
{
	char dir[1024];
	size_t n = strnlen(path, sizeof dir);
	if (n >= sizeof dir) return -1;
	memcpy(dir, path, n + 1);
	char *slash = strrchr(dir, '/');
	if (!slash) { dir[0] = '.'; dir[1] = '\0'; }
	else if (slash == dir) { dir[1] = '\0'; }    /* path is `/foo`, dir is `/` */
	else *slash = '\0';

	int dfd = open(dir, O_RDONLY | O_DIRECTORY);
	if (dfd < 0) return -1;
	int rc = fsync(dfd);
	close(dfd);
	return rc;
}

static int write_file_atomic(const char *path, const char *content)
{
	char tmp[1024];
	if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return -1;
	FILE *f = fopen(tmp, "wb");
	if (!f) return -1;
	size_t len = strlen(content);
	if (fwrite(content, 1, len, f) != len) { fclose(f); unlink(tmp); return -1; }
	if (fflush(f) != 0)                    { fclose(f); unlink(tmp); return -1; }
	int fd = fileno(f);
	if (fd >= 0 && fsync(fd) != 0)         { fclose(f); unlink(tmp); return -1; }
	if (fclose(f) != 0) { unlink(tmp); return -1; }
	if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
	(void)fsync_parent_dir(path);
	return 0;
}

static int rmrf(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode)) return unlink(path);

	DIR *d = opendir(path);
	if (!d) return -1;
	struct dirent *e;
	int rc = 0;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
		char sub[1024];
		if ((size_t)snprintf(sub, sizeof sub, "%s/%s", path, e->d_name) >= sizeof sub) {
			rc = -1; continue;
		}
		if (rmrf(sub) != 0) rc = -1;
	}
	closedir(d);
	if (rmdir(path) != 0) rc = -1;
	return rc;
}

/* ============================================================
 * task_id validation
 * ============================================================ */

/*
 * Reject any task_id that could escape the state directory or contain
 * shell metacharacters. We accept only the UUID v4 grammar (or its
 * unhyphenated 32-char form) which is what the portal always emits.
 */
static int task_id_valid(const char *id)
{
	if (!id) return 0;
	size_t n = strlen(id);
	if (n != 36 && n != 32) return 0;
	for (size_t i = 0; i < n; i++) {
		char c = id[i];
		if (n == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) {
			if (c != '-') return 0;
		} else if (!((c >= '0' && c <= '9') ||
		             (c >= 'a' && c <= 'f') ||
		             (c >= 'A' && c <= 'F'))) {
			return 0;
		}
	}
	return 1;
}

/* ============================================================
 * Result JSON build
 * ============================================================ */

static char *build_result_json(const worker_ctx_t *ctx,
                               const char *task_id,
                               const char *status,
                               const char *failure_reason,
                               int   has_exit_code, int exit_code,
                               long  duration_ms,
                               const char *stdout_tail,
                               const char *stderr_tail)
{
	cJSON *root = cJSON_CreateObject();
	if (!root) return NULL;

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	char now_buf[32];
	iso8601_utc(ts.tv_sec, now_buf, sizeof now_buf);

	cJSON_AddStringToObject(root, "message_type",     "task.result");
	cJSON_AddStringToObject(root, "machine_id",       ctx->cfg.machine_id ? ctx->cfg.machine_id : "");
	cJSON_AddStringToObject(root, "agent_version",    ctx->cfg.agent_version ? ctx->cfg.agent_version : AGENT_VERSION);
	cJSON_AddStringToObject(root, "collected_at",     now_buf);

	char host[256];
	if (gethostname(host, sizeof host) == 0) {
		host[sizeof host - 1] = '\0';
		cJSON_AddStringToObject(root, "hostname", host);
	} else {
		cJSON_AddStringToObject(root, "hostname", "unknown");
	}

	char msg_id[64];
	uuid_v4(msg_id, sizeof msg_id);
	cJSON_AddStringToObject(root, "message_id", msg_id);

	/*
	 * Round 4 F16: emit explicit nulls for boot_time / agent_started_at
	 * so engine consumers see consistent schema between task.result and
	 * inventory/metrics/error. The actual values live in collect.c's
	 * static caches; worker.c is intentionally independent of collector
	 * (CLAUDE.md "single binary single process" with worker module
	 * separately testable). Portal sees null and knows to source these
	 * from the same machine_id's most recent metric/inventory message.
	 */
	cJSON_AddNullToObject(root, "boot_time");
	cJSON_AddNullToObject(root, "agent_started_at");

	cJSON_AddStringToObject(root, "task_id", task_id ? task_id : "");
	cJSON_AddStringToObject(root, "status",  status);
	if (failure_reason && *failure_reason)
		cJSON_AddStringToObject(root, "failure_reason", failure_reason);
	else
		cJSON_AddNullToObject(root, "failure_reason");
	if (has_exit_code) cJSON_AddNumberToObject(root, "exit_code", exit_code);
	else               cJSON_AddNullToObject(root, "exit_code");
	cJSON_AddNumberToObject(root, "duration_ms", (double)duration_ms);
	cJSON_AddStringToObject(root, "stdout_tail",  stdout_tail ? stdout_tail : "");
	cJSON_AddStringToObject(root, "stderr_tail",  stderr_tail ? stderr_tail : "");
	cJSON_AddStringToObject(root, "completed_at", now_buf);

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return json;
}

static const char *reason_for_status(download_status_t  ds,
                                     extract_status_t   es,
                                     exec_status_t      xs)
{
	if (ds == DOWNLOAD_ERR_URL_NOT_ALLOWED)     return "url_not_allowed";
	if (ds == DOWNLOAD_ERR_INSUFFICIENT_DISK)   return "insufficient_disk";
	if (ds == DOWNLOAD_ERR_DOWNLOAD_FAILED)     return "download_failed";
	if (ds == DOWNLOAD_ERR_SHA256_MISMATCH)     return "sha256_mismatch";
	if (ds == DOWNLOAD_ERR_INTERNAL)            return "internal_error";

	if (es == EXTRACT_ERR_OPEN)                 return "extract_failed";
	if (es == EXTRACT_ERR_FORBIDDEN_TYPE)       return "extract_failed";
	if (es == EXTRACT_ERR_PATH_TRAVERSAL)       return "extract_failed";
	if (es == EXTRACT_ERR_WRITE)                return "extract_failed";
	if (es == EXTRACT_ERR_INTERNAL)             return "internal_error";

	if (xs == EXEC_ERR_SCRIPT_NOT_FOUND)        return "script_not_found";
	if (xs == EXEC_ERR_SCRIPT_FAILED)           return "script_failed";
	if (xs == EXEC_ERR_SCRIPT_TIMEOUT)          return "script_timeout";
	if (xs == EXEC_ERR_INTERNAL)                return "internal_error";
	return "internal_error";
}

/* ============================================================
 * Child entry — one task lifecycle
 * ============================================================ */

static void child_write_result_file(const worker_ctx_t *ctx,
                                    const char *task_id,
                                    const char *json)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s.json", ctx->results_dir, task_id);
	write_file_atomic(path, json);
}

static void child_run_task(worker_ctx_t *ctx, cJSON *task)
{
	/*
	 * CRITICAL #5 (round 2): close the inherited AMQP TLS socket and any
	 * other parent-side fds *immediately on entry*. FD_CLOEXEC fires only
	 * at execve, so without this the worker child carries the broker
	 * socket through download + extract + any helper fork-without-exec.
	 * The grandchild (install.sh) gets another sweep in exec.c, but that
	 * window is too late — libcurl/libarchive may fork before then.
	 *
	 * CRITICAL #9 (round 2): become our own process group leader so the
	 * parent's drain escalation `kill(-child_pid, SIG)` actually targets
	 * us (and our grandchild via group inheritance). Without this, the
	 * worker child shares the agent's pgid; -child_pid points to a non-
	 * existent pgid and silently returns ESRCH — drain becomes dead code.
	 */
	/*
	 * Round 3: setpgid first so drain escalation has a target group
	 * even if close_inherited_fds takes a moment.
	 */
	(void)setpgid(0, 0);
	close_inherited_fds();

	const cJSON *jid       = cJSON_GetObjectItemCaseSensitive(task, "task_id");
	const cJSON *jmachine  = cJSON_GetObjectItemCaseSensitive(task, "machine_id");
	const cJSON *jdownload = cJSON_GetObjectItemCaseSensitive(task, "download");
	const cJSON *jinstall  = cJSON_GetObjectItemCaseSensitive(task, "install");

	const char *task_id = cJSON_IsString(jid) ? jid->valuestring : NULL;
	if (!task_id_valid(task_id)) {
		_exit(2);  /* Parent will synthesize internal_error with no file. */
	}

	/* Machine_id mismatch: produce already_done-style noop so portal isn't blind. */
	if (cJSON_IsString(jmachine) && ctx->cfg.machine_id &&
	    strcmp(jmachine->valuestring, ctx->cfg.machine_id) != 0) {
		char *res = build_result_json(ctx, task_id, "failure", "internal_error",
		                              0, 0, 0, "", "machine_id mismatch — task routed in error\n");
		if (res) { child_write_result_file(ctx, task_id, res); free(res); }
		_exit(0);
	}

	const char *url        = NULL;
	const char *sha256     = NULL;
	int64_t     size_bytes = 0;
	if (cJSON_IsObject(jdownload)) {
		const cJSON *ju = cJSON_GetObjectItemCaseSensitive(jdownload, "url");
		const cJSON *jh = cJSON_GetObjectItemCaseSensitive(jdownload, "sha256");
		const cJSON *jb = cJSON_GetObjectItemCaseSensitive(jdownload, "size_bytes");
		if (cJSON_IsString(ju)) url    = ju->valuestring;
		if (cJSON_IsString(jh)) sha256 = jh->valuestring;
		if (cJSON_IsNumber(jb)) size_bytes = (int64_t)jb->valuedouble;
	}

	const char *install_type = "shell"; /* default — install.type 누락 시 backward-compat */
	const char *script  = "install.sh";
	int timeout_sec     = 600;
	const char **iargs  = NULL;
	if (cJSON_IsObject(jinstall)) {
		const cJSON *jty = cJSON_GetObjectItemCaseSensitive(jinstall, "type");
		const cJSON *js  = cJSON_GetObjectItemCaseSensitive(jinstall, "script");
		const cJSON *jt  = cJSON_GetObjectItemCaseSensitive(jinstall, "timeout_sec");
		const cJSON *ja  = cJSON_GetObjectItemCaseSensitive(jinstall, "args");
		if (cJSON_IsString(jty) && *jty->valuestring) install_type = jty->valuestring;
		if (cJSON_IsString(js)) script      = js->valuestring;
		if (cJSON_IsNumber(jt)) timeout_sec = (int)jt->valuedouble;
		if (cJSON_IsArray(ja)) {
			int n = cJSON_GetArraySize(ja);
			iargs = (const char **)calloc((size_t)n + 1, sizeof(char *));
			if (iargs) {
				for (int i = 0; i < n; i++) {
					const cJSON *e = cJSON_GetArrayItem(ja, i);
					if (cJSON_IsString(e)) iargs[i] = e->valuestring;
				}
			}
		}
	}

	/* Linux agent는 install.type="shell" 만 처리. direct_exec / msi 등 자기 OS
	 * 아닌 type 수신 시 즉시 result 발행 + ack (DLQ 회피). */
	if (strcmp(install_type, "shell") != 0) {
		char *res = build_result_json(ctx, task_id, "failure",
		                              "unsupported_install_type",
		                              0, 0, 0, "",
		                              "install.type not handled by this OS\n");
		if (res) { child_write_result_file(ctx, task_id, res); free(res); }
		free(iargs);
		_exit(0);
	}

	/* Workspace directory. */
	char work[1024];
	snprintf(work, sizeof work, "%s/agent-task-%s", ctx->cfg.tmp_dir, task_id);
	rmrf(work);
	if (mkdir(work, 0700) != 0) {
		char *res = build_result_json(ctx, task_id, "failure", "internal_error",
		                              0, 0, 0, "", "workspace mkdir failed\n");
		if (res) { child_write_result_file(ctx, task_id, res); free(res); }
		free(iargs);
		_exit(0);
	}

	struct timespec ts0;
	clock_gettime(CLOCK_MONOTONIC, &ts0);

	/* 1. Download. */
	char tar_path[1100];
	snprintf(tar_path, sizeof tar_path, "%s/package.tar", work);
	download_status_t ds = (url && sha256 && size_bytes > 0)
		? download_package(url, sha256, size_bytes,
		                   ctx->cfg.allowed_hosts_csv,
		                   ctx->cfg.tmp_dir,
		                   ctx->cfg.disk_reserve_mb,
		                   tar_path)
		: DOWNLOAD_ERR_INTERNAL;

	extract_status_t es = EXTRACT_OK;
	exec_status_t    xs = EXEC_OK;
	exec_result_t    er = { .exit_code = -1 };

	if (ds == DOWNLOAD_OK) {
		es = extract_tarball(tar_path, work);
		if (es == EXTRACT_OK) {
			xs = exec_install_script(work, script, iargs,
			                         timeout_sec,
			                         ctx->cfg.mem_limit_mb,
			                         ctx->cfg.fsize_limit_mb,
			                         ctx->cfg.nofile_limit,
			                         task_id, ctx->cfg.machine_id, &er);
		}
	}

	struct timespec ts1;
	clock_gettime(CLOCK_MONOTONIC, &ts1);
	long duration_ms = (ts1.tv_sec - ts0.tv_sec) * 1000L
	                 + (ts1.tv_nsec - ts0.tv_nsec) / 1000000L;

	int success = (ds == DOWNLOAD_OK && es == EXTRACT_OK && xs == EXEC_OK);
	const char *reason = success ? "" : reason_for_status(ds, es, xs);
	int has_exit = (xs != EXEC_ERR_SCRIPT_NOT_FOUND && xs != EXEC_ERR_INTERNAL &&
	                ds == DOWNLOAD_OK && es == EXTRACT_OK);

	char *res = build_result_json(ctx, task_id,
	                              success ? "success" : "failure",
	                              success ? "" : reason,
	                              has_exit, er.exit_code,
	                              duration_ms,
	                              er.stdout_tail, er.stderr_tail);
	if (res) {
		child_write_result_file(ctx, task_id, res);
		free(res);
	}

	rmrf(work);
	free(iargs);
	_exit(0);
}

/* ============================================================
 * Parent — publish + ack + move to /done
 * ============================================================ */

static int read_result_file(const char *path, char **out_body)
{
	*out_body = read_file_all(path);
	return *out_body ? 0 : -1;
}

static int move_to_done(const worker_ctx_t *ctx,
                        const char *task_id)
{
	char src[1024], dst[1024];
	snprintf(src, sizeof src, "%s/%s.json", ctx->results_dir, task_id);
	snprintf(dst, sizeof dst, "%s/%s.json", ctx->done_dir,    task_id);
	return rename(src, dst);
}

static int publish_result_and_ack(worker_ctx_t *ctx,
                                  const char *task_id,
                                  uint64_t delivery_tag,
                                  const char *body)
{
	int rc = publish_conn_publish(ctx->conn,
	                              ctx->cfg.amqp.exchange,
	                              ctx->cfg.result_routing_key,
	                              body, strlen(body));
	if (rc != 0) {
		fprintf(stderr, "[worker] task.result publish failed for %s — file kept in results/ for retry\n",
		        task_id);
		return -1;
	}
	/*
	 * Round 6 M5: rename(/results→/done) failure means /done won't exist.
	 * If we ack now, broker drops the message and any future redelivery
	 * (e.g. broker recovers, queue redeclares) re-executes the install.
	 * Don't ack — let broker hold the unacked tag; replay_pending_results
	 * on next startup will move /results→/done correctly.
	 */
	if (move_to_done(ctx, task_id) != 0) {
		fprintf(stderr, "[worker] move to /done failed for %s: %s — leaving unacked\n",
		        task_id, strerror(errno));
		return -1;
	}

	/*
	 * Round 5 H1: ack failure is a transport problem, NOT a task problem.
	 * The result was published and /done is durable. Mark the connection
	 * dead so the next tick reconnects, but RETURN SUCCESS so the caller
	 * clears inflight state. Broker redelivery (after consumer_timeout
	 * on the now-dead channel) hits I1 via /done and emits already_done.
	 *
	 * Returning -1 here as round 4 did caused publish_reaped_result to
	 * retry on next tick, but the result file had already been moved to
	 * /done — the retry then took the "no result file" branch and
	 * published a spurious internal_error synth, contradicting the real
	 * result the portal had already received.
	 */
	if (delivery_tag != 0 && publish_conn_ack(ctx->conn, delivery_tag) != 0) {
		fprintf(stderr, "[worker] basic.ack failed for %s — task is durably done; broker will redeliver, I1 will gate\n",
		        task_id);
		ctx->conn_dead = 1;
	}
	return 0;
}

static int publish_synth_failure(worker_ctx_t *ctx,
                                 const char *task_id,
                                 uint64_t delivery_tag,
                                 const char *reason,
                                 const char *err_tail)
{
	char *body = build_result_json(ctx, task_id, "failure", reason,
	                               0, 0, 0, "", err_tail ? err_tail : "");
	if (!body) return -1;

	/*
	 * Order matters for crash safety (CRITICAL #13):
	 *   1. write /done marker durably first so any redelivery is gated by I1
	 *   2. publish to broker
	 *   3. ack
	 *
	 * Round 6 M5: /done write failure (ENOSPC, EROFS, perms) MUST abort
	 * the publish. Otherwise broker is acked but no /done marker exists,
	 * and a future redelivery would re-execute the task — the very
	 * idempotency hole this ordering exists to close.
	 */
	char dst[1024];
	if ((size_t)snprintf(dst, sizeof dst, "%s/%s.json", ctx->done_dir, task_id) >= sizeof dst) {
		free(body);
		return -1;
	}
	if (write_file_atomic(dst, body) != 0) {
		fprintf(stderr, "[worker] /done write failed for %s — aborting synth (broker will redeliver)\n",
		        task_id);
		free(body);
		return -1;
	}

	int rc = publish_conn_publish(ctx->conn,
	                              ctx->cfg.amqp.exchange,
	                              ctx->cfg.result_routing_key,
	                              body, strlen(body));
	free(body);
	if (rc != 0) return -1;
	/*
	 * Round 5 H2: same fix as publish_result_and_ack. /done was already
	 * written before publish; ack failure is purely transport. Mark conn
	 * dead but return success so caller clears inflight. Broker redelivery
	 * hits I1 via /done.
	 */
	if (delivery_tag != 0 && publish_conn_ack(ctx->conn, delivery_tag) != 0) {
		fprintf(stderr, "[worker] synth ack failed for %s — /done is durable; broker will redeliver, I1 will gate\n",
		        task_id);
		ctx->conn_dead = 1;
	}
	return 0;
}

/* ============================================================
 * Startup cleanup (D3)
 * ============================================================ */

static void purge_expired_done(const worker_ctx_t *ctx)
{
	DIR *d = opendir(ctx->done_dir);
	if (!d) return;
	time_t now = time(NULL);
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		char path[1024];
		if ((size_t)snprintf(path, sizeof path, "%s/%s", ctx->done_dir, e->d_name) >= sizeof path) continue;
		struct stat st;
		if (stat(path, &st) != 0) continue;
		if (now - st.st_mtime > (time_t)ctx->cfg.done_retention_sec) unlink(path);
	}
	closedir(d);
}

/*
 * Filename must look like `<task_id>.json` where task_id passes the same
 * validator used for incoming messages. Anything else (write_file_atomic
 * leftovers like `*.tmp`, hand-dropped files, files with embedded
 * separators) is left untouched.
 */
static int replay_filename_to_task_id(const char *fname, char *out, size_t out_sz)
{
	size_t n = strlen(fname);
	const char *suffix = ".json";
	size_t slen = strlen(suffix);
	if (n <= slen) return 0;
	if (strcmp(fname + n - slen, suffix) != 0) return 0;
	size_t base = n - slen;
	if (base >= out_sz) return 0;
	memcpy(out, fname, base);
	out[base] = '\0';
	return task_id_valid(out);
}

static void replay_pending_results(worker_ctx_t *ctx)
{
	DIR *d = opendir(ctx->results_dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;

		char task_id[64];
		if (!replay_filename_to_task_id(e->d_name, task_id, sizeof task_id)) {
			fprintf(stderr, "[worker] replay: skipping unrecognized file '%s'\n", e->d_name);
			continue;
		}

		char path[1024];
		if ((size_t)snprintf(path, sizeof path, "%s/%s", ctx->results_dir, e->d_name) >= sizeof path) continue;

		char *body = NULL;
		if (read_result_file(path, &body) != 0 || !body) continue;
		int rc = publish_conn_publish(ctx->conn,
		                              ctx->cfg.amqp.exchange,
		                              ctx->cfg.result_routing_key,
		                              body, strlen(body));
		free(body);
		if (rc != 0) {
			fprintf(stderr, "[worker] replay publish failed for %s — left for next startup\n", e->d_name);
			continue;
		}
		/* Move /results → /done. The original broker delivery_tag is
		 * long gone after our crash; the redelivered task (if any) hits
		 * the /done marker and is acked + skipped in worker_tick. */
		char done_path[1024];
		snprintf(done_path, sizeof done_path, "%s/%s.json", ctx->done_dir, task_id);
		rename(path, done_path);
	}
	closedir(d);
}

static void purge_stale_workspaces(const worker_ctx_t *ctx)
{
	DIR *d = opendir(ctx->cfg.tmp_dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, "agent-task-", 11) != 0) continue;
		char path[1024];
		if ((size_t)snprintf(path, sizeof path, "%s/%s", ctx->cfg.tmp_dir, e->d_name) >= sizeof path) continue;
		rmrf(path);
	}
	closedir(d);
}

/*
 * CRITICAL #10: any /running/<task_id> marker present at startup means a
 * previous agent instance died mid-install. We can't know whether the
 * child finished or not; treat as crashed-during-install. Publish a synth
 * failure (best-effort — connection may not be up yet on first init), and
 * move marker to /done so future redeliveries are gated by I1.
 */
static void recover_stale_running(worker_ctx_t *ctx)
{
	DIR *d = opendir(ctx->running_dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		char task_id[64];
		if (!replay_filename_to_task_id(e->d_name, task_id, sizeof task_id)) continue;

		/*
		 * HIGH-C (round 2): if /done/<id>.json or /results/<id>.json
		 * already exists, the task either completed in a prior run or
		 * was just replayed by replay_pending_results. Either way the
		 * authoritative result is already on disk — do NOT overwrite
		 * with a synth failure body. Just unlink the stale /running
		 * marker and move on.
		 */
		char done_path[1024], results_path[1024], src_path[1024];
		int have_done    = 0, have_results = 0;
		if ((size_t)snprintf(done_path,    sizeof done_path,    "%s/%s.json", ctx->done_dir,    task_id) < sizeof done_path)
			have_done = file_exists(done_path);
		if ((size_t)snprintf(results_path, sizeof results_path, "%s/%s.json", ctx->results_dir, task_id) < sizeof results_path)
			have_results = file_exists(results_path);

		int safe_to_unlink_running = 1;

		if (!have_done && !have_results) {
			char *body = build_result_json(ctx, task_id, "failure", "internal_error",
			                               0, 0, 0, "",
			                               "agent terminated mid-install; recovered on restart\n");
			if (!body) {
				/* OOM: leave /running in place; next startup retries. */
				safe_to_unlink_running = 0;
			} else {
				/*
				 * Round 3 CRIT-N3+N4: write /done BEFORE publish (same
				 * pattern as publish_synth_failure). If the write fails
				 * (disk full, RO mount, perms), do NOT publish and do
				 * NOT unlink /running — the next startup retries. Without
				 * this guard we could end up with /done missing AND
				 * /running unlinked → broker redelivers → fresh install
				 * of an already-attempted task.
				 */
				if (write_file_atomic(done_path, body) != 0) {
					fprintf(stderr, "[worker] recover: /done write failed for %s — leaving /running for next startup\n", task_id);
					safe_to_unlink_running = 0;
				} else {
					/* /done is durable. Best-effort publish — failure is
					 * acceptable because next startup will republish via
					 * replay_pending_results path (no, actually /done is
					 * already written so I1 will skip). The portal may
					 * never see this synth-failure if publish fails AND
					 * broker never redelivers — accepted as "best-effort
					 * recovery on a permanently-broken connection". */
					(void)publish_conn_publish(ctx->conn,
					                           ctx->cfg.amqp.exchange,
					                           ctx->cfg.result_routing_key,
					                           body, strlen(body));
				}
				free(body);
			}
		} else {
			fprintf(stderr, "[worker] recover: task %s has prior result on disk — skipping synth (done=%d results=%d)\n",
			        task_id, have_done, have_results);
		}

		if (safe_to_unlink_running &&
		    (size_t)snprintf(src_path, sizeof src_path, "%s/%s", ctx->running_dir, e->d_name) < sizeof src_path)
			unlink(src_path);
	}
	closedir(d);
}

/*
 * Returns 1 if a /running/<task_id> marker currently exists. Used by
 * try_pick_new_task to detect mid-install redeliveries (broker
 * consumer_timeout fired while child was still running). The current
 * parent will publish the result when the child reaps, so the redelivery
 * just acks and drops with no extra publish.
 */
static int running_marker_present(const worker_ctx_t *ctx, const char *task_id)
{
	char path[1024];
	if ((size_t)snprintf(path, sizeof path, "%s/%s.json", ctx->running_dir, task_id) >= sizeof path)
		return 0;
	return file_exists(path);
}

/*
 * Returns 0 on success, -1 on failure. CRITICAL #10 (round 2): caller
 * MUST abort the fork on failure — without the marker, a redelivery
 * during in-flight install will not be gated by I1 and double-execution
 * becomes possible (the original C10 race uncovered).
 */
static int write_running_marker(const worker_ctx_t *ctx, const char *task_id)
{
	char path[1024];
	if ((size_t)snprintf(path, sizeof path, "%s/%s.json", ctx->running_dir, task_id) >= sizeof path)
		return -1;

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	char now_buf[32];
	iso8601_utc(ts.tv_sec, now_buf, sizeof now_buf);

	char body[256];
	snprintf(body, sizeof body,
	         "{\"task_id\":\"%s\",\"started_at\":\"%s\"}\n", task_id, now_buf);
	return write_file_atomic(path, body);
}

static void clear_running_marker(const worker_ctx_t *ctx, const char *task_id)
{
	char path[1024];
	if ((size_t)snprintf(path, sizeof path, "%s/%s.json", ctx->running_dir, task_id) >= sizeof path) return;
	(void)unlink(path);
}

/* ============================================================
 * Public API
 * ============================================================ */

worker_ctx_t *worker_init(const worker_config_t *cfg)
{
	if (!cfg || !cfg->machine_id || !cfg->queue_name) return NULL;
	worker_ctx_t *ctx = (worker_ctx_t *)calloc(1, sizeof *ctx);
	if (!ctx) return NULL;
	ctx->cfg = *cfg;

	snprintf(ctx->results_dir, sizeof ctx->results_dir, "%s/results", ctx->cfg.state_dir);
	snprintf(ctx->done_dir,    sizeof ctx->done_dir,    "%s/done",    ctx->cfg.state_dir);
	snprintf(ctx->running_dir, sizeof ctx->running_dir, "%s/running", ctx->cfg.state_dir);

	if (mkdir_p(ctx->results_dir, 0700) != 0 ||
	    mkdir_p(ctx->done_dir,    0700) != 0 ||
	    mkdir_p(ctx->running_dir, 0700) != 0) {
		fprintf(stderr, "[worker] state directory init failed: %s\n", strerror(errno));
		free(ctx);
		return NULL;
	}

	ctx->conn = publish_conn_open(&ctx->cfg.amqp);
	if (!ctx->conn) {
		fprintf(stderr, "[worker] AMQP connection (agent-worker) failed\n");
		free(ctx);
		return NULL;
	}

	purge_stale_workspaces(ctx);
	replay_pending_results(ctx);
	recover_stale_running(ctx);
	purge_expired_done(ctx);

	fprintf(stderr, "[worker] initialized — queue=%s exchange=%s\n",
	        ctx->cfg.queue_name, ctx->cfg.amqp.exchange);
	return ctx;
}

void worker_begin_drain(worker_ctx_t *ctx)
{
	if (ctx) ctx->drain = 1;
}

void worker_force_child_term(worker_ctx_t *ctx, int hard)
{
	if (!ctx || ctx->child_pid == 0) return;
	int sig = hard ? SIGKILL : SIGTERM;
	pid_t pgid = ctx->child_pid;
	(void)kill(-pgid, sig);    /* ESRCH = pgid already gone — fine. */
	fprintf(stderr, "[worker] forced %s to child pgid=%d\n",
	        hard ? "SIGKILL" : "SIGTERM", (int)pgid);
}

int worker_idle(const worker_ctx_t *ctx)
{
	/*
	 * Round 4 F1+F2: idle requires BOTH no live child AND no pending
	 * publish. After round-3's reap split, child_pid==0 alone means
	 * "OS process reaped" but the result may still be sitting in
	 * /results awaiting a successful publish_reaped_result. Drain
	 * exiting at child_pid==0 alone would orphan that result until
	 * next agent restart.
	 */
	return ctx && ctx->child_pid == 0 && ctx->inflight_task_id[0] == '\0';
}

int worker_has_live_child(const worker_ctx_t *ctx)
{
	return ctx && ctx->child_pid != 0;
}

void worker_shutdown(worker_ctx_t *ctx)
{
	if (!ctx) return;
	if (ctx->conn) publish_conn_close(ctx->conn);
	free(ctx);
}

/* ============================================================
 * worker_tick — one main-loop step
 * ============================================================ */

/*
 * Publish the result for the just-reaped child. Called only after
 * reap_child_only returned 1 (child reaped) AND the AMQP connection
 * is alive. Reads /results/<task_id>.json (or synthesizes a failure
 * if missing), publishes, acks, moves to /done, clears /running.
 */
static int publish_reaped_result(worker_ctx_t *ctx)
{
	if (ctx->inflight_task_id[0] == '\0') return 0;  /* nothing to publish */

	const char *task_id = ctx->inflight_task_id;
	uint64_t    tag     = ctx->inflight_delivery_tag;

	char path[1024];
	snprintf(path, sizeof path, "%s/%s.json", ctx->results_dir, task_id);

	int publish_rc;
	if (file_exists(path)) {
		char *body = NULL;
		if (read_result_file(path, &body) == 0 && body) {
			publish_rc = publish_result_and_ack(ctx, task_id, tag, body);
			free(body);
		} else {
			publish_rc = publish_synth_failure(ctx, task_id, tag, "internal_error",
			                                   "result file unreadable after child exit\n");
		}
	} else {
		/* Child died before writing the file (signal / OOM / abort). */
		publish_rc = publish_synth_failure(ctx, task_id, tag, "internal_error",
		                                   "worker child exited without writing result\n");
	}

	if (publish_rc != 0) {
		/* Leave inflight_task_id intact so a later tick can retry. */
		return -1;
	}

	/* Success: clear /running and inflight state. */
	clear_running_marker(ctx, task_id);
	ctx->inflight_delivery_tag   = 0;
	ctx->inflight_task_id[0]     = '\0';
	return 0;
}

static int try_pick_new_task(worker_ctx_t *ctx)
{
	/*
	 * Round 4 F1+F2: also bail when a previous task's publish is still
	 * pending (inflight_task_id non-empty). Otherwise basic_get could
	 * fetch task Y and overwrite inflight_task_id, orphaning task X's
	 * result file in /results.
	 */
	if (ctx->drain || ctx->child_pid != 0 || ctx->inflight_task_id[0] != '\0')
		return 0;

	char    *body = NULL;
	size_t   blen = 0;
	uint64_t tag  = 0;
	int rc = publish_conn_get(ctx->conn, ctx->cfg.queue_name,
	                          &body, &blen, &tag);
	if (rc == 1) return 0;         /* queue empty */
	if (rc < 0)  return -1;        /* connection-level failure */

	cJSON *task = cJSON_Parse(body);
	if (!task) {
		fprintf(stderr, "[worker] malformed task — dropping (ack)\n");
		if (publish_conn_ack(ctx->conn, tag) != 0) ctx->conn_dead = 1;
		free(body);
		return 0;
	}

	const cJSON *jid = cJSON_GetObjectItemCaseSensitive(task, "task_id");
	const char *task_id = cJSON_IsString(jid) ? jid->valuestring : NULL;
	if (!task_id_valid(task_id)) {
		fprintf(stderr, "[worker] invalid task_id — dropping (ack)\n");
		if (publish_conn_ack(ctx->conn, tag) != 0) ctx->conn_dead = 1;
		cJSON_Delete(task);
		free(body);
		return 0;
	}

	/* Idempotency: /done marker present → skip install.
	 * Round 4 F4+F5: capture publish/ack rc; mark conn_dead on failure
	 * so the next tick reconnects instead of attempting basic_get on a
	 * corrupted channel. */
	char done_path[1024];
	snprintf(done_path, sizeof done_path, "%s/%s.json", ctx->done_dir, task_id);
	if (file_exists(done_path)) {
		fprintf(stderr, "[worker] task %s already_done — synthesizing result\n", task_id);
		char *res = build_result_json(ctx, task_id, "failure", "already_done",
		                              0, 0, 0, "", "");
		if (!res) {
			/*
			 * Round 5: OOM building synth body. Do NOT ack — leave the
			 * message unacked so broker redelivers when memory recovers.
			 * Acking without publishing would make portal believe the task
			 * never reached the agent.
			 */
			fprintf(stderr, "[worker] OOM building already_done synth for %s — leaving unacked\n", task_id);
			cJSON_Delete(task);
			free(body);
			return 0;
		}
		int pub_rc = publish_conn_publish(ctx->conn,
		                                  ctx->cfg.amqp.exchange,
		                                  ctx->cfg.result_routing_key,
		                                  res, strlen(res));
		free(res);
		if (pub_rc != 0) {
			/* publish failed → don't ack; broker redelivers. */
			ctx->conn_dead = 1;
			cJSON_Delete(task);
			free(body);
			return 0;
		}
		if (publish_conn_ack(ctx->conn, tag) != 0) ctx->conn_dead = 1;
		cJSON_Delete(task);
		free(body);
		return 0;
	}

	/*
	 * CRITICAL #10: redelivery while a previous attempt is still running.
	 * The /running marker says some agent (this one or a previous instance
	 * before crash) is mid-install. The original parent will publish the
	 * result; this redelivery should just be acked and dropped without
	 * publishing anything to avoid double results. If the original parent
	 * is dead, recover_stale_running on next startup will publish the synth
	 * failure for that abandoned task.
	 */
	if (running_marker_present(ctx, task_id)) {
		fprintf(stderr, "[worker] task %s already in-flight — redelivery dropped\n", task_id);
		if (publish_conn_ack(ctx->conn, tag) != 0) ctx->conn_dead = 1;
		cJSON_Delete(task);
		free(body);
		return 0;
	}

	/* Write /running marker BEFORE fork so a redelivery during this run
	 * (broker consumer_timeout, agent restart) can detect the in-flight
	 * state and not double-execute. CRITICAL #10 (round 2): if marker
	 * write fails (disk full, RO mount, perms), DO NOT fork — there is
	 * no way to gate the redelivery race. Synthesize a failure result
	 * so portal sees the task didn't run. */
	if (write_running_marker(ctx, task_id) != 0) {
		fprintf(stderr, "[worker] task %s: /running marker write failed — aborting\n", task_id);
		if (publish_synth_failure(ctx, task_id, tag, "internal_error",
		                          "agent could not write /running marker; install skipped to prevent double-execution\n") != 0)
			ctx->conn_dead = 1;
		cJSON_Delete(task);
		free(body);
		return 0;
	}

	/*
	 * Round 4 F3 + Round 5: fork() failure is a LOCAL resource issue
	 * (EAGAIN under memory/process pressure), not a broker problem.
	 * publish_synth_failure writes /done BEFORE publish, so we clear the
	 * /running marker only AFTER synth has at least committed /done. If
	 * synth itself OOMs (no /done written, no publish), we leave /running
	 * in place so the next startup's recover_stale_running can publish
	 * the synth properly — preventing the double-execute hole where both
	 * /running and /done would be missing.
	 */
	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "[worker] fork failed: %s\n", strerror(errno));
		int synth_rc = publish_synth_failure(ctx, task_id, tag, "internal_error",
		                                     "agent could not fork worker child\n");
		if (synth_rc == 0) {
			/* /done is durable; safe to drop the in-flight marker. */
			clear_running_marker(ctx, task_id);
		} else {
			/* synth failed (publish or OOM); leave /running for next-startup recovery. */
			ctx->conn_dead = 1;
		}
		cJSON_Delete(task);
		free(body);
		return 0;
	}
	if (pid == 0) {
		/*
		 * Child: do not double-close the AMQP connection on _exit. The
		 * actual fd closure happens via close_inherited_fds() at the top
		 * of child_run_task (round 2 CRIT-E).
		 */
		ctx->conn = NULL;
		child_run_task(ctx, task);
		_exit(0);                               /* unreachable */
	}

	/*
	 * Parent: also call setpgid(child_pid, child_pid) to close the race
	 * where drain runs before the child's own setpgid takes effect (round 3
	 * C-2). One of the two calls is redundant; either succeeding is enough.
	 */
	(void)setpgid(pid, pid);

	ctx->child_pid             = pid;
	ctx->inflight_delivery_tag = tag;
	strncpy(ctx->inflight_task_id, task_id, sizeof ctx->inflight_task_id - 1);
	ctx->inflight_task_id[sizeof ctx->inflight_task_id - 1] = '\0';

	fprintf(stderr, "[worker] task %s started — pid=%d\n", task_id, (int)pid);

	cJSON_Delete(task);
	free(body);
	return 0;
}

/*
 * CRITICAL #2 + #3: reconnect after transport / channel failures.
 * Closing and re-opening the connection is the simplest way to recover
 * from any of: heartbeat-driven socket close, broker-side channel
 * exception (e.g. NOT_FOUND from basic_get on a queue the portal hasn't
 * declared yet), or TLS-level read/write errors. Outstanding delivery
 * tags are dropped — this is fine because we have not yet acked them and
 * the broker will redeliver after timeout / requeue.
 *
 * HIGH (round 2): exponential backoff against the broker. Without this,
 * a persistent failure (queue not yet declared, broker down for hours)
 * would trigger a TLS handshake every tick — broker rate-limits or
 * connection counter alerts will fire.
 */
static int reconnect_if_dead(worker_ctx_t *ctx)
{
	if (!ctx->conn_dead && ctx->conn) return 0;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (ctx->reconnect_backoff_sec > 0 &&
	    ctx->last_reconnect_attempt.tv_sec != 0) {
		long elapsed = (long)(now.tv_sec - ctx->last_reconnect_attempt.tv_sec);
		if (elapsed < ctx->reconnect_backoff_sec)
			return -1;   /* still inside backoff window */
	}
	ctx->last_reconnect_attempt = now;

	if (ctx->conn) { publish_conn_close(ctx->conn); ctx->conn = NULL; }
	ctx->conn = publish_conn_open(&ctx->cfg.amqp);
	if (!ctx->conn) {
		int next = ctx->reconnect_backoff_sec * 2;
		if (next < 1) next = 1;
		if (next > WORKER_RECONNECT_BACKOFF_MAX) next = WORKER_RECONNECT_BACKOFF_MAX;
		ctx->reconnect_backoff_sec = next;
		fprintf(stderr, "[worker] reconnect failed — next attempt in ≥%ds\n", next);
		return -1;
	}
	ctx->conn_dead = 0;
	ctx->reconnect_backoff_sec = 0;
	fprintf(stderr, "[worker] AMQP connection re-established\n");

	/*
	 * After a reconnect the in-flight delivery_tag is stale — that channel
	 * is gone. The broker still has the message unacked from the old
	 * channel and will redeliver after timeout. /running marker keeps I1
	 * gating intact for the redelivery.
	 */
	ctx->inflight_delivery_tag = 0;
	return 0;
}

void worker_keepalive(worker_ctx_t *ctx)
{
	if (!ctx || !ctx->conn || ctx->conn_dead) return;
	if (publish_conn_pump(ctx->conn) < 0)
		ctx->conn_dead = 1;
}

/*
 * Round 3: split waitpid from publish so drain can reap even when the
 * AMQP connection is dead and stuck in backoff. Without this, the OS
 * child becomes a zombie and worker_idle never returns true → drain
 * loop spins forever (or until systemd's TimeoutStopSec SIGKILLs us,
 * losing the broker ack and the result publish).
 */
static int reap_child_only(worker_ctx_t *ctx)
{
	if (ctx->child_pid == 0) return 0;
	int status = 0;
	pid_t rc = waitpid(ctx->child_pid, &status, WNOHANG);
	if (rc == 0) return 0;        /* still running */
	if (rc < 0)  return -1;

	/* Mark task as no longer in-flight on our side. The result file
	 * (if any) stays in /results until publish succeeds — either later
	 * this session (when conn comes back) or on next agent startup
	 * via replay_pending_results. */
	(void)status;
	ctx->child_pid = 0;
	return 1;
}

int worker_tick(worker_ctx_t *ctx)
{
	if (!ctx) return -1;

	/*
	 * Reap first — independent of AMQP state. Lets drain progress even
	 * when broker is unreachable.
	 */
	int reaped = reap_child_only(ctx);
	if (reaped < 0) return -1;

	if (reconnect_if_dead(ctx) < 0) {
		/* Connection still down. If we just reaped, the result file
		 * remains in /results for next-tick or next-startup replay. */
		return -1;
	}
	if (publish_conn_pump(ctx->conn) < 0) {  /* CRITICAL #1: heartbeat keepalive */
		ctx->conn_dead = 1;
		return -1;
	}

	/*
	 * Round 4 F1+F2: publish ANY pending result, not only the just-reaped
	 * one. After a publish failure on a previous tick, inflight_task_id
	 * stays set with child_pid==0 (the comment in publish_reaped_result
	 * documents this "leave intact for retry" intent — but the previous
	 * gating only fired on `reaped == 1`, so the retry never happened).
	 */
	if (ctx->child_pid == 0 && ctx->inflight_task_id[0] != '\0') {
		if (publish_reaped_result(ctx) < 0) {
			ctx->conn_dead = 1;
			return -1;
		}
	}

	if (try_pick_new_task(ctx) < 0) { ctx->conn_dead = 1; return -1; }

	return 0;
}
