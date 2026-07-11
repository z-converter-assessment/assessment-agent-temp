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

static void metrics_collect_cpu(cJSON *root);
static void metrics_collect_disk(cJSON *root);
static void metrics_collect_filesystem(cJSON *root);
static void metrics_collect_memory(cJSON *root);
static void metrics_collect_network(cJSON *root);

cJSON *build_error_payload(const char *machine_id, const char *agent_version,
                           const char *error_code, const char *error_message,
                           const char *failed_component, int retry_count,
                           const char *first_failed_at, const char *recovered_at)
{
	cJSON *m = cJSON_CreateObject();
	if (!m) return NULL;
	wire_add_envelope(m, "error", machine_id, agent_version);
	cJSON_AddStringToObject(m, "error_code",       error_code       ? error_code       : "UNKNOWN");
	cJSON_AddStringToObject(m, "error_message",    error_message    ? error_message    : "");
	cJSON_AddStringToObject(m, "failed_component", failed_component ? failed_component : "agent");
	if (retry_count >= 0)   cJSON_AddNumberToObject(m, "retry_count", retry_count);
	if (first_failed_at)    cJSON_AddStringToObject(m, "first_failed_at", first_failed_at);
	if (recovered_at)       cJSON_AddStringToObject(m, "recovered_at",    recovered_at);
	return m;
}

/* system.cpu: per-cpu = NtQuery(SystemProcessorPerformanceInformation, class 8) 동적버퍼(L2, 고코어 무음드롭 방지).
 * run_queue = perflib System\Processor Queue Length. logical.count = GetNativeSystemInfo. blocked = Windows null. */
static void metrics_collect_cpu(cJSON *root)
{
	cJSON *ns = wire_ns(root, "system.cpu");
	cJSON *m_time = wire_metric(ns, "cpu.time", "counter", "s");
	SYSTEM_INFO si; GetNativeSystemInfo(&si);
	DWORD ncpu = si.dwNumberOfProcessors;

	typedef LONG (WINAPI *NQSI)(ULONG, PVOID, ULONG, PULONG);
	HMODULE nt = GetModuleHandleA("ntdll.dll");
	NQSI f = nt ? (NQSI)(void *)GetProcAddress(nt, "NtQuerySystemInformation") : NULL;
	if (f) {
		ULONG cap = ncpu * 64 + 512, ret = 0;
		BYTE *buf = (BYTE *)malloc(cap);
		if (buf) {
			LONG st = f(8, buf, cap, &ret);
			if (st == (LONG)0xC0000004UL && ret > cap) {   /* LENGTH_MISMATCH -> 동적 확장 */
				BYTE *nb = (BYTE *)realloc(buf, ret);
				if (nb) { buf = nb; cap = ret; st = f(8, buf, cap, &ret); }
			}
			if (st == 0) {
				/* 엔트리(x86): IdleTime(8) KernelTime(8) UserTime(8) DpcTime(8) InterruptTime(8) InterruptCount(4)+pad -> 48B. */
				const size_t ENT = 48;
				DWORD have = (DWORD)(ret / ENT);
				if (have > ncpu) have = ncpu;
				for (DWORD i = 0; i < have; i++) {
					BYTE *e = buf + (size_t)i * ENT;
					long long idle   = *(long long *)(e + 0);
					long long kernel = *(long long *)(e + 8);
					long long user   = *(long long *)(e + 16);
					long long sys    = kernel - idle;   /* Windows KernelTime 은 idle 포함 */
					char idxs[16]; snprintf(idxs, sizeof idxs, "%lu", (unsigned long)i);
					cJSON *p;
					p = wire_point(m_time); wire_point_attr(p, "cpu", idxs); wire_point_attr(p, "state", "user");   wire_point_value(p, (double)user / 1e7);
					p = wire_point(m_time); wire_point_attr(p, "cpu", idxs); wire_point_attr(p, "state", "system"); wire_point_value(p, (double)(sys < 0 ? 0 : sys) / 1e7);
					p = wire_point(m_time); wire_point_attr(p, "cpu", idxs); wire_point_attr(p, "state", "idle");   wire_point_value(p, (double)idle / 1e7);
				}
			}
			free(buf);
		}
	}

	/* run_queue = perflib System\Processor Queue Length. */
	int have_rq = 0; unsigned long long rq = 0;
	DWORD i_sys = perf_index("System");
	if (i_sys) {
		char vn[16]; snprintf(vn, sizeof vn, "%lu", (unsigned long)i_sys);
		BYTE *pb = perf_query(vn);
		RegCloseKey(HKEY_PERFORMANCE_DATA);
		if (pb) {
			PERF_DATA_BLOCK *db = (PERF_DATA_BLOCK *)pb;
			PERF_OBJECT_TYPE *o = perf_object(db, i_sys);
			if (o && o->NumInstances == PERF_NO_INSTANCES) {
				PERF_COUNTER_BLOCK *cb = (PERF_COUNTER_BLOCK *)((BYTE *)o + o->DefinitionLength);
				PERF_COUNTER_DEFINITION *c = perf_counter(o, perf_index("Processor Queue Length"));
				if (c) { rq = perf_read(cb, c); have_rq = 1; }
			}
			free(pb);
		}
	}
	cJSON *m_rq = wire_metric(ns, "cpu.run_queue", "gauge", "tasks");
	{ cJSON *p = wire_point(m_rq); wire_point_attr(p, "source", "processor_queue");
	  if (have_rq) wire_point_value(p, (double)rq); else wire_point_null(p); }
	/* blocked: Windows 개념 없음 -> null. */
	cJSON *m_bl = wire_metric(ns, "cpu.blocked", "gauge", "tasks");
	wire_point_null(wire_point(m_bl));
	cJSON *m_lc = wire_metric(ns, "cpu.logical.count", "gauge", "cpu");
	wire_point_value(wire_point(m_lc), (double)ncpu);
}

