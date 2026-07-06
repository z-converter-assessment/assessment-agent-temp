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
#include <time.h>
#include <unistd.h>

#define AGENT_VERSION_FALLBACK "0.0.0-dev"

static const char *cached_boot_time_iso(void)
{
	static char buf[32];
	static int initialized = 0;
	static int ok = 0;

	if (!initialized) {
		initialized = 1;
		char *content = read_file_all("/proc/stat");
		if (content) {

			const char *p = NULL;
			if (strncmp(content, "btime ", 6) == 0) {
				p = content + 6;
			} else {
				const char *needle = strstr(content, "\nbtime ");
				if (needle) p = needle + 7;
			}
			if (p) {
				char *end = NULL;
				long long epoch = strtoll(p, &end, 10);
				if (end != p && epoch > 0) {
					iso8601_utc((time_t)epoch, buf, sizeof buf);
					ok = 1;
				}
			}
			free(content);
		}
	}
	return ok ? buf : NULL;
}

static const char *cached_agent_started_at_iso(void)
{
	static char buf[32];
	static int initialized = 0;

	if (!initialized) {
		initialized = 1;
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		iso8601_utc(now.tv_sec, buf, sizeof buf);
	}
	return buf;
}

static void add_common_metadata(cJSON *obj,
                                const char *message_type,
                                const char *machine_id,
                                const char *agent_version)
{
	cJSON_AddStringToObject(obj, "message_type", message_type);
	/* machine_id는 원시 머신 식별자다. 구할 수 없으면 억지로 채우지 않고 null(측정 불가).
	 * 식별·라우팅은 agent_id로 하고, composite_id는 mac 기반으로 유니크가 유지된다. */
	if (machine_id && *machine_id)
		cJSON_AddStringToObject(obj, "machine_id", machine_id);
	else
		cJSON_AddNullToObject(obj, "machine_id");
	cJSON_AddStringToObject(obj, "composite_id", cached_composite_id(machine_id));
	cJSON_AddStringToObject(obj, "agent_id", cached_agent_id());
	cJSON_AddStringToObject(obj, "os_family", "linux");
	cJSON_AddStringToObject(obj, "agent_version",
	                        agent_version && *agent_version ? agent_version
	                                                        : AGENT_VERSION_FALLBACK);

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	char ts_buf[32];
	iso8601_utc(ts.tv_sec, ts_buf, sizeof ts_buf);
	cJSON_AddStringToObject(obj, "collected_at", ts_buf);

	const char *override = getenv("AGENT_HOSTNAME_OVERRIDE");
	char host_buf[HOST_NAME_MAX + 1];
	if (override && *override) {
		cJSON_AddStringToObject(obj, "hostname", override);
	} else if (gethostname(host_buf, sizeof host_buf) == 0) {
		host_buf[sizeof host_buf - 1] = '\0';
		cJSON_AddStringToObject(obj, "hostname", host_buf);
	} else {
		cJSON_AddStringToObject(obj, "hostname", "unknown");
	}

	char uuid_buf[64];
	uuid_v4(uuid_buf, sizeof uuid_buf);
	cJSON_AddStringToObject(obj, "message_id", uuid_buf);

	const char *boot = cached_boot_time_iso();
	if (boot) cJSON_AddStringToObject(obj, "boot_time", boot);
	else      cJSON_AddNullToObject(obj, "boot_time");

	cJSON_AddStringToObject(obj, "agent_started_at",
	                        cached_agent_started_at_iso());
}

static cJSON *or_empty_array(cJSON *arr)
{
	return arr ? arr : cJSON_CreateArray();
}

/* For fields with a legitimate "unmeasurable" state (services: no systemd and
 * no /var/lock/subsys), keep the key present as null instead of dropping it on
 * a C-NULL (e.g. alloc failure). cJSON silently ignores a NULL item add. */
static cJSON *or_json_null(cJSON *v)
{
	return v ? v : cJSON_CreateNull();
}

/* 정수 metrics 를 발행하되 -1 센티넬(측정불가)이면 null. 카운터/kb 등 음수 불가
 * 값에 공통 적용 — 값=실측/null=측정불가 계약을 한 곳에서 강제한다. */
static void add_long_or_null(cJSON *root, const char *key, long val)
{
	if (val < 0) cJSON_AddNullToObject(root, key);
	else         cJSON_AddNumberToObject(root, key, (double)val);
}

/* /proc 파일 하나를 읽어 첫 정수를 발행한다. 파일 부재(모듈/기능 미탑재)면
 * null — conntrack 처럼 존재 자체가 조건부인 카운터용. */
static void add_proc_long_file(cJSON *root, const char *key, const char *path)
{
	char *c = read_file_all(path);
	if (c) {
		cJSON_AddNumberToObject(root, key, (double)strtol(c, NULL, 10));
		free(c);
	} else {
		cJSON_AddNullToObject(root, key);
	}
}

static int is_excluded_block_dev(const char *name)
{
	if (!name || !*name) return 1;
	return strncmp(name, "loop", 4) == 0
	    || strncmp(name, "ram",  3) == 0
	    || strncmp(name, "sr",   2) == 0
	    || strncmp(name, "fd",   2) == 0;
}

static int parse_major_minor(const char *s, int *major, int *minor)
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

static void add_major_minor(cJSON *obj, int major, int minor)
{
	if (major >= 0) cJSON_AddNumberToObject(obj, "major", (double)major);
	if (minor >= 0) cJSON_AddNumberToObject(obj, "minor", (double)minor);
}

static int is_machine_id(const char *s)
{
	size_t n = 0;
	for (const char *p = s; *p; p++) {
		if (!isxdigit((unsigned char)*p))
			return 0;
		n++;
	}
	return n == 32;
}

static char *try_machine_id_file(const char *path)
{
	char *raw = read_file_all(path);
	if (!raw)
		return NULL;
	trim_inplace(raw);
	if (!is_machine_id(raw)) {
		free(raw);
		return NULL;
	}
	return raw;
}

static char *try_dbus_uuidgen(void)
{
	char *raw = run_cmd("dbus-uuidgen --get 2>/dev/null");
	if (!raw)
		return NULL;
	trim_inplace(raw);
	if (!*raw || !is_machine_id(raw)) {
		free(raw);
		return NULL;
	}
	return raw;
}

static const char *detect_cloud_vendor(void)
{
	char *vendor = read_file_all("/sys/class/dmi/id/sys_vendor");
	if (!vendor)
		return NULL;
	trim_inplace(vendor);

	const char *result = NULL;
	if (strstr(vendor, "Amazon"))
		result = "aws";
	else if (strstr(vendor, "Microsoft"))
		result = "azure";
	else if (strstr(vendor, "Google"))
		result = "gcp";

	free(vendor);
	return result;
}

