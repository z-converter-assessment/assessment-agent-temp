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

#include "collect_internal.h"

int is_excluded_block_dev(const char *name)
{
	if (!name || !*name) return 1;
	return strncmp(name, "loop", 4) == 0
	    || strncmp(name, "ram",  3) == 0
	    || strncmp(name, "sr",   2) == 0
	    || strncmp(name, "fd",   2) == 0;
}

int parse_major_minor(const char *s, int *major, int *minor)
{
	*major = -1;
	*minor = -1;
	if (!s) return 0;
	const char *colon = strchr(s, ':');
	if (!colon || colon == s) return 0;
	char *end = NULL;
	long mj = strtol(s, &end, 10);
	if (end != colon) return 0;
	long mn = strtol(colon + 1, &end, 10);
	if (end == colon + 1) return 0;
	*major = (int)mj;
	*minor = (int)mn;
	return 1;
}

long meminfo_get_kb(const char *content, const char *key)
{
	size_t key_len = strlen(key);
	const char *p = content;
	while (*p) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		if (len > key_len && strncmp(p, key, key_len) == 0 && p[key_len] == ':') {
			const char *v = p + key_len + 1;
			while (*v == ' ' || *v == '\t')
				v++;
			char *end;
			long val = strtol(v, &end, 10);
			if (end != v)
				return val;
			return -1;
		}
		if (!eol) break;
		p = eol + 1;
	}
	return -1;
}

const char *net_kind(const char *ifname)
{
	if (!ifname || !*ifname) return "virtual";
	if (strcmp(ifname, "lo") == 0) return "loopback";
	char p[320];
	snprintf(p, sizeof p, "/sys/class/net/%s/bridge", ifname);
	if (access(p, F_OK) == 0) return "bridge";
	snprintf(p, sizeof p, "/sys/class/net/%s/bonding", ifname);
	if (access(p, F_OK) == 0) return "bond_master";
	snprintf(p, sizeof p, "/sys/class/net/%s/bonding_slave", ifname);
	if (access(p, F_OK) == 0) return "bond_member";
	snprintf(p, sizeof p, "/sys/class/net/%s/tun_flags", ifname);
	if (access(p, F_OK) == 0) return "tunnel";
	/* uevent DEVTYPE is the kernel's authoritative virtual-device type; map all
	 * relevant types here. Tunnel encaps fold to "tunnel"; macvlan/ipvlan/macvtap
	 * fall through to "virtual" below. */
	snprintf(p, sizeof p, "/sys/class/net/%s/uevent", ifname);
	char *ue = read_file_all(p);
	if (ue) {
		const char *dt = strstr(ue, "DEVTYPE=");
		char devtype[32] = { 0 };
		if (dt) sscanf(dt + 8, "%31[^\n]", devtype);
		free(ue);
		if (strcmp(devtype, "vlan")   == 0) return "vlan";
		if (strcmp(devtype, "bridge") == 0) return "bridge";
		if (strcmp(devtype, "bond")   == 0) return "bond_master";
		if (strcmp(devtype, "veth")   == 0) return "veth";
		if (strcmp(devtype, "vxlan")  == 0 || strcmp(devtype, "geneve") == 0 ||
		    strcmp(devtype, "gre")    == 0 || strcmp(devtype, "gretap") == 0 ||
		    strcmp(devtype, "ip6gre") == 0 || strcmp(devtype, "sit")    == 0 ||
		    strcmp(devtype, "ipip")   == 0 || strcmp(devtype, "ip6tnl") == 0 ||
		    strcmp(devtype, "vti")    == 0 || strcmp(devtype, "vti6")   == 0)
			return "tunnel";
	}
	if (strncmp(ifname, "veth", 4) == 0)   return "veth";
	if (strncmp(ifname, "docker", 6) == 0) return "bridge";
	if (strncmp(ifname, "virbr", 5) == 0)  return "bridge";
	if (strncmp(ifname, "br-", 3) == 0)    return "bridge";
	snprintf(p, sizeof p, "/sys/class/net/%s/device", ifname);
	if (access(p, F_OK) == 0) return "physical";
	return "virtual";
}

