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

#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0600
#define AGENT_NT6 1
#else
#define AGENT_NT6 0
#endif

#if !AGENT_NT6

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
#define inet_ntop compat_inet_ntop
#endif

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

static void add_common_metadata(cJSON *root, const char *msg_type,
                                const char *machine_id, const char *agent_version)
{
	cache_process_times();

	cJSON_AddStringToObject(root, "message_type", msg_type);
	/* machine_id는 원시 머신 식별자다. 구할 수 없으면 억지로 채우지 않고 null(측정 불가).
	 * 식별·라우팅은 agent_id로 하고, composite_id는 mac 기반으로 유니크가 유지된다. */
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

	char hostname[256] = "unknown";
	DWORD sz = (DWORD)sizeof hostname;
	GetComputerNameA(hostname, &sz);
	cJSON_AddStringToObject(root, "hostname", hostname);

	char msg_id[64];
	uuid_v4(msg_id, sizeof msg_id);
	cJSON_AddStringToObject(root, "message_id", msg_id);

	if (g_boot_time_iso[0]) cJSON_AddStringToObject(root, "boot_time", g_boot_time_iso);
	else                    cJSON_AddNullToObject(root, "boot_time");
	if (g_agent_started_iso[0]) cJSON_AddStringToObject(root, "agent_started_at", g_agent_started_iso);
	else                        cJSON_AddNullToObject(root, "agent_started_at");
}

static char *http_get_short(const char *url, const char *header, int put_request);
static char *try_cloud_instance_id(void);

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

static void os_version_info(char *display_out, size_t dsz, char *build_out, size_t bsz)
{
	display_out[0] = '\0';
	build_out[0]   = '\0';

	HKEY hKey;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
	    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
	    0, KEY_READ, &hKey) != ERROR_SUCCESS) return;

	DWORD sz;

	sz = (DWORD)dsz;
	if (RegQueryValueExA(hKey, "DisplayVersion", NULL, NULL,
	                     (LPBYTE)display_out, &sz) != ERROR_SUCCESS) {
		sz = (DWORD)dsz;
		RegQueryValueExA(hKey, "ReleaseId", NULL, NULL,
		                 (LPBYTE)display_out, &sz);
	}

	char cur_build[32] = {0};
	sz = sizeof cur_build;
	RegQueryValueExA(hKey, "CurrentBuildNumber", NULL, NULL,
	                 (LPBYTE)cur_build, &sz);

	DWORD ubr = 0;
	DWORD ubr_sz = sizeof ubr;
	DWORD ubr_type = 0;
	RegQueryValueExA(hKey, "UBR", NULL, &ubr_type,
	                 (LPBYTE)&ubr, &ubr_sz);

	if (cur_build[0]) {
		if (ubr_type == REG_DWORD)
			snprintf(build_out, bsz, "%s.%lu", cur_build, (unsigned long)ubr);
		else
			snprintf(build_out, bsz, "%s", cur_build);
	}
	RegCloseKey(hKey);

	/* DisplayVersion/ReleaseId는 Windows 10/Server 2016+ 전용 레지스트리다. 없는 구버전
	 * (2012R2=6.3, 2003=5.2 등)은 RtlGetVersion(ntdll)의 NT major.minor로 채운다 —
	 * manifest 영향으로 부정확한 GetVersionEx와 달리 실제 버전을 준다. */
	if (display_out[0] == '\0') {
		HMODULE nt = GetModuleHandleW(L"ntdll.dll");
		typedef LONG (WINAPI *RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
		RtlGetVersionFn rgv = nt ? (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
		if (rgv) {
			RTL_OSVERSIONINFOW vi;
			vi.dwOSVersionInfoSize = sizeof vi;
			if (rgv(&vi) == 0)
				snprintf(display_out, dsz, "%lu.%lu",
				         (unsigned long)vi.dwMajorVersion, (unsigned long)vi.dwMinorVersion);
		}
	}
}

static void cpu_model_string(char *out, size_t len)
{
	int regs[4] = {0};
	__cpuid(regs, 0x80000000);
	if ((unsigned)regs[0] < 0x80000004) {
		snprintf(out, len, "Unknown");
		return;
	}
	char brand[49] = {0};
	__cpuid((int *)&brand[0],  0x80000002);
	__cpuid((int *)&brand[16], 0x80000003);
	__cpuid((int *)&brand[32], 0x80000004);
	brand[48] = '\0';
	const char *p = brand;
	while (*p == ' ') p++;
	snprintf(out, len, "%s", p);
}

/* 진짜 free(완전 미사용 물리메모리) kb를 perflib에서 실측. 정의는 perf 헬퍼 뒤.
 * 실패/미지원(NT5.2 등)이면 -1 -> 호출측이 null 발행. */
static long long query_free_kb(void);

static void fill_memory_inventory(cJSON *out)
{
	MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
	if (!GlobalMemoryStatusEx(&ms)) {
		cJSON_AddNullToObject(out, "mem_total_kb");
		cJSON_AddNullToObject(out, "swap_total_kb");
		return;
	}
	long long mem_total_kb = (long long)(ms.ullTotalPhys / 1024ULL);

	long long swap_total_kb = (ms.ullTotalPageFile > ms.ullTotalPhys)
	    ? (long long)((ms.ullTotalPageFile - ms.ullTotalPhys) / 1024ULL)
	    : 0;
	cJSON_AddNumberToObject(out, "mem_total_kb",  (double)mem_total_kb);
	cJSON_AddNumberToObject(out, "swap_total_kb", (double)swap_total_kb);
}

static void fill_memory_metrics(cJSON *out)
{
	MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
	if (!GlobalMemoryStatusEx(&ms)) {
		cJSON_AddNullToObject(out, "mem_total_kb");
		cJSON_AddNullToObject(out, "mem_free_kb");
		cJSON_AddNullToObject(out, "mem_available_kb");
		cJSON_AddNullToObject(out, "mem_buffers_kb");
		cJSON_AddNullToObject(out, "mem_cached_kb");
		cJSON_AddNullToObject(out, "swap_total_kb");
		cJSON_AddNullToObject(out, "swap_free_kb");
		return;
	}
	long long mem_total = (long long)(ms.ullTotalPhys     / 1024ULL);
	long long mem_avail = (long long)(ms.ullAvailPhys     / 1024ULL);
	long long swap_total = (ms.ullTotalPageFile > ms.ullTotalPhys)
	    ? (long long)((ms.ullTotalPageFile - ms.ullTotalPhys) / 1024ULL)
	    : 0;
	long long swap_avail = (ms.ullAvailPageFile > ms.ullAvailPhys)
	    ? (long long)((ms.ullAvailPageFile - ms.ullAvailPhys) / 1024ULL)
	    : 0;
	/* canonical 불변식: swap_free <= swap_total. TotalPageFile-TotalPhys와
	 * AvailPageFile-AvailPhys는 서로 다른 뺄셈이라 상한 관계가 없어, 소스에서
	 * clamp해 엔진 defensive clamp가 정상 에이전트에선 발동하지 않게 한다. */
	if (swap_avail > swap_total) swap_avail = swap_total;

	cJSON_AddNumberToObject(out, "mem_total_kb",     (double)mem_total);
	/* 정석: mem_free=진짜 free(실측), mem_available=회수 가능분. Windows에서 둘은
	 * 다른 값이다. free는 perflib로 실측하고(못 얻으면 null), available은 AvailPhys. */
	long long free_kb = query_free_kb();
	if (free_kb >= 0) cJSON_AddNumberToObject(out, "mem_free_kb", (double)free_kb);
	else              cJSON_AddNullToObject  (out, "mem_free_kb");
	cJSON_AddNumberToObject(out, "mem_available_kb", (double)mem_avail);

	/* buffers/cached: Linux page cache 세분 개념. Windows에 등가 없음 -> null(측정 불가). */
	cJSON_AddNullToObject(out, "mem_buffers_kb");
	cJSON_AddNullToObject(out, "mem_cached_kb");
	cJSON_AddNumberToObject(out, "swap_total_kb",    (double)swap_total);
	cJSON_AddNumberToObject(out, "swap_free_kb",     (double)swap_avail);
}

static void fill_cpu_stat(cJSON *m)
{
	cJSON *cs = cJSON_AddObjectToObject(m, "cpu_stat");

	/* 정석 의미론: 값=실측, null=측정 불가. Windows GetSystemTimes는 idle/kernel/user만
	 * 준다. nice/iowait/irq/softirq/steal은 Windows에 개념 자체가 없어(측정 불가) null로
	 * 둔다 — 0으로 채우면 "실측 0(예: iowait 없음)"과 구분되지 않는다. */
	FILETIME idleTime, kernelTime, userTime;
	if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
		cJSON_AddNullToObject(cs, "user");
		cJSON_AddNullToObject(cs, "nice");
		cJSON_AddNullToObject(cs, "system");
		cJSON_AddNullToObject(cs, "idle");
		cJSON_AddNullToObject(cs, "iowait");
		cJSON_AddNullToObject(cs, "irq");
		cJSON_AddNullToObject(cs, "softirq");
		cJSON_AddNullToObject(cs, "steal");
		return;
	}
	ULARGE_INTEGER idle, kern, usr;
	idle.LowPart = idleTime.dwLowDateTime;   idle.HighPart = idleTime.dwHighDateTime;
	kern.LowPart = kernelTime.dwLowDateTime; kern.HighPart = kernelTime.dwHighDateTime;
	usr.LowPart  = userTime.dwLowDateTime;   usr.HighPart  = userTime.dwHighDateTime;

	ULONGLONG sys_only = (kern.QuadPart > idle.QuadPart) ? (kern.QuadPart - idle.QuadPart) : 0;

	cJSON_AddNumberToObject(cs, "user",    (double)(usr.QuadPart  / 100000ULL));
	cJSON_AddNullToObject  (cs, "nice");
	cJSON_AddNumberToObject(cs, "system",  (double)(sys_only       / 100000ULL));
	cJSON_AddNumberToObject(cs, "idle",    (double)(idle.QuadPart  / 100000ULL));
	cJSON_AddNullToObject  (cs, "iowait");
	cJSON_AddNullToObject  (cs, "irq");
	cJSON_AddNullToObject  (cs, "softirq");
	cJSON_AddNullToObject  (cs, "steal");
}

