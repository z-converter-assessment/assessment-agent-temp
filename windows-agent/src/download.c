/**
 * @file download.c
 * @brief HTTPS/HTTP package download (Windows) — sha256 + size cap + host whitelist + disk check.
 *
 * Linux download.c 와 동일 파이프라인 — pre-flight checks (scheme / host whitelist /
 * disk space) → libcurl streaming GET (redirects off, HTTPS 시 TLS verify on) →
 * write callback tees body bytes to sha256 EVP context + destination file →
 * size 초과 시 abort → 최종 digest 비교. 실패 경로마다 partial file 삭제.
 *
 * Linux 와의 유일한 차이:
 *   - statvfs(...)         → GetDiskFreeSpaceExA(...)
 *   - O_NOFOLLOW open      → 단순 _unlink + fopen("wb")
 *                            (Windows symlink 위협 모델이 다르며, %TEMP% 권한이
 *                             admin/SYSTEM 으로 제한된 상태가 전제)
 */

#define WIN32_LEAN_AND_MEAN

#include "download.h"

#include <curl/curl.h>
#include <openssl/evp.h>
#include "openssl_compat.h"   /* EVP_MD_CTX_new/_free on OpenSSL 1.0.2 (legacy) */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <io.h>

/* ============================================================
 * Helpers — host parsing / whitelist (Linux 와 동일)
 * ============================================================ */

static void download_str_tolower(char *s)
{
	if (!s) return;
	for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

/* 1 = https://, 0 = http://, -1 = unsupported. */
static int url_scheme_kind(const char *url)
{
	if (!url) return -1;
	if (_strnicmp(url, "https://", 8) == 0) return 1;
	if (_strnicmp(url, "http://",  7) == 0) return 0;
	return -1;
}

int download_url_is_https(const char *url)
{
	return url_scheme_kind(url) == 1;
}

int download_url_extract_host(const char *url, char *out, size_t out_sz)
{
	if (!url || !out || out_sz == 0) return 0;
	int kind = url_scheme_kind(url);
	if (kind < 0) return 0;
	const char *p = url + (kind == 1 ? 8 : 7);

	const char *at    = strchr(p, '@');
	const char *slash = strchr(p, '/');
	if (at && (!slash || at < slash)) p = at + 1;

	size_t i = 0;
	while (p[i] && p[i] != '/' && p[i] != ':' && p[i] != '?' && p[i] != '#') i++;
	if (i == 0 || i >= out_sz) return 0;

	memcpy(out, p, i);
	out[i] = '\0';
	download_str_tolower(out);
	return 1;
}

int download_host_allowed(const char *host, const char *allowed_hosts_csv)
{
	if (!host || !*host) return 0;
	if (!allowed_hosts_csv || !*allowed_hosts_csv) return 0;

	char host_lc[256];
	size_t hlen = strlen(host);
	if (hlen >= sizeof host_lc) return 0;
	memcpy(host_lc, host, hlen + 1);
	download_str_tolower(host_lc);

	const char *p = allowed_hosts_csv;
	while (*p) {
		while (*p == ',' || *p == ' ' || *p == '\t') p++;
		if (!*p) break;

		const char *start = p;
		while (*p && *p != ',') p++;
		size_t tlen = (size_t)(p - start);
		while (tlen > 0 && (start[tlen - 1] == ' ' || start[tlen - 1] == '\t')) tlen--;
		if (tlen == 0 || tlen >= sizeof host_lc) continue;

		char tok[256];
		memcpy(tok, start, tlen);
		tok[tlen] = '\0';
		download_str_tolower(tok);

		if (strcmp(tok, host_lc) == 0) return 1;
	}
	return 0;
}

/* ============================================================
 * Disk space pre-flight (Windows: GetDiskFreeSpaceExA)
 * ============================================================ */

#define DOWNLOAD_MAX_SIZE_BYTES (16LL * 1024 * 1024 * 1024)

static int size_bytes_in_range(int64_t expected_size_bytes)
{
	return expected_size_bytes > 0 &&
	       expected_size_bytes <= DOWNLOAD_MAX_SIZE_BYTES;
}

static int has_enough_space(const char *tmp_dir,
                            int64_t expected_size_bytes,
                            int disk_reserve_mb)
{
	ULARGE_INTEGER free_to_caller;
	ULARGE_INTEGER total_bytes;
	ULARGE_INTEGER total_free;
	const char *path = (tmp_dir && *tmp_dir) ? tmp_dir : "C:\\Windows\\Temp";

	if (!GetDiskFreeSpaceExA(path, &free_to_caller, &total_bytes, &total_free))
		return 0;

	if (disk_reserve_mb < 0)         disk_reserve_mb = 0;
	if (disk_reserve_mb > 1024 * 1024) disk_reserve_mb = 1024 * 1024; /* 1 TB cap */

	uint64_t avail     = (uint64_t)free_to_caller.QuadPart;
	uint64_t need_bytes = (uint64_t)expected_size_bytes;
	uint64_t reserve    = (uint64_t)disk_reserve_mb * 1024ULL * 1024ULL;
	if (need_bytes > UINT64_MAX - reserve) return 0;   /* overflow guard */
	return avail >= need_bytes + reserve;
}

/* ============================================================
 * libcurl write callback — tees to file + sha256 + size cap
 * ============================================================ */

struct sink {
	FILE          *fp;
	EVP_MD_CTX    *md;
	int64_t        written;
	int64_t        cap;
	int            overflow;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct sink *s = (struct sink *)userdata;
	size_t n = size * nmemb;
	if (s->overflow) return 0;
	if ((int64_t)(s->written + (int64_t)n) > s->cap) {
		s->overflow = 1;
		return 0;
	}
	if (fwrite(ptr, 1, n, s->fp) != n)         return 0;
	if (EVP_DigestUpdate(s->md, ptr, n) != 1)  return 0;
	s->written += (int64_t)n;
	return n;
}

/* ============================================================
 * Public — download_package
 * ============================================================ */

static int hex_eq_ci(const char *a, const char *b)
{
	if (!a || !b) return 0;
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
		a++; b++;
	}
	return *a == 0 && *b == 0;
}

