#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "collect.h"
#include "util.h"

#include <cJSON.h>
#include <openssl/evp.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#define AGENT_VERSION_FALLBACK "0.0.0-dev"
#include "collect_internal.h"

static void metrics_collect_cgroup(cJSON *root);
static void metrics_collect_cpu(cJSON *root);
static void metrics_collect_disk(cJSON *root);
static void metrics_collect_disk_errors(cJSON *root);
static void metrics_collect_filesystem(cJSON *root);
static void metrics_collect_memory(cJSON *root);
static void metrics_collect_memory_errors(cJSON *root);
static void metrics_collect_network(cJSON *root);
static void metrics_collect_pressure(cJSON *root);

/* v2 system.disk: /proc/diskstats raw counters(base 단위). throughput+io_time(%util)+operation_time(await)+queue.
 * diskstats(name 이후): f1 reads f2 rd_merge f3 sectors_rd f4 ms_rd | f5 writes f6 wr_merge f7 sectors_wr f8 ms_wr | f9 in_flight f10 io_ticks f11 weighted. */
static void metrics_collect_disk(cJSON *root)
{
	cJSON *ns    = wire_ns(root, "system.disk");
	cJSON *m_io  = wire_metric(ns, "disk.io",             "counter", "By");
	cJSON *m_ops = wire_metric(ns, "disk.operations",     "counter", "operations");
	cJSON *m_iot = wire_metric(ns, "disk.io_time",        "counter", "s");
	cJSON *m_opt = wire_metric(ns, "disk.operation_time", "counter", "s");
	cJSON *m_pnd = wire_metric(ns, "disk.pending_operations", "gauge", "operations");

	char *content = read_file_all("/proc/diskstats");
	if (!content) return;
	char *save = NULL;
	for (char *line = strtok_r(content, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		long major = 0, minor = 0;
		char dev[64] = { 0 };
		long rc = 0, rm = 0, sr = 0, tr = 0, wc = 0, wm = 0, sw = 0, tw = 0,
		     inflight = 0, ticks = 0, weighted = 0;
		int n = sscanf(line, "%ld %ld %63s %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld",
		               &major, &minor, dev, &rc, &rm, &sr, &tr,
		               &wc, &wm, &sw, &tw, &inflight, &ticks, &weighted);
		if (n < 7) continue;
		if (is_excluded_block_dev(dev)) continue;
		char sp[256];
		snprintf(sp, sizeof sp, "/sys/block/%s", dev);
		if (access(sp, F_OK) != 0) continue;

		char id[300];
		disk_device_id(dev, id, sizeof id);
		cJSON *p;
		wire_point_dev_dir(m_io, id, "read", (double)sr * 512.0);
		if (n >= 10) { wire_point_dev_dir(m_io, id, "write", (double)sw * 512.0); }
		wire_point_dev_dir(m_ops, id, "read", (double)rc);
		if (n >= 8)  { wire_point_dev_dir(m_ops, id, "write", (double)wc); }
		p = wire_point(m_iot); wire_point_attr(p, "device", id);
		if (n >= 13) wire_point_value(p, (double)ticks / 1000.0); else wire_point_null(p);
		wire_point_dev_dir(m_opt, id, "read", (double)tr / 1000.0);
		if (n >= 11) { wire_point_dev_dir(m_opt, id, "write", (double)tw / 1000.0); }
		p = wire_point(m_pnd); wire_point_attr(p, "device", id);
		if (n >= 12) wire_point_value(p, (double)inflight); else wire_point_null(p);
	}
	free(content);
}

/* P2 system.cpu: /proc/stat per-cpu jiffies(->s) + run_queue/blocked + logical.count. */
static void metrics_collect_cpu(cJSON *root)
{
	cJSON *ns = wire_ns(root, "system.cpu");
	cJSON *m_time = wire_metric(ns, "cpu.time", "counter", "s");
	long clk = sysconf(_SC_CLK_TCK);
	if (clk <= 0) clk = 100;
	static const char *const st[8] = { "user","nice","system","idle","iowait","irq","softirq","steal" };

	char *content = read_file_all("/proc/stat");
	long procs_running = -1, procs_blocked = -1;
	int ncpu = 0;
	if (content) {
		char *save = NULL;
		for (char *line = strtok_r(content, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			if (strncmp(line, "cpu", 3) == 0 && line[3] >= '0' && line[3] <= '9') {
				int idx = (int)strtol(line + 3, NULL, 10);
				long c[8] = { 0 };
				const char *sp = line;
				while (*sp && *sp != ' ') sp++;   /* skip "cpuN" */
				int cn = sscanf(sp, "%ld %ld %ld %ld %ld %ld %ld %ld",
				                &c[0],&c[1],&c[2],&c[3],&c[4],&c[5],&c[6],&c[7]);
				if (cn < 4) continue;
				ncpu++;
				char idxs[16]; snprintf(idxs, sizeof idxs, "%d", idx);
				for (int k = 0; k < cn; k++) {
					cJSON *p = wire_point(m_time);
					wire_point_attr(p, "cpu", idxs); wire_point_attr(p, "state", st[k]);
					wire_point_value(p, (double)c[k] / (double)clk);
				}
			} else if (strncmp(line, "procs_running ", 14) == 0) {
				procs_running = strtol(line + 14, NULL, 10);
			} else if (strncmp(line, "procs_blocked ", 14) == 0) {
				procs_blocked = strtol(line + 14, NULL, 10);
			}
		}
		free(content);
	}
	wire_metric_scalar(ns, "cpu.run_queue", "gauge", "tasks", procs_running >= 0, (double)procs_running);
	/* run_queue point 에 source 라벨 */
	{ cJSON *rq = cJSON_GetObjectItemCaseSensitive(ns, "cpu.run_queue");
	  cJSON *pts = cJSON_GetObjectItemCaseSensitive(rq, "points");
	  if (cJSON_GetArraySize(pts)) wire_point_attr(cJSON_GetArrayItem(pts,0), "source", "procs_running"); }
	wire_metric_scalar(ns, "cpu.blocked", "gauge", "tasks", procs_blocked >= 0, (double)procs_blocked);
	{ cJSON *bq = cJSON_GetObjectItemCaseSensitive(ns, "cpu.blocked");
	  cJSON *pts = cJSON_GetObjectItemCaseSensitive(bq, "points");
	  if (cJSON_GetArraySize(pts)) wire_point_attr(cJSON_GetArrayItem(pts,0), "source", "procs_blocked"); }
	wire_metric_scalar(ns, "cpu.logical.count", "gauge", "cpu", ncpu > 0, (double)ncpu);
}

/* P2 system.memory + paging: /proc/meminfo + /proc/vmstat. base 단위 By. */
static void metrics_collect_memory(cJSON *root)
{
	char *mi = read_file_all("/proc/meminfo");
	cJSON *ns = wire_ns(root, "system.memory");
	cJSON *usage = wire_metric(ns, "memory.usage", "gauge", "By");
	long total = mi ? meminfo_get_kb(mi, "MemTotal") : -1;
	long avail = mi ? meminfo_get_kb(mi, "MemAvailable") : -1;
	long freek = mi ? meminfo_get_kb(mi, "MemFree") : -1;
	long cached = mi ? meminfo_get_kb(mi, "Cached") : -1;
	long buffers = mi ? meminfo_get_kb(mi, "Buffers") : -1;
	long committed = mi ? meminfo_get_kb(mi, "Committed_AS") : -1;
	long commitlim = mi ? meminfo_get_kb(mi, "CommitLimit") : -1;
	long hwcorrupt = mi ? meminfo_get_kb(mi, "HardwareCorrupted") : -1;
	struct { const char *st; long kb; } states[] = {
		{ "free", freek }, { "cached", cached }, { "buffers", buffers }, { "available", avail },
	};
	for (size_t i = 0; i < sizeof states / sizeof states[0]; i++) {
		cJSON *p = wire_point(usage); wire_point_attr(p, "state", states[i].st);
		if (states[i].kb >= 0) wire_point_value(p, (double)states[i].kb * 1024.0); else wire_point_null(p);
	}
	wire_metric_scalar(ns, "memory.limit",        "gauge", "By", total >= 0,     (double)total * 1024.0);
	wire_metric_scalar(ns, "memory.commit.usage", "gauge", "By", committed >= 0, (double)committed * 1024.0);
	wire_metric_scalar(ns, "memory.commit.limit", "gauge", "By", commitlim >= 0, (double)commitlim * 1024.0);
	wire_metric_scalar(ns, "memory.hardware_corrupted", "gauge", "By", hwcorrupt >= 0, (double)hwcorrupt * 1024.0);
	free(mi);

	/* vmstat: oom_kill(4.13+) + swap in/out + major fault. */
	char *vm = read_file_all("/proc/vmstat");
	long oom = -1, pin = -1, pout = -1, pmaj = -1;
	if (vm) {
		char *save = NULL;
		for (char *line = strtok_r(vm, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			if      (!strncmp(line, "oom_kill ", 9))    oom  = strtol(line + 9, NULL, 10);
			else if (!strncmp(line, "pswpin ", 7))      pin  = strtol(line + 7, NULL, 10);
			else if (!strncmp(line, "pswpout ", 8))     pout = strtol(line + 8, NULL, 10);
			else if (!strncmp(line, "pgmajfault ", 11)) pmaj = strtol(line + 11, NULL, 10);
		}
		free(vm);
	}
	wire_metric_scalar(ns, "memory.oom_kill", "counter", "events", oom >= 0, (double)oom);

	cJSON *pns = wire_ns(root, "system.paging");
	cJSON *po = wire_metric(pns, "paging.operations", "counter", "operations");
	if (pin >= 0)  { cJSON *p = wire_point(po); wire_point_attr(p, "direction", "in");  wire_point_value(p, (double)pin); }
	if (pout >= 0) { cJSON *p = wire_point(po); wire_point_attr(p, "direction", "out"); wire_point_value(p, (double)pout); }
	if (pmaj >= 0) { cJSON *p = wire_point(po); wire_point_attr(p, "direction", "in"); wire_point_attr(p, "type", "major"); wire_point_value(p, (double)pmaj); }
}

/* P3 system.network: /proc/net/dev per-iface(io/packets/errors/dropped) + link.speed(util 분모)
 * + tcp.retransmits(전역) + conntrack. device attr=MAC 안정키. lo 제외. */
static void metrics_collect_network(cJSON *root)
{
	cJSON *ns   = wire_ns(root, "system.network");
	cJSON *m_io = wire_metric(ns, "network.io",         "counter", "By");
	cJSON *m_pk = wire_metric(ns, "network.packets",    "counter", "packets");
	cJSON *m_er = wire_metric(ns, "network.errors",     "counter", "errors");
	cJSON *m_dr = wire_metric(ns, "network.dropped",    "counter", "packets");
	cJSON *m_sp = wire_metric(ns, "network.link.speed", "gauge",   "bit/s");

	char *content = read_file_all("/proc/net/dev");
	if (content) {
		int line_no = 0; char *save = NULL;
		for (char *line = strtok_r(content, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			if (++line_no <= 2) continue;
			char *colon = strchr(line, ':'); if (!colon) continue;
			*colon = '\0';
			char *iface = line; while (*iface == ' ' || *iface == '\t') iface++;
			if (strcmp(iface, "lo") == 0) continue;
			long rb=0,rp=0,re=0,rd=0,tb=0,tp=0,te=0,td=0;
			int n = sscanf(colon + 1, "%ld %ld %ld %ld %*d %*d %*d %*d %ld %ld %ld %ld",
			               &rb,&rp,&re,&rd,&tb,&tp,&te,&td);
			if (n < 8) continue;
			char id[320]; net_device_id(iface, id, sizeof id);
			cJSON *p;
			wire_point_dev_dir(m_io, id, "receive", (double)rb);
			wire_point_dev_dir(m_io, id, "transmit", (double)tb);
			wire_point_dev_dir(m_pk, id, "receive", (double)rp);
			wire_point_dev_dir(m_pk, id, "transmit", (double)tp);
			wire_point_dev_dir(m_er, id, "receive", (double)re);
			wire_point_dev_dir(m_er, id, "transmit", (double)te);
			wire_point_dev_dir(m_dr, id, "receive", (double)rd);
			wire_point_dev_dir(m_dr, id, "transmit", (double)td);
			/* link.speed: /sys/class/net/<if>/speed(Mbps)->bit/s. virtio 부재/미지원 -> null(가짜 0 금지). */
			char sp[300], spv[32]; snprintf(sp, sizeof sp, "/sys/class/net/%s/speed", iface);
			p=wire_point(m_sp); wire_point_attr(p,"device",id);
			if (read_sysfs_str(sp, spv, sizeof spv)) {
				long mbps = strtol(spv, NULL, 10);
				if (mbps > 0) wire_point_value(p, (double)mbps * 1e6); else wire_point_null(p);
			} else wire_point_null(p);
		}
		free(content);
	}
	long rt = snmp_tcp_retranssegs();
	wire_metric_scalar(ns, "network.tcp.retransmits", "counter", "segments", rt >= 0, (double)rt);
	long ctc = -1, ctm = -1;
	{ char *c = read_file_all("/proc/sys/net/netfilter/nf_conntrack_count"); if (c){ ctc=strtol(c,NULL,10); free(c);} }
	{ char *c = read_file_all("/proc/sys/net/netfilter/nf_conntrack_max");   if (c){ ctm=strtol(c,NULL,10); free(c);} }
	wire_metric_scalar(ns, "network.conntrack.usage", "gauge", "entries", ctc >= 0, (double)ctc);
	wire_metric_scalar(ns, "network.conntrack.limit", "gauge", "entries", ctm >= 0, (double)ctm);
}

/* P3 system.pressure: PSI(4.20+) /proc/pressure/{cpu,memory,io}. stall.time(counter,s; total us->s,
 * 14일 saturation canonical) + stall.ratio(gauge,1; avg10/60/300). 미지원 -> namespace null. */
static void metrics_collect_pressure(cJSON *root)
{
	static const struct { const char *path; const char *res; } psi[] = {
		{ "/proc/pressure/cpu", "cpu" }, { "/proc/pressure/memory", "memory" }, { "/proc/pressure/io", "io" },
	};
	cJSON *ns = wire_ns(root, "system.pressure");
	cJSON *m_time  = wire_metric(ns, "pressure.stall.time",  "counter", "s");
	cJSON *m_ratio = wire_metric(ns, "pressure.stall.ratio", "gauge",   "1");
	int any = 0;
	for (size_t i = 0; i < sizeof psi / sizeof psi[0]; i++) {
		char *c = read_file_all(psi[i].path);
		if (!c) continue;
		any = 1;
		char *save = NULL;
		for (char *line = strtok_r(c, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			char scope[8] = {0}; double a10=0,a60=0,a300=0; long long total=0;
			if (sscanf(line, "%7s avg10=%lf avg60=%lf avg300=%lf total=%lld", scope,&a10,&a60,&a300,&total) < 5) continue;
			cJSON *p;
			p=wire_point(m_time); wire_point_attr(p,"resource",psi[i].res); wire_point_attr(p,"scope",scope); wire_point_value(p,(double)total/1e6);
			struct { const char *w; double v; } win[3] = {{"avg10",a10},{"avg60",a60},{"avg300",a300}};
			for (int k=0;k<3;k++){ p=wire_point(m_ratio); wire_point_attr(p,"resource",psi[i].res); wire_point_attr(p,"scope",scope); wire_point_attr(p,"window",win[k].w); wire_point_value(p, win[k].v/100.0); }
		}
		free(c);
	}
	if (!any) { cJSON_DeleteItemFromObject(root, "system.pressure"); cJSON_AddNullToObject(root, "system.pressure"); }
}

/* P4 system.filesystem: statvfs over 실 마운트. usage(gauge,By; used/free/reserved) + inodes(gauge,count). */
static void metrics_collect_filesystem(cJSON *root)
{
	cJSON *ns = wire_ns(root, "system.filesystem");
	cJSON *m_use = wire_metric(ns, "filesystem.usage",  "gauge", "By");
	cJSON *m_ino = wire_metric(ns, "filesystem.inodes", "gauge", "count");
	size_t n = 0;
	struct mount_entry *mounts = list_real_mounts(&n);
	for (size_t i = 0; i < n; i++) {
		struct statvfs st;
		if (statvfs(mounts[i].mount, &st) != 0) continue;
		double frsize = (double)st.f_frsize;
		double total = (double)st.f_blocks * frsize;
		double freeb = (double)st.f_bfree  * frsize;
		double availb= (double)st.f_bavail * frsize;
		cJSON *p;
		p=wire_point(m_use); wire_point_attr(p,"mountpoint",mounts[i].mount); wire_point_attr(p,"state","used");     wire_point_value(p, total - freeb);
		p=wire_point(m_use); wire_point_attr(p,"mountpoint",mounts[i].mount); wire_point_attr(p,"state","free");     wire_point_value(p, availb);
		p=wire_point(m_use); wire_point_attr(p,"mountpoint",mounts[i].mount); wire_point_attr(p,"state","reserved"); wire_point_value(p, freeb - availb);
		p=wire_point(m_ino); wire_point_attr(p,"mountpoint",mounts[i].mount); wire_point_attr(p,"state","used");
		if (st.f_files > 0) wire_point_value(p, (double)(st.f_files - st.f_ffree)); else wire_point_null(p);
		p=wire_point(m_ino); wire_point_attr(p,"mountpoint",mounts[i].mount); wire_point_attr(p,"state","free");
		if (st.f_files > 0) wire_point_value(p, (double)st.f_ffree); else wire_point_null(p);
	}
	free_mount_entries(mounts, n);
}

/* P5 E축(disk): mdraid mismatch_cnt + ext4 errors_count. RAID 배열/fs 레벨 참조라 device 스킴 md:/name:. */
static void metrics_collect_disk_errors(cJSON *root)
{
	cJSON *ns = wire_ns(root, "system.disk");
	cJSON *m_err = wire_metric(ns, "disk.errors", "counter", "errors");
	char val[64];
	DIR *d = opendir("/sys/block");
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) != NULL) {
			if (strncmp(e->d_name, "md", 2) != 0) continue;
			char pth[300]; snprintf(pth, sizeof pth, "/sys/block/%s/md/mismatch_cnt", e->d_name);
			if (read_sysfs_str(pth, val, sizeof val)) {
				cJSON *p = wire_point(m_err);
				char dev[80]; snprintf(dev, sizeof dev, "md:%s", e->d_name);
				wire_point_attr(p, "device", dev); wire_point_attr(p, "type", "mismatch");
				wire_point_value(p, (double)strtoll(val, NULL, 10));
			}
		}
		closedir(d);
	}
	d = opendir("/sys/fs/ext4");
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) != NULL) {
			if (e->d_name[0] == '.') continue;
			char pth[300]; snprintf(pth, sizeof pth, "/sys/fs/ext4/%s/errors_count", e->d_name);
			if (read_sysfs_str(pth, val, sizeof val)) {
				cJSON *p = wire_point(m_err);
				char dev[80]; snprintf(dev, sizeof dev, "name:%s", e->d_name);
				wire_point_attr(p, "device", dev); wire_point_attr(p, "type", "fs");
				wire_point_value(p, (double)strtoll(val, NULL, 10));
			}
		}
		closedir(d);
	}
}

