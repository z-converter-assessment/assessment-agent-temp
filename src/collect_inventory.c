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
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

struct sock_inode_owner {
	long inode;
	int  pid;
	char comm[64];
};

static cJSON *inv_collect_block_devices(void);
static cJSON *inv_collect_external_ip(void);
static cJSON *inv_collect_listen_ports(void);
static cJSON *inv_collect_net_interfaces(void);
static cJSON *inv_collect_lvm_vgs(void);
static cJSON *inv_collect_boot(void);
static cJSON *inv_collect_nonblock_mounts(void);
static cJSON *inv_collect_services(void);
static cJSON *inv_collect_services_sysv(void);
static const char *dev_id_type(const char *full);
static int add_cpu_cores(cJSON *root);
static int add_debian_version_fallback(cJSON *root);
static int add_kernel_version(cJSON *root);
static int add_lsb_release_fallback(cJSON *root);
static int add_os_release(cJSON *root);
static int add_redhat_release_fallback(cJSON *root);
static int add_suse_release_fallback(cJSON *root);
static int dev_mount_info(const char *name, char *fst, size_t fsz, char *mnt, size_t msz);
static int is_remote_unconnected(const char *remote);
static int read_os_release_field(const char *content, const char *key, char **out);
static int read_pid_comm(int pid, char *out, size_t out_len);
static int read_sysv_pidfile(const char *name);
static struct sock_inode_owner *build_socket_owner_map(size_t *out_count);
static void add_cpu_model(cJSON *root);
static cJSON *bd_add(cJSON *arr, const char *name, const char *type, long long size,
                   const char *fst, const char *mnt, const char *parent, const char *idfull);
static void parse_tcp_v4_hex_addr(const char *hex8, char *out, size_t out_len);
static void parse_tcp_v6_hex_addr(const char *hex32, char *out, size_t out_len);
static void scan_proto_sockets(const char *path, const char *proto,
                               int is_v6, int is_udp,
                               cJSON *arr,
                               const struct sock_inode_owner *map, size_t map_n);

static int read_os_release_field(const char *content, const char *key, char **out)
{
	*out = NULL;
	size_t key_len = strlen(key);
	const char *p = content;
	while (*p) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		if (len > key_len && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
			const char *v = p + key_len + 1;
			size_t vlen = len - key_len - 1;
			if (vlen >= 2 && v[0] == '"' && v[vlen - 1] == '"') {
				v++;
				vlen -= 2;
			}
			char *r = malloc(vlen + 1);
			if (!r)
				return 0;
			memcpy(r, v, vlen);
			r[vlen] = '\0';
			*out = r;
			return 1;
		}
		if (!eol)
			break;
		p = eol + 1;
	}
	return 0;
}

static int add_redhat_release_fallback(cJSON *root)
{
	static const char *files[] = {
		"/etc/redhat-release", "/etc/centos-release",
		"/etc/oracle-release", "/etc/system-release", NULL,
	};
	char *content = NULL;
	for (int i = 0; files[i]; i++) {
		content = read_file_all(files[i]);
		if (content && content[0])
			break;
		free(content);
		content = NULL;
	}
	if (!content)
		return 0;

	char *nl = strchr(content, '\n');
	if (nl)
		*nl = '\0';

	char low[256];
	size_t i = 0;
	for (; content[i] && i < sizeof low - 1; i++)
		low[i] = (char)tolower((unsigned char)content[i]);
	low[i] = '\0';

	const char *os_id = "rhel";
	if (strstr(low, "centos"))            os_id = "centos";
	else if (strstr(low, "rocky"))        os_id = "rocky";
	else if (strstr(low, "almalinux"))    os_id = "almalinux";
	else if (strstr(low, "oracle"))       os_id = "ol";
	else if (strstr(low, "amazon"))       os_id = "amzn";
	else if (strstr(low, "red hat") || strstr(low, "redhat")) os_id = "rhel";

	char ver[32] = {0};
	const char *rel = strstr(low, "release ");
	if (rel) {
		const char *p = rel + 8;
		while (*p == ' ' || *p == '\t')
			p++;
		size_t v = 0;
		while ((*p >= '0' && *p <= '9') || *p == '.') {
			if (v < sizeof ver - 1)
				ver[v++] = *p;
			p++;
		}
		ver[v] = '\0';
	}

	if (ver[0] == '\0') {
		free(content);
		return 0;
	}

	cJSON_AddStringToObject(root, "os_id", os_id);
	cJSON_AddStringToObject(root, "os_version", ver);
	cJSON_AddNullToObject(root, "os_codename");
	free(content);
	return 1;
}

/* 구세대 SUSE(/etc/SuSE-release) 보완 — SLES11 은 os-release 가 없어 이 파일로만 식별.
 * VERSION+PATCHLEVEL -> os_version="11.3"(PATCHLEVEL 없으면 VERSION 만). */
static int add_suse_release_fallback(cJSON *root)
{
	char *content = read_file_all("/etc/SuSE-release");
	if (!content || !content[0]) {
		free(content);
		return 0;
	}

	char first[256];
	size_t fi = 0;
	for (; content[fi] && content[fi] != '\n' && fi < sizeof first - 1; fi++)
		first[fi] = (char)tolower((unsigned char)content[fi]);
	first[fi] = '\0';

	const char *os_id = NULL;
	if      (strstr(first, "enterprise server"))  os_id = "sles";
	else if (strstr(first, "enterprise desktop")) os_id = "sled";
	else if (strstr(first, "opensuse"))           os_id = "opensuse";
	else if (strstr(first, "suse"))               os_id = "suse";

	char version[16] = {0}, patch[16] = {0};
	for (char *p = content; p && *p; ) {
		char *eol = strchr(p, '\n');
		if (eol) *eol = '\0';
		char *dst = NULL; size_t dsz = 0;
		if      (strncmp(p, "VERSION", 7) == 0)     { dst = version; dsz = sizeof version; }
		else if (strncmp(p, "PATCHLEVEL", 10) == 0) { dst = patch;   dsz = sizeof patch; }
		if (dst) {
			const char *q = strchr(p, '=');
			if (q) {
				q++;
				while (*q == ' ' || *q == '\t') q++;
				size_t v = 0;
				while (*q >= '0' && *q <= '9' && v < dsz - 1)
					dst[v++] = *q++;
				dst[v] = '\0';
			}
		}
		if (!eol) break;
		*eol = '\n';
		p = eol + 1;
	}

	if (!os_id || version[0] == '\0') {
		free(content);
		return 0;
	}

	char os_version[32];
	if (patch[0])
		snprintf(os_version, sizeof os_version, "%s.%s", version, patch);
	else
		snprintf(os_version, sizeof os_version, "%s", version);

	cJSON_AddStringToObject(root, "os_id", os_id);
	cJSON_AddStringToObject(root, "os_version", os_version);
	cJSON_AddNullToObject(root, "os_codename");
	free(content);
	return 1;
}

/* /etc/lsb-release 보완(os-release 없는 lsb-only 구형 배포). DISTRIB_* -> os_id(소문자)/os_version/os_codename. */
static int add_lsb_release_fallback(cJSON *root)
{
	char *content = read_file_all("/etc/lsb-release");
	if (!content || !content[0]) {
		free(content);
		return 0;
	}
	char *id = NULL, *ver = NULL, *code = NULL;
	read_os_release_field(content, "DISTRIB_ID", &id);
	read_os_release_field(content, "DISTRIB_RELEASE", &ver);
	read_os_release_field(content, "DISTRIB_CODENAME", &code);
	free(content);
	/* 빈 문자열(DISTRIB_ID= 처럼 값 없는 키)은 측정 불가 -> null 로 강등(""로 발행 금지). */
	if ((!id || !*id) && (!ver || !*ver)) {
		free(id); free(ver); free(code);
		return 0;
	}
	if (id && *id)
		for (char *p = id; *p; p++)
			*p = (char)tolower((unsigned char)*p);
	(id   && *id)   ? cJSON_AddStringToObject(root, "os_id", id)         : cJSON_AddNullToObject(root, "os_id");
	(ver  && *ver)  ? cJSON_AddStringToObject(root, "os_version", ver)   : cJSON_AddNullToObject(root, "os_version");
	(code && *code) ? cJSON_AddStringToObject(root, "os_codename", code) : cJSON_AddNullToObject(root, "os_codename");
	free(id); free(ver); free(code);
	return 1;
}

/* /etc/debian_version 보완(lsb-release 도 없는 최구형 Debian). 파일 내용이 버전 문자열이다. */
static int add_debian_version_fallback(cJSON *root)
{
	char *content = read_file_all("/etc/debian_version");
	if (!content || !content[0]) {
		free(content);
		return 0;
	}
	char *nl = strchr(content, '\n');
	if (nl) *nl = '\0';
	char *v = content;
	while (*v == ' ' || *v == '\t') v++;
	if (!*v) {
		free(content);
		return 0;
	}
	cJSON_AddStringToObject(root, "os_id", "debian");
	cJSON_AddStringToObject(root, "os_version", v);
	cJSON_AddNullToObject(root, "os_codename");
	free(content);
	return 1;
}

static int add_os_release(cJSON *root)
{
	char *content = read_file_all("/etc/os-release");
	if (!content || !content[0]) {
		free(content);   /* 존재하나 빈 os-release 도 폴백을 타게 한다 */

		if (add_redhat_release_fallback(root))
			return 1;
		if (add_suse_release_fallback(root))
			return 1;
		if (add_lsb_release_fallback(root))
			return 1;
		if (add_debian_version_fallback(root))
			return 1;
		cJSON_AddNullToObject(root, "os_id");
		cJSON_AddNullToObject(root, "os_version");
		cJSON_AddNullToObject(root, "os_codename");
		return 0;
	}

	char *id = NULL, *ver = NULL, *code = NULL;
	read_os_release_field(content, "ID", &id);
	read_os_release_field(content, "VERSION_ID", &ver);
	read_os_release_field(content, "VERSION_CODENAME", &code);

	id   ? cJSON_AddStringToObject(root, "os_id", id)         : cJSON_AddNullToObject(root, "os_id");
	ver  ? cJSON_AddStringToObject(root, "os_version", ver)   : cJSON_AddNullToObject(root, "os_version");
	code ? cJSON_AddStringToObject(root, "os_codename", code) : cJSON_AddNullToObject(root, "os_codename");

	free(id); free(ver); free(code); free(content);
	return 1;
}

static int add_kernel_version(cJSON *root)
{
	struct utsname u;
	if (uname(&u) != 0)
		return 0;
	cJSON_AddStringToObject(root, "kernel_version", u.release);
	return 1;
}

void collect_add_os_result_fields(cJSON *root)
{
	cJSON_AddStringToObject(root, "os_family", "linux");
	add_os_release(root);
}

