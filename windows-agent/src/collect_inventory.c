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
static cJSON *bd_add(cJSON *arr, const char *name, const char *type, long long size,
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

/* 재현 os 확장. arch/bits/boot_firmware/secure_boot/edition/timezone/rtc_utc.
   미측정=null(위조 금지). 세대 분기는 GetProcAddress 런타임(NT5.2 하드임포트 금지). */

/* 레지스트리 REG_SZ 를 wide 로 읽어 UTF-8 로 변환. RegQueryValueExA(ANSI)는 비영어 로케일에서
   값을 로컬 코드페이지(예 Korean CP949)로 돌려줘 invalid UTF-8 을 만든다(NT5.2 timezone StandardName
   폴백이 로컬라이즈 한국어 -> 엔진 dead-letter). wide 읽기 + WideCharToMultiByte(CP_UTF8)로 항상 UTF-8. */
static int reg_read_sz(HKEY root, const char *path, const char *val, char *out, size_t outsz)
{
	HKEY h;
	if (RegOpenKeyExA(root, path, 0, KEY_READ, &h) != ERROR_SUCCESS)
		return 0;
	WCHAR wval[128];
	MultiByteToWideChar(CP_ACP, 0, val, -1, wval, 128);
	WCHAR wbuf[512];
	DWORD sz = sizeof wbuf;
	DWORD type = 0;
	LONG rc = RegQueryValueExW(h, wval, NULL, &type, (LPBYTE)wbuf, &sz);
	RegCloseKey(h);
	if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
		return 0;
	int wn = (int)(sz / sizeof(WCHAR));
	while (wn > 0 && wbuf[wn - 1] == L'\0') wn--; /* 종단 NUL 제거 */
	int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, wn, out, (int)outsz - 1, NULL, NULL);
	out[(n > 0 && n < (int)outsz) ? n : 0] = '\0';
	return out[0] ? 1 : 0;
}

/* 레지스트리 REG_DWORD 읽기. 성공 1 */
static int reg_read_dword(HKEY root, const char *path, const char *val, DWORD *out)
{
	HKEY h;
	if (RegOpenKeyExA(root, path, 0, KEY_READ, &h) != ERROR_SUCCESS)
		return 0;
	DWORD sz = sizeof(*out);
	DWORD type = 0;
	LONG rc = RegQueryValueExA(h, val, NULL, &type, (LPBYTE)out, &sz);
	RegCloseKey(h);
	return (rc == ERROR_SUCCESS && type == REG_DWORD) ? 1 : 0;
}

/* boot_firmware: GetFirmwareType(NT6.2+) 1차, GetFirmwareEnvironmentVariableA 트릭(XP+) 폴백 */
static void os_add_boot_firmware(cJSON *m)
{
	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	if (k32) {
		typedef BOOL (WINAPI *GetFirmwareTypeFn)(void *);
		GetFirmwareTypeFn gft = (GetFirmwareTypeFn)(void *)GetProcAddress(k32, "GetFirmwareType");
		if (gft) {
			int ft = 0; /* FirmwareTypeBios=1, FirmwareTypeUefi=2 */
			if (gft(&ft)) {
				if (ft == 2)      { cJSON_AddStringToObject(m, "boot_firmware", "uefi"); return; }
				else if (ft == 1) { cJSON_AddStringToObject(m, "boot_firmware", "bios"); return; }
			}
		}
	}
	/* 폴백: 더미 GUID 로 GetFirmwareEnvironmentVariableA 호출. BIOS=ERROR_INVALID_FUNCTION(1) */
	SetLastError(0);
	GetFirmwareEnvironmentVariableA("", "{00000000-0000-0000-0000-000000000000}", NULL, 0);
	DWORD err = GetLastError();
	if (err == ERROR_INVALID_FUNCTION)
		cJSON_AddStringToObject(m, "boot_firmware", "bios");
	else if (err == ERROR_NOACCESS || err == ERROR_ENVVAR_NOT_FOUND || err == ERROR_SUCCESS)
		cJSON_AddStringToObject(m, "boot_firmware", "uefi");
	else
		cJSON_AddNullToObject(m, "boot_firmware");
}

