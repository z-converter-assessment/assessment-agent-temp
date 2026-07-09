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


static cJSON *inv_collect_block_devices(void);
static cJSON *inv_collect_external_ip(void);
static cJSON *inv_collect_listen_ports(void);
static cJSON *inv_collect_net_interfaces(void);
static cJSON *inv_collect_services(void);
static void add_listen_entry(cJSON *arr, const char *proto, const char *addr,
                             unsigned short port, DWORD pid);
static void bd_add_win(cJSON *arr, const char *name, const char *type, long long size,
                       const char *fst, const char *mnt, const char *parent, const char *idfull);
static void collect_tcp4_listen(cJSON *arr);
static void collect_tcp6_listen(cJSON *arr);
static void collect_udp4_listen(cJSON *arr);
static void collect_udp6_listen(cJSON *arr);
static void cpu_model_string(char *out, size_t len);
static void fill_comm_for_pid(DWORD pid, char *out, size_t out_sz);
static void fill_comm_toolhelp(DWORD pid, char *out, size_t out_sz);

void os_version_info(char *display_out, size_t dsz, char *build_out, size_t bsz)
{
	if (dsz) display_out[0] = '\0';
	if (bsz) build_out[0]   = '\0';

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

	/* DisplayVersion/ReleaseId는 Win10/2016+ 전용 레지스트리다. 구버전은 RtlGetVersion(ntdll)의 NT major.minor로 채운다(manifest 영향 없는 실제 버전). */
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
		if (len) out[0] = '\0';   /* brand string unsupported -> caller emits null */
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

static cJSON *inv_collect_external_ip(void)
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

	char *ip = fetch_imds_chain(
		"http://169.254.169.254/latest/meta-data/public-ipv4",
		"http://169.254.169.254/metadata/instance/network/interface/0/"
		"ipv4/ipAddress/0/publicIpAddress?api-version=2021-02-01&format=text",
		"http://metadata.google.internal/computeMetadata/v1/instance/"
		"network-interfaces/0/access-configs/0/external-ip");

	if (!ip || !*ip) {
		free(ip);
		return cJSON_CreateNull();
	}
	cJSON *arr = cJSON_CreateArray();
	cJSON_AddItemToArray(arr, cJSON_CreateString(ip));
	free(ip);
	return arr;
}

static cJSON *inv_collect_services(void)
{
	SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
	if (!scm) return cJSON_CreateNull();

	DWORD bytes_needed = 0, services_returned = 0, resume = 0;

	/* 1차 size 쿼리(정상이면 FALSE+ERROR_MORE_DATA로 bytes_needed 채움). bytes_needed 미설정=접근 불가 -> null(빈 배열=0개와 다름). */
	EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE,
	                      NULL, 0, &bytes_needed, &services_returned, &resume, NULL);
	if (bytes_needed == 0) { CloseServiceHandle(scm); return cJSON_CreateNull(); }

	BYTE *buf = malloc(bytes_needed);
	if (!buf) { CloseServiceHandle(scm); return cJSON_CreateNull(); }

	/* 2차 실쿼리. 실패(서비스 증가 등)면 열거 미완이므로 null(부분/빈 목록 위조 금지). */
	if (!EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE,
	                           buf, bytes_needed, &bytes_needed, &services_returned,
	                           &resume, NULL)) {
		free(buf);
		CloseServiceHandle(scm);
		return cJSON_CreateNull();
	}

	cJSON *arr = cJSON_CreateArray();
	{
		ENUM_SERVICE_STATUS_PROCESSW *es = (ENUM_SERVICE_STATUS_PROCESSW *)buf;
		for (DWORD i = 0; i < services_returned; i++) {
			if (es[i].ServiceStatusProcess.dwCurrentState != SERVICE_RUNNING) continue;
			char name[256] = {0};
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

/* Toolhelp32(kernel32)로 pid -> exe 베이스명. OpenProcess 불필요라 System(4) 등 보호 프로세스도 커버. */
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

	/* 1차: QueryFullProcessImageName(NT6+ export라 런타임 해소, WOW64에서도 64비트 대상 경로 반환).
	 * 보호 프로세스(OpenProcess 거부)와 NT5.2(이 API 없음)는 2차 Toolhelp32 폴백. */
	typedef BOOL (WINAPI *QFPIN_fn)(HANDLE, DWORD, LPWSTR, PDWORD);
	static int resolved = 0;
	static QFPIN_fn p_qfpin = NULL;
	if (!resolved) {
		resolved = 1;
		HMODULE k = GetModuleHandleA("kernel32.dll");
		if (k)
			p_qfpin = (QFPIN_fn)(void *)GetProcAddress(k, "QueryFullProcessImageNameW");
	}

	if (p_qfpin) {
		HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (hp) {
			wchar_t wname[1024];
			DWORD wsz = 1024;
			if (p_qfpin(hp, 0, wname, &wsz)) {
				wchar_t *base = wcsrchr(wname, L'\\');
				base = base ? base + 1 : wname;
				WideCharToMultiByte(CP_UTF8, 0, base, -1, out, (int)out_sz, NULL, NULL);
				char *dot = strrchr(out, '.');
				if (dot && _stricmp(dot, ".exe") == 0) *dot = '\0';
			}
			CloseHandle(hp);
		}
	}
	if (!out[0])
		fill_comm_toolhelp(pid, out, out_sz);
}

static void add_listen_entry(cJSON *arr, const char *proto, const char *addr,
                             unsigned short port, DWORD pid)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "proto", proto);
	cJSON_AddStringToObject(o, "addr",  addr);
	cJSON_AddNumberToObject(o, "port",  port);
	cJSON_AddNullToObject  (o, "uid");
	/* pid 0은 System Idle이라 소켓 소유자일 수 없다 -> 소유자 미상 null(추측 금지). */
	if (pid) cJSON_AddNumberToObject(o, "pid", (double)pid);
	else     cJSON_AddNullToObject  (o, "pid");
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

