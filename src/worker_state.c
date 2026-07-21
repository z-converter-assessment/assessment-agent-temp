#define _POSIX_C_SOURCE 200809L

#include "worker_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* worker state-dir 의 파일시스템 plumbing — 원자적 쓰기(temp+fsync+rename),
 * 재귀 생성/삭제, task_id 검증. worker.c(오케스트레이션)와 분리. */

int mkdir_p(const char *path, mode_t mode)
{
	if (!path || !*path) return -1;
	char tmp[1024];
	size_t n = strnlen(path, sizeof tmp);
	if (n >= sizeof tmp) return -1;
	memcpy(tmp, path, n + 1);

	for (size_t i = 1; i < n; i++) {
		if (tmp[i] == '/') {
			tmp[i] = '\0';
			if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
			tmp[i] = '/';
		}
	}
	if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
	return 0;
}

int file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int fsync_parent_dir(const char *path)
{
	char dir[1024];
	size_t n = strnlen(path, sizeof dir);
	if (n >= sizeof dir) return -1;
	memcpy(dir, path, n + 1);
	char *slash = strrchr(dir, '/');
	if (!slash) { dir[0] = '.'; dir[1] = '\0'; }
	else if (slash == dir) { dir[1] = '\0'; }
	else *slash = '\0';

	int dfd = open(dir, O_RDONLY | O_DIRECTORY);
	if (dfd < 0) return -1;
	int rc = fsync(dfd);
	close(dfd);
	return rc;
}

int write_file_atomic(const char *path, const char *content)
{
	char tmp[1024];
	if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return -1;
	FILE *f = fopen(tmp, "wb");
	if (!f) return -1;
	size_t len = strlen(content);
	if (fwrite(content, 1, len, f) != len) { fclose(f); unlink(tmp); return -1; }
	if (fflush(f) != 0)                    { fclose(f); unlink(tmp); return -1; }
	int fd = fileno(f);
	if (fd >= 0 && fsync(fd) != 0)         { fclose(f); unlink(tmp); return -1; }
	if (fclose(f) != 0) { unlink(tmp); return -1; }
	if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
	(void)fsync_parent_dir(path);
	return 0;
}

int rmrf(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode)) return unlink(path);

	DIR *d = opendir(path);
	if (!d) return -1;
	struct dirent *e;
	int rc = 0;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
		char sub[1024];
		if ((size_t)snprintf(sub, sizeof sub, "%s/%s", path, e->d_name) >= sizeof sub) {
			rc = -1; continue;
		}
		if (rmrf(sub) != 0) rc = -1;
	}
	closedir(d);
	if (rmdir(path) != 0) rc = -1;
	return rc;
}

int task_id_valid(const char *id)
{
	if (!id) return 0;
	size_t n = strlen(id);
	if (n != 36 && n != 32) return 0;
	for (size_t i = 0; i < n; i++) {
		char c = id[i];
		if (n == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) {
			if (c != '-') return 0;
		} else if (!((c >= '0' && c <= '9') ||
		             (c >= 'a' && c <= 'f') ||
		             (c >= 'A' && c <= 'F'))) {
			return 0;
		}
	}
	return 1;
}
