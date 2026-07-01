/**
 * @file collect.c
 * @brief Win32 collectors. Builds v2 payloads identical to the Linux agent
 *        (assessment-agent/docs/payload-schema.md).
 *
 * Sources mapped from /proc to Win32 API:
 *   /etc/machine-id        → HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid
 *   /etc/os-release        → HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion
 *   /proc/cpuinfo          → CPUID brand (0x80000002..04)
 *   /proc/meminfo          → GlobalMemoryStatusEx
 *   /proc/stat             → GetSystemTimes
 *   /proc/diskstats        → DeviceIoControl(IOCTL_DISK_PERFORMANCE)
 *   /proc/net/dev          → GetIfTable2
 *   /proc/net/{tcp,udp}    → GetExtendedTcpTable / GetExtendedUdpTable
 *   /proc/self/mountinfo   → GetLogicalDriveStringsW + GetVolumeInformationW
 *   getifaddrs             → GetAdaptersAddresses (also yields PhysicalAddress for MAC)
 *   systemctl list-units   → EnumServicesStatusExW (SCM)
 *
 * Windows-only payload differences (schema-compatible nulls / zeros):
 *   - metrics.load_*m                          : null
 *   - cpu_stat.{nice,iowait,irq,softirq,steal} : 0 (no Windows analog)
 *   - listen_ports[].uid                       : null (no POSIX uid)
 *
 * Unit conversion:
 *   - cpu_stat       : FILETIME 100ns ÷ 100000 → 10ms tick (Linux HZ=100 호환)
 *   - sectors_*      : BytesRead/Written ÷ 512 (Linux diskstats 호환)
 */

#define WIN32_LEAN_AND_MEAN

#include "collect.h"
#include "util.h"
#include "cJSON.h"
#include <openssl/evp.h>
#include "openssl_compat.h"   /* EVP_MD_CTX_new/_free on OpenSSL 1.0.2 (legacy) */
#include <curl/curl.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <intrin.h>      /* __cpuid */

#include <winsock2.h>    /* MUST precede windows.h */
#include <ws2tcpip.h>    /* inet_ntop, struct sockaddr_in6 */
#include <iphlpapi.h>    /* GetAdaptersAddresses, GetIfTable2, GetExtendedTcpTable */
#include <windows.h>
#include <winioctl.h>    /* IOCTL_DISK_PERFORMANCE, DISK_PERFORMANCE, GET_LENGTH_INFORMATION */
#include <ntddstor.h>    /* IOCTL_STORAGE_GET_DEVICE_NUMBER, STORAGE_DEVICE_NUMBER */
#include "nt52_compat.h" /* strtok_s/strncpy_s — NT5.2 msvcrt 부재 대비(legacy 빌드) */
/* QueryFullProcessImageNameW lives in <processthreadsapi.h> (auto-pulled by
 * <windows.h>) since Vista — <psapi.h> not needed. */

/* ============================================================
 *  NT 5.2 (Server 2003 / XP x64) legacy-API compat
 *
 *  The legacy build (PROFILE=legacy, _WIN32_WINNT=0x0502, OpenSSL 1.0.2u)
 *  predates several APIs this collector uses on Vista+/OpenSSL 1.1+:
 *    - GetIfTable2 / MIB_IF_ROW2 / FreeMibTable  → GetIfTable / MIB_IFROW
 *    - inet_ntop                                 → small polyfill below
 *    - IP_ADAPTER_UNICAST_ADDRESS.OnLinkPrefixLength (no member on XP struct)
 *    - EVP_MD_CTX_new / _free                    → EVP_MD_CTX_create / _destroy
 *    - QueryFullProcessImageNameW (+ PROCESS_QUERY_LIMITED_INFORMATION)
 *  Per the collector contract, fields that cannot be obtained on the legacy
 *  OS degrade to empty/absent (never an error).
 * ============================================================ */
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0600
#define AGENT_NT6 1
#else
#define AGENT_NT6 0
#endif

#if !AGENT_NT6
/* inet_ntop is Vista+. Minimal polyfill (uncompressed IPv6 — acceptable for
 * the display-only ip_internal field; the engine keys on composite_id). */
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

