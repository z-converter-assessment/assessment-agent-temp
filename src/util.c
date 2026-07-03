#define _GNU_SOURCE            /* syscall() prototype for close_inherited_fds */
#define _POSIX_C_SOURCE 200809L

#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

char *run_cmd(const char *cmd)
{
	FILE *fp = popen(cmd, "r");
	if (!fp)
		return NULL;

	size_t cap = 4096;
	size_t len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		pclose(fp);
		return NULL;
	}

	char chunk[4096];
	size_t n;
	while ((n = fread(chunk, 1, sizeof chunk, fp)) > 0) {
		if (len + n + 1 > cap) {
			while (len + n + 1 > cap)
				cap *= 2;
			char *nb = realloc(buf, cap);
			if (!nb) {
				free(buf);
				pclose(fp);
				return NULL;
			}
			buf = nb;
		}
		memcpy(buf + len, chunk, n);
		len += n;
	}
	buf[len] = '\0';

	int status = pclose(fp);
	if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		free(buf);
		return NULL;
	}
	return buf;
}

char *read_file_all(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;

	size_t cap = 4096;
	size_t len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		close(fd);
		return NULL;
	}

	for (;;) {
		if (len + 1 >= cap) {
			cap *= 2;
			char *nb = realloc(buf, cap);
			if (!nb) {
				free(buf);
				close(fd);
				return NULL;
			}
			buf = nb;
		}
		ssize_t n = read(fd, buf + len, cap - len - 1);
		if (n < 0) {
			free(buf);
			close(fd);
			return NULL;
		}
		if (n == 0)
			break;
		len += (size_t)n;
	}
	buf[len] = '\0';
	close(fd);
	return buf;
}

const char *getenv_default(const char *name, const char *fallback)
{
	const char *v = getenv(name);
	return (v && *v) ? v : fallback;
}

int parse_bool(const char *s, int fallback)
{
	if (!s || !*s) return fallback;
	if (!strcasecmp(s, "1")    || !strcasecmp(s, "true") ||
	    !strcasecmp(s, "yes")  || !strcasecmp(s, "on")   ||
	    !strcasecmp(s, "y")    || !strcasecmp(s, "t"))
		return 1;
	if (!strcasecmp(s, "0")    || !strcasecmp(s, "false") ||
	    !strcasecmp(s, "no")   || !strcasecmp(s, "off")   ||
	    !strcasecmp(s, "n")    || !strcasecmp(s, "f"))
		return 0;

	return -1;
}

int getenv_bool(const char *name, int fallback)
{
	const char *v = getenv(name);
	if (!v || !*v) return fallback;
	int parsed = parse_bool(v, -1);
	if (parsed < 0) {
		fprintf(stderr, "[agent] WARN: env %s=\"%s\" not a recognized boolean (use true/false/1/0/yes/no/on/off); using default %d\n",
		        name, v, fallback);
		return fallback;
	}
	return parsed;
}

int getenv_int_or(const char *name, int fallback)
{
	const char *v = getenv(name);
	if (!v || !*v) return fallback;
	char *end = NULL;
	errno = 0;
	long n = strtol(v, &end, 10);
	if (errno != 0 || end == v || *end != '\0') {
		fprintf(stderr, "[agent] WARN: env %s=\"%s\" not a valid integer; using default %d\n",
		        name, v, fallback);
		return fallback;
	}
	if (n < INT_MIN || n > INT_MAX) {
		fprintf(stderr, "[agent] WARN: env %s=\"%s\" out of int range; using default %d\n",
		        name, v, fallback);
		return fallback;
	}
	return (int)n;
}

char *trim_inplace(char *s)
{
	if (!s)
		return s;
	char *start = s;
	while (*start && isspace((unsigned char)*start))
		start++;
	if (start != s)
		memmove(s, start, strlen(start) + 1);
	size_t l = strlen(s);
	while (l > 0 && isspace((unsigned char)s[l - 1]))
		s[--l] = '\0';
	return s;
}

