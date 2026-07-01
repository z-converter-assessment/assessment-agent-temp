/**
 * @file worker.c
 * @brief task.install consumer (Windows) — state machine + idempotency + drain.
 *
 * Linux worker.c 의 Windows 포팅. 핵심 흐름 동일, fork 대신 install thread:
 *   IDLE   — basic_get 폴링. message 받으면 install thread 생성 → BUSY
 *   BUSY   — WaitForSingleObject(thread, 0) 으로 종료 폴링. 종료 시 result 파일 읽음 →
 *            publish → ack → /done 마커 이동 → IDLE
 *   DRAIN  — Service Stop 신호 후. 신규 basic_get 정지. 진행 중 thread 마무리 대기.
 *
 * Install thread:
 *   download_package() + exec_install() → result JSON 을 results\<task_id>.json 에 쓰고 종료.
 *   부모 (worker_tick) 가 그 파일 읽어 publish 후 done\ 으로 이동.
 *
 * Idempotency:
 *   done\<task_id>.json 가 있으면 install 시도 없이 "already_done" result 합성 후 ack.
 *
 * v1 단순화 (Linux 대비 생략한 디테일):
 *   - startup /results scan 후 재발행 (TODO: 다음 PR)
 *   - /running marker (mid-install 크래시 감지) — Service Stop 만으로는 부족할 때 필요. TODO.
 *   - drain escalation 의 정밀한 +5s SIGKILL 타이머 — exec.c 내부 Job Object timeout 으로 흡수.
 */

#define WIN32_LEAN_AND_MEAN

#include "worker.h"
#include "download.h"
#include "exec.h"
#include "service.h"   /* stop_requested — download cancel hook */
#include "util.h"
#include "cJSON.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <windows.h>
#include <process.h>      /* _beginthreadex */
#include <direct.h>       /* _mkdir */
#include "nt52_compat.h"  /* strncpy_s — NT5.2 msvcrt 부재 대비(legacy 빌드) */

#ifndef AGENT_VERSION
#define AGENT_VERSION "1.0.0"
#endif

/* ============================================================
 * Context
 * ============================================================ */

typedef enum { WSTATE_IDLE = 0, WSTATE_BUSY, WSTATE_DRAIN } worker_state_t;

typedef struct {
	/* Install thread arguments (heap-allocated, freed by thread on exit). */
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
	char         *result_path;     /* 결과 파일 — thread 가 작성 */
	char         *running_marker_path;  /* /running\<task_id>.json — thread 정상 종료 시 제거 */
	HANDLE        job;             /* Job Object handle — caller (worker) 가 소유. exec_install 에 전달 */
	const char   *allowed_hosts_csv;  /* config 참조 — context lifetime */
	const char   *tmp_dir;
	int           disk_reserve_mb;
	const char   *machine_id;
	const char   *agent_version;
	char        **install_args;    /* install.args 복제 — NULL-terminated. exec_install argv_extra 로 전달.
	                                  thread 가 주소공간 공유 → 각 문자열 strdup 필수 (task JSON free 후 UAF 방지). */
} install_thread_arg_t;

struct worker_ctx_s {
	worker_config_t cfg;
	publish_conn_t *conn;
	int             conn_dead;

	worker_state_t  state;

	/* In-flight install state (state == BUSY 일 때만 유효). */
	HANDLE          install_thread;
	HANDLE          install_job;          /* exec_install 에 전달한 Job — drain 시 TerminateJobObject 로 강제 종료 */
	char            inflight_task_id[128];
	uint64_t        inflight_delivery_tag;

	/* Cached dir paths. */
	char            results_dir[512];
	char            done_dir[512];
	char            running_dir[512];

	/* AMQP reconnect backoff with monotonic clock (Linux clock_gettime 대응). */
	int             reconnect_backoff_sec;       /* current window (s), 1→2→4→...→60 */
	ULONGLONG       last_reconnect_attempt_ms;   /* monotonic_ms() of last attempt; 0 = never */
};