static int add_cpu_cores(cJSON *root)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	if (n <= 0)
		return 0;
	cJSON_AddNumberToObject(root, "cpu_cores", (double)n);
	return 1;
}

static void add_cpu_model(cJSON *root)
{
	char *content = read_file_all("/proc/cpuinfo");
	if (!content) {
		cJSON_AddNullToObject(root, "cpu_model");
		return;
	}
	const char *p = content;
	while (*p) {
		if (strncmp(p, "model name", 10) == 0) {
			const char *colon = strchr(p, ':');
			if (colon) {
				const char *v = colon + 1;
				while (*v == ' ' || *v == '\t')
					v++;
				const char *eol = strchr(v, '\n');
				size_t vlen = eol ? (size_t)(eol - v) : strlen(v);
				char *r = malloc(vlen + 1);
				if (r) {
					memcpy(r, v, vlen);
					r[vlen] = '\0';
					trim_inplace(r);
					cJSON_AddStringToObject(root, "cpu_model", r);
					free(r);
					free(content);
					return;
				}
			}
		}
		const char *eol = strchr(p, '\n');
		if (!eol) break;
		p = eol + 1;
	}
	cJSON_AddNullToObject(root, "cpu_model");
	free(content);
}

static cJSON *inv_collect_external_ip(void)
{
	const char *override = getenv("AGENT_EXTERNAL_IP");
	if (override && *override) {
		cJSON *arr = cJSON_CreateArray();
		char *copy = strdup(override);
		if (!copy)
			return arr;
		char *save = NULL;
		for (char *tok = strtok_r(copy, ",", &save); tok;
		     tok = strtok_r(NULL, ",", &save)) {
			trim_inplace(tok);
			if (*tok)
				cJSON_AddItemToArray(arr, cJSON_CreateString(tok));
		}
		free(copy);
		return arr;
	}

	char *out = fetch_cloud_metadata(
		"public-ipv4",
		"network/interface/0/ipv4/ipAddress/0/publicIpAddress",
		"instance/network-interfaces/0/access-configs/0/external-ip");
	if (!out)
		return cJSON_CreateNull();
	if (!*out) {
		free(out);
		return cJSON_CreateArray();
	}
	cJSON *arr = cJSON_CreateArray();
	cJSON_AddItemToArray(arr, cJSON_CreateString(out));
	free(out);
	return arr;
}

/* SysV pid 파일에서 pid 읽기(/var/run/<name>.pid 또는 <name>/<name>.pid). 죽은 pid(/proc 부재)·pid 파일 없는 서비스는 -1. */
static int read_sysv_pidfile(const char *name)
{
	char path[256];
	snprintf(path, sizeof path, "/var/run/%s.pid", name);
	char *c = read_file_all(path);
	if (!c) {
		snprintf(path, sizeof path, "/var/run/%s/%s.pid", name, name);
		c = read_file_all(path);
	}
	if (!c)
		return -1;
	int pid = atoi(c);
	free(c);
	if (pid <= 0)
		return -1;
	char pp[64];
	snprintf(pp, sizeof pp, "/proc/%d", pid);
	struct stat st;
	if (stat(pp, &st) != 0)
		return -1;
	return pid;
}

/* systemd 없는 SysV 폴백(EL6 등). /var/lock/subsys/<name> 로 실행 서비스 열거,
 * pid 파일/comm 으로 pid/exe. systemd 와 같은 {unit,sub,pid,exe} 스키마(없으면 null). */
static cJSON *inv_collect_services_sysv(void)
{
	DIR *d = opendir("/var/lock/subsys");
	if (!d)
		return cJSON_CreateNull();

	cJSON *arr = cJSON_CreateArray();
	if (!arr) { closedir(d); return NULL; }

	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "unit", de->d_name);
		cJSON_AddStringToObject(item, "sub",  "running");
		int pid = read_sysv_pidfile(de->d_name);
		if (pid > 0) {
			cJSON_AddNumberToObject(item, "pid", (double)pid);
			char comm[64];
			if (read_pid_comm(pid, comm, sizeof comm))
				cJSON_AddStringToObject(item, "exe", comm);
			else
				cJSON_AddNullToObject(item, "exe");
		} else {
			cJSON_AddNullToObject(item, "pid");
			cJSON_AddNullToObject(item, "exe");
		}
		cJSON_AddItemToArray(arr, item);
	}
	closedir(d);
	return arr;
}

/* services[]에 pid/exe 를 붙여 listen_ports[].pid 와 조인. per-unit show 는 fork 비용 커서
 * 실행 unit 전체를 한 번의 `systemctl show -p Id,MainPID`로 배치 조회. */
static cJSON *inv_collect_services(void)
{
	char *out = run_cmd(
		"systemctl list-units --type=service --type=socket "
		"--state=running --state=listening "
		"--no-pager --plain --no-legend 2>/dev/null");
	if (!out)
		return inv_collect_services_sysv();   /* systemd 부재(EL6 등) -> SysV 폴백 */

	cJSON *arr = cJSON_CreateArray();
	if (!arr) { free(out); return NULL; }

	char units_buf[8192];
	size_t units_len = 0;
	units_buf[0] = '\0';

	char *save = NULL;
	for (char *line = strtok_r(out, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *tok_save = NULL;
		char *unit   = strtok_r(line, " \t", &tok_save);
		if (!unit || !*unit) continue;
		char *load   = strtok_r(NULL, " \t", &tok_save);
		char *active = strtok_r(NULL, " \t", &tok_save);
		char *sub    = strtok_r(NULL, " \t", &tok_save);
		(void)load; (void)active;
		if (!sub) continue;

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "unit", unit);
		cJSON_AddStringToObject(item, "sub",  sub);
		cJSON_AddItemToArray(arr, item);

		/* .service unit 만 배치 조회한다. socket 은 MainPID 가 없어 show 가 exit!=0 ->
		 * run_cmd 가 stdout 을 폐기 -> 같은 배치의 service pid 까지 유실된다. socket 은
		 * pid=null 이 정답이라 제외. unit명은 공백/쉘 메타문자 없어 bare 로 이어붙인다. */
		size_t ul = strlen(unit);
		int is_service = (ul > 8 && strcmp(unit + ul - 8, ".service") == 0);
		if (is_service && units_len + ul + 2 < sizeof units_buf) {
			if (units_len) units_buf[units_len++] = ' ';
			memcpy(units_buf + units_len, unit, ul);
			units_len += ul;
			units_buf[units_len] = '\0';
		}
	}
	free(out);

	if (units_buf[0]) {
		char cmd[8320];
		snprintf(cmd, sizeof cmd,
		         "systemctl show --property=Id,MainPID %s 2>/dev/null", units_buf);
		char *show = run_cmd(cmd);
		if (show) {
			/* systemctl show 는 property 출력 순서를 안 지킨다 — 순서 의존 말고
			 * Id/MainPID 쌍이 다 차면 매칭(한 unit 블록당 각 1개). */
			char cur_id[256] = "";
			int  cur_pid = -1, have_pid = 0;
			char *s2 = NULL;
			for (char *l = strtok_r(show, "\n", &s2); l; l = strtok_r(NULL, "\n", &s2)) {
				if (strncmp(l, "Id=", 3) == 0)
					snprintf(cur_id, sizeof cur_id, "%s", l + 3);
				else if (strncmp(l, "MainPID=", 8) == 0) {
					cur_pid = atoi(l + 8);
					have_pid = 1;
				}
				if (!cur_id[0] || !have_pid)
					continue;
				cJSON *it;
				cJSON_ArrayForEach(it, arr) {
					cJSON *u = cJSON_GetObjectItemCaseSensitive(it, "unit");
					if (!cJSON_IsString(u) || strcmp(u->valuestring, cur_id) != 0)
						continue;
					if (cJSON_GetObjectItemCaseSensitive(it, "pid"))
						break;
					if (cur_pid > 0) {
						cJSON_AddNumberToObject(it, "pid", (double)cur_pid);
						char comm[64];
						if (read_pid_comm(cur_pid, comm, sizeof comm))
							cJSON_AddStringToObject(it, "exe", comm);
						else
							cJSON_AddNullToObject(it, "exe");
					} else {
						cJSON_AddNullToObject(it, "pid");
						cJSON_AddNullToObject(it, "exe");
					}
					break;
				}
				cur_id[0] = '\0';
				have_pid = 0;
			}
			free(show);
		}
	}

	/* show에서 못 잡힌 항목은 pid/exe를 null로 채워 스키마 일관 유지. */
	cJSON *it;
	cJSON_ArrayForEach(it, arr) {
		if (!cJSON_GetObjectItemCaseSensitive(it, "pid")) {
			cJSON_AddNullToObject(it, "pid");
			cJSON_AddNullToObject(it, "exe");
		}
	}
	return arr;
}

static int read_pid_comm(int pid, char *out, size_t out_len)
{
	if (!out || out_len == 0) return 0;
	out[0] = '\0';
	char path[64];
	snprintf(path, sizeof path, "/proc/%d/comm", pid);
	char *content = read_file_all(path);
	if (!content) return 0;
	trim_inplace(content);
	strncpy(out, content, out_len - 1);
	out[out_len - 1] = '\0';
	free(content);
	return 1;
}

static struct sock_inode_owner *build_socket_owner_map(size_t *out_count)
{
	*out_count = 0;
	DIR *proc = opendir("/proc");
	if (!proc) return NULL;

	size_t cap = 256, count = 0;
	struct sock_inode_owner *map = malloc(sizeof *map * cap);
	if (!map) { closedir(proc); return NULL; }

	struct dirent *e;
	while ((e = readdir(proc)) != NULL) {
		char *end = NULL;
		long pid = strtol(e->d_name, &end, 10);
		if (!end || *end != '\0' || pid <= 0) continue;

		char fd_dir[64];
		snprintf(fd_dir, sizeof fd_dir, "/proc/%ld/fd", pid);
		DIR *fdd = opendir(fd_dir);
		if (!fdd) continue;

		char comm[64] = { 0 };
		read_pid_comm((int)pid, comm, sizeof comm);

		struct dirent *fe;
		while ((fe = readdir(fdd)) != NULL) {
			if (fe->d_name[0] == '.') continue;
			char fd_path[160], target[256];
			snprintf(fd_path, sizeof fd_path, "%s/%s", fd_dir, fe->d_name);
			ssize_t n = readlink(fd_path, target, sizeof target - 1);
			if (n <= 0) continue;
			target[n] = '\0';
			if (strncmp(target, "socket:[", 8) != 0) continue;
			long inode = strtol(target + 8, NULL, 10);
			if (inode <= 0) continue;

			if (count + 1 > cap) {
				cap *= 2;
				struct sock_inode_owner *nr = realloc(map, sizeof *map * cap);
				if (!nr) { closedir(fdd); goto done; }
				map = nr;
			}
			map[count].inode = inode;
			map[count].pid = (int)pid;
			strncpy(map[count].comm, comm, sizeof map[count].comm - 1);
			map[count].comm[sizeof map[count].comm - 1] = '\0';
			count++;
		}
		closedir(fdd);
	}
done:
	closedir(proc);
	*out_count = count;
	return map;
}

