#define WIN32_LEAN_AND_MEAN

#include "exec.h"
#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <windows.h>

#define WORK_DIR_MAX 1024

typedef struct {
	char  *buf;
	size_t cap;
	size_t len;
} tail_buf_t;

static void tail_append(tail_buf_t *t, const char *data, size_t n)
{
	if (!t->cap) return;
	for (size_t i = 0; i < n; i++) {
		t->buf[t->len % t->cap] = data[i];
		t->len++;
	}
}

static void tail_finalize(tail_buf_t *t, char *out, size_t out_sz)
{
	if (out_sz == 0) return;
	out[0] = '\0';
	if (!t->cap || t->len == 0) return;

	size_t kept = t->len < t->cap ? t->len : t->cap;
	if (kept >= out_sz) kept = out_sz - 1;

	if (t->len <= t->cap) {
		memcpy(out, t->buf, kept);
	} else {
		size_t start = t->len % t->cap;
		size_t first = t->cap - start;
		if (first > kept) first = kept;
		memcpy(out, t->buf + start, first);
		if (kept > first) memcpy(out + first, t->buf, kept - first);
	}
	for (size_t i = 0; i < kept; i++) {
		unsigned char c = (unsigned char)out[i];
		if (c == '\t' || c == '\n' || c == '\r') continue;
		if (c < 0x20 || c > 0x7e) out[i] = '?';
	}
	out[kept] = '\0';
}

/* env 블록에 "NAME=VAL\0" 엔트리를 덧붙인다. NUL 까지 다 들어갈 때만 써서 (cap - pos)
 * size_t 언더플로에 의한 버퍼 밖 쓰기와 부분 엔트리 잔존을 막는다. */
static void env_append(char *blk, size_t cap, int *pos, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int need = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (need < 0 || (size_t)*pos + (size_t)need + 1 >= cap)
		return;  /* 안 들어가면 스킵(부분 쓰기 금지) */
	va_start(ap, fmt);
	vsnprintf(blk + *pos, cap - (size_t)*pos, fmt, ap);
	va_end(ap);
	*pos += need + 1;  /* +1 = NUL 구분자 (calloc 로 이미 0) */
}

static char *build_env_block(const char *task_id, const char *machine_id)
{

	const char *path_env       = getenv("PATH");
	const char *temp_env       = getenv("TEMP");
	const char *userprofile    = getenv("USERPROFILE");
	const char *systemroot     = getenv("SystemRoot");
	if (!path_env)    path_env    = "C:\\Windows\\System32;C:\\Windows";
	if (!temp_env)    temp_env    = "C:\\Windows\\Temp";
	if (!userprofile) userprofile = "C:\\Windows\\Temp";
	if (!systemroot)  systemroot  = "C:\\Windows";

	size_t cap = 8192;
	char *blk = (char *)calloc(1, cap);
	if (!blk) return NULL;

	int pos = 0;
	env_append(blk, cap, &pos, "PATH=%s",        path_env);
	env_append(blk, cap, &pos, "TEMP=%s",        temp_env);
	env_append(blk, cap, &pos, "TMP=%s",         temp_env);
	env_append(blk, cap, &pos, "USERPROFILE=%s", userprofile);
	env_append(blk, cap, &pos, "SystemRoot=%s",  systemroot);
	if (task_id    && *task_id)
		env_append(blk, cap, &pos, "TASK_ID=%s", task_id);
	if (machine_id && *machine_id)
		env_append(blk, cap, &pos, "MACHINE_ID=%s", machine_id);

	return blk;
}

static void append_quoted(char *dst, size_t dst_sz, const char *src)
{
	size_t cur = strlen(dst);
	if (cur + 1 >= dst_sz) return;
	int needs_quote = (strchr(src, ' ') || strchr(src, '\t'));
	if (cur > 0 && cur + 1 < dst_sz) dst[cur++] = ' ';
	if (needs_quote && cur + 1 < dst_sz) dst[cur++] = '"';
	for (const char *p = src; *p && cur + 1 < dst_sz; p++) dst[cur++] = *p;
	if (needs_quote && cur + 1 < dst_sz) dst[cur++] = '"';
	dst[cur] = '\0';
}

