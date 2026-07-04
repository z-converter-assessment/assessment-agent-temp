#define WIN32_LEAN_AND_MEAN

#include "worker.h"
#include "collect.h"
#include "download.h"
#include "exec.h"
#include "service.h"
#include "util.h"
#include "cJSON.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <windows.h>
#include <process.h>
#include <direct.h>
#include "nt52_compat.h"

#ifndef AGENT_VERSION
#define AGENT_VERSION "1.0.0"
#endif

typedef enum { WSTATE_IDLE = 0, WSTATE_BUSY, WSTATE_DRAIN } worker_state_t;

typedef struct {

	char         *task_id;
	char         *url;
	char         *sha256;
	int64_t       size_bytes;
	exec_install_type_t install_type;
	int           timeout_sec;
	int           mem_limit_mb;
	int           fsize_limit_mb;
	int           active_proc_limit;
	char         *work_dir;
	char         *target_file;
	char         *result_path;
	char         *running_marker_path;
	HANDLE        job;
	const char   *allowed_hosts_csv;
	const char   *tmp_dir;
	int           disk_reserve_mb;
	const char   *machine_id;
	const char   *agent_version;
	char        **install_args;

} install_thread_arg_t;

struct worker_ctx_s {
	worker_config_t cfg;
	publish_conn_t *conn;
	int             conn_dead;

	worker_state_t  state;

	HANDLE          install_thread;
	HANDLE          install_job;
	char            inflight_task_id[128];
	uint64_t        inflight_delivery_tag;

	char            results_dir[512];
	char            done_dir[512];
	char            running_dir[512];

	int             reconnect_backoff_sec;
	ULONGLONG       last_reconnect_attempt_ms;
};

#define WORKER_RECONNECT_BACKOFF_MAX 60

static int ensure_dir(const char *path)
{
	if (!path || !*path) return -1;
	DWORD attr = GetFileAttributesA(path);
	if (attr != INVALID_FILE_ATTRIBUTES) {
		return (attr & FILE_ATTRIBUTE_DIRECTORY) ? 0 : -1;
	}

	char tmp[1024];
	size_t n = strlen(path);
	if (n >= sizeof tmp) return -1;
	memcpy(tmp, path, n + 1);
	for (size_t i = 1; i < n; i++) {
		if (tmp[i] == '\\' || tmp[i] == '/') {
			char saved = tmp[i];
			tmp[i] = '\0';
			DWORD a = GetFileAttributesA(tmp);
			if (a == INVALID_FILE_ATTRIBUTES) {
				if (!CreateDirectoryA(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
					return -1;
			}
			tmp[i] = saved;
		}
	}
	if (!CreateDirectoryA(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
		return -1;
	return 0;
}

static int file_exists(const char *path)
{
	DWORD a = GetFileAttributesA(path);
	return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int write_file_atomic(const char *path, const char *content)
{
	char tmp[1024];
	if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return -1;
	FILE *f = fopen(tmp, "wb");
	if (!f) return -1;
	size_t len = strlen(content);
	if (fwrite(content, 1, len, f) != len) { fclose(f); DeleteFileA(tmp); return -1; }
	if (fflush(f) != 0)                    { fclose(f); DeleteFileA(tmp); return -1; }
	if (fclose(f) != 0) { DeleteFileA(tmp); return -1; }

	if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
		DeleteFileA(tmp);
		return -1;
	}
	return 0;
}

static int read_file_all(const char *path, char **out, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
	long sz = ftell(f);
	if (sz < 0) { fclose(f); return -1; }
	rewind(f);
	char *buf = (char *)malloc((size_t)sz + 1);
	if (!buf) { fclose(f); return -1; }
	size_t got = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	if (got != (size_t)sz) { free(buf); return -1; }
	buf[sz] = '\0';
	*out = buf;
	*out_len = (size_t)sz;
	return 0;
}

static int rmrf_recursive(const char *path)
{
	DWORD attr = GetFileAttributesA(path);
	if (attr == INVALID_FILE_ATTRIBUTES) return 0;
	if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
		return DeleteFileA(path) ? 0 : -1;
	}
	char pattern[1024];
	if ((size_t)snprintf(pattern, sizeof pattern, "%s\\*", path) >= sizeof pattern) return -1;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	int rc = 0;
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
			char sub[1024];
			if ((size_t)snprintf(sub, sizeof sub, "%s\\%s", path, fd.cFileName) >= sizeof sub) { rc = -1; continue; }
			if (rmrf_recursive(sub) != 0) rc = -1;
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	if (!RemoveDirectoryA(path)) rc = -1;
	return rc;
}

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

static char *iso8601_now(void)
{
	static char buf[32];
	time_t t = time(NULL);
	struct tm gm;
	gmtime_s(&gm, &t);
	strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &gm);
	return buf;
}

/* task.result os_version은 inventory와 동일 소스(DisplayVersion, fallback ReleaseId)를 쓴다. */
static void os_display_version(char *out, size_t out_sz)
{
	if (out_sz) out[0] = '\0';
	HKEY hKey;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
	    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
	    0, KEY_READ, &hKey) != ERROR_SUCCESS) return;
	DWORD sz = (DWORD)out_sz;
	if (RegQueryValueExA(hKey, "DisplayVersion", NULL, NULL, (LPBYTE)out, &sz) != ERROR_SUCCESS) {
		sz = (DWORD)out_sz;
		RegQueryValueExA(hKey, "ReleaseId", NULL, NULL, (LPBYTE)out, &sz);
	}
	RegCloseKey(hKey);
}