/* 클라우드 IMDS 에서 metadata 값 하나를 페치한다. vendor 별 인증/헤더/경로 차이를
 * 흡수 — AWS 는 IMDSv2 토큰 선취득, azure/gcp 는 전용 헤더. vendor 가 아니거나
 * 페치 실패면 NULL, 200 이지만 빈 응답이면 빈 문자열(호출자가 null/빈배열 구분). */
static char *fetch_cloud_metadata(const char *aws_path, const char *azure_path,
                                  const char *gcp_path)
{
	const char *vendor = detect_cloud_vendor();
	if (!vendor)
		return NULL;

	char *out = NULL;
	char cmd[512];
	if (strcmp(vendor, "aws") == 0) {
		char *token = run_cmd(
			"curl -fsS -m 1 -X PUT "
			"-H 'X-aws-ec2-metadata-token-ttl-seconds: 60' "
			"http://169.254.169.254/latest/api/token 2>/dev/null");
		if (token && *token) {
			trim_inplace(token);
			snprintf(cmd, sizeof cmd,
			         "curl -fsS -m 1 -H 'X-aws-ec2-metadata-token: %s' "
			         "http://169.254.169.254/latest/meta-data/%s 2>/dev/null",
			         token, aws_path);
			out = run_cmd(cmd);
		}
		free(token);
	} else if (strcmp(vendor, "azure") == 0) {
		snprintf(cmd, sizeof cmd,
		         "curl -fsS -m 1 -H 'Metadata: true' "
		         "'http://169.254.169.254/metadata/instance/%s"
		         "?api-version=2021-02-01&format=text' 2>/dev/null",
		         azure_path);
		out = run_cmd(cmd);
	} else if (strcmp(vendor, "gcp") == 0) {
		snprintf(cmd, sizeof cmd,
		         "curl -fsS -m 1 -H 'Metadata-Flavor: Google' "
		         "http://metadata.google.internal/computeMetadata/v1/%s 2>/dev/null",
		         gcp_path);
		out = run_cmd(cmd);
	}
	if (!out)
		return NULL;
	trim_inplace(out);
	return out;
}

static char *try_cloud_instance_id(void)
{
	char *out = fetch_cloud_metadata("instance-id", "compute/vmId", "instance/id");
	if (!out || !*out) {
		free(out);
		return NULL;
	}
	return out;
}

char *resolve_machine_id(void)
{
	char *id = try_machine_id_file("/etc/machine-id");
	if (id)
		return id;
	id = try_machine_id_file("/var/lib/dbus/machine-id");
	if (id)
		return id;
	id = try_dbus_uuidgen();
	if (id)
		return id;
	return try_cloud_instance_id();
}

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

/* 구세대 SUSE(/etc/SuSE-release) 보완. os-release 는 SLES 12+·openSUSE 에만 있어
 * SLES 11 은 이 파일로만 OS 를 식별한다. 포맷:
 *   SUSE Linux Enterprise Server 11 (x86_64)
 *   VERSION = 11
 *   PATCHLEVEL = 3
 * -> os_id=sles, os_version="11.3"(PATCHLEVEL 없으면 VERSION 만), os_codename=null. */
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

/* /etc/lsb-release 보완(os-release 없는 lsb-only 구형 Ubuntu/Debian 등).
 * DISTRIB_ID/DISTRIB_RELEASE/DISTRIB_CODENAME -> os_id(소문자)/os_version/os_codename. */
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
	add_os_release(root);   /* os_id, os_version, os_codename */
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

static long meminfo_get_kb(const char *content, const char *key)
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

static int add_mem_total_swap_total(cJSON *root)
{
	char *content = read_file_all("/proc/meminfo");
	if (!content)
		return 0;
	long mem_total = meminfo_get_kb(content, "MemTotal");
	long swap_total = meminfo_get_kb(content, "SwapTotal");
	free(content);
	if (mem_total < 0)
		return 0;
	cJSON_AddNumberToObject(root, "mem_total_kb", (double)mem_total);
	add_long_or_null(root, "swap_total_kb", swap_total);
	return 1;
}

/* ---- device kind 분류기 — disk_io/net_io/mounts/interfaces 공용 ----
 * "가상이냐" 판정을 한 곳으로 모아 엔진의 정규식/major 추론을 대체한다. */

static const char *disk_kind(const char *dev)
{
	char p[288];
	snprintf(p, sizeof p, "/sys/class/block/%s/partition", dev);
	if (access(p, F_OK) == 0) return "partition";
	snprintf(p, sizeof p, "/sys/block/%s/dm", dev);
	if (access(p, F_OK) == 0) return "lvm";
	snprintf(p, sizeof p, "/sys/block/%s/md", dev);
	if (access(p, F_OK) == 0) return "raid";
	snprintf(p, sizeof p, "/sys/block/%s/device", dev);
	if (access(p, F_OK) == 0) return "physical";
	return "virtual";
}

static const char *net_kind(const char *ifname)
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
	/* uevent DEVTYPE is the kernel's own name for the virtual device type —
	 * authoritative and general, so map every taxonomy-relevant type from it
	 * rather than only vlan. Overlay/tunnel encaps all fold to "tunnel";
	 * macvlan/ipvlan/macvtap have no dedicated kind and fall through to
	 * "virtual" below. */
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

static const char *mount_kind(const char *mountpoint, const char *fstype)
{
	if (mountpoint && (strcmp(mountpoint, "/boot") == 0 ||
	                   strncmp(mountpoint, "/boot/", 6) == 0))
		return "boot";
	if (fstype && (strcmp(fstype, "squashfs") == 0 ||
	               strcmp(fstype, "iso9660") == 0 ||
	               strcmp(fstype, "udf") == 0))
		return "image";
	return "data";
}

