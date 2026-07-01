/**
 * @file service.h
 * @brief Windows Service Control Manager dispatcher.
 *
 * The agent runs in one of two modes:
 *   - Service mode (default when launched by SCM via `sc.exe start`):
 *     StartServiceCtrlDispatcher → ServiceMain → RegisterServiceCtrlHandlerEx
 *     → collection loop until SCM sends SERVICE_CONTROL_STOP.
 *
 *   - Console mode (`assessment-agent.exe --console`): bypass SCM, run the
 *     same collection loop with a Ctrl+C handler. Used for local debugging
 *     and during install.ps1 smoke tests.
 *
 * SERVICE_NAME 은 install.ps1 의 `sc.exe create` 명칭과 정확히 일치해야 함.
 */

#ifndef ASSESSMENT_AGENT_SERVICE_H
#define ASSESSMENT_AGENT_SERVICE_H

#define ASSESSMENT_AGENT_SERVICE_NAME L"assessment-agent"

/**
 * @brief Hand off to the Service Control Manager.
 *        Blocks until SCM dispatches ServiceMain and the agent exits.
 *        Returns 0 on clean stop, non-zero on dispatcher / init failure.
 */
int run_as_service(void);

/**
 * @brief Foreground / console mode — same loop without SCM.
 *        Ctrl+C handler triggers stop. Returns the agent exit code.
 */
int run_as_console(void);

/**
 * @brief Request the collection loop to exit.
 *        Safe to call from SCM control handler or console signal handler.
 */
void request_stop(void);

/**
 * @brief Returns non-zero when a stop has been requested.
 *        The loop polls this between iterations.
 */
int  stop_requested(void);

/**
 * @brief drain 도중 SCM 에 "아직 진행 중" 신호를 주기적으로 보냄.
 *
 * 호출하지 않으면 service_ctrl_handler 가 설정한 30초 dwWaitHint 만료 후 SCM 이
 * service stuck 판정 → 강제 kill 가능. drain 4-phase (GRACE 600s + TERM 30s +
 * PUBLISH 180s) 가 30초보다 길어지므로 main 의 drain loop 가 반드시 이 함수를 호출.
 *
 * dwCheckPoint 가 자동 증분되어 SCM 이 progress 인식.
 *
 * @param wait_hint_ms 다음 update 까지의 expected 대기 (밀리초). 1초 호출 주기면
 *                     1500~3000 정도 권장 (한 슬라이스 + 여유).
 */
void service_stop_pending_update(unsigned long wait_hint_ms);

#endif
