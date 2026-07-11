#define WIN32_LEAN_AND_MEAN

#include "collect.h"
#include "util.h"
#include "cJSON.h"
#include <openssl/evp.h>
#include "openssl_compat.h"
#include <curl/curl.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <intrin.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <tlhelp32.h>
#include <winperf.h>
#include "nt52_compat.h"
#include "collect_internal.h"

static char g_boot_time_iso[32]      = {0};
static char g_agent_started_iso[32]  = {0};
static int  g_times_cached           = 0;

static void cache_process_times(void)
{
	if (g_times_cached) return;
	get_boot_time_iso8601(g_boot_time_iso, sizeof g_boot_time_iso);
	time_t now; time(&now);
	iso8601_utc(now, g_agent_started_iso, sizeof g_agent_started_iso);
	g_times_cached = 1;
}

/* --- datapoint-array 빌더 (Linux 트리와 동형) --- */
cJSON *wire_ns(cJSON *root, const char *ns)
{
	cJSON *o = cJSON_GetObjectItemCaseSensitive(root, ns);
	if (!o) { o = cJSON_CreateObject(); cJSON_AddItemToObject(root, ns, o); }
	return o;
}

cJSON *wire_metric(cJSON *ns, const char *name, const char *type, const char *unit)
{
	cJSON *m = cJSON_CreateObject();
	cJSON_AddStringToObject(m, "type", type);
	cJSON_AddStringToObject(m, "unit", unit);
	cJSON_AddItemToObject(m, "points", cJSON_CreateArray());
	cJSON_AddItemToObject(ns, name, m);
	return m;
}

cJSON *wire_point(cJSON *metric)
{
	cJSON *p = cJSON_CreateObject();
	cJSON_AddItemToObject(p, "attr", cJSON_CreateObject());
	cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(metric, "points"), p);
	return p;
}

void wire_point_attr(cJSON *point, const char *k, const char *v)
{
	if (v && *v)
		cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(point, "attr"), k, v);
}

void wire_point_value(cJSON *point, double v) { cJSON_AddNumberToObject(point, "value", v); }

void wire_point_null(cJSON *point) { cJSON_AddNullToObject(point, "value"); }


/* device(+direction) 데이터포인트 한 줄 발행 — 가장 흔한 패턴의 헬퍼. direction 이 NULL 이면 device 만. */
void wire_point_dev_dir(cJSON *metric, const char *device, const char *direction, double value)
{
	cJSON *p = wire_point(metric);
	wire_point_attr(p, "device", device);
	if (direction) wire_point_attr(p, "direction", direction);
	wire_point_value(p, value);
}
void wire_metric_scalar(cJSON *ns, const char *name, const char *type,
                              const char *unit, int have, double v)
{
	cJSON *m = wire_metric(ns, name, type, unit);
	cJSON *p = wire_point(m);
	if (have) wire_point_value(p, v); else wire_point_null(p);
}

void wire_add_envelope(cJSON *root, const char *msg_type,
                                const char *machine_id, const char *agent_version)
{
	cache_process_times();

	cJSON_AddStringToObject(root, "schema_version", "1.0");
	cJSON_AddStringToObject(root, "message_type", msg_type);
	/* machine_id 못 구하면 null(위조 금지). 식별·라우팅은 agent_id, 유니크는 mac 기반 composite_id가 유지. */
	if (machine_id && *machine_id)
		cJSON_AddStringToObject(root, "machine_id", machine_id);
	else
		cJSON_AddNullToObject(root, "machine_id");
	cJSON_AddStringToObject(root, "composite_id", cached_composite_id(machine_id));
	cJSON_AddStringToObject(root, "agent_id",     cached_agent_id());
	cJSON_AddStringToObject(root, "os_family",    "windows");
	cJSON_AddStringToObject(root, "agent_version", agent_version ? agent_version : "0.0.0");

	time_t now; time(&now);
	char now_iso[32];
	iso8601_utc(now, now_iso, sizeof now_iso);
	cJSON_AddStringToObject(root, "collected_at", now_iso);

	char hostname[256];
	get_hostname_utf8(hostname, sizeof hostname);
	cJSON_AddStringToObject(root, "hostname", hostname);

	char msg_id[64];
	uuid_v4(msg_id, sizeof msg_id);
	cJSON_AddStringToObject(root, "message_id", msg_id);

	if (g_boot_time_iso[0]) cJSON_AddStringToObject(root, "boot_time", g_boot_time_iso);
	else                    cJSON_AddNullToObject(root, "boot_time");
	if (g_agent_started_iso[0]) cJSON_AddStringToObject(root, "agent_started_at", g_agent_started_iso);
	else                        cJSON_AddNullToObject(root, "agent_started_at");
}