static cJSON *enumerate_physical_disks(void)
{
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < 32; i++) {
		wchar_t path[64];
		swprintf(path, 64, L"\\\\.\\PhysicalDrive%d", i);
		/* GENERIC_READ 필요: access=0 핸들엔 IOCTL_DISK_GET_LENGTH_INFO가 ACCESS_DENIED.
		 * 에이전트는 LocalSystem이라 물리 드라이브 read 권한 있음. */
		HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		                       NULL, OPEN_EXISTING, 0, NULL);
		if (h == INVALID_HANDLE_VALUE)
			break;
		GET_LENGTH_INFORMATION gli;
		DWORD ret = 0;
		if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
		                    &gli, sizeof gli, &ret, NULL)) {
			cJSON *o = cJSON_CreateObject();
			char name[32];
			snprintf(name, sizeof name, "PhysicalDrive%d", i);
			cJSON_AddStringToObject(o, "name", name);
			cJSON_AddNumberToObject(o, "major", 0);
			cJSON_AddNumberToObject(o, "minor", i);
			cJSON_AddNumberToObject(o, "size_bytes", (double)gli.Length.QuadPart);
			cJSON_AddStringToObject(o, "type", "disk");
			cJSON_AddStringToObject(o, "kind", "physical");
			cJSON_AddItemToArray(arr, o);
		}
		CloseHandle(h);
	}
	return arr;
}

/* disk_queue: 물리 디스크별 순간 큐 깊이를 IOCTL_DISK_PERFORMANCE.QueueDepth로 실측한다.
 * diskperf(디스크 성능 카운터)가 미부착이면(OpenStack virtio 등) IOCTL이
 * ERROR_INVALID_FUNCTION을 반환하므로 그 디스크는 측정 불가로 보고 배열에서 제외한다.
 * 빈 배열=미측정. 실측 idle은 queue=0. perflib "Current Disk Queue Length"는 diskperf
 * 미부착 시에도 raw 0을 반환해 측정 불가와 실측 0을 구분하지 못하므로 IOCTL을 쓴다. */
static cJSON *query_disk_queue(void)
{
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < 32; i++) {
		wchar_t path[64];
		swprintf(path, 64, L"\\\\.\\PhysicalDrive%d", i);
		HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		                       NULL, OPEN_EXISTING, 0, NULL);
		if (h == INVALID_HANDLE_VALUE)
			break;
		DISK_PERFORMANCE dp;
		DWORD ret = 0;
		if (DeviceIoControl(h, IOCTL_DISK_PERFORMANCE, NULL, 0, &dp, sizeof dp, &ret, NULL)) {
			cJSON *o = cJSON_CreateObject();
			char dev[32];
			snprintf(dev, sizeof dev, "PhysicalDrive%d", i);
			cJSON_AddStringToObject(o, "device", dev);
			cJSON_AddNumberToObject(o, "queue", (double)dp.QueueDepth);
			cJSON_AddItemToArray(arr, o);
		}
		CloseHandle(h);
	}
	return arr;
}