int is_excluded_fstype(const char *fstype)
{
	static const char *skip_fs[] = {
		"proc", "sysfs", "cgroup", "cgroup2", "devpts", "tmpfs",
		"devtmpfs", "mqueue", "hugetlbfs", "fusectl", "debugfs",
		"tracefs", "securityfs", "pstore", "autofs", "rpc_pipefs",
		"binfmt_misc", "configfs", "bpf", "ramfs", "overlay", "squashfs",
		"nsfs",

		"9p", "virtiofs", "fuse.lxcfs", "fuse.gvfsd-fuse",
		NULL,
	};
	if (!fstype) return 1;
	for (int i = 0; skip_fs[i]; i++)
		if (strcmp(fstype, skip_fs[i]) == 0)
			return 1;
	return 0;
}

/* /proc/filesystems 의 leading "nodev" = backing block device 없는 fs (kernel's own
 * authority). Catches pseudo mounts the static skip list misses, on any kernel,
 * without a growing denylist. Result cached. */
int fstype_is_nodev(const char *fstype)
{
	static char nodev[2048] = "\n";
	static int loaded = 0;
	if (!loaded) {
		loaded = 1;
		char *content = read_file_all("/proc/filesystems");
		if (content) {
			size_t len = 1;
			char *save = NULL;
			for (char *line = strtok_r(content, "\n", &save); line;
			     line = strtok_r(NULL, "\n", &save)) {
				if (strncmp(line, "nodev", 5) != 0) continue;
				const char *name = line + 5;
				while (*name == ' ' || *name == '\t') name++;
				size_t nl = strlen(name);
				if (nl == 0 || len + nl + 1 >= sizeof nodev) continue;
				memcpy(nodev + len, name, nl);
				len += nl;
				nodev[len++] = '\n';
				nodev[len]   = '\0';
			}
			free(content);
		}
	}
	if (!fstype || !*fstype) return 0;
	char needle[80];
	int n = snprintf(needle, sizeof needle, "\n%s\n", fstype);
	if (n <= 0 || (size_t)n >= sizeof needle) return 0;
	return strstr(nodev, needle) != NULL;
}

/* nodev fs that still hold real data and must NOT be dropped as pseudo: network
 * filesystems and FUSE. fuseblk (real block device) is not nodev, never reaches here. */
int is_kept_deviceless_fs(const char *fstype)
{
	if (!fstype) return 0;
	if (strncmp(fstype, "fuse", 4) == 0) return 1;
	static const char *net_fs[] = {
		"nfs", "nfs4", "cifs", "smb3", "smbfs", "ceph", "glusterfs",
		"afs", "lustre", "orangefs", "ncpfs", "coda", "beegfs", NULL,
	};
	for (int i = 0; net_fs[i]; i++)
		if (strcmp(fstype, net_fs[i]) == 0)
			return 1;
	return 0;
}

void free_mount_entries(struct mount_entry *arr, size_t n)
{
	if (!arr) return;
	for (size_t i = 0; i < n; i++) {
		free(arr[i].mount);
		free(arr[i].fstype);
	}
	free(arr);
}

int parse_mountinfo_line(const char *line,
                                int *major, int *minor,
                                char **mount_out, char **fstype_out)
{
	char *copy = strdup(line);
	if (!copy) return 0;

	int mj = -1, mn = -1;
	char *mnt = NULL, *fst = NULL;
	int seen_dash = 0;
	int idx = 0;

	char *save = NULL;
	for (char *tok = strtok_r(copy, " ", &save); tok;
	     tok = strtok_r(NULL, " ", &save)) {
		idx++;
		if (idx == 3) {
			if (!parse_major_minor(tok, &mj, &mn)) goto fail;
		} else if (idx == 5) {
			mnt = strdup(tok);
			if (!mnt) goto fail;
		} else if (!seen_dash && idx >= 7
		           && tok[0] == '-' && tok[1] == '\0') {
			seen_dash = 1;
		} else if (seen_dash && !fst) {
			fst = strdup(tok);
			if (!fst) goto fail;
			break;
		}
	}
	free(copy);

	if (mj < 0 || !mnt || !fst) {
		free(mnt); free(fst);
		return 0;
	}

	*major = mj;
	*minor = mn;
	*mount_out = mnt;
	*fstype_out = fst;
	return 1;

fail:
	free(copy);
	free(mnt);
	free(fst);
	return 0;
}

