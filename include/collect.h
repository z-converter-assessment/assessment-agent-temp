#ifndef ASSESSMENT_AGENT_COLLECT_H
#define ASSESSMENT_AGENT_COLLECT_H

#include <cJSON.h>

char *resolve_machine_id(void);

const char *cached_composite_id(const char *machine_id);

/* task.result 등 inventory 외 메시지에서 os_family/os_id/os_version을 재사용. */
void collect_add_os_result_fields(cJSON *root);

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