static int drive_letter_to_phys_num(wchar_t letter)
{
	wchar_t path[8];
	swprintf(path, 8, L"\\\\.\\%c:", (int)letter);
	HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                       NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) return -1;
	STORAGE_DEVICE_NUMBER sdn;
	DWORD ret = 0;
	int n = -1;
	if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
	                    &sdn, sizeof sdn, &ret, NULL)) {
		n = (int)sdn.DeviceNumber;
	}
	CloseHandle(h);
	return n;
}

static cJSON *enumerate_mounts(int include_fstype)
{
	cJSON *arr = cJSON_CreateArray();
	wchar_t drives[256];
	DWORD len = GetLogicalDriveStringsW(256, drives);
	if (len == 0 || len > 256) return arr;

	wchar_t *p = drives;
	while (*p) {
		if (GetDriveTypeW(p) == DRIVE_FIXED) {
			ULARGE_INTEGER avail_to_caller, total, total_free;
			if (GetDiskFreeSpaceExW(p, &avail_to_caller, &total, &total_free)) {
				cJSON *o = cJSON_CreateObject();
				char mount[16];
				WideCharToMultiByte(CP_UTF8, 0, p, -1, mount, sizeof mount, NULL, NULL);
				cJSON_AddStringToObject(o, "mount", mount);

				int dnum = drive_letter_to_phys_num(p[0]);
				cJSON_AddNumberToObject(o, "major", 0);
				cJSON_AddNumberToObject(o, "minor", dnum >= 0 ? dnum : 0);
				cJSON_AddNumberToObject(o, "total_bytes", (double)total.QuadPart);
				cJSON_AddNumberToObject(o, "free_bytes",  (double)total_free.QuadPart);
				cJSON_AddNumberToObject(o, "avail_bytes", (double)avail_to_caller.QuadPart);
				cJSON_AddStringToObject(o, "kind", "data");

				if (include_fstype) {
					wchar_t fs[16] = {0};
					if (GetVolumeInformationW(p, NULL, 0, NULL, NULL, NULL, fs, 16)) {
						char fstype[16];
						WideCharToMultiByte(CP_UTF8, 0, fs, -1, fstype, sizeof fstype, NULL, NULL);
						for (char *q = fstype; *q; q++)
							*q = (char)tolower((unsigned char)*q);
						cJSON_AddStringToObject(o, "fstype", fstype);
					} else {
						cJSON_AddNullToObject(o, "fstype");
					}
				}
				cJSON_AddItemToArray(arr, o);
			}
		}
		p += wcslen(p) + 1;
	}
	return arr;
}

/* ---- perflib (HKEY_PERFORMANCE_DATA) — 언어독립 raw 카운터 ----
 * OpenStack virtio 디스크는 IOCTL_DISK_PERFORMANCE 미지원(ERROR_INVALID_FUNCTION). perflib은
 * 파티션/볼륨 매니저가 유지하는 카운터라 디스크 드라이버 IOCTL과 무관하게 동작한다.
 * 카운터/객체 인덱스는 OS 버전별로 달라 하드코딩하지 않고 Perflib\009(항상 영어)에서 이름->인덱스 조회. */

static DWORD perf_index(const char *want)
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
static BYTE *perf_query(const char *value_name)
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

static PERF_OBJECT_TYPE *perf_object(PERF_DATA_BLOCK *db, DWORD idx)
{
	if (!idx) return NULL;
	PERF_OBJECT_TYPE *o = (PERF_OBJECT_TYPE *)((BYTE *)db + db->HeaderLength);
	for (DWORD i = 0; i < db->NumObjectTypes; i++) {
		if (o->ObjectNameTitleIndex == idx) return o;
		o = (PERF_OBJECT_TYPE *)((BYTE *)o + o->TotalByteLength);
	}
	return NULL;
}

static PERF_COUNTER_DEFINITION *perf_counter(PERF_OBJECT_TYPE *o, DWORD idx)
{
	if (!o || !idx) return NULL;
	PERF_COUNTER_DEFINITION *c = (PERF_COUNTER_DEFINITION *)((BYTE *)o + o->HeaderLength);
	for (DWORD i = 0; i < o->NumCounters; i++) {
		if (c->CounterNameTitleIndex == idx) return c;
		c = (PERF_COUNTER_DEFINITION *)((BYTE *)c + c->ByteLength);
	}
	return NULL;
}

static unsigned long long perf_read(PERF_COUNTER_BLOCK *cb, PERF_COUNTER_DEFINITION *c)
{
	if (!cb || !c) return 0;
	BYTE *p = (BYTE *)cb + c->CounterOffset;
	if (c->CounterSize >= 8) return *(unsigned long long *)p;
	return *(DWORD *)p;
}

/* PhysicalDisk 인스턴스명 "0 C:" -> 물리 드라이브 번호. 숫자 시작 아니면(-1) 스킵(_Total 등). */
static int perf_disk_num(const wchar_t *name)
{
	if (name[0] < L'0' || name[0] > L'9') return -1;
	return (int)wcstol(name, NULL, 10);
}

/* Memory\"Free & Zero Page List Bytes"(NT6.0+, 단일 인스턴스 오브젝트)로 진짜 free를
 * 실측한다. GlobalMemoryStatusEx.ullAvailPhys(=available, 회수 가능분 포함)와 다르다.
 * 이 카운터가 없는 NT5.2나 조회 실패 시 -1(호출측이 null 발행). */
static long long query_free_kb(void)
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

/* NtQuerySystemInformation(SystemPerformanceInformation=2): I/O 매니저가 직접 유지하는
 * 시스템 전역 누적 I/O 카운터. PhysicalDisk/LogicalDisk perflib·IOCTL_DISK_PERFORMANCE가
 * diskperf 성능 통계에 의존하는 반면 이 카운터는 provider 독립이라, virtio 등에서 diskperf가
 * 미수집(카운터 raw=0)인 환경의 disk_io 폴백 소스로 쓴다. 성공 시 1, 실패 시 0. */
static int query_system_io(unsigned long long *read_ops, unsigned long long *write_ops,
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
	/* SYSTEM_PERFORMANCE_INFORMATION 안정 prefix: +8 IoReadTransferCount(8),
	 * +16 IoWriteTransferCount(8), +32 IoReadOperationCount(4), +36 IoWriteOperationCount(4). */
	*read_bytes  = *(unsigned long long *)(buf + 8);
	*write_bytes = *(unsigned long long *)(buf + 16);
	*read_ops    = *(unsigned long *)(buf + 32);
	*write_ops   = *(unsigned long *)(buf + 36);
	return 1;
}

/* disk_io 폴백: perflib PhysicalDisk 객체(인스턴스=물리 드라이브)에서 누적 read/write count·bytes.
 * NtQuerySystemInformation을 못 쓸 때만 사용. perflib 디스크 카운터는 diskperf 성능 통계 수집에
 * 의존해 환경에 따라 죽거나(2012R2+virtio: raw=0) 비단조(win2003/win2016: 부팅후 리셋)라 2차 소스다. */
