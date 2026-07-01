/**
 * @file util.h
 * @brief Process / environment utilities (Windows).
 *
 * Linux util.h 와 페이로드용 헬퍼는 동일 시그니처. fork/close_inherited_fds
 * 같은 POSIX-전용 헬퍼는 제외 (Worker가 v2로 미뤄졌고, 윈도우는 서비스 모델).
 */

#ifndef ASSESSMENT_AGENT_UTIL_H
#define ASSESSMENT_AGENT_UTIL_H

#include <stddef.h>
#include <time.h>

/**
 * @brief Load a .env-style file into the process environment.
 *
 * Existing variables are not overwritten (shell env wins). Missing files are
 * ignored silently. Supports `KEY=VALUE`, single-quoted, double-quoted, and
 * `# comment` lines.
 *
 * Windows: uses _putenv_s.
 */
void load_env_file(const char *path);

const char *getenv_default(const char *name, const char *fallback);

/**
 * @brief Parse @p s as boolean (case-insensitive).
 *   True:  "1", "true", "yes", "on", "y", "t" → 1
 *   False: "0", "false", "no", "off", "n", "f" → 0
 *   NULL/empty: @p fallback
 *   Unrecognized: -1 sentinel
 */
int parse_bool(const char *s, int fallback);

int getenv_bool(const char *name, int fallback);
int getenv_int_or(const char *name, int fallback);

/**
 * @brief Format @p t as ISO 8601 UTC into @p buf (size >= 21).
 *        Output: `YYYY-MM-DDTHH:MM:SSZ`.
 */
char *iso8601_utc(time_t t, char *buf, size_t len);

/**
 * @brief Fill @p buf with @p len cryptographically-random bytes.
 *
 * NT-version-agnostic CSPRNG. Resolves `BCryptGenRandom` from `bcrypt.dll`
 * dynamically (Vista / Server 2008+); on hosts without bcrypt (NT 5.2 /
 * Server 2003) it falls back to the legacy CryptoAPI
 * (`CryptAcquireContextW(PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)` +
 * `CryptGenRandom`, both in advapi32 which is present on every NT).
 *
 * Because bcrypt is resolved at runtime, no static `-lbcrypt` import is
 * emitted — keeping the import table valid on Server 2003.
 *
 * @return 1 on success, 0 on failure (buf contents undefined on failure).
 */
int compat_rand_bytes(unsigned char *buf, size_t len);

/**
 * @brief Monotonic milliseconds since boot, 64-bit.
 *
 * Single entry point so every call site is NT-version-agnostic. Resolves
 * `GetTickCount64` (Vista / Server 2008+) once via GetProcAddress; if absent
 * (NT 5.2 / Server 2003) it falls back to the 32-bit `GetTickCount` and
 * extends it to 64-bit by tracking wraps across calls (single-threaded use).
 *
 * Return type is ULONGLONG-compatible so existing call sites and struct
 * fields are unchanged.
 */
unsigned long long monotonic_ms(void);

/**
 * @brief Generate a UUID v4 string into @p buf (size >= 37).
 *        Source: CoCreateGuid, with compat_rand_bytes available for callers
 *        that need raw random bytes (e.g. installer MachineGuid).
 */
char *uuid_v4(char *buf, size_t len);

/**
 * @brief Apply ±@p frac uniform jitter to @p base_sec, using rand().
 *        Result: `base_sec * (1 + U(-frac, +frac))` floored.
 *        Returns @p base_sec unchanged when @p base_sec <= 0.
 */
int jitter_seconds(int base_sec, double frac);

/**
 * @brief Compute boot time as ISO 8601 UTC.
 *
 * monotonic_ms() + current wall-clock to derive system boot wall-clock,
 * cached at process start (NTP drift는 한 번 캡처된 이후 재읽기 안 함).
 * Returns @p buf on success, NULL on failure.
 */
char *get_boot_time_iso8601(char *buf, size_t len);

/**
 * @brief Build "%LOCALAPPDATA%\\assessment-agent[\\suffix]".
 *
 * The user-level install root, shared by the self-installer and the running
 * agent so the vendor binary never touches Program Files / ProgramData and
 * needs no admin for its own files. @p suffix may be NULL/empty. Returns 0 on
 * success, -1 on truncation or missing LOCALAPPDATA. Wide + ANSI variants.
 */
int agent_data_path_w(const wchar_t *suffix, wchar_t *out, size_t cap);
int agent_data_path_a(const char *suffix, char *out, size_t cap);

#endif
