#include "collect.h"
#include "installer.h"
#include "publish.h"
#include "service.h"
#include "util.h"
#include "worker.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <winsock2.h>
#include <windows.h>

#ifndef AGENT_VERSION
#define AGENT_VERSION "0.0.0-dev"
#endif

static const char *user_worker_state_default(void)
{
	static char buf[MAX_PATH];
	if (buf[0]) return buf;
	if (agent_data_path_a("worker", buf, sizeof buf) != 0)
		buf[0] = '\0';
	return buf[0] ? buf : "C:\\Users\\Public\\assessment-agent\\worker";
}

static const char *user_tmp_default(void)
{
	const char *t = getenv("TEMP");
	if (t && *t) return t;
	t = getenv("TMP");
	return (t && *t) ? t : "C:\\Windows\\Temp";
}

static publish_config_t make_publish_config(void)
{

	publish_config_t cfg;
	memset(&cfg, 0, sizeof cfg);
	cfg.host                = getenv_default("RABBITMQ_HOST", "localhost");
	cfg.port                = getenv_int_or ("RABBITMQ_PORT", 0);
	cfg.vhost               = getenv_default("RABBITMQ_VHOST", "/");
	cfg.user                = getenv_default("RABBITMQ_USER", "admin");
	cfg.password            = getenv_default("RABBITMQ_PASS", "admin");
	cfg.exchange            = getenv_default("RABBITMQ_EXCHANGE", "assessment");
	cfg.heartbeat_sec       = getenv_int_or ("RABBITMQ_HEARTBEAT_SEC", 60);
	cfg.tls_enabled         = getenv_bool   ("RABBITMQ_TLS_ENABLED", 0);
	cfg.tls_ca_path         = getenv_default("RABBITMQ_TLS_CA_PATH", "");
	cfg.tls_verify_peer     = getenv_bool   ("RABBITMQ_TLS_VERIFY_PEER", 1);
	cfg.tls_verify_hostname = getenv_bool   ("RABBITMQ_TLS_VERIFY_HOSTNAME", 1);
	cfg.tls_cert_path       = getenv_default("RABBITMQ_TLS_CERT_PATH", "");
	cfg.tls_key_path        = getenv_default("RABBITMQ_TLS_KEY_PATH", "");
	if (cfg.port <= 0)
		cfg.port = cfg.tls_enabled ? 5671 : 5672;
	return cfg;
}

static worker_config_t make_worker_config(const publish_config_t *pcfg,
                                          const char *machine_id,
                                          char *queue_name_buf,
                                          size_t qbuf_sz)
{
	worker_config_t wcfg;
	memset(&wcfg, 0, sizeof wcfg);

	const char *wuser = getenv("RABBITMQ_WORKER_USER");
	const char *wpass = getenv("RABBITMQ_WORKER_PASS");
	if (!wuser || !*wuser || !wpass || !*wpass) {
		return wcfg;
	}

	wcfg.amqp           = *pcfg;
	wcfg.amqp.user      = wuser;
	wcfg.amqp.password  = wpass;
	wcfg.amqp.exchange  = getenv_default("WORKER_TASK_EXCHANGE", "assessment.tasks");

	/* 식별·라우팅은 agent_id 기준(엔진이 agent.tasks.{agent_id} declare/route).
	 * composite_id/machine_id 는 감사용으로 payload 에만 실린다. */
	const char *aid = cached_agent_id();
	snprintf(queue_name_buf, qbuf_sz, "%s.%s",
	         getenv_default("WORKER_TASK_QUEUE_PREFIX", "agent.tasks"),
	         aid);
	wcfg.queue_name         = queue_name_buf;
	wcfg.result_routing_key = getenv_default("WORKER_TASK_RESULT_KEY", "task.result");
	wcfg.machine_id         = machine_id;
	wcfg.agent_version      = AGENT_VERSION;
	wcfg.state_dir          = getenv_default("WORKER_STATE_DIR", user_worker_state_default());
	wcfg.tmp_dir            = getenv_default("WORKER_TMP_DIR",   user_tmp_default());
	wcfg.allowed_hosts_csv  = getenv_default("WORKER_DOWNLOAD_ALLOWED_HOSTS", "");
	wcfg.done_retention_sec = getenv_int_or ("WORKER_DONE_RETENTION_SEC", 604800);
	wcfg.disk_reserve_mb    = getenv_int_or ("WORKER_DISK_RESERVE_MB",    50);
	wcfg.mem_limit_mb       = getenv_int_or ("WORKER_INSTALL_MEM_LIMIT_MB",   2048);
	wcfg.fsize_limit_mb     = getenv_int_or ("WORKER_INSTALL_FSIZE_LIMIT_MB", 5120);
	wcfg.active_proc_limit  = getenv_int_or ("WORKER_INSTALL_ACTIVE_PROC_LIMIT", 32);
	return wcfg;
}