static void inv_collect_os_repro(cJSON *m)
{
	SYSTEM_INFO si;
	GetNativeSystemInfo(&si);
	const char *arch = NULL;
	int bits = 0;
	switch (si.wProcessorArchitecture) {
	case PROCESSOR_ARCHITECTURE_AMD64: arch = "x86_64";  bits = 64; break;
	case PROCESSOR_ARCHITECTURE_INTEL: arch = "i686";    bits = 32; break;
	case PROCESSOR_ARCHITECTURE_IA64:  arch = "ia64";    bits = 64; break;
	case 12 /*ARM64*/:                 arch = "aarch64"; bits = 64; break;
	case PROCESSOR_ARCHITECTURE_ARM:   arch = "arm";     bits = 32; break;
	default: break;
	}
	if (arch) {
		cJSON_AddStringToObject(m, "arch", arch);
		cJSON_AddNumberToObject(m, "bits", bits);
	} else {
		cJSON_AddNullToObject(m, "arch");
		cJSON_AddNullToObject(m, "bits");
	}
	os_add_boot_firmware(m);
	/* secure_boot: SecureBoot\State UEFISecureBootEnabled DWORD. 부재=null */
	{
		DWORD v = 0;
		if (reg_read_dword(HKEY_LOCAL_MACHINE,
		    "SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", "UEFISecureBootEnabled", &v))
			cJSON_AddBoolToObject(m, "secure_boot", v ? 1 : 0);
		else
			cJSON_AddNullToObject(m, "secure_boot");
	}
	/* edition: CurrentVersion EditionID(SKU 코드). 부재=null */
	{
		char ed[64] = {0};
		if (reg_read_sz(HKEY_LOCAL_MACHINE,
		    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "EditionID", ed, sizeof ed))
			cJSON_AddStringToObject(m, "edition", ed);
		else
			cJSON_AddNullToObject(m, "edition");
	}
	/* timezone: TimeZoneKeyName(Vista+) 1차, StandardName(XP/2003) 폴백. 원문(엔진이 CLDR 매핑) */
	{
		char tz[128] = {0};
		if (reg_read_sz(HKEY_LOCAL_MACHINE,
		        "SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation", "TimeZoneKeyName", tz, sizeof tz)
		    || reg_read_sz(HKEY_LOCAL_MACHINE,
		        "SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation", "StandardName", tz, sizeof tz))
			cJSON_AddStringToObject(m, "timezone", tz);
		else
			cJSON_AddNullToObject(m, "timezone");
	}
	/* rtc_utc: RealTimeIsUniversal DWORD. 부재=null(위조 금지 — 기본 local 을 false 로 위조하지 않음) */
	{
		DWORD v = 0;
		if (reg_read_dword(HKEY_LOCAL_MACHINE,
		    "SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation", "RealTimeIsUniversal", &v))
			cJSON_AddBoolToObject(m, "rtc_utc", v ? 1 : 0);
		else
			cJSON_AddNullToObject(m, "rtc_utc");
	}
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

	inv_collect_os_repro(m);

	SYSTEM_INFO si;
	GetNativeSystemInfo(&si);
	cJSON_AddNumberToObject(m, "cpu_cores", (double)si.dwNumberOfProcessors);

	char cpu_brand[64];
	cpu_model_string(cpu_brand, sizeof cpu_brand);
	if (cpu_brand[0]) cJSON_AddStringToObject(m, "cpu_model", cpu_brand);
	else              cJSON_AddNullToObject  (m, "cpu_model");

	/* mem_total_bytes (base 단위). swap 은 block_devices type=swap(pagefile) 로. */
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
	/* boot/nonblock_mounts: Linux GRUB cmdline / mountinfo 개념 -> Windows 는 측정불가 null(필드셋 대칭). */
	cJSON_AddNullToObject(m, "boot");
	cJSON_AddNullToObject(m, "nonblock_mounts");

	return m;
}

static cJSON *bd_add(cJSON *arr, const char *name, const char *type, long long size,
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
	return o;
}

/* ---- 파티션 레이아웃: IOCTL_DISK_GET_DRIVE_LAYOUT_EX(XP/2003+, IOCTL 이라 import 없음) ----
   partition_table(disk 노드) + type='part' 노드 신설(parent=disk id). Linux part 노드와 필드셋 대칭. */