void load_env_file(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return;

	char line[1024];
	while (fgets(line, sizeof line, f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0' || *p == '\n' || *p == '#')
			continue;

		char *eq = strchr(p, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *key = p;
		char *val = eq + 1;

		char *key_end = eq - 1;
		while (key_end >= key && (*key_end == ' ' || *key_end == '\t')) {
			*key_end = '\0';
			key_end--;
		}
		if (*key == '\0')
			continue;

		size_t vl = strlen(val);
		while (vl > 0 &&
		       (val[vl - 1] == '\n' || val[vl - 1] == '\r' ||
		        val[vl - 1] == ' ' || val[vl - 1] == '\t')) {
			val[--vl] = '\0';
		}

		if (vl >= 2 && ((val[0] == '"' && val[vl - 1] == '"') ||
		                (val[0] == '\'' && val[vl - 1] == '\''))) {
			val[vl - 1] = '\0';
			val++;
		}

		setenv(key, val, 0);
	}
	fclose(f);
}

char *iso8601_utc(time_t t, char *buf, size_t len)
{
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
	return buf;
}

char *iso8601_utc_ms(struct timespec ts, char *buf, size_t len)
{
	struct tm tm;
	gmtime_r(&ts.tv_sec, &tm);
	char base[32];
	strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &tm);
	long ms = ts.tv_nsec / 1000000L;
	snprintf(buf, len, "%s.%03ldZ", base, ms);
	return buf;
}

char *uuid_v4(char *buf, size_t len)
{
	FILE *f = fopen("/proc/sys/kernel/random/uuid", "r");
	if (f) {
		if (fgets(buf, (int)len, f)) {
			size_t l = strlen(buf);
			if (l > 0 && buf[l - 1] == '\n')
				buf[l - 1] = '\0';
			fclose(f);
			return buf;
		}
		fclose(f);
	}

	char hostname[64];
	if (gethostname(hostname, sizeof hostname) != 0)
		snprintf(hostname, sizeof hostname, "unknown");
	hostname[sizeof hostname - 1] = '\0';   /* gethostname truncation may not NUL-terminate */
	struct timeval tv;
	gettimeofday(&tv, NULL);
	snprintf(buf, len, "%s-%d-%ld%06ld",
	         hostname, (int)getpid(),
	         (long)tv.tv_sec, (long)tv.tv_usec);
	return buf;
}

int jitter_seconds(int base_sec, double frac)
{
	if (base_sec <= 0) return base_sec;
	if (frac < 0)      frac = 0;
	if (frac >= 1.0)   frac = 0.999;

	double u = ((double)rand() / (double)RAND_MAX) * (2.0 * frac) - frac;
	double v = (double)base_sec * (1.0 + u);
	return (int)v;
}

#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/syscall.h>

static int sweep_via_proc_self_fd(void)
{
	DIR *d = opendir("/proc/self/fd");
	if (!d) return -1;
	int dirfd_to_skip = dirfd(d);
	int saw_any = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
		int fd = atoi(e->d_name);
		if (fd <= STDERR_FILENO || fd == dirfd_to_skip) continue;
		(void)close(fd);
		saw_any = 1;
	}
	closedir(d);

	return saw_any ? 0 : -1;
}

static void sweep_numeric(void)
{
	struct rlimit rl;
	long max_fd = 1024;
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 &&
	    rl.rlim_cur != RLIM_INFINITY) {
		max_fd = (long)rl.rlim_cur;
	}
	if (max_fd > 4096) max_fd = 4096;
	for (long fd = STDERR_FILENO + 1; fd < max_fd; fd++)
		(void)close((int)fd);
}

void close_inherited_fds(void)
{
#if defined(__linux__) && defined(SYS_close_range)
	if (syscall(SYS_close_range, (unsigned int)3, ~0u, 0u) == 0) return;
#endif
	if (sweep_via_proc_self_fd() == 0) return;
	sweep_numeric();
}