/* task.result 페이로드 구성의 단일 소스. task 처리 경로(build_result_json 래퍼, ctx 보유)와
 * install 스레드(install_thread_arg_t 보유)가 모두 이걸 호출해, 정상 완료 result 와 synth/복구
 * result 의 필드 셋이 갈리는 드리프트를 막는다. machine_id/agent_version 만 호출자별로 다르다. */
static char *build_result_json_raw(const char *machine_id, const char *agent_version,
                               const char *task_id,
                               const char *status,
                               const char *failure_reason,
                               int has_exit_code, int exit_code,
                               long duration_ms,
                               const char *stdout_tail,
                               const char *stderr_tail)
{
	cJSON *root = cJSON_CreateObject();
	if (!root) return NULL;
	cJSON_AddStringToObject(root, "message_type",     "task.result");
	cJSON_AddStringToObject(root, "machine_id",       machine_id ? machine_id : "");
	cJSON_AddStringToObject(root, "agent_id",         cached_agent_id());
	cJSON_AddStringToObject(root, "os_family",        "windows");
	cJSON_AddStringToObject(root, "os_id",            "windows");
	char os_build_b[32];
	os_display_version(os_build_b, sizeof os_build_b);
	if (os_build_b[0])
		cJSON_AddStringToObject(root, "os_version", os_build_b);
	else
		cJSON_AddNullToObject  (root, "os_version");
	cJSON_AddNullToObject  (root, "os_codename");   /* Windows 는 codename 개념 없음(inventory 와 동일 null) */
	cJSON_AddStringToObject(root, "agent_version",    agent_version ? agent_version : AGENT_VERSION);
	cJSON_AddStringToObject(root, "collected_at",     iso8601_now());

	char hostname[256] = "unknown";
	DWORD sz = (DWORD)sizeof hostname;
	GetComputerNameA(hostname, &sz);
	cJSON_AddStringToObject(root, "hostname", hostname);

	char msg_id[64];
	uuid_v4(msg_id, sizeof msg_id);
	cJSON_AddStringToObject(root, "message_id",       msg_id);
	cJSON_AddNullToObject  (root, "boot_time");
	cJSON_AddNullToObject  (root, "agent_started_at");

	cJSON_AddStringToObject(root, "task_id",          task_id ? task_id : "");
	cJSON_AddStringToObject(root, "status",           status ? status : "failure");
	if (failure_reason && *failure_reason)
		cJSON_AddStringToObject(root, "failure_reason", failure_reason);
	else
		cJSON_AddNullToObject  (root, "failure_reason");

	if (has_exit_code)
		cJSON_AddNumberToObject(root, "exit_code", exit_code);
	else
		cJSON_AddNullToObject  (root, "exit_code");

	cJSON_AddNumberToObject(root, "duration_ms", duration_ms);
	cJSON_AddStringToObject(root, "stdout_tail", stdout_tail ? stdout_tail : "");
	cJSON_AddStringToObject(root, "stderr_tail", stderr_tail ? stderr_tail : "");
	cJSON_AddStringToObject(root, "completed_at", iso8601_now());

	char *s = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return s;
}

/* ctx 편의 래퍼 — task 처리 경로(child_run_task 등)가 쓴다. 필드 구성은 _raw 단일 소스. */
static char *build_result_json(const worker_ctx_t *ctx,
                               const char *task_id,
                               const char *status,
                               const char *failure_reason,
                               int has_exit_code, int exit_code,
                               long duration_ms,
                               const char *stdout_tail,
                               const char *stderr_tail)
{
	return build_result_json_raw(ctx->cfg.machine_id, ctx->cfg.agent_version,
	                             task_id, status, failure_reason,
	                             has_exit_code, exit_code, duration_ms,
	                             stdout_tail, stderr_tail);
}

/* wire 계약 conformance(emit dry-run)용 대표 task.result. 실제 발행 경로와 동일한
 * build_result_json_raw 직렬화를 태운다(더미 task 값 — 값이 아니라 구조를 검증). */
char *worker_emit_sample_result_json(const char *machine_id, const char *agent_version)
{
	return build_result_json_raw(machine_id, agent_version,
	                             "00000000-0000-0000-0000-000000000000",
	                             "success", NULL,
	                             1, 0, 0L, "", "");
}

