#define _POSIX_C_SOURCE 200809L

#include "collect.h"
#include "installer.h"
#include "publish.h"
#include "util.h"
#include "worker.h"

#include <cJSON.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef AGENT_VERSION
#define AGENT_VERSION "0.0.0-dev"
#endif

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static time_t next_inventory_deadline(time_t now, int refresh_sec)
{
	return now + (time_t)jitter_seconds(refresh_sec, 0.15);
}

static publish_config_t make_publish_config(void)
{

	publish_config_t cfg = {
		.host        = getenv_default("RABBITMQ_HOST", "localhost"),
		.port        = getenv_int_or("RABBITMQ_PORT", 0),
		.vhost       = getenv_default("RABBITMQ_VHOST", "/"),
		.user        = getenv_default("RABBITMQ_USER", "admin"),
		.password    = getenv_default("RABBITMQ_PASS", "admin"),
		.exchange    = getenv_default("RABBITMQ_EXCHANGE", "assessment"),

		.heartbeat_sec = getenv_int_or("RABBITMQ_HEARTBEAT_SEC", 60),

		.tls_enabled = getenv_bool("RABBITMQ_TLS_ENABLED", 0),
		.tls_ca_path = getenv_default("RABBITMQ_TLS_CA_PATH", ""),
		.tls_verify_peer     = getenv_bool("RABBITMQ_TLS_VERIFY_PEER", 1),
		.tls_verify_hostname = getenv_bool("RABBITMQ_TLS_VERIFY_HOSTNAME", 1),
		.tls_cert_path = getenv_default("RABBITMQ_TLS_CERT_PATH", ""),
		.tls_key_path  = getenv_default("RABBITMQ_TLS_KEY_PATH", ""),
	};
	if (cfg.port <= 0)
		cfg.port = cfg.tls_enabled ? 5671 : 5672;
	return cfg;
}

static int serialize_and_publish(const publish_config_t *cfg,
                                 const char *routing_key,
                                 cJSON *msg)
{
	if (!msg)
		return -1;
	char *body = cJSON_PrintUnformatted(msg);
	int rc = -1;
	if (body)
		rc = publish_message(cfg, routing_key, body, strlen(body));
	free(body);
	cJSON_Delete(msg);
	return rc;
}

static void emit_error(const publish_config_t *cfg,
                       const char *machine_id,
                       const char *error_code,
                       const char *error_message,
                       const char *failed_component,
                       int retry_count,
                       const char *first_failed_at,
                       const char *recovered_at)
{
	const char *rk = getenv_default("RABBITMQ_ROUTING_KEY_ERROR", "server.error");
	cJSON *msg = build_error_payload(machine_id ? machine_id : "",
	                                 AGENT_VERSION,
	                                 error_code, error_message,
	                                 failed_component,
	                                 retry_count,
	                                 first_failed_at, recovered_at);
	int rc = serialize_and_publish(cfg, rk, msg);
	if (rc != 0) {
		fprintf(stderr, "[agent] failed to publish error message %s\n",
		        error_code ? error_code : "?");
	}
}

