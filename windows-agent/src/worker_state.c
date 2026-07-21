#define WIN32_LEAN_AND_MEAN

#include "worker_internal.h"

#include <stdio.h>
#include <string.h>

#include <windows.h>
#include "nt52_compat.h"

/* worker state-dir 파일시스템 plumbing — 원자적 쓰기(temp+MoveFileEx)/재귀 생성·삭제/
 * task_id 검증. reparse point(junction)는 leaf 취급(타겟 미하강). worker.c 와 분리. */

int ensure_dir(const char *path)
{
	if (!path || !*path) return -1;
	DWORD attr = GetFileAttributesA(path);
	if (attr != INVALID_FILE_ATTRIBUTES) {
		return (attr & FILE_ATTRIBUTE_DIRECTORY) ? 0 : -1;
	}

	char tmp[1024];
	size_t n = strlen(path);
	if (n >= sizeof tmp) return -1;
	memcpy(tmp, path, n + 1);
	for (size_t i = 1; i < n; i++) {
		if (tmp[i] == '\\' || tmp[i] == '/') {
			char saved = tmp[i];
			tmp[i] = '\0';
			DWORD a = GetFileAttributesA(tmp);
			if (a == INVALID_FILE_ATTRIBUTES) {
				if (!CreateDirectoryA(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
					return -1;
			}
			tmp[i] = saved;
		}
	}
	if (!CreateDirectoryA(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
		return -1;
	return 0;
}

int file_exists(const char *path)
{
	DWORD a = GetFileAttributesA(path);
	return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int write_file_atomic(const char *path, const char *content)
{
	char tmp[1024];
	if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return -1;
	FILE *f = fopen(tmp, "wb");
	if (!f) return -1;
	size_t len = strlen(content);
	if (fwrite(content, 1, len, f) != len) { fclose(f); DeleteFileA(tmp); return -1; }
	if (fflush(f) != 0)                    { fclose(f); DeleteFileA(tmp); return -1; }
	if (fclose(f) != 0) { DeleteFileA(tmp); return -1; }

	if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
		DeleteFileA(tmp);
		return -1;
	}
	return 0;
}

int rmrf_recursive(const char *path)
{
	DWORD attr = GetFileAttributesA(path);
	if (attr == INVALID_FILE_ATTRIBUTES) return 0;
	/* reparse point(junction/심볼릭)는 leaf 로 취급 — 타겟으로 내려가지 않고 링크 자체만 제거한다.
	   junction 은 DIRECTORY+REPARSE 비트를 함께 가지므로 REPARSE 를 먼저 본다(Linux lstat+unlink 대칭,
	   미검사 시 SYSTEM 권한으로 junction 타겟의 실파일을 재귀 삭제하게 됨). */
	if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
		if (attr & FILE_ATTRIBUTE_DIRECTORY)
			return RemoveDirectoryA(path) ? 0 : -1;
		return DeleteFileA(path) ? 0 : -1;
	}
	if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
		return DeleteFileA(path) ? 0 : -1;
	}
	char pattern[1024];
	if ((size_t)snprintf(pattern, sizeof pattern, "%s\\*", path) >= sizeof pattern) return -1;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	int rc = 0;
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
			char sub[1024];
			if ((size_t)snprintf(sub, sizeof sub, "%s\\%s", path, fd.cFileName) >= sizeof sub) { rc = -1; continue; }
			if (rmrf_recursive(sub) != 0) rc = -1;
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	if (!RemoveDirectoryA(path)) rc = -1;
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
