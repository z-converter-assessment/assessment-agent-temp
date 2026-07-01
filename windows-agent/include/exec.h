/**
 * @file exec.h
 * @brief Sandboxed install execution (Windows).
 *
 * Linux exec.c 의 Windows 대응. Linux 는 install.sh (shell script) 만 처리하지만
 * Windows 는 install.type 에 따라 두 갈래로 분기:
 *   - "direct_exec" — 다운로드된 EXE 를 CreateProcessW 로 직접 실행
 *   - "msi"         — msiexec.exe /i {path} /quiet /norestart
 *
 * 자식 sandbox 설정:
 *   - lpEnvironment 에 minimal env block (PATH/TEMP/USERPROFILE/TASK_ID/MACHINE_ID).
 *     부모의 RABBITMQ_* / .env 값은 절대 상속하지 않음 (Linux clearenv + whitelist 와 동일).
 *   - CWD = work_dir (extract_dir 대응 — Windows 는 extract 없이 다운로드 dir 자체)
 *   - stdin = NUL (Linux /dev/null), stdout/stderr → anonymous pipe → ring buffer 4096 byte tail
 *   - Job Object assignment + JOBOBJECT_EXTENDED_LIMIT_INFORMATION:
 *       * JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE   — 부모 죽으면 자식 전체 정리
 *       * JOB_OBJECT_LIMIT_PROCESS_MEMORY      — mem_limit_mb (Linux RLIMIT_AS 대응)
 *       * JOB_OBJECT_LIMIT_ACTIVE_PROCESS      — active_proc_limit (Linux NOFILE 대응 아님,
 *                                                Windows 에서 fd cap 강제 어려움 — proc count 로 폭주 차단)
 *   - Wall-clock timeout — 부모가 WaitForSingleObject(handle, timeout_ms):
 *       timeout 만료 시 TerminateJobObject(job, 1) → +5s 후 강제 정리 보장
 *
 * msi exit code 정책:
 *   - 0       — EXEC_OK (정상)
 *   - 3010    — EXEC_OK (success — reboot required). reboot 신호는 task.result 에 별도 표시 안 함
 *   - 그 외   — EXEC_ERR_SCRIPT_FAILED
 */

#ifndef ASSESSMENT_AGENT_WIN_EXEC_H
#define ASSESSMENT_AGENT_WIN_EXEC_H

#include <stddef.h>

typedef enum {
	EXEC_INSTALL_TYPE_DIRECT_EXEC = 0,
	EXEC_INSTALL_TYPE_MSI         = 1,
} exec_install_type_t;

typedef enum {
	EXEC_OK = 0,                 /* exit_code == 0 (msi: 0 또는 3010) */
	EXEC_ERR_SCRIPT_FAILED,      /* exit_code != 0 (msi: 0/3010 외) */
	EXEC_ERR_SCRIPT_TIMEOUT,     /* wall-clock timeout → TerminateJobObject */
	EXEC_ERR_SCRIPT_NOT_FOUND,   /* target_file 존재하지 않음 */
	EXEC_ERR_INTERNAL,           /* CreateProcessW / Job Object / pipe 실패 등 */
} exec_status_t;

typedef struct {
	int   exit_code;             /* CreateProcessW 결과 (msi 의 3010 도 그대로 노출) */
	long  duration_ms;
	char  stdout_tail[4096];
	char  stderr_tail[4096];
} exec_result_t;

/**
 * @brief Execute install target with EC1-equivalent sandbox + Job Object containment.
 *
 * Job Object 핸들은 **caller 가 미리 만들어 전달** — worker_force_child_term 이
 * drain escalation 시 TerminateJobObject 로 자식 process tree 즉시 강제 종료할
 * 수 있도록 함. exec_install 은 받은 job 에 process attach + ExtendedLimitInformation
 * 설정만 책임. CloseHandle(job) 은 caller (worker.c) 의 책임.
 *
 * job_handle == NULL 이면 exec_install 이 자체 Job 생성 + 정리 (test / standalone 호출용
 * backward-compat). 이 경우 외부 강제 종료는 불가.
 *
 * @param job_handle        외부에서 미리 만든 Job Object 핸들 (또는 NULL).
 * @param type              EXEC_INSTALL_TYPE_DIRECT_EXEC | EXEC_INSTALL_TYPE_MSI
 * @param work_dir          Sandbox dir. 존재 가정. 자식 CWD.
 * @param target_file       direct_exec: 실행할 EXE 절대 경로. msi: .msi 절대 경로.
 * @param argv_extra        추가 인자 (NULL-terminated). NULL 가능.
 * @param timeout_sec       Wall-clock timeout. <= 0 이면 비활성.
 * @param mem_limit_mb      Job Object ProcessMemoryLimit (MB). <= 0 이면 미적용.
 * @param fsize_limit_mb    참고용 (Windows 직접 강제 어려움).
 * @param active_proc_limit Job Object ActiveProcessLimit. <= 0 이면 내부 default (32) 적용.
 * @param task_id           TASK_ID env 로 전달.
 * @param machine_id        MACHINE_ID env 로 전달.
 * @param out               호출 후 채워짐. INTERNAL 실패 시 호출자가 부분 값 의존 금지.
 */
exec_status_t exec_install(void                *job_handle,
                           exec_install_type_t  type,
                           const char          *work_dir,
                           const char          *target_file,
                           const char         **argv_extra,
                           int                  timeout_sec,
                           int                  mem_limit_mb,
                           int                  fsize_limit_mb,
                           int                  active_proc_limit,
                           const char          *task_id,
                           const char          *machine_id,
                           exec_result_t       *out);

#endif