static int publish_with_retry(const publish_config_t *cfg,
                              const char *routing_key,
                              cJSON *msg,
                              const char *machine_id,
                              int max_backoff,
                              worker_ctx_t *worker)
{
	if (!msg)
		return -1;
	char *body = cJSON_PrintUnformatted(msg);
	cJSON_Delete(msg);
	if (!body)
		return -1;

	unsigned int backoff = 1;
	int retry_count = 0;
	char first_failed_at[32] = { 0 };

	for (;;) {
		int rc = publish_message(cfg, routing_key, body, strlen(body));
		if (rc == 0) {
			free(body);
			if (retry_count > 0) {
				char now_buf[32];
				struct timespec ts;
				clock_gettime(CLOCK_REALTIME, &ts);
				iso8601_utc(ts.tv_sec, now_buf, sizeof now_buf);
				char detail[160];
				snprintf(detail, sizeof detail,
				         "broker reconnected after %d retries", retry_count);
				emit_error(cfg, machine_id,
				           "PUBLISH_RECOVERED", detail, "publish",
				           retry_count, first_failed_at, now_buf);
			}
			return 0;
		}

		if (g_stop) {
			free(body);
			return -1;
		}

		if (retry_count == 0) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			iso8601_utc(ts.tv_sec, first_failed_at, sizeof first_failed_at);
		}
		retry_count++;

		if (backoff > (unsigned int)max_backoff)
			backoff = (unsigned int)max_backoff;
		fprintf(stderr, "[agent] publish failed, retry %d in %us\n",
		        retry_count, backoff);
		/* backoff 대기 중 worker 를 서비스한다 — metrics/inventory publish 실패가 worker 서브시스템
		 * (heartbeat pump·task 처리)을 굶기지 않게. backoff 를 chunk 로 쪼개 keepalive/tick 을 흘린다.
		 * worker 는 별도 exchange/계정이라 metrics 장애와 독립적으로 살아있을 수 있다. */
		{
			unsigned int rem = backoff;
			while (rem > 0 && !g_stop) {
				unsigned int s = rem > 5u ? 5u : rem;
				sleep(s);
				rem -= s;
				if (worker) {
					worker_keepalive(worker);
					worker_tick(worker);
				}
			}
		}
		backoff *= 2;
	}
}

static publish_config_t make_worker_publish_config(void)
{
	publish_config_t cfg = make_publish_config();
	cfg.user     = getenv_default("RABBITMQ_WORKER_USER", "");
	cfg.password = getenv_default("RABBITMQ_WORKER_PASS", "");
	cfg.exchange = getenv_default("WORKER_TASK_EXCHANGE", "assessment.tasks");
	return cfg;
}

static void log_optional_cmds(void)
{
	const char *cmds[] = { "lsblk", "curl", "dbus-uuidgen", NULL };
	for (int i = 0; cmds[i]; i++) {
		char buf[96];
		snprintf(buf, sizeof buf,
		         "command -v %s >/dev/null 2>&1", cmds[i]);
		int rc = system(buf);
		fprintf(stderr, "[agent] cmd %-13s %s\n",
		        cmds[i],
		        (rc == 0) ? "available"
		                  : "MISSING (silent fallback / null)");
	}
}

static void print_usage(void)
{
	fprintf(stderr,
	    "assessment-agent — Resource assessment collector\n"
	    "\n"
	    "Subcommands:\n"
	    "  install [--image-prep]   register systemd service (start unless --image-prep)\n"
	    "  uninstall [--purge]      stop + remove service (--purge also wipes state)\n"
	    "  prep-image               clear /etc/machine-id for VM image cloning\n"
	    "  (no args)                run the collector + worker (used by systemd)\n");
}

/* dry-run: 페이로드 하나를 stdout 에 pretty-print(발행 없음) — 실제 직렬화 코드를 태워
 * 필드/타입/null 이 스키마(schema/wire.schema.json)와 일치하는지 CI 가 검증하는 입력원. */
