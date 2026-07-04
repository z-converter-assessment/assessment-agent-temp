#define _POSIX_C_SOURCE 200809L

#include "worker.h"
#include "collect.h"
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

struct worker_ctx_s {
	worker_config_t cfg;
	publish_conn_t *conn;

	int  drain;
	int  conn_dead;

	struct timespec last_reconnect_attempt;
	int  reconnect_backoff_sec;

	pid_t    child_pid;
	uint64_t inflight_delivery_tag;
	char     inflight_task_id[128];

	char results_dir[512];
	char done_dir[512];
	char running_dir[512];
};

#define WORKER_RECONNECT_BACKOFF_MAX 60

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

static int fsync_parent_dir(const char *path)
{
	char dir[1024];
	size_t n = strnlen(path, sizeof dir);
	if (n >= sizeof dir) return -1;
	memcpy(dir, path, n + 1);
	char *slash = strrchr(dir, '/');
	if (!slash) { dir[0] = '.'; dir[1] = '\0'; }
	else if (slash == dir) { dir[1] = '\0'; }
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

/* task.result 페이로드 구성의 단일 소스. task 처리 경로(build_result_json 래퍼, ctx 보유)와
 * emit dry-run(worker_emit_sample_result_json)이 모두 이걸 호출해 필드 셋 드리프트를 막는다.
 * machine_id/agent_version 만 호출자별로 다르다. */
static char *build_result_json_raw(const char *machine_id, const char *agent_version,
                                   const char *task_id,
                                   const char *status,
                                   const char *failure_reason,
                                   int   has_exit_code, int exit_code, int signal_no,
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
	/* machine_id 부재 시 null — inventory/metrics/error(add_common_metadata)와 통일.
	 * ""로 채우면 같은 호스트가 메시지별로 ""/null 로 갈려 감사/조인이 오염된다. */
	if (machine_id && *machine_id)
		cJSON_AddStringToObject(root, "machine_id", machine_id);
	else
		cJSON_AddNullToObject  (root, "machine_id");
	cJSON_AddStringToObject(root, "agent_id",         cached_agent_id());
	collect_add_os_result_fields(root);   /* os_family/os_id/os_version/os_codename */
	cJSON_AddStringToObject(root, "agent_version",    agent_version ? agent_version : AGENT_VERSION);
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
	/* exit_code 와 signal_no 는 상호배타(POSIX wait status): 정상 종료면 exit_code 실측
	 * signal_no null, 시그널 종료면 그 반대, 상태 미포착이면 둘 다 null. */
	if (signal_no > 0) cJSON_AddNumberToObject(root, "signal_no", signal_no);
	else               cJSON_AddNullToObject(root, "signal_no");
	cJSON_AddNumberToObject(root, "duration_ms", (double)duration_ms);
	cJSON_AddStringToObject(root, "stdout_tail",  stdout_tail ? stdout_tail : "");
	cJSON_AddStringToObject(root, "stderr_tail",  stderr_tail ? stderr_tail : "");
	cJSON_AddStringToObject(root, "completed_at", now_buf);

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return json;
}

/* ctx 편의 래퍼 — task 처리 경로가 쓴다. 필드 구성은 _raw 단일 소스. */
static char *build_result_json(const worker_ctx_t *ctx,
                               const char *task_id,
                               const char *status,
                               const char *failure_reason,
                               int   has_exit_code, int exit_code, int signal_no,
                               long  duration_ms,
                               const char *stdout_tail,
                               const char *stderr_tail)
{
	return build_result_json_raw(ctx->cfg.machine_id, ctx->cfg.agent_version,
	                             task_id, status, failure_reason,
	                             has_exit_code, exit_code, signal_no, duration_ms,
	                             stdout_tail, stderr_tail);
}

/* wire 계약 conformance(emit dry-run)용 대표 task.result. 실제 발행 경로와 동일한
 * build_result_json_raw 직렬화를 태운다(더미 task 값 — 값이 아니라 구조를 검증). */
char *worker_emit_sample_result_json(const char *machine_id, const char *agent_version)
{
	return build_result_json_raw(machine_id, agent_version,
	                             "00000000-0000-0000-0000-000000000000",
	                             "success", NULL,
	                             1, 0, 0, 0L, "", "");
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
	if (xs == EXEC_ERR_SCRIPT_UNSAFE)           return "script_unsafe";
	if (xs == EXEC_ERR_SCRIPT_FAILED)           return "script_failed";
	if (xs == EXEC_ERR_SCRIPT_TIMEOUT)          return "script_timeout";
	if (xs == EXEC_ERR_INTERNAL)                return "internal_error";
	return "internal_error";
}

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

	(void)setpgid(0, 0);
	close_inherited_fds();

	const cJSON *jid       = cJSON_GetObjectItemCaseSensitive(task, "task_id");
	const cJSON *jmachine  = cJSON_GetObjectItemCaseSensitive(task, "machine_id");
	const cJSON *jdownload = cJSON_GetObjectItemCaseSensitive(task, "download");
	const cJSON *jinstall  = cJSON_GetObjectItemCaseSensitive(task, "install");

	const char *task_id = cJSON_IsString(jid) ? jid->valuestring : NULL;
	if (!task_id_valid(task_id)) {
		_exit(2);
	}

	if (cJSON_IsString(jmachine) && ctx->cfg.machine_id &&
	    strcmp(jmachine->valuestring, ctx->cfg.machine_id) != 0) {
		char *res = build_result_json(ctx, task_id, "failure", "internal_error",
		                              0, 0, 0, 0, "", "machine_id mismatch — task routed in error\n");
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

	const char *install_type = "shell";
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
		/* Non-positive timeout would disable both the wall-clock kill and the
		 * RLIMIT_CPU cap in exec.c, hanging the single in-flight worker slot.
		 * Ignore it and keep the default. */
		if (cJSON_IsNumber(jt) && jt->valuedouble > 0) timeout_sec = (int)jt->valuedouble;
		if (cJSON_IsArray(ja)) {
			int n = cJSON_GetArraySize(ja);
			iargs = (const char **)calloc((size_t)n + 1, sizeof(char *));
			if (iargs) {
				/* Compact: keep only string args, in order. A non-string
				 * element (argv can't carry it) is skipped rather than left
				 * as a NULL hole that would truncate every later arg in
				 * exec.c's NULL-terminated argv scan. */
				int j = 0;
				for (int i = 0; i < n; i++) {
					const cJSON *e = cJSON_GetArrayItem(ja, i);
					if (cJSON_IsString(e)) iargs[j++] = e->valuestring;
				}
			}
		}
	}

	if (strcmp(install_type, "shell") != 0) {
		char *res = build_result_json(ctx, task_id, "failure",
		                              "unsupported_install_type",
		                              0, 0, 0, 0, "",
		                              "install.type not handled by this OS\n");
		if (res) { child_write_result_file(ctx, task_id, res); free(res); }
		free(iargs);
		_exit(0);
	}

	char work[1024];
	snprintf(work, sizeof work, "%s/agent-task-%s", ctx->cfg.tmp_dir, task_id);
	rmrf(work);
	if (mkdir(work, 0700) != 0) {
		char *res = build_result_json(ctx, task_id, "failure", "internal_error",
		                              0, 0, 0, 0, "", "workspace mkdir failed\n");
		if (res) { child_write_result_file(ctx, task_id, res); free(res); }
		free(iargs);
		_exit(0);
	}

	struct timespec ts0;
	clock_gettime(CLOCK_MONOTONIC, &ts0);

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
	/* exit_code is a real measurement only when the script exited normally.
	 * A signal-killed child (timeout SIGKILL, or a crash) has signal_no>0 and
	 * leaves exit_code at its -1 placeholder — emit null, not a fake -1. */
	int has_exit = (xs != EXEC_ERR_SCRIPT_NOT_FOUND && xs != EXEC_ERR_SCRIPT_UNSAFE &&
	                xs != EXEC_ERR_INTERNAL && er.signal_no == 0 &&
	                ds == DOWNLOAD_OK && es == EXTRACT_OK);

	char *res = build_result_json(ctx, task_id,
	                              success ? "success" : "failure",
	                              success ? "" : reason,
	                              has_exit, er.exit_code, er.signal_no,
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

	if (move_to_done(ctx, task_id) != 0) {
		fprintf(stderr, "[worker] move to /done failed for %s: %s — leaving unacked\n",
		        task_id, strerror(errno));
		return -1;
	}

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
	                               0, 0, 0, 0, "", err_tail ? err_tail : "");
	if (!body) return -1;

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

	if (delivery_tag != 0 && publish_conn_ack(ctx->conn, delivery_tag) != 0) {
		fprintf(stderr, "[worker] synth ack failed for %s — /done is durable; broker will redeliver, I1 will gate\n",
		        task_id);
		ctx->conn_dead = 1;
	}
	return 0;
}

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