static void disk_io_perflib(cJSON *arr)
{
	DWORD i_pd = perf_index("PhysicalDisk");
	if (!i_pd) return;
	char vn[16];
	snprintf(vn, sizeof vn, "%lu", (unsigned long)i_pd);
	BYTE *buf = perf_query(vn);
	RegCloseKey(HKEY_PERFORMANCE_DATA);
	if (!buf) return;
	PERF_DATA_BLOCK *db = (PERF_DATA_BLOCK *)buf;
	PERF_OBJECT_TYPE *o = perf_object(db, i_pd);
	if (o && o->NumInstances > 0) {
		PERF_COUNTER_DEFINITION *c_r  = perf_counter(o, perf_index("Disk Reads/sec"));
		PERF_COUNTER_DEFINITION *c_w  = perf_counter(o, perf_index("Disk Writes/sec"));
		PERF_COUNTER_DEFINITION *c_rb = perf_counter(o, perf_index("Disk Read Bytes/sec"));
		PERF_COUNTER_DEFINITION *c_wb = perf_counter(o, perf_index("Disk Write Bytes/sec"));
		PERF_INSTANCE_DEFINITION *inst = (PERF_INSTANCE_DEFINITION *)((BYTE *)o + o->DefinitionLength);
		for (LONG i = 0; i < o->NumInstances; i++) {
			wchar_t *wname = (wchar_t *)((BYTE *)inst + inst->NameOffset);
			PERF_COUNTER_BLOCK *cb = (PERF_COUNTER_BLOCK *)((BYTE *)inst + inst->ByteLength);
			int drive = perf_disk_num(wname);
			if (drive >= 0) {
				char dev[32];
				snprintf(dev, sizeof dev, "PhysicalDrive%d", drive);
				cJSON *e = cJSON_CreateObject();
				cJSON_AddStringToObject(e, "device", dev);
				cJSON_AddNumberToObject(e, "major", 0);
				cJSON_AddNumberToObject(e, "minor", drive);
				cJSON_AddNumberToObject(e, "reads_completed",  (double)perf_read(cb, c_r));
				cJSON_AddNumberToObject(e, "writes_completed", (double)perf_read(cb, c_w));
				cJSON_AddNumberToObject(e, "sectors_read",     (double)(perf_read(cb, c_rb) / 512));
				cJSON_AddNumberToObject(e, "sectors_written",  (double)(perf_read(cb, c_wb) / 512));
				cJSON_AddStringToObject(e, "kind", "physical");
				cJSON_AddItemToArray(arr, e);
			}
			inst = (PERF_INSTANCE_DEFINITION *)((BYTE *)cb + cb->ByteLength);
		}
	}
	free(buf);
}

/* disk_io: 1차 소스는 NtQuerySystemInformation(SystemPerformanceInformation)의 시스템 전역 누적 I/O.
 * I/O 매니저가 직접 유지해 단조증가·provider 독립(NT5.2~NT10)이라 perflib 디스크 카운터의
 * 환경별 불안정(2012R2 raw=0, win2003/win2016 부팅후 리셋)을 회피한다. 시스템 전역 집계라
 * 단일 엔트리(PhysicalDrive0)로 보고한다. NtQuery 불가 시에만 perflib per-disk로 폴백. */
static cJSON *enumerate_disk_io(void)
{
	cJSON *arr = cJSON_CreateArray();
	unsigned long long rop = 0, wop = 0, rby = 0, wby = 0;
	if (query_system_io(&rop, &wop, &rby, &wby)) {
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "device", "PhysicalDrive0");
		cJSON_AddNumberToObject(e, "major", 0);
		cJSON_AddNumberToObject(e, "minor", 0);
		cJSON_AddNumberToObject(e, "reads_completed",  (double)rop);
		cJSON_AddNumberToObject(e, "writes_completed", (double)wop);
		cJSON_AddNumberToObject(e, "sectors_read",     (double)(rby / 512));
		cJSON_AddNumberToObject(e, "sectors_written",  (double)(wby / 512));
		cJSON_AddStringToObject(e, "kind", "physical");
		cJSON_AddItemToArray(arr, e);
		return arr;
	}
	disk_io_perflib(arr);
	return arr;
}

/* IfType -> kind. Windows는 세분 분류(bridge/veth/bond)가 안 나와
 * coarse(physical/loopback/tunnel/virtual)로만 태그한다 — 엔진이 수용. */
static const char *win_net_kind(ULONG if_type)
{
	if (if_type == IF_TYPE_SOFTWARE_LOOPBACK) return "loopback";
	if (if_type == IF_TYPE_TUNNEL)            return "tunnel";
	if (if_type == IF_TYPE_ETHERNET_CSMACD || if_type == 71 /* IEEE80211 */)
		return "physical";
	return "virtual";
}

#if AGENT_NT6
static cJSON *enumerate_net_io(void)
{
	cJSON *arr = cJSON_CreateArray();
	PMIB_IF_TABLE2 table = NULL;
	if (GetIfTable2(&table) != NO_ERROR || !table) return arr;
	for (ULONG i = 0; i < table->NumEntries; i++) {
		MIB_IF_ROW2 *r = &table->Table[i];
		if (r->Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		if (r->OperStatus != IfOperStatusUp) continue;
		/* WFP/QoS 필터 드라이버는 기반 NIC의 카운터를 그대로 복제한다 -> 스킵(중복 합산 방지). */
		if (r->InterfaceAndOperStatusFlags.FilterInterface) continue;

		char name[256];
		WideCharToMultiByte(CP_UTF8, 0, r->Alias, -1, name, sizeof name, NULL, NULL);

		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "interface", name);
		cJSON_AddNumberToObject(o, "rx_bytes",   (double)r->InOctets);
		cJSON_AddNumberToObject(o, "tx_bytes",   (double)r->OutOctets);
		cJSON_AddNumberToObject(o, "rx_packets", (double)(r->InUcastPkts + r->InNUcastPkts));
		cJSON_AddNumberToObject(o, "tx_packets", (double)(r->OutUcastPkts + r->OutNUcastPkts));
		cJSON_AddNumberToObject(o, "rx_errors",  (double)r->InErrors);
		cJSON_AddNumberToObject(o, "tx_errors",  (double)r->OutErrors);
		/* 하드웨어 NIC만 win_net_kind(physical 가능). 비-하드웨어(가상/miniport)는 virtual. */
		cJSON_AddStringToObject(o, "kind",
		    r->InterfaceAndOperStatusFlags.HardwareInterface ? win_net_kind(r->Type) : "virtual");
		cJSON_AddItemToArray(arr, o);
	}
	FreeMibTable(table);
	return arr;
}
#else

