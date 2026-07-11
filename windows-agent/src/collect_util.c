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

/* NT5.2 로드 가드: NT6+ 전용 API 하드임포트 금지(전부 GetProcAddress 런타임 해소), 세대 분기는 RtlGetVersion(major>=6)으로 런타임에. */
int agent_is_nt6(void)
{
	static int resolved = 0, is_nt6 = 0;
	if (!resolved) {
		resolved = 1;
		HMODULE nt = GetModuleHandleA("ntdll.dll");
		typedef LONG (WINAPI *RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
		RtlGetVersionFn rgv = nt ? (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
		if (rgv) {
			RTL_OSVERSIONINFOW vi;
			ZeroMemory(&vi, sizeof vi);
			vi.dwOSVersionInfoSize = sizeof vi;
			if (rgv(&vi) == 0)
				is_nt6 = (vi.dwMajorVersion >= 6);
		}
	}
	return is_nt6;
}

/* NT5.2 ws2_32엔 inet_ntop이 없어(Vista+ export) 이 완전형 폴백을 쓴다. */
static const char *compat_inet_ntop(int af, const void *src, char *dst, size_t size)
{
	const unsigned char *b = (const unsigned char *)src;
	if (af == AF_INET) {
		snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
		return dst;
	}
	if (af == AF_INET6) {
		snprintf(dst, size, "%x:%x:%x:%x:%x:%x:%x:%x",
		         (b[0] << 8) | b[1],  (b[2] << 8) | b[3],
		         (b[4] << 8) | b[5],  (b[6] << 8) | b[7],
		         (b[8] << 8) | b[9],  (b[10] << 8) | b[11],
		         (b[12] << 8) | b[13], (b[14] << 8) | b[15]);
		return dst;
	}
	return NULL;
}

const char *agent_inet_ntop(int af, const void *src, char *dst, size_t size)
{
	typedef PCSTR (WSAAPI *inet_ntop_fn)(INT, const void *, PSTR, size_t);
	static int resolved = 0;
	static inet_ntop_fn real = NULL;
	if (!resolved) {
		resolved = 1;
		HMODULE ws = GetModuleHandleA("ws2_32.dll");
		if (ws)
			real = (inet_ntop_fn)(void *)GetProcAddress(ws, "inet_ntop");
	}
	if (real)
		return real(af, src, dst, size);
	return compat_inet_ntop(af, src, dst, size);
}

/* \\.\PhysicalDrive<i> 를 GENERIC_READ 로 연다 — access=0 핸들엔 디스크 IOCTL 이 ACCESS_DENIED. */
HANDLE open_physical_drive(int i)
{
	wchar_t path[64];
	swprintf(path, 64, L"\\\\.\\PhysicalDrive%d", i);
	return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                   NULL, OPEN_EXISTING, 0, NULL);
}

DWORD perf_index(const char *want)
{
	static char *buf = NULL;
	static int loaded = 0;
	if (!loaded) {
		loaded = 1;
		HKEY k;
		if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
		        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Perflib\\009",
		        0, KEY_READ, &k) == ERROR_SUCCESS) {
			DWORD sz = 0;
			if (RegQueryValueExA(k, "Counter", NULL, NULL, NULL, &sz) == ERROR_SUCCESS && sz) {
				buf = malloc(sz);
				if (buf && RegQueryValueExA(k, "Counter", NULL, NULL, (LPBYTE)buf, &sz) != ERROR_SUCCESS) {
					free(buf); buf = NULL;
				}
			}
			RegCloseKey(k);
		}
	}
	if (!buf) return 0;
	/* REG_MULTI_SZ: "<index>\0<name>\0<index>\0<name>\0...\0" (index 먼저) */
	for (char *p = buf; *p; ) {
		char *sidx = p; p += strlen(p) + 1;
		if (!*p) break;
		char *name = p; p += strlen(p) + 1;
		if (_stricmp(name, want) == 0) return (DWORD)strtoul(sidx, NULL, 10);
	}
	return 0;
}