/* Windows GUID -> 소문자 무중괄호(Linux GPT part_type 포맷과 통일). API가 논리 GUID 필드 제공(바이트 스왑 불요). */
static void win_gpt_guid_str(const GUID *g, char *out, size_t outsz)
{
	snprintf(out, outsz, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	    (unsigned long)g->Data1, g->Data2, g->Data3,
	    g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
	    g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

/* GPT type GUID(소문자 무중괄호) -> parted 계열 시맨틱 플래그명. Linux 트리와 동일 표. */
static const char *gpt_type_flag(const char *guid)
{
	if (!strcmp(guid, "c12a7328-f81f-11d2-ba4b-00a0c93ec93b")) return "esp";
	if (!strcmp(guid, "21686148-6449-6e6f-744e-656564454649")) return "bios_grub";
	if (!strcmp(guid, "e6d6d379-f507-44c2-a23c-238f2a3df928")) return "lvm";
	if (!strcmp(guid, "a19d880f-05fc-4d3b-a006-743f0f84911e")) return "raid";
	if (!strcmp(guid, "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f")) return "swap";
	if (!strcmp(guid, "e3c9e316-0b5c-4db8-817d-f92df00215ae")) return "msftres";
	return NULL;
}

/* MBR type 바이트 -> 시맨틱 플래그명. Linux 트리와 동일 표. */
static const char *mbr_type_flag(unsigned char t)
{
	switch (t) {
	case 0x82: return "swap";
	case 0x8e: return "lvm";
	case 0xfd: return "raid";
	case 0xef: return "esp";
	default:   return NULL;
	}
}

/* GPT Name[36](WCHAR, 미종단 가능) -> UTF-8. 빈 문자열이면 out="" */
static void win_gpt_name(const WCHAR *w, char *out, size_t outsz)
{
	int len = 0;
	while (len < 36 && w[len]) len++;
	if (len == 0) { out[0] = '\0'; return; }
	int n = WideCharToMultiByte(CP_UTF8, 0, w, len, out, (int)outsz - 1, NULL, NULL);
	out[(n > 0 && n < (int)outsz) ? n : 0] = '\0';
}

/* IOCTL_DISK_GET_DRIVE_LAYOUT_EX 를 확장 버퍼로 읽어 malloc 버퍼 반환(호출자 free). 실패 NULL */
static BYTE *win_read_drive_layout(HANDLE h)
{
	DWORD cap = 16384; /* 헤더 + 파티션 배열(넉넉) */
	for (int tries = 0; tries < 3; tries++) {
		BYTE *buf = (BYTE *)malloc(cap);
		if (!buf) return NULL;
		DWORD ret = 0;
		if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, buf, cap, &ret, NULL))
			return buf;
		DWORD err = GetLastError();
		free(buf);
		if (err != ERROR_INSUFFICIENT_BUFFER && err != ERROR_MORE_DATA) return NULL;
		cap *= 4;
	}
	return NULL;
}

/* disk 상세: sector_size(geometry)/serial(STORAGE_DEVICE_DESCRIPTOR)/rotational(seek penalty). 핸들 살아있을 때 수집. */
struct wdmeta { long long sector; int have_sector; char serial[160]; int rot; }; /* rot: -1 unknown, 0 ssd, 1 hdd */