static cJSON *collect_disks_via_lsblk(void)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;

	char *out = run_cmd("lsblk -dn -b -e 7,11 -o NAME,KNAME,MAJ:MIN,SIZE,TYPE -J 2>/dev/null");
	if (!out)
		return arr;

	cJSON *parsed = cJSON_Parse(out);
	free(out);
	if (!parsed)
		return arr;

	cJSON *devices = cJSON_GetObjectItemCaseSensitive(parsed, "blockdevices");
	cJSON *dev = NULL;
	cJSON_ArrayForEach(dev, devices) {
		cJSON *name = cJSON_GetObjectItemCaseSensitive(dev, "name");
		cJSON *kname = cJSON_GetObjectItemCaseSensitive(dev, "kname");

		cJSON *majmin = cJSON_GetObjectItemCaseSensitive(dev, "maj:min");
		if (!majmin)
			majmin = cJSON_GetObjectItemCaseSensitive(dev, "MAJ:MIN");
		cJSON *size = cJSON_GetObjectItemCaseSensitive(dev, "size");
		cJSON *type = cJSON_GetObjectItemCaseSensitive(dev, "type");
		if (!cJSON_IsString(name))
			continue;
		if (is_excluded_block_dev(name->valuestring))
			continue;

		int major = -1, minor = -1;
		if (cJSON_IsString(majmin))
			parse_major_minor(majmin->valuestring, &major, &minor);

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "name", name->valuestring);
		add_major_minor(item, major, minor);

		/* size/type come straight from lsblk; if a field is absent or malformed
		 * emit null rather than a fabricated 0 / "disk". The engine filters on
		 * kind (below), not on the raw lsblk type. */
		if (cJSON_IsNumber(size))
			cJSON_AddNumberToObject(item, "size_bytes", size->valuedouble);
		else if (cJSON_IsString(size) && *size->valuestring)
			cJSON_AddNumberToObject(item, "size_bytes",
			                        strtod(size->valuestring, NULL));
		else
			cJSON_AddNullToObject(item, "size_bytes");

		if (cJSON_IsString(type))
			cJSON_AddStringToObject(item, "type", type->valuestring);
		else
			cJSON_AddNullToObject(item, "type");
		/* disk_kind 는 sysfs 를 kernel 이름 키로 probe 하므로 KNAME(dm-0 등)을 쓴다 — lsblk
		 * NAME(친화명 vg0-lv0)이면 sysfs miss 로 오분류(virtual). disk_io[](diskstats kernel
		 * 이름)와 같은 키로 분류해 크로스메시지 kind 드리프트를 막는다. name 발행은 NAME 유지. */
		const char *kn = (cJSON_IsString(kname) && *kname->valuestring)
		                 ? kname->valuestring : name->valuestring;
		cJSON_AddStringToObject(item, "kind", disk_kind(kn));
		cJSON_AddItemToArray(arr, item);
	}
	cJSON_Delete(parsed);
	return arr;
}

static cJSON *collect_disks_via_sysfs(void)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;

	DIR *d = opendir("/sys/block");
	if (!d)
		return arr;

	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		if (is_excluded_block_dev(e->d_name))
			continue;

		char path[512];
		snprintf(path, sizeof path, "/sys/block/%s/device", e->d_name);
		if (access(path, F_OK) != 0)
			continue;

		snprintf(path, sizeof path, "/sys/block/%s/size", e->d_name);
		char *content = read_file_all(path);
		if (!content)
			continue;
		long sectors = strtol(content, NULL, 10);
		free(content);
		if (sectors <= 0)
			continue;

		int major = -1, minor = -1;
		snprintf(path, sizeof path, "/sys/block/%s/dev", e->d_name);
		char *dev_str = read_file_all(path);
		if (dev_str) {
			trim_inplace(dev_str);
			parse_major_minor(dev_str, &major, &minor);
			free(dev_str);
		}

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "name", e->d_name);
		add_major_minor(item, major, minor);
		cJSON_AddNumberToObject(item, "size_bytes", (double)sectors * 512.0);
		cJSON_AddStringToObject(item, "type", "disk");
		cJSON_AddStringToObject(item, "kind", disk_kind(e->d_name));
		cJSON_AddItemToArray(arr, item);
	}
	closedir(d);
	return arr;
}

static cJSON *collect_disks(void)
{
	cJSON *arr = collect_disks_via_lsblk();
	if (arr && cJSON_GetArraySize(arr) > 0)
		return arr;
	cJSON_Delete(arr);
	return collect_disks_via_sysfs();
}

static int is_excluded_fstype(const char *fstype)
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

/* /proc/filesystems marks device-less filesystems with a leading "nodev" — the
 * kernel's own authority on what has no backing block device. Every pseudo/
 * virtual fs (proc, sysfs, cgroup, selinuxfs, usbfs, ...) is nodev; real block
 * filesystems (ext4, xfs, fuseblk/ntfs-3g) are not. This catches pseudo mounts
 * the static skip list above misses, on any kernel, without a growing denylist.
 * The registered set only lists currently-loaded fs, which always includes any
 * fs that is actually mounted — exactly the ones we classify. Result cached. */
static int fstype_is_nodev(const char *fstype)
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

/* Device-less filesystems that nonetheless hold real (remote/user) data and so
 * must NOT be dropped as pseudo: network filesystems and FUSE mounts. fuseblk
 * (real block device, e.g. ntfs-3g) is not nodev and never reaches here. */
static int is_kept_deviceless_fs(const char *fstype)
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

struct mount_entry {
	int   major;
	int   minor;
	char *mount;
	char *fstype;
};

static void free_mount_entries(struct mount_entry *arr, size_t n)
{
	if (!arr) return;
	for (size_t i = 0; i < n; i++) {
		free(arr[i].mount);
		free(arr[i].fstype);
	}
	free(arr);
}

static int parse_mountinfo_line(const char *line,
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

static void dedup_mounts(struct mount_entry *arr, size_t *count)
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

static struct mount_entry *list_real_mounts(size_t *out_count)
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
		/* Drop pseudo/virtual mounts so mounts[] carries only real storage:
		 * the static skip list (fast path) plus any device-less (nodev) fs the
		 * kernel reports, except network/FUSE which hold real data. Without the
		 * nodev net, fs missing from the skip list (selinuxfs, usbfs, ...) would
		 * leak through and be mislabeled kind="data". */
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

/* Split by role so the static inventory does not carry time-series data:
 *   with_usage=0 (inventory) -> structure: mount/major/minor/total_bytes/fstype/kind
 *   with_usage=1 (metrics)   -> usage:     mount/kind/fstype/total_bytes/free/avail
 * total_bytes stays in both (metrics needs it for %, like mem_total_kb); the
 * dynamic free/avail live only in metrics, so disk usage has one time-series
 * source instead of also being duplicated into every inventory snapshot. */
static cJSON *collect_mounts(int with_usage)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;

	size_t n = 0;
	struct mount_entry *mounts = list_real_mounts(&n);
	for (size_t i = 0; i < n; i++) {
		struct statvfs st;
		if (statvfs(mounts[i].mount, &st) != 0)
			continue;
		double total = (double)st.f_blocks * (double)st.f_frsize;

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "mount", mounts[i].mount);
		cJSON_AddStringToObject(item, "kind",
		                        mount_kind(mounts[i].mount, mounts[i].fstype));
		cJSON_AddStringToObject(item, "fstype", mounts[i].fstype);
		cJSON_AddNumberToObject(item, "total_bytes", total);
		if (with_usage) {
			double freeb = (double)st.f_bfree  * (double)st.f_frsize;
			double avail = (double)st.f_bavail * (double)st.f_frsize;
			cJSON_AddNumberToObject(item, "free_bytes",  freeb);
			cJSON_AddNumberToObject(item, "avail_bytes", avail);
			/* inode: 작은 파일 폭증 시 바이트가 남아도 inode 가 먼저 소진돼
			 * ENOSPC. 같은 statvfs 호출이라 공짜. inode 개념 없는 fs 는 f_files=0. */
			cJSON_AddNumberToObject(item, "inodes_total", (double)st.f_files);
			cJSON_AddNumberToObject(item, "inodes_free",  (double)st.f_ffree);
		} else {
			add_major_minor(item, mounts[i].major, mounts[i].minor);
		}
		cJSON_AddItemToArray(arr, item);
	}
	free_mount_entries(mounts, n);
	return arr;
}