/* value_name = 공백구분 객체 인덱스("2 4 234"). 반환 버퍼는 free, HKEY_PERFORMANCE_DATA는 호출측이 RegCloseKey. */
BYTE *perf_query(const char *value_name)
{
	DWORD sz = 65536;
	BYTE *buf = malloc(sz);
	if (!buf) return NULL;
	for (;;) {
		DWORD type = 0, ret = sz;
		LONG rc = RegQueryValueExA(HKEY_PERFORMANCE_DATA, value_name, NULL, &type, buf, &ret);
		if (rc == ERROR_SUCCESS) return buf;
		if (rc == ERROR_MORE_DATA) {
			sz *= 2;
			BYTE *nb = realloc(buf, sz);
			if (!nb) { free(buf); return NULL; }
			buf = nb;
			continue;
		}
		free(buf);
		return NULL;
	}
}

PERF_OBJECT_TYPE *perf_object(PERF_DATA_BLOCK *db, DWORD idx)
{
	if (!idx) return NULL;
	PERF_OBJECT_TYPE *o = (PERF_OBJECT_TYPE *)((BYTE *)db + db->HeaderLength);
	for (DWORD i = 0; i < db->NumObjectTypes; i++) {
		if (o->ObjectNameTitleIndex == idx) return o;
		o = (PERF_OBJECT_TYPE *)((BYTE *)o + o->TotalByteLength);
	}
	return NULL;
}

PERF_COUNTER_DEFINITION *perf_counter(PERF_OBJECT_TYPE *o, DWORD idx)
{
	if (!o || !idx) return NULL;
	PERF_COUNTER_DEFINITION *c = (PERF_COUNTER_DEFINITION *)((BYTE *)o + o->HeaderLength);
	for (DWORD i = 0; i < o->NumCounters; i++) {
		if (c->CounterNameTitleIndex == idx) return c;
		c = (PERF_COUNTER_DEFINITION *)((BYTE *)c + c->ByteLength);
	}
	return NULL;
}

unsigned long long perf_read(PERF_COUNTER_BLOCK *cb, PERF_COUNTER_DEFINITION *c)
{
	if (!cb || !c) return 0;
	BYTE *p = (BYTE *)cb + c->CounterOffset;
	if (c->CounterSize >= 8) return *(unsigned long long *)p;
	return *(DWORD *)p;
}

/* PhysicalDisk 인스턴스명 "0 C:" -> 물리 드라이브 번호. 숫자 시작 아니면(-1) 스킵(_Total 등). */
int perf_disk_num(const wchar_t *name)
{
	if (name[0] < L'0' || name[0] > L'9') return -1;
	return (int)wcstol(name, NULL, 10);
}

/* Memory\"Free & Zero Page List Bytes"(NT6.0+)로 진짜 free 실측 — ullAvailPhys(회수 가능분 포함)와 다르다. NT5.2/실패 시 -1. */
long long query_free_kb(void)
{
	DWORD i_mem = perf_index("Memory");
	if (!i_mem) return -1;
	char vn[16];
	snprintf(vn, sizeof vn, "%lu", (unsigned long)i_mem);
	BYTE *buf = perf_query(vn);
	RegCloseKey(HKEY_PERFORMANCE_DATA);
	if (!buf) return -1;
	long long free_kb = -1;
	PERF_DATA_BLOCK *db = (PERF_DATA_BLOCK *)buf;
	PERF_OBJECT_TYPE *omem = perf_object(db, i_mem);
	if (omem && omem->NumInstances == PERF_NO_INSTANCES) {
		PERF_COUNTER_BLOCK *cb = (PERF_COUNTER_BLOCK *)((BYTE *)omem + omem->DefinitionLength);
		PERF_COUNTER_DEFINITION *c = perf_counter(omem, perf_index("Free & Zero Page List Bytes"));
		if (c) free_kb = (long long)(perf_read(cb, c) / 1024ULL);
	}
	free(buf);
	return free_kb;
}

/* NtQuerySystemInformation(SystemPerformanceInformation=2): I/O 매니저가 직접 유지하는 시스템 전역 누적 I/O.
 * perflib/IOCTL과 달리 diskperf 독립이라 virtio 등 diskperf 미수집 환경의 disk_io 소스. 성공 1, 실패 0. */