static cJSON *enumerate_net_io(void)
{
	cJSON *arr = cJSON_CreateArray();
	ULONG sz = 0;
	if (GetIfTable(NULL, &sz, FALSE) != ERROR_INSUFFICIENT_BUFFER) return arr;
	MIB_IFTABLE *t = (MIB_IFTABLE *)malloc(sz);
	if (!t) return arr;
	if (GetIfTable(t, &sz, FALSE) != NO_ERROR) { free(t); return arr; }
	for (DWORD i = 0; i < t->dwNumEntries; i++) {
		MIB_IFROW *r = &t->table[i];
		if (r->dwType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

		char name[256];
		WideCharToMultiByte(CP_UTF8, 0, r->wszName, -1, name, sizeof name, NULL, NULL);

		if (name[0] == '\0') {
			if (r->dwDescrLen > 0 && r->bDescr[0])
				snprintf(name, sizeof name, "%.*s", (int)r->dwDescrLen, (char *)r->bDescr);
			else
				snprintf(name, sizeof name, "if%lu", (unsigned long)r->dwIndex);
		}

		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "interface", name);
		cJSON_AddNumberToObject(o, "rx_bytes",   (double)r->dwInOctets);
		cJSON_AddNumberToObject(o, "tx_bytes",   (double)r->dwOutOctets);
		cJSON_AddNumberToObject(o, "rx_packets", (double)((double)r->dwInUcastPkts + r->dwInNUcastPkts));
		cJSON_AddNumberToObject(o, "tx_packets", (double)((double)r->dwOutUcastPkts + r->dwOutNUcastPkts));
		cJSON_AddNumberToObject(o, "rx_errors",  (double)r->dwInErrors);
		cJSON_AddNumberToObject(o, "tx_errors",  (double)r->dwOutErrors);
		cJSON_AddStringToObject(o, "kind", win_net_kind(r->dwType));
		cJSON_AddItemToArray(arr, o);
	}
	free(t);
	return arr;
}
#endif

static int mac_cmp(const void *a, const void *b)
{
	return strcmp(*(const char **)a, *(const char **)b);
}

#if !AGENT_NT6
/* NT5.2: GAA가 gateway를 안 줘, IfIndex의 IPv4 default route gateway를
 * GetIpForwardTable(NT4+)에서 직접 찾는다. NT6+는 FirstGatewayAddress를 쓴다. */
static int legacy_ipv4_gateway(DWORD if_index, char *out, size_t out_sz)
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
#endif