static int ipv4_netmask_prefix(uint32_t mask_n)
{
	uint32_t m = ntohl(mask_n);
	int n = 0;
	while (m & 0x80000000u) { n++; m <<= 1; }
	return n;
}

static int ipv6_netmask_prefix(const struct sockaddr_in6 *mask)
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

/* /proc/net/route의 default route(dest=0, mask=0) gateway를 iface->IP 문자열 맵으로.
 * 엔진 토폴로지 서브넷 disambiguation용. IPv4만 취득(엔진 용도는 사설 대역 중복 판별). */
static cJSON *build_default_gw_v4(void)
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

/* 구조화 인터페이스 배열(name/address/prefix/family/kind/gateway, IPv6 포함). */
static cJSON *collect_interfaces(void)
{
	cJSON *arr = cJSON_CreateArray();
	struct ifaddrs *ifap = NULL;
	if (getifaddrs(&ifap) != 0)
		return arr;

	cJSON *gw4 = build_default_gw_v4();

	for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr)
			continue;
		if (ifa->ifa_flags & IFF_LOOPBACK)
			continue;
		int fam = ifa->ifa_addr->sa_family;
		char ip[INET6_ADDRSTRLEN];
		int prefix = 0;
		const char *family;
		if (fam == AF_INET) {
			struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
			if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip))
				continue;
			if (ifa->ifa_netmask && ifa->ifa_netmask->sa_family == AF_INET)
				prefix = ipv4_netmask_prefix(
				    ((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr.s_addr);
			family = "ipv4";
		} else if (fam == AF_INET6) {
			struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ifa->ifa_addr;
			if (!inet_ntop(AF_INET6, &sin6->sin6_addr, ip, sizeof ip))
				continue;
			if (ifa->ifa_netmask && ifa->ifa_netmask->sa_family == AF_INET6)
				prefix = ipv6_netmask_prefix(
				    (struct sockaddr_in6 *)ifa->ifa_netmask);
			family = "ipv6";
		} else {
			continue;
		}

		const char *name = ifa->ifa_name ? ifa->ifa_name : "";
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "name",    name);
		cJSON_AddStringToObject(o, "address", ip);
		cJSON_AddNumberToObject(o, "prefix",  (double)prefix);
		cJSON_AddStringToObject(o, "family",  family);
		cJSON_AddStringToObject(o, "kind",    net_kind(name));
		/* gateway = 이 iface의 IPv4 default route(있으면). v6·미보유는 null. */
		cJSON *g = NULL;
		if (fam == AF_INET) {
			cJSON *hit = cJSON_GetObjectItem(gw4, name);
			if (hit && cJSON_IsString(hit))
				g = cJSON_CreateString(hit->valuestring);
		}
		cJSON_AddItemToObject(o, "gateway", g ? g : cJSON_CreateNull());
		cJSON_AddItemToArray(arr, o);
	}
	freeifaddrs(ifap);
	cJSON_Delete(gw4);
	return arr;
}

static int mac_str_cmp(const void *a, const void *b)
{
	return strcmp(*(const char * const *)a, *(const char * const *)b);
}

static cJSON *collect_mac_addresses(void)
{
	cJSON *arr = cJSON_CreateArray();
	DIR *d = opendir("/sys/class/net");
	if (!d)
		return arr;

	enum { MAC_CAP = 64 };
	char *macs[MAC_CAP];
	int count = 0;

	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')                       continue;
		if (strcmp(e->d_name, "lo") == 0)              continue;
		if (strncmp(e->d_name, "docker", 6) == 0)      continue;
		if (strncmp(e->d_name, "br-", 3) == 0)         continue;
		if (strncmp(e->d_name, "veth", 4) == 0)        continue;
		if (strncmp(e->d_name, "virbr", 5) == 0)       continue;

		char type_path[256];
		snprintf(type_path, sizeof type_path,
		         "/sys/class/net/%s/type", e->d_name);
		char *type_str = read_file_all(type_path);
		if (!type_str)
			continue;
		int iftype = atoi(type_str);
		free(type_str);
		if (iftype != 1)
			continue;

		char addr_path[256];
		snprintf(addr_path, sizeof addr_path,
		         "/sys/class/net/%s/address", e->d_name);
		char *addr = read_file_all(addr_path);
		if (!addr)
			continue;
		trim_inplace(addr);

		int nonzero = 0;
		for (char *p = addr; *p; p++) {
			*p = (char)tolower((unsigned char)*p);
			if (*p != '0' && *p != ':')
				nonzero = 1;
		}
		if (!nonzero || strlen(addr) != 17) {
			free(addr);
			continue;
		}

		int dup = 0;
		for (int i = 0; i < count; i++) {
			if (strcmp(macs[i], addr) == 0) {
				dup = 1;
				break;
			}
		}
		if (!dup && count < MAC_CAP) {
			macs[count++] = addr;
		} else {
			free(addr);
		}
	}
	closedir(d);

	qsort(macs, count, sizeof(char *), mac_str_cmp);
	for (int i = 0; i < count; i++) {
		cJSON_AddItemToArray(arr, cJSON_CreateString(macs[i]));
		free(macs[i]);
	}
	return arr;
}