int query_system_io(unsigned long long *read_ops, unsigned long long *write_ops,
                           unsigned long long *read_bytes, unsigned long long *write_bytes)
{
	typedef LONG (WINAPI *NQSI)(ULONG, PVOID, ULONG, PULONG);
	HMODULE nt = GetModuleHandleA("ntdll.dll");
	NQSI f = nt ? (NQSI)(void *)GetProcAddress(nt, "NtQuerySystemInformation") : NULL;
	if (!f) return 0;
	BYTE buf[2048];
	ULONG ret = 0;
	LONG st = f(2, buf, sizeof buf, &ret);
	if (st == (LONG)0xC0000004UL && ret > 0 && ret <= sizeof buf)  /* STATUS_INFO_LENGTH_MISMATCH */
		st = f(2, buf, ret, &ret);
	if (st != 0 || ret < 40) return 0;
	/* SYSTEM_PERFORMANCE_INFORMATION 안정 prefix 오프셋: +8 ReadBytes(8), +16 WriteBytes(8), +32 ReadOps(4), +36 WriteOps(4). */
	*read_bytes  = *(unsigned long long *)(buf + 8);
	*write_bytes = *(unsigned long long *)(buf + 16);
	*read_ops    = *(unsigned long *)(buf + 32);
	*write_ops   = *(unsigned long *)(buf + 36);
	return 1;
}

/* IfType -> kind. Windows는 세분 분류가 없어 coarse(physical/loopback/tunnel/virtual)로만 태그. */
const char *win_net_kind(ULONG if_type)
{
	if (if_type == IF_TYPE_SOFTWARE_LOOPBACK) return "loopback";
	if (if_type == IF_TYPE_TUNNEL)            return "tunnel";
	if (if_type == IF_TYPE_ETHERNET_CSMACD || if_type == 71 /* IEEE80211 */)
		return "physical";
	return "virtual";
}

/* NT6+: MIB_IF_ROW2.HardwareInterface로 하드웨어 NIC 판정 — net_io와 같은 게이트를 태워 크로스메시지 kind 일치.
 * NT5.2는 이 플래그가 없어 1 반환(net_io 폴백과 대칭). GetIfEntry2는 GetProcAddress 해소(NT5.2 로드 가드). */
int iface_is_hardware(DWORD if_index)
{
	if (!agent_is_nt6()) return 1;
	typedef DWORD (WINAPI *GetIfEntry2_fn)(PMIB_IF_ROW2);
	static int resolved = 0;
	static GetIfEntry2_fn p_get2 = NULL;
	if (!resolved) {
		resolved = 1;
		HMODULE ip = GetModuleHandleA("iphlpapi.dll");
		if (ip) p_get2 = (GetIfEntry2_fn)(void *)GetProcAddress(ip, "GetIfEntry2");
	}
	if (!p_get2) return 1;
	MIB_IF_ROW2 row;
	memset(&row, 0, sizeof row);
	row.InterfaceIndex = if_index;
	if (p_get2(&row) != NO_ERROR) return 1;
	return row.InterfaceAndOperStatusFlags.HardwareInterface ? 1 : 0;
}

/* NT5.2: GAA가 gateway를 안 줘 IPv4 default route를 GetIpForwardTable(NT4+)에서 직접 찾는다(NT6+는 FirstGatewayAddress). */
int legacy_ipv4_gateway(DWORD if_index, char *out, size_t out_sz)
{
	ULONG sz = 0;
	if (GetIpForwardTable(NULL, &sz, FALSE) != ERROR_INSUFFICIENT_BUFFER)
		return 0;
	MIB_IPFORWARDTABLE *t = malloc(sz);
	if (!t)
		return 0;
	int found = 0;
	if (GetIpForwardTable(t, &sz, FALSE) == NO_ERROR) {
		for (DWORD i = 0; i < t->dwNumEntries; i++) {
			MIB_IPFORWARDROW *r = &t->table[i];
			if (r->dwForwardDest == 0 && r->dwForwardMask == 0 &&
			    r->dwForwardIfIndex == if_index) {
				struct in_addr a;
				a.S_un.S_addr = r->dwForwardNextHop;
				if (inet_ntop(AF_INET, &a, out, out_sz)) { found = 1; break; }
			}
		}
	}
	free(t);
	return found;
}

/* NT5.2 구형 GAA엔 OnLinkPrefixLength가 없어 IPv4 prefix를 GetIpAddrTable(NT4+) dwMask popcount로 실측(NT6+는 직접).
 * IPv6는 이 테이블에 없어 측정 불가 -> 호출측 null. addr_be/dwAddr은 둘 다 network byte order. */