static void win_read_disk_meta(HANDLE h, struct wdmeta *m)
{
	m->have_sector = 0; m->serial[0] = '\0'; m->rot = -1;
	DWORD ret = 0;
	DISK_GEOMETRY geo;
	if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &geo, sizeof geo, &ret, NULL) && geo.BytesPerSector) {
		m->sector = geo.BytesPerSector;
		m->have_sector = 1;
	}
	STORAGE_PROPERTY_QUERY q; memset(&q, 0, sizeof q); q.PropertyId = StorageDeviceProperty; q.QueryType = PropertyStandardQuery;
	BYTE sbuf[2048] = {0}; /* 제로초기화: offset 이 반환영역 밖이어도 %s 가 NUL 만나 OOB 방지 */
	if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, sbuf, sizeof sbuf, &ret, NULL)) {
		STORAGE_DEVICE_DESCRIPTOR *sd = (STORAGE_DEVICE_DESCRIPTOR *)sbuf;
		if (sd->SerialNumberOffset && sd->SerialNumberOffset < ret) { /* 반환 바이트수(ret) 내부만 */
			char *s = (char *)sbuf + sd->SerialNumberOffset;
			while (*s == ' ') s++;
			snprintf(m->serial, sizeof m->serial, "%s", s);
			for (int k = (int)strlen(m->serial) - 1; k >= 0 && m->serial[k] == ' '; k--) m->serial[k] = '\0';
		}
	}
	/* StorageDeviceSeekPenaltyProperty=7. IncursSeekPenalty TRUE=HDD. NT5.2/미지원 -> rot 유지(-1) */
	STORAGE_PROPERTY_QUERY q2; memset(&q2, 0, sizeof q2); q2.PropertyId = (STORAGE_PROPERTY_ID)7; q2.QueryType = PropertyStandardQuery;
	struct { DWORD Version; DWORD Size; BOOLEAN IncursSeekPenalty; } sp; memset(&sp, 0, sizeof sp);
	if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q2, sizeof q2, &sp, sizeof sp, &ret, NULL) && ret >= sizeof sp)
		m->rot = sp.IncursSeekPenalty ? 1 : 0;
}

static void win_attach_disk_meta(cJSON *node, const struct wdmeta *m)
{
	if (m->have_sector) cJSON_AddNumberToObject(node, "sector_size", (double)m->sector);
	else                cJSON_AddNullToObject(node, "sector_size");
	if (m->serial[0]) cJSON_AddStringToObject(node, "serial", m->serial);
	else              cJSON_AddNullToObject(node, "serial");
	cJSON_AddNullToObject(node, "wwn"); /* virtio/하이퍼바이저 미제공 -> null */
	if (m->rot < 0) cJSON_AddNullToObject(node, "rotational");
	else            cJSON_AddBoolToObject(node, "rotational", m->rot ? 1 : 0);
}