char *resolve_machine_id(void)
{
	HKEY hKey;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
	    "SOFTWARE\\Microsoft\\Cryptography",
	    0, KEY_READ | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS) {
		fprintf(stderr, "[agent] MachineGuid registry open failed - trying cloud IMDS fallback\n");
		return try_cloud_instance_id();
	}
	char buf[128] = {0};
	DWORD sz = sizeof buf;
	DWORD type = 0;
	LONG r = RegQueryValueExA(hKey, "MachineGuid", NULL, &type, (LPBYTE)buf, &sz);
	RegCloseKey(hKey);
	if (r != ERROR_SUCCESS || type != REG_SZ) {
		fprintf(stderr, "[agent] MachineGuid query failed - trying cloud IMDS fallback\n");
		return try_cloud_instance_id();
	}

	size_t l = strlen(buf);
	while (l > 0 && (buf[l-1] == '\0' || buf[l-1] == '\r' || buf[l-1] == '\n' || buf[l-1] == ' '))
		buf[--l] = '\0';
	for (size_t i = 0; i < l; i++)
		buf[i] = (char)tolower((unsigned char)buf[i]);

	if (l == 0) {
		fprintf(stderr, "[agent] MachineGuid empty - trying cloud IMDS fallback\n");
		return try_cloud_instance_id();
	}

	char *out = malloc(l + 1);
	if (!out) return NULL;
	memcpy(out, buf, l + 1);
	return out;
}

static int mac_cmp(const void *a, const void *b)
{
	return strcmp(*(const char **)a, *(const char **)b);
}

/* GAA에서 물리 MAC(6바이트, up·non-loopback·non-tunnel)을 dedup·정렬해 mac_list에 채운다(원소 malloc, 반환=개수).
 * mac_addresses와 composite_id 해시 입력이 이 목록을 공유하므로 필터/정렬을 이 한 곳에서만 정의(두 MAC 집합 동일 보장). */
static int collect_mac_list(IP_ADAPTER_ADDRESSES *aa, char *mac_list[], int cap)
{
	int mac_count = 0;
	for (IP_ADAPTER_ADDRESSES *p = aa; p; p = p->Next) {
		if (p->OperStatus != IfOperStatusUp)        continue;
		if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		if (p->IfType == IF_TYPE_TUNNEL)            continue;
		if (p->PhysicalAddressLength != 6)          continue;
		if (mac_count >= cap)                       break;

		char mac[18];
		snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x",
			p->PhysicalAddress[0], p->PhysicalAddress[1], p->PhysicalAddress[2],
			p->PhysicalAddress[3], p->PhysicalAddress[4], p->PhysicalAddress[5]);

		int dup = 0;
		for (int i = 0; i < mac_count; i++)
			if (strcmp(mac_list[i], mac) == 0) { dup = 1; break; }
		if (!dup) {
			mac_list[mac_count] = malloc(sizeof mac);
			if (mac_list[mac_count]) {
				memcpy(mac_list[mac_count], mac, sizeof mac);
				mac_count++;
			}
		}
	}
	qsort(mac_list, mac_count, sizeof(char *), mac_cmp);
	return mac_count;
}