void dedup_mounts(struct mount_entry *arr, size_t *count)
{
	size_t out = 0;
	for (size_t i = 0; i < *count; i++) {
		int seen = 0;
		for (size_t j = 0; j < out; j++) {
			if (arr[j].major == arr[i].major &&
			    arr[j].minor == arr[i].minor) {
				seen = 1;
				break;
			}
		}
		if (seen) {
			free(arr[i].mount);
			free(arr[i].fstype);
			continue;
		}
		if (out != i) arr[out] = arr[i];
		out++;
	}
	*count = out;
}

struct mount_entry *list_real_mounts(size_t *out_count)
{
	*out_count = 0;
	char *content = read_file_all("/proc/self/mountinfo");
	if (!content) return NULL;

	size_t cap = 16, count = 0;
	struct mount_entry *arr = malloc(sizeof *arr * cap);
	if (!arr) { free(content); return NULL; }

	char *save = NULL;
	for (char *line = strtok_r(content, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		int mj = -1, mn = -1;
		char *mnt = NULL, *fst = NULL;
		if (!parse_mountinfo_line(line, &mj, &mn, &mnt, &fst))
			continue;
		/* Drop pseudo/virtual mounts: static skip list + nodev fs (except
		 * network/FUSE). Without the nodev net, fs missing from the skip list
		 * (selinuxfs, usbfs, ...) leak through mislabeled kind="data". */
		if (is_excluded_fstype(fst) ||
		    (fstype_is_nodev(fst) && !is_kept_deviceless_fs(fst))) {
			free(mnt); free(fst);
			continue;
		}
		if (count + 1 > cap) {
			cap *= 2;
			struct mount_entry *nr = realloc(arr, sizeof *arr * cap);
			if (!nr) { free(mnt); free(fst); break; }
			arr = nr;
		}
		arr[count].major = mj;
		arr[count].minor = mn;
		arr[count].mount = mnt;
		arr[count].fstype = fst;
		count++;
	}
	free(content);

	dedup_mounts(arr, &count);
	*out_count = count;
	return arr;
}

int ipv4_netmask_prefix(uint32_t mask_n)
{
	uint32_t m = ntohl(mask_n);
	int n = 0;
	while (m & 0x80000000u) { n++; m <<= 1; }
	return n;
}

int ipv6_netmask_prefix(const struct sockaddr_in6 *mask)
{
	int prefix = 0;
	for (int i = 0; i < 16; i++) {
		unsigned char b = mask->sin6_addr.s6_addr[i];
		if (b == 0xff) { prefix += 8; continue; }
		while (b & 0x80) { prefix++; b <<= 1; }
		break;
	}
	return prefix;
}

/* /proc/net/route default route(dest=0,mask=0) gateway 를 iface->IP 맵으로. 서브넷 disambiguation 용, IPv4 만. */
cJSON *build_default_gw_v4(void)
{
	cJSON *map = cJSON_CreateObject();
	FILE *f = fopen("/proc/net/route", "r");
	if (!f)
		return map;

	char line[512];
	if (fgets(line, sizeof line, f)) {   /* skip header row */
		while (fgets(line, sizeof line, f)) {
			char iface[64];
			unsigned long dest, gw, flags, mask;
			int refcnt, use, metric, mtu, win, irtt;
			if (sscanf(line, "%63s %lx %lx %lx %d %d %d %lx %d %d %d",
			           iface, &dest, &gw, &flags, &refcnt, &use,
			           &metric, &mask, &mtu, &win, &irtt) < 8)
				continue;
			if (dest != 0 || mask != 0 || gw == 0)
				continue;   /* default route + 실 gateway만 */
			if (cJSON_GetObjectItem(map, iface))
				continue;   /* iface당 첫 default route */
			struct in_addr a;
			a.s_addr = (in_addr_t)gw;   /* hex는 network-order LE 정수 (amd64) */
			char buf[INET_ADDRSTRLEN];
			if (inet_ntop(AF_INET, &a, buf, sizeof buf))
				cJSON_AddStringToObject(map, iface, buf);
		}
	}
	fclose(f);
	return map;
}

/* /proc/net/snmp Tcp: RetransSegs — 컬럼 수가 커널마다 달라 헤더행 인덱스로 값행 매칭. 없으면 -1. */
long snmp_tcp_retranssegs(void)
{
	char *content = read_file_all("/proc/net/snmp");
	if (!content)
		return -1;
	long result = -1;
	char *hdr = NULL, *val = NULL, *save = NULL;
	for (char *line = strtok_r(content, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "Tcp: ", 5) == 0) {
			if (!hdr)
				hdr = line;
			else {
				val = line;
				break;
			}
		}
	}
	if (hdr && val) {
		int idx = -1, i = 0;
		char *hs = NULL;
		for (char *t = strtok_r(hdr, " ", &hs); t;
		     t = strtok_r(NULL, " ", &hs), i++) {
			if (strcmp(t, "RetransSegs") == 0) {
				idx = i;
				break;
			}
		}
		if (idx >= 0) {
			int j = 0;
			char *vs = NULL;
			for (char *t = strtok_r(val, " ", &vs); t;
			     t = strtok_r(NULL, " ", &vs), j++) {
				if (j == idx) {
					result = strtol(t, NULL, 10);
					break;
				}
			}
		}
	}
	free(content);
	return result;
}

