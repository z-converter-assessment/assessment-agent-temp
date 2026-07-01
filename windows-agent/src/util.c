/**
 * @file util.c
 * @brief util.h implementation (Windows). No AMQP / cJSON dependencies.
 *
 * POSIX 헬퍼 (popen, gmtime_r, /proc/sys/kernel/random/uuid 등) 를 Win32 등가
 * (_popen, gmtime_s, CoCreateGuid 등) 로 대체. 페이로드 계약은 Linux와 동일.
 */

#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <windows.h>
#include <objbase.h>   /* CoCreateGuid */
#include <wincrypt.h>  /* CryptAcquireContextW / CryptGenRandom (NT 5.2 fallback) */
#include "nt52_compat.h" /* _putenv_s — NT5.2 msvcrt 부재 대비(legacy 빌드) */

/* ---------- compat: CSPRNG (bcrypt → CryptoAPI fallback) ----------
 *
 * Modern path: BCryptGenRandom (bcrypt.dll, Vista / Server 2008+).
 * Legacy path: CryptGenRandom (advapi32, every NT incl. Server 2003).
 *
 * bcrypt is resolved dynamically so it is NOT a static import — Server 2003
 * has no bcrypt.dll and a static -lbcrypt would make the binary fail to load.
 */
int compat_rand_bytes(unsigned char *buf, size_t len)
{
	if (!buf || len == 0)
		return 0;

	/* BCryptGenRandom(NULL, buf, len, BCRYPT_USE_SYSTEM_PREFERRED_RNG=0x02).
	 * NTSTATUS return; 0 (STATUS_SUCCESS) == ok. Signature mirrored locally
	 * so we don't need bcrypt.h at compile time. */
	typedef LONG (WINAPI *BCryptGenRandom_t)(void *, unsigned char *,
	                                         unsigned long, unsigned long);
	static BCryptGenRandom_t s_bcrypt = NULL;
	static int s_bcrypt_resolved = 0;
	if (!s_bcrypt_resolved) {
		s_bcrypt_resolved = 1;
		HMODULE h = LoadLibraryW(L"bcrypt.dll");
		if (h)
			s_bcrypt = (BCryptGenRandom_t)(void *)
			           GetProcAddress(h, "BCryptGenRandom");
		/* Intentionally leak the HMODULE: the agent uses RNG for the whole
		 * process lifetime, so keeping bcrypt.dll mapped is correct. */
	}
	if (s_bcrypt) {
		if (s_bcrypt(NULL, buf, (unsigned long)len, 0x00000002UL) == 0)
			return 1;
		/* fall through to CryptoAPI on the rare bcrypt failure */
	}

	/* Legacy CryptoAPI — present on NT 5.2. VERIFYCONTEXT = no keyset, just
	 * RNG, so it never touches the user profile / persisted keys. */
	HCRYPTPROV prov = 0;
	if (CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL,
	                         CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
		BOOL ok = CryptGenRandom(prov, (DWORD)len, buf);
		CryptReleaseContext(prov, 0);
		if (ok)
			return 1;
	}
	return 0;
}

/* ---------- compat: monotonic clock (GetTickCount64 → GetTickCount) ----------
 *
 * GetTickCount64 exists on Vista / Server 2008+ only. Resolve it once; on
 * NT 5.2 fall back to the 32-bit GetTickCount and extend to 64-bit by tracking
 * wraps. The agent's tick usage is single-threaded (parent loop + worker child
 * before any thread spawn), so the static state needs no locking.
 */
unsigned long long monotonic_ms(void)
{
	typedef ULONGLONG (WINAPI *GetTickCount64_t)(void);
	static GetTickCount64_t s_gtc64 = NULL;
	static int s_resolved = 0;
	if (!s_resolved) {
		s_resolved = 1;
		HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
		if (k32)
			s_gtc64 = (GetTickCount64_t)(void *)
			          GetProcAddress(k32, "GetTickCount64");
	}
	if (s_gtc64)
		return (unsigned long long)s_gtc64();

	/* Fallback: 32-bit GetTickCount wraps every ~49.7 days. Track the high
	 * 32 bits by detecting backward steps (current < last → a wrap). */
	static DWORD     s_last = 0;
	static ULONGLONG s_high = 0;   /* accumulated wraps << 32 */
	DWORD now = GetTickCount();
	if (now < s_last)
		s_high += 0x100000000ULL;  /* one wrap (2^32 ms) */
	s_last = now;
	return s_high + (ULONGLONG)now;
}

/* ---------- env helpers ---------- */
const char *getenv_default(const char *name, const char *fallback)
{
	const char *v = getenv(name);
	return (v && *v) ? v : fallback;
}

int parse_bool(const char *s, int fallback)
{
	if (!s || !*s) return fallback;
	if (!_stricmp(s, "1")    || !_stricmp(s, "true") ||
	    !_stricmp(s, "yes")  || !_stricmp(s, "on")   ||
	    !_stricmp(s, "y")    || !_stricmp(s, "t"))
		return 1;
	if (!_stricmp(s, "0")    || !_stricmp(s, "false") ||
	    !_stricmp(s, "no")   || !_stricmp(s, "off")   ||
	    !_stricmp(s, "n")    || !_stricmp(s, "f"))
		return 0;
	return -1;
}

