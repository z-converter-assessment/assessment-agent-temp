/**
 * @file worker.h
 * @brief task.install consumer state machine + idempotency / cleanup (Windows).
 *
 * Linux worker 와 동일한 외부 인터페이스. 내부 구현이 Windows native API 로 포팅:
 *   - fork/exec       → CreateProcessW
 *   - waitpid(WNOHANG) → WaitForSingleObject(handle, 0)
 *   - kill(-pgid,sig) → TerminateJobObject (Job Object 단위)
 *   - setrlimit       → JOBOBJECT_EXTENDED_LIMIT_INFORMATION
 *   - clearenv + whitelist → CreateProcessW lpEnvironment (minimal env block)
 *   - state dir       → %ProgramData%\\assessment-agent\\worker\\{results,done,running}\\
 *
 * 자기 OS 처리 범위:
 *   - install.type="direct_exec" — 다운로드 파일을 CreateProcessW 로 직접 실행
 *   - install.type="msi"         — msiexec.exe /i {path} /quiet /norestart
 *   - install.type="shell"       — unsupported_install_type 으로 즉시 reject + ack
 *
 * One-time API surface (Linux 와 동일):
 *   - worker_init / worker_tick / worker_keepalive / worker_begin_drain /
 *     worker_force_child_term / worker_idle / worker_has_live_child / worker_shutdown
 */

#ifndef ASSESSMENT_AGENT_WIN_WORKER_H
#define ASSESSMENT_AGENT_WIN_WORKER_H

#include "publish.h"

#include <stddef.h>

typedef struct worker_ctx_s worker_ctx_t;

typedef struct {
	publish_config_t amqp;                /* AMQP target + worker creds */
	const char      *queue_name;          /* e.g. "agent.tasks.<composite_id>" */
	const char      *result_routing_key;  /* e.g. "task.result" */
	const char      *machine_id;
	const char      *agent_version;
	const char      *state_dir;           /* e.g. "C:\\ProgramData\\assessment-agent\\worker" */
	const char      *tmp_dir;             /* e.g. %TEMP% or C:\\Windows\\Temp */
	const char      *allowed_hosts_csv;   /* WORKER_DOWNLOAD_ALLOWED_HOSTS */
	int              done_retention_sec;
	int              disk_reserve_mb;
	int              mem_limit_mb;        /* Job Object ProcessMemoryLimit */
	int              fsize_limit_mb;      /* (best-effort; Windows Job Object 직접 대응 없음) */
	int              active_proc_limit;   /* Job Object ActiveProcessLimit (Linux NOFILE 대응 아님) */
} worker_config_t;

/**
 * @brief Initialize the worker subsystem.
 *
 * State directories 생성, `/results` 의 미발행 result 파일 startup 재발행,
 * 만료된 `/done/*.json` 정리, broker 연결. 실패 시 NULL 반환.
 */
worker_ctx_t *worker_init(const worker_config_t *cfg);

/**
 * @brief One main-loop step. Non-blocking. 0 success, -1 irrecoverable.
 */
int worker_tick(worker_ctx_t *ctx);

/**
 * @brief AMQP heartbeat 유지용 — sleep 사이에 librabbitmq frame pump.
 */
void worker_keepalive(worker_ctx_t *ctx);

/**
 * @brief Service Stop 수신 — 신규 task pickup 정지. 진행 중 task 는 reap 까지 대기.
 */
void worker_begin_drain(worker_ctx_t *ctx);

/**
 * @brief 진행 중 child process group 에 종료 신호. @p hard != 0 → TerminateJobObject.
 *        @p hard == 0 → SetEvent(stop_event) + 정상 종료 유도 (install 측이 받아서 처리할
 *        보장은 없음 — Windows 의 graceful shutdown 은 console handler 한정).
 *        no-op 가능 (in-flight 없을 때).
 */
void worker_force_child_term(worker_ctx_t *ctx, int hard);

/**
 * @brief 1 if no child in-flight AND no result publish pending. drain 완료 판정.
 */
int worker_idle(const worker_ctx_t *ctx);

/**
 * @brief 1 if OS process still being awaited. drain escalation timer 의 kill target 존재 판정.
 */
int worker_has_live_child(const worker_ctx_t *ctx);

/**
 * @brief Free resources. 호출 전 in-flight child reap 완료 가정 (drain 패턴).
 */
void worker_shutdown(worker_ctx_t *ctx);

#endif