const char *cached_composite_id(const char *machine_id)
{
	static char hex_buf[65];
	static int  cached = 0;
	if (cached)
		return hex_buf;

	hex_buf[0] = '\0';
	cached = 1;

	cJSON *macs = collect_mac_addresses();
	EVP_MD_CTX *md = EVP_MD_CTX_new();
	if (!md) {
		cJSON_Delete(macs);
		fprintf(stderr, "[agent] composite_id: EVP_MD_CTX_new failed\n");
		return hex_buf;
	}

	int ok = (EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1);
	if (ok) {
		const char *mid = (machine_id && *machine_id) ? machine_id : "";
		ok = ok && (EVP_DigestUpdate(md, mid, strlen(mid)) == 1);
		ok = ok && (EVP_DigestUpdate(md, "\n", 1) == 1);
		int n = macs ? cJSON_GetArraySize(macs) : 0;
		for (int i = 0; ok && i < n; i++) {
			const cJSON *e = cJSON_GetArrayItem(macs, i);
			if (!cJSON_IsString(e)) continue;
			ok = (EVP_DigestUpdate(md, e->valuestring,
			                      strlen(e->valuestring)) == 1);
			if (ok && i + 1 < n)
				ok = (EVP_DigestUpdate(md, "\n", 1) == 1);
		}
	}

	unsigned char raw[EVP_MAX_MD_SIZE];
	unsigned int rawlen = 0;
	ok = ok && (EVP_DigestFinal_ex(md, raw, &rawlen) == 1);

	EVP_MD_CTX_free(md);
	cJSON_Delete(macs);

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

/* 첫 실행 시 UUIDv4 생성 -> state dir에 영구 저장 -> 재사용.
 * 저장 경로는 install이 잡아주는 WORKER_STATE_DIR(없으면 XDG_STATE_HOME/HOME
 * 폴백)을 재사용해 user-level/SysV 어느 설치 모델에서도 쓰기 가능하게 한다.
 * prep-image(image-prep.sh)가 이 파일을 지워 클론마다 새로 생성되게 한다. */
const char *cached_agent_id(void)
{
	static char id_buf[64];
	static int  cached = 0;
	if (cached)
		return id_buf;
	cached = 1;
	id_buf[0] = '\0';

	char dir[512];
	const char *sd = getenv("WORKER_STATE_DIR");
	if (sd && *sd) {
		snprintf(dir, sizeof dir, "%s", sd);
	} else {
		const char *xdg  = getenv("XDG_STATE_HOME");
		const char *home = getenv("HOME");
		if (xdg && *xdg)
			snprintf(dir, sizeof dir, "%s/assessment-agent", xdg);
		else if (home && *home)
			snprintf(dir, sizeof dir, "%s/.local/state/assessment-agent", home);
		else
			snprintf(dir, sizeof dir, "/var/lib/assessment-agent");
	}
	char path[600];
	snprintf(path, sizeof path, "%s/agent-id", dir);

	char *content = read_file_all(path);
	if (content) {
		trim_inplace(content);
		if (strlen(content) >= 32 && strlen(content) < sizeof id_buf) {
			snprintf(id_buf, sizeof id_buf, "%s", content);
			free(content);
			return id_buf;
		}
		free(content);
	}

	char uuid[64];
	uuid_v4(uuid, sizeof uuid);
	snprintf(id_buf, sizeof id_buf, "%s", uuid);

	/* best-effort 영구화: dir은 보통 install이 생성. 없으면 재귀 생성 시도. */
	for (char *p = dir + 1; *p; p++) {
		if (*p == '/') { *p = '\0'; mkdir(dir, 0700); *p = '/'; }
	}
	mkdir(dir, 0700);
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n", id_buf);
		fclose(f);
		chmod(path, 0600);
	}
	return id_buf;
}

static cJSON *collect_external_ip(void)
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

static int read_pid_comm(int pid, char *out, size_t out_len);

/* SysV 서비스의 pid를 pid 파일에서 읽는다. 관례상 /var/run/<name>.pid, 일부는
 * /var/run/<name>/<name>.pid. 죽은 pid 파일(프로세스 종료 후 잔존)은 /proc/<pid>
 * 부재로 걸러 -1. 데몬이 아닌 서비스(iptables/network 등)는 pid 파일이 없어 -1. */
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

/* systemd 없는 SysV 호스트(EL6 등): systemctl 부재로 collect_services()가 진입하지
 * 못하므로 여기로 폴백한다. RHEL init.d 스크립트가 start 시 touch 하는
 * /var/lock/subsys/<name> 로 실행 중 서비스를 열거하고, /var/run/<name>.pid 로 pid,
 * /proc/<pid>/comm 으로 exe 를 best-effort로 채운다. systemd 경로와 같은
 * {unit, sub, pid, exe} 스키마를 유지한다(pid 파일 없는 서비스는 pid/exe=null,
 * systemd의 MainPID 없는 unit과 동일). */
static cJSON *collect_services_sysv(void)
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

/* services[]에 pid(MainPID)/exe(comm)를 붙여 listen_ports[].pid와 조인 가능하게 한다.
 * per-unit `systemctl show` 반복은 fork 비용이 커서, 실행 중 unit 전체를 한 번의
 * `systemctl show -p Id,MainPID <units...>`로 배치 조회해 unit명으로 매칭한다. */
static cJSON *collect_services(void)
{
	char *out = run_cmd(
		"systemctl list-units --type=service --type=socket "
		"--state=running --state=listening "
		"--no-pager --plain --no-legend 2>/dev/null");
	if (!out)
		return collect_services_sysv();   /* systemd 부재(EL6 등) -> SysV 폴백 */

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

		/* pid/exe 배치 조회(systemctl show)는 .service unit에만 한다. socket unit은
		 * MainPID property가 없어(프로세스가 아니라 커널 소켓) show 가 exit!=0 을 내고,
		 * run_cmd 는 exit!=0 이면 유효한 stdout까지 폐기하므로 같은 배치에 섞인 service
		 * pid 까지 전부 유실된다. socket 은 pid=null 이 정확한 값이라 애초에 제외한다.
		 * unit명은 systemctl 규칙상 공백/쉘 메타문자가 없어 bare로 이어붙인다. */
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
			/* systemctl show는 --property 요청 순서를 지키지 않아(예: MainPID가
			 * Id보다 먼저) 나오고, strtok_r는 블록 구분 빈 줄을 건너뛴다. 그래서
			 * 순서에 의존하지 않고 Id/MainPID를 쌍으로 모아 둘 다 채워지면 매칭한다.
			 * 한 unit 블록은 Id 1개 + MainPID 1개를 정확히 낸다. */
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

struct sock_inode_owner {
	long inode;
	int  pid;
	char comm[64];
};

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

#define TCP_STATE_LISTEN_HEX "0A"

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

static cJSON *collect_listen_ports(void)
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
	add_common_metadata(root, "inventory", machine_id, agent_version);

	int ok = 1;
	add_os_release(root);
	if (!add_kernel_version(root))     ok = 0;
	if (!add_cpu_cores(root))          ok = 0;
	add_cpu_model(root);
	if (!add_mem_total_swap_total(root)) ok = 0;

	cJSON_AddItemToObject(root, "disks",       or_empty_array(collect_disks()));
	cJSON_AddItemToObject(root, "mounts",      or_empty_array(collect_mounts(0)));

	cJSON_AddItemToObject(root, "services",     or_json_null(collect_services()));
	cJSON_AddItemToObject(root, "listen_ports", or_empty_array(collect_listen_ports()));
	cJSON_AddItemToObject(root, "interfaces",    or_empty_array(collect_interfaces()));
	cJSON_AddItemToObject(root, "mac_addresses", or_empty_array(collect_mac_addresses()));

	cJSON_AddItemToObject(root, "ip_external", collect_external_ip());

	if (!ok) {
		cJSON_Delete(root);
		return NULL;
	}
	return root;
}