static int replay_filename_to_task_id(const char *fname, char *out, size_t out_sz)
{
	size_t n = strlen(fname);
	const char *suffix = ".json";
	size_t slen = strlen(suffix);
	if (n <= slen) return 0;
	if (_stricmp(fname + n - slen, suffix) != 0) return 0;
	size_t base = n - slen;
	if (base >= out_sz) return 0;
	memcpy(out, fname, base);
	out[base] = '\0';
	return task_id_valid(out);
}

static void purge_expired_done(const struct worker_ctx_s *ctx)
{
	char pattern[1024];
	if ((size_t)snprintf(pattern, sizeof pattern, "%s\\*.json", ctx->done_dir) >= sizeof pattern) return;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE) return;

	FILETIME now_ft;
	GetSystemTimeAsFileTime(&now_ft);
	ULARGE_INTEGER now;
	now.LowPart  = now_ft.dwLowDateTime;
	now.HighPart = now_ft.dwHighDateTime;

	ULONGLONG retention_100ns = (ULONGLONG)ctx->cfg.done_retention_sec * 10000000ULL;

	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		ULARGE_INTEGER mt;
		mt.LowPart  = fd.ftLastWriteTime.dwLowDateTime;
		mt.HighPart = fd.ftLastWriteTime.dwHighDateTime;
		if (now.QuadPart > mt.QuadPart &&
		    (now.QuadPart - mt.QuadPart) > retention_100ns) {
			char path[1024];
			if ((size_t)snprintf(path, sizeof path, "%s\\%s", ctx->done_dir, fd.cFileName) < sizeof path)
				DeleteFileA(path);
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
}

static void purge_stale_workspaces(const struct worker_ctx_s *ctx)
{
	char pattern[1024];
	if ((size_t)snprintf(pattern, sizeof pattern, "%s\\agent-task-*", ctx->cfg.tmp_dir) >= sizeof pattern) return;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
		char path[1024];
		if ((size_t)snprintf(path, sizeof path, "%s\\%s", ctx->cfg.tmp_dir, fd.cFileName) < sizeof path)
			rmrf_recursive(path);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
}

static int running_marker_present(const struct worker_ctx_s *ctx, const char *task_id)
{
	char path[1024];
	if ((size_t)snprintf(path, sizeof path, "%s\\%s.json", ctx->running_dir, task_id) >= sizeof path)
		return 0;
	return file_exists(path);
}

static int write_running_marker(const struct worker_ctx_s *ctx, const char *task_id)
{
	char path[1024];
	if ((size_t)snprintf(path, sizeof path, "%s\\%s.json", ctx->running_dir, task_id) >= sizeof path)
		return -1;

	char body[256];
	snprintf(body, sizeof body,
	         "{\"task_id\":\"%s\",\"started_at\":\"%s\"}\n",
	         task_id, iso8601_now());
	return write_file_atomic(path, body);
}

static void clear_running_marker(const struct worker_ctx_s *ctx, const char *task_id)
{
	char path[1024];
	if ((size_t)snprintf(path, sizeof path, "%s\\%s.json", ctx->running_dir, task_id) >= sizeof path) return;
	DeleteFileA(path);
}

static void replay_pending_results(struct worker_ctx_s *ctx)
{
	if (!ctx->conn || ctx->conn_dead) return;
	char pattern[1024];
	if ((size_t)snprintf(pattern, sizeof pattern, "%s\\*.json", ctx->results_dir) >= sizeof pattern) return;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		char task_id[64];
		if (!replay_filename_to_task_id(fd.cFileName, task_id, sizeof task_id)) {
			fprintf(stderr, "[worker] replay: skipping unrecognized file '%s'\n", fd.cFileName);
			continue;
		}
		char path[1024];
		if ((size_t)snprintf(path, sizeof path, "%s\\%s", ctx->results_dir, fd.cFileName) >= sizeof path) continue;
		char *body = NULL;
		size_t body_len = 0;
		if (read_file_all(path, &body, &body_len) != 0 || !body) continue;
		int rc = publish_conn_publish(ctx->conn,
		                              ctx->cfg.amqp.exchange ? ctx->cfg.amqp.exchange : "assessment.tasks",
		                              ctx->cfg.result_routing_key ? ctx->cfg.result_routing_key : "task.result",
		                              body, body_len);
		free(body);
		if (rc != 0) {
			fprintf(stderr, "[worker] replay publish failed for %s — left for next startup\n", fd.cFileName);
			continue;
		}
		char dp[1024];
		snprintf(dp, sizeof dp, "%s\\%s.json", ctx->done_dir, task_id);
		MoveFileExA(path, dp, MOVEFILE_REPLACE_EXISTING);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
}

static char *build_result_json(const struct worker_ctx_s *ctx,
                               const char *task_id, const char *status,
                               const char *failure_reason,
                               int has_exit_code, int exit_code, long duration_ms,
                               const char *stdout_tail, const char *stderr_tail);

