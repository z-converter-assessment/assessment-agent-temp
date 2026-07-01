#ifndef ASSESSMENT_AGENT_WIN_EXEC_H
#define ASSESSMENT_AGENT_WIN_EXEC_H

#include <stddef.h>

typedef enum {
	EXEC_INSTALL_TYPE_DIRECT_EXEC = 0,
	EXEC_INSTALL_TYPE_MSI         = 1,
} exec_install_type_t;

typedef enum {
	EXEC_OK = 0,
	EXEC_ERR_SCRIPT_FAILED,
	EXEC_ERR_SCRIPT_TIMEOUT,
	EXEC_ERR_SCRIPT_NOT_FOUND,
	EXEC_ERR_INTERNAL,
} exec_status_t;

typedef struct {
	int   exit_code;
	long  duration_ms;
	char  stdout_tail[4096];
	char  stderr_tail[4096];
} exec_result_t;

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
