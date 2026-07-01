#ifndef NT52_COMPAT_H
#define NT52_COMPAT_H

#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600

#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif

static inline int nt52_putenv_s(const char *name, const char *value)
{
	if (!name) return 22;
	size_t n = strlen(name) + 1 + (value ? strlen(value) : 0) + 1;
	char *buf = (char *)malloc(n);
	if (!buf) return 12;
	snprintf(buf, n, "%s=%s", name, value ? value : "");
	int rc = _putenv(buf);
	free(buf);
	return rc == 0 ? 0 : 22;
}

static inline int nt52_strncpy_s(char *dst, size_t dstsz,
                                 const char *src, size_t count)
{
	if (!dst || dstsz == 0) return 22;
	if (!src) { dst[0] = '\0'; return 22; }
	size_t limit = (count == _TRUNCATE || count >= dstsz) ? dstsz - 1 : count;
	size_t i = 0;
	for (; i < limit && src[i]; i++) dst[i] = src[i];
	dst[i] = '\0';
	return 0;
}

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

#endif
#endif