/* system.memory + paging: GlobalMemoryStatusEx + perflib Memory(commit/pages). */
static void metrics_collect_memory(cJSON *root)
{
	cJSON *ns = wire_ns(root, "system.memory");
	cJSON *usage = wire_metric(ns, "memory.usage", "gauge", "By");
	MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
	int okmem = GlobalMemoryStatusEx(&ms);
	long long freek = query_free_kb();   /* 진짜 free(NT6+), 실패 -1 */
	{ cJSON *p = wire_point(usage); wire_point_attr(p, "state", "available");
	  if (okmem) wire_point_value(p, (double)ms.ullAvailPhys); else wire_point_null(p); }
	{ cJSON *p = wire_point(usage); wire_point_attr(p, "state", "free");
	  if (freek >= 0) wire_point_value(p, (double)freek * 1024.0); else wire_point_null(p); }
	wire_metric_scalar(ns, "memory.limit", "gauge", "By", okmem, (double)ms.ullTotalPhys);

	/* commit + paging: perflib Memory (단일 인스턴스). */
	int have_cu = 0, have_cl = 0, have_pin = 0, have_pout = 0;
	unsigned long long cu = 0, cl = 0, pin = 0, pout = 0;
	DWORD i_mem = perf_index("Memory");
	if (i_mem) {
		char vn[16]; snprintf(vn, sizeof vn, "%lu", (unsigned long)i_mem);
		BYTE *pb = perf_query(vn);
		RegCloseKey(HKEY_PERFORMANCE_DATA);
		if (pb) {
			PERF_DATA_BLOCK *db = (PERF_DATA_BLOCK *)pb;
			PERF_OBJECT_TYPE *o = perf_object(db, i_mem);
			if (o && o->NumInstances == PERF_NO_INSTANCES) {
				PERF_COUNTER_BLOCK *cb = (PERF_COUNTER_BLOCK *)((BYTE *)o + o->DefinitionLength);
				PERF_COUNTER_DEFINITION *c;
				if ((c = perf_counter(o, perf_index("Committed Bytes"))))   { cu = perf_read(cb, c); have_cu = 1; }
				if ((c = perf_counter(o, perf_index("Commit Limit"))))      { cl = perf_read(cb, c); have_cl = 1; }
				if ((c = perf_counter(o, perf_index("Pages Input/sec"))))   { pin = perf_read(cb, c); have_pin = 1; }
				if ((c = perf_counter(o, perf_index("Pages Output/sec"))))  { pout = perf_read(cb, c); have_pout = 1; }
			}
			free(pb);
		}
	}
	wire_metric_scalar(ns, "memory.commit.usage", "gauge", "By", have_cu, (double)cu);
	wire_metric_scalar(ns, "memory.commit.limit", "gauge", "By", have_cl, (double)cl);
	/* oom_kill/hardware_corrupted/edac: Windows 개념 없음 -> null. edac 는 correctable/
	 * uncorrectable 2점 null 로 발행해 Linux 트리와 필드셋 패리티를 맞춘다(값 위조 아님, 측정불가=null). */
	wire_metric_scalar(ns, "memory.oom_kill", "counter", "events", 0, 0);
	wire_metric_scalar(ns, "memory.hardware_corrupted", "gauge", "By", 0, 0);
	{
		cJSON *edac = wire_metric(ns, "memory.edac", "counter", "errors");
		cJSON *ep;
		ep = wire_point(edac); wire_point_attr(ep, "type", "correctable");   wire_point_null(ep);
		ep = wire_point(edac); wire_point_attr(ep, "type", "uncorrectable"); wire_point_null(ep);
	}

	cJSON *pns = wire_ns(root, "system.paging");
	cJSON *po = wire_metric(pns, "paging.operations", "counter", "operations");
	if (have_pin)  { cJSON *p = wire_point(po); wire_point_attr(p, "direction", "in");  wire_point_value(p, (double)pin); }
	if (have_pout) { cJSON *p = wire_point(po); wire_point_attr(p, "direction", "out"); wire_point_value(p, (double)pout); }
}