static void parse_tcp_v4_hex_addr(const char *hex8, char *out, size_t out_len)
{
	unsigned int a = (unsigned int)strtoul(hex8, NULL, 16);
	snprintf(out, out_len, "%u.%u.%u.%u",
	         a & 0xff, (a >> 8) & 0xff,
	         (a >> 16) & 0xff, (a >> 24) & 0xff);
}

static void parse_tcp_v6_hex_addr(const char *hex32, char *out, size_t out_len)
{
	struct in6_addr addr;
	memset(&addr, 0, sizeof addr);
	for (int i = 0; i < 4; i++) {
		char buf[9];
		memcpy(buf, hex32 + i * 8, 8);
		buf[8] = '\0';
		unsigned int dw = (unsigned int)strtoul(buf, NULL, 16);
		addr.s6_addr[i * 4 + 0] = (uint8_t)( dw        & 0xff);
		addr.s6_addr[i * 4 + 1] = (uint8_t)((dw >> 8)  & 0xff);
		addr.s6_addr[i * 4 + 2] = (uint8_t)((dw >> 16) & 0xff);
		addr.s6_addr[i * 4 + 3] = (uint8_t)((dw >> 24) & 0xff);
	}
	inet_ntop(AF_INET6, &addr, out, (socklen_t)out_len);
}

static int is_remote_unconnected(const char *remote)
{
	if (!remote || !*remote) return 0;
	for (const char *p = remote; *p; p++) {
		if (*p == ':') continue;
		if (*p != '0') return 0;
	}
	return 1;
}

static void scan_proto_sockets(const char *path, const char *proto,
                               int is_v6, int is_udp,
                               cJSON *arr,
                               const struct sock_inode_owner *map, size_t map_n)
{
	char *content = read_file_all(path);
	if (!content) return;

	int line_no = 0;
	char *save = NULL;
	for (char *line = strtok_r(content, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		if (line_no++ == 0) continue;

		while (*line == ' ' || *line == '\t') line++;
		char *colon = strchr(line, ':');
		if (!colon) continue;
		char *rest = colon + 1;
		while (*rest == ' ' || *rest == '\t') rest++;

		char local[80], remote[80], state[8];
		char txrxq[40], trtm[40], retr[16];
		long uid = 0, timeout = 0, inode = 0;
		int n = sscanf(rest,
		               "%79s %79s %7s %39s %39s %15s %ld %ld %ld",
		               local, remote, state, txrxq, trtm, retr,
		               &uid, &timeout, &inode);
		if (n < 9) continue;

		if (is_udp) {
			if (!is_remote_unconnected(remote)) continue;
		} else {
			if (strcmp(state, TCP_STATE_LISTEN_HEX) != 0) continue;
		}

		char *port_sep = strrchr(local, ':');
		if (!port_sep) continue;
		*port_sep = '\0';
		unsigned int port = (unsigned int)strtoul(port_sep + 1, NULL, 16);
		if (port == 0) continue;

		char addr_buf[INET6_ADDRSTRLEN] = { 0 };
		if (is_v6) {
			if (strlen(local) != 32) continue;
			parse_tcp_v6_hex_addr(local, addr_buf, sizeof addr_buf);
		} else {
			if (strlen(local) != 8) continue;
			parse_tcp_v4_hex_addr(local, addr_buf, sizeof addr_buf);
		}

		int pid = -1;
		const char *comm = NULL;
		for (size_t i = 0; i < map_n; i++) {
			if (map[i].inode == inode) {
				pid = map[i].pid;
				comm = map[i].comm;
				break;
			}
		}

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "proto", proto);
		cJSON_AddStringToObject(item, "addr", addr_buf);
		cJSON_AddNumberToObject(item, "port", (double)port);
		cJSON_AddNumberToObject(item, "uid",  (double)uid);
		if (pid > 0) cJSON_AddNumberToObject(item, "pid", (double)pid);
		else         cJSON_AddNullToObject(item, "pid");
		if (comm && *comm) cJSON_AddStringToObject(item, "comm", comm);
		else               cJSON_AddNullToObject(item, "comm");
		cJSON_AddItemToArray(arr, item);
	}
	free(content);
}

static cJSON *inv_collect_listen_ports(void)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr) return NULL;

	size_t map_n = 0;
	struct sock_inode_owner *map = build_socket_owner_map(&map_n);

	scan_proto_sockets("/proc/net/tcp",  "tcp",  0, 0, arr, map, map_n);
	scan_proto_sockets("/proc/net/tcp6", "tcp6", 1, 0, arr, map, map_n);
	scan_proto_sockets("/proc/net/udp",  "udp",  0, 1, arr, map, map_n);
	scan_proto_sockets("/proc/net/udp6", "udp6", 1, 1, arr, map, map_n);

	free(map);
	return arr;
}

/* 재현 os 확장. arch/bits/boot_firmware/secure_boot/edition(Linux null)/timezone/rtc_utc.
   미측정=null(위조 금지). edition 은 Windows 전용이라 Linux 는 항상 null. */

/* secure_boot: efivars SecureBoot-<GUID> 파일 5번째 바이트(offset 4)=1 활성/0 비활성. 부재=null */
static void os_add_secure_boot(cJSON *root)
{
	DIR *d = opendir("/sys/firmware/efi/efivars");
	if (!d) {
		cJSON_AddNullToObject(root, "secure_boot");
		return;
	}
	struct dirent *e;
	int done = 0;
	while ((e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, "SecureBoot-", 11) != 0)
			continue;
		char path[512];
		snprintf(path, sizeof path, "/sys/firmware/efi/efivars/%s", e->d_name);
		FILE *f = fopen(path, "rb");
		if (!f)
			break;
		unsigned char buf[8];
		size_t n = fread(buf, 1, sizeof buf, f);
		fclose(f);
		if (n >= 5) {
			cJSON_AddBoolToObject(root, "secure_boot", buf[4] ? 1 : 0);
			done = 1;
		}
		break;
	}
	closedir(d);
	if (!done)
		cJSON_AddNullToObject(root, "secure_boot");
}

/* timezone: /etc/localtime 심링크 -> ".../zoneinfo/<IANA>" 에서 IANA 원문. 폴백 /etc/timezone. 부재=null */
static void os_add_timezone(cJSON *root)
{
	char link[512];
	ssize_t n = readlink("/etc/localtime", link, sizeof link - 1);
	if (n > 0) {
		link[n] = '\0';
		const char *z = strstr(link, "zoneinfo/");
		if (z && z[9]) {
			cJSON_AddStringToObject(root, "timezone", z + 9);
			return;
		}
	}
	char *tz = read_file_all("/etc/timezone");
	if (tz) {
		char *p = tz;
		while (*p && *p != '\n' && *p != '\r')
			p++;
		*p = '\0';
		if (tz[0]) {
			cJSON_AddStringToObject(root, "timezone", tz);
			free(tz);
			return;
		}
		free(tz);
	}
	cJSON_AddNullToObject(root, "timezone");
}

/* rtc_utc: /etc/adjtime 3번째 줄 "UTC"=true / "LOCAL"=false. 부재=null(위조 금지) */
static void os_add_rtc_utc(cJSON *root)
{
	char *a = read_file_all("/etc/adjtime");
	if (!a) {
		cJSON_AddNullToObject(root, "rtc_utc");
		return;
	}
	/* 3번째 줄로 이동 */
	char *l1 = strchr(a, '\n');
	char *l2 = l1 ? strchr(l1 + 1, '\n') : NULL;
	char *l3 = l2 ? l2 + 1 : NULL;
	if (l3 && strncmp(l3, "UTC", 3) == 0)
		cJSON_AddBoolToObject(root, "rtc_utc", 1);
	else if (l3 && strncmp(l3, "LOCAL", 5) == 0)
		cJSON_AddBoolToObject(root, "rtc_utc", 0);
	else
		cJSON_AddNullToObject(root, "rtc_utc");
	free(a);
}

static void inv_collect_os_repro(cJSON *root)
{
	struct utsname u;
	int have = (uname(&u) == 0);
	if (have && u.machine[0]) {
		cJSON_AddStringToObject(root, "arch", u.machine);
		/* bits: u.machine 문자열 매핑(sizeof(void*) 아님). "64" 포함 또는 s390x=64, 그 외 32 */
		int is64 = (strstr(u.machine, "64") != NULL) || (strcmp(u.machine, "s390x") == 0);
		cJSON_AddNumberToObject(root, "bits", is64 ? 64 : 32);
	} else {
		cJSON_AddNullToObject(root, "arch");
		cJSON_AddNullToObject(root, "bits");
	}
	/* boot_firmware: /sys/firmware/efi 존재=uefi, /sys/firmware 만=bios, /sys 미접근=null */
	if (access("/sys/firmware/efi", F_OK) == 0)
		cJSON_AddStringToObject(root, "boot_firmware", "uefi");
	else if (access("/sys/firmware", F_OK) == 0)
		cJSON_AddStringToObject(root, "boot_firmware", "bios");
	else
		cJSON_AddNullToObject(root, "boot_firmware");
	os_add_secure_boot(root);
	cJSON_AddNullToObject(root, "edition"); /* Linux 없음 */
	os_add_timezone(root);
	os_add_rtc_utc(root);
}

cJSON *collect_inventory_payload(const char *machine_id, const char *agent_version)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;
	wire_add_envelope(root, "inventory", machine_id, agent_version);

	int ok = 1;
	add_os_release(root);
	if (!add_kernel_version(root))     ok = 0;
	inv_collect_os_repro(root);
	if (!add_cpu_cores(root))          ok = 0;
	add_cpu_model(root);

	/* mem_total_bytes (base 단위). swap 은 block_devices type=swap 노드로. */
	{
		char *mi = read_file_all("/proc/meminfo");
		long kb = mi ? meminfo_get_kb(mi, "MemTotal") : -1;
		free(mi);
		if (kb >= 0) cJSON_AddNumberToObject(root, "mem_total_bytes", (double)kb * 1024.0);
		else         cJSON_AddNullToObject(root, "mem_total_bytes");
	}

	cJSON_AddItemToObject(root, "services",     wire_or_null(inv_collect_services()));
	cJSON_AddItemToObject(root, "listen_ports", wire_or_empty_array(inv_collect_listen_ports()));
	cJSON_AddItemToObject(root, "ip_external",  inv_collect_external_ip());

	cJSON_AddItemToObject(root, "block_devices",  wire_or_empty_array(inv_collect_block_devices()));
	cJSON_AddItemToObject(root, "net_interfaces", wire_or_empty_array(inv_collect_net_interfaces()));
	{
		/* lvm_vgs: /etc/lvm/backup 있을 때만 발행(LVM 미설치 호스트는 키 생략) */
		cJSON *lvgs = inv_collect_lvm_vgs();
		if (lvgs) cJSON_AddItemToObject(root, "lvm_vgs", lvgs);
	}
	cJSON_AddItemToObject(root, "boot", inv_collect_boot());
	{
		cJSON *nbm = inv_collect_nonblock_mounts();
		if (nbm) cJSON_AddItemToObject(root, "nonblock_mounts", nbm);
	}

	if (!ok) {
		cJSON_Delete(root);
		return NULL;
	}
	return root;
}