static int build_cmdline(exec_install_type_t type,
                         const char *target_file,
                         const char **argv_extra,
                         char *out, size_t out_sz)
{
	if (out_sz == 0) return -1;
	out[0] = '\0';

	if (type == EXEC_INSTALL_TYPE_MSI) {
		append_quoted(out, out_sz, "msiexec.exe");
		append_quoted(out, out_sz, "/i");
		append_quoted(out, out_sz, target_file);
		append_quoted(out, out_sz, "/quiet");
		append_quoted(out, out_sz, "/norestart");
		return 0;
	}

	append_quoted(out, out_sz, target_file);
	if (argv_extra) {
		for (size_t i = 0; argv_extra[i]; i++)
			append_quoted(out, out_sz, argv_extra[i]);
	}
	return 0;
}

static HANDLE create_job(int mem_limit_mb, int active_proc_limit)
{
	HANDLE job = CreateJobObjectA(NULL, NULL);
	if (!job) return NULL;

	JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
	memset(&info, 0, sizeof info);
	info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

	if (mem_limit_mb > 0) {
		info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
		info.ProcessMemoryLimit = (SIZE_T)mem_limit_mb * 1024ULL * 1024ULL;
	}
	if (active_proc_limit > 0) {
		info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
		info.BasicLimitInformation.ActiveProcessLimit = (DWORD)active_proc_limit;
	}

	if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
	                             &info, sizeof info)) {
		CloseHandle(job);
		return NULL;
	}
	return job;
}

static int drain_once(HANDLE pipe, tail_buf_t *t)
{
	for (;;) {
		DWORD available = 0;
		if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) {
			DWORD err = GetLastError();
			if (err == ERROR_BROKEN_PIPE) return 1;
			return -1;
		}
		if (available == 0) return 0;

		char buf[4096];
		DWORD to_read = available > sizeof buf ? (DWORD)sizeof buf : available;
		DWORD got = 0;
		if (!ReadFile(pipe, buf, to_read, &got, NULL)) {
			DWORD err = GetLastError();
			if (err == ERROR_BROKEN_PIPE) return 1;
			return -1;
		}
		if (got == 0) return 1;
		tail_append(t, buf, (size_t)got);
	}
}

static long monotonic_ms_since(ULONGLONG t0)
{
	return (long)(monotonic_ms() - t0);
}

