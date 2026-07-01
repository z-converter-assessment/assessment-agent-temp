#ifndef ASSESSMENT_AGENT_UTIL_H
#define ASSESSMENT_AGENT_UTIL_H

#include <stddef.h>
#include <time.h>

char *run_cmd(const char *cmd);

char *read_file_all(const char *path);

void load_env_file(const char *path);

const char *getenv_default(const char *name, const char *fallback);

int parse_bool(const char *s, int fallback);

int getenv_bool(const char *name, int fallback);

int getenv_int_or(const char *name, int fallback);

char *trim_inplace(char *s);

char *iso8601_utc(time_t t, char *buf, size_t len);

char *iso8601_utc_ms(struct timespec ts, char *buf, size_t len);

char *uuid_v4(char *buf, size_t len);

int jitter_seconds(int base_sec, double frac);

void close_inherited_fds(void);

#endif
