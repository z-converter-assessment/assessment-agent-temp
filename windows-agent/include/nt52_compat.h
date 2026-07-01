#ifndef NT52_COMPAT_H
#define NT52_COMPAT_H
/*
 * NT 5.2 (Windows Server 2003 / XP x64) secure-CRT compat.
 *
 * The "secure" CRT functions strncpy_s / strtok_s were added to the SYSTEM
 * msvcrt.dll only in the Vista/Win7 era. Windows Server 2003's msvcrt.dll does
 * NOT export them, so the legacy (PROFILE=legacy / legacy32, _WIN32_WINNT=0x0502)
 * build fails to LOAD on 2003 with "entry point strncpy_s/strtok_s not found in
 * msvcrt.dll" — the process never reaches main(). (modern/win7 builds resolve
 * these from a newer msvcrt and are unaffected.)
 *
 * Provide drop-in replacements for the NT 5.2 target only. Include this header
 * AFTER <string.h> in any TU that calls strncpy_s/strtok_s.
 */
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600

#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif

/* _putenv_s(name,value) — secure CRT, NT5.2 msvcrt 부재. 구형 _putenv(2003 존재)로
 * 대체. Windows _putenv 는 문자열을 복사하므로 임시 버퍼 free 안전. */
static inline int nt52_putenv_s(const char *name, const char *value)
{
	if (!name) return 22;
	size_t n = strlen(name) + 1 + (value ? strlen(value) : 0) + 1;
	char *buf = (char *)malloc(n);
	if (!buf) return 12; /* ENOMEM */
	snprintf(buf, n, "%s=%s", name, value ? value : "");
	int rc = _putenv(buf);
	free(buf);
	return rc == 0 ? 0 : 22;
}

/* MSVC strncpy_s(dst, dstsz, src, count): copy up to count chars, always
 * NUL-terminate within dstsz. count==_TRUNCATE => fit as much as possible. */
static inline int nt52_strncpy_s(char *dst, size_t dstsz,
                                 const char *src, size_t count)
{
	if (!dst || dstsz == 0) return 22; /* EINVAL */
	if (!src) { dst[0] = '\0'; return 22; }
	size_t limit = (count == _TRUNCATE || count >= dstsz) ? dstsz - 1 : count;
	size_t i = 0;
	for (; i < limit && src[i]; i++) dst[i] = src[i];
	dst[i] = '\0';
	return 0;
}

/* MSVC strtok_s(str, delim, ctx) == POSIX strtok_r. */
static inline char *nt52_strtok_s(char *str, const char *delim, char **ctx)
{
	char *s = str ? str : (ctx ? *ctx : NULL);
	if (!s) return NULL;
	s += strspn(s, delim);
	if (!*s) { if (ctx) *ctx = s; return NULL; }
	char *end = s + strcspn(s, delim);
	if (*end) { *end = '\0'; if (ctx) *ctx = end + 1; }
	else      { if (ctx) *ctx = end; }
	return s;
}

#define strncpy_s nt52_strncpy_s
#define strtok_s  nt52_strtok_s
#define _putenv_s nt52_putenv_s

#endif /* NT 5.2 target */
#endif /* NT52_COMPAT_H */