static void recover_stale_running(struct worker_ctx_s *ctx)
{
	char pattern[1024];
	if ((size_t)snprintf(pattern, sizeof pattern, "%s\\*.json", ctx->running_dir) >= sizeof pattern) return;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		char task_id[64];
		if (!replay_filename_to_task_id(fd.cFileName, task_id, sizeof task_id)) continue;

		char done_path[1024], results_path[1024], src_path[1024];
		int have_done    = 0, have_results = 0;
		if ((size_t)snprintf(done_path,    sizeof done_path,    "%s\\%s.json", ctx->done_dir,    task_id) < sizeof done_path)
			have_done = file_exists(done_path);
		if ((size_t)snprintf(results_path, sizeof results_path, "%s\\%s.json", ctx->results_dir, task_id) < sizeof results_path)
			have_results = file_exists(results_path);

		int safe_to_unlink = 1;

		if (!have_done && !have_results) {
			char *body = build_result_json(ctx, task_id, "failure", "internal_error",
			                               0, 0, 0, "",
			                               "agent terminated mid-install; recovered on restart\n");
			if (!body) {
				safe_to_unlink = 0;
			} else {

				if (write_file_atomic(done_path, body) != 0) {
					fprintf(stderr, "[worker] recover: /done write failed for %s — leaving /running for next startup\n", task_id);
					safe_to_unlink = 0;
				} else if (ctx->conn && !ctx->conn_dead) {
					(void)publish_conn_publish(ctx->conn,
					                           ctx->cfg.amqp.exchange ? ctx->cfg.amqp.exchange : "assessment.tasks",
					                           ctx->cfg.result_routing_key ? ctx->cfg.result_routing_key : "task.result",
					                           body, strlen(body));
				}
				free(body);
			}
		} else {
			fprintf(stderr, "[worker] recover: task %s has prior result on disk — skipping synth (done=%d results=%d)\n",
			        task_id, have_done, have_results);
		}

		if (safe_to_unlink &&
		    (size_t)snprintf(src_path, sizeof src_path, "%s\\%s", ctx->running_dir, fd.cFileName) < sizeof src_path)
			DeleteFileA(src_path);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
}

static const char *reason_for_status(download_status_t ds, exec_status_t xs)
{
	if (ds == DOWNLOAD_ERR_URL_NOT_ALLOWED)   return "url_not_allowed";
	if (ds == DOWNLOAD_ERR_INSUFFICIENT_DISK) return "insufficient_disk";
	if (ds == DOWNLOAD_ERR_DOWNLOAD_FAILED)   return "download_failed";
	if (ds == DOWNLOAD_ERR_SHA256_MISMATCH)   return "sha256_mismatch";
	if (ds == DOWNLOAD_ERR_INTERNAL)          return "internal_error";

	if (xs == EXEC_ERR_SCRIPT_NOT_FOUND)      return "script_not_found";
	if (xs == EXEC_ERR_SCRIPT_FAILED)         return "script_failed";
	if (xs == EXEC_ERR_SCRIPT_TIMEOUT)        return "script_timeout";
	if (xs == EXEC_ERR_INTERNAL)              return "internal_error";
	return "internal_error";
}

static int worker_download_cancel_cb(void *user)
{
	(void)user;
	return stop_requested();
}

static void free_install_args(char **args)
{
	if (!args) return;
	for (int i = 0; args[i]; i++) free(args[i]);
	free(args);
}

static unsigned __stdcall install_thread_main(void *arg)
{
	install_thread_arg_t *a = (install_thread_arg_t *)arg;

	rmrf_recursive(a->work_dir);
	ensure_dir(a->work_dir);

	ULONGLONG t0 = monotonic_ms();

	download_status_t ds = download_package(a->url, a->sha256, a->size_bytes,
	                                        a->allowed_hosts_csv, a->tmp_dir,
	                                        a->disk_reserve_mb, a->target_file,
	                                        worker_download_cancel_cb, NULL);

	exec_status_t  xs = EXEC_OK;
	exec_result_t  er = { .exit_code = -1 };

	if (ds == DOWNLOAD_OK) {
		xs = exec_install((void *)a->job, a->install_type,
		                  a->work_dir, a->target_file, (const char **)a->install_args,
		                  a->timeout_sec, a->mem_limit_mb, a->fsize_limit_mb,
		                  a->active_proc_limit, a->task_id, a->machine_id, &er);
	}

	long duration_ms = (long)(monotonic_ms() - t0);
	int success = (ds == DOWNLOAD_OK && xs == EXEC_OK);
	const char *reason = success ? "" : reason_for_status(ds, xs);
	/* exit_code is a real result only when the process exited on its own. On
	 * timeout exec.c force-terminates it (TerminateProcess code 1), so that 1
	 * is our kill code, not the installer's result -> emit null, not a fake 1. */
	int has_exit = (ds == DOWNLOAD_OK && xs != EXEC_ERR_SCRIPT_NOT_FOUND &&
	                xs != EXEC_ERR_SCRIPT_TIMEOUT && xs != EXEC_ERR_INTERNAL);

	char *json = build_result_json_raw(a->machine_id, a->agent_version,
	                                   a->task_id, success ? "success" : "failure",
	                                   reason, has_exit, er.exit_code, duration_ms,
	                                   er.stdout_tail, er.stderr_tail);
	if (json) {
		write_file_atomic(a->result_path, json);
		free(json);
	}

	rmrf_recursive(a->work_dir);

	if (a->running_marker_path) DeleteFileA(a->running_marker_path);

	free(a->task_id);    free(a->url);         free(a->sha256);
	free(a->work_dir);   free(a->target_file); free(a->result_path);
	free(a->running_marker_path);
	free_install_args(a->install_args);
	free(a);
	return 0;
}