int legacy_ipv4_prefix(DWORD if_index, unsigned addr_be, int *out_prefix)
{
	ULONG sz = 0;
	if (GetIpAddrTable(NULL, &sz, FALSE) != ERROR_INSUFFICIENT_BUFFER)
		return 0;
	MIB_IPADDRTABLE *t = malloc(sz);
	if (!t)
		return 0;
	int found = 0;
	if (GetIpAddrTable(t, &sz, FALSE) == NO_ERROR) {
		for (DWORD i = 0; i < t->dwNumEntries; i++) {
			MIB_IPADDRROW *r = &t->table[i];
			if (r->dwIndex == if_index && r->dwAddr == addr_be) {
				unsigned m = r->dwMask, n = 0;
				while (m) { n += m & 1u; m >>= 1; }
				*out_prefix = (int)n;
				found = 1;
				break;
			}
		}
	}
	free(t);
	return found;
}

/* NT5.2 GAA엔 TransmitLinkSpeed가 없어 GetIfTable(NT4+) MIB_IFROW.dwSpeed(bit/s)로 실측. NT6+는 직접 사용. */
int legacy_iface_speed(DWORD if_index, ULONG64 *out_bps)
{
	ULONG sz = 0;
	if (GetIfTable(NULL, &sz, FALSE) != ERROR_INSUFFICIENT_BUFFER)
		return 0;
	MIB_IFTABLE *t = malloc(sz);
	if (!t)
		return 0;
	int found = 0;
	if (GetIfTable(t, &sz, FALSE) == NO_ERROR) {
		for (DWORD i = 0; i < t->dwNumEntries; i++) {
			MIB_IFROW *r = &t->table[i];
			if (r->dwIndex == if_index && r->dwSpeed > 0) {
				*out_bps = r->dwSpeed;
				found = 1;
				break;
			}
		}
	}
	free(t);
	return found;
}

size_t http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct http_sink *s = (struct http_sink *)userdata;
	size_t n = size * nmemb;
	if (s->len + n > HTTP_GET_MAX_BYTES) n = HTTP_GET_MAX_BYTES - s->len;
	if (n == 0) return size * nmemb;
	if (s->len + n + 1 > s->cap) {
		size_t new_cap = s->len + n + 64;
		char *nb = (char *)realloc(s->buf, new_cap);
		if (!nb) return 0;
		s->buf = nb;
		s->cap = new_cap;
	}
	memcpy(s->buf + s->len, ptr, n);
	s->len += n;
	return size * nmemb;
}

char *http_get_short(const char *url, const char *header, int put_request)
{
	CURL *c = curl_easy_init();
	if (!c) return NULL;
	struct curl_slist *headers = NULL;
	if (header) headers = curl_slist_append(headers, header);
	struct http_sink s = { NULL, 0, 0 };

	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 1L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 1L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http");
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, http_write_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
	if (headers) curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	if (put_request) curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PUT");

	CURLcode cc = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	if (headers) curl_slist_free_all(headers);
	curl_easy_cleanup(c);

	if (cc != CURLE_OK || code != 200 || s.len == 0) {
		free(s.buf);
		return NULL;
	}
	char *out = (char *)realloc(s.buf, s.len + 1);
	if (!out) { free(s.buf); return NULL; }
	out[s.len] = '\0';

	size_t l = strlen(out);
	while (l > 0 && (out[l-1] == '\n' || out[l-1] == '\r' || out[l-1] == ' ' || out[l-1] == '\t')) out[--l] = '\0';
	if (l == 0) { free(out); return NULL; }
	return out;
}

/* AWS(IMDSv2)->Azure->GCP 순차 fallback으로 IMDS metadata 하나를 페치(앞 provider가 주면 이후 스킵). 전부 실패 시 NULL. */
char *fetch_imds_chain(const char *aws_metadata_url,
                              const char *azure_url, const char *gcp_url)
{
	char *v = NULL;
	char *token = http_get_short(
		"http://169.254.169.254/latest/api/token",
		"X-aws-ec2-metadata-token-ttl-seconds: 60", 1);
	if (token && *token) {
		char hdr[256];
		snprintf(hdr, sizeof hdr, "X-aws-ec2-metadata-token: %s", token);
		v = http_get_short(aws_metadata_url, hdr, 0);
	}
	free(token);
	if (!v)
		v = http_get_short(azure_url, "Metadata: true", 0);
	if (!v)
		v = http_get_short(gcp_url, "Metadata-Flavor: Google", 0);
	return v;
}