static void fill_network_info(cJSON *inv)
{
	cJSON *ifaces = cJSON_AddArrayToObject(inv, "interfaces");
	cJSON *macs   = cJSON_AddArrayToObject(inv, "mac_addresses");

	/* gateway = 이 주소와 같은 family의 default route. NT6+는 GAA_FLAG_INCLUDE_GATEWAYS의
	 * FirstGatewayAddress, NT5.2는 GetIpForwardTable(IPv4 default route)로 얻는다. */
	ULONG gaa_flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
#if AGENT_NT6
	gaa_flags |= GAA_FLAG_INCLUDE_GATEWAYS;
#endif

	ULONG buf_len = 16 * 1024;
	IP_ADAPTER_ADDRESSES *aa = malloc(buf_len);
	if (!aa) return;

	ULONG ret = GetAdaptersAddresses(AF_UNSPEC, gaa_flags, NULL, aa, &buf_len);
	if (ret == ERROR_BUFFER_OVERFLOW) {
		free(aa);
		aa = malloc(buf_len);
		if (!aa) return;
		ret = GetAdaptersAddresses(AF_UNSPEC, gaa_flags, NULL, aa, &buf_len);
	}
	if (ret != NO_ERROR) { free(aa); return; }

	enum { MAC_CAP = 64 };
	char *mac_list[MAC_CAP];
	int mac_count = 0;

	for (IP_ADAPTER_ADDRESSES *p = aa; p; p = p->Next) {
		if (p->OperStatus != IfOperStatusUp) continue;
		if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		if (p->IfType == IF_TYPE_TUNNEL) continue;

		for (IP_ADAPTER_UNICAST_ADDRESS *u = p->FirstUnicastAddress; u; u = u->Next) {
			char ip[INET6_ADDRSTRLEN] = {0};
			const char *family = NULL;
			if (u->Address.lpSockaddr->sa_family == AF_INET) {
				struct sockaddr_in *sa = (struct sockaddr_in *)u->Address.lpSockaddr;
				inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip);
				family = "ipv4";
			} else if (u->Address.lpSockaddr->sa_family == AF_INET6) {
				struct sockaddr_in6 *sa = (struct sockaddr_in6 *)u->Address.lpSockaddr;
				inet_ntop(AF_INET6, &sa->sin6_addr, ip, sizeof ip);
				family = "ipv6";
			}
			if (!ip[0]) continue;
			unsigned prefix = 0;
#if AGENT_NT6
			prefix = (unsigned)u->OnLinkPrefixLength;
#endif

			/* 구조화 interfaces[] (name/address/prefix/family/kind).
			 * cutover로 구형 ip_internal 대체(단독 발행). */
			char ifname[256];
			WideCharToMultiByte(CP_UTF8, 0, p->FriendlyName, -1,
			                    ifname, sizeof ifname, NULL, NULL);
			cJSON *io = cJSON_CreateObject();
			cJSON_AddStringToObject(io, "name",    ifname);
			cJSON_AddStringToObject(io, "address", ip);
			cJSON_AddNumberToObject(io, "prefix",  (double)prefix);
			cJSON_AddStringToObject(io, "family",  family ? family : "");
			cJSON_AddStringToObject(io, "kind",    win_net_kind(p->IfType));

			/* gateway = 이 주소와 같은 family의 default route. NT6+는 FirstGatewayAddress,
			 * NT5.2는 GetIpForwardTable(IPv4 default route). IPv6 항목은 null. */
			cJSON *gw_item = NULL;
#if AGENT_NT6
			int ufam = u->Address.lpSockaddr->sa_family;
			for (IP_ADAPTER_GATEWAY_ADDRESS_LH *g = p->FirstGatewayAddress; g; g = g->Next) {
				if (!g->Address.lpSockaddr || g->Address.lpSockaddr->sa_family != ufam)
					continue;
				char gbuf[INET6_ADDRSTRLEN] = {0};
				if (ufam == AF_INET)
					inet_ntop(AF_INET, &((struct sockaddr_in *)g->Address.lpSockaddr)->sin_addr, gbuf, sizeof gbuf);
				else if (ufam == AF_INET6)
					inet_ntop(AF_INET6, &((struct sockaddr_in6 *)g->Address.lpSockaddr)->sin6_addr, gbuf, sizeof gbuf);
				if (gbuf[0]) { gw_item = cJSON_CreateString(gbuf); break; }
			}
#else
			if (u->Address.lpSockaddr->sa_family == AF_INET) {
				char gbuf[INET_ADDRSTRLEN];
				if (legacy_ipv4_gateway(p->IfIndex, gbuf, sizeof gbuf))
					gw_item = cJSON_CreateString(gbuf);
			}
#endif
			cJSON_AddItemToObject(io, "gateway", gw_item ? gw_item : cJSON_CreateNull());
			cJSON_AddItemToArray(ifaces, io);
		}

		if (p->PhysicalAddressLength == 6 && mac_count < MAC_CAP) {
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
	}

	qsort(mac_list, mac_count, sizeof(char *), mac_cmp);
	for (int i = 0; i < mac_count; i++) {
		cJSON_AddItemToArray(macs, cJSON_CreateString(mac_list[i]));
		free(mac_list[i]);
	}
	free(aa);
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
	if (aa && ret == NO_ERROR) {
		for (IP_ADAPTER_ADDRESSES *p = aa; p; p = p->Next) {
			if (p->OperStatus != IfOperStatusUp)        continue;
			if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
			if (p->IfType == IF_TYPE_TUNNEL)            continue;
			if (p->PhysicalAddressLength != 6)          continue;
			if (mac_count >= MAC_CAP)                   break;

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
	}
	free(aa);
	qsort(mac_list, mac_count, sizeof(char *), mac_cmp);

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

/* 첫 실행 시 UUIDv4 생성 -> %ProgramData%\assessment-agent\agent-id에
 * 영구 저장 -> 재사용. LocalSystem 서비스가 항상 읽고 쓸 수 있는 고정 경로다.
 * prep-image가 이 파일을 지워 클론마다 새로 생성되게 한다. */
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
	f = fopen(path, "w");
	if (f) { fprintf(f, "%s\n", id_buf); fclose(f); }
	return id_buf;
}

struct http_sink {
	char  *buf;
	size_t len;
	size_t cap;
};

#define HTTP_GET_MAX_BYTES 256

static size_t http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
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

static char *http_get_short(const char *url, const char *header, int put_request)
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

static cJSON *collect_external_ip(void)
{
	const char *override = getenv("AGENT_EXTERNAL_IP");
	if (override && *override) {
		cJSON *arr = cJSON_CreateArray();
		char *copy = _strdup(override);
		if (!copy) return arr;
		char *save = NULL;
		for (char *tok = strtok_s(copy, ",", &save); tok; tok = strtok_s(NULL, ",", &save)) {
			while (*tok == ' ' || *tok == '\t') tok++;
			size_t l = strlen(tok);
			while (l > 0 && (tok[l-1] == ' ' || tok[l-1] == '\t' || tok[l-1] == '\r' || tok[l-1] == '\n')) tok[--l] = '\0';
			if (*tok) cJSON_AddItemToArray(arr, cJSON_CreateString(tok));
		}
		free(copy);
		return arr;
	}

	char *ip = NULL;

	char *token = http_get_short(
		"http://169.254.169.254/latest/api/token",
		"X-aws-ec2-metadata-token-ttl-seconds: 60",
		1 );
	if (token && *token) {
		char hdr[256];
		snprintf(hdr, sizeof hdr, "X-aws-ec2-metadata-token: %s", token);
		ip = http_get_short("http://169.254.169.254/latest/meta-data/public-ipv4", hdr, 0);
	}
	free(token);

	if (!ip) {
		ip = http_get_short(
			"http://169.254.169.254/metadata/instance/network/interface/0/"
			"ipv4/ipAddress/0/publicIpAddress?api-version=2021-02-01&format=text",
			"Metadata: true", 0);
	}
	if (!ip) {
		ip = http_get_short(
			"http://metadata.google.internal/computeMetadata/v1/instance/"
			"network-interfaces/0/access-configs/0/external-ip",
			"Metadata-Flavor: Google", 0);
	}

	if (!ip || !*ip) {
		free(ip);
		return cJSON_CreateNull();
	}
	cJSON *arr = cJSON_CreateArray();
	cJSON_AddItemToArray(arr, cJSON_CreateString(ip));
	free(ip);
	return arr;
}

static char *try_cloud_instance_id(void)
{
	char *id = NULL;

	char *token = http_get_short(
		"http://169.254.169.254/latest/api/token",
		"X-aws-ec2-metadata-token-ttl-seconds: 60",
		1 );
	if (token && *token) {
		char hdr[256];
		snprintf(hdr, sizeof hdr, "X-aws-ec2-metadata-token: %s", token);
		id = http_get_short("http://169.254.169.254/latest/meta-data/instance-id", hdr, 0);
	}
	free(token);

	if (!id) {
		id = http_get_short(
			"http://169.254.169.254/metadata/instance/compute/vmId?api-version=2021-02-01&format=text",
			"Metadata: true", 0);
	}
	if (!id) {
		id = http_get_short(
			"http://metadata.google.internal/computeMetadata/v1/instance/id",
			"Metadata-Flavor: Google", 0);
	}

	if (id) fprintf(stderr, "[agent] cloud IMDS instance-id fallback succeeded\n");
	return id;
}

static void fill_comm_for_pid(DWORD pid, char *out, size_t out_sz);

static cJSON *enumerate_running_services(void)
{
	SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
	if (!scm) return cJSON_CreateNull();

	cJSON *arr = cJSON_CreateArray();
	DWORD bytes_needed = 0, services_returned = 0, resume = 0;

	EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE,
	                      NULL, 0, &bytes_needed, &services_returned, &resume, NULL);
	if (bytes_needed == 0) { CloseServiceHandle(scm); return arr; }

	BYTE *buf = malloc(bytes_needed);
	if (!buf) { CloseServiceHandle(scm); return arr; }

	if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE,
	                          buf, bytes_needed, &bytes_needed, &services_returned,
	                          &resume, NULL)) {
		ENUM_SERVICE_STATUS_PROCESSW *es = (ENUM_SERVICE_STATUS_PROCESSW *)buf;
		for (DWORD i = 0; i < services_returned; i++) {
			if (es[i].ServiceStatusProcess.dwCurrentState != SERVICE_RUNNING) continue;
			char name[256];
			WideCharToMultiByte(CP_UTF8, 0, es[i].lpServiceName, -1,
			                    name, sizeof name, NULL, NULL);
			cJSON *o = cJSON_CreateObject();
			cJSON_AddStringToObject(o, "unit", name);
			cJSON_AddStringToObject(o, "sub",  "running");
			DWORD spid = es[i].ServiceStatusProcess.dwProcessId;
			if (spid) cJSON_AddNumberToObject(o, "pid", (double)spid);
			else      cJSON_AddNullToObject  (o, "pid");
			char exe[256];
			fill_comm_for_pid(spid, exe, sizeof exe);
			if (exe[0]) cJSON_AddStringToObject(o, "exe", exe);
			else        cJSON_AddNullToObject  (o, "exe");
			cJSON_AddItemToArray(arr, o);
		}
	}
	free(buf);
	CloseServiceHandle(scm);
	return arr;
}