/* ============================================================
 *  Process-lifetime cached timestamps (boot_time, agent_started_at)
 * ============================================================ */
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

/* cached_composite_id 는 collect.h 에 public 으로 노출 — main.c 가 worker 큐 이름
 * 빌드에 사용. 실제 정의는 fill_network_info() 이후. */

/* ============================================================
 *  Common metadata (every message)
 * ============================================================ */
static void add_common_metadata(cJSON *root, const char *msg_type,
                                const char *machine_id, const char *agent_version)
{
	cache_process_times();

	cJSON_AddStringToObject(root, "message_type", msg_type);
	cJSON_AddStringToObject(root, "machine_id",   machine_id ? machine_id : "");
	cJSON_AddStringToObject(root, "composite_id", cached_composite_id(machine_id));
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

/* Forward decls — 정의는 cloud metadata 영역 (cached_composite_id 다음). */
static char *http_get_short(const char *url, const char *header, int put_request);
static char *try_cloud_instance_id(void);

/* ============================================================
 *  machine_id — Registry HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid
 *               + cloud IMDS instance-id fallback (Sysprep 안 한 image /
 *                 비표준 환경 안전망 — Linux /etc/machine-id 결손 케이스와 대칭).
 * ============================================================ */
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

	/* Trim any stray trailing NUL / CR / LF and lowercase to match
	 * the /etc/machine-id (32-char hex) shape the engine expects to
	 * normalize across platforms. */
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

/* ============================================================
 *  OS version (Registry CurrentVersion)
 * ============================================================ */
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
}

/* ============================================================
 *  CPU model (CPUID extended brand string)
 * ============================================================ */
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

/* ============================================================
 *  Memory (GlobalMemoryStatusEx)
 * ============================================================ */
static void fill_memory_inventory(cJSON *out)
{
	MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
	if (!GlobalMemoryStatusEx(&ms)) {
		cJSON_AddNullToObject(out, "mem_total_kb");
		cJSON_AddNullToObject(out, "swap_total_kb");
		return;
	}
	long long mem_total_kb = (long long)(ms.ullTotalPhys / 1024ULL);
	/* swap_total = page file size = total commit limit - physical */
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

	cJSON_AddNumberToObject(out, "mem_total_kb",     (double)mem_total);
	cJSON_AddNumberToObject(out, "mem_free_kb",      (double)mem_avail);
	cJSON_AddNumberToObject(out, "mem_available_kb", (double)mem_avail);
	/* Windows에는 Linux buffers/cached 와 1:1 대응이 없으므로 null. */
	cJSON_AddNullToObject(out, "mem_buffers_kb");
	cJSON_AddNullToObject(out, "mem_cached_kb");
	cJSON_AddNumberToObject(out, "swap_total_kb",    (double)swap_total);
	cJSON_AddNumberToObject(out, "swap_free_kb",     (double)swap_avail);
}

/* ============================================================
 *  CPU stat (GetSystemTimes — 100ns ticks → 10ms ticks)
 * ============================================================ */
static void fill_cpu_stat(cJSON *m)
{
	cJSON *cs = cJSON_AddObjectToObject(m, "cpu_stat");

	FILETIME idleTime, kernelTime, userTime;
	if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
		cJSON_AddNumberToObject(cs, "user",    0);
		cJSON_AddNumberToObject(cs, "nice",    0);
		cJSON_AddNumberToObject(cs, "system",  0);
		cJSON_AddNumberToObject(cs, "idle",    0);
		cJSON_AddNumberToObject(cs, "iowait",  0);
		cJSON_AddNumberToObject(cs, "irq",     0);
		cJSON_AddNumberToObject(cs, "softirq", 0);
		cJSON_AddNumberToObject(cs, "steal",   0);
		return;
	}
	ULARGE_INTEGER idle, kern, usr;
	idle.LowPart = idleTime.dwLowDateTime;   idle.HighPart = idleTime.dwHighDateTime;
	kern.LowPart = kernelTime.dwLowDateTime; kern.HighPart = kernelTime.dwHighDateTime;
	usr.LowPart  = userTime.dwLowDateTime;   usr.HighPart  = userTime.dwHighDateTime;

	/* KernelTime includes IdleTime; subtract to get system-only. */
	ULONGLONG sys_only = (kern.QuadPart > idle.QuadPart) ? (kern.QuadPart - idle.QuadPart) : 0;

	cJSON_AddNumberToObject(cs, "user",    (double)(usr.QuadPart  / 100000ULL));
	cJSON_AddNumberToObject(cs, "nice",    0);
	cJSON_AddNumberToObject(cs, "system",  (double)(sys_only       / 100000ULL));
	cJSON_AddNumberToObject(cs, "idle",    (double)(idle.QuadPart  / 100000ULL));
	cJSON_AddNumberToObject(cs, "iowait",  0);
	cJSON_AddNumberToObject(cs, "irq",     0);
	cJSON_AddNumberToObject(cs, "softirq", 0);
	cJSON_AddNumberToObject(cs, "steal",   0);
}