/* id "scheme:value" -> id_type(scheme). block_device.id_type enum. */
static const char *dev_id_type(const char *full)
{
	if (!strncmp(full, "dm:", 3))       return "dm";
	if (!strncmp(full, "partuuid:", 9)) return "partuuid";
	if (!strncmp(full, "wwid:", 5))     return "wwid";
	if (!strncmp(full, "serial:", 7))   return "serial";
	if (!strncmp(full, "by-path:", 8))  return "by-path";
	if (!strncmp(full, "fsuuid:", 7))   return "fsuuid";
	return "name";
}

/* /proc/mounts 에서 device basename 의 fstype/mountpoint. /dev/mapper 심볼릭은 realpath 로 dm-N 해석. */
static int dev_mount_info(const char *name, char *fst, size_t fsz, char *mnt, size_t msz)
{
	FILE *f = fopen("/proc/mounts", "r");
	if (!f) return 0;
	char line[1200]; int found = 0;
	while (fgets(line, sizeof line, f)) {
		char src[300], m[300], t[80];
		if (sscanf(line, "%299s %299s %79s", src, m, t) != 3) continue;
		if (strncmp(src, "/dev/", 5) != 0) continue;
		char rp[4100]; const char *base;
		if (realpath(src, rp)) { base = strrchr(rp, '/'); base = base ? base + 1 : rp; }
		else                   { base = strrchr(src, '/'); base = base ? base + 1 : src; }
		if (strcmp(base, name) == 0) { snprintf(fst, fsz, "%s", t); snprintf(mnt, msz, "%s", m); mount_unescape(mnt); found = 1; break; }
	}
	fclose(f);
	return found;
}

/* ---- 파티션 테이블 파서: GPT(LBA1)/MBR(LBA0) 직독 ----
   O_NONBLOCK open(미디어 대기 hang 회피) + bounded pread. 실패 시 그 디스크만 kind=none.
   part_type/name/flags 는 이 테이블과 sysfs 시작오프셋 매칭으로 채운다. */

#define PT_MAX 128

struct pt_ent {
	unsigned long long start_lba;   /* 테이블 LBA 단위(GPT=논리섹터, MBR=512 가정) */
	char type[40];                  /* "0x83" 또는 소문자 무중괄호 GUID */
	char name[128];                 /* GPT 레이블(UTF-8), MBR="" */
	unsigned long long attr;        /* GPT 속성 비트 */
	unsigned char mbr_status;       /* MBR status(0x80=active) */
	unsigned char mbr_type;         /* MBR type 바이트 */
	int is_mbr;
};

struct pt_table {
	int kind;   /* 0 none, 1 gpt, 2 mbr */
	int lbs;    /* 논리 섹터 크기(GPT LBA -> byte 환산) */
	int n;
	struct pt_ent e[PT_MAX];
};