worker_ctx_t *worker_init(const worker_config_t *cfg)
{
	if (!cfg || !cfg->queue_name || !*cfg->queue_name) return NULL;

	worker_ctx_t *ctx = (worker_ctx_t *)calloc(1, sizeof *ctx);
	if (!ctx) return NULL;
	ctx->cfg = *cfg;
	ctx->state = WSTATE_IDLE;

	const char *base = (cfg->state_dir && *cfg->state_dir)
		? cfg->state_dir : "C:\\ProgramData\\assessment-agent\\worker";
	snprintf(ctx->results_dir, sizeof ctx->results_dir, "%s\\results", base);
	snprintf(ctx->done_dir,    sizeof ctx->done_dir,    "%s\\done",    base);
	snprintf(ctx->running_dir, sizeof ctx->running_dir, "%s\\running", base);

	if (ensure_dir(ctx->results_dir) != 0 ||
	    ensure_dir(ctx->done_dir)    != 0 ||
	    ensure_dir(ctx->running_dir) != 0) {
		fprintf(stderr, "[worker] failed to create state dirs under %s\n", base);
		free(ctx);
		return NULL;
	}

	ctx->conn = publish_conn_open(&ctx->cfg.amqp);
	ctx->conn_dead = (ctx->conn == NULL);
	ctx->reconnect_backoff_sec = 0;
	if (ctx->conn) {
		fprintf(stderr, "[worker] AMQP connected (queue=%s)\n", ctx->cfg.queue_name);
	} else {
		fprintf(stderr, "[worker] AMQP initial connect failed - will retry on next tick\n");
	}

	recover_stale_running(ctx);
	replay_pending_results(ctx);
	purge_expired_done(ctx);
	purge_stale_workspaces(ctx);

	return ctx;
}

void worker_shutdown(worker_ctx_t *ctx)
{
	if (!ctx) return;
	if (ctx->install_thread) {

		if (ctx->install_job) TerminateJobObject(ctx->install_job, 1);
		WaitForSingleObject(ctx->install_thread, 5000);
		CloseHandle(ctx->install_thread);
		ctx->install_thread = NULL;
	}
	if (ctx->install_job) {
		CloseHandle(ctx->install_job);
		ctx->install_job = NULL;
	}
	if (ctx->conn) publish_conn_close(ctx->conn);
	free(ctx);
}

static int ensure_connection(worker_ctx_t *ctx)
{
	if (ctx->conn && !ctx->conn_dead) return 0;

	ULONGLONG now_ms = monotonic_ms();
	if (ctx->reconnect_backoff_sec > 0 && ctx->last_reconnect_attempt_ms > 0) {
		ULONGLONG since_ms = now_ms - ctx->last_reconnect_attempt_ms;
		if (since_ms < (ULONGLONG)ctx->reconnect_backoff_sec * 1000ULL)
			return -1;
	}
	ctx->last_reconnect_attempt_ms = now_ms;

	if (ctx->conn) { publish_conn_close(ctx->conn); ctx->conn = NULL; }

	ctx->conn = publish_conn_open(&ctx->cfg.amqp);
	if (!ctx->conn) {
		if (ctx->reconnect_backoff_sec < WORKER_RECONNECT_BACKOFF_MAX)
			ctx->reconnect_backoff_sec = ctx->reconnect_backoff_sec
				? ctx->reconnect_backoff_sec * 2 : 1;
		return -1;
	}
	ctx->conn_dead = 0;
	ctx->reconnect_backoff_sec = 0;
	return 0;
}

