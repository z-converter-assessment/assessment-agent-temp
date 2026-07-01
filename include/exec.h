#ifndef ASSESSMENT_AGENT_EXEC_H
#define ASSESSMENT_AGENT_EXEC_H

#include <stddef.h>
#include <sys/types.h>

typedef enum {
	EXEC_OK = 0,
	EXEC_ERR_SCRIPT_FAILED,
	EXEC_ERR_SCRIPT_TIMEOUT,
	EXEC_ERR_SCRIPT_NOT_FOUND,
	EXEC_ERR_INTERNAL,
} exec_status_t;

typedef struct {
	int   exit_code;
	int   signal_no;
	long  duration_ms;
	char  stdout_tail[4096];
	char  stderr_tail[4096];
} exec_result_t;

exec_status_t exec_install_script(const char  *extract_dir,
                                  const char  *script,
                                  const char **argv_extra,
                                  int          timeout_sec,
                                  int          mem_limit_mb,
                                  int          fsize_limit_mb,
                                  int          nofile_limit,
                                  const char  *task_id,
                                  const char  *machine_id,
                                  exec_result_t *out);

#endif