static int add_cpu_stat(cJSON *root)
{
	char *content = read_file_all("/proc/stat");
	if (!content)
		return 0;
	long v[8] = { 0 };
	int got = sscanf(content, "cpu  %ld %ld %ld %ld %ld %ld %ld %ld",
	                 &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
	if (got < 4) {
		free(content);
		return 0;
	}
	/* sscanf fills left-to-right: fields [0,got) are measured, the rest weren't
	 * present on this line — emit those as null, not a fabricated 0. Standard
	 * /proc/stat on kernel 2.6.32+ always yields all 8. */
	static const char *const keys[8] = {
		"user", "nice", "system", "idle", "iowait", "irq", "softirq", "steal"
	};
	cJSON *obj = cJSON_CreateObject();
	for (int k = 0; k < 8; k++) {
		if (k < got)
			cJSON_AddNumberToObject(obj, keys[k], (double)v[k]);
		else
			cJSON_AddNullToObject(obj, keys[k]);
	}
	cJSON_AddItemToObject(root, "cpu_stat", obj);

	/* per-core 이용률 원자료(cpu0..N) + procs_running/procs_blocked.
	 * 같은 /proc/stat 한 번 읽기로 공짜. per-core=단일스레드 병목 감지,
	 * procs_running=CPU 포화 주신호, procs_blocked=IO발 로드 분리(근본원인 핵심). */
	cJSON *cores = cJSON_CreateArray();
	long procs_running = -1, procs_blocked = -1;
	char *save = NULL;
	for (char *line = strtok_r(content, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "cpu", 3) == 0 && line[3] >= '0' && line[3] <= '9') {
			long c[8] = { 0 };
			int cn = sscanf(line, "cpu%*d %ld %ld %ld %ld %ld %ld %ld %ld",
			                &c[0], &c[1], &c[2], &c[3], &c[4], &c[5], &c[6], &c[7]);
			if (cn < 4)
				continue;
			cJSON *core = cJSON_CreateObject();
			for (int k = 0; k < 8; k++) {
				if (k < cn)
					cJSON_AddNumberToObject(core, keys[k], (double)c[k]);
				else
					cJSON_AddNullToObject(core, keys[k]);
			}
			cJSON_AddItemToArray(cores, core);
		} else if (strncmp(line, "procs_running ", 14) == 0) {
			procs_running = strtol(line + 14, NULL, 10);
		} else if (strncmp(line, "procs_blocked ", 14) == 0) {
			procs_blocked = strtol(line + 14, NULL, 10);
		}
	}
	free(content);

	cJSON_AddItemToObject(root, "cpu_per_core", cores);
	add_long_or_null(root, "procs_running", procs_running);
	add_long_or_null(root, "procs_blocked", procs_blocked);
	return 1;
}

static long zoneinfo_wmark_low_pages(void)
{
	char *content = read_file_all("/proc/zoneinfo");
	if (!content)
		return -1;
	long total = 0;
	int found = 0;
	const char *p = content;
	while (*p) {
		const char *eol = strchr(p, '\n');
		const char *s = p;
		while (*s == ' ' || *s == '\t')
			s++;
		if (strncmp(s, "low", 3) == 0 && (s[3] == ' ' || s[3] == '\t')) {
			const char *v = s + 3;
			while (*v == ' ' || *v == '\t')
				v++;
			char *end;
			long val = strtol(v, &end, 10);
			if (end != v) {
				total += val;
				found = 1;
			}
		}
		if (!eol) break;
		p = eol + 1;
	}
	free(content);
	return found ? total : -1;
}

static long derive_mem_available_kb(const char *meminfo, long mem_free)
{
	if (mem_free < 0)
		return -1;
	long active_file   = meminfo_get_kb(meminfo, "Active(file)");
	long inactive_file = meminfo_get_kb(meminfo, "Inactive(file)");
	long sreclaimable  = meminfo_get_kb(meminfo, "SReclaimable");
	if (active_file < 0 || inactive_file < 0 || sreclaimable < 0)
		return -1;

	long wmark_pages = zoneinfo_wmark_low_pages();
	if (wmark_pages < 0)
		return -1;
	long page_kb = sysconf(_SC_PAGESIZE) / 1024;
	if (page_kb <= 0)
		page_kb = 4;
	long wmark_low = wmark_pages * page_kb;

	long available = mem_free - wmark_low;

	long pagecache = active_file + inactive_file;
	pagecache -= (pagecache / 2 < wmark_low) ? pagecache / 2 : wmark_low;
	available += pagecache;

	available += sreclaimable -
	             ((sreclaimable / 2 < wmark_low) ? sreclaimable / 2 : wmark_low);

	if (available < 0)
		available = 0;
	return available;
}