static int spawn_install(worker_ctx_t *ctx, cJSON *task)
{
	const cJSON *jid       = cJSON_GetObjectItemCaseSensitive(task, "task_id");
	const cJSON *jmachine  = cJSON_GetObjectItemCaseSensitive(task, "machine_id");
	const cJSON *jdownload = cJSON_GetObjectItemCaseSensitive(task, "download");
	const cJSON *jinstall  = cJSON_GetObjectItemCaseSensitive(task, "install");

	const char *task_id = cJSON_IsString(jid) ? jid->valuestring : NULL;
	if (!task_id_valid(task_id)) return -1;

	if (cJSON_IsString(jmachine) && ctx->cfg.machine_id &&
	    strcmp(jmachine->valuestring, ctx->cfg.machine_id) != 0) {
		char *res = build_result_json(ctx, task_id, "failure", "internal_error",
		                              0, 0, 0, "",
		                              "machine_id mismatch - task routed in error\n");
		if (res) {
			char rp[1024];
			snprintf(rp, sizeof rp, "%s\\%s.json", ctx->results_dir, task_id);
			write_file_atomic(rp, res);
			free(res);
		}
		strncpy_s(ctx->inflight_task_id, sizeof ctx->inflight_task_id,
		          task_id, _TRUNCATE);
		return 1;
	}

	const char *install_type_s = "shell";
	int timeout_sec = 600;
	if (cJSON_IsObject(jinstall)) {
		const cJSON *jty = cJSON_GetObjectItemCaseSensitive(jinstall, "type");
		const cJSON *jt  = cJSON_GetObjectItemCaseSensitive(jinstall, "timeout_sec");
		if (cJSON_IsString(jty) && *jty->valuestring) install_type_s = jty->valuestring;
		/* Non-positive timeout would disable the wall-clock kill in exec.c
		 * (term_at_ms=-1), letting a hung installer occupy the single worker
		 * slot forever. Ignore it and keep the default. */
		if (cJSON_IsNumber(jt) && jt->valuedouble > 0) timeout_sec = (int)jt->valuedouble;
	}

	exec_install_type_t install_type;
	if (strcmp(install_type_s, "direct_exec") == 0)      install_type = EXEC_INSTALL_TYPE_DIRECT_EXEC;
	else if (strcmp(install_type_s, "msi") == 0)         install_type = EXEC_INSTALL_TYPE_MSI;
	else {

		char *res = build_result_json(ctx, task_id, "failure",
		                              "unsupported_install_type",
		                              0, 0, 0, "",
		                              "install.type not handled by this OS\n");
		if (res) {
			char rp[1024];
			snprintf(rp, sizeof rp, "%s\\%s.json", ctx->results_dir, task_id);
			write_file_atomic(rp, res);
			free(res);
		}

		strncpy_s(ctx->inflight_task_id, sizeof ctx->inflight_task_id,
		          task_id, _TRUNCATE);
		return 1;
	}

	const char *url = NULL, *sha256 = NULL;
	int64_t size_bytes = 0;
	if (cJSON_IsObject(jdownload)) {
		const cJSON *ju = cJSON_GetObjectItemCaseSensitive(jdownload, "url");
		const cJSON *jh = cJSON_GetObjectItemCaseSensitive(jdownload, "sha256");
		const cJSON *jb = cJSON_GetObjectItemCaseSensitive(jdownload, "size_bytes");
		if (cJSON_IsString(ju)) url    = ju->valuestring;
		if (cJSON_IsString(jh)) sha256 = jh->valuestring;
		if (cJSON_IsNumber(jb)) size_bytes = (int64_t)jb->valuedouble;
	}

	install_thread_arg_t *a = (install_thread_arg_t *)calloc(1, sizeof *a);
	if (!a) return -1;
	a->task_id           = _strdup(task_id);
	a->url               = _strdup(url    ? url    : "");
	a->sha256            = _strdup(sha256 ? sha256 : "");
	a->size_bytes        = size_bytes;
	a->install_type      = install_type;
	a->timeout_sec       = timeout_sec;
	a->mem_limit_mb      = ctx->cfg.mem_limit_mb;
	a->fsize_limit_mb    = ctx->cfg.fsize_limit_mb;
	a->active_proc_limit = ctx->cfg.active_proc_limit;
	a->allowed_hosts_csv = ctx->cfg.allowed_hosts_csv;
	a->tmp_dir           = ctx->cfg.tmp_dir;
	a->disk_reserve_mb   = ctx->cfg.disk_reserve_mb;
	a->machine_id        = ctx->cfg.machine_id;
	a->agent_version     = ctx->cfg.agent_version;

	{
		const cJSON *ja = cJSON_IsObject(jinstall)
			? cJSON_GetObjectItemCaseSensitive(jinstall, "args") : NULL;
		if (cJSON_IsArray(ja)) {
			int n = cJSON_GetArraySize(ja);
			a->install_args = (char **)calloc((size_t)n + 1, sizeof(char *));
			if (a->install_args) {
				int k = 0;
				for (int i = 0; i < n; i++) {
					const cJSON *e = cJSON_GetArrayItem(ja, i);
					if (cJSON_IsString(e)) a->install_args[k++] = _strdup(e->valuestring);
				}
			}
		}
	}

	char work[1024], target[1024], res[1024], run[1024];
	snprintf(work,   sizeof work,   "%s\\agent-task-%s", ctx->cfg.tmp_dir, task_id);
	const char *ext = (install_type == EXEC_INSTALL_TYPE_MSI) ? "msi" : "exe";
	snprintf(target, sizeof target, "%s\\package.%s", work, ext);
	snprintf(res,    sizeof res,    "%s\\%s.json", ctx->results_dir, task_id);
	snprintf(run,    sizeof run,    "%s\\%s.json", ctx->running_dir, task_id);
	a->work_dir            = _strdup(work);
	a->target_file         = _strdup(target);
	a->result_path         = _strdup(res);
	a->running_marker_path = _strdup(run);

	if (!a->task_id || !a->url || !a->sha256 ||
	    !a->work_dir || !a->target_file || !a->result_path || !a->running_marker_path) {
		free(a->task_id); free(a->url); free(a->sha256);
		free(a->work_dir); free(a->target_file); free(a->result_path);
		free(a->running_marker_path);
		free_install_args(a->install_args);
		free(a);
		return -1;
	}

	if (write_running_marker(ctx, a->task_id) != 0) {
		fprintf(stderr, "[worker] write_running_marker failed for %s - aborting spawn\n", a->task_id);
		free(a->task_id); free(a->url); free(a->sha256);
		free(a->work_dir); free(a->target_file); free(a->result_path);
		free(a->running_marker_path);
		free_install_args(a->install_args);
		free(a);
		return -1;
	}

	HANDLE job = CreateJobObjectA(NULL, NULL);
	if (!job) {
		fprintf(stderr, "[worker] CreateJobObjectA failed for %s - aborting spawn\n", a->task_id);
		clear_running_marker(ctx, a->task_id);
		free(a->task_id); free(a->url); free(a->sha256);
		free(a->work_dir); free(a->target_file); free(a->result_path);
		free(a->running_marker_path);
		free_install_args(a->install_args);
		free(a);
		return -1;
	}
	ctx->install_job = job;
	a->job           = job;

	uintptr_t th = _beginthreadex(NULL, 0, install_thread_main, a, 0, NULL);
	if (th == 0) {
		CloseHandle(job);
		ctx->install_job = NULL;
		clear_running_marker(ctx, a->task_id);
		free(a->task_id); free(a->url); free(a->sha256);
		free(a->work_dir); free(a->target_file); free(a->result_path);
		free(a->running_marker_path);
		free_install_args(a->install_args);
		free(a);
		return -1;
	}
	ctx->install_thread = (HANDLE)th;
	strncpy_s(ctx->inflight_task_id, sizeof ctx->inflight_task_id,
	          task_id, _TRUNCATE);
	return 0;
}