/* disk 노드에 partition_table 부착 + part 노드 발행. layout NULL 이면 partition_table=null. */
static void win_emit_layout(cJSON *arr, cJSON *dn, const BYTE *layout,
                            const char *parentid, const char *diskname)
{
	if (!layout) { cJSON_AddNullToObject(dn, "partition_table"); return; }
	const DRIVE_LAYOUT_INFORMATION_EX *dl = (const DRIVE_LAYOUT_INFORMATION_EX *)layout;
	if      (dl->PartitionStyle == PARTITION_STYLE_GPT) cJSON_AddStringToObject(dn, "partition_table", "gpt");
	else if (dl->PartitionStyle == PARTITION_STYLE_MBR) cJSON_AddStringToObject(dn, "partition_table", "mbr");
	else                                                cJSON_AddNullToObject(dn, "partition_table");

	DWORD n = dl->PartitionCount;
	if (n > 512) n = 512; /* 방어 상한 */
	for (DWORD i = 0; i < n; i++) {
		const PARTITION_INFORMATION_EX *pe = &dl->PartitionEntry[i];
		if (pe->PartitionNumber == 0) continue;          /* MBR 빈 프라이머리 슬롯/확장 컨테이너 */
		if (pe->PartitionLength.QuadPart == 0) continue;

		char pname[48];
		snprintf(pname, sizeof pname, "%s-part%lu", diskname, (unsigned long)pe->PartitionNumber);
		int is_gpt = (pe->PartitionStyle == PARTITION_STYLE_GPT);
		char idfull[128];
		char tguid[48] = {0};
		if (is_gpt) {
			char pid[48];
			win_gpt_guid_str(&pe->Gpt.PartitionId, pid, sizeof pid);
			snprintf(idfull, sizeof idfull, "partuuid:%s", pid);
		} else {
			snprintf(idfull, sizeof idfull, "name:%s", pname);
		}
		cJSON *pn = bd_add(arr, pname, "part", (long long)pe->PartitionLength.QuadPart,
		                       NULL, NULL, parentid, idfull);
		cJSON_AddNumberToObject(pn, "part_number", (double)pe->PartitionNumber);
		cJSON_AddNumberToObject(pn, "part_start_bytes", (double)pe->StartingOffset.QuadPart);

		if (is_gpt) {
			win_gpt_guid_str(&pe->Gpt.PartitionType, tguid, sizeof tguid);
			cJSON_AddStringToObject(pn, "part_type", tguid);
			char nm[160]; /* GPT Name 36 WCHAR * 3(UTF-8 BMP) + NUL 여유 */
			win_gpt_name(pe->Gpt.Name, nm, sizeof nm);
			if (nm[0]) cJSON_AddStringToObject(pn, "part_name", nm);
			else       cJSON_AddNullToObject(pn, "part_name");
			cJSON *fl = cJSON_CreateArray();
			const char *tf = gpt_type_flag(tguid);
			if (tf) cJSON_AddItemToArray(fl, cJSON_CreateString(tf));
			unsigned long long at = (unsigned long long)pe->Gpt.Attributes;
			if (at & (1ULL << 0))  cJSON_AddItemToArray(fl, cJSON_CreateString("required"));
			if (at & (1ULL << 2))  cJSON_AddItemToArray(fl, cJSON_CreateString("legacy_boot"));
			if (at & (1ULL << 62)) cJSON_AddItemToArray(fl, cJSON_CreateString("hidden"));
			if (at & (1ULL << 63)) cJSON_AddItemToArray(fl, cJSON_CreateString("no_automount"));
			cJSON_AddItemToObject(pn, "part_flags", fl);
		} else if (pe->PartitionStyle == PARTITION_STYLE_MBR) {
			unsigned char t = pe->Mbr.PartitionType;
			char th[8];
			snprintf(th, sizeof th, "0x%02x", t);
			cJSON_AddStringToObject(pn, "part_type", th);
			cJSON_AddNullToObject(pn, "part_name");
			cJSON *fl = cJSON_CreateArray();
			if (pe->Mbr.BootIndicator) cJSON_AddItemToArray(fl, cJSON_CreateString("boot"));
			const char *tf = mbr_type_flag(t);
			if (tf) cJSON_AddItemToArray(fl, cJSON_CreateString(tf));
			cJSON_AddItemToObject(pn, "part_flags", fl);
		} else {
			cJSON_AddNullToObject(pn, "part_type");
			cJSON_AddNullToObject(pn, "part_name");
			cJSON_AddNullToObject(pn, "part_flags");
		}
	}
}

/* fs 메타(volume 노드): fs_uuid=볼륨 시리얼(XXXX-XXXX, Windows 규약) / fs_label / block_size=클러스터.
   mount_options/fs_freq/fs_passno 는 Windows fstab 부재 -> 생략(엔진 OUTPUT null). Linux 트리와 필드셋 대칭. */
static void win_fs_meta(cJSON *node, const char *label, DWORD serial, long long cluster)
{
	if (serial) {
		char u[16];
		snprintf(u, sizeof u, "%04X-%04X", (unsigned)(serial >> 16), (unsigned)(serial & 0xFFFF));
		cJSON_AddStringToObject(node, "fs_uuid", u);
	}
	if (label && label[0]) cJSON_AddStringToObject(node, "fs_label", label);
	if (cluster > 0)       cJSON_AddNumberToObject(node, "block_size", (double)cluster);
}

