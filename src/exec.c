/**
 * @file exec.c
 * @brief fork + execve of install.sh under EC1 environment.
 *
 * Parent side: opens two pipes (stdout, stderr), forks, then drains both
 * pipes via poll()-style select loop while watching the child PID. On
 * timeout, sends SIGTERM to the child's process group (set via setsid in
 * the child); +5s after that, SIGKILL. Pipe drain continues until both
 * ends close so the result tails capture any output flushed during
 * shutdown.
 *
 * Child side: clearenv → whitelist rebuild → setsid → chdir → stdio
 * redirection → setrlimit → execve. All steps before execve are wrapped
 * in `if (...) _exit(EXEC_CHILD_BOOTSTRAP_FAIL)` so a parent waitpid sees
 * a distinguishable exit code on bootstrap failure.
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "exec.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/*
 * Distinguishable from POSIX shell conventions:
 *   126 = "command found but not executable"
 *   127 = "command not found"
 *   128+N = "killed by signal N"
 * Picking 124 leaves all of those alone so a parent waitpid can tell
 * "child failed to set up its environment" from "the install script
 * itself died with a similar status".
 */
#define EXEC_CHILD_BOOTSTRAP_FAIL 124

/* ============================================================
 * Tail-buffer helpers — keep only the last N bytes
 * ============================================================ */

typedef struct {
	char  *buf;
	size_t cap;
	size_t len;      /* total bytes ever appended */
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
	/* Sanitize: replace bytes outside ASCII printable + \t\n with '?'. */
	for (size_t i = 0; i < kept; i++) {
		unsigned char c = (unsigned char)out[i];
		if (c == '\t' || c == '\n') continue;
		if (c < 0x20 || c > 0x7e) out[i] = '?';
	}
	out[kept] = '\0';
}

/* ============================================================
 * Child-side bootstrap
 * ============================================================ */

/*
 * Round 3: parameter is overloaded by resource type:
 *   RLIMIT_CPU      → seconds (raw value)
 *   RLIMIT_NOFILE   → fd count (raw value)
 *   RLIMIT_AS / RLIMIT_FSIZE → MB (multiplied to bytes)
 *
 * Previously RLIMIT_NOFILE was treated as MB and multiplied by 1024*1024,
 * setting fd table to 1 GB entries — kernel clamps to a hard limit but
 * the requested value was nonsense.
 */
static int set_rlimit(int resource, long value)
{
	if (value <= 0) return 0;
	struct rlimit rl;
	if (resource == RLIMIT_CPU || resource == RLIMIT_NOFILE) {
		rl.rlim_cur = (rlim_t)value;
		rl.rlim_max = (rlim_t)value;
	} else {
		rl.rlim_cur = (rlim_t)value * 1024ULL * 1024ULL;
		rl.rlim_max = rl.rlim_cur;
	}
	return setrlimit(resource, &rl);
}

static int child_bootstrap(const char *extract_dir,
                           const char *task_id, const char *machine_id,
                           int stdin_fd, int stdout_fd, int stderr_fd,
                           int timeout_sec, int mem_mb, int fsize_mb,
                           int nofile)
{
	/*
	 * Round 3 C-1: do NOT call setsid() here. The grandchild must stay
	 * in the worker child's process group so that the agent's drain
	 * escalation `kill(-worker_child_pid, SIGTERM)` reaches install.sh.
	 * Previously setsid() created a new session; -worker_child_pid then
	 * targeted only the worker child, leaving install.sh as a long-running
	 * orphan reparented to init.
	 */
	if (chdir(extract_dir) != 0)            return -1;

	if (dup2(stdin_fd, STDIN_FILENO)   < 0) return -1;
	if (dup2(stdout_fd, STDOUT_FILENO) < 0) return -1;
	if (dup2(stderr_fd, STDERR_FILENO) < 0) return -1;
	if (stdin_fd  > STDERR_FILENO) close(stdin_fd);
	if (stdout_fd > STDERR_FILENO) close(stdout_fd);
	if (stderr_fd > STDERR_FILENO) close(stderr_fd);

	close_inherited_fds();

	umask(022);

	/*
	 * Round 10: reset SIGPIPE to SIG_DFL before execve. POSIX guarantees
	 * SIG_IGN dispositions survive execve, so without this reset install.sh
	 * inherits the agent's SIGPIPE-ignore (round-9 fix for broker reset
	 * crashes). install scripts relying on classic pipeline semantics
	 * (`yes | foo`, `tar | head`) would then hang because SIGPIPE never
	 * fires when the pipe peer closes. Restoring SIG_DFL here applies
	 * only to the install.sh process tree.
	 */
	signal(SIGPIPE, SIG_DFL);

	if (clearenv() != 0) return -1;
	setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
	setenv("LANG", "C.UTF-8", 1);
	setenv("HOME", "/tmp", 1);
	setenv("USER", "agent", 1);
	if (task_id)    setenv("TASK_ID",    task_id, 1);
	if (machine_id) setenv("MACHINE_ID", machine_id, 1);

	if (set_rlimit(RLIMIT_NOFILE, (long)nofile)     != 0) return -1;
	if (set_rlimit(RLIMIT_AS,     mem_mb)           != 0) return -1;
	if (set_rlimit(RLIMIT_FSIZE,  fsize_mb)         != 0) return -1;
	if (timeout_sec > 0 &&
	    set_rlimit(RLIMIT_CPU,    (long)timeout_sec) != 0) return -1;

	return 0;
}