static cJSON *inv_collect_listen_ports(void)
{
	cJSON *arr = cJSON_CreateArray();
	collect_tcp4_listen(arr);
	collect_tcp6_listen(arr);
	collect_udp4_listen(arr);
	collect_udp6_listen(arr);
	return arr;
}

cJSON *collect_inventory_payload(const char *machine_id, const char *agent_version)
{
	cJSON *m = cJSON_CreateObject();
	if (!m) return NULL;
	wire_add_envelope(m, "inventory", machine_id, agent_version);

	cJSON_AddStringToObject(m, "os_id", "windows");

	char display[64] = {0}, build[64] = {0};
	os_version_info(display, sizeof display, build, sizeof build);
	if (display[0]) cJSON_AddStringToObject(m, "os_version", display);
	else            cJSON_AddNullToObject  (m, "os_version");
	cJSON_AddNullToObject  (m, "os_codename");
	if (build[0]) cJSON_AddStringToObject(m, "kernel_version", build);
	else          cJSON_AddNullToObject  (m, "kernel_version");

	SYSTEM_INFO si;
	GetNativeSystemInfo(&si);
	cJSON_AddNumberToObject(m, "cpu_cores", (double)si.dwNumberOfProcessors);

	char cpu_brand[64];
	cpu_model_string(cpu_brand, sizeof cpu_brand);
	if (cpu_brand[0]) cJSON_AddStringToObject(m, "cpu_model", cpu_brand);
	else              cJSON_AddNullToObject  (m, "cpu_model");

	/* mem_total_bytes (base 단위). swap 은 block_devices type=swap(pagefile) 로(P4). */
	MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
	if (GlobalMemoryStatusEx(&ms))
		cJSON_AddNumberToObject(m, "mem_total_bytes", (double)ms.ullTotalPhys);
	else
		cJSON_AddNullToObject(m, "mem_total_bytes");

	cJSON_AddItemToObject(m, "services",     inv_collect_services());
	cJSON_AddItemToObject(m, "listen_ports", inv_collect_listen_ports());
	cJSON_AddItemToObject(m, "ip_external",  inv_collect_external_ip());

	cJSON_AddItemToObject(m, "block_devices",  inv_collect_block_devices());
	cJSON_AddItemToObject(m, "net_interfaces", inv_collect_net_interfaces());

	return m;
}

static void bd_add_win(cJSON *arr, const char *name, const char *type, long long size,
                       const char *fst, const char *mnt, const char *parent, const char *idfull)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "name", name);
	cJSON_AddStringToObject(o, "type", type);
	if (size >= 0) cJSON_AddNumberToObject(o, "size_bytes", (double)size); else cJSON_AddNullToObject(o, "size_bytes");
	if (fst && *fst) cJSON_AddStringToObject(o, "fstype", fst); else cJSON_AddNullToObject(o, "fstype");
	if (mnt && *mnt) cJSON_AddStringToObject(o, "mountpoint", mnt); else cJSON_AddNullToObject(o, "mountpoint");
	if (parent) cJSON_AddStringToObject(o, "parent", parent); else cJSON_AddNullToObject(o, "parent");
	cJSON_AddStringToObject(o, "id", win_id_value(idfull));
	cJSON_AddStringToObject(o, "id_type", win_id_type(idfull));
	cJSON_AddItemToArray(arr, o);
}