/* block_devices: 물리디스크(disk, gptid/mbrsig/serial) + 볼륨(volume, volguid, parent=disk extents). */
#define WIN_MAX_PHYSDRIVE 32   /* PhysicalDriveN 스캔 상한(N=0..31) */
static cJSON *inv_collect_block_devices(void)
{
	cJSON *arr = cJSON_CreateArray();
	char diskid[WIN_MAX_PHYSDRIVE][96]; int diskvalid[WIN_MAX_PHYSDRIVE] = {0};
	for (int i = 0; i < WIN_MAX_PHYSDRIVE; i++) {
		HANDLE h = open_physical_drive(i);
		if (h == INVALID_HANDLE_VALUE) continue;
		GET_LENGTH_INFORMATION gli; DWORD ret = 0; long long size = -1;
		if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &gli, sizeof gli, &ret, NULL)) size = (long long)gli.Length.QuadPart;
		BYTE *layout = win_read_drive_layout(h);   /* 핸들 살아있을 때 파티션 레이아웃 확보 */
		struct wdmeta dm; win_read_disk_meta(h, &dm);
		CloseHandle(h);
		char idfull[96]; win_disk_id(i, idfull, sizeof idfull);
		snprintf(diskid[i], sizeof diskid[i], "%s", win_id_value(idfull)); diskvalid[i] = 1;
		char name[32]; snprintf(name, sizeof name, "PhysicalDrive%d", i);
		cJSON *dn = bd_add(arr, name, "disk", size, NULL, NULL, NULL, idfull);
		win_attach_disk_meta(dn, &dm);
		win_emit_layout(arr, dn, layout, diskid[i], name);
		free(layout);
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
			wchar_t labelw[128] = {0}; DWORD volserial = 0;
			GetVolumeInformationW(vol, labelw, 128, &volserial, NULL, NULL, fsw, 16);
			if (fsw[0]) { WideCharToMultiByte(CP_UTF8, 0, fsw, -1, fst, sizeof fst, NULL, NULL); for (char *q = fst; *q; q++) *q = (char)tolower((unsigned char)*q); }
			char vlabel[256] = {0};
			if (labelw[0]) WideCharToMultiByte(CP_UTF8, 0, labelw, -1, vlabel, sizeof vlabel, NULL, NULL);
			DWORD spc = 0, bps = 0, fc = 0, tc = 0; long long cluster = -1;
			if (GetDiskFreeSpaceW(vol, &spc, &bps, &fc, &tc) && spc && bps) cluster = (long long)spc * bps;
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
						const char *par = (dn >= 0 && dn < WIN_MAX_PHYSDRIVE && diskvalid[dn]) ? diskid[dn] : NULL;
						cJSON *vn = bd_add(arr, mnt[0] ? mnt : volu, "volume", vsize, fst[0] ? fst : NULL, mnt[0] ? mnt : NULL, par, idfull);
						win_fs_meta(vn, vlabel, volserial, cluster);
						emitted = 1;
					}
				}
				CloseHandle(vh);
			}
			if (!emitted) { cJSON *vn = bd_add(arr, mnt[0] ? mnt : volu, "volume", vsize, fst[0] ? fst : NULL, mnt[0] ? mnt : NULL, NULL, idfull); win_fs_meta(vn, vlabel, volserial, cluster); }
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

/* 라우트: GetIpForwardTable(IPv4, NT5.2 iphlpapi)에서 ifindex 의 정적(NETMGMT) 비-default 라우트.
   테이블 NULL(조회 실패)=null, 무매칭=[](측정 empty). */
static cJSON *win_iface_routes(const MIB_IPFORWARDTABLE *ipf, DWORD ifindex)
{
	if (!ipf) return NULL;
	cJSON *arr = cJSON_CreateArray();
	for (DWORD i = 0; i < ipf->dwNumEntries; i++) {
		const MIB_IPFORWARDROW *r = &ipf->table[i];
		if (r->dwForwardIfIndex != ifindex) continue;
		if (r->dwForwardDest == 0) continue;          /* default */
		if (r->dwForwardNextHop == 0) continue;       /* 링크 자동 */
		if (r->dwForwardProto != MIB_IPPROTO_NETMGMT) continue; /* 정적만(3) */
		unsigned d = r->dwForwardDest, m = r->dwForwardMask, g = r->dwForwardNextHop;
		unsigned pfx = 0, mm = m;
		while (mm) { pfx += mm & 1; mm >>= 1; }
		char cidr[32], via[16];
		snprintf(cidr, sizeof cidr, "%u.%u.%u.%u/%u", d & 0xFF, (d >> 8) & 0xFF, (d >> 16) & 0xFF, (d >> 24) & 0xFF, pfx);
		snprintf(via, sizeof via, "%u.%u.%u.%u", g & 0xFF, (g >> 8) & 0xFF, (g >> 16) & 0xFF, (g >> 24) & 0xFF);
		cJSON *ro = cJSON_CreateObject();
		cJSON_AddStringToObject(ro, "dest", cidr);
		cJSON_AddStringToObject(ro, "via", via);
		cJSON_AddItemToArray(arr, ro);
	}
	return arr;
}

