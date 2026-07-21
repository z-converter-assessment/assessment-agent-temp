#ifndef ASSESSMENT_AGENT_WORKER_INTERNAL_H
#define ASSESSMENT_AGENT_WORKER_INTERNAL_H

#include <sys/types.h>

/* worker_*.c 공용 내부 선언 — 파일시스템 plumbing(worker_state.c).
 * 공개 API 는 worker.h. 이 헤더는 worker_*.c 만 포함한다(collect_internal.h 와 동일 원리). */

int mkdir_p(const char *path, mode_t mode);
int file_exists(const char *path);
int fsync_parent_dir(const char *path);
int write_file_atomic(const char *path, const char *content);
int rmrf(const char *path);
int task_id_valid(const char *id);

#endif