static int add_meminfo_full(cJSON *root)
{
	char *content = read_file_all("/proc/meminfo");
	if (!content)
		return 0;
	long mem_total     = meminfo_get_kb(content, "MemTotal");
	long mem_free      = meminfo_get_kb(content, "MemFree");
	long mem_available = meminfo_get_kb(content, "MemAvailable");
	long mem_buffers   = meminfo_get_kb(content, "Buffers");
	long mem_cached    = meminfo_get_kb(content, "Cached");
	long swap_total    = meminfo_get_kb(content, "SwapTotal");
	long swap_free     = meminfo_get_kb(content, "SwapFree");

	/* MemAvailable is kernel 3.14+. On older kernels (EL6 2.6.32, EL7 3.10)
	 * reproduce the kernel's si_mem_available() from zoneinfo watermarks. If
	 * even that can't be computed, leave it unmeasured (null) — do not fall
	 * back to a free+buffers+cached guess, which overstates reclaimability. */
	if (mem_available < 0)
		mem_available = derive_mem_available_kb(content, mem_free);

	free(content);

	if (mem_total < 0)
		return 0;

	cJSON_AddNumberToObject(root, "mem_total_kb", (double)mem_total);
	add_long_or_null(root, "mem_free_kb",      mem_free);
	add_long_or_null(root, "mem_buffers_kb",   mem_buffers);
	add_long_or_null(root, "mem_cached_kb",    mem_cached);
	add_long_or_null(root, "swap_total_kb",    swap_total);
	add_long_or_null(root, "swap_free_kb",     swap_free);
	add_long_or_null(root, "mem_available_kb", mem_available);
	return 1;
}

static int add_loadavg(cJSON *root)
{
	char *content = read_file_all("/proc/loadavg");
	if (!content)
		return 0;
	double l1 = 0, l5 = 0, l15 = 0;
	int got = sscanf(content, "%lf %lf %lf", &l1, &l5, &l15);
	free(content);
	if (got != 3)
		return 0;
	cJSON_AddNumberToObject(root, "load_1m",  l1);
	cJSON_AddNumberToObject(root, "load_5m",  l5);
	cJSON_AddNumberToObject(root, "load_15m", l15);
	return 1;
}

/* /proc/vmstat 의 스왑 발생/OOM 카운터. pswpin/pswpout 은 메모리 포화 주신호이자
 * 메모리발 디스크 I/O 를 구분하는 근본원인 판별 신호. oom_kill 은 4.13+ 만 존재.
 * 실패해도 metrics 전체를 실패시키지 않는다(선택 신호 -> null). */
static void add_vmstat(cJSON *root)
{
	long pswpin = -1, pswpout = -1, oom_kill = -1;
	char *content = read_file_all("/proc/vmstat");
	if (content) {
		char *save = NULL;
		for (char *line = strtok_r(content, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			if (strncmp(line, "pswpin ", 7) == 0)
				pswpin = strtol(line + 7, NULL, 10);
			else if (strncmp(line, "pswpout ", 8) == 0)
				pswpout = strtol(line + 8, NULL, 10);
			else if (strncmp(line, "oom_kill ", 9) == 0)
				oom_kill = strtol(line + 9, NULL, 10);
		}
		free(content);
	}
	/* pgmajfault 는 요청서 지침대로 발행하지 않는다 — 파일 mmap major fault 가 섞여
	 * 대용량 파일 호스트(DB 등)를 메모리 압박으로 오판한다. */
	add_long_or_null(root, "pswpin",   pswpin);
	add_long_or_null(root, "pswpout",  pswpout);
	add_long_or_null(root, "oom_kill", oom_kill);
}

static cJSON *collect_disk_io(void)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;
	char *content = read_file_all("/proc/diskstats");
	if (!content)
		return arr;

	char *save = NULL;
	for (char *line = strtok_r(content, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		long major = 0, minor = 0;
		char dev[64] = { 0 };
		long reads_completed = 0, reads_merged = 0;
		long sectors_read = 0, time_reading = 0;
		long writes_completed = 0, writes_merged = 0;
		long sectors_written = 0, time_writing = 0;
		long ios_in_progress = 0, time_io = 0, weighted_time = 0;

		int n = sscanf(line, "%ld %ld %63s %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld",
		               &major, &minor, dev,
		               &reads_completed, &reads_merged, &sectors_read, &time_reading,
		               &writes_completed, &writes_merged, &sectors_written, &time_writing,
		               &ios_in_progress, &time_io, &weighted_time);
		if (n < 7)
			continue;
		if (is_excluded_block_dev(dev))
			continue;

		char path[256];
		snprintf(path, sizeof path, "/sys/block/%s", dev);
		if (access(path, F_OK) != 0)
			continue;

		/* n>=7 guarantees the read fields (cols 4,6). The write fields live at
		 * cols 8 and 10 — emit null if this line was shorter than that rather
		 * than a fabricated 0. Standard /proc/diskstats on 2.6.32+ has 14 cols. */
		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "device", dev);
		add_major_minor(item, (int)major, (int)minor);
		cJSON_AddNumberToObject(item, "reads_completed",  (double)reads_completed);
		add_long_or_null(item, "writes_completed", n >= 8  ? writes_completed : -1);
		cJSON_AddNumberToObject(item, "sectors_read",     (double)sectors_read);
		add_long_or_null(item, "sectors_written",  n >= 10 ? sectors_written : -1);
		/* await 원자료(엔진이 델타/완료수로 응답지연 산출) + %util·avgqu 참고값.
		 * diskstats 필드: 7=time_reading(ms) 11=time_writing(ms) 13=io_ticks(ms)
		 * 14=weighted(ms). n>=7 은 위 continue 로 보장, 그 외는 짧은 라인이면 null. */
		cJSON_AddNumberToObject(item, "time_reading_ms", (double)time_reading);
		add_long_or_null(item, "time_writing_ms", n >= 11 ? time_writing : -1);
		add_long_or_null(item, "io_ticks_ms",     n >= 13 ? time_io : -1);
		add_long_or_null(item, "weighted_io_ms",  n >= 14 ? weighted_time : -1);
		cJSON_AddStringToObject(item, "kind", disk_kind(dev));
		cJSON_AddItemToArray(arr, item);
	}
	free(content);
	return arr;
}