static int serialize_and_publish(const publish_config_t *cfg,
                                 const char *routing_key, cJSON *msg)
{
	if (!msg) return -1;
	char *body = cJSON_PrintUnformatted(msg);
	int rc = -1;
	if (body) rc = publish_message(cfg, routing_key, body, strlen(body));
	free(body);
	cJSON_Delete(msg);
	return rc;
}

static void emit_error(const publish_config_t *cfg, const char *machine_id,
                       const char *code, const char *msg, const char *comp,
                       int retry_count, const char *first_at,
                       const char *recovered_at)
{
	const char *rk = getenv_default("RABBITMQ_ROUTING_KEY_ERROR", "server.error");
	cJSON *e = build_error_payload(machine_id ? machine_id : "",
	                               AGENT_VERSION,
	                               code, msg, comp,
	                               retry_count, first_at, recovered_at);
	int rc = serialize_and_publish(cfg, rk, e);
	if (rc != 0)
		fprintf(stderr, "[agent] failed to publish error %s\n",
		        code ? code : "?");
}

static void interruptible_sleep(int seconds, worker_ctx_t *worker)
{
	for (int i = 0; i < seconds && !stop_requested(); i++) {
		Sleep(1000);
		if (worker) worker_keepalive(worker);
	}
}

static time_t next_inventory_deadline(time_t now, int refresh_sec)
{
	return now + (time_t)jitter_seconds(refresh_sec, 0.15);
}

static int publish_with_retry(const publish_config_t *cfg, const char *rk,
                              cJSON *msg, const char *machine_id,
                              int max_backoff, worker_ctx_t *worker)
{
	if (!msg) return -1;
	char *body = cJSON_PrintUnformatted(msg);
	cJSON_Delete(msg);
	if (!body) return -1;

	unsigned int backoff = 1;
	int retry_count = 0;
	char first_failed_at[32] = {0};

	for (;;) {
		int rc = publish_message(cfg, rk, body, strlen(body));
		if (rc == 0) {
			free(body);
			if (retry_count > 0) {
				time_t now; time(&now);
				char now_buf[32];
				iso8601_utc(now, now_buf, sizeof now_buf);
				char detail[160];
				snprintf(detail, sizeof detail,
				         "broker reconnected after %d retries", retry_count);
				emit_error(cfg, machine_id, "PUBLISH_RECOVERED",
				           detail, "publish",
				           retry_count, first_failed_at, now_buf);
			}
			return 0;
		}
		if (stop_requested()) { free(body); return -1; }

		if (retry_count == 0) {
			time_t now; time(&now);
			iso8601_utc(now, first_failed_at, sizeof first_failed_at);
		}
		retry_count++;
		if (backoff > (unsigned int)max_backoff)
			backoff = (unsigned int)max_backoff;
		fprintf(stderr, "[agent] publish failed, retry %d in %us\n",
		        retry_count, backoff);

		/* backoff 대기 중 worker 서비스: interruptible_sleep 이 초당 heartbeat 를 흘리고 chunk 마다
		 * worker_tick 으로 task 도 처리해 metrics 장애가 worker(heartbeat/task)를 굶기지 않게 한다.
		 * worker 는 별도 exchange/계정이라 metrics 장애와 독립적으로 살아있을 수 있다. */
		{
			int rem = (int)backoff;
			while (rem > 0 && !stop_requested()) {
				int s = rem > 5 ? 5 : rem;
				interruptible_sleep(s, worker);
				rem -= s;
				if (worker) worker_tick(worker);
			}
		}
		if (stop_requested()) { free(body); return -1; }
		backoff *= 2;
	}
}

