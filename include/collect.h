/**
 * @file collect.h
 * @brief Inventory and metrics collectors. Builds v2 payloads.
 *
 * All collectors are stateless. Cumulative counters (`/proc/stat`,
 * `/proc/diskstats`, `/proc/net/dev`) are emitted raw — the engine computes
 * deltas, percentages, and rates from two consecutive snapshots.
 */

#ifndef ASSESSMENT_AGENT_COLLECT_H
#define ASSESSMENT_AGENT_COLLECT_H

#include <cJSON.h>

/**
 * @brief Resolve the immutable server identifier.
 *
 * Resolution order:
 *   1. /etc/machine-id (32-char hex, systemd standard)
 *   2. `dbus-uuidgen --get` output
 *   3. Cloud metadata instance-id (AWS IMDSv2 / Azure / GCP)
 *
 * Returns a malloc'd string the caller must free, or NULL if all fallbacks
 * fail (treated as a fatal collect error by main).
 */
char *resolve_machine_id(void);

/**
 * @brief composite_id = sha256_hex(machine_id + "\n" + mac1 + "\n" + ...).
 *
 * Process-lifetime 1회 계산 후 캐시. 반환된 buffer 는 static 이라 caller 가
 * free 하지 않음. machine_id 가 NULL/empty 면 MAC 만 hash.
 *
 * agent 가 자기 task.install 큐 이름 (`agent.tasks.<composite_id>`) 빌드 +
 * payload `composite_id` 필드 채울 때 같은 값 사용.
 */
const char *cached_composite_id(const char *machine_id);

/**
 * @brief Produce an `inventory` payload conforming to docs/payload-schema.md §1.
 *
 * Common metadata (`message_type`, `machine_id`, `agent_version`,
 * `collected_at`, `hostname`, `message_id`) is included. `ip_external` may be
 * `null` if cloud metadata is unreachable.
 *
 * @param machine_id    Pre-resolved server id (must not be NULL).
 * @param agent_version Build-time version string.
 * @return cJSON object on success (caller deletes), NULL on critical collect failure.
 */
cJSON *collect_inventory_payload(const char *machine_id, const char *agent_version);

/**
 * @brief Produce a `metrics` payload conforming to docs/payload-schema.md §2.
 *
 * All `/proc` counters are emitted as raw cumulative values; no in-agent delta
 * computation. `mem_available_kb` is `null` on kernels lacking `MemAvailable`.
 *
 * @param machine_id    Pre-resolved server id.
 * @param agent_version Build-time version string.
 * @return cJSON object on success (caller deletes), NULL on critical collect failure.
 */
cJSON *collect_metrics_payload(const char *machine_id, const char *agent_version);

/**
 * @brief Produce an `error` payload conforming to docs/payload-schema.md §3.
 *
 * Used for agent-side collect/publish failures only. Consumer-side failures
 * are handled by the broker DLX, not by this message.
 *
 * @param machine_id        Server id (best-effort; pass empty string if unresolved).
 * @param agent_version     Build-time version string.
 * @param error_code        Classification code, e.g. `COLLECT_MEMINFO_FAILED`.
 * @param error_message     Human readable detail.
 * @param failed_component  `"collect"` or `"publish"`.
 * @param retry_count       Optional retry summary (>= 0). Pass -1 to omit.
 * @param first_failed_at   Optional ISO 8601 string. NULL to omit.
 * @param recovered_at      Optional ISO 8601 string. NULL to omit.
 * @return cJSON object (caller deletes). Never NULL unless OOM.
 */
cJSON *build_error_payload(const char *machine_id,
                           const char *agent_version,
                           const char *error_code,
                           const char *error_message,
                           const char *failed_component,
                           int         retry_count,
                           const char *first_failed_at,
                           const char *recovered_at);

#endif