/* Toolhelp32(kernel32)로 pid -> exe 베이스명. OpenProcess가 필요 없어 System(4) 등
 * 보호 프로세스도 커버한다(커널이 스냅샷에 이름을 제공). */
static void fill_comm_toolhelp(DWORD pid, char *out, size_t out_sz)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return;
	PROCESSENTRY32W pe;
	pe.dwSize = sizeof pe;
	if (Process32FirstW(snap, &pe)) {
		do {
			if (pe.th32ProcessID == pid) {
				WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, out, (int)out_sz, NULL, NULL);
				char *dot = strrchr(out, '.');
				if (dot && _stricmp(dot, ".exe") == 0) *dot = '\0';
				break;
			}
		} while (Process32NextW(snap, &pe));
	}
	CloseHandle(snap);
}

static void fill_comm_for_pid(DWORD pid, char *out, size_t out_sz)
{
	out[0] = '\0';
	if (pid == 0) return;
#if AGENT_NT6
	/* 1차: QueryFullProcessImageName(전체 경로 -> 베이스명). System(4) 등 보호 프로세스는
	 * OpenProcess가 거부돼 결과가 비므로 2차로 Toolhelp32 폴백. */
	HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (hp) {
		wchar_t wname[1024];
		DWORD wsz = 1024;
		if (QueryFullProcessImageNameW(hp, 0, wname, &wsz)) {
			wchar_t *base = wcsrchr(wname, L'\\');
			base = base ? base + 1 : wname;
			WideCharToMultiByte(CP_UTF8, 0, base, -1, out, (int)out_sz, NULL, NULL);
			char *dot = strrchr(out, '.');
			if (dot && _stricmp(dot, ".exe") == 0) *dot = '\0';
		}
		CloseHandle(hp);
	}
	if (!out[0])
		fill_comm_toolhelp(pid, out, out_sz);
#else
	/* NT5.2(2003/XP): QueryFullProcessImageName이 없어 Toolhelp32만 쓴다. */
	fill_comm_toolhelp(pid, out, out_sz);
#endif
}

static void add_listen_entry(cJSON *arr, const char *proto, const char *addr,
                             unsigned short port, DWORD pid)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "proto", proto);
	cJSON_AddStringToObject(o, "addr",  addr);
	cJSON_AddNumberToObject(o, "port",  port);
	cJSON_AddNullToObject  (o, "uid");
	cJSON_AddNumberToObject(o, "pid",   (double)pid);
	char comm[256];
	fill_comm_for_pid(pid, comm, sizeof comm);
	if (comm[0]) cJSON_AddStringToObject(o, "comm", comm);
	else         cJSON_AddNullToObject  (o, "comm");
	cJSON_AddItemToArray(arr, o);
}