int agent_run(void)
{

	const char *dbg_path = getenv("AGENT_DEBUG_LOG");
	int g_dbg = (dbg_path && *dbg_path);
	if (g_dbg) {
		freopen(dbg_path, "w", stderr);
		setvbuf(stderr, NULL, _IONBF, 0);
	} else {
		/* As a service there is no console -> stderr's handle is invalid, and
		 * unconditional fprintf(stderr) FAULTs on NT5.2 (2003/XP) msvcrt (NT6+
		 * tolerates it), killing the agent. Point stderr at NUL so writes no-op. */
		freopen("NUL", "w", stderr);
	}
#define DBG(...) do { if (g_dbg) fprintf(stderr, __VA_ARGS__); } while (0)

	DBG("[dbg] enter; WSAStartup\n");
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	DBG("[dbg] after WSAStartup\n");

	load_env_file(".env");
	{
		char p[MAX_PATH];
		if (agent_data_path_a("agent.env", p, sizeof p) == 0)
			load_env_file(p);
		if (agent_data_path_a("agent.env.local", p, sizeof p) == 0)
			load_env_file(p);
	}

	srand((unsigned int)(time(NULL) ^ GetCurrentProcessId()));

	int interval    = getenv_int_or("AGENT_INTERVAL_SEC", 60);
	int inv_refresh = getenv_int_or("AGENT_INVENTORY_REFRESH_SEC", 3600);
	publish_config_t cfg = make_publish_config();

	const char *rk_inv = getenv_default("RABBITMQ_ROUTING_KEY_INVENTORY", "server.inventory");
	const char *rk_met = getenv_default("RABBITMQ_ROUTING_KEY_METRICS",   "server.metrics");

	char *machine_id = resolve_machine_id();
	if (!machine_id) {
		fprintf(stderr, "[agent] machine_id resolution failed "
		                "(HKLM\\SOFTWARE\\Microsoft\\Cryptography\\MachineGuid missing)\n");
		emit_error(&cfg, NULL, "MACHINE_ID_UNRESOLVED",
		           "registry MachineGuid missing", "collect",
		           -1, NULL, NULL);
		WSACleanup();
		return 2;
	}
	fprintf(stderr, "[agent] machine_id=%s\n", machine_id);

	DBG("[dbg] before collect_inventory\n");
	cJSON *inv = collect_inventory_payload(machine_id, AGENT_VERSION);
	DBG("[dbg] after collect_inventory inv=%p\n", (void*)inv);
	if (!inv) {
		emit_error(&cfg, machine_id, "COLLECT_INVENTORY_FAILED",
		           "core inventory source unreadable", "collect",
		           -1, NULL, NULL);
		fprintf(stderr, "[agent] inventory collect failed; continuing with metrics only\n");
	} else {
		int max_backoff = interval > 0 ? interval : 60;
		if (publish_with_retry(&cfg, rk_inv, inv, machine_id, max_backoff, NULL) == 0)
			fprintf(stderr, "[agent] published inventory\n");
	}

	if (interval <= 0) {
		cJSON *m = collect_metrics_payload(machine_id, AGENT_VERSION);
		if (!m) {
			emit_error(&cfg, machine_id, "COLLECT_METRICS_FAILED",
			           "core metrics source unreadable", "collect",
			           -1, NULL, NULL);
			free(machine_id); WSACleanup();
			return 1;
		}
		int rc = serialize_and_publish(&cfg, rk_met, m);
		free(machine_id); WSACleanup();
		return rc == 0 ? 0 : 1;
	}

	fprintf(stderr, "[agent] loop mode: interval=%ds, inventory_refresh=%ds\n",
	        interval, inv_refresh);

	char wqueue_buf[128] = {0};
	worker_ctx_t *worker = NULL;
	{
		worker_config_t wcfg = make_worker_config(&cfg, machine_id,
		                                          wqueue_buf, sizeof wqueue_buf);
		if (wcfg.queue_name && *wcfg.queue_name) {
			worker = worker_init(&wcfg);
			fprintf(stderr, "[agent] worker %s (queue=%s)\n",
			        worker ? "enabled" : "init failed — publish-only",
			        wcfg.queue_name);
		} else {
			fprintf(stderr, "[agent] worker disabled (RABBITMQ_WORKER_USER/PASS unset)\n");
		}
	}

	time_t inv_next = (inv_refresh > 0)
		? next_inventory_deadline(time(NULL), inv_refresh) : 0;

	while (!stop_requested()) {
		cJSON *m = collect_metrics_payload(machine_id, AGENT_VERSION);
		if (!m) {
			emit_error(&cfg, machine_id, "COLLECT_METRICS_FAILED",
			           "core metrics source unreadable", "collect",
			           -1, NULL, NULL);
		} else {
			publish_with_retry(&cfg, rk_met, m, machine_id, interval, worker);
		}

		if (inv_refresh > 0 && time(NULL) >= inv_next) {
			cJSON *iv = collect_inventory_payload(machine_id, AGENT_VERSION);
			if (!iv) {
				emit_error(&cfg, machine_id, "COLLECT_INVENTORY_FAILED",
				           "core inventory source unreadable", "collect",
				           -1, NULL, NULL);
			} else if (publish_with_retry(&cfg, rk_inv, iv, machine_id, interval, worker) == 0) {
				fprintf(stderr, "[agent] republished inventory (periodic)\n");
			}
			inv_next = next_inventory_deadline(time(NULL), inv_refresh);
		}

		if (worker) worker_tick(worker);

		if (stop_requested()) break;
		interruptible_sleep(interval, worker);
	}

	if (worker) {
		worker_begin_drain(worker);
		int grace_sec   = getenv_int_or("AGENT_DRAIN_GRACE_SEC",   600);
		int term_sec    = getenv_int_or("AGENT_DRAIN_TERM_SEC",    30);
		int publish_sec = getenv_int_or("AGENT_DRAIN_PUBLISH_SEC", 180);
		fprintf(stderr, "[agent] draining worker (grace=%ds, term=%ds, publish-stuck=%ds)\n",
		        grace_sec, term_sec, publish_sec);

		ULONGLONG t0 = monotonic_ms();
		ULONGLONG reap_done_at = 0;
		int term_sent = 0;
		int kill_sent = 0;

		while (!worker_idle(worker)) {
			worker_tick(worker);

			service_stop_pending_update(2500);
			long elapsed = (long)((monotonic_ms() - t0) / 1000ULL);

			if (!term_sent && elapsed >= grace_sec) {
				fprintf(stderr, "[agent] drain phase 2: soft term\n");
				worker_force_child_term(worker, 0);
				term_sent = 1;
			} else if (term_sent && !kill_sent && elapsed >= grace_sec + term_sec) {
				fprintf(stderr, "[agent] drain phase 3: hard kill\n");
				worker_force_child_term(worker, 1);
				kill_sent = 1;
			}

			if (reap_done_at == 0 && !worker_has_live_child(worker))
				reap_done_at = monotonic_ms();
			if (reap_done_at != 0 &&
			    (long)((monotonic_ms() - reap_done_at) / 1000ULL) >= publish_sec) {
				fprintf(stderr, "[agent] drain: result publish stuck for %ds - giving up; "
				                "result file will replay on next startup\n", publish_sec);
				break;
			}
			Sleep(1000);
		}
		worker_shutdown(worker);
	}

	fprintf(stderr, "[agent] stopping (machine_id=%s)\n", machine_id);
	free(machine_id);
	WSACleanup();
	return 0;
#undef DBG
}