/* ============================================================
 *  Physical disks
 * ============================================================ */
static cJSON *enumerate_physical_disks(void)
{
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < 32; i++) {
		wchar_t path[64];
		swprintf(path, 64, L"\\\\.\\PhysicalDrive%d", i);
		HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
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
			cJSON_AddItemToArray(arr, o);
		}
		CloseHandle(h);
	}
	return arr;
}

/* Map a drive root (e.g. "C:\") to its physical drive number. */
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

/* ============================================================
 *  Mounts (logical drives with type=FIXED)
 *
 *  include_fstype: 1 for inventory (with fstype), 0 for metrics.
 * ============================================================ */
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

/* ============================================================
 *  Disk IO (IOCTL_DISK_PERFORMANCE per physical drive)
 *
 *  diskperf 카운터는 Windows Server 2008+ 자동 활성화. 비활성화된 환경에서는
 *  값이 0으로 보이며 그 케이스는 빈 카운터로 emit (engine은 동일하게 delta 0).
 * ============================================================ */
static cJSON *enumerate_disk_io(void)
{
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < 32; i++) {
		wchar_t path[64];
		swprintf(path, 64, L"\\\\.\\PhysicalDrive%d", i);
		HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
		                       NULL, OPEN_EXISTING, 0, NULL);
		if (h == INVALID_HANDLE_VALUE) break;

		DISK_PERFORMANCE dp;
		DWORD ret = 0;
		if (DeviceIoControl(h, IOCTL_DISK_PERFORMANCE, NULL, 0,
		                    &dp, sizeof dp, &ret, NULL)) {
			cJSON *o = cJSON_CreateObject();
			char name[32];
			snprintf(name, sizeof name, "PhysicalDrive%d", i);
			cJSON_AddStringToObject(o, "device", name);
			cJSON_AddNumberToObject(o, "major", 0);
			cJSON_AddNumberToObject(o, "minor", i);
			cJSON_AddNumberToObject(o, "reads_completed",  (double)dp.ReadCount);
			cJSON_AddNumberToObject(o, "writes_completed", (double)dp.WriteCount);
			/* 512-byte sectors for diskstats wire-compat. */
			cJSON_AddNumberToObject(o, "sectors_read",
			                        (double)(dp.BytesRead.QuadPart / 512));
			cJSON_AddNumberToObject(o, "sectors_written",
			                        (double)(dp.BytesWritten.QuadPart / 512));
			cJSON_AddItemToArray(arr, o);
		}
		CloseHandle(h);
	}
	return arr;
}

/* ============================================================
 *  Network IO (GetIfTable2)
 * ============================================================ */
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
		cJSON_AddItemToArray(arr, o);
	}
	FreeMibTable(table);
	return arr;
}
#else
/* NT 5.2 fallback: GetIfTable2 / MIB_IF_ROW2 / FreeMibTable are Vista+. The
 * classic GetIfTable / MIB_IFROW (NT4+) exposes 32-bit counters — sufficient
 * for the wire format (engine handles counter wrap). */
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
		/* NT5.2 일부 인터페이스는 wszName 이 빈 문자열 -> interface="" 가 되어
		 * 엔진 NetIo 검증(min_length>=1)에서 메트릭 메시지 전체가 거부된다.
		 * description -> "if<index>" 순으로 fallback 해 항상 non-empty 보장. */
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
		cJSON_AddItemToArray(arr, o);
	}
	free(t);
	return arr;
}
#endif