/* system.disk: throughput=NtQuery(시스템 전역·diskperf 독립·단조, device PhysicalDrive0),
 * saturation(io_time/operation_time/pending)=perflib PhysicalDisk per-disk + 유효성 게이트(raw!=0/idle>0, 가짜0 금지).
 * NOTE(accept 게이트): perflib counter-type 수식(% Idle Time=100ns 역산, Avg.Disk sec=PERF_AVERAGE_TIMER raw/PerfFreq)의
 * 값 정확성은 dp-win2016 raw-vs-cooked 대조로 검증 필요. divisor(PerfFreq vs 1e7)는 그 대조로 확정. */
static void metrics_collect_disk(cJSON *root)
{
	cJSON *ns    = wire_ns(root, "system.disk");
	cJSON *m_io  = wire_metric(ns, "disk.io",             "counter", "By");
	cJSON *m_ops = wire_metric(ns, "disk.operations",     "counter", "operations");
	cJSON *m_iot = wire_metric(ns, "disk.io_time",        "counter", "s");
	cJSON *m_opt = wire_metric(ns, "disk.operation_time", "counter", "s");
	cJSON *m_pnd = wire_metric(ns, "disk.pending_operations", "gauge", "operations");

	/* NtQuery 는 시스템 전역 집계(I/O 매니저) — 물리 디스크별 아님. perflib per-disk(PhysicalDriveN)와
	 * device 키 충돌 방지 위해 'aggregate:system'. await 는 perflib per-disk operation_time/operations 로 페어. */
	unsigned long long rop = 0, wop = 0, rby = 0, wby = 0;
	if (query_system_io(&rop, &wop, &rby, &wby)) {
		wire_point_dev_dir(m_io, "aggregate:system", "read", (double)rby);
		wire_point_dev_dir(m_io, "aggregate:system", "write", (double)wby);
		wire_point_dev_dir(m_ops, "aggregate:system", "read", (double)rop);
		wire_point_dev_dir(m_ops, "aggregate:system", "write", (double)wop);
	}

	DWORD i_pd = perf_index("PhysicalDisk");
	if (!i_pd) return;
	char vn[16]; snprintf(vn, sizeof vn, "%lu", (unsigned long)i_pd);
	BYTE *buf = perf_query(vn);
	RegCloseKey(HKEY_PERFORMANCE_DATA);
	if (!buf) return;
	PERF_DATA_BLOCK *db = (PERF_DATA_BLOCK *)buf;
	long long perf_freq = db->PerfFreq.QuadPart;
	PERF_OBJECT_TYPE *o = perf_object(db, i_pd);
	if (o && o->NumInstances > 0) {
		PERF_COUNTER_DEFINITION *c_idle = perf_counter(o, perf_index("% Idle Time"));
		PERF_COUNTER_DEFINITION *c_rt   = perf_counter(o, perf_index("Avg. Disk sec/Read"));
		PERF_COUNTER_DEFINITION *c_wt   = perf_counter(o, perf_index("Avg. Disk sec/Write"));
		PERF_COUNTER_DEFINITION *c_q    = perf_counter(o, perf_index("Current Disk Queue Length"));
		PERF_COUNTER_DEFINITION *c_ro   = perf_counter(o, perf_index("Disk Reads/sec"));
		PERF_COUNTER_DEFINITION *c_wo   = perf_counter(o, perf_index("Disk Writes/sec"));
		PERF_COUNTER_DEFINITION *c_rb   = perf_counter(o, perf_index("Disk Read Bytes/sec"));
		PERF_COUNTER_DEFINITION *c_wb   = perf_counter(o, perf_index("Disk Write Bytes/sec"));
		PERF_INSTANCE_DEFINITION *inst = (PERF_INSTANCE_DEFINITION *)((BYTE *)o + o->DefinitionLength);
		for (LONG i = 0; i < o->NumInstances; i++) {
			wchar_t *wname = (wchar_t *)((BYTE *)inst + inst->NameOffset);
			PERF_COUNTER_BLOCK *cb = (PERF_COUNTER_BLOCK *)((BYTE *)inst + inst->ByteLength);
			int drive = perf_disk_num(wname);
			if (drive >= 0) {
				char dev[32]; snprintf(dev, sizeof dev, "name:PhysicalDrive%d", drive);
				cJSON *p;
				/* io_time(busy s) = uptime(부팅기준 s) - idle(% Idle Time raw, 부팅기준 100ns->s). idle 누적기는
				 * PDH % Idle Time raw 와 동일값임을 값검증에서 확인(idx=1482, agent idle==PDH RawValue, Δ 일치).
				 * uptime 기준이라 재부팅 시 idle 과 함께 0 리셋 -> 엔진이 counter reset 로 깨끗이 감지(1601 기준
				 * PerfTime100nSec 는 절대값 425년 + 재부팅 시 spike 라 안 씀). 게이트: idle>0(diskperf 수집중). */
				unsigned long long idle = c_idle ? perf_read(cb, c_idle) : 0;
				p = wire_point(m_iot); wire_point_attr(p, "device", dev);
				if (c_idle && idle > 0) {
					double busy = (double)monotonic_ms() / 1000.0 - (double)idle / 1e7;
					/* busy<0 은 시계 skew(uptime<idle) — 측정 불가라 null(가짜 0 금지). */
					if (busy < 0) wire_point_null(p); else wire_point_value(p, busy);
				} else wire_point_null(p);
				/* operation_time(await 분자, saturation): perflib 는 diskperf 의존이라 raw>0 일 때만 값,
				   아니면 null(가짜 0 금지). io_time 의 idle>0 게이트와 대칭 — raw=0 을 실측 0초로 발행하면
				   엔진이 await=0(포화 없음)으로 오판한다. */
				unsigned long long rt_raw = c_rt ? perf_read(cb, c_rt) : 0;
				p = wire_point(m_opt); wire_point_attr(p, "device", dev); wire_point_attr(p, "direction", "read");
				if (c_rt && perf_freq > 0 && rt_raw > 0) wire_point_value(p, (double)rt_raw / (double)perf_freq); else wire_point_null(p);
				unsigned long long wt_raw = c_wt ? perf_read(cb, c_wt) : 0;
				p = wire_point(m_opt); wire_point_attr(p, "device", dev); wire_point_attr(p, "direction", "write");
				if (c_wt && perf_freq > 0 && wt_raw > 0) wire_point_value(p, (double)wt_raw / (double)perf_freq); else wire_point_null(p);
				p = wire_point(m_ops); wire_point_attr(p, "device", dev); wire_point_attr(p, "direction", "read");
				if (c_ro) wire_point_value(p, (double)perf_read(cb, c_ro)); else wire_point_null(p);
				p = wire_point(m_ops); wire_point_attr(p, "device", dev); wire_point_attr(p, "direction", "write");
				if (c_wo) wire_point_value(p, (double)perf_read(cb, c_wo)); else wire_point_null(p);
				/* disk.io per-disk bytes(계약: aggregate:system NtQuery + perflib per-disk Read/Write Bytes/sec).
				   disk.operations 형제와 동일 패턴 - Linux per-device disk.io 와 대칭. m_io 는 aggregate:system 점도 함께 갖는다. */
				p = wire_point(m_io); wire_point_attr(p, "device", dev); wire_point_attr(p, "direction", "read");
				if (c_rb) wire_point_value(p, (double)perf_read(cb, c_rb)); else wire_point_null(p);
				p = wire_point(m_io); wire_point_attr(p, "device", dev); wire_point_attr(p, "direction", "write");
				if (c_wb) wire_point_value(p, (double)perf_read(cb, c_wb)); else wire_point_null(p);
				p = wire_point(m_pnd); wire_point_attr(p, "device", dev);
				if (c_q) wire_point_value(p, (double)perf_read(cb, c_q)); else wire_point_null(p);
			}
			inst = (PERF_INSTANCE_DEFINITION *)((BYTE *)cb + cb->ByteLength);
		}
	}
	free(buf);
}

