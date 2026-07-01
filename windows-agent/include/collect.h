#ifndef ASSESSMENT_AGENT_COLLECT_H
#define ASSESSMENT_AGENT_COLLECT_H

#include "cJSON.h"

char *resolve_machine_id(void);

const char *cached_composite_id(const char *machine_id);

/* 첫 실행 시 생성·영구 저장하는 안정 식별자(UUID). MAC/machine_id와 무관. */
const char *cached_agent_id(void);

cJSON *collect_inventory_payload(const char *machine_id, const char *agent_version);

cJSON *collect_metrics_payload(const char *machine_id, const char *agent_version);

cJSON *build_error_payload(const char *machine_id,
                           const char *agent_version,
                           const char *error_code,
                           const char *error_message,
                           const char *failed_component,
                           int         retry_count,
                           const char *first_failed_at,
                           const char *recovered_at);

#endif