static int publish_result_and_ack(worker_ctx_t *ctx)
{
	char rp[1024];
	snprintf(rp, sizeof rp, "%s\\%s.json", ctx->results_dir, ctx->inflight_task_id);

	char *body = NULL;
	size_t body_len = 0;
	int from_synth = 0;
	if (read_file_all(rp, &body, &body_len) != 0) {
		fprintf(stderr, "[worker] result file missing %s - synthesizing internal_error\n", rp);
		char *synth = build_result_json(ctx, ctx->inflight_task_id, "failure",
		                                "internal_error", 0, 0, 0, "",
		                                "result file missing\n");
		if (synth) { body = synth; body_len = strlen(synth); from_synth = 1; }
	}

	if (!body) {

		ctx->inflight_task_id[0] = '\0';
		ctx->inflight_delivery_tag = 0;
		return -1;
	}

	if (!ctx->conn || ctx->conn_dead) {
		free(body);
		return -1;
	}

	int pub_rc = publish_conn_publish(ctx->conn,
		ctx->cfg.amqp.exchange ? ctx->cfg.amqp.exchange : "assessment.tasks",
		ctx->cfg.result_routing_key ? ctx->cfg.result_routing_key : "task.result",
		body, body_len);
	free(body);

	if (pub_rc != 0) {
		fprintf(stderr, "[worker] result publish failed - connection marked dead, will retry next tick\n");
		ctx->conn_dead = 1;
		return -1;
	}

	if (ctx->inflight_delivery_tag != 0 &&
	    publish_conn_ack(ctx->conn, ctx->inflight_delivery_tag) != 0) {
		fprintf(stderr, "[worker] basic.ack failed - connection marked dead\n");
		ctx->conn_dead = 1;

		return -1;
	}

	if (!from_synth) {
		char dp[1024];
		snprintf(dp, sizeof dp, "%s\\%s.json", ctx->done_dir, ctx->inflight_task_id);
		MoveFileExA(rp, dp, MOVEFILE_REPLACE_EXISTING);
	}

	ctx->inflight_task_id[0] = '\0';
	ctx->inflight_delivery_tag = 0;
	return 0;
}