/* P5 E축(memory): EDAC correctable/uncorrectable 누적. VM/가상칩셋은 EDAC 미노출 -> null(측정불가). */
static void metrics_collect_memory_errors(cJSON *root)
{
	cJSON *ns = wire_ns(root, "system.memory");
	cJSON *m = wire_metric(ns, "memory.edac", "counter", "errors");
	long long ce = -1, ue = -1;
	DIR *d = opendir("/sys/devices/system/edac/mc");
	if (d) {
		struct dirent *e; char val[64];
		while ((e = readdir(d)) != NULL) {
			if (strncmp(e->d_name, "mc", 2) != 0) continue;
			char pth[320];
			snprintf(pth, sizeof pth, "/sys/devices/system/edac/mc/%s/ce_count", e->d_name);
			if (read_sysfs_str(pth, val, sizeof val)) { if (ce < 0) ce = 0; ce += strtoll(val, NULL, 10); }
			snprintf(pth, sizeof pth, "/sys/devices/system/edac/mc/%s/ue_count", e->d_name);
			if (read_sysfs_str(pth, val, sizeof val)) { if (ue < 0) ue = 0; ue += strtoll(val, NULL, 10); }
		}
		closedir(d);
	}
	cJSON *p;
	p=wire_point(m); wire_point_attr(p,"type","correctable");   if (ce >= 0) wire_point_value(p,(double)ce); else wire_point_null(p);
	p=wire_point(m); wire_point_attr(p,"type","uncorrectable"); if (ue >= 0) wire_point_value(p,(double)ue); else wire_point_null(p);
}