exec_status_t exec_install(void                *job_handle_in,
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
                           exec_result_t       *out)
{
	(void)fsize_limit_mb;

	if (!work_dir || !target_file || !out) return EXEC_ERR_INTERNAL;
	memset(out, 0, sizeof *out);
	out->exit_code = -1;

	DWORD attr = GetFileAttributesA(target_file);
	if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
		return EXEC_ERR_SCRIPT_NOT_FOUND;
	}

	if (active_proc_limit <= 0) active_proc_limit = 32;

	HANDLE job = (HANDLE)job_handle_in;
	int    owns_job = 0;
	if (!job) {
		job = create_job(mem_limit_mb, active_proc_limit);
		if (!job) return EXEC_ERR_INTERNAL;
		owns_job = 1;
	} else {

		JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
		memset(&info, 0, sizeof info);
		info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (mem_limit_mb > 0) {
			info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
			info.ProcessMemoryLimit = (SIZE_T)mem_limit_mb * 1024ULL * 1024ULL;
		}
		if (active_proc_limit > 0) {
			info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
			info.BasicLimitInformation.ActiveProcessLimit = (DWORD)active_proc_limit;
		}
		(void)SetInformationJobObject(job, JobObjectExtendedLimitInformation,
		                              &info, sizeof info);
	}

	SECURITY_ATTRIBUTES sa;
	sa.nLength              = sizeof sa;
	sa.bInheritHandle       = TRUE;
	sa.lpSecurityDescriptor = NULL;

	HANDLE out_r = NULL, out_w = NULL;
	HANDLE err_r = NULL, err_w = NULL;
	HANDLE nul   = INVALID_HANDLE_VALUE;
	if (!CreatePipe(&out_r, &out_w, &sa, 0) ||
	    !CreatePipe(&err_r, &err_w, &sa, 0)) {
		if (out_r) CloseHandle(out_r);
		if (out_w) CloseHandle(out_w);
		if (err_r) CloseHandle(err_r);
		if (err_w) CloseHandle(err_w);
		if (owns_job) CloseHandle(job);
		return EXEC_ERR_INTERNAL;
	}

	SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

	nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &sa,
	                  OPEN_EXISTING, 0, NULL);
	if (nul == INVALID_HANDLE_VALUE) {
		CloseHandle(out_r); CloseHandle(out_w);
		CloseHandle(err_r); CloseHandle(err_w);
		if (owns_job) CloseHandle(job);
		return EXEC_ERR_INTERNAL;
	}

	char cmdline[4096];
	if (build_cmdline(type, target_file, argv_extra, cmdline, sizeof cmdline) != 0) {
		CloseHandle(nul);
		CloseHandle(out_r); CloseHandle(out_w);
		CloseHandle(err_r); CloseHandle(err_w);
		if (owns_job) CloseHandle(job);
		return EXEC_ERR_INTERNAL;
	}
	char *env_block = build_env_block(task_id, machine_id);
	if (!env_block) {
		CloseHandle(nul);
		CloseHandle(out_r); CloseHandle(out_w);
		CloseHandle(err_r); CloseHandle(err_w);
		if (owns_job) CloseHandle(job);
		return EXEC_ERR_INTERNAL;
	}

	STARTUPINFOA si;
	memset(&si, 0, sizeof si);
	si.cb         = sizeof si;
	si.dwFlags    = STARTF_USESTDHANDLES;
	si.hStdInput  = nul;
	si.hStdOutput = out_w;
	si.hStdError  = err_w;

	PROCESS_INFORMATION pi;
	memset(&pi, 0, sizeof pi);

	BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL,
	                         TRUE,
	                         CREATE_SUSPENDED | CREATE_NO_WINDOW,
	                         env_block, work_dir, &si, &pi);

	free(env_block);

	CloseHandle(out_w);
	CloseHandle(err_w);
	CloseHandle(nul);

	if (!ok) {
		CloseHandle(out_r);
		CloseHandle(err_r);
		if (owns_job) CloseHandle(job);
		return EXEC_ERR_INTERNAL;
	}

	if (!AssignProcessToJobObject(job, pi.hProcess)) {
		TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		CloseHandle(out_r);
		CloseHandle(err_r);
		if (owns_job) CloseHandle(job);
		return EXEC_ERR_INTERNAL;
	}
	ResumeThread(pi.hThread);

	ULONGLONG t0 = monotonic_ms();
	char out_storage[4096];
	char err_storage[4096];
	tail_buf_t out_t = { .buf = out_storage, .cap = sizeof out_storage, .len = 0 };
	tail_buf_t err_t = { .buf = err_storage, .cap = sizeof err_storage, .len = 0 };

	long term_at_ms = (timeout_sec > 0) ? (long)timeout_sec * 1000L : -1L;
	int  term_sent = 0;
	int  kill_sent = 0;
	int  out_open  = 1, err_open = 1;

	for (;;) {

		if (out_open) {
			int r = drain_once(out_r, &out_t);
			if (r != 0) { CloseHandle(out_r); out_r = NULL; out_open = 0; }
		}
		if (err_open) {
			int r = drain_once(err_r, &err_t);
			if (r != 0) { CloseHandle(err_r); err_r = NULL; err_open = 0; }
		}

		long elapsed = monotonic_ms_since(t0);
		if (term_at_ms >= 0 && !term_sent && elapsed >= term_at_ms) {
			TerminateJobObject(job, 1);
			term_sent = 1;
		}
		if (term_sent && !kill_sent && elapsed >= term_at_ms + 5000L) {

			TerminateProcess(pi.hProcess, 1);
			kill_sent = 1;
		}

		DWORD wr = WaitForSingleObject(pi.hProcess, 200);
		if (wr == WAIT_OBJECT_0) {

			if (out_open) { drain_once(out_r, &out_t); CloseHandle(out_r); out_r = NULL; out_open = 0; }
			if (err_open) { drain_once(err_r, &err_t); CloseHandle(err_r); err_r = NULL; err_open = 0; }

			DWORD exit_code = 0;
			GetExitCodeProcess(pi.hProcess, &exit_code);
			out->exit_code   = (int)exit_code;
			out->duration_ms = monotonic_ms_since(t0);
			tail_finalize(&out_t, out->stdout_tail, sizeof out->stdout_tail);
			tail_finalize(&err_t, out->stderr_tail, sizeof out->stderr_tail);

			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
			if (owns_job) CloseHandle(job);

			if (type == EXEC_INSTALL_TYPE_MSI) {
				if (exit_code == 0 || exit_code == 3010) return EXEC_OK;
				return term_sent ? EXEC_ERR_SCRIPT_TIMEOUT : EXEC_ERR_SCRIPT_FAILED;
			}

			if (exit_code == 0) return EXEC_OK;
			return term_sent ? EXEC_ERR_SCRIPT_TIMEOUT : EXEC_ERR_SCRIPT_FAILED;
		}

	}
}