static void print_usage(void)
{
	fprintf(stderr,
	    "assessment-agent.exe — Resource assessment collector\n"
	    "\n"
	    "Registers a LocalSystem auto-start Windows service. Run install from an\n"
	    "elevated (Administrator) prompt. Files live under\n"
	    "%%ProgramData%%\\assessment-agent.\n"
	    "\n"
	    "Subcommands:\n"
	    "  install [--image-prep]    install + register the Windows service\n"
	    "                            (start unless --image-prep)\n"
	    "  uninstall                 stop + remove the service\n"
	    "  prep-image [--sysprep]    regenerate MachineGuid for image cloning\n"
	    "                            (best-effort; needs admin, never fatal)\n"
	    "  --console, -c, run        foreground run (for manual testing)\n");
}

/* dry-run: 한 페이로드를 수집해 stdout 으로 출력(발행 없음). wire 계약(schema/wire.schema.json)
 * 검증 입력원 — 실제 직렬화 코드를 태워 필드/타입/null 이 스키마와 일치하는지 CI 가 강제한다. */
static int emit_payload(const char *which)
{
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	load_env_file(".env");
	char *machine_id = resolve_machine_id();
	if (!machine_id) machine_id = strdup("");
	if (!machine_id) { WSACleanup(); return 2; }
	if (strcmp(which, "task.result") == 0) {
		char *s = worker_emit_sample_result_json(machine_id, AGENT_VERSION);
		free(machine_id); WSACleanup();
		if (!s) { fprintf(stderr, "[agent] emit: task.result build failed\n"); return 1; }
		printf("%s\n", s);
		free(s);
		return 0;
	}
	cJSON *p = NULL;
	if (strcmp(which, "inventory") == 0)
		p = collect_inventory_payload(machine_id, AGENT_VERSION);
	else if (strcmp(which, "metrics") == 0)
		p = collect_metrics_payload(machine_id, AGENT_VERSION);
	else if (strcmp(which, "error") == 0)
		p = build_error_payload(machine_id, AGENT_VERSION, "SAMPLE_ERROR",
		                        "contract emit sample", "collect", 0,
		                        "1970-01-01T00:00:00Z", "1970-01-01T00:00:00Z");
	else {
		fprintf(stderr, "[agent] emit: expected 'inventory', 'metrics', 'task.result', or 'error', got '%s'\n", which);
		free(machine_id); WSACleanup(); return 2;
	}
	free(machine_id);
	if (!p) { fprintf(stderr, "[agent] emit: collect returned null\n"); WSACleanup(); return 1; }
	char *s = cJSON_Print(p);
	cJSON_Delete(p);
	WSACleanup();
	if (!s) return 1;
	printf("%s\n", s);
	free(s);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2) {
		const char *cmd = argv[1];
		int flag = 0;
		for (int i = 2; i < argc; i++) {
			if (strcmp(argv[i], "--image-prep") == 0 ||
			    strcmp(argv[i], "--sysprep") == 0)
				flag = 1;
		}
		if (strcmp(cmd, "install") == 0)
			return installer_run_install(flag);
		if (strcmp(cmd, "uninstall") == 0)
			return installer_run_uninstall();
		if (strcmp(cmd, "prep-image") == 0)
			return installer_run_prep_image(flag);
		if (strcmp(cmd, "emit") == 0)
			return emit_payload(argc >= 3 ? argv[2] : "");
		if (strcmp(cmd, "--console") == 0 || strcmp(cmd, "-c") == 0 ||
		    strcmp(cmd, "run") == 0)
			return run_as_console();
		if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
			print_usage();
			return 0;
		}
		fprintf(stderr, "[agent] unknown subcommand: %s\n\n", cmd);
		print_usage();
		return 2;
	}
	return run_as_service();
}