static cJSON *collect_net_io(void)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;
	char *content = read_file_all("/proc/net/dev");
	if (!content)
		return arr;

	int line_no = 0;
	char *save = NULL;
	for (char *line = strtok_r(content, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		line_no++;
		if (line_no <= 2)
			continue;

		char *colon = strchr(line, ':');
		if (!colon)
			continue;
		*colon = '\0';
		char *iface = line;
		while (*iface == ' ' || *iface == '\t')
			iface++;
		if (strcmp(iface, "lo") == 0)
			continue;

		long rx_bytes = 0, rx_packets = 0, rx_errors = 0, rx_drop = 0;
		long tx_bytes = 0, tx_packets = 0, tx_errors = 0, tx_drop = 0;

		/* /proc/net/dev — rx: bytes packets errs drop [fifo frame compressed
		 * multicast], tx: bytes packets errs drop [...]. drop 은 포화 신호라
		 * 이제 버리지 않고 파싱한다(errs 는 이미 발행 중). */
		int n = sscanf(colon + 1,
		               "%ld %ld %ld %ld %*d %*d %*d %*d "
		               "%ld %ld %ld %ld",
		               &rx_bytes, &rx_packets, &rx_errors, &rx_drop,
		               &tx_bytes, &tx_packets, &tx_errors, &tx_drop);
		if (n < 8)
			continue;

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "interface", iface);
		cJSON_AddNumberToObject(item, "rx_bytes",   (double)rx_bytes);
		cJSON_AddNumberToObject(item, "tx_bytes",   (double)tx_bytes);
		cJSON_AddNumberToObject(item, "rx_packets", (double)rx_packets);
		cJSON_AddNumberToObject(item, "tx_packets", (double)tx_packets);
		cJSON_AddNumberToObject(item, "rx_errors",  (double)rx_errors);
		cJSON_AddNumberToObject(item, "tx_errors",  (double)tx_errors);
		cJSON_AddNumberToObject(item, "rx_drops",   (double)rx_drop);
		cJSON_AddNumberToObject(item, "tx_drops",   (double)tx_drop);
		cJSON_AddStringToObject(item, "kind", net_kind(iface));
		cJSON_AddItemToArray(arr, item);
	}
	free(content);
	return arr;
}

/* /proc/net/snmp 의 Tcp: RetransSegs — 헤더행에서 인덱스 찾아 값행 같은 위치.
 * 커널/버전마다 컬럼 수가 달라 고정 오프셋 대신 헤더 매칭. 없으면 -1. */
static long snmp_tcp_retranssegs(void)
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

/* 네트워크 품질 신규 신호: TCP 재전송(재전송 품질) · TIME_WAIT 적체 · conntrack
 * 사용률(연결 고갈). 전부 raw 카운터(엔진이 비율 계산). conntrack 모듈 미로드면
 * 파일 부재 -> null(skip). */
static void add_net_quality(cJSON *root)
{
	add_long_or_null(root, "tcp_retrans_segs", snmp_tcp_retranssegs());

	long tcp_tw = -1;
	char *ss = read_file_all("/proc/net/sockstat");
	if (ss) {
		char *p = strstr(ss, "TCP: ");
		if (p) {
			char *tw = strstr(p, "tw ");
			if (tw)
				tcp_tw = strtol(tw + 3, NULL, 10);
		}
		free(ss);
	}
	add_long_or_null(root, "tcp_tw", tcp_tw);

	add_proc_long_file(root, "conntrack_count",
	                   "/proc/sys/net/netfilter/nf_conntrack_count");
	add_proc_long_file(root, "conntrack_max",
	                   "/proc/sys/net/netfilter/nf_conntrack_max");
}

/* /proc/schedstat 의 runqueue 대기시간 누적(ns) 합 — 실행 대기 적분값이라
 * procs_running 스냅샷보다 표본 사이 스파이크를 안 놓친다. CONFIG_SCHEDSTATS
 * 없으면 파일 부재 -> null. cpuN 행 8번째 필드(0-idx 7)=runqueue wait time. */
static void add_schedstat(cJSON *root)
{
	long total_wait = -1;
	char *content = read_file_all("/proc/schedstat");
	if (content) {
		long acc = 0;
		int found = 0;
		char *save = NULL;
		for (char *line = strtok_r(content, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			if (strncmp(line, "cpu", 3) == 0 && line[3] >= '0' && line[3] <= '9') {
				unsigned long f[9] = { 0 };
				int cn = sscanf(line,
				                "cpu%*d %lu %lu %lu %lu %lu %lu %lu %lu %lu",
				                &f[0], &f[1], &f[2], &f[3], &f[4],
				                &f[5], &f[6], &f[7], &f[8]);
				if (cn >= 8) {
					acc += (long)f[7];
					found = 1;
				}
			}
		}
		free(content);
		if (found)
			total_wait = acc;
	}
	add_long_or_null(root, "schedstat_run_wait_ns", total_wait);
}

/* PSI (4.20+) — 관측·검증용(분류 미사용, 요청서). some total(us 누적)만 raw 발행.
 * 커널에 없으면 파일 부재 -> null. */
static void add_psi(cJSON *root)
{
	static const struct {
		const char *path;
		const char *key;
	} psi[] = {
		{ "/proc/pressure/cpu",    "psi_cpu_some_total" },
		{ "/proc/pressure/memory", "psi_mem_some_total" },
		{ "/proc/pressure/io",     "psi_io_some_total" },
	};
	for (size_t i = 0; i < sizeof psi / sizeof psi[0]; i++) {
		long total = -1;
		char *c = read_file_all(psi[i].path);
		if (c) {
			char *t = strstr(c, "some");
			if (t) {
				char *tt = strstr(t, "total=");
				if (tt)
					total = strtol(tt + 6, NULL, 10);
			}
			free(c);
		}
		add_long_or_null(root, psi[i].key, total);
	}
}

/* 설정된 수집 주기(초). 엔진 sample_sufficiency 하드코딩(1440/day) 대체 기준값.
 * main.c 루프와 동일 env(AGENT_INTERVAL_SEC, 기본 60)를 읽는다. */
static int agent_interval_sec(void)
{
	int n = getenv_int_or("AGENT_INTERVAL_SEC", 60);
	return (n > 0 && n < 86400) ? n : 60;
}

cJSON *collect_metrics_payload(const char *machine_id, const char *agent_version)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;
	add_common_metadata(root, "metrics", machine_id, agent_version);
	cJSON_AddNumberToObject(root, "collection_interval_sec", agent_interval_sec());

	int ok = 1;
	if (!add_cpu_stat(root))     ok = 0;
	if (!add_meminfo_full(root)) ok = 0;
	if (!add_loadavg(root))      ok = 0;
	add_vmstat(root);      /* 스왑/OOM (근본원인) — 실패해도 null 로 싣고 계속 */
	add_net_quality(root); /* TCP 재전송/TIME_WAIT/conntrack — 네트워크 품질 */
	add_schedstat(root);   /* runqueue 대기 누적(적분값) */
	add_psi(root);         /* PSI some total — 관측용(분류 미사용) */

	cJSON_AddItemToObject(root, "disk_io", or_empty_array(collect_disk_io()));
	cJSON_AddItemToObject(root, "mounts",  or_empty_array(collect_mounts(1)));
	cJSON_AddItemToObject(root, "net_io",  or_empty_array(collect_net_io()));

	if (!ok) {
		cJSON_Delete(root);
		return NULL;
	}
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
	add_common_metadata(root, "error", machine_id, agent_version);

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
