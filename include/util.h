/**
 * @file util.h
 * @brief Process / environment utilities shared by collect and publish.
 *
 * Domain logic (AMQP, payload schema) does not belong here.
 */

#ifndef ASSESSMENT_AGENT_UTIL_H
#define ASSESSMENT_AGENT_UTIL_H

#include <stddef.h>
#include <time.h>

/**
 * @brief Run a shell command and capture stdout into a malloc'd string.
 *
 * Returns NULL if popen fails, the child exits abnormally, exit status != 0,
 * or memory cannot be allocated. Caller frees with free().
 */
char *run_cmd(const char *cmd);

/**
 * @brief Read an entire file into a malloc'd null-terminated string.
 *
 * Returns NULL on open failure or OOM. Caller frees with free().
 */
char *read_file_all(const char *path);

/**
 * @brief Load a .env-style file into the process environment.
 *
 * Existing variables are not overwritten (shell env wins). Missing files are
 * ignored silently. Supports `KEY=VALUE`, single-quoted, double-quoted, and
 * `# comment` lines.
 */
void load_env_file(const char *path);

/**
 * @brief getenv() wrapper. Returns @p fallback if the variable is unset or empty.
 */
const char *getenv_default(const char *name, const char *fallback);

/**
 * @brief Parse @p s as a boolean (case-insensitive).
 *
 *   True:  "1", "true", "yes", "on", "y", "t" → returns 1
 *   False: "0", "false", "no", "off", "n", "f" → returns 0
 *   NULL/empty: returns @p fallback
 *   Other (e.g. "2", "enabled", typos): returns -1 (sentinel)
 *
 * `getenv_bool` translates the -1 sentinel into a warning + fallback,
 * so unrecognized operator values are surfaced rather than silently
 * disabling features.
 */
int parse_bool(const char *s, int fallback);

/**
 * @brief Read env var @p name and parse as boolean. Wraps getenv + parse_bool.
 */
int getenv_bool(const char *name, int fallback);

/**
 * @brief Read env var @p name and parse as int (defensively).
 *
 * Like atoi but: returns @p fallback when env unset, empty, or unparseable
 * (instead of silently returning 0 which can disable safety caps).
 */
int getenv_int_or(const char *name, int fallback);

/**
 * @brief Trim ASCII whitespace from both ends of @p s in place.
 */
char *trim_inplace(char *s);

/**
 * @brief Format @p t as ISO 8601 UTC into @p buf (size >= 21).
 *
 * Output: `YYYY-MM-DDTHH:MM:SSZ`. Returns @p buf.
 */
char *iso8601_utc(time_t t, char *buf, size_t len);

/**
 * @brief Format @p ts as ISO 8601 UTC with millisecond precision into @p buf
 *        (size >= 25).
 *
 * Output: `YYYY-MM-DDTHH:MM:SS.sssZ`. Returns @p buf.
 */
char *iso8601_utc_ms(struct timespec ts, char *buf, size_t len);

/**
 * @brief Generate a UUID v4 string into @p buf (size >= 37).
 *
 * Reads /proc/sys/kernel/random/uuid first; if that fails, builds a synthetic
 * value from hostname/pid/timestamp.
 */
char *uuid_v4(char *buf, size_t len);

/**
 * @brief Apply ±@p frac uniform jitter to @p base_sec, using rand().
 *
 * Result is `base_sec * (1 + U(-frac, +frac))` rounded down. Caller is
 * responsible for seeding rand() once at process start. @p frac is clamped
 * to [0, 1). Returns @p base_sec unchanged when @p base_sec <= 0.
 */
int jitter_seconds(int base_sec, double frac);

/**
 * @brief Close every inherited fd above STDERR_FILENO.
 *
 * Strategy:
 *   1. close_range(2) syscall (Linux 5.9+) — atomic, fastest
 *   2. /proc/self/fd walk — portable on Linux
 *   3. numeric sweep up to RLIMIT_NOFILE (capped at 4096) — last resort
 *
 * Used by worker child immediately after fork to drop the inherited
 * AMQP TLS socket and any other parent-side fds that should not survive
 * into the install.sh process tree (CRITICAL #5 + the worker-child
 * carrier window from the round-2 review).
 *
 * Safe to call from a forked-but-not-execve'd child. Does NOT touch
 * fds 0/1/2 (caller is responsible for setting up stdio).
 */
void close_inherited_fds(void);

#endif