/* ============================================================
 *  IP addresses + MAC addresses (GetAdaptersAddresses, single pass)
 *
 *  mac_addresses[] 는 (machine_id, mac_addresses) 조합으로 이미지 클론
 *  충돌 감지에 사용된다. 가상/터널 인터페이스는 제외, 정렬 + dedup.
 * ============================================================ */
static int mac_cmp(const void *a, const void *b)
{
	return strcmp(*(const char **)a, *(const char **)b);
}

static void fill_network_info(cJSON *inv)
{
	cJSON *ips  = cJSON_AddArrayToObject(inv, "ip_internal");
	cJSON *macs = cJSON_AddArrayToObject(inv, "mac_addresses");

	ULONG buf_len = 16 * 1024;
	IP_ADAPTER_ADDRESSES *aa = malloc(buf_len);
	if (!aa) return;

	ULONG ret = GetAdaptersAddresses(AF_UNSPEC,
		GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
		NULL, aa, &buf_len);
	if (ret == ERROR_BUFFER_OVERFLOW) {
		free(aa);
		aa = malloc(buf_len);
		if (!aa) return;
		ret = GetAdaptersAddresses(AF_UNSPEC,
			GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
			NULL, aa, &buf_len);
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
			if (u->Address.lpSockaddr->sa_family == AF_INET) {
				struct sockaddr_in *sa = (struct sockaddr_in *)u->Address.lpSockaddr;
				inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip);
			} else if (u->Address.lpSockaddr->sa_family == AF_INET6) {
				struct sockaddr_in6 *sa = (struct sockaddr_in6 *)u->Address.lpSockaddr;
				inet_ntop(AF_INET6, &sa->sin6_addr, ip, sizeof ip);
			}
			if (!ip[0]) continue;
			char cidr[INET6_ADDRSTRLEN + 5];
#if AGENT_NT6
			/* CIDR: "<ip>/<prefix>" — OnLinkPrefixLength is supplied by
			 * GetAdaptersAddresses (Vista+). Buffer fits IPv6 + "/128". */
			snprintf(cidr, sizeof cidr, "%s/%u", ip,
				(unsigned)u->OnLinkPrefixLength);
#else
			/* NT 5.2: IP_ADAPTER_UNICAST_ADDRESS_XP has no OnLinkPrefixLength.
			 * Emit the bare IP (no prefix) — ip_internal is display-only; the
			 * engine keys hosts on composite_id. */
			snprintf(cidr, sizeof cidr, "%s", ip);
#endif
			cJSON_AddItemToArray(ips, cJSON_CreateString(cidr));
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

/* ============================================================
 *  composite_id = sha256_hex(machine_id + "\n" + mac1 + "\n" + ...)
 *
 *  이미지 클론으로 machine_id가 중복되는 환경에서 호스트 식별성을 보강하는 보조
 *  식별자. MAC 수집 정책은 fill_network_info()와 동일 (IfOperStatusUp, loopback /
 *  tunnel 제외, PhysicalAddressLength==6, lowercase, dedup, sort). 양 OS agent가
 *  같은 정규형으로 같은 값을 산출.
 *
 *  모든 NIC이 가상이거나 MAC이 비어있으면 sha256(machine_id + "\n")로 떨어지며
 *  식별성이 약해진다 — composite_id로도 못 가르는 엣지 케이스는 한계로 수용.
 *  엔진이 별도 신호로 충돌 감지해야 한다.
 *
 *  프로세스 lifetime 내 1회 계산 후 캐시.
 * ============================================================ */
const char *cached_composite_id(const char *machine_id)
{
	static char hex_buf[65];
	static int  cached = 0;
	if (cached)
		return hex_buf;

	hex_buf[0] = '\0';
	cached = 1;   /* fail-once: EVP / GetAdaptersAddresses 실패 시 재시도 회피 */

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

/* ============================================================
 *  Cloud metadata HTTP fetch (libcurl, 1s timeout, 256B hard cap)
 *
 *  external IP / instance-id 응답은 모두 매우 짧음 — 응답 크기를 256B 로 cap 해
 *  악의/오동작 metadata 서버로부터의 메모리 폭주 방지. AGENT_EXTERNAL_IP env
 *  override 가 있으면 cloud 호출 자체 skip.
 * ============================================================ */

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
	if (n == 0) return size * nmemb;  /* swallow remaining bytes silently */
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
	/* trim trailing whitespace */
	size_t l = strlen(out);
	while (l > 0 && (out[l-1] == '\n' || out[l-1] == '\r' || out[l-1] == ' ' || out[l-1] == '\t')) out[--l] = '\0';
	if (l == 0) { free(out); return NULL; }
	return out;
}

/*
 * Cloud external IP — AGENT_EXTERNAL_IP override → AWS IMDSv2 → Azure → GCP 순.
 * 전 실패 시 null 리터럴 (server.error 발행 안 함, optional 정책).
 */
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

	/* AWS IMDSv2 — token PUT 후 GET. */
	char *token = http_get_short(
		"http://169.254.169.254/latest/api/token",
		"X-aws-ec2-metadata-token-ttl-seconds: 60",
		1 /* PUT */);
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

/*
 * Cloud IMDS instance-id fallback — Registry MachineGuid 결손/공백 시 호출됨.
 * AWS IMDSv2 / Azure vmId / GCP instance id 순. 전 실패 시 NULL.
 *
 * 형식 차이가 약간 있어도 (AWS = i-xxxxxxxxx, Azure = UUID, GCP = 정수형 string)
 * engine 의 String(64) 제약은 모두 통과 — composite_id 의 해시 source 로만 들어가
 * 형식 일관성은 불필요.
 */
static char *try_cloud_instance_id(void)
{
	char *id = NULL;

	/* AWS IMDSv2 */
	char *token = http_get_short(
		"http://169.254.169.254/latest/api/token",
		"X-aws-ec2-metadata-token-ttl-seconds: 60",
		1 /* PUT */);
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

/* ============================================================
 *  Services (EnumServicesStatusExW — running only)
 * ============================================================ */
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
			cJSON_AddItemToArray(arr, o);
		}
	}
	free(buf);
	CloseServiceHandle(scm);
	return arr;
}