static void recover_stale_running(worker_ctx_t *ctx)
{
	DIR *d = opendir(ctx->running_dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		char task_id[64];
		if (!replay_filename_to_task_id(e->d_name, task_id, sizeof task_id)) continue;

		char done_path[1024], results_path[1024], src_path[1024];
		int have_done    = 0, have_results = 0;
		if ((size_t)snprintf(done_path,    sizeof done_path,    "%s/%s.json", ctx->done_dir,    task_id) < sizeof done_path)
			have_done = file_exists(done_path);
		if ((size_t)snprintf(results_path, sizeof results_path, "%s/%s.json", ctx->results_dir, task_id) < sizeof results_path)
			have_results = file_exists(results_path);

		int safe_to_unlink_running = 1;

		if (!have_done && !have_results) {
			char *body = build_result_json(ctx, task_id, "failure", "internal_error",
			                               0, 0, 0, 0, "",
			                               "agent terminated mid-install; recovered on restart\n");
			if (!body) {

				safe_to_unlink_running = 0;
			} else {

				if (write_file_atomic(done_path, body) != 0) {
					fprintf(stderr, "[worker] recover: /done write failed for %s — leaving /running for next startup\n", task_id);
					safe_to_unlink_running = 0;
				} else {

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

static int running_marker_present(const worker_ctx_t *ctx, const char *task_id)
{
	char path[1024];
	if ((size_t)snprintf(path, sizeof path, "%s/%s.json", ctx->running_dir, task_id) >= sizeof path)
		return 0;
	return file_exists(path);
}

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
	(void)kill(-pgid, sig);
	fprintf(stderr, "[worker] forced %s to child pgid=%d\n",
	        hard ? "SIGKILL" : "SIGTERM", (int)pgid);
}

int worker_idle(const worker_ctx_t *ctx)
{

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

static int publish_reaped_result(worker_ctx_t *ctx)
{
	if (ctx->inflight_task_id[0] == '\0') return 0;

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

		publish_rc = publish_synth_failure(ctx, task_id, tag, "internal_error",
		                                   "worker child exited without writing result\n");
	}

	if (publish_rc != 0) {

		return -1;
	}

	clear_running_marker(ctx, task_id);
	ctx->inflight_delivery_tag   = 0;
	ctx->inflight_task_id[0]     = '\0';
	return 0;
}

static int try_pick_new_task(worker_ctx_t *ctx)
{

	if (ctx->drain || ctx->child_pid != 0 || ctx->inflight_task_id[0] != '\0')
		return 0;

	char    *body = NULL;
	size_t   blen = 0;
	uint64_t tag  = 0;
	int rc = publish_conn_get(ctx->conn, ctx->cfg.queue_name,
	                          &body, &blen, &tag);
	if (rc == 1) return 0;
	if (rc == 2) {
		/* task 큐가 아직 없음(engine 이 첫 task 때 생성) — 정상 대기 상태다. 404 가 채널을
		 * 닫았으니 채널만 조용히 재오픈하고, 실패하면 전체 reconnect 로 폴백한다. 경고 없음. */
		if (publish_conn_recover_channel(ctx->conn) != 0)
			ctx->conn_dead = 1;
		return 0;
	}
	if (rc < 0)  return -1;

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

	char done_path[1024];
	snprintf(done_path, sizeof done_path, "%s/%s.json", ctx->done_dir, task_id);
	if (file_exists(done_path)) {
		fprintf(stderr, "[worker] task %s already_done — synthesizing result\n", task_id);
		char *res = build_result_json(ctx, task_id, "failure", "already_done",
		                              0, 0, 0, 0, "", "");
		if (!res) {

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

	if (running_marker_present(ctx, task_id)) {
		fprintf(stderr, "[worker] task %s already in-flight — redelivery dropped\n", task_id);
		if (publish_conn_ack(ctx->conn, tag) != 0) ctx->conn_dead = 1;
		cJSON_Delete(task);
		free(body);
		return 0;
	}

	if (write_running_marker(ctx, task_id) != 0) {
		fprintf(stderr, "[worker] task %s: /running marker write failed — aborting\n", task_id);
		if (publish_synth_failure(ctx, task_id, tag, "internal_error",
		                          "agent could not write /running marker; install skipped to prevent double-execution\n") != 0)
			ctx->conn_dead = 1;
		cJSON_Delete(task);
		free(body);
		return 0;
	}

	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "[worker] fork failed: %s\n", strerror(errno));
		int synth_rc = publish_synth_failure(ctx, task_id, tag, "internal_error",
		                                     "agent could not fork worker child\n");
		if (synth_rc == 0) {

			clear_running_marker(ctx, task_id);
		} else {

			ctx->conn_dead = 1;
		}
		cJSON_Delete(task);
		free(body);
		return 0;
	}
	if (pid == 0) {

		ctx->conn = NULL;
		child_run_task(ctx, task);
		_exit(0);
	}

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

static int reconnect_if_dead(worker_ctx_t *ctx)
{
	if (!ctx->conn_dead && ctx->conn) return 0;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (ctx->reconnect_backoff_sec > 0 &&
	    ctx->last_reconnect_attempt.tv_sec != 0) {
		long elapsed = (long)(now.tv_sec - ctx->last_reconnect_attempt.tv_sec);
		if (elapsed < ctx->reconnect_backoff_sec)
			return -1;
	}
	ctx->last_reconnect_attempt = now;

	if (ctx->conn) { publish_conn_close(ctx->conn); ctx->conn = NULL; }
	ctx->conn = publish_conn_open(&ctx->cfg.amqp);
	if (!ctx->conn) {
		int next = ctx->reconnect_backoff_sec * 2;
		if (next < 1) next = 1;
		if (next > WORKER_RECONNECT_BACKOFF_MAX) next = WORKER_RECONNECT_BACKOFF_MAX;
		ctx->reconnect_backoff_sec = next;
		fprintf(stderr, "[worker] reconnect failed — next attempt in >=%ds\n", next);
		return -1;
	}
	ctx->conn_dead = 0;
	ctx->reconnect_backoff_sec = 0;
	fprintf(stderr, "[worker] AMQP connection re-established\n");

	ctx->inflight_delivery_tag = 0;
	return 0;
}

void worker_keepalive(worker_ctx_t *ctx)
{
	if (!ctx || !ctx->conn || ctx->conn_dead) return;
	if (publish_conn_pump(ctx->conn) < 0)
		ctx->conn_dead = 1;
}

static int reap_child_only(worker_ctx_t *ctx)
{
	if (ctx->child_pid == 0) return 0;
	int status = 0;
	pid_t rc = waitpid(ctx->child_pid, &status, WNOHANG);
	if (rc == 0) return 0;
	if (rc < 0)  return -1;

	(void)status;
	ctx->child_pid = 0;
	return 1;
}

int worker_tick(worker_ctx_t *ctx)
{
	if (!ctx) return -1;

	int reaped = reap_child_only(ctx);
	if (reaped < 0) return -1;

	if (reconnect_if_dead(ctx) < 0) {

		return -1;
	}
	if (publish_conn_pump(ctx->conn) < 0) {
		ctx->conn_dead = 1;
		return -1;
	}

	if (ctx->child_pid == 0 && ctx->inflight_task_id[0] != '\0') {
		if (publish_reaped_result(ctx) < 0) {
			ctx->conn_dead = 1;
			return -1;
		}
	}

	if (try_pick_new_task(ctx) < 0) { ctx->conn_dead = 1; return -1; }

	return 0;
}