static unsigned pt_le32(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static unsigned long long pt_le64(const unsigned char *p)
{
	return (unsigned long long)pt_le32(p) | ((unsigned long long)pt_le32(p + 4) << 32);
}

static int disk_logical_block_size(const char *dev)
{
	char p[320], v[64];
	snprintf(p, sizeof p, "/sys/block/%s/queue/logical_block_size", dev);
	if (read_sysfs_str(p, v, sizeof v)) {
		int b = atoi(v);
		if (b > 0) return b;
	}
	return 512;
}

/* GPT type GUID(소문자 무중괄호) -> parted 계열 시맨틱 플래그명. 없으면 NULL */
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

/* MBR type 바이트 -> 시맨틱 플래그명. 없으면 NULL */
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

/* 16바이트 GPT GUID -> 소문자 무중괄호 문자열(mixed-endian: 앞 3필드 LE, 뒤 2필드 BE). */
static void gpt_guid_str(const unsigned char *g, char *out, size_t outsz)
{
	snprintf(out, outsz,
	    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	    g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
	    g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* UTF-16LE(최대 in_bytes, NUL 종단) -> UTF-8. BMP만(서로게이트는 건너뜀). */
static void utf16le_to_utf8(const unsigned char *in, size_t in_bytes, char *out, size_t outsz)
{
	size_t oi = 0;
	for (size_t i = 0; i + 1 < in_bytes && oi + 1 < outsz; i += 2) {
		unsigned cp = (unsigned)in[i] | ((unsigned)in[i + 1] << 8);
		if (cp == 0) break;
		if (cp >= 0xD800 && cp <= 0xDFFF) continue; /* 서로게이트 반쪽 무시 */
		if (cp < 0x80) {
			out[oi++] = (char)cp;
		} else if (cp < 0x800) {
			if (oi + 2 >= outsz) break;
			out[oi++] = (char)(0xC0 | (cp >> 6));
			out[oi++] = (char)(0x80 | (cp & 0x3F));
		} else {
			if (oi + 3 >= outsz) break;
			out[oi++] = (char)(0xE0 | (cp >> 12));
			out[oi++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[oi++] = (char)(0x80 | (cp & 0x3F));
		}
	}
	out[oi] = '\0';
}

static void parse_partition_table(const char *dev, struct pt_table *t)
{
	t->kind = 0;
	t->n = 0;
	t->lbs = disk_logical_block_size(dev);
	char path[300];
	snprintf(path, sizeof path, "/dev/%s", dev);
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return;

	unsigned char lba01[1024];
	ssize_t r = pread(fd, lba01, sizeof lba01, 0);
	if (r < 512) { close(fd); return; }

	int gpt = 0;
	if (r >= 1024 && memcmp(lba01 + 512, "EFI PART", 8) == 0)
		gpt = 1;
	else if (lba01[510] == 0x55 && lba01[511] == 0xAA && lba01[446 + 4] == 0xEE)
		gpt = 1;

	unsigned char hdr[512];
	if (gpt) {
		/* GPT 헤더는 LBA1 = 오프셋 lbs. 512e 는 lba01 안(512), 4Kn 은 오프셋 4096 별도 pread. */
		const unsigned char *h;
		if (t->lbs == 512 && r >= 1024) {
			h = lba01 + 512;
		} else {
			ssize_t hr = pread(fd, hdr, sizeof hdr, (off_t)t->lbs);
			h = (hr >= 92) ? hdr : NULL;
		}
		unsigned long long ent_lba = h ? pt_le64(h + 72) : 0;
		unsigned num = h ? pt_le32(h + 80) : 0;
		unsigned esz = h ? pt_le32(h + 84) : 0;
		if (h && memcmp(h, "EFI PART", 8) == 0 && esz >= 128 && esz <= 512 && num > 0 && num <= 4096) {
			if (num > PT_MAX) num = PT_MAX;
			size_t region = (size_t)num * esz;
			if (region > 128 * 512) region = 128 * 512; /* bounded */
			unsigned char *buf = malloc(region);
			if (buf) {
				off_t off = (off_t)ent_lba * t->lbs;
				ssize_t er = pread(fd, buf, region, off);
				if (er > 0) {
					t->kind = 1;
					unsigned cnt = (unsigned)((size_t)er / esz);
					for (unsigned i = 0; i < cnt && t->n < PT_MAX; i++) {
						const unsigned char *e = buf + (size_t)i * esz;
						int zero = 1;
						for (int b = 0; b < 16; b++)
							if (e[b]) { zero = 0; break; }
						if (zero) continue; /* 빈 엔트리 */
						struct pt_ent *pe = &t->e[t->n++];
						pe->is_mbr = 0;
						pe->mbr_status = 0;
						pe->mbr_type = 0;
						pe->start_lba = pt_le64(e + 32);
						pe->attr = pt_le64(e + 48);
						gpt_guid_str(e, pe->type, sizeof pe->type);
						utf16le_to_utf8(e + 56, 72, pe->name, sizeof pe->name);
					}
				}
				free(buf);
			}
		}
	} else if (lba01[510] == 0x55 && lba01[511] == 0xAA) {
		t->kind = 2; /* MBR 프라이머리 4엔트리(논리 파티션은 미매칭 -> null) */
		for (int i = 0; i < 4; i++) {
			const unsigned char *e = lba01 + 446 + i * 16;
			unsigned char type = e[4];
			if (type == 0x00) continue;
			struct pt_ent *pe = &t->e[t->n++];
			pe->is_mbr = 1;
			pe->attr = 0;
			pe->mbr_status = e[0];
			pe->mbr_type = type;
			pe->start_lba = pt_le32(e + 8);
			snprintf(pe->type, sizeof pe->type, "0x%02x", type);
			pe->name[0] = '\0';
		}
	}
	close(fd);
}

/* sysfs part 시작바이트로 테이블 엔트리 매칭 -> part_type/name/flags 부착. 미매칭=null 3종. */
static void part_attach_meta(cJSON *node, const struct pt_table *t, long long start_bytes)
{
	const struct pt_ent *m = NULL;
	if (t->kind && start_bytes >= 0) {
		for (int i = 0; i < t->n; i++) {
			/* GPT/MBR 공히 start_lba 는 논리섹터(lbs) 단위. 커널 msdos 파서도 4Kn 에서 lbs 배율. */
			long long ebytes = (long long)t->e[i].start_lba * t->lbs;
			if (ebytes == start_bytes) { m = &t->e[i]; break; }
		}
	}
	if (!m) {
		cJSON_AddNullToObject(node, "part_type");
		cJSON_AddNullToObject(node, "part_name");
		cJSON_AddNullToObject(node, "part_flags");
		return;
	}
	cJSON_AddStringToObject(node, "part_type", m->type);
	if (!m->is_mbr && m->name[0]) cJSON_AddStringToObject(node, "part_name", m->name);
	else                         cJSON_AddNullToObject(node, "part_name");
	cJSON *fl = cJSON_CreateArray();
	if (m->is_mbr) {
		if (m->mbr_status == 0x80) cJSON_AddItemToArray(fl, cJSON_CreateString("boot"));
		const char *tf = mbr_type_flag(m->mbr_type);
		if (tf) cJSON_AddItemToArray(fl, cJSON_CreateString(tf));
	} else {
		const char *tf = gpt_type_flag(m->type);
		if (tf) cJSON_AddItemToArray(fl, cJSON_CreateString(tf));
		if (m->attr & (1ULL << 0))  cJSON_AddItemToArray(fl, cJSON_CreateString("required"));
		if (m->attr & (1ULL << 2))  cJSON_AddItemToArray(fl, cJSON_CreateString("legacy_boot"));
		if (m->attr & (1ULL << 62)) cJSON_AddItemToArray(fl, cJSON_CreateString("hidden"));
		if (m->attr & (1ULL << 63)) cJSON_AddItemToArray(fl, cJSON_CreateString("no_automount"));
	}
	cJSON_AddItemToObject(node, "part_flags", fl);
}

/* ---- fs 메타: fs_uuid/fs_label/block_size/mount_options/fs_freq/fs_passno ----
   발견 시에만 발행(미발견=키 생략, 엔진 OUTPUT 에서 null). swap 노드 제외. */

static int hexnib(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* udev by-label/by-uuid 이름의 \xNN 이스케이프 디코드(공백 등). */
static void blk_decode_escapes(const char *in, char *out, size_t outsz)
{
	size_t o = 0;
	for (size_t i = 0; in[i] && o + 1 < outsz; ) {
		if (in[i] == '\\' && in[i + 1] == 'x' && hexnib(in[i + 2]) >= 0 && hexnib(in[i + 3]) >= 0) {
			out[o++] = (char)((hexnib(in[i + 2]) << 4) | hexnib(in[i + 3]));
			i += 4;
		} else {
			out[o++] = in[i++];
		}
	}
	out[o] = '\0';
}

/* /dev/disk/by-* 에서 dev(블록 basename)를 가리키는 심링크 이름 역매핑. */
static int blk_reverse_map(const char *dir, const char *dev, char *out, size_t outsz)
{
	DIR *d = opendir(dir);
	if (!d) return 0;
	struct dirent *e;
	int found = 0;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char lp[512], tgt[512];
		snprintf(lp, sizeof lp, "%s/%s", dir, e->d_name);
		ssize_t n = readlink(lp, tgt, sizeof tgt - 1);
		if (n <= 0) continue;
		tgt[n] = '\0';
		const char *base = strrchr(tgt, '/');
		base = base ? base + 1 : tgt;
		if (strcmp(base, dev) == 0) {
			blk_decode_escapes(e->d_name, out, outsz);
			found = 1;
			break;
		}
	}
	closedir(d);
	return found;
}

/* /etc/fstab 매칭(UUID=/LABEL=/dev/ 또는 마운트포인트) -> mount_options/fs_freq/fs_passno. */
static int fstab_lookup(const char *dev, const char *uuid, const char *label,
                        const char *partuuid, const char *mountpoint, cJSON **out_opts, int *freq, int *passno)
{
	FILE *f = fopen("/etc/fstab", "r");
	if (!f) return 0;
	char line[1024];
	int found = 0;
	while (fgets(line, sizeof line, f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\n' || *p == '\0') continue;
		char spec[400], mp[400], type[80], opts[400];
		int fr = 0, pa = 0;
		int nf = sscanf(p, "%399s %399s %79s %399s %d %d", spec, mp, type, opts, &fr, &pa);
		if (nf < 4) continue;
		int match = 0;
		if (!strncmp(spec, "UUID=", 5) && uuid && !strcmp(spec + 5, uuid)) match = 1;
		else if (!strncmp(spec, "LABEL=", 6) && label && !strcmp(spec + 6, label)) match = 1;
		else if (!strncmp(spec, "PARTUUID=", 9) && partuuid && !strcasecmp(spec + 9, partuuid)) match = 1;
		else if (!strncmp(spec, "/dev/", 5)) {
			char rp[4100]; const char *base;
			if (realpath(spec, rp)) { base = strrchr(rp, '/'); base = base ? base + 1 : rp; }
			else                    { base = strrchr(spec, '/'); base = base ? base + 1 : spec; }
			if (!strcmp(base, dev)) match = 1;
		}
		else if (mountpoint && mountpoint[0] && !strcmp(mp, mountpoint)) match = 1;
		if (!match) continue;
		cJSON *arr = cJSON_CreateArray();
		char *save = NULL;
		for (char *tok = strtok_r(opts, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
			cJSON_AddItemToArray(arr, cJSON_CreateString(tok));
		*out_opts = arr;
		*freq = (nf >= 5) ? fr : 0;
		*passno = (nf >= 6) ? pa : 0;
		found = 1;
		break;
	}
	fclose(f);
	return found;
}

static void attach_fs_meta(cJSON *node, const char *dev, const char *mountpoint, const char *idfull)
{
	char uuid[160] = {0}, label[160] = {0};
	int have_uuid = blk_reverse_map("/dev/disk/by-uuid", dev, uuid, sizeof uuid);
	int have_label = blk_reverse_map("/dev/disk/by-label", dev, label, sizeof label);
	if (have_uuid)  cJSON_AddStringToObject(node, "fs_uuid", uuid);
	if (have_label) cJSON_AddStringToObject(node, "fs_label", label);
	if (mountpoint && mountpoint[0]) {
		struct statvfs vfs;
		if (statvfs(mountpoint, &vfs) == 0 && vfs.f_bsize > 0)
			cJSON_AddNumberToObject(node, "block_size", (double)vfs.f_bsize);
	}
	/* part 노드의 partuuid(id_type=partuuid)면 fstab PARTUUID= 매칭에 쓴다. */
	const char *partuuid = (strcmp(dev_id_type(idfull), "partuuid") == 0) ? dev_id_value(idfull) : NULL;
	cJSON *opts = NULL; int fr = 0, pa = 0;
	if (fstab_lookup(dev, have_uuid ? uuid : NULL, have_label ? label : NULL, partuuid, mountpoint, &opts, &fr, &pa)) {
		cJSON_AddItemToObject(node, "mount_options", opts);
		cJSON_AddNumberToObject(node, "fs_freq", fr);
		cJSON_AddNumberToObject(node, "fs_passno", pa);
	}
}

/* /dev/disk/by-id/wwn-0x... 역매핑 -> "0x..."(schema wwn 포맷). */
static int disk_wwn(const char *dev, char *out, size_t outsz)
{
	DIR *d = opendir("/dev/disk/by-id");
	if (!d) return 0;
	struct dirent *e;
	int found = 0;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "wwn-", 4) != 0) continue;
		char lp[512], tgt[512];
		snprintf(lp, sizeof lp, "/dev/disk/by-id/%s", e->d_name);
		ssize_t n = readlink(lp, tgt, sizeof tgt - 1);
		if (n <= 0) continue;
		tgt[n] = '\0';
		const char *base = strrchr(tgt, '/');
		base = base ? base + 1 : tgt;
		if (strcmp(base, dev) == 0) {
			snprintf(out, outsz, "%s", e->d_name + 4); /* "wwn-0x5000..." -> "0x5000..." */
			found = 1;
			break;
		}
	}
	closedir(d);
	return found;
}

/* disk 상세(disk 노드): sector_size/serial/wwn/rotational. 미측정=null. */
static void attach_disk_meta(cJSON *node, const char *dev)
{
	char p[320], v[160];
	cJSON_AddNumberToObject(node, "sector_size", disk_logical_block_size(dev));
	snprintf(p, sizeof p, "/sys/block/%s/queue/rotational", dev);
	if (read_sysfs_str(p, v, sizeof v)) cJSON_AddBoolToObject(node, "rotational", atoi(v) ? 1 : 0);
	else                                cJSON_AddNullToObject(node, "rotational");
	char ser[160] = {0};
	snprintf(p, sizeof p, "/sys/block/%s/serial", dev);
	if (!read_sysfs_str(p, ser, sizeof ser)) {
		snprintf(p, sizeof p, "/sys/block/%s/device/serial", dev);
		read_sysfs_str(p, ser, sizeof ser);
	}
	if (ser[0]) cJSON_AddStringToObject(node, "serial", ser);
	else        cJSON_AddNullToObject(node, "serial");
	char wwn[160];
	if (disk_wwn(dev, wwn, sizeof wwn)) cJSON_AddStringToObject(node, "wwn", wwn);
	else                                cJSON_AddNullToObject(node, "wwn");
}

/* ---- LVM/RAID/crypt(Linux 전용 노드타입) ---- */

/* crypt: dmuuid CRYPT-LUKS1/2 -> luks1/luks2, 그 외(PLAIN/VERITY 등)=null */
static void attach_crypt_meta(cJSON *node, const char *dmuuid)
{
	if (!strncmp(dmuuid, "CRYPT-LUKS1", 11))      cJSON_AddStringToObject(node, "crypt_type", "luks1");
	else if (!strncmp(dmuuid, "CRYPT-LUKS2", 11)) cJSON_AddStringToObject(node, "crypt_type", "luks2");
	else                                          cJSON_AddNullToObject(node, "crypt_type");
}

/* raid: /sys/block/<dev>/md/{level,chunk_size,metadata_version,uuid} */
static void attach_raid_meta(cJSON *node, const char *dev)
{
	char p[320], v[128];
	snprintf(p, sizeof p, "/sys/block/%s/md/level", dev);
	if (read_sysfs_str(p, v, sizeof v) && !strncmp(v, "raid", 4) && v[4] >= '0' && v[4] <= '9')
		cJSON_AddNumberToObject(node, "raid_level", atoi(v + 4));
	else
		cJSON_AddNullToObject(node, "raid_level"); /* linear/multipath 등 비수치=null */
	snprintf(p, sizeof p, "/sys/block/%s/md/chunk_size", dev);
	if (read_sysfs_str(p, v, sizeof v) && strtol(v, NULL, 10) > 0)
		cJSON_AddNumberToObject(node, "raid_chunk_kib", (double)(strtol(v, NULL, 10) / 1024));
	else
		cJSON_AddNullToObject(node, "raid_chunk_kib");
	snprintf(p, sizeof p, "/sys/block/%s/md/metadata_version", dev);
	if (read_sysfs_str(p, v, sizeof v) && v[0]) cJSON_AddStringToObject(node, "raid_metadata", v);
	else                                        cJSON_AddNullToObject(node, "raid_metadata");
	snprintf(p, sizeof p, "/sys/block/%s/md/uuid", dev);
	if (read_sysfs_str(p, v, sizeof v) && v[0]) cJSON_AddStringToObject(node, "raid_uuid", v);
	else                                        cJSON_AddNullToObject(node, "raid_uuid");
}

/* LVM backup 텍스트에서 key = "value" / key = NUM 첫 매치 추출. */
static int lvm_kv_str(const char *buf, const char *key, char *out, size_t outsz)
{
	char pat[64];
	snprintf(pat, sizeof pat, "%s = \"", key);
	const char *p = strstr(buf, pat);
	if (!p) return 0;
	p += strlen(pat);
	const char *e = strchr(p, '"');
	if (!e) return 0;
	size_t n = (size_t)(e - p);
	if (n >= outsz) n = outsz - 1;
	memcpy(out, p, n);
	out[n] = '\0';
	return 1;
}
static int lvm_kv_num(const char *buf, const char *key, long long *out)
{
	char pat[64];
	snprintf(pat, sizeof pat, "%s = ", key);
	const char *p = strstr(buf, pat);
	if (!p) return 0;
	*out = strtoll(p + strlen(pat), NULL, 10);
	return 1;
}

/* buf 안 "key = N" 전 occurrence 합산(pe_count/extent_count 등 VG 전체 집계). 토큰 경계(앞 공백/탭/개행) 확인. */
static long long lvm_kv_sum(const char *buf, const char *key)
{
	char pat[64];
	snprintf(pat, sizeof pat, "%s = ", key);
	size_t plen = strlen(pat);
	long long sum = 0;
	const char *p = buf;
	while ((p = strstr(p, pat)) != NULL) {
		int boundary = (p == buf) || p[-1] == '\t' || p[-1] == '\n' || p[-1] == ' ';
		const char *v = p + plen;
		p = v;
		if (boundary) sum += strtoll(v, NULL, 10);
	}
	return sum;
}

/* alloc_pe: 물리 PE 를 소비하는 세그먼트(type=striped/linear)의 extent_count 만 합산한다.
   thin/raid/cache/mirror/snapshot 의 상위 세그먼트 extent_count 는 가상 크기라 PE 를 소비하지 않고,
   실제 소비는 hidden sub-LV(_tdata/_tmeta/_rimage/_rmeta/_cdata/_cmeta)의 striped 세그먼트에만 있다.
   extent_count 를 무조건 합산하면 이중계산으로 alloc_pe 가 부풀어 free 가 음수->0 으로 위조된다.
   각 세그먼트는 extent_count 1개 + type 1개라, 다음 extent_count 전까지의 type 으로 짝짓는다. */
static long long lvm_alloc_pe(const char *buf)
{
	long long sum = 0;
	const char *ecpat = "extent_count = ";
	size_t eclen = strlen(ecpat);
	const char *p = buf;
	while ((p = strstr(p, ecpat)) != NULL) {
		int boundary = (p == buf) || p[-1] == '\t' || p[-1] == '\n' || p[-1] == ' ';
		const char *v = p + eclen;
		long long ec = strtoll(v, NULL, 10);
		p = v;
		if (!boundary) continue;
		const char *nextec = strstr(v, ecpat);
		const char *typ = strstr(v, "type = \"");
		if (typ && (!nextec || typ < nextec)) {
			const char *t = typ + 8; /* strlen("type = \"") */
			if (strncmp(t, "striped\"", 8) == 0 || strncmp(t, "linear\"", 7) == 0)
				sum += ec;
		}
	}
	return sum;
}

/* dm 이름 "vg-lv"(리터럴 '-'는 '--') -> vg/lv 분리. */
static void lvm_split_dmname(const char *s, char *vg, size_t vgsz, char *lv, size_t lvsz)
{
	size_t n = strlen(s), i = 0, o = 0;
	vg[0] = lv[0] = '\0';
	while (i < n && o + 1 < vgsz) {
		if (s[i] == '-' && i + 1 < n && s[i + 1] == '-') { vg[o++] = '-'; i += 2; continue; }
		if (s[i] == '-') { i++; break; } /* 구분자 */
		vg[o++] = s[i++];
	}
	vg[o] = '\0';
	o = 0;
	while (i < n && o + 1 < lvsz) {
		if (s[i] == '-' && i + 1 < n && s[i + 1] == '-') { lv[o++] = '-'; i += 2; continue; }
		lv[o++] = s[i++];
	}
	lv[o] = '\0';
}

/* lvm 노드: dm/name -> vg/lv, /etc/lvm/backup/<vg> -> segtype/stripes/stripe_size. */
static void attach_lvm_meta(cJSON *node, const char *dev)
{
	char p[320], dmname[256] = {0};
	snprintf(p, sizeof p, "/sys/block/%s/dm/name", dev);
	char vg[160] = {0}, lv[160] = {0};
	if (read_sysfs_str(p, dmname, sizeof dmname) && dmname[0])
		lvm_split_dmname(dmname, vg, sizeof vg, lv, sizeof lv);
	if (vg[0]) cJSON_AddStringToObject(node, "lvm_vg", vg); else cJSON_AddNullToObject(node, "lvm_vg");
	if (lv[0]) cJSON_AddStringToObject(node, "lvm_lv", lv); else cJSON_AddNullToObject(node, "lvm_lv");
	char segtype[40] = {0};
	int stripes = -1, stripe_kib = -1;
	if (vg[0] && lv[0]) {
		char path[400];
		snprintf(path, sizeof path, "/etc/lvm/backup/%s", vg);
		char *buf = read_file_all(path);
		if (buf) {
			char needle[200];
			snprintf(needle, sizeof needle, "\n\t\t%s {", lv);
			const char *b = strstr(buf, needle);
			if (b) {
				const char *end = strstr(b + 1, "\n\t\t}");
				size_t blen = end ? (size_t)(end - b) : strlen(b);
				char *block = malloc(blen + 1);
				if (block) {
					memcpy(block, b, blen);
					block[blen] = '\0';
					lvm_kv_str(block, "type", segtype, sizeof segtype);
					long long sc = 0, ss = 0;
					if (lvm_kv_num(block, "stripe_count", &sc)) stripes = (int)sc;
					if (lvm_kv_num(block, "stripe_size", &ss))  stripe_kib = (int)(ss / 2); /* 512섹터 -> KiB */
					/* LVM2 는 linear 세그먼트를 type=striped + stripe_count=1 로 저장 -> lvs 규약대로 linear 로 정규화 */
					if (strcmp(segtype, "striped") == 0 && stripes <= 1)
						snprintf(segtype, sizeof segtype, "linear");
					free(block);
				}
			}
			free(buf);
		}
	}
	if (segtype[0])    cJSON_AddStringToObject(node, "lvm_segtype", segtype); else cJSON_AddNullToObject(node, "lvm_segtype");
	if (stripes >= 0)  cJSON_AddNumberToObject(node, "lvm_stripes", stripes); else cJSON_AddNullToObject(node, "lvm_stripes");
	if (stripe_kib >= 0) cJSON_AddNumberToObject(node, "lvm_stripe_size_kib", stripe_kib); else cJSON_AddNullToObject(node, "lvm_stripe_size_kib");
}

static void attach_dm_type_meta(cJSON *node, const char *dev, const char *type, const char *dmuuid)
{
	if      (!strcmp(type, "crypt")) attach_crypt_meta(node, dmuuid);
	else if (!strcmp(type, "raid"))  attach_raid_meta(node, dev);
	else if (!strcmp(type, "lvm"))   attach_lvm_meta(node, dev);
	/* dm/mpath: 추가 필드 없음 */
}

/* 최상위 lvm_vgs: /etc/lvm/backup/<vg> 각각 -> name/vg_uuid/extent_size_bytes. 나머지 1차 null. */
static cJSON *inv_collect_lvm_vgs(void)
{
	DIR *d = opendir("/etc/lvm/backup");
	if (!d) return NULL;
	cJSON *arr = cJSON_CreateArray();
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char path[400];
		snprintf(path, sizeof path, "/etc/lvm/backup/%s", e->d_name);
		char *buf = read_file_all(path);
		if (!buf) continue;
		char vgid[96] = {0};
		long long ext = 0;
		lvm_kv_str(buf, "id", vgid, sizeof vgid);
		int have_ext = lvm_kv_num(buf, "extent_size", &ext);
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "name", e->d_name);
		/* size/free: 같은 backup 파일 재파싱. total_pe=sum(pe_count), alloc_pe=물리 세그먼트 extent_count 합,
		   size=total_pe*extent_size(섹터)*512, free=(total_pe-alloc_pe)*extent_size*512. 추가 I/O·외부명령 없음. */
		long long total_pe = lvm_kv_sum(buf, "pe_count");
		long long alloc_pe = lvm_alloc_pe(buf);
		if (have_ext && total_pe > 0) {
			long long freepe = total_pe - alloc_pe;
			if (freepe < 0) freepe = 0;
			cJSON_AddNumberToObject(o, "size_bytes", (double)(total_pe * ext * 512));
			cJSON_AddNumberToObject(o, "free_bytes", (double)(freepe * ext * 512));
		} else {
			cJSON_AddNullToObject(o, "size_bytes");
			cJSON_AddNullToObject(o, "free_bytes");
		}
		/* data_percent/metadata_percent: thin pool 런타임 할당 상태라 정적 백업에 없음 -> null(외부명령 금지). */
		cJSON_AddNullToObject(o, "data_percent");
		cJSON_AddNullToObject(o, "metadata_percent");
		if (vgid[0])  cJSON_AddStringToObject(o, "vg_uuid", vgid); else cJSON_AddNullToObject(o, "vg_uuid");
		if (have_ext) cJSON_AddNumberToObject(o, "extent_size_bytes", (double)(ext * 512)); else cJSON_AddNullToObject(o, "extent_size_bytes");
		/* pv_ids: physical_volumes 의 각 device="/dev/X" 를 basename -> resolve_block_id 로 block_device
		   안정 id 값(join-ready)으로 발행 -> block_devices[].id 와 조인된다. 없으면 null. */
		{
			cJSON *pv_ids = cJSON_CreateArray();
			const char *pat = "device = \"";
			size_t patlen = strlen(pat);
			const char *dp = buf;
			while ((dp = strstr(dp, pat)) != NULL) {
				dp += patlen;
				const char *end = strchr(dp, '"');
				if (!end) break;
				char devpath[300];
				size_t n = (size_t)(end - dp);
				if (n >= sizeof devpath) n = sizeof devpath - 1;
				memcpy(devpath, dp, n);
				devpath[n] = '\0';
				const char *base = strrchr(devpath, '/');
				base = base ? base + 1 : devpath;
				char sid[320];
				resolve_block_id(base, sid, sizeof sid);
				cJSON_AddItemToArray(pv_ids, cJSON_CreateString(dev_id_value(sid)));
				dp = end + 1;
			}
			if (cJSON_GetArraySize(pv_ids) > 0) {
				cJSON_AddItemToObject(o, "pv_ids", pv_ids);
			} else {
				cJSON_Delete(pv_ids);
				cJSON_AddNullToObject(o, "pv_ids");
			}
		}
		cJSON_AddItemToArray(arr, o);
		free(buf);
	}
	closedir(d);
	return arr;
}

/* ---- boot + nonblock_mounts ---- */

/* boot: /proc/cmdline + root= 참조 방식. grub_install_target 1차 null(ESP 기반 판별 후속). */
static cJSON *inv_collect_boot(void)
{
	cJSON *o = cJSON_CreateObject();
	char *cmd = read_file_all("/proc/cmdline");
	if (cmd) {
		char *nl = strchr(cmd, '\n');
		if (nl) *nl = '\0';
		cJSON_AddStringToObject(o, "kernel_cmdline", cmd);
		const char *r = strstr(cmd, "root=");
		const char *rt = NULL;
		if (r) {
			r += 5;
			if      (!strncmp(r, "UUID=", 5))     rt = "uuid";
			else if (!strncmp(r, "LABEL=", 6))    rt = "label";
			else if (!strncmp(r, "PARTUUID=", 9)) rt = "partuuid";
			else if (!strncmp(r, "/dev/", 5))     rt = "path";
		}
		if (rt) cJSON_AddStringToObject(o, "root_ref_type", rt);
		else    cJSON_AddNullToObject(o, "root_ref_type");
		free(cmd);
	} else {
		cJSON_AddNullToObject(o, "kernel_cmdline");
		cJSON_AddNullToObject(o, "root_ref_type");
	}
	cJSON_AddNullToObject(o, "grub_install_target");
	return o;
}

/* 재현 관련 비-블록 fs 화이트리스트(pseudo-fs proc/sys/cgroup 등 제외). */
static int is_nonblock_fs(const char *fs)
{
	static const char *wl[] = { "tmpfs", "ramfs", "nfs", "nfs4", "cifs", "smb3", "smbfs", "9p", "ceph", "glusterfs", NULL };
	for (int i = 0; wl[i]; i++)
		if (!strcmp(fs, wl[i])) return 1;
	if (!strncmp(fs, "fuse.", 5)) return 1; /* fuse.sshfs 등(제어용 fusectl 은 제외) */
	return 0;
}

/* nonblock_mounts: /proc/self/mountinfo 에서 블록장치 없는 마운트(tmpfs/nfs/cifs/9p) + bind(root!='/'). */
static cJSON *inv_collect_nonblock_mounts(void)
{
	FILE *f = fopen("/proc/self/mountinfo", "r");
	if (!f) return NULL;
	cJSON *arr = cJSON_CreateArray();
	char line[2048];
	while (fgets(line, sizeof line, f)) {
		char *dash = strstr(line, " - ");
		if (!dash) continue;
		int mid, pid, maj, min;
		char root[512], mp[512], mopts[512];
		if (sscanf(line, "%d %d %d:%d %511s %511s %511s", &mid, &pid, &maj, &min, root, mp, mopts) < 7) continue;
		char fstype[64], source[512], sopts[512];
		if (sscanf(dash + 3, "%63s %511s %511s", fstype, source, sopts) < 2) continue;
		int is_bind = (maj != 0 && strcmp(root, "/") != 0);
		int is_nb = (maj == 0 && is_nonblock_fs(fstype));
		if (!is_nb && !is_bind) continue;
		const char *src = is_bind ? root : source;
		cJSON *opts = NULL;
		int fr = 0, pa = 0;
		if (!fstab_lookup("", NULL, NULL, NULL, mp, &opts, &fr, &pa)) {
			/* fstab 부재(런타임-only) -> mountinfo 옵션 + 0/0 */
			opts = cJSON_CreateArray();
			char *save = NULL;
			for (char *tok = strtok_r(mopts, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
				cJSON_AddItemToArray(opts, cJSON_CreateString(tok));
			fr = 0; pa = 0;
		}
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "source", src);
		cJSON_AddStringToObject(o, "target", mp);
		cJSON_AddStringToObject(o, "fstype", fstype);
		cJSON_AddItemToObject(o, "options", opts);
		cJSON_AddNumberToObject(o, "fs_freq", fr);
		cJSON_AddNumberToObject(o, "fs_passno", pa);
		cJSON_AddItemToArray(arr, o);
	}
	fclose(f);
	return arr;
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
	cJSON_AddStringToObject(o, "id", dev_id_value(idfull));
	cJSON_AddStringToObject(o, "id_type", dev_id_type(idfull));
	if (strcmp(type, "swap") != 0)
		attach_fs_meta(o, name, mnt, idfull);
	cJSON_AddItemToArray(arr, o);
	return o;
}

/* block_devices: /sys/block whole-disk + partitions + dm(LVM/crypt/mpath)/md. parent=부모 id 값(복수면 노드 반복). swap=/proc/swaps. */
static cJSON *inv_collect_block_devices(void)
{
	cJSON *arr = cJSON_CreateArray();
	DIR *d = opendir("/sys/block");
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) != NULL) {
			const char *dev = e->d_name;
			if (dev[0] == '.' || is_excluded_block_dev(dev)) continue;
			char pth[320], v[256];
			long long size = -1;
			snprintf(pth, sizeof pth, "/sys/block/%s/size", dev);
			if (read_sysfs_str(pth, v, sizeof v)) size = strtoll(v, NULL, 10) * 512;
			char idfull[320]; disk_device_id(dev, idfull, sizeof idfull);
			char dmuuid[256];
			snprintf(pth, sizeof pth, "/sys/block/%s/dm/uuid", dev);
			int is_dm = read_sysfs_str(pth, dmuuid, sizeof dmuuid);
			int is_md = (strncmp(dev, "md", 2) == 0);
			const char *type = "disk";
			if (is_dm) {
				if      (!strncmp(dmuuid, "LVM-", 4))   type = "lvm";
				else if (!strncmp(dmuuid, "CRYPT-", 6)) type = "crypt";
				else if (!strncmp(dmuuid, "mpath-", 6)) type = "mpath";
				else                                     type = "dm";
			} else if (is_md) type = "raid";
			char fst[80] = {0}, mnt[300] = {0};
			int hm = dev_mount_info(dev, fst, sizeof fst, mnt, sizeof mnt);
			if (is_dm || is_md) {
				/* parent = 각 슬레이브(PV/멤버)의 id 값. 복수면 노드 반복. */
				char slp[320]; snprintf(slp, sizeof slp, "/sys/block/%s/slaves", dev);
				DIR *sd = opendir(slp); int emitted = 0;
				if (sd) {
					struct dirent *se;
					while ((se = readdir(sd))) {
						if (se->d_name[0] == '.') continue;
						char sid[320]; resolve_block_id(se->d_name, sid, sizeof sid);
						char pval[300]; snprintf(pval, sizeof pval, "%s", dev_id_value(sid));
						cJSON *nn = bd_add(arr, dev, type, size, hm ? fst : NULL, hm ? mnt : NULL, pval, idfull);
						attach_dm_type_meta(nn, dev, type, dmuuid);
						emitted = 1;
					}
					closedir(sd);
				}
				if (!emitted) {
					cJSON *nn = bd_add(arr, dev, type, size, hm ? fst : NULL, hm ? mnt : NULL, NULL, idfull);
					attach_dm_type_meta(nn, dev, type, dmuuid);
				}
			} else {
				struct pt_table pt;
				parse_partition_table(dev, &pt);
				cJSON *dn = bd_add(arr, dev, "disk", size, hm ? fst : NULL, hm ? mnt : NULL, NULL, idfull);
				attach_disk_meta(dn, dev);
				if (pt.kind == 1)      cJSON_AddStringToObject(dn, "partition_table", "gpt");
				else if (pt.kind == 2) cJSON_AddStringToObject(dn, "partition_table", "mbr");
				else                   cJSON_AddNullToObject(dn, "partition_table");
				/* 파티션 */
				snprintf(pth, sizeof pth, "/sys/block/%s", dev);
				DIR *pd = opendir(pth);
				if (pd) {
					struct dirent *pe;
					while ((pe = readdir(pd))) {
						if (strncmp(pe->d_name, dev, strlen(dev)) != 0) continue;
						char ppth[420];
						snprintf(ppth, sizeof ppth, "/sys/block/%s/%s/partition", dev, pe->d_name);
						if (access(ppth, F_OK) != 0) continue;
						int partno = -1;
						char pv[64];
						if (read_sysfs_str(ppth, pv, sizeof pv)) partno = atoi(pv);
						long long psize = -1, startsec = -1;
						snprintf(ppth, sizeof ppth, "/sys/block/%s/%s/size", dev, pe->d_name);
						if (read_sysfs_str(ppth, v, sizeof v)) psize = strtoll(v, NULL, 10) * 512;
						snprintf(ppth, sizeof ppth, "/sys/block/%s/%s/start", dev, pe->d_name);
						if (read_sysfs_str(ppth, v, sizeof v)) startsec = strtoll(v, NULL, 10);
						char pidfull[320]; part_device_id(pe->d_name, pidfull, sizeof pidfull);
						char pfst[80] = {0}, pmnt[300] = {0};
						int phm = dev_mount_info(pe->d_name, pfst, sizeof pfst, pmnt, sizeof pmnt);
						char parval[300]; snprintf(parval, sizeof parval, "%s", dev_id_value(idfull));
						cJSON *pn = bd_add(arr, pe->d_name, "part", psize, phm ? pfst : NULL, phm ? pmnt : NULL, parval, pidfull);
						if (partno >= 0) cJSON_AddNumberToObject(pn, "part_number", partno);
						else             cJSON_AddNullToObject(pn, "part_number");
						long long start_bytes = (startsec >= 0) ? startsec * 512 : -1;
						if (start_bytes >= 0) cJSON_AddNumberToObject(pn, "part_start_bytes", (double)start_bytes);
						else                  cJSON_AddNullToObject(pn, "part_start_bytes");
						part_attach_meta(pn, &pt, start_bytes);
					}
					closedir(pd);
				}
			}
		}
		closedir(d);
	}
	/* swap 노드(/proc/swaps): 파티션/파일 스왑. */
	FILE *sw = fopen("/proc/swaps", "r");
	if (sw) {
		char line[600]; int ln = 0;
		while (fgets(line, sizeof line, sw)) {
			if (++ln == 1) continue;
			char fname[300], stype[32]; long long ksize = -1;
			if (sscanf(line, "%299s %31s %lld", fname, stype, &ksize) < 3) continue;
			const char *base = strrchr(fname, '/'); base = base ? base + 1 : fname;
			char idfull[320];
			if (strcmp(stype, "partition") == 0) resolve_block_id(base, idfull, sizeof idfull);
			else                                 snprintf(idfull, sizeof idfull, "name:%s", base);
			bd_add(arr, base, "swap", ksize >= 0 ? ksize * 1024 : -1, "swap", "[SWAP]", NULL, idfull);
		}
		fclose(sw);
	}
	return arr;
}