int getenv_bool(const char *name, int fallback)
{
	const char *v = getenv(name);
	if (!v || !*v) return fallback;
	int parsed = parse_bool(v, -1);
	if (parsed < 0) {
		fprintf(stderr, "[agent] WARN: env %s=\"%s\" not a recognized boolean "
		                "(use true/false/1/0/yes/no/on/off); using default %d\n",
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

		/* "do not overwrite" — shell env wins (matches POSIX setenv flag 0). */
		if (getenv(key) == NULL)
			_putenv_s(key, val);
	}
	fclose(f);
}

/* ---------- time / uuid ---------- */
char *iso8601_utc(time_t t, char *buf, size_t len)
{
	struct tm tm_buf;
	gmtime_s(&tm_buf, &t);
	strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
	return buf;
}

char *uuid_v4(char *buf, size_t len)
{
	GUID g;
	if (CoCreateGuid(&g) == S_OK) {
		snprintf(buf, len,
		         "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		         (unsigned long)g.Data1, g.Data2, g.Data3,
		         g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
		         g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
		return buf;
	}
	/* Synthetic fallback (not RFC-4122 but unique enough for diagnostics). */
	char hostname[64] = "unknown";
	DWORD sz = (DWORD)sizeof hostname;
	GetComputerNameA(hostname, &sz);
	SYSTEMTIME st;
	GetSystemTime(&st);
	snprintf(buf, len, "%s-%lu-%04d%02d%02dT%02d%02d%02d.%03d",
	         hostname, (unsigned long)GetCurrentProcessId(),
	         st.wYear, st.wMonth, st.wDay,
	         st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
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

/* ---------- boot time ----------
 *
 * Linux 에이전트는 `/proc/uptime` + `CLOCK_REALTIME` 로 부팅 wall-clock 합성.
 * Windows 등가: monotonic_ms() (ms since boot, monotonic) + 현재 wall-clock.
 * 페이로드 v3.1 의 `boot_time` 은 프로세스 시작 시 1회 캐시되며, 본 함수는
 * main.c 에서 한 번 호출되어 그 값을 캐시한다.
 *
 * NT 5.2 (Server 2003) 한계 — 수용됨:
 *   GetTickCount64 가 없는 NT 5.2 에서는 monotonic_ms 가 32-bit GetTickCount
 *   폴백을 쓴다. 이 폴백은 프로세스 시작 이전에 이미 일어난 wrap 을 알 수 없으므로,
 *   uptime > 49.7일 인 호스트에서는 여기서 계산한 절대 boot_time 이 49.7일 배수만큼
 *   미래로 어긋난다. 그러나:
 *     - boot_time 은 프로세스 시작 시 1회만 캡처·캐시된다.
 *     - engine 은 boot_time 의 *절대값* 이 아니라 *변화* (prev != curr) 만으로
 *       counter-reset 을 감지한다 (CLAUDE.md "boot_time" 계약 참고).
 *   따라서 offset 오차가 있어도 reset 감지는 깨지지 않는다 (재부팅 시 uptime 이
 *   리셋 → boot_time 값이 바뀜 → engine 이 series cut 인식).
 *   Vista / 2008+ 는 GetTickCount64 를 동적 resolve 하므로 정확하다.
 */
char *get_boot_time_iso8601(char *buf, size_t len)
{
	ULONGLONG uptime_ms = monotonic_ms();

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	ULARGE_INTEGER now_ft;
	now_ft.LowPart  = ft.dwLowDateTime;
	now_ft.HighPart = ft.dwHighDateTime;

	/* FILETIME epoch = 1601-01-01 UTC. Unix epoch = 1970-01-01 UTC.
	 * Difference = 11644473600 seconds = 116444736000000000 100ns ticks. */
	ULONGLONG ft_unix_100ns = now_ft.QuadPart - 116444736000000000ULL;
	time_t now_sec = (time_t)(ft_unix_100ns / 10000000ULL);
	time_t boot_sec = now_sec - (time_t)(uptime_ms / 1000ULL);

	return iso8601_utc(boot_sec, buf, len);
}

/* ---------- agent data dir (%LOCALAPPDATA%\assessment-agent) ----------
 *
 * User-level install root. Both the self-installer and the running agent
 * resolve their config / state / installed exe under here, so the vendor
 * agent never touches system locations (Program Files / ProgramData) and
 * needs no admin to read or write its own files. `suffix` (may be NULL/empty)
 * is appended with a backslash. Returns 0 on success, -1 on truncation or a
 * missing LOCALAPPDATA.
 */
int agent_data_path_w(const wchar_t *suffix, wchar_t *out, size_t cap)
{
	wchar_t base[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) return -1;
	int w = (suffix && *suffix)
		? _snwprintf(out, cap, L"%ls\\assessment-agent\\%ls", base, suffix)
		: _snwprintf(out, cap, L"%ls\\assessment-agent", base);
	return (w > 0 && (size_t)w < cap) ? 0 : -1;
}

int agent_data_path_a(const char *suffix, char *out, size_t cap)
{
	char base[MAX_PATH];
	DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) return -1;
	int w = (suffix && *suffix)
		? _snprintf(out, cap, "%s\\assessment-agent\\%s", base, suffix)
		: _snprintf(out, cap, "%s\\assessment-agent", base);
	return (w > 0 && (size_t)w < cap) ? 0 : -1;
}
