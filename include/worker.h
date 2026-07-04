#ifndef ASSESSMENT_AGENT_WORKER_H
#define ASSESSMENT_AGENT_WORKER_H

#include "publish.h"

#include <stddef.h>

typedef struct worker_ctx_s worker_ctx_t;

typedef struct {
	publish_config_t amqp;
	const char      *queue_name;
	const char      *result_routing_key;
	const char      *machine_id;
	const char      *agent_version;
	const char      *state_dir;
	const char      *tmp_dir;
	const char      *allowed_hosts_csv;
	int              done_retention_sec;
	int              disk_reserve_mb;
	int              mem_limit_mb;
	int              fsize_limit_mb;
	int              nofile_limit;
} worker_config_t;

/* emit dry-run 계약 검증용 대표 task.result JSON(호출자 free). 실제 발행 경로와 동일 직렬화. */
char *worker_emit_sample_result_json(const char *machine_id, const char *agent_version);

worker_ctx_t *worker_init(const worker_config_t *cfg);

int worker_tick(worker_ctx_t *ctx);

void worker_keepalive(worker_ctx_t *ctx);

void worker_begin_drain(worker_ctx_t *ctx);

void worker_force_child_term(worker_ctx_t *ctx, int hard);

int worker_idle(const worker_ctx_t *ctx);

int worker_has_live_child(const worker_ctx_t *ctx);

void worker_shutdown(worker_ctx_t *ctx);

#endif