/* 라우트: /proc/net/route(IPv4)에서 iff 의 비-default(dest!=0) + gateway!=0(정적) 라우트 {dest CIDR, via}.
   파일 열림+무매칭=[](측정 empty), 파일 실패=NULL(호출부 null). */
static cJSON *iface_routes(const char *iff)
{
	FILE *f = fopen("/proc/net/route", "r");
	if (!f) return NULL;
	cJSON *arr = cJSON_CreateArray();
	char line[512];
	int first = 1;
	while (fgets(line, sizeof line, f)) {
		if (first) { first = 0; continue; }
		char rif[64];
		unsigned long dest, gw, flags, mask;
		int refc, use, metric;
		if (sscanf(line, "%63s %lx %lx %lx %d %d %d %lx", rif, &dest, &gw, &flags, &refc, &use, &metric, &mask) < 8) continue;
		if (strcmp(rif, iff) != 0) continue;
		if (dest == 0 || gw == 0) continue; /* default 또는 링크 자동 라우트 제외 */
		unsigned pfx = 0, m = (unsigned)mask;
		while (m) { pfx += m & 1; m >>= 1; }
		char cidr[32], gwip[16];
		snprintf(cidr, sizeof cidr, "%lu.%lu.%lu.%lu/%u",
		    dest & 0xFF, (dest >> 8) & 0xFF, (dest >> 16) & 0xFF, (dest >> 24) & 0xFF, pfx);
		snprintf(gwip, sizeof gwip, "%lu.%lu.%lu.%lu",
		    gw & 0xFF, (gw >> 8) & 0xFF, (gw >> 16) & 0xFF, (gw >> 24) & 0xFF);
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "dest", cidr);
		cJSON_AddStringToObject(r, "via", gwip);
		cJSON_AddItemToArray(arr, r);
	}
	fclose(f);
	return arr;
}