static void collect_tcp4_listen(cJSON *arr)
{
	DWORD size = 0;
	if (GetExtendedTcpTable(NULL, &size, FALSE, AF_INET,
	                        TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER) return;
	MIB_TCPTABLE_OWNER_PID *t = malloc(size);
	if (!t) return;
	if (GetExtendedTcpTable(t, &size, FALSE, AF_INET,
	                        TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
		for (DWORD i = 0; i < t->dwNumEntries; i++) {
			struct in_addr a;
			a.S_un.S_addr = t->table[i].dwLocalAddr;
			char ip[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &a, ip, sizeof ip);
			unsigned short port = ntohs((u_short)t->table[i].dwLocalPort);
			add_listen_entry(arr, "tcp", ip, port, t->table[i].dwOwningPid);
		}
	}
	free(t);
}

static void collect_tcp6_listen(cJSON *arr)
{
	DWORD size = 0;
	if (GetExtendedTcpTable(NULL, &size, FALSE, AF_INET6,
	                        TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER) return;
	MIB_TCP6TABLE_OWNER_PID *t = malloc(size);
	if (!t) return;
	if (GetExtendedTcpTable(t, &size, FALSE, AF_INET6,
	                        TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
		for (DWORD i = 0; i < t->dwNumEntries; i++) {
			struct in6_addr a;
			memcpy(&a, t->table[i].ucLocalAddr, sizeof a);
			char ip[INET6_ADDRSTRLEN];
			inet_ntop(AF_INET6, &a, ip, sizeof ip);
			unsigned short port = ntohs((u_short)t->table[i].dwLocalPort);
			add_listen_entry(arr, "tcp6", ip, port, t->table[i].dwOwningPid);
		}
	}
	free(t);
}

static void collect_udp4_listen(cJSON *arr)
{
	DWORD size = 0;
	if (GetExtendedUdpTable(NULL, &size, FALSE, AF_INET,
	                        UDP_TABLE_OWNER_PID, 0) != ERROR_INSUFFICIENT_BUFFER) return;
	MIB_UDPTABLE_OWNER_PID *t = malloc(size);
	if (!t) return;
	if (GetExtendedUdpTable(t, &size, FALSE, AF_INET,
	                        UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
		for (DWORD i = 0; i < t->dwNumEntries; i++) {
			struct in_addr a;
			a.S_un.S_addr = t->table[i].dwLocalAddr;
			char ip[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &a, ip, sizeof ip);
			unsigned short port = ntohs((u_short)t->table[i].dwLocalPort);
			add_listen_entry(arr, "udp", ip, port, t->table[i].dwOwningPid);
		}
	}
	free(t);
}

static void collect_udp6_listen(cJSON *arr)
{
	DWORD size = 0;
	if (GetExtendedUdpTable(NULL, &size, FALSE, AF_INET6,
	                        UDP_TABLE_OWNER_PID, 0) != ERROR_INSUFFICIENT_BUFFER) return;
	MIB_UDP6TABLE_OWNER_PID *t = malloc(size);
	if (!t) return;
	if (GetExtendedUdpTable(t, &size, FALSE, AF_INET6,
	                        UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
		for (DWORD i = 0; i < t->dwNumEntries; i++) {
			struct in6_addr a;
			memcpy(&a, t->table[i].ucLocalAddr, sizeof a);
			char ip[INET6_ADDRSTRLEN];
			inet_ntop(AF_INET6, &a, ip, sizeof ip);
			unsigned short port = ntohs((u_short)t->table[i].dwLocalPort);
			add_listen_entry(arr, "udp6", ip, port, t->table[i].dwOwningPid);
		}
	}
	free(t);
}

static cJSON *enumerate_listen_ports(void)
{
	cJSON *arr = cJSON_CreateArray();
	collect_tcp4_listen(arr);
	collect_tcp6_listen(arr);
	collect_udp4_listen(arr);
	collect_udp6_listen(arr);
	return arr;
}

cJSON *build_error_payload(const char *machine_id, const char *agent_version,
                           const char *error_code, const char *error_message,
                           const char *failed_component, int retry_count,
                           const char *first_failed_at, const char *recovered_at)
{
	cJSON *m = cJSON_CreateObject();
	if (!m) return NULL;
	add_common_metadata(m, "error", machine_id, agent_version);
	cJSON_AddStringToObject(m, "error_code",       error_code       ? error_code       : "UNKNOWN");
	cJSON_AddStringToObject(m, "error_message",    error_message    ? error_message    : "");
	cJSON_AddStringToObject(m, "failed_component", failed_component ? failed_component : "agent");
	if (retry_count >= 0)   cJSON_AddNumberToObject(m, "retry_count", retry_count);
	if (first_failed_at)    cJSON_AddStringToObject(m, "first_failed_at", first_failed_at);
	if (recovered_at)       cJSON_AddStringToObject(m, "recovered_at",    recovered_at);
	return m;
}

cJSON *collect_inventory_payload(const char *machine_id, const char *agent_version)
{
	cJSON *m = cJSON_CreateObject();
	if (!m) return NULL;
	add_common_metadata(m, "inventory", machine_id, agent_version);

	cJSON_AddStringToObject(m, "os_id", "windows");

	char display[64] = {0}, build[64] = {0};
	os_version_info(display, sizeof display, build, sizeof build);
	cJSON_AddStringToObject(m, "os_version",    display[0] ? display : "");
	cJSON_AddNullToObject  (m, "os_codename");
	cJSON_AddStringToObject(m, "kernel_version", build[0] ? build : "");

	SYSTEM_INFO si;
	GetNativeSystemInfo(&si);
	cJSON_AddNumberToObject(m, "cpu_cores", (double)si.dwNumberOfProcessors);

	char cpu_brand[64];
	cpu_model_string(cpu_brand, sizeof cpu_brand);
	cJSON_AddStringToObject(m, "cpu_model", cpu_brand);

	fill_memory_inventory(m);

	cJSON_AddItemToObject(m, "disks",        enumerate_physical_disks());
	cJSON_AddItemToObject(m, "mounts",       enumerate_mounts(1));
	cJSON_AddItemToObject(m, "services",     enumerate_running_services());
	cJSON_AddItemToObject(m, "listen_ports", enumerate_listen_ports());

	fill_network_info(m);
	cJSON_AddItemToObject(m, "ip_external", collect_external_ip());

	return m;
}

static void fill_saturation(cJSON *m)
{
	cJSON *s = cJSON_CreateObject();

	/* cpu_run_queue(System\Processor Queue Length), mem_paging_rate(Memory\Pages/sec)는
	 * perflib. 카운터를 못 읽으면(측정 불가) null, 읽으면 값(idle이면 실측 0). */
	DWORD i_sys = perf_index("System");
	DWORD i_mem = perf_index("Memory");
	char vn[32];
	snprintf(vn, sizeof vn, "%lu %lu", (unsigned long)i_sys, (unsigned long)i_mem);
	BYTE *buf = perf_query(vn);
	RegCloseKey(HKEY_PERFORMANCE_DATA);

	int have_cpu = 0, have_mem = 0;
	unsigned long long cpu_q = 0, pages = 0;

	if (buf) {
		PERF_DATA_BLOCK *db = (PERF_DATA_BLOCK *)buf;

		PERF_OBJECT_TYPE *osys = perf_object(db, i_sys);
		if (osys && osys->NumInstances == PERF_NO_INSTANCES) {
			PERF_COUNTER_BLOCK *cb = (PERF_COUNTER_BLOCK *)((BYTE *)osys + osys->DefinitionLength);
			PERF_COUNTER_DEFINITION *c = perf_counter(osys, perf_index("Processor Queue Length"));
			if (c) { cpu_q = perf_read(cb, c); have_cpu = 1; }
		}

		PERF_OBJECT_TYPE *omem = perf_object(db, i_mem);
		if (omem && omem->NumInstances == PERF_NO_INSTANCES) {
			PERF_COUNTER_BLOCK *cb = (PERF_COUNTER_BLOCK *)((BYTE *)omem + omem->DefinitionLength);
			PERF_COUNTER_DEFINITION *c = perf_counter(omem, perf_index("Pages/sec"));
			if (c) { pages = perf_read(cb, c); have_mem = 1; }
		}
		free(buf);
	}

	/* disk_queue는 IOCTL_DISK_PERFORMANCE로 실측 — diskperf 미부착이면 측정 불가라
	 * 빈 배열(perflib의 raw-0 오표기 회피). */
	cJSON_AddItemToObject(s, "disk_queue", query_disk_queue());
	if (have_cpu) cJSON_AddNumberToObject(s, "cpu_run_queue",   (double)cpu_q);
	else          cJSON_AddNullToObject  (s, "cpu_run_queue");
	if (have_mem) cJSON_AddNumberToObject(s, "mem_paging_rate", (double)pages);
	else          cJSON_AddNullToObject  (s, "mem_paging_rate");

	cJSON_AddItemToObject(m, "saturation", s);
}

/* 설정된 수집 주기(초). main.c 루프와 동일 env(AGENT_INTERVAL_SEC, 기본 60). */
static int agent_interval_sec(void)
{
	const char *v = getenv("AGENT_INTERVAL_SEC");
	if (v && *v) {
		char *end;
		long n = strtol(v, &end, 10);
		if (end != v && n > 0 && n < 86400)
			return (int)n;
	}
	return 60;
}

cJSON *collect_metrics_payload(const char *machine_id, const char *agent_version)
{
	cJSON *m = cJSON_CreateObject();
	if (!m) return NULL;
	add_common_metadata(m, "metrics", machine_id, agent_version);
	cJSON_AddNumberToObject(m, "collection_interval_sec", agent_interval_sec());

	fill_cpu_stat(m);
	fill_memory_metrics(m);

	cJSON_AddNullToObject(m, "load_1m");
	cJSON_AddNullToObject(m, "load_5m");
	cJSON_AddNullToObject(m, "load_15m");

	cJSON_AddItemToObject(m, "disk_io", enumerate_disk_io());
	cJSON_AddItemToObject(m, "mounts",  enumerate_mounts(0));
	cJSON_AddItemToObject(m, "net_io",  enumerate_net_io());
	fill_saturation(m);

	return m;
}