/* P4 block_devices: 물리디스크(disk, gptid/mbrsig/serial) + 볼륨(volume, volguid, parent=disk extents). */
static cJSON *inv_collect_block_devices(void)
{
	cJSON *arr = cJSON_CreateArray();
	char diskid[32][96]; int diskvalid[32] = {0};
	for (int i = 0; i < 32; i++) {
		HANDLE h = open_physical_drive(i);
		if (h == INVALID_HANDLE_VALUE) continue;
		GET_LENGTH_INFORMATION gli; DWORD ret = 0; long long size = -1;
		if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &gli, sizeof gli, &ret, NULL)) size = (long long)gli.Length.QuadPart;
		CloseHandle(h);
		char idfull[96]; win_disk_id(i, idfull, sizeof idfull);
		snprintf(diskid[i], sizeof diskid[i], "%s", win_id_value(idfull)); diskvalid[i] = 1;
		char name[32]; snprintf(name, sizeof name, "PhysicalDrive%d", i);
		bd_add_win(arr, name, "disk", size, NULL, NULL, NULL, idfull);
	}
	wchar_t vol[MAX_PATH];
	HANDLE fv = FindFirstVolumeW(vol, MAX_PATH);
	if (fv != INVALID_HANDLE_VALUE) {
		do {
			char volu[160] = {0}; WideCharToMultiByte(CP_UTF8, 0, vol, -1, volu, sizeof volu, NULL, NULL);
			char guid[96] = {0}; char *b = strchr(volu, '{');
			if (b) { char *e = strchr(b, '}'); if (e) { int n = (int)(e - b + 1); if (n < (int)sizeof guid) { memcpy(guid, b, n); guid[n] = '\0'; } } }
			char idfull[128];
			if (guid[0]) snprintf(idfull, sizeof idfull, "volguid:%s", guid); else snprintf(idfull, sizeof idfull, "name:%s", volu);
			wchar_t fsw[16] = {0}; char fst[16] = {0};
			GetVolumeInformationW(vol, NULL, 0, NULL, NULL, NULL, fsw, 16);
			if (fsw[0]) { WideCharToMultiByte(CP_UTF8, 0, fsw, -1, fst, sizeof fst, NULL, NULL); for (char *q = fst; *q; q++) *q = (char)tolower((unsigned char)*q); }
			wchar_t names[256] = {0}; DWORD cnt = 0; char mnt[64] = {0};
			if (GetVolumePathNamesForVolumeNameW(vol, names, 256, &cnt) && names[0]) {
				WideCharToMultiByte(CP_UTF8, 0, names, -1, mnt, sizeof mnt, NULL, NULL);
				size_t l = strlen(mnt); if (l && mnt[l-1] == '\\') mnt[l-1] = '\0';
			}
			long long vsize = -1; ULARGE_INTEGER a, t, f;
			if (GetDiskFreeSpaceExW(vol, &a, &t, &f)) vsize = (long long)t.QuadPart;
			wchar_t volnb[MAX_PATH]; wcsncpy(volnb, vol, MAX_PATH); volnb[MAX_PATH-1] = L'\0';
			size_t wl = wcslen(volnb); if (wl && volnb[wl-1] == L'\\') volnb[wl-1] = L'\0';
			HANDLE vh = CreateFileW(volnb, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
			int emitted = 0;
			if (vh != INVALID_HANDLE_VALUE) {
				BYTE eb[4096]; DWORD ret = 0;
				if (DeviceIoControl(vh, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, eb, sizeof eb, &ret, NULL)) {
					VOLUME_DISK_EXTENTS *vde = (VOLUME_DISK_EXTENTS *)eb;
					for (DWORD k = 0; k < vde->NumberOfDiskExtents; k++) {
						int dn = (int)vde->Extents[k].DiskNumber;
						const char *par = (dn >= 0 && dn < 32 && diskvalid[dn]) ? diskid[dn] : NULL;
						bd_add_win(arr, mnt[0] ? mnt : volu, "volume", vsize, fst[0] ? fst : NULL, mnt[0] ? mnt : NULL, par, idfull);
						emitted = 1;
					}
				}
				CloseHandle(vh);
			}
			if (!emitted) bd_add_win(arr, mnt[0] ? mnt : volu, "volume", vsize, fst[0] ? fst : NULL, mnt[0] ? mnt : NULL, NULL, idfull);
		} while (FindNextVolumeW(fv, vol, MAX_PATH));
		FindVolumeClose(fv);
	}

	/* pagefile(swap 노드): NtQuerySystemInformation SystemPageFileInformation(class 18). Linux swap 노드와
	 * 필드셋 패리티. 관측 전용(레지스트리 미변경). id/id_type=null(볼륨 GUID 아닌 파일). 없으면 미발행. */
	{
		/* UNICODE_STRING(PageFileName)을 raw 필드로 인라인 — winternl.h 미포함 회피. */
		typedef struct _AGENT_PAGEFILE_INFO {
			ULONG  NextEntryOffset;
			ULONG  TotalSize;      /* pages */
			ULONG  TotalInUse;
			ULONG  PeakUsage;
			USHORT NameLength;     /* bytes */
			USHORT NameMaxLength;
			PWSTR  NameBuffer;
		} AGENT_PAGEFILE_INFO;
		typedef LONG (WINAPI *NQSI)(ULONG, PVOID, ULONG, PULONG);
		HMODULE nt = GetModuleHandleA("ntdll.dll");
		NQSI f = nt ? (NQSI)(void *)GetProcAddress(nt, "NtQuerySystemInformation") : NULL;
		if (f) {
			ULONG cap = 4096, ret = 0;
			BYTE *buf = (BYTE *)malloc(cap);
			if (buf) {
				LONG st = f(18, buf, cap, &ret);
				if (st == (LONG)0xC0000004UL && ret > cap) {   /* LENGTH_MISMATCH -> 동적 확장 */
					BYTE *nb = (BYTE *)realloc(buf, ret);
					if (nb) { buf = nb; cap = ret; st = f(18, buf, cap, &ret); }
				}
				if (st == 0 && ret >= sizeof(AGENT_PAGEFILE_INFO)) {
					SYSTEM_INFO si; GetSystemInfo(&si);
					DWORD page = si.dwPageSize ? si.dwPageSize : 4096;
					BYTE *p = buf;
					for (;;) {
						AGENT_PAGEFILE_INFO *pf = (AGENT_PAGEFILE_INFO *)p;
						long long size = (long long)pf->TotalSize * (long long)page;
						char path[300] = {0};
						if (pf->NameBuffer && pf->NameLength)
							WideCharToMultiByte(CP_UTF8, 0, pf->NameBuffer,
							                    pf->NameLength / 2, path, sizeof path - 1, NULL, NULL);
						const char *disp = path;                       /* \??\C:\pagefile.sys -> C:\pagefile.sys */
						if (strncmp(disp, "\\??\\", 4) == 0) disp += 4;
						const char *base = strrchr(disp, '\\'); base = base ? base + 1 : disp;
						cJSON *o = cJSON_CreateObject();
						cJSON_AddStringToObject(o, "name", base[0] ? base : "pagefile");
						cJSON_AddStringToObject(o, "type", "swap");
						cJSON_AddNumberToObject(o, "size_bytes", (double)size);
						cJSON_AddNullToObject(o, "fstype");
						if (disp[0]) cJSON_AddStringToObject(o, "mountpoint", disp); else cJSON_AddNullToObject(o, "mountpoint");
						cJSON_AddNullToObject(o, "parent");
						cJSON_AddNullToObject(o, "id");
						cJSON_AddNullToObject(o, "id_type");
						cJSON_AddItemToArray(arr, o);
						if (!pf->NextEntryOffset) break;
						p += pf->NextEntryOffset;
					}
				}
				free(buf);
			}
		}
	}
	return arr;
}