/* /etc/resolv.conf nameserver 목록(전역). 없으면 NULL. */
static cJSON *resolv_dns(void)
{
	FILE *f = fopen("/etc/resolv.conf", "r");
	if (!f) return NULL;
	cJSON *arr = NULL;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (strncmp(p, "nameserver", 10) != 0) continue;
		p += 10;
		char ns[128];
		if (sscanf(p, "%127s", ns) != 1) continue;
		if (!arr) arr = cJSON_CreateArray();
		cJSON_AddItemToArray(arr, cJSON_CreateString(ns));
	}
	fclose(f);
	return arr;
}

/* netlink RTM_GETADDR 덤프 -> (ifindex,family,addr) 별 IFA_F_PERMANENT.
   getifaddrs 는 주소 플래그를 안 줘서 netlink 로 permanent 비트를 읽어 static/dhcp 를 구분한다. */
struct addr_origin { int ifindex; int family; unsigned char addr[16]; int permanent; };

static size_t netlink_addr_origins(struct addr_origin *out, size_t cap)
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0) return 0;
	struct { struct nlmsghdr nh; struct ifaddrmsg ifa; } req;
	memset(&req, 0, sizeof req);
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
	req.nh.nlmsg_type = RTM_GETADDR;
	req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.nh.nlmsg_seq = 1;
	req.ifa.ifa_family = AF_UNSPEC;
	if (send(fd, &req, req.nh.nlmsg_len, 0) < 0) { close(fd); return 0; }
	char buf[16384];
	size_t n = 0;
	int done = 0;
	while (!done) {
		ssize_t rl = recv(fd, buf, sizeof buf, 0);
		if (rl <= 0) break;
		int len = (int)rl;
		struct nlmsghdr *nh = (struct nlmsghdr *)buf;
		for (; NLMSG_OK(nh, len); nh = NLMSG_NEXT(nh, len)) {
			if (nh->nlmsg_type == NLMSG_DONE || nh->nlmsg_type == NLMSG_ERROR) { done = 1; break; }
			if (nh->nlmsg_type != RTM_NEWADDR) continue;
			struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nh);
			if (ifa->ifa_family != AF_INET && ifa->ifa_family != AF_INET6) continue;
			struct rtattr *rta = IFA_RTA(ifa);
			int rtalen = IFA_PAYLOAD(nh);
			const unsigned char *addr = NULL; int addrlen = 0;
			unsigned int flags = ifa->ifa_flags;
			for (; RTA_OK(rta, rtalen); rta = RTA_NEXT(rta, rtalen)) {
				if (rta->rta_type == IFA_ADDRESS)    { addr = (const unsigned char *)RTA_DATA(rta); addrlen = RTA_PAYLOAD(rta); }
				else if (rta->rta_type == IFA_FLAGS) flags = *(unsigned int *)RTA_DATA(rta);
			}
			if (!addr || n >= cap) continue;
			out[n].ifindex = (int)ifa->ifa_index;
			out[n].family = ifa->ifa_family;
			memset(out[n].addr, 0, sizeof out[n].addr);
			memcpy(out[n].addr, addr, addrlen > 16 ? 16 : addrlen);
			out[n].permanent = (flags & IFA_F_PERMANENT) ? 1 : 0;
			n++;
		}
	}
	close(fd);
	return n;
}