/* net_interfaces: GAA per-adapter -> MAC id + kind + speed + addresses[] + gateway. */
static cJSON *inv_collect_net_interfaces(void)
{
	cJSON *arr = cJSON_CreateArray();
	/* 정적 라우트 테이블 1회 조회(iface 별 필터) */
	MIB_IPFORWARDTABLE *ipf = NULL;
	{
		ULONG sz = 0;
		if (GetIpForwardTable(NULL, &sz, FALSE) == ERROR_INSUFFICIENT_BUFFER) {
			ipf = (MIB_IPFORWARDTABLE *)malloc(sz);
			if (ipf && GetIpForwardTable(ipf, &sz, FALSE) != NO_ERROR) { free(ipf); ipf = NULL; }
		}
	}
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
	if (agent_is_nt6()) flags |= GAA_FLAG_INCLUDE_GATEWAYS;
	ULONG buf_len = 16 * 1024; IP_ADAPTER_ADDRESSES *aa = malloc(buf_len);
	if (!aa) { if (ipf) free(ipf); return arr; }
	ULONG ret = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, aa, &buf_len);
	if (ret == ERROR_BUFFER_OVERFLOW) { free(aa); aa = malloc(buf_len); if (!aa) { if (ipf) free(ipf); return arr; } ret = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, aa, &buf_len); }
	if (ret != NO_ERROR) { free(aa); if (ipf) free(ipf); return arr; }
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
			/* origin: PrefixOrigin(Vista+). Manual=static, Dhcp=dhcp. NT5.2/기타=null */
			if (agent_is_nt6()) {
				if (u->PrefixOrigin == IpPrefixOriginManual)    cJSON_AddStringToObject(ad, "origin", "static");
				else if (u->PrefixOrigin == IpPrefixOriginDhcp) cJSON_AddStringToObject(ad, "origin", "dhcp");
				else                                            cJSON_AddNullToObject(ad, "origin");
			} else cJSON_AddNullToObject(ad, "origin");
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
		/* mtu */
		if (p->Mtu && p->Mtu != 0xFFFFFFFFUL) cJSON_AddNumberToObject(o, "mtu", (double)p->Mtu);
		else                                  cJSON_AddNullToObject(o, "mtu");
		/* dns: per-adapter DNS 서버(FirstDnsServerAddress, XP+) */
		cJSON *dnsarr = NULL;
		for (IP_ADAPTER_DNS_SERVER_ADDRESS *ds = p->FirstDnsServerAddress; ds; ds = ds->Next) {
			if (!ds->Address.lpSockaddr) continue;
			int df = ds->Address.lpSockaddr->sa_family;
			char db[INET6_ADDRSTRLEN] = {0};
			if (df == AF_INET)       inet_ntop(AF_INET, &((struct sockaddr_in *)ds->Address.lpSockaddr)->sin_addr, db, sizeof db);
			else if (df == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)ds->Address.lpSockaddr)->sin6_addr, db, sizeof db);
			else continue;
			if (!db[0]) continue;
			if (!dnsarr) dnsarr = cJSON_CreateArray();
			cJSON_AddItemToArray(dnsarr, cJSON_CreateString(db));
		}
		cJSON_AddItemToObject(o, "dns", dnsarr ? dnsarr : cJSON_CreateNull());
		/* routes: 정적 비-default(IPv4) */
		cJSON *rts = win_iface_routes(ipf, p->IfIndex ? p->IfIndex : p->Ipv6IfIndex);
		cJSON_AddItemToObject(o, "routes", rts ? rts : cJSON_CreateNull());
		/* bond_mode/vlan_id: Windows 팀ing/VLAN 은 별도 스택 -> null(위조 금지) */
		cJSON_AddNullToObject(o, "bond_mode");
		cJSON_AddNullToObject(o, "vlan_id");
		cJSON_AddItemToArray(arr, o);
	}
	free(aa);
	if (ipf) free(ipf);
	return arr;
}