/* P3 net_interfaces: GAA per-adapter -> MAC id + kind + speed + addresses[] + gateway. */
static cJSON *inv_collect_net_interfaces(void)
{
	cJSON *arr = cJSON_CreateArray();
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	if (agent_is_nt6()) flags |= GAA_FLAG_INCLUDE_GATEWAYS;
	ULONG buf_len = 16 * 1024; IP_ADAPTER_ADDRESSES *aa = malloc(buf_len);
	if (!aa) return arr;
	ULONG ret = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, aa, &buf_len);
	if (ret == ERROR_BUFFER_OVERFLOW) { free(aa); aa = malloc(buf_len); if (!aa) return arr; ret = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, aa, &buf_len); }
	if (ret != NO_ERROR) { free(aa); return arr; }
	for (IP_ADAPTER_ADDRESSES *p = aa; p; p = p->Next) {
		if (p->OperStatus != IfOperStatusUp) continue;
		if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		char ifname[256] = {0}; WideCharToMultiByte(CP_UTF8, 0, p->FriendlyName, -1, ifname, sizeof ifname, NULL, NULL);
		char fb[32]; snprintf(fb, sizeof fb, "if%lu", (unsigned long)p->IfIndex);
		char idfull[80]; mac_to_devid(p->PhysicalAddress, p->PhysicalAddressLength, fb, idfull, sizeof idfull);
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "name", ifname);
		cJSON_AddStringToObject(o, "id", win_id_value(idfull));
		cJSON_AddStringToObject(o, "id_type", (!strncmp(idfull, "mac:", 4)) ? "mac" : "name");
		DWORD if_idx = p->IfIndex ? p->IfIndex : p->Ipv6IfIndex;
		cJSON_AddStringToObject(o, "kind", iface_is_hardware(if_idx) ? win_net_kind(p->IfType) : "virtual");
		if (agent_is_nt6() && p->TransmitLinkSpeed && p->TransmitLinkSpeed != 0xFFFFFFFFFFFFFFFFULL)
			cJSON_AddNumberToObject(o, "speed_mbps", (double)(p->TransmitLinkSpeed / 1000000ULL));
		else cJSON_AddNullToObject(o, "speed_mbps");
		cJSON *addrs = cJSON_CreateArray();
		for (IP_ADAPTER_UNICAST_ADDRESS *u = p->FirstUnicastAddress; u; u = u->Next) {
			if (!u->Address.lpSockaddr) continue;
			char ip[INET6_ADDRSTRLEN] = {0}; const char *family = NULL; int prefix = -1;
			int fam = u->Address.lpSockaddr->sa_family;
			if (fam == AF_INET)  { inet_ntop(AF_INET, &((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr, ip, sizeof ip); family = "ipv4"; }
			else if (fam == AF_INET6) { inet_ntop(AF_INET6, &((struct sockaddr_in6 *)u->Address.lpSockaddr)->sin6_addr, ip, sizeof ip); family = "ipv6"; }
			if (!ip[0]) continue;
			if (agent_is_nt6()) prefix = (int)u->OnLinkPrefixLength;
			else if (fam == AF_INET) { unsigned ab = ((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.S_un.S_addr; int pfx; if (legacy_ipv4_prefix(p->IfIndex, ab, &pfx)) prefix = pfx; }
			cJSON *ad = cJSON_CreateObject();
			cJSON_AddStringToObject(ad, "address", ip);
			if (prefix >= 0) cJSON_AddNumberToObject(ad, "prefix", (double)prefix); else cJSON_AddNullToObject(ad, "prefix");
			cJSON_AddStringToObject(ad, "family", family);
			cJSON_AddItemToArray(addrs, ad);
		}
		cJSON_AddItemToObject(o, "addresses", addrs);
		cJSON *gw = NULL;
		if (agent_is_nt6()) {
			for (IP_ADAPTER_GATEWAY_ADDRESS_LH *g = p->FirstGatewayAddress; g; g = g->Next) {
				if (!g->Address.lpSockaddr) continue;
				int gf = g->Address.lpSockaddr->sa_family; char gb[INET6_ADDRSTRLEN] = {0};
				if (gf == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)g->Address.lpSockaddr)->sin_addr, gb, sizeof gb);
				else if (gf == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)g->Address.lpSockaddr)->sin6_addr, gb, sizeof gb);
				if (gb[0]) { gw = cJSON_CreateString(gb); break; }
			}
		} else {
			char gb[INET_ADDRSTRLEN]; if (legacy_ipv4_gateway(p->IfIndex, gb, sizeof gb)) gw = cJSON_CreateString(gb);
		}
		cJSON_AddItemToObject(o, "gateway", gw ? gw : cJSON_CreateNull());
		cJSON_AddItemToArray(arr, o);
	}
	free(aa);
	return arr;
}