/* net_interfaces: /sys/class/net 열거 -> MAC id + kind + speed + addresses[](getifaddrs) + origin(netlink) + gateway. */
static cJSON *inv_collect_net_interfaces(void)
{
	cJSON *arr = cJSON_CreateArray();
	cJSON *gw4 = build_default_gw_v4();
	cJSON *dns = resolv_dns(); /* 전역 DNS. default-route iface 에 부착 */
	struct ifaddrs *ifap = NULL;
	getifaddrs(&ifap);
	static struct addr_origin origins[256];
	size_t n_origin = netlink_addr_origins(origins, sizeof origins / sizeof origins[0]);

	DIR *d = opendir("/sys/class/net");
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) != NULL) {
			const char *iff = e->d_name;
			if (iff[0] == '.' || strcmp(iff, "lo") == 0) continue;
			char idfull[320]; net_device_id(iff, idfull, sizeof idfull);
			const char *id_type = (!strncmp(idfull, "mac:", 4)) ? "mac"
			                    : (!strncmp(idfull, "by-path:", 8)) ? "by-path" : "name";
			cJSON *o = cJSON_CreateObject();
			cJSON_AddStringToObject(o, "name", iff);
			cJSON_AddStringToObject(o, "id", dev_id_value(idfull));
			cJSON_AddStringToObject(o, "id_type", id_type);
			cJSON_AddStringToObject(o, "kind", net_kind(iff));
			char sp[320], spv[32];
			snprintf(sp, sizeof sp, "/sys/class/net/%s/speed", iff);
			if (read_sysfs_str(sp, spv, sizeof spv) && strtol(spv, NULL, 10) > 0)
				cJSON_AddNumberToObject(o, "speed_mbps", (double)strtol(spv, NULL, 10));
			else cJSON_AddNullToObject(o, "speed_mbps");
			/* addresses[] */
			unsigned int ifidx = if_nametoindex(iff);
			cJSON *addrs = cJSON_CreateArray();
			for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
				if (!ifa->ifa_addr || !ifa->ifa_name || strcmp(ifa->ifa_name, iff) != 0) continue;
				int fam = ifa->ifa_addr->sa_family;
				char ip[INET6_ADDRSTRLEN]; int prefix = 0; const char *family;
				if (fam == AF_INET) {
					if (!inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, ip, sizeof ip)) continue;
					if (ifa->ifa_netmask) prefix = ipv4_netmask_prefix(((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr.s_addr);
					family = "ipv4";
				} else if (fam == AF_INET6) {
					if (!inet_ntop(AF_INET6, &((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr, ip, sizeof ip)) continue;
					if (ifa->ifa_netmask) prefix = ipv6_netmask_prefix((struct sockaddr_in6 *)ifa->ifa_netmask);
					family = "ipv6";
				} else continue;
				cJSON *a = cJSON_CreateObject();
				cJSON_AddStringToObject(a, "address", ip);
				cJSON_AddNumberToObject(a, "prefix", (double)prefix);
				cJSON_AddStringToObject(a, "family", family);
				/* origin: netlink IFA_F_PERMANENT 조회. permanent=static, 아니면 dhcp, 미매칭=null. */
				const char *origin = NULL;
				const unsigned char *ab = (fam == AF_INET)
					? (const unsigned char *)&((struct sockaddr_in *)ifa->ifa_addr)->sin_addr
					: (const unsigned char *)&((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr;
				int ablen = (fam == AF_INET) ? 4 : 16;
				for (size_t k = 0; k < n_origin; k++) {
					if (origins[k].ifindex == (int)ifidx && origins[k].family == fam &&
					    memcmp(origins[k].addr, ab, ablen) == 0) {
						origin = origins[k].permanent ? "static" : "dhcp";
						break;
					}
				}
				if (origin) cJSON_AddStringToObject(a, "origin", origin);
				else        cJSON_AddNullToObject(a, "origin");
				cJSON_AddItemToArray(addrs, a);
			}
			cJSON_AddItemToObject(o, "addresses", addrs);
			cJSON *hit = cJSON_GetObjectItem(gw4, iff);
			int has_gw = (hit && cJSON_IsString(hit));
			cJSON_AddItemToObject(o, "gateway", has_gw ? cJSON_CreateString(hit->valuestring) : cJSON_CreateNull());
			/* mtu */
			char np[320], nv[64];
			snprintf(np, sizeof np, "/sys/class/net/%s/mtu", iff);
			if (read_sysfs_str(np, nv, sizeof nv) && atoi(nv) > 0) cJSON_AddNumberToObject(o, "mtu", atoi(nv));
			else                                                   cJSON_AddNullToObject(o, "mtu");
			/* bond_mode: 본딩 iface 의 raw 토큰(엔진 정규화) */
			snprintf(np, sizeof np, "/sys/class/net/%s/bonding/mode", iff);
			if (read_sysfs_str(np, nv, sizeof nv)) {
				char tok[64];
				if (sscanf(nv, "%63s", tok) == 1) cJSON_AddStringToObject(o, "bond_mode", tok);
				else                              cJSON_AddNullToObject(o, "bond_mode");
			} else cJSON_AddNullToObject(o, "bond_mode");
			/* vlan_id: /proc/net/vlan/<iff> VID */
			int vid = -1;
			snprintf(np, sizeof np, "/proc/net/vlan/%s", iff);
			FILE *vf = fopen(np, "r");
			if (vf) {
				char l[256];
				while (fgets(l, sizeof l, vf)) {
					char *q = strstr(l, "VID:");
					if (q) { vid = atoi(q + 4); break; }
				}
				fclose(vf);
			}
			if (vid >= 0) cJSON_AddNumberToObject(o, "vlan_id", vid);
			else          cJSON_AddNullToObject(o, "vlan_id");
			/* routes: 정적 비-default */
			cJSON *rts = iface_routes(iff);
			cJSON_AddItemToObject(o, "routes", rts ? rts : cJSON_CreateNull());
			/* dns: 전역 목록을 default-route iface 에만 부착(복제) */
			if (has_gw && dns) cJSON_AddItemToObject(o, "dns", cJSON_Duplicate(dns, 1));
			else               cJSON_AddNullToObject(o, "dns");
			cJSON_AddItemToArray(arr, o);
		}
		closedir(d);
	}
	if (ifap) freeifaddrs(ifap);
	cJSON_Delete(gw4);
	if (dns) cJSON_Delete(dns);
	return arr;
}