#define WORKER_RECONNECT_BACKOFF_MAX 60

/* ============================================================
 * Filesystem helpers
 * ============================================================ */

static int ensure_dir(const char *path)
{
	if (!path || !*path) return -1;
	DWORD attr = GetFileAttributesA(path);
	if (attr != INVALID_FILE_ATTRIBUTES) {
		return (attr & FILE_ATTRIBUTE_DIRECTORY) ? 0 : -1;
	}
	/* Recursive create — strip last segment 까지 재귀. */
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
	/* MoveFileExA REPLACE_EXISTING 으로 atomic rename. */
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

/* ============================================================
 * task_id 검증 (Linux 와 동일 grammar: UUID 또는 32-char hex)
 * ============================================================ */
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
 * Result JSON builder
 * ============================================================ */

static char *iso8601_now(void)
{
	static char buf[32];
	time_t t = time(NULL);
	struct tm gm;
	gmtime_s(&gm, &t);
	strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &gm);
	return buf;
}

/* OS 버전 식별자 — Registry CurrentBuildNumber (bare, UBR 미포함). 엔진의 성공 exit code
 * 보정 정책 키 (예: Windows Server 2022 = "20348"). collect.c 의 인벤토리용 os_version_info
 * 는 build.UBR 형식이지만, 정책은 빌드 메이저만 보면 되므로 task.result 에는 bare build 만 발행.
 * 읽기 실패 시 빈 문자열 (엔진은 nullable 로 흡수). */
static void os_build_number(char *out, size_t out_sz)
{
	if (out_sz) out[0] = '\0';
	HKEY hKey;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
	    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
	    0, KEY_READ, &hKey) != ERROR_SUCCESS) return;
	DWORD sz = (DWORD)out_sz;
	RegQueryValueExA(hKey, "CurrentBuildNumber", NULL, NULL, (LPBYTE)out, &sz);
	RegCloseKey(hKey);
}

