#ifndef ASSESSMENT_AGENT_DOWNLOAD_H
#define ASSESSMENT_AGENT_DOWNLOAD_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
	DOWNLOAD_OK = 0,
	DOWNLOAD_ERR_URL_NOT_ALLOWED,
	DOWNLOAD_ERR_INSUFFICIENT_DISK,
	DOWNLOAD_ERR_DOWNLOAD_FAILED,
	DOWNLOAD_ERR_SHA256_MISMATCH,
	DOWNLOAD_ERR_INTERNAL,
} download_status_t;

download_status_t download_package(const char *url,
                                   const char *expected_sha256_hex,
                                   int64_t     expected_size_bytes,
                                   const char *allowed_hosts_csv,
                                   const char *tmp_dir,
                                   int         disk_reserve_mb,
                                   const char *out_path);

int download_host_allowed(const char *host, const char *allowed_hosts_csv);

int download_url_extract_host(const char *url, char *out, size_t out_sz);

int download_url_is_https(const char *url);

#endif