/* libcurl progress callback — cancel check 만 함. transfer 진행 정보는 무시. */
struct cancel_ctx {
	download_cancel_fn cb;
	void              *user;
};

static int xferinfo_cb(void *clientp,
                       curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
	(void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
	struct cancel_ctx *c = (struct cancel_ctx *)clientp;
	if (c && c->cb && c->cb(c->user)) return 1;  /* 비-0 = abort */
	return 0;
}

download_status_t download_package(const char        *url,
                                   const char        *expected_sha256_hex,
                                   int64_t            expected_size_bytes,
                                   const char        *allowed_hosts_csv,
                                   const char        *tmp_dir,
                                   int                disk_reserve_mb,
                                   const char        *out_path,
                                   download_cancel_fn cancel_cb,
                                   void              *cancel_user)
{
	if (!url || !expected_sha256_hex || !out_path) return DOWNLOAD_ERR_INTERNAL;
	if (!size_bytes_in_range(expected_size_bytes)) return DOWNLOAD_ERR_DOWNLOAD_FAILED;

	/* 1. Scheme + host whitelist */
	char host[256];
	if (!download_url_extract_host(url, host, sizeof host)) return DOWNLOAD_ERR_URL_NOT_ALLOWED;
	if (!download_host_allowed(host, allowed_hosts_csv))    return DOWNLOAD_ERR_URL_NOT_ALLOWED;

	/* 2. Disk pre-flight */
	if (!has_enough_space(tmp_dir, expected_size_bytes, disk_reserve_mb))
		return DOWNLOAD_ERR_INSUFFICIENT_DISK;

	/* 3. 기존 파일 제거 + fopen.
	 * Windows symlink/junction 위협 모델은 Linux 와 다름. %TEMP% 권한이 admin/SYSTEM
	 * 으로 제한된 상태가 install.ps1 의 ACL tighten 으로 보장된다 — agent service 계정
	 * (LocalSystem) 만 접근 가능하므로 race 공격면이 최소. */
	_unlink(out_path);
	FILE *fp = fopen(out_path, "wb");
	if (!fp) return DOWNLOAD_ERR_INTERNAL;

	EVP_MD_CTX *md = EVP_MD_CTX_new();
	if (!md) { fclose(fp); _unlink(out_path); return DOWNLOAD_ERR_INTERNAL; }
	if (EVP_DigestInit_ex(md, EVP_sha256(), NULL) != 1) {
		EVP_MD_CTX_free(md); fclose(fp); _unlink(out_path);
		return DOWNLOAD_ERR_INTERNAL;
	}

	struct sink s = { .fp = fp, .md = md, .written = 0,
	                  .cap = expected_size_bytes, .overflow = 0 };

	/* 4. libcurl easy handle (Linux 와 동일 옵션 셋). */
	CURL *c = curl_easy_init();
	if (!c) { EVP_MD_CTX_free(md); fclose(fp); _unlink(out_path); return DOWNLOAD_ERR_INTERNAL; }

	long http_code = 0;
	int is_https = download_url_is_https(url);
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https,http");
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);          /* RD2 */
	if (is_https) {
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
	}
	curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);

	/* Cancellation hook — drain 시 stop_requested() / 기타 신호로 abort. */
	struct cancel_ctx cancel = { .cb = cancel_cb, .user = cancel_user };
	if (cancel_cb) {
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
		curl_easy_setopt(c, CURLOPT_XFERINFODATA, &cancel);
	}

	CURLcode cc = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
	curl_easy_cleanup(c);
	fclose(fp);

	if (cc != CURLE_OK) {
		EVP_MD_CTX_free(md); _unlink(out_path);
		return DOWNLOAD_ERR_DOWNLOAD_FAILED;
	}
	if (http_code >= 300) {
		EVP_MD_CTX_free(md); _unlink(out_path);
		return DOWNLOAD_ERR_DOWNLOAD_FAILED;
	}
	if (s.written != expected_size_bytes) {
		EVP_MD_CTX_free(md); _unlink(out_path);
		return DOWNLOAD_ERR_DOWNLOAD_FAILED;
	}

	/* 5. Final sha256 compare */
	unsigned char raw[EVP_MAX_MD_SIZE];
	unsigned int  raw_len = 0;
	if (EVP_DigestFinal_ex(md, raw, &raw_len) != 1 || raw_len != 32) {
		EVP_MD_CTX_free(md); _unlink(out_path);
		return DOWNLOAD_ERR_INTERNAL;
	}
	EVP_MD_CTX_free(md);

	char got[65] = { 0 };
	for (unsigned int i = 0; i < raw_len; i++)
		snprintf(got + i * 2, 3, "%02x", raw[i]);

	if (!hex_eq_ci(got, expected_sha256_hex)) {
		_unlink(out_path);
		return DOWNLOAD_ERR_SHA256_MISMATCH;
	}
	return DOWNLOAD_OK;
}