static char *build_result_json(const worker_ctx_t *ctx,
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
	cJSON_AddStringToObject(root, "machine_id",       ctx->cfg.machine_id ? ctx->cfg.machine_id : "");
	cJSON_AddStringToObject(root, "os_family",        "windows");
	char os_build_b[32];
	os_build_number(os_build_b, sizeof os_build_b);
	if (os_build_b[0])
		cJSON_AddStringToObject(root, "os_version", os_build_b);
	else
		cJSON_AddNullToObject  (root, "os_version");
	cJSON_AddStringToObject(root, "agent_version",    ctx->cfg.agent_version ? ctx->cfg.agent_version : AGENT_VERSION);
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

/* ============================================================
 * Startup cleanup + recovery (Linux D3 / CRITICAL #10 패턴 포팅)
 * ============================================================ */

/*
 * 파일명이 `<task_id>.json` 형식인지 검증하고 task_id 부분 추출.
 * write_file_atomic 의 `*.tmp` 잔여물 / 수기 파일 등은 skip.
 */
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

/*
 * /done\<task_id>.json 중 retention 지난 파일 삭제. 디스크 점유 방지.
 */
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

/*
 * %TEMP%\agent-task-* 잔여 workspace 삭제. 부모 crash 후 남은 폴더.
 */
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

/*
 * /running\<task_id>.json marker 헬퍼.
 * spawn_install 직전 작성 + install thread 정상 종료 시 제거.
 * worker_init 의 recover_stale_running 가 잔존 marker 를 mid-install 크래시로 처리.
 */
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

/*
 * Startup /results scan — 부모 crash 후 미발행 result 파일들을 broker 에 재발행.
 * 원본 delivery_tag 는 채널 close 로 무효이지만, redelivered task 가 /done 마커를 통해
 * already_done 으로 처리되도록 publish 후 /done 으로 이동.
 */
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

/*
 * Forward decl — recover_stale_running 가 사용. build_result_json 은 아래쪽에 정의.
 */
static char *build_result_json(const struct worker_ctx_s *ctx,
                               const char *task_id, const char *status,
                               const char *failure_reason,
                               int has_exit_code, int exit_code, long duration_ms,
                               const char *stdout_tail, const char *stderr_tail);

/*
 * Startup /running scan — marker 존재 = 이전 인스턴스가 mid-install 크래시.
 * /done 또는 /results 에 이미 결과가 있으면 그 결과를 신뢰하고 marker 만 제거.
 * 둘 다 없으면 synth failure 를 /done 에 쓰고 best-effort publish (connection
 * 다운된 케이스에서는 broker redelivery + I1 로 복구).
 */
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
				/* /done 먼저 쓰고 publish (write 실패 시 /running 유지 — 다음 startup 재시도) */
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

/* failure_reason mapping from download / exec status. */
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

/* ============================================================
 * Install thread
 *
 * thread 함수 — download → exec → result 파일 작성 → 종료.
 * thread 가 stage 별 결과를 results\<task_id>.json 에 쓰면
 * worker_tick (부모) 가 그 파일 읽어 publish 처리.
 * ============================================================ */

/* libcurl cancellation wrapper — Service Stop 시 download 진행 중인 thread 가 즉시 abort.
 * drain 시 PUBLISH-stuck 4-phase 의 wedge 회피. */
static int worker_download_cancel_cb(void *user)
{
	(void)user;
	return stop_requested();
}

/* install_args (NULL-terminated, 각 원소 strdup) 해제. */
static void free_install_args(char **args)
{
	if (!args) return;
	for (int i = 0; args[i]; i++) free(args[i]);
	free(args);
}

static unsigned __stdcall install_thread_main(void *arg)
{
	install_thread_arg_t *a = (install_thread_arg_t *)arg;

	/* Pre-create work dir */
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
	int has_exit = (ds == DOWNLOAD_OK && xs != EXEC_ERR_SCRIPT_NOT_FOUND && xs != EXEC_ERR_INTERNAL);

	/* Result JSON 작성 — 부모가 읽음. ctx 가 없으니 stub fields 직접 채움. */
	cJSON *root = cJSON_CreateObject();
	if (root) {
		cJSON_AddStringToObject(root, "message_type",     "task.result");
		cJSON_AddStringToObject(root, "machine_id",       a->machine_id ? a->machine_id : "");
		cJSON_AddStringToObject(root, "os_family",        "windows");
		char os_build_b[32];
		os_build_number(os_build_b, sizeof os_build_b);
		if (os_build_b[0])
			cJSON_AddStringToObject(root, "os_version", os_build_b);
		else
			cJSON_AddNullToObject  (root, "os_version");
		cJSON_AddStringToObject(root, "agent_version",    a->agent_version ? a->agent_version : AGENT_VERSION);
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

		cJSON_AddStringToObject(root, "task_id",          a->task_id ? a->task_id : "");
		cJSON_AddStringToObject(root, "status",           success ? "success" : "failure");
		if (success)
			cJSON_AddNullToObject(root, "failure_reason");
		else
			cJSON_AddStringToObject(root, "failure_reason", reason);

		if (has_exit)
			cJSON_AddNumberToObject(root, "exit_code", er.exit_code);
		else
			cJSON_AddNullToObject  (root, "exit_code");

		cJSON_AddNumberToObject(root, "duration_ms", duration_ms);
		cJSON_AddStringToObject(root, "stdout_tail", er.stdout_tail);
		cJSON_AddStringToObject(root, "stderr_tail", er.stderr_tail);
		cJSON_AddStringToObject(root, "completed_at", iso8601_now());

		char *json = cJSON_PrintUnformatted(root);
		if (json) {
			write_file_atomic(a->result_path, json);
			free(json);
		}
		cJSON_Delete(root);
	}

	/* Workspace 정리 (다운로드 파일 + 추출 흔적 제거 — Windows direct_exec / msi 는
	 * target_file 만 work_dir 안에 있음). */
	rmrf_recursive(a->work_dir);

	/* CRITICAL #10: result 파일 작성이 끝났으니 /running marker 제거.
	 * 이 시점 이후 부모가 crash 해도 startup replay_pending_results 가 /results 파일을
	 * 처리하고, 이 task 는 /done 으로 이동되어 redelivery 시 I1 가드에 막힘. */
	if (a->running_marker_path) DeleteFileA(a->running_marker_path);

	/* free thread args */
	free(a->task_id);    free(a->url);         free(a->sha256);
	free(a->work_dir);   free(a->target_file); free(a->result_path);
	free(a->running_marker_path);
	free_install_args(a->install_args);
	free(a);
	return 0;
}

/* ============================================================
 * worker_init / worker_shutdown
 * ============================================================ */

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

	/* AMQP 시도 — 성공/실패 모두 OK (실패 시 다음 tick 재시도, replay 만 deferred). */
	ctx->conn = publish_conn_open(&ctx->cfg.amqp);
	ctx->conn_dead = (ctx->conn == NULL);
	ctx->reconnect_backoff_sec = 0;
	if (ctx->conn) {
		fprintf(stderr, "[worker] AMQP connected (queue=%s)\n", ctx->cfg.queue_name);
	} else {
		fprintf(stderr, "[worker] AMQP initial connect failed - will retry on next tick\n");
	}

	/* Startup procedures — Linux 패턴 포팅.
	 *   1. recover_stale_running: 이전 인스턴스 mid-install 크래시 처리 (/running marker 잔존)
	 *   2. replay_pending_results: 미발행 result 파일 publish + /done 이동 (connect 필요)
	 *   3. purge_expired_done: retention 지난 /done 마커 삭제 (connect 무관)
	 *   4. purge_stale_workspaces: 잔존 %TEMP%\agent-task-* 정리
	 *
	 * 순서 주의: recover 가 /done 을 쓰므로 replay 보다 먼저. replay 는 /done 이동을
	 * 위해 recover 가 만든 /done 마커와 충돌 없게 task_id 가 다르다는 가정 (다른
	 * 시점 task 들이라 OK). purge_expired_done 은 두 작업 다 끝난 뒤. */
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
		/* drain 패턴 가정 — thread 가 이미 종료된 상태에서 호출돼야 함.
		 * 만약 살아있으면 Job kill 후 짧게 wait. */
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

/* ============================================================
 * AMQP reconnect (lazy, called from worker_tick)
 * ============================================================ */
static int ensure_connection(worker_ctx_t *ctx)
{
	if (ctx->conn && !ctx->conn_dead) return 0;

	/* Backoff: 직전 attempt 이후 backoff_sec 가 안 지났으면 skip — broker 다운 시
	 * 매 tick (1m) 무의미한 connect 시도 회피. Monotonic clock (GetTickCount64). */
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

/* ============================================================
 * worker_tick — state machine
 * ============================================================ */

static int spawn_install(worker_ctx_t *ctx, cJSON *task)
{
	const cJSON *jid       = cJSON_GetObjectItemCaseSensitive(task, "task_id");
	const cJSON *jmachine  = cJSON_GetObjectItemCaseSensitive(task, "machine_id");
	const cJSON *jdownload = cJSON_GetObjectItemCaseSensitive(task, "download");
	const cJSON *jinstall  = cJSON_GetObjectItemCaseSensitive(task, "install");

	const char *task_id = cJSON_IsString(jid) ? jid->valuestring : NULL;
	if (!task_id_valid(task_id)) return -1;

	/* machine_id mismatch — portal 라우팅 오류 안전망. 합성 result + 즉시 ack.
	 * (Linux child_run_task 의 mismatch 가드 포팅) */
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
		return 1;  /* result ready, no thread */
	}

	const char *install_type_s = "shell";   /* backward-compat default */
	int timeout_sec = 600;
	if (cJSON_IsObject(jinstall)) {
		const cJSON *jty = cJSON_GetObjectItemCaseSensitive(jinstall, "type");
		const cJSON *jt  = cJSON_GetObjectItemCaseSensitive(jinstall, "timeout_sec");
		if (cJSON_IsString(jty) && *jty->valuestring) install_type_s = jty->valuestring;
		if (cJSON_IsNumber(jt)) timeout_sec = (int)jt->valuedouble;
	}

	/* Windows agent 는 direct_exec / msi 만 처리. shell 은 즉시 reject. */
	exec_install_type_t install_type;
	if (strcmp(install_type_s, "direct_exec") == 0)      install_type = EXEC_INSTALL_TYPE_DIRECT_EXEC;
	else if (strcmp(install_type_s, "msi") == 0)         install_type = EXEC_INSTALL_TYPE_MSI;
	else {
		/* unsupported — 합성 result publish + ack. */
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
		/* state 를 BUSY 처럼 두지 않고 즉시 IDLE 유지 — tick 끝에 publish + ack. */
		strncpy_s(ctx->inflight_task_id, sizeof ctx->inflight_task_id,
		          task_id, _TRUNCATE);
		return 1;  /* "result file ready, no thread" 신호 */
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

	/* install.args 파싱 — direct_exec 패키지(NSIS)에 -s/-u 등 인자 전달.
	 * Linux worker 는 fork 라 valuestring 참조로 충분하지만, Windows 는 install thread 가
	 * 부모와 주소공간을 공유하므로 각 문자열을 strdup 으로 복제한다(task JSON free 후 UAF 방지).
	 * NULL-terminated → exec_install 의 argv_extra 로 그대로 전달. msi 타입은 exec.c 에서 무시. */
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

	/* CRITICAL #10: /running marker 작성 실패 시 thread 시작 금지. 없이 시작하면
	 * redelivery 시 I1 가드가 작동 안 해서 double-execute 가능. */
	if (write_running_marker(ctx, a->task_id) != 0) {
		fprintf(stderr, "[worker] write_running_marker failed for %s - aborting spawn\n", a->task_id);
		free(a->task_id); free(a->url); free(a->sha256);
		free(a->work_dir); free(a->target_file); free(a->result_path);
		free(a->running_marker_path);
		free_install_args(a->install_args);
		free(a);
		return -1;
	}

	/* Job Object 사전 생성 — worker 가 핸들 소유. drain escalation 시
	 * worker_force_child_term(ctx, 1) 이 TerminateJobObject(ctx->install_job, 1) 로
	 * 자식 process tree 즉시 강제 종료할 수 있게 됨. 실패 시 spawn abort. */
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
		clear_running_marker(ctx, a->task_id);  /* thread 시작 실패 — marker 회수 */
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

/*
 * Publish + ack 시도. 성공 시 0 반환 + inflight 비움 + result 파일 → /done 이동.
 * 실패 시 -1 반환 + inflight 유지 (다음 tick 또는 다음 startup replay 가 처리).
 * 영구 실패 (파일 missing + synth 도 실패) 도 inflight 비움 + drop.
 */
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
		/* 결과 파일도 없고 synth OOM — drop (보고 못 함). inflight 비움. */
		ctx->inflight_task_id[0] = '\0';
		ctx->inflight_delivery_tag = 0;
		return -1;
	}

	/* conn dead — 다음 tick / startup replay 에 미룸. inflight 유지. */
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
		return -1;  /* inflight 유지 — 다음 tick 재시도 */
	}

	if (ctx->inflight_delivery_tag != 0 &&
	    publish_conn_ack(ctx->conn, ctx->inflight_delivery_tag) != 0) {
		fprintf(stderr, "[worker] basic.ack failed - connection marked dead\n");
		ctx->conn_dead = 1;
		/* publish 는 성공했지만 ack 실패 — 다음 tick 에 ack 만 다시 시도. inflight 유지.
		 * 단 result 가 broker 에 이미 들어갔으니 redelivery 시 done 마커 + I1 로 가드. */
		return -1;
	}

	/* 전 성공: result 파일을 /done\ 으로 이동 (atomic, fail-safe — Linux와 동일). */
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

	/* AMQP 연결 보장. 실패 시 다음 tick 으로. */
	if (ensure_connection(ctx) != 0) return 0;

	/* BUSY: install thread 종료 폴링 + result publish. publish 실패 시 BUSY 유지하여
	 * 다음 tick 재시도 (conn 살아나거나 다음 startup replay). */
	if (ctx->state == WSTATE_BUSY) {
		if (ctx->install_thread) {
			DWORD wr = WaitForSingleObject(ctx->install_thread, 0);
			if (wr == WAIT_OBJECT_0) {
				CloseHandle(ctx->install_thread);
				ctx->install_thread = NULL;
				/* Job 도 정리 — KILL_ON_JOB_CLOSE 로 잔여 process 자동 cleanup. */
				if (ctx->install_job) {
					CloseHandle(ctx->install_job);
					ctx->install_job = NULL;
				}
				if (publish_result_and_ack(ctx) == 0)
					ctx->state = WSTATE_IDLE;
				/* else: inflight 유지 — 다음 tick 에서 thread==NULL 분기로 재진입 */
			}
		} else {
			/* thread 없음 (unsupported_install_type / mismatch / publish 재시도 케이스) */
			if (publish_result_and_ack(ctx) == 0)
				ctx->state = WSTATE_IDLE;
		}
	}

	if (ctx->state != WSTATE_IDLE) return 0;

	/* IDLE: basic_get 시도. */
	char *body = NULL;
	size_t body_len = 0;
	uint64_t tag = 0;
	int gr = publish_conn_get(ctx->conn, ctx->cfg.queue_name, &body, &body_len, &tag);
	if (gr == 1) return 0;       /* empty */
	if (gr < 0) { ctx->conn_dead = 1; return 0; }

	/* 메시지 받음 — parse + spawn. */
	cJSON *task = cJSON_ParseWithLength(body, body_len);
	free(body);
	if (!task) {
		/* malformed — ack 후 drop (DLQ 회피, 즉시 처리). */
		publish_conn_ack(ctx->conn, tag);
		return 0;
	}

	const cJSON *jid = cJSON_GetObjectItemCaseSensitive(task, "task_id");
	const char *tid = cJSON_IsString(jid) ? jid->valuestring : NULL;

	/* Idempotency: /done/<task_id>.json 발견 → already_done 합성. */
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

	/* sr == 0: thread 시작, sr == 1: 합성 result 즉시 작성됨 (unsupported_install_type).
	 * 둘 다 state = BUSY 로 둠 — 다음 tick 에 publish + ack. */
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
	/* state 가 BUSY 면 다음 tick 에서 thread 종료 대기 → publish → DRAIN 으로. */
}

void worker_force_child_term(worker_ctx_t *ctx, int hard)
{
	if (!ctx) return;
	(void)hard;  /* Windows 에 graceful term 개념 없음 — soft/hard 모두 즉시 kill. */

	/* Job Object 단위로 강제 종료 — 자식 process tree (download / install / msiexec)
	 * 전체가 즉시 종료됨 (KILL_ON_JOB_CLOSE 와 별개로 즉시 효과). 그 결과 install
	 * thread 의 exec_install 도 WaitForSingleObject 에서 깨어나 EXEC_ERR_SCRIPT_TIMEOUT
	 * 으로 반환 → result 파일 작성 후 thread 종료 → worker_tick 의 reap path 진입. */
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
