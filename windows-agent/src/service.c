#include "service.h"

#include <stdio.h>
#include <string.h>

#include <windows.h>

extern int agent_run(void);

static volatile LONG       g_stop          = 0;
static SERVICE_STATUS_HANDLE g_status_handle = NULL;
static SERVICE_STATUS      g_status        = {0};

void request_stop(void)
{
	InterlockedExchange(&g_stop, 1);
}

int stop_requested(void)
{
	return InterlockedCompareExchange(&g_stop, 0, 0) != 0;
}

void service_stop_pending_update(unsigned long wait_hint_ms)
{
	if (!g_status_handle) return;

	static DWORD pending_checkpoint = 1;
	g_status.dwCurrentState  = SERVICE_STOP_PENDING;
	g_status.dwWin32ExitCode = NO_ERROR;
	g_status.dwWaitHint      = (DWORD)wait_hint_ms;
	g_status.dwControlsAccepted = 0;
	g_status.dwCheckPoint    = pending_checkpoint++;
	SetServiceStatus(g_status_handle, &g_status);
}

static void report_status(DWORD state, DWORD exit_code, DWORD wait_hint)
{
	static DWORD checkpoint = 1;
	g_status.dwCurrentState  = state;
	g_status.dwWin32ExitCode = exit_code;
	g_status.dwWaitHint      = wait_hint;

	g_status.dwControlsAccepted =
		(state == SERVICE_START_PENDING) ? 0
		                                 : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);

	g_status.dwCheckPoint =
		(state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0
		                                                       : checkpoint++;

	if (g_status_handle)
		SetServiceStatus(g_status_handle, &g_status);
}

static DWORD WINAPI service_ctrl_handler(DWORD ctrl, DWORD evt,
                                         LPVOID data, LPVOID ctx)
{
	(void)evt; (void)data; (void)ctx;
	switch (ctrl) {
	case SERVICE_CONTROL_STOP:
	case SERVICE_CONTROL_SHUTDOWN:
		report_status(SERVICE_STOP_PENDING, NO_ERROR, 30000);
		request_stop();
		return NO_ERROR;
	case SERVICE_CONTROL_INTERROGATE:
		return NO_ERROR;
	default:
		return ERROR_CALL_NOT_IMPLEMENTED;
	}
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv)
{
	(void)argc; (void)argv;

	g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	g_status_handle = RegisterServiceCtrlHandlerExW(
		ASSESSMENT_AGENT_SERVICE_NAME, service_ctrl_handler, NULL);
	if (!g_status_handle) return;

	report_status(SERVICE_START_PENDING, NO_ERROR, 3000);
	report_status(SERVICE_RUNNING,       NO_ERROR, 0);

	int rc = agent_run();

	report_status(SERVICE_STOPPED,
	              rc == 0 ? NO_ERROR : (DWORD)rc, 0);
}

int run_as_service(void)
{
	SERVICE_TABLE_ENTRYW dispatch[] = {
		{ (LPWSTR)ASSESSMENT_AGENT_SERVICE_NAME, service_main },
		{ NULL, NULL }
	};
	if (!StartServiceCtrlDispatcherW(dispatch)) {
		DWORD err = GetLastError();
		if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
			fprintf(stderr,
			        "[agent] not started by SCM — for foreground use --console\n");
		} else {
			fprintf(stderr,
			        "[agent] StartServiceCtrlDispatcher failed: %lu\n",
			        (unsigned long)err);
		}
		return 1;
	}
	return 0;
}

static BOOL WINAPI console_ctrl_handler(DWORD ctrl)
{
	switch (ctrl) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_SHUTDOWN_EVENT:
	case CTRL_LOGOFF_EVENT:
		request_stop();
		return TRUE;
	}
	return FALSE;
}

int run_as_console(void)
{
	SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
	fprintf(stderr, "[agent] console mode (Ctrl+C to stop)\n");
	return agent_run();
}
