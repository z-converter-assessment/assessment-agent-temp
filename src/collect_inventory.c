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

struct sock_inode_owner {
	long inode;
	int  pid;
	char comm[64];
};

static cJSON *inv_collect_block_devices(void);
static cJSON *inv_collect_external_ip(void);
static cJSON *inv_collect_listen_ports(void);
static cJSON *inv_collect_net_interfaces(void);
static cJSON *inv_collect_services(void);
static cJSON *inv_collect_services_sysv(void);
static const char *dev_id_type(const char *full);
static const char *dev_id_value(const char *full);
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
static void bd_add(cJSON *arr, const char *name, const char *type, long long size,
                   const char *fst, const char *mnt, const char *parent, const char *idfull);
static void parse_tcp_v4_hex_addr(const char *hex8, char *out, size_t out_len);
static void parse_tcp_v6_hex_addr(const char *hex32, char *out, size_t out_len);
static void part_device_id(const char *part, char *out, size_t outsz);
static void resolve_block_id(const char *name, char *out, size_t outsz);
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

cJSON *collect_inventory_payload(const char *machine_id, const char *agent_version)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;
	wire_add_envelope(root, "inventory", machine_id, agent_version);

	int ok = 1;
	add_os_release(root);
	if (!add_kernel_version(root))     ok = 0;
	if (!add_cpu_cores(root))          ok = 0;
	add_cpu_model(root);

	/* mem_total_bytes (base 단위). swap 은 block_devices type=swap 노드로(P4). */
	{
		char *mi = read_file_all("/proc/meminfo");
		long kb = mi ? meminfo_get_kb(mi, "MemTotal") : -1;
		if (mi) free(mi);
		if (kb >= 0) cJSON_AddNumberToObject(root, "mem_total_bytes", (double)kb * 1024.0);
		else         cJSON_AddNullToObject(root, "mem_total_bytes");
	}

	cJSON_AddItemToObject(root, "services",     wire_or_null(inv_collect_services()));
	cJSON_AddItemToObject(root, "listen_ports", wire_or_empty_array(inv_collect_listen_ports()));
	cJSON_AddItemToObject(root, "ip_external",  inv_collect_external_ip());

	cJSON_AddItemToObject(root, "block_devices",  wire_or_empty_array(inv_collect_block_devices()));
	cJSON_AddItemToObject(root, "net_interfaces", wire_or_empty_array(inv_collect_net_interfaces()));

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

static const char *dev_id_value(const char *full)
{
	const char *c = strchr(full, ':');
	return c ? c + 1 : full;
}

/* 파티션 안정키: by-partuuid -> name. */
static void part_device_id(const char *part, char *out, size_t outsz)
{
	DIR *d = opendir("/dev/disk/by-partuuid");
	if (d) {
		struct dirent *e; char lp[600], tgt[300];
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.') continue;
			snprintf(lp, sizeof lp, "/dev/disk/by-partuuid/%s", e->d_name);
			ssize_t r = readlink(lp, tgt, sizeof tgt - 1);
			if (r <= 0) continue;
			tgt[r] = '\0';
			const char *base = strrchr(tgt, '/'); base = base ? base + 1 : tgt;
			if (strcmp(base, part) == 0) { snprintf(out, outsz, "partuuid:%s", e->d_name); closedir(d); return; }
		}
		closedir(d);
	}
	snprintf(out, outsz, "name:%s", part);
}

/* 슬레이브/PV 는 파티션일 수도 whole-disk 일 수도 -> 각각의 안정키로 해석. */
static void resolve_block_id(const char *name, char *out, size_t outsz)
{
	char pth[320];
	snprintf(pth, sizeof pth, "/sys/class/block/%s/partition", name);
	if (access(pth, F_OK) == 0) part_device_id(name, out, outsz);
	else                        disk_device_id(name, out, outsz);
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
		if (strcmp(base, name) == 0) { snprintf(fst, fsz, "%s", t); snprintf(mnt, msz, "%s", m); found = 1; break; }
	}
	fclose(f);
	return found;
}

static void bd_add(cJSON *arr, const char *name, const char *type, long long size,
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
	cJSON_AddItemToArray(arr, o);
}

/* P4 block_devices: /sys/block whole-disk + partitions + dm(LVM/crypt/mpath)/md. parent=부모 id 값(복수면 노드 반복). swap=/proc/swaps. */
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
						bd_add(arr, dev, type, size, hm ? fst : NULL, hm ? mnt : NULL, pval, idfull);
						emitted = 1;
					}
					closedir(sd);
				}
				if (!emitted) bd_add(arr, dev, type, size, hm ? fst : NULL, hm ? mnt : NULL, NULL, idfull);
			} else {
				bd_add(arr, dev, "disk", size, hm ? fst : NULL, hm ? mnt : NULL, NULL, idfull);
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
						long long psize = -1;
						snprintf(ppth, sizeof ppth, "/sys/block/%s/%s/size", dev, pe->d_name);
						if (read_sysfs_str(ppth, v, sizeof v)) psize = strtoll(v, NULL, 10) * 512;
						char pidfull[320]; part_device_id(pe->d_name, pidfull, sizeof pidfull);
						char pfst[80] = {0}, pmnt[300] = {0};
						int phm = dev_mount_info(pe->d_name, pfst, sizeof pfst, pmnt, sizeof pmnt);
						char parval[300]; snprintf(parval, sizeof parval, "%s", dev_id_value(idfull));
						bd_add(arr, pe->d_name, "part", psize, phm ? pfst : NULL, phm ? pmnt : NULL, parval, pidfull);
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

/* P3 net_interfaces: /sys/class/net 열거 -> MAC id + kind + speed + addresses[](getifaddrs) + gateway(default route). */
static cJSON *inv_collect_net_interfaces(void)
{
	cJSON *arr = cJSON_CreateArray();
	cJSON *gw4 = build_default_gw_v4();
	struct ifaddrs *ifap = NULL;
	getifaddrs(&ifap);

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
				cJSON_AddItemToArray(addrs, a);
			}
			cJSON_AddItemToObject(o, "addresses", addrs);
			cJSON *hit = cJSON_GetObjectItem(gw4, iff);
			cJSON_AddItemToObject(o, "gateway", (hit && cJSON_IsString(hit)) ? cJSON_CreateString(hit->valuestring) : cJSON_CreateNull());
			cJSON_AddItemToArray(arr, o);
		}
		closedir(d);
	}
	if (ifap) freeifaddrs(ifap);
	cJSON_Delete(gw4);
	return arr;
}