/* ============================================================
 * Parent-side drain + timeout enforcement
 * ============================================================ */

static long elapsed_ms(struct timespec start)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long ms = (now.tv_sec - start.tv_sec) * 1000L
	        + (now.tv_nsec - start.tv_nsec) / 1000000L;
	return ms < 0 ? 0 : ms;
}

static int set_nonblocking(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl < 0) return -1;
	return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/*
 * Returns 1 if peer closed the pipe (EOF), 0 if more data may arrive,
 * -1 on a real read error. Distinguishing EOF from EAGAIN lets the caller
 * retire the fd from the select set instead of spinning at 200ms ticks
 * on an already-closed pipe (CRITICAL #12).
 */
static int drain_once(int fd, tail_buf_t *t)
{
	char buf[4096];
	for (;;) {
		ssize_t n = read(fd, buf, sizeof buf);
		if (n > 0)            { tail_append(t, buf, (size_t)n); continue; }
		if (n == 0)           return 1;             /* EOF */
		if (errno == EINTR)   continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
		return -1;
	}
}

/* ============================================================
 * Public — exec_install_script
 * ============================================================ */

exec_status_t exec_install_script(const char  *extract_dir,
                                  const char  *script,
                                  const char **argv_extra,
                                  int          timeout_sec,
                                  int          mem_limit_mb,
                                  int          fsize_limit_mb,
                                  int          nofile_limit,
                                  const char  *task_id,
                                  const char  *machine_id,
                                  exec_result_t *out)
{
	if (!extract_dir || !script || !out) return EXEC_ERR_INTERNAL;

	memset(out, 0, sizeof *out);
	out->exit_code = -1;

	/* Pre-flight: script exists and is regular. */
	{
		char full[4096];
		snprintf(full, sizeof full, "%s/%s", extract_dir, script);
		struct stat st;
		if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
			return EXEC_ERR_SCRIPT_NOT_FOUND;
	}

	int devnull = open("/dev/null", O_RDONLY);
	if (devnull < 0) return EXEC_ERR_INTERNAL;

	int out_pipe[2] = { -1, -1 };
	int err_pipe[2] = { -1, -1 };
	if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
		if (out_pipe[0] >= 0) close(out_pipe[0]);
		if (out_pipe[1] >= 0) close(out_pipe[1]);
		if (err_pipe[0] >= 0) close(err_pipe[0]);
		if (err_pipe[1] >= 0) close(err_pipe[1]);
		close(devnull);
		return EXEC_ERR_INTERNAL;
	}

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	pid_t pid = fork();
	if (pid < 0) {
		close(out_pipe[0]); close(out_pipe[1]);
		close(err_pipe[0]); close(err_pipe[1]);
		close(devnull);
		return EXEC_ERR_INTERNAL;
	}

	if (pid == 0) {
		/* CHILD */
		close(out_pipe[0]); close(err_pipe[0]);
		if (child_bootstrap(extract_dir, task_id, machine_id,
		                    devnull, out_pipe[1], err_pipe[1],
		                    timeout_sec, mem_limit_mb, fsize_limit_mb,
		                    nofile_limit > 0 ? nofile_limit : 4096) != 0)
			_exit(EXEC_CHILD_BOOTSTRAP_FAIL);

		/* Build argv: "./script" [+ argv_extra...]. */
		size_t extra_count = 0;
		if (argv_extra) while (argv_extra[extra_count]) extra_count++;

		char **argv = (char **)malloc(sizeof(char *) * (2 + extra_count + 1));
		if (!argv) _exit(EXEC_CHILD_BOOTSTRAP_FAIL);

		char abs_script[4096];
		snprintf(abs_script, sizeof abs_script, "./%s", script);
		argv[0] = abs_script;
		for (size_t i = 0; i < extra_count; i++) argv[i + 1] = (char *)argv_extra[i];
		argv[1 + extra_count] = NULL;

		execve(abs_script, argv, environ);
		_exit(EXEC_CHILD_BOOTSTRAP_FAIL);
	}

	/* PARENT */
	close(out_pipe[1]); close(err_pipe[1]); close(devnull);
	set_nonblocking(out_pipe[0]);
	set_nonblocking(err_pipe[0]);

	char out_storage[4096];
	char err_storage[4096];
	tail_buf_t out_t = { .buf = out_storage, .cap = sizeof out_storage, .len = 0 };
	tail_buf_t err_t = { .buf = err_storage, .cap = sizeof err_storage, .len = 0 };

	int term_sent  = 0;
	int kill_sent  = 0;
	long term_at_ms = (timeout_sec > 0) ? (long)timeout_sec * 1000L : -1L;

	for (;;) {
		fd_set rfds;
		FD_ZERO(&rfds);
		int maxfd = -1;
		if (out_pipe[0] >= 0) { FD_SET(out_pipe[0], &rfds); if (out_pipe[0] > maxfd) maxfd = out_pipe[0]; }
		if (err_pipe[0] >= 0) { FD_SET(err_pipe[0], &rfds); if (err_pipe[0] > maxfd) maxfd = err_pipe[0]; }

		struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };  /* 200ms */
		int sr = (maxfd >= 0) ? select(maxfd + 1, &rfds, NULL, NULL, &tv) : 0;
		if (sr < 0 && errno != EINTR) break;

		/*
		 * After EOF, retire the fd from the select set (CRITICAL #12) by
		 * closing it and setting it to -1 — otherwise select() would
		 * report it ready every iteration and we'd burn CPU at 5Hz.
		 */
		if (out_pipe[0] >= 0 && (sr < 0 || FD_ISSET(out_pipe[0], &rfds))) {
			int r = drain_once(out_pipe[0], &out_t);
			if (r != 0) { close(out_pipe[0]); out_pipe[0] = -1; }
		}
		if (err_pipe[0] >= 0 && (sr < 0 || FD_ISSET(err_pipe[0], &rfds))) {
			int r = drain_once(err_pipe[0], &err_t);
			if (r != 0) { close(err_pipe[0]); err_pipe[0] = -1; }
		}

		long elapsed = elapsed_ms(t0);
		if (term_at_ms >= 0 && !term_sent && elapsed >= term_at_ms) {
			/*
			 * Round 3 C-1: grandchild no longer setsid()s, so it shares
			 * our pgid. `kill(-pid)` would target a non-existent group.
			 * Use single-process kill — install.sh's own forked helpers
			 * (rare) inherit our group and will be cleaned up when the
			 * agent's drain escalation targets the worker_child pgid.
			 */
			kill(pid, SIGTERM);
			term_sent = 1;
		}
		if (term_sent && !kill_sent && elapsed >= term_at_ms + 5000L) {
			kill(pid, SIGKILL);
			kill_sent = 1;
		}

		int status = 0;
		pid_t rc = waitpid(pid, &status, WNOHANG);
		if (rc == pid) {
			/* final drain — pipes may still hold buffered output. */
			if (out_pipe[0] >= 0) { drain_once(out_pipe[0], &out_t); close(out_pipe[0]); out_pipe[0] = -1; }
			if (err_pipe[0] >= 0) { drain_once(err_pipe[0], &err_t); close(err_pipe[0]); err_pipe[0] = -1; }

			out->duration_ms = elapsed_ms(t0);
			tail_finalize(&out_t, out->stdout_tail, sizeof out->stdout_tail);
			tail_finalize(&err_t, out->stderr_tail, sizeof out->stderr_tail);

			if (WIFEXITED(status)) {
				out->exit_code = WEXITSTATUS(status);
				if (out->exit_code == 0)                              return EXEC_OK;
				if (out->exit_code == EXEC_CHILD_BOOTSTRAP_FAIL)      return EXEC_ERR_INTERNAL;
				return EXEC_ERR_SCRIPT_FAILED;
			}
			if (WIFSIGNALED(status)) {
				out->signal_no = WTERMSIG(status);
				if (term_sent) return EXEC_ERR_SCRIPT_TIMEOUT;
				return EXEC_ERR_SCRIPT_FAILED;
			}
			return EXEC_ERR_INTERNAL;
		}
		if (rc < 0 && errno != EINTR) break;
	}

	/* Unreachable in well-behaved cases — fall-through. */
	if (out_pipe[0] >= 0) { close(out_pipe[0]); out_pipe[0] = -1; }
	if (err_pipe[0] >= 0) { close(err_pipe[0]); err_pipe[0] = -1; }
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	out->duration_ms = elapsed_ms(t0);
	tail_finalize(&out_t, out->stdout_tail, sizeof out->stdout_tail);
	tail_finalize(&err_t, out->stderr_tail, sizeof out->stderr_tail);
	return EXEC_ERR_INTERNAL;
}
