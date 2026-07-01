/**
 * @file download.h
 * @brief HTTPS package download with sha256 + host whitelist + size cap.
 *
 * Security boundaries (see docs/worker-task-design.md A-series decisions):
 *   - HTTP or HTTPS scheme accepted. HTTPS performs TLS peer + hostname
 *     verification; HTTP relies on the sha256 streaming check alone for
 *     integrity (intended for trusted intra-network ZDM / mirror hosts
 *     where TLS certificates are not deployed).
 *   - Redirects disabled (RD2). Any 30x response is treated as failure.
 *   - Host must match WORKER_DOWNLOAD_ALLOWED_HOSTS exactly (case-insensitive).
 *     Wildcards and subdomain matching are NOT supported.
 *   - sha256 is streamed during download via OpenSSL EVP_DigestUpdate and
 *     compared at completion; mismatch is a hard failure.
 *   - Download is aborted once received bytes exceed `expected_size_bytes`.
 *   - statvfs() on WORKER_TMP_DIR is checked before starting the transfer;
 *     free space below `expected_size_bytes + WORKER_DISK_RESERVE_MB` is a
 *     pre-flight failure (insufficient_disk).
 */

#ifndef ASSESSMENT_AGENT_DOWNLOAD_H
#define ASSESSMENT_AGENT_DOWNLOAD_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Outcome enumeration mirrored 1:1 onto task.result failure_reason.
 *
 * DOWNLOAD_OK indicates success and is the only value where the file at
 * @p out_path holds a verified package.
 */
typedef enum {
	DOWNLOAD_OK = 0,
	DOWNLOAD_ERR_URL_NOT_ALLOWED,
	DOWNLOAD_ERR_INSUFFICIENT_DISK,
	DOWNLOAD_ERR_DOWNLOAD_FAILED,   /* network / HTTP error / size overflow / redirect */
	DOWNLOAD_ERR_SHA256_MISMATCH,
	DOWNLOAD_ERR_INTERNAL,          /* OOM, fopen, OpenSSL init, etc. */
} download_status_t;

/**
 * @brief Download @p url to @p out_path, verifying sha256 + size + host.
 *
 * Streams the body to @p out_path while updating an OpenSSL SHA256 digest.
 * Aborts the transfer if received bytes exceed @p expected_size_bytes or
 * if the digest at completion does not match @p expected_sha256_hex.
 *
 * @param url                  HTTP or HTTPS URL. Must start with `https://` or `http://`.
 * @param expected_sha256_hex  64-char hex sha256.
 * @param expected_size_bytes  Strict upper bound (compared during transfer).
 * @param allowed_hosts_csv    Comma-separated host whitelist (NULL/empty = block all).
 * @param tmp_dir              Directory where @p out_path lives (for statvfs check).
 * @param disk_reserve_mb      Free-space margin above @p expected_size_bytes (MB).
 * @param out_path             Filesystem destination. Created/overwritten.
 *
 * @return DOWNLOAD_OK on success; otherwise an error code (matches
 *         task.result `failure_reason` enum semantics).
 */
download_status_t download_package(const char *url,
                                   const char *expected_sha256_hex,
                                   int64_t     expected_size_bytes,
                                   const char *allowed_hosts_csv,
                                   const char *tmp_dir,
                                   int         disk_reserve_mb,
                                   const char *out_path);

/**
 * @brief Return 1 if @p host is allowed by @p allowed_hosts_csv.
 *
 * Matching is case-insensitive ASCII exact compare against comma-separated
 * tokens (whitespace around tokens is trimmed). NULL or empty whitelist
 * blocks everything (returns 0).
 */
int download_host_allowed(const char *host, const char *allowed_hosts_csv);

/**
 * @brief Extract the host portion of an HTTP/HTTPS URL into @p out (size @p out_sz).
 *
 * Strips scheme, userinfo, port, and trailing path. Returns 1 on success,
 * 0 if @p url is not `http(s)://...` or hostname is empty.
 */
int download_url_extract_host(const char *url, char *out, size_t out_sz);

/**
 * @brief Returns 1 if @p url uses HTTPS scheme (= TLS verify required),
 *        0 if HTTP, 0 also if unsupported. Caller uses this to decide
 *        whether to apply SSL_VERIFYPEER/HOST inside libcurl options.
 */
int download_url_is_https(const char *url);

#endif
