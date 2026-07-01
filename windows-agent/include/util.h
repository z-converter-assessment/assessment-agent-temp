#ifndef ASSESSMENT_AGENT_UTIL_H
#define ASSESSMENT_AGENT_UTIL_H

#include <stddef.h>
#include <time.h>

void load_env_file(const char *path);

const char *getenv_default(const char *name, const char *fallback);

int parse_bool(const char *s, int fallback);

int getenv_bool(const char *name, int fallback);
int getenv_int_or(const char *name, int fallback);

char *iso8601_utc(time_t t, char *buf, size_t len);

int compat_rand_bytes(unsigned char *buf, size_t len);

unsigned long long monotonic_ms(void);

char *uuid_v4(char *buf, size_t len);

int jitter_seconds(int base_sec, double frac);

char *get_boot_time_iso8601(char *buf, size_t len);

int agent_data_path_w(const wchar_t *suffix, wchar_t *out, size_t cap);
int agent_data_path_a(const char *suffix, char *out, size_t cap);

#endif