/* ============================================================
 *  Listen ports (TCP + UDP, IPv4 + IPv6)
 *
 *  uid 는 Windows 에 POSIX uid 가 없으므로 null. comm 은 OpenProcess 권한이
 *  없으면 null. pid 는 항상 채움.
 * ============================================================ */
static void fill_comm_for_pid(DWORD pid, char *out, size_t out_sz)
{
	out[0] = '\0';
	if (pid == 0) return;
#if AGENT_NT6
	HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!hp) return;
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
#else
	/* NT 5.2: QueryFullProcessImageNameW + PROCESS_QUERY_LIMITED_INFORMATION
	 * are Vista+. comm is nullable by contract — leave empty on legacy. */
	(void)pid; (void)out_sz;
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

/* ============================================================
 *  Public API
 * ============================================================ */
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

	fill_network_info(m);  /* adds ip_internal[] and mac_addresses[] */
	cJSON_AddItemToObject(m, "ip_external", collect_external_ip());

	return m;
}

cJSON *collect_metrics_payload(const char *machine_id, const char *agent_version)
{
	cJSON *m = cJSON_CreateObject();
	if (!m) return NULL;
	add_common_metadata(m, "metrics", machine_id, agent_version);

	fill_cpu_stat(m);
	fill_memory_metrics(m);

	cJSON_AddNullToObject(m, "load_1m");
	cJSON_AddNullToObject(m, "load_5m");
	cJSON_AddNullToObject(m, "load_15m");

	cJSON_AddItemToObject(m, "disk_io", enumerate_disk_io());
	cJSON_AddItemToObject(m, "mounts",  enumerate_mounts(0));
	cJSON_AddItemToObject(m, "net_io",  enumerate_net_io());

	return m;
}