/* 수집 주기(초). main.c와 동일 env(AGENT_INTERVAL_SEC, 기본 60)의 raw 값을 clamp 없이 보고 —
 * 상한 clamp하면 발행값이 실제 주기와 어긋나고, one-shot(interval<=0)은 0 발행 시 엔진 86400/interval에서 0 나누기라 60으로 보고. */
int agent_interval_sec(void)
{
	int n = getenv_int_or("AGENT_INTERVAL_SEC", 60);
	return n > 0 ? n : 60;
}

/* MAC 바이트 -> "mac:XX:.." device 안정키. 부재(가상 miniport)면 name: 폴백. */
void mac_to_devid(const unsigned char *mac, unsigned len, const char *fallback, char *out, size_t outsz)
{
	if (len >= 6 && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]))
		snprintf(out, outsz, "mac:%02X:%02X:%02X:%02X:%02X:%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
	else
		snprintf(out, outsz, "name:%s", fallback ? fallback : "unknown");
}

const char *win_id_type(const char *full)
{
	if (!strncmp(full, "gptid:", 6))    return "gptid";
	if (!strncmp(full, "mbrsig:", 7))   return "mbrsig";
	if (!strncmp(full, "serial:", 7))   return "serial";
	if (!strncmp(full, "volguid:", 8))  return "volguid";
	if (!strncmp(full, "partuuid:", 9)) return "partuuid";
	if (!strncmp(full, "wwid:", 5))     return "wwid";
	return "name";
}

const char *win_id_value(const char *full)
{
	const char *c = strchr(full, ':');
	return c ? c + 1 : full;
}

/* Windows 디스크 안정키(D4 카탈로그): GPT DiskId(gptid) -> MBR Signature(mbrsig) -> serial -> name. */
void win_disk_id(int i, char *out, size_t outsz)
{
	HANDLE h = open_physical_drive(i);
	if (h == INVALID_HANDLE_VALUE) { snprintf(out, outsz, "name:PhysicalDrive%d", i); return; }
	DWORD ret = 0; BYTE buf[2048];
	if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, buf, sizeof buf, &ret, NULL)) {
		DRIVE_LAYOUT_INFORMATION_EX *dl = (DRIVE_LAYOUT_INFORMATION_EX *)buf;
		if (dl->PartitionStyle == PARTITION_STYLE_GPT) {
			GUID g = dl->Gpt.DiskId;
			snprintf(out, outsz, "gptid:{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
			         (unsigned long)g.Data1, g.Data2, g.Data3,
			         g.Data4[0],g.Data4[1],g.Data4[2],g.Data4[3],g.Data4[4],g.Data4[5],g.Data4[6],g.Data4[7]);
			CloseHandle(h); return;
		} else if (dl->PartitionStyle == PARTITION_STYLE_MBR && dl->Mbr.Signature) {
			snprintf(out, outsz, "mbrsig:%lu", (unsigned long)dl->Mbr.Signature);
			CloseHandle(h); return;
		}
	}
	STORAGE_PROPERTY_QUERY q; memset(&q, 0, sizeof q); q.PropertyId = StorageDeviceProperty; q.QueryType = PropertyStandardQuery;
	BYTE sbuf[2048] = {0};   /* 제로초기화 + offset<ret 로 OOB/미기록영역 read 방지(win_read_disk_meta 미러) */
	if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, sbuf, sizeof sbuf, &ret, NULL)) {
		STORAGE_DEVICE_DESCRIPTOR *sd = (STORAGE_DEVICE_DESCRIPTOR *)sbuf;
		if (sd->SerialNumberOffset && sd->SerialNumberOffset < ret) {
			char *ser = (char *)sbuf + sd->SerialNumberOffset;
			while (*ser == ' ') ser++;
			char trimmed[128]; snprintf(trimmed, sizeof trimmed, "%s", ser);
			for (int k = (int)strlen(trimmed) - 1; k >= 0 && trimmed[k] == ' '; k--) trimmed[k] = '\0';
			if (trimmed[0]) { snprintf(out, outsz, "serial:%s", trimmed); CloseHandle(h); return; }
		}
	}
	CloseHandle(h);
	snprintf(out, outsz, "name:PhysicalDrive%d", i);
}