const char *cached_composite_id(const char *machine_id)
{
	static char hex_buf[65];
	static int  cached = 0;
	if (cached)
		return hex_buf;

	hex_buf[0] = '\0';
	cached = 1;

	enum { MAC_CAP = 64 };
	char *mac_list[MAC_CAP];
	int mac_count = 0;

	ULONG buf_len = 16 * 1024;
	IP_ADAPTER_ADDRESSES *aa = malloc(buf_len);
	ULONG ret = ERROR_NOT_ENOUGH_MEMORY;
	if (aa) {
		ret = GetAdaptersAddresses(AF_UNSPEC,
			GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
			NULL, aa, &buf_len);
		if (ret == ERROR_BUFFER_OVERFLOW) {
			free(aa);
			aa = malloc(buf_len);
			if (aa) {
				ret = GetAdaptersAddresses(AF_UNSPEC,
					GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
					NULL, aa, &buf_len);
			}
		}
	}
	if (aa && ret == NO_ERROR)
		mac_count = collect_mac_list(aa, mac_list, MAC_CAP);
	free(aa);

	EVP_MD_CTX *md = EVP_MD_CTX_new();
	int ok = (md != NULL) && (EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1);
	if (ok) {
		const char *mid = (machine_id && *machine_id) ? machine_id : "";
		ok = (EVP_DigestUpdate(md, mid, strlen(mid)) == 1);
		if (ok) ok = (EVP_DigestUpdate(md, "\n", 1) == 1);
		for (int i = 0; ok && i < mac_count; i++) {
			ok = (EVP_DigestUpdate(md, mac_list[i], strlen(mac_list[i])) == 1);
			if (ok && i + 1 < mac_count)
				ok = (EVP_DigestUpdate(md, "\n", 1) == 1);
		}
	}

	unsigned char raw[EVP_MAX_MD_SIZE];
	unsigned int rawlen = 0;
	if (ok) ok = (EVP_DigestFinal_ex(md, raw, &rawlen) == 1);
	if (md) EVP_MD_CTX_free(md);

	for (int i = 0; i < mac_count; i++) free(mac_list[i]);

	if (!ok || rawlen != 32) {
		fprintf(stderr, "[agent] composite_id: EVP digest failed\n");
		hex_buf[0] = '\0';
		return hex_buf;
	}

	static const char hex_chars[] = "0123456789abcdef";
	for (unsigned int i = 0; i < 32; i++) {
		hex_buf[i * 2]     = hex_chars[(raw[i] >> 4) & 0xF];
		hex_buf[i * 2 + 1] = hex_chars[raw[i] & 0xF];
	}
	hex_buf[64] = '\0';
	return hex_buf;
}

/* 첫 실행 시 UUIDv4 생성 -> %ProgramData%\assessment-agent\agent-id에 영구 저장·재사용. prep-image가 지워 클론마다 재생성. */
const char *cached_agent_id(void)
{
	static char id_buf[64];
	static int  cached = 0;
	if (cached)
		return id_buf;
	cached = 1;
	id_buf[0] = '\0';

	char path[MAX_PATH];
	if (agent_data_path_a("agent-id", path, sizeof path) != 0) {
		uuid_v4(id_buf, sizeof id_buf);   /* 경로 못 잡으면 휘발성 */
		return id_buf;
	}

	FILE *f = fopen(path, "r");
	if (f) {
		if (fgets(id_buf, sizeof id_buf, f)) {
			size_t l = strlen(id_buf);
			while (l && (id_buf[l - 1] == '\n' || id_buf[l - 1] == '\r' ||
			             id_buf[l - 1] == ' '))
				id_buf[--l] = '\0';
			if (l >= 32) { fclose(f); return id_buf; }
		}
		fclose(f);
	}

	uuid_v4(id_buf, sizeof id_buf);
	char dir[MAX_PATH];
	if (agent_data_path_a(NULL, dir, sizeof dir) == 0)
		CreateDirectoryA(dir, NULL);   /* base는 보통 install이 생성 */
	/* agent_id는 불변 계약 — 절단 파일이 남으면 다음 시작에 새 UUID가 재발급된다(식별키·큐 통째 변경).
	 * temp에 쓰고 FlushFileBuffers 후 MoveFileExA(REPLACE|WRITE_THROUGH)로 원자 교체, 실패 시 파일 미생성(휘발성 id로 강등). */
	char tmp[MAX_PATH];
	if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) < sizeof tmp) {
		HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		                       FILE_ATTRIBUTE_NORMAL, NULL);
		if (h != INVALID_HANDLE_VALUE) {
			char line[80];
			int n = snprintf(line, sizeof line, "%s\n", id_buf);
			DWORD wr = 0;
			BOOL ok = (n > 0 && (size_t)n < sizeof line &&
			           WriteFile(h, line, (DWORD)n, &wr, NULL) && wr == (DWORD)n &&
			           FlushFileBuffers(h));
			CloseHandle(h);
			if (ok)
				MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
			else
				DeleteFileA(tmp);
		}
	}
	return id_buf;
}

char *try_cloud_instance_id(void)
{
	char *id = fetch_imds_chain(
		"http://169.254.169.254/latest/meta-data/instance-id",
		"http://169.254.169.254/metadata/instance/compute/vmId?api-version=2021-02-01&format=text",
		"http://metadata.google.internal/computeMetadata/v1/instance/id");
	if (id) fprintf(stderr, "[agent] cloud IMDS instance-id fallback succeeded\n");
	return id;
}