/* E축(disk): Linux 는 mdraid mismatch/ext4 errors_count 를 발행하나 Windows 는 그 소스 개념이
 * 부재하다(eventlog/WHEA 파싱 미구현). disk.errors metric 키만 두어 Linux 와 필드셋 패리티를
 * 맞춘다 — points 는 소스 부재라 빈 배열(값 위조 아님, Linux 도 md/ext4 부재 시 빈 metric). */
static void metrics_collect_disk_errors(cJSON *root)
{
	cJSON *ns = wire_ns(root, "system.disk");
	wire_metric(ns, "disk.errors", "counter", "errors");
}

/* system.network: GetIfTable2(NT6, GetProcAddress) / GetIfTable(NT5.2 폴백). device=MAC.
 * io/packets/errors/dropped/link.speed + tcp.retransmits(GetTcpStatistics). conntrack 는 Windows 부재. */
static void metrics_collect_network(cJSON *root)
{
	cJSON *ns   = wire_ns(root, "system.network");
	cJSON *m_io = wire_metric(ns, "network.io",         "counter", "By");
	cJSON *m_pk = wire_metric(ns, "network.packets",    "counter", "packets");
	cJSON *m_er = wire_metric(ns, "network.errors",     "counter", "errors");
	cJSON *m_dr = wire_metric(ns, "network.dropped",    "counter", "packets");
	cJSON *m_sp = wire_metric(ns, "network.link.speed", "gauge",   "bit/s");

	typedef DWORD (WINAPI *GetIfTable2_fn)(PMIB_IF_TABLE2 *);
	typedef void  (WINAPI *FreeMibTable_fn)(PVOID);
	static int resolved = 0; static GetIfTable2_fn p_get = NULL; static FreeMibTable_fn p_free = NULL;
	if (!resolved) { resolved = 1; HMODULE ip = GetModuleHandleA("iphlpapi.dll");
		if (ip) { p_get = (GetIfTable2_fn)(void *)GetProcAddress(ip, "GetIfTable2"); p_free = (FreeMibTable_fn)(void *)GetProcAddress(ip, "FreeMibTable"); } }

	if (p_get && p_free) {
		PMIB_IF_TABLE2 table = NULL;
		if (p_get(&table) == NO_ERROR && table) {
			for (ULONG i = 0; i < table->NumEntries; i++) {
				MIB_IF_ROW2 *r = &table->Table[i];
				if (r->Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
				if (r->OperStatus != IfOperStatusUp) continue;
				if (r->InterfaceAndOperStatusFlags.FilterInterface) continue;
				/* MAC-less 폴백 키를 inventory(if<IfIndex>)·NT5.2 metrics 와 통일 — Alias 는 개명 가능·비조인이라 안 쓴다. */
				char fb[32]; snprintf(fb, sizeof fb, "if%lu", (unsigned long)r->InterfaceIndex);
				char id[80]; mac_to_devid(r->PhysicalAddress, r->PhysicalAddressLength, fb, id, sizeof id);
				cJSON *p;
				wire_point_dev_dir(m_io, id, "receive", (double)r->InOctets);
				wire_point_dev_dir(m_io, id, "transmit", (double)r->OutOctets);
				wire_point_dev_dir(m_pk, id, "receive", (double)(r->InUcastPkts + r->InNUcastPkts));
				wire_point_dev_dir(m_pk, id, "transmit", (double)(r->OutUcastPkts + r->OutNUcastPkts));
				wire_point_dev_dir(m_er, id, "receive", (double)r->InErrors);
				wire_point_dev_dir(m_er, id, "transmit", (double)r->OutErrors);
				wire_point_dev_dir(m_dr, id, "receive", (double)r->InDiscards);
				wire_point_dev_dir(m_dr, id, "transmit", (double)r->OutDiscards);
				p=wire_point(m_sp); wire_point_attr(p,"device",id);
				ULONG64 spd = r->ReceiveLinkSpeed ? r->ReceiveLinkSpeed : r->TransmitLinkSpeed;
				if (spd > 0 && spd != 0xFFFFFFFFFFFFFFFFULL) wire_point_value(p,(double)spd); else wire_point_null(p);
			}
			p_free(table);
		}
	} else {
		ULONG sz = 0;
		if (GetIfTable(NULL, &sz, FALSE) == ERROR_INSUFFICIENT_BUFFER) {
			MIB_IFTABLE *t = (MIB_IFTABLE *)malloc(sz);
			if (t && GetIfTable(t, &sz, FALSE) == NO_ERROR) {
				for (DWORD i = 0; i < t->dwNumEntries; i++) {
					MIB_IFROW *r = &t->table[i];
					if (r->dwType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
					char nm[32]; snprintf(nm, sizeof nm, "if%lu", (unsigned long)r->dwIndex);
					char id[80]; mac_to_devid(r->bPhysAddr, r->dwPhysAddrLen, nm, id, sizeof id);
					cJSON *p;
					wire_point_dev_dir(m_io, id, "receive", (double)r->dwInOctets);
					wire_point_dev_dir(m_io, id, "transmit", (double)r->dwOutOctets);
					wire_point_dev_dir(m_pk, id, "receive", (double)((double)r->dwInUcastPkts + r->dwInNUcastPkts));
					wire_point_dev_dir(m_pk, id, "transmit", (double)((double)r->dwOutUcastPkts + r->dwOutNUcastPkts));
					wire_point_dev_dir(m_er, id, "receive", (double)r->dwInErrors);
					wire_point_dev_dir(m_er, id, "transmit", (double)r->dwOutErrors);
					wire_point_dev_dir(m_dr, id, "receive", (double)r->dwInDiscards);
					wire_point_dev_dir(m_dr, id, "transmit", (double)r->dwOutDiscards);
					p=wire_point(m_sp); wire_point_attr(p,"device",id);
					if (r->dwSpeed > 0) wire_point_value(p,(double)r->dwSpeed); else wire_point_null(p);
				}
			}
			free(t);
		}
	}
	MIB_TCPSTATS ts;
	if (GetTcpStatistics(&ts) == NO_ERROR)
		wire_metric_scalar(ns, "network.tcp.retransmits", "counter", "segments", 1, (double)ts.dwRetransSegs);
	else
		wire_metric_scalar(ns, "network.tcp.retransmits", "counter", "segments", 0, 0);
	/* conntrack: Windows 개념 부재 -> null 2점(Linux 필드셋 패리티, 값 위조 아님). */
	wire_metric_scalar(ns, "network.conntrack.usage", "gauge", "entries", 0, 0);
	wire_metric_scalar(ns, "network.conntrack.limit", "gauge", "entries", 0, 0);
}

/* system.filesystem: 고정 볼륨 usage(used/free). NTFS 는 inode 개념 부재 -> inodes null. */
static void metrics_collect_filesystem(cJSON *root)
{
	cJSON *ns = wire_ns(root, "system.filesystem");
	cJSON *m_use = wire_metric(ns, "filesystem.usage",  "gauge", "By");
	cJSON *m_ino = wire_metric(ns, "filesystem.inodes", "gauge", "count");
	wchar_t drives[256];
	DWORD cap = (DWORD)(sizeof drives / sizeof drives[0]);
	DWORD len = GetLogicalDriveStringsW(cap, drives);
	if (len == 0 || len > cap) return;
	for (wchar_t *p = drives; *p; p += wcslen(p) + 1) {
		if (GetDriveTypeW(p) != DRIVE_FIXED) continue;
		char mount[16] = {0}; WideCharToMultiByte(CP_UTF8, 0, p, -1, mount, sizeof mount, NULL, NULL);
		size_t ml = strlen(mount); if (ml && mount[ml-1] == '\\') mount[ml-1] = '\0';
		ULARGE_INTEGER avail, total, tfree;
		if (!GetDiskFreeSpaceExW(p, &avail, &total, &tfree)) continue;
		cJSON *pt;
		pt=wire_point(m_use); wire_point_attr(pt,"mountpoint",mount); wire_point_attr(pt,"state","used"); wire_point_value(pt,(double)total.QuadPart - (double)tfree.QuadPart);
		pt=wire_point(m_use); wire_point_attr(pt,"mountpoint",mount); wire_point_attr(pt,"state","free"); wire_point_value(pt,(double)avail.QuadPart);
		pt=wire_point(m_ino); wire_point_attr(pt,"mountpoint",mount); wire_point_attr(pt,"state","used"); wire_point_null(pt);
	}
}

cJSON *collect_metrics_payload(const char *machine_id, const char *agent_version)
{
	cJSON *m = cJSON_CreateObject();
	if (!m) return NULL;
	wire_add_envelope(m, "metrics", machine_id, agent_version);
	cJSON_AddNumberToObject(m, "collection_interval_sec", agent_interval_sec());

	metrics_collect_cpu(m);
	metrics_collect_memory(m);   /* + system.paging */
	metrics_collect_network(m);
	metrics_collect_disk(m);
	metrics_collect_disk_errors(m);         /* disk.errors (E축, Linux 필드셋 패리티) */
	metrics_collect_filesystem(m);
	cJSON_AddNullToObject(m, "system.pressure");   /* Windows: PSI 부재(스키마 강제 null) */
	cJSON_AddNullToObject(m, "system.cgroup");     /* Windows: cgroup 부재 */

	return m;
}