int worker_tick(worker_ctx_t *ctx)
{
	if (!ctx) return -1;

	if (ensure_connection(ctx) != 0) return 0;

	if (ctx->state == WSTATE_BUSY) {
		if (ctx->install_thread) {
			DWORD wr = WaitForSingleObject(ctx->install_thread, 0);
			if (wr == WAIT_OBJECT_0) {
				CloseHandle(ctx->install_thread);
				ctx->install_thread = NULL;

				if (ctx->install_job) {
					CloseHandle(ctx->install_job);
					ctx->install_job = NULL;
				}
				if (publish_result_and_ack(ctx) == 0)
					ctx->state = WSTATE_IDLE;

			}
		} else {

			if (publish_result_and_ack(ctx) == 0)
				ctx->state = WSTATE_IDLE;
		}
	}

	if (ctx->state != WSTATE_IDLE) return 0;

	char *body = NULL;
	size_t body_len = 0;
	uint64_t tag = 0;
	int gr = publish_conn_get(ctx->conn, ctx->cfg.queue_name, &body, &body_len, &tag);
	if (gr == 1) return 0;
	if (gr == 2) {
		/* task 큐가 아직 없음(engine 이 첫 task 때 생성) — 정상 대기. 404 가 채널을 닫았으니
		 * 채널만 조용히 재오픈하고, 실패하면 전체 reconnect 로 폴백한다. 경고 없음. */
		if (publish_conn_recover_channel(ctx->conn) != 0)
			ctx->conn_dead = 1;
		return 0;
	}
	if (gr < 0) { ctx->conn_dead = 1; return 0; }

	cJSON *task = cJSON_ParseWithLength(body, body_len);
	free(body);
	if (!task) {

		publish_conn_ack(ctx->conn, tag);
		return 0;
	}

	const cJSON *jid = cJSON_GetObjectItemCaseSensitive(task, "task_id");
	const char *tid = cJSON_IsString(jid) ? jid->valuestring : NULL;

	if (tid && task_id_valid(tid)) {
		char dp[1024];
		snprintf(dp, sizeof dp, "%s\\%s.json", ctx->done_dir, tid);
		if (file_exists(dp)) {
			char *res = build_result_json(ctx, tid, "failure", "already_done",
			                              0, 0, 0, "", "");
			if (res) {
				publish_conn_publish(ctx->conn,
					ctx->cfg.amqp.exchange ? ctx->cfg.amqp.exchange : "assessment.tasks",
					ctx->cfg.result_routing_key ? ctx->cfg.result_routing_key : "task.result",
					res, strlen(res));
				free(res);
			}
			publish_conn_ack(ctx->conn, tag);
			cJSON_Delete(task);
			return 0;
		}

		/* /running 마커가 있으면 이미 in-flight 인 task 의 redelivery다 — 이중 실행을 막고
		 * ack 후 drop 한다(Linux try_pick_new_task 와 동일 가드). 정상 신규 task 는 마커가
		 * 없어 그대로 진행한다. */
		if (running_marker_present(ctx, tid)) {
			fprintf(stderr, "[worker] task %s already in-flight — redelivery dropped\n", tid);
			publish_conn_ack(ctx->conn, tag);
			cJSON_Delete(task);
			return 0;
		}
	}

	ctx->inflight_delivery_tag = tag;
	int sr = spawn_install(ctx, task);
	cJSON_Delete(task);

	if (sr < 0) {
		fprintf(stderr, "[worker] spawn_install failed — synthesizing internal_error\n");
		char *res = build_result_json(ctx, tid ? tid : "", "failure",
		                              "internal_error", 0, 0, 0, "",
		                              "worker spawn failed\n");
		if (res && ctx->conn && !ctx->conn_dead) {
			publish_conn_publish(ctx->conn,
				ctx->cfg.amqp.exchange ? ctx->cfg.amqp.exchange : "assessment.tasks",
				ctx->cfg.result_routing_key ? ctx->cfg.result_routing_key : "task.result",
				res, strlen(res));
			publish_conn_ack(ctx->conn, tag);
		}
		free(res);
		ctx->inflight_task_id[0] = '\0';
		ctx->inflight_delivery_tag = 0;
		return 0;
	}

	ctx->state = WSTATE_BUSY;
	return 0;
}

void worker_keepalive(worker_ctx_t *ctx)
{
	if (!ctx || !ctx->conn || ctx->conn_dead) return;
	if (publish_conn_pump(ctx->conn) != 0) ctx->conn_dead = 1;
}

void worker_begin_drain(worker_ctx_t *ctx)
{
	if (!ctx) return;
	ctx->state = (ctx->install_thread || ctx->inflight_task_id[0])
		? WSTATE_BUSY : WSTATE_DRAIN;

}

void worker_force_child_term(worker_ctx_t *ctx, int hard)
{
	if (!ctx) return;
	(void)hard;

	if (ctx->install_job) {
		fprintf(stderr, "[worker] worker_force_child_term: TerminateJobObject (hard kill)\n");
		TerminateJobObject(ctx->install_job, 1);
	}
}

int worker_idle(const worker_ctx_t *ctx)
{
	if (!ctx) return 1;
	return ctx->state == WSTATE_IDLE && ctx->install_thread == NULL
	    && ctx->inflight_task_id[0] == '\0';
}

int worker_has_live_child(const worker_ctx_t *ctx)
{
	if (!ctx || !ctx->install_thread) return 0;
	return WaitForSingleObject(ctx->install_thread, 0) == WAIT_TIMEOUT ? 1 : 0;
}