static int emit_payload(const char *which)
{
	load_env_file(".env");
	char *machine_id = resolve_machine_id();
	if (!machine_id) machine_id = strdup("");
	if (!machine_id) return 2;
	if (strcmp(which, "task.result") == 0) {
		char *s = worker_emit_sample_result_json(machine_id, AGENT_VERSION);
		free(machine_id);
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
		free(machine_id);
		return 2;
	}
	free(machine_id);
	if (!p) { fprintf(stderr, "[agent] emit: collect returned null\n"); return 1; }
	char *s = cJSON_Print(p);
	cJSON_Delete(p);
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
			    strcmp(argv[i], "--purge") == 0)
				flag = 1;
		}
		if (strcmp(cmd, "install") == 0)
			return installer_run_install(flag);
		if (strcmp(cmd, "uninstall") == 0)
			return installer_run_uninstall(flag);
		if (strcmp(cmd, "prep-image") == 0)
			return installer_run_prep_image();
		if (strcmp(cmd, "emit") == 0)
			return emit_payload(argc >= 3 ? argv[2] : "");
		if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
			print_usage();
			return 0;
		}
		fprintf(stderr, "[agent] unknown subcommand: %s\n\n", cmd);
		print_usage();
		return 2;
	}

	signal(SIGINT,  on_signal);
	signal(SIGTERM, on_signal);

	signal(SIGPIPE, SIG_IGN);

	umask(077);

	load_env_file(".env");
	log_optional_cmds();

	srand((unsigned int)(time(NULL) ^ getpid()));

	int interval = getenv_int_or("AGENT_INTERVAL_SEC", 60);
	int inv_refresh = getenv_int_or("AGENT_INVENTORY_REFRESH_SEC", 3600);
	publish_config_t cfg = make_publish_config();

	const char *rk_inv = getenv_default("RABBITMQ_ROUTING_KEY_INVENTORY", "server.inventory");
	const char *rk_met = getenv_default("RABBITMQ_ROUTING_KEY_METRICS",   "server.metrics");

	char *machine_id = resolve_machine_id();
	if (!machine_id) {

		fprintf(stderr, "[agent] machine_id unresolved (no /etc/machine-id, no "
		                "dbus-uuidgen, no IMDS) — continuing; composite_id is "
		                "derived from MAC addresses (machine_id is display-only).\n");
		machine_id = strdup("");
		if (!machine_id) return 2;
	}
	fprintf(stderr, "[agent] machine_id=%s\n", machine_id);

	cJSON *inv = collect_inventory_payload(machine_id, AGENT_VERSION);
	if (!inv) {
		emit_error(&cfg, machine_id,
		           "COLLECT_INVENTORY_FAILED",
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
			emit_error(&cfg, machine_id,
			           "COLLECT_METRICS_FAILED",
			           "core metrics source unreadable", "collect",
			           -1, NULL, NULL);
			free(machine_id);
			return 1;
		}
		int rc = serialize_and_publish(&cfg, rk_met, m);
		free(machine_id);
		return rc == 0 ? 0 : 1;
	}

	worker_ctx_t *worker = NULL;
	/* worker 활성은 USER+PASS 둘 다 요구 — 비번 없이는 브로커 인증이 어차피 실패하므로 조용한 연결
	 * 실패 대신 명확히 비활성(Windows 트리와 동일 게이팅). */
	const char *worker_user = getenv_default("RABBITMQ_WORKER_USER", "");
	const char *worker_pass = getenv_default("RABBITMQ_WORKER_PASS", "");
	if (*worker_user && *worker_pass) {

		publish_config_t wcfg = make_worker_publish_config();

		const char *queue_prefix = getenv_default("WORKER_TASK_QUEUE_PREFIX", "agent.tasks");

		/* 식별·라우팅은 안정 agent_id 기준 — 엔진이 agent.tasks.{agent_id}를 declare/route.
		 * composite_id/machine_id는 payload 감사용. */
		const char *aid = cached_agent_id();
		char queue_name[256];
		snprintf(queue_name, sizeof queue_name, "%s.%s", queue_prefix, aid);

		worker_config_t wc = {
			.amqp                = wcfg,
			.queue_name          = queue_name,
			.result_routing_key  = getenv_default("WORKER_TASK_RESULT_KEY", "task.result"),
			.machine_id          = machine_id,
			.agent_version       = AGENT_VERSION,
			.state_dir           = getenv_default("WORKER_STATE_DIR", "/var/lib/agent-worker"),
			.tmp_dir             = getenv_default("WORKER_TMP_DIR",   "/tmp"),
			.allowed_hosts_csv   = getenv_default("WORKER_DOWNLOAD_ALLOWED_HOSTS", ""),
			.done_retention_sec  = getenv_int_or("WORKER_DONE_RETENTION_SEC", 604800),
			.disk_reserve_mb     = getenv_int_or("WORKER_DISK_RESERVE_MB", 50),
			.mem_limit_mb        = getenv_int_or("WORKER_INSTALL_MEM_LIMIT_MB",   2048),
			.fsize_limit_mb      = getenv_int_or("WORKER_INSTALL_FSIZE_LIMIT_MB", 5120),
			.nofile_limit        = getenv_int_or("WORKER_INSTALL_NOFILE",        4096),
		};
		worker = worker_init(&wc);
		if (!worker) {
			fprintf(stderr, "[agent] worker init failed — running collector only\n");
		}
	} else {
		fprintf(stderr, "[agent] worker disabled (RABBITMQ_WORKER_USER/PASS unset)\n");
	}

	fprintf(stderr, "[agent] loop mode: interval=%ds, inventory_refresh=%ds, worker=%s (Ctrl+C to exit)\n",
	        interval, inv_refresh, worker ? "on" : "off");

	time_t inv_next = (inv_refresh > 0)
		? next_inventory_deadline(time(NULL), inv_refresh)
		: 0;

	while (!g_stop) {
		cJSON *m = collect_metrics_payload(machine_id, AGENT_VERSION);
		if (!m) {
			emit_error(&cfg, machine_id,
			           "COLLECT_METRICS_FAILED",
			           "core metrics source unreadable", "collect",
			           -1, NULL, NULL);
		} else {
			publish_with_retry(&cfg, rk_met, m, machine_id, interval, worker);
		}

		if (inv_refresh > 0 && time(NULL) >= inv_next) {
			cJSON *iv = collect_inventory_payload(machine_id, AGENT_VERSION);
			if (!iv) {
				emit_error(&cfg, machine_id,
				           "COLLECT_INVENTORY_FAILED",
				           "core inventory source unreadable", "collect",
				           -1, NULL, NULL);
			} else if (publish_with_retry(&cfg, rk_inv, iv, machine_id, interval, worker) == 0) {
				fprintf(stderr, "[agent] republished inventory (periodic)\n");
			}
			inv_next = next_inventory_deadline(time(NULL), inv_refresh);
		}

		if (worker) {
			if (worker_tick(worker) < 0)
				fprintf(stderr, "[agent] worker tick error — continuing\n");
		}

		if (g_stop) break;

		int remaining = interval;
		const int chunk = 25;
		while (remaining > 0 && !g_stop) {
			int s = remaining > chunk ? chunk : remaining;
			sleep((unsigned int)s);
			remaining -= s;
			if (g_stop) break;
			if (worker) worker_keepalive(worker);
		}
	}

	if (worker) {
		worker_begin_drain(worker);
		int grace_sec   = getenv_int_or("AGENT_DRAIN_GRACE_SEC",   600);
		int term_sec    = getenv_int_or("AGENT_DRAIN_TERM_SEC",    30);

		int publish_sec = getenv_int_or("AGENT_DRAIN_PUBLISH_SEC", 180);
		fprintf(stderr, "[agent] draining worker (grace=%ds, term=%ds, publish-stuck=%ds)\n",
		        grace_sec, term_sec, publish_sec);

		time_t t0 = time(NULL);
		time_t reap_done_at = 0;
		int term_sent = 0;
		int kill_sent = 0;
		while (!worker_idle(worker)) {
			worker_tick(worker);
			long elapsed = (long)(time(NULL) - t0);

			if (!term_sent && elapsed >= grace_sec) {
				worker_force_child_term(worker, 0);
				term_sent = 1;
			} else if (term_sent && !kill_sent && elapsed >= grace_sec + term_sec) {
				worker_force_child_term(worker, 1);
				kill_sent = 1;
			}

			if (reap_done_at == 0 && !worker_has_live_child(worker))
				reap_done_at = time(NULL);
			if (reap_done_at != 0 &&
			    (long)(time(NULL) - reap_done_at) >= publish_sec) {
				fprintf(stderr, "[agent] drain: result publish stuck for %ds — giving up; result file will replay on next startup\n",
				        publish_sec);
				break;
			}
			sleep(1);
		}
		worker_shutdown(worker);
	}

	free(machine_id);
	return 0;
}
