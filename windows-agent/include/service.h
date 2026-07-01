#ifndef ASSESSMENT_AGENT_SERVICE_H
#define ASSESSMENT_AGENT_SERVICE_H

#define ASSESSMENT_AGENT_SERVICE_NAME L"assessment-agent"

int run_as_service(void);

int run_as_console(void);

void request_stop(void);

int  stop_requested(void);

void service_stop_pending_update(unsigned long wait_hint_ms);

#endif