/* main.c 가 실제 sleep 하는 수집 주기(초)를 그대로 보고. interval>0 은 raw 값 그대로
 * (상한 clamp 금지 — 접으면 엔진 표본 기준이 오염). interval<=0(one-shot)은 엔진의
 * expected=86400/interval div-by-zero 회피로 기본 60 으로 보고. */
int agent_interval_sec(void)
{
	int n = getenv_int_or("AGENT_INTERVAL_SEC", 60);
	return n > 0 ? n : 60;
}

/* sysfs 단일값 읽어 trim. 성공 1. */
int read_sysfs_str(const char *path, char *out, size_t outsz)
{
	char *c = read_file_all(path);
	if (!c) return 0;
	size_t n = strlen(c);
	while (n && (c[n-1] == '\n' || c[n-1] == '\r' || c[n-1] == ' ' || c[n-1] == '\t'))
		c[--n] = '\0';
	if (n == 0) { free(c); return 0; }
	snprintf(out, outsz, "%s", c);
	free(c);
	return 1;
}

/* device 안정키: dm/uuid -> serial -> by-path -> name. '<scheme>:<value>'. */
void disk_device_id(const char *dev, char *out, size_t outsz)
{
	char path[300], val[256];
	snprintf(path, sizeof path, "/sys/block/%s/dm/uuid", dev);
	if (read_sysfs_str(path, val, sizeof val)) { snprintf(out, outsz, "dm:%s", val); return; }
	snprintf(path, sizeof path, "/sys/block/%s/serial", dev);
	if (read_sysfs_str(path, val, sizeof val)) { snprintf(out, outsz, "serial:%s", val); return; }
	snprintf(path, sizeof path, "/sys/block/%s/device/serial", dev);
	if (read_sysfs_str(path, val, sizeof val)) { snprintf(out, outsz, "serial:%s", val); return; }
	DIR *d = opendir("/dev/disk/by-path");
	if (d) {
		struct dirent *e;
		char lp[600], tgt[300];
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.') continue;
			snprintf(lp, sizeof lp, "/dev/disk/by-path/%s", e->d_name);
			ssize_t r = readlink(lp, tgt, sizeof tgt - 1);
			if (r <= 0) continue;
			tgt[r] = '\0';
			const char *base = strrchr(tgt, '/');
			base = base ? base + 1 : tgt;
			if (strcmp(base, dev) == 0) {
				snprintf(out, outsz, "by-path:%s", e->d_name);
				closedir(d);
				return;
			}
		}
		closedir(d);
	}
	snprintf(out, outsz, "name:%s", dev);
}

/* net device 안정키: MAC(mac:xx..) -> by-path(PCI 경로) -> name. '<scheme>:<value>'. */
void net_device_id(const char *iface, char *out, size_t outsz)
{
	char path[300], mac[64];
	snprintf(path, sizeof path, "/sys/class/net/%s/address", iface);
	if (read_sysfs_str(path, mac, sizeof mac) && strlen(mac) >= 17 &&
	    strcmp(mac, "00:00:00:00:00:00") != 0) {
		snprintf(out, outsz, "mac:%s", mac);
		return;
	}
	snprintf(path, sizeof path, "/sys/class/net/%s/device", iface);
	char lp[300];
	ssize_t k = readlink(path, lp, sizeof lp - 1);
	if (k > 0) {
		lp[k] = '\0';
		char *base = strrchr(lp, '/');
		snprintf(out, outsz, "by-path:%s", base ? base + 1 : lp);
		return;
	}
	snprintf(out, outsz, "name:%s", iface);
}