/* P6 system.cgroup: v2(unified) 우선 + v1 폴백. cpu throttling + memory usage/limit.
 * fleet VM 은 대개 root cgroup(제한 없음) -> 있는 신호만 발행, 전무하면 namespace null. */
static void metrics_collect_cgroup(cJSON *root)
{
	cJSON *ns = wire_ns(root, "system.cgroup");
	int any = 0;
	char *cs = read_file_all("/sys/fs/cgroup/cpu.stat");           /* v2 */
	if (!cs) cs = read_file_all("/sys/fs/cgroup/cpu/cpu.stat");    /* v1 */
	if (cs) {
		long long thr = -1, thr_usec = -1;
		char *save = NULL;
		for (char *line = strtok_r(cs, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			if      (!strncmp(line, "nr_throttled ", 13))   thr      = strtoll(line + 13, NULL, 10);
			else if (!strncmp(line, "throttled_usec ", 15)) thr_usec = strtoll(line + 15, NULL, 10);
			else if (!strncmp(line, "throttled_time ", 15)) thr_usec = strtoll(line + 15, NULL, 10) / 1000; /* v1 ns->us */
		}
		free(cs);
		if (thr >= 0)      { wire_metric_scalar(ns, "cgroup.cpu.throttled.count", "counter", "events", 1, (double)thr); any = 1; }
		if (thr_usec >= 0) { wire_metric_scalar(ns, "cgroup.cpu.throttled.time",  "counter", "s", 1, (double)thr_usec / 1e6); any = 1; }
	}
	{ char *c = read_file_all("/sys/fs/cgroup/memory.current");
	  if (!c) c = read_file_all("/sys/fs/cgroup/memory/memory.usage_in_bytes");
	  if (c) { wire_metric_scalar(ns, "cgroup.memory.usage", "gauge", "By", 1, (double)strtoll(c, NULL, 10)); free(c); any = 1; } }
	{ char *c = read_file_all("/sys/fs/cgroup/memory.max");
	  int v1 = 0; if (!c) { c = read_file_all("/sys/fs/cgroup/memory/memory.limit_in_bytes"); v1 = 1; }
	  if (c) {
		long long lim = strtoll(c, NULL, 10);
		/* v2 "max"(무제한) / v1 초대형 sentinel 은 제한 없음 -> 생략. */
		if (strncmp(c, "max", 3) != 0 && !(v1 && lim >= 0x7000000000000000LL))
			{ wire_metric_scalar(ns, "cgroup.memory.limit", "gauge", "By", 1, (double)lim); any = 1; }
		free(c);
	  } }
	if (!any) { cJSON_DeleteItemFromObject(root, "system.cgroup"); cJSON_AddNullToObject(root, "system.cgroup"); }
}

cJSON *collect_metrics_payload(const char *machine_id, const char *agent_version)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;
	wire_add_envelope(root, "metrics", machine_id, agent_version);
	cJSON_AddNumberToObject(root, "collection_interval_sec", agent_interval_sec());

	metrics_collect_cpu(root);
	metrics_collect_memory(root);   /* + system.paging */
	metrics_collect_memory_errors(root);   /* memory.edac (E축) */
	metrics_collect_network(root);
	metrics_collect_disk(root);
	metrics_collect_disk_errors(root);      /* disk.errors (E축) */
	metrics_collect_pressure(root);
	metrics_collect_filesystem(root);
	metrics_collect_cgroup(root);

	return root;
}

cJSON *build_error_payload(const char *machine_id,
                           const char *agent_version,
                           const char *error_code,
                           const char *error_message,
                           const char *failed_component,
                           int         retry_count,
                           const char *first_failed_at,
                           const char *recovered_at)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;
	wire_add_envelope(root, "error", machine_id, agent_version);

	cJSON_AddStringToObject(root, "error_code",       error_code       ? error_code       : "UNKNOWN");
	cJSON_AddStringToObject(root, "error_message",    error_message    ? error_message    : "");
	cJSON_AddStringToObject(root, "failed_component", failed_component ? failed_component : "agent");

	if (retry_count >= 0)
		cJSON_AddNumberToObject(root, "retry_count", (double)retry_count);
	if (first_failed_at)
		cJSON_AddStringToObject(root, "first_failed_at", first_failed_at);
	if (recovered_at)
		cJSON_AddStringToObject(root, "recovered_at", recovered_at);

	return root;
}
