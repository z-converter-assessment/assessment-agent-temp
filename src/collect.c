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

/* --- v2 datapoint-array 빌더 --- system.<ns> -> metric{type,unit,points:[{attr,value}]}.
 * 에이전트는 raw 누적/순간값만 싣고 delta/rate/util 은 엔진. base 단위(s/By/1/count). */
static cJSON *ns_get(cJSON *root, const char *ns)
{
	cJSON *o = cJSON_GetObjectItemCaseSensitive(root, ns);
	if (!o) { o = cJSON_CreateObject(); cJSON_AddItemToObject(root, ns, o); }
	return o;
}
static cJSON *metric_new(cJSON *ns, const char *name, const char *type, const char *unit)
{
	cJSON *m = cJSON_CreateObject();
	cJSON_AddStringToObject(m, "type", type);
	cJSON_AddStringToObject(m, "unit", unit);
	cJSON_AddItemToObject(m, "points", cJSON_CreateArray());
	cJSON_AddItemToObject(ns, name, m);
	return m;
}
static cJSON *point_new(cJSON *metric)
{
	cJSON *p = cJSON_CreateObject();
	cJSON_AddItemToObject(p, "attr", cJSON_CreateObject());
	cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(metric, "points"), p);
	return p;
}
static void point_attr(cJSON *point, const char *k, const char *v)
{
	if (v && *v)
		cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(point, "attr"), k, v);
}
static void point_value(cJSON *point, double v) { cJSON_AddNumberToObject(point, "value", v); }
static void point_value_null(cJSON *point) { cJSON_AddNullToObject(point, "value"); }
/* 단일 무-attr scalar point 발행 헬퍼(gauge/counter 공통). */
static void metric_scalar(cJSON *ns, const char *name, const char *type,
                          const char *unit, int have, double v)
{
	cJSON *m = metric_new(ns, name, type, unit);
	cJSON *p = point_new(m);
	if (have) point_value(p, v); else point_value_null(p);
}

static void add_common_metadata(cJSON *obj,
                                const char *message_type,
                                const char *machine_id,
                                const char *agent_version)
{
	cJSON_AddStringToObject(obj, "schema_version", "2.0");
	cJSON_AddStringToObject(obj, "message_type", message_type);
	/* machine_id 없으면 null(측정 불가) — 식별·라우팅은 agent_id, 유니크는 mac 기반 composite_id. */
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

/* Keep a legitimately-unmeasurable field (e.g. services) present as null instead
 * of dropping the key; cJSON silently ignores a NULL item add. */
static cJSON *or_json_null(cJSON *v)
{
	return v ? v : cJSON_CreateNull();
}

/* -1 센티넬(측정불가)이면 null. 값=실측/null=측정불가 계약을 한 곳에서 강제. */
static void add_long_or_null(cJSON *root, const char *key, long val)
{
	if (val < 0) cJSON_AddNullToObject(root, key);
	else         cJSON_AddNumberToObject(root, key, (double)val);
}

/* /proc 파일의 첫 정수 발행. 파일 부재(conntrack 등 조건부 카운터)면 null. */
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
	/* schema 가 major/minor 를 required(int_or_null)로 강제 — 실패(-1)는 키 생략 말고 명시적 null. */
	if (major >= 0) cJSON_AddNumberToObject(obj, "major", (double)major);
	else            cJSON_AddNullToObject(obj, "major");
	if (minor >= 0) cJSON_AddNumberToObject(obj, "minor", (double)minor);
	else            cJSON_AddNullToObject(obj, "minor");
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

/* 클라우드 IMDS metadata 페치(AWS 는 IMDSv2 토큰 선취득). vendor 아님/실패면 NULL,
 * 200+빈 응답이면 빈 문자열(호출자가 null/빈배열 구분). */
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

		/* size/type absent or malformed -> null, not a fabricated 0/"disk"
		 * (engine filters on kind below, not raw lsblk type). */
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
		/* disk_kind 는 sysfs 를 kernel 이름으로 probe -> KNAME(dm-0) 사용. NAME(vg0-lv0)이면
		 * sysfs miss 로 오분류. disk_io[]와 같은 키로 크로스메시지 kind 드리프트 방지(name 발행은 NAME). */
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

		/* /device 심링크로 거르지 않는다 — dm-N/mdN 은 device 가 없어, 누락되면 disk_io[] 와
		 * device 셋이 어긋난다. kind 는 disk_kind()가 sysfs 신호로 판별. */
		char path[512];
		snprintf(path, sizeof path, "/sys/block/%s/size", e->d_name);
		char *content = read_file_all(path);
		long sectors = -1;
		if (content) {
			sectors = strtol(content, NULL, 10);
			free(content);
		}

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
		/* size 못 읽으면(-1) null. sectors==0 은 유효한 실측 0(비활성 dm)이라 그대로 싣는다. */
		if (sectors >= 0)
			cJSON_AddNumberToObject(item, "size_bytes", (double)sectors * 512.0);
		else
			cJSON_AddNullToObject(item, "size_bytes");
		/* type 은 lsblk 의 실측 필드다 — sysfs 폴백은 알 수 없으므로 대체값 대신 null. */
		cJSON_AddNullToObject(item, "type");
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

/* /proc/filesystems 의 leading "nodev" = backing block device 없는 fs (kernel's own
 * authority). Catches pseudo mounts the static skip list misses, on any kernel,
 * without a growing denylist. Result cached. */
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

/* nodev fs that still hold real data and must NOT be dropped as pseudo: network
 * filesystems and FUSE. fuseblk (real block device) is not nodev, never reaches here. */
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

/* Split by role: with_usage=0 (inventory) -> structure incl. major/minor;
 * with_usage=1 (metrics) -> usage (free/avail). total_bytes in both (% calc). */
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
			/* inode 개념 없는 fs(vfat 등)는 f_files=0 -> 실측 0 과 구분되게 null. */
			if (st.f_files > 0) {
				cJSON_AddNumberToObject(item, "inodes_total", (double)st.f_files);
				cJSON_AddNumberToObject(item, "inodes_free",  (double)st.f_ffree);
			} else {
				cJSON_AddNullToObject(item, "inodes_total");
				cJSON_AddNullToObject(item, "inodes_free");
			}
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

/* /proc/net/route default route(dest=0,mask=0) gateway 를 iface->IP 맵으로. 서브넷 disambiguation 용, IPv4 만. */
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

/* 첫 실행 시 UUIDv4 생성 -> WORKER_STATE_DIR(폴백 XDG_STATE_HOME/HOME)에 영구 저장 -> 재사용.
 * image-prep.sh 가 이 파일을 지워 클론마다 재생성. */
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
	/* agent_id 불변 계약: 절단 파일이 남으면 UUID 재발급 -> temp+fsync+rename 원자적 쓰기. */
	char tmp[640];
	snprintf(tmp, sizeof tmp, "%s/agent-id.tmp.%ld", dir, (long)getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd >= 0) {
		char line[80];
		int n = snprintf(line, sizeof line, "%s\n", id_buf);
		if (n > 0 && (size_t)n < sizeof line &&
		    write(fd, line, (size_t)n) == (ssize_t)n && fsync(fd) == 0) {
			close(fd);
			if (rename(tmp, path) != 0) unlink(tmp);
		} else {
			close(fd);
			unlink(tmp);
		}
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

/* services[]에 pid/exe 를 붙여 listen_ports[].pid 와 조인. per-unit show 는 fork 비용 커서
 * 실행 unit 전체를 한 번의 `systemctl show -p Id,MainPID`로 배치 조회. */
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

	/* mem_total_bytes (base 단위). swap 은 block_devices type=swap 노드로(P4). */
	{
		char *mi = read_file_all("/proc/meminfo");
		long kb = mi ? meminfo_get_kb(mi, "MemTotal") : -1;
		if (mi) free(mi);
		if (kb >= 0) cJSON_AddNumberToObject(root, "mem_total_bytes", (double)kb * 1024.0);
		else         cJSON_AddNullToObject(root, "mem_total_bytes");
	}

	cJSON_AddItemToObject(root, "services",     or_json_null(collect_services()));
	cJSON_AddItemToObject(root, "listen_ports", or_empty_array(collect_listen_ports()));
	cJSON_AddItemToObject(root, "ip_external",  collect_external_ip());

	/* v2 정규화 노드: 실내용은 P4(block_devices/net_interfaces). P1 은 required 배열 스텁. */
	cJSON_AddItemToObject(root, "block_devices",  cJSON_CreateArray());
	cJSON_AddItemToObject(root, "net_interfaces", cJSON_CreateArray());

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
	/* sscanf fills left-to-right: [0,got) measured, rest null (not a fabricated 0).
	 * /proc/stat on 2.6.32+ always yields all 8. */
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

	/* per-core(cpu0..N) + procs_running(CPU 포화)/procs_blocked(IO 로드 분리).
	 * 같은 /proc/stat 한 번 읽기로 공짜. */
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

	/* MemAvailable is kernel 3.14+. On older kernels reproduce si_mem_available()
	 * from zoneinfo watermarks; if that fails, null — no free+buffers+cached
	 * guess (it overstates reclaimability). */
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

/* /proc/vmstat 스왑/OOM 카운터. oom_kill 은 4.13+ 만 존재.
 * 실패해도 metrics 실패시키지 않는다(선택 신호 -> null). */
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
	/* pgmajfault 는 미발행 — 파일 mmap major fault 가 섞여 대용량 파일 호스트를 메모리 압박으로 오판. */
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

		/* n>=7 guarantees read fields (cols 4,6); write fields (cols 8,10) ->
		 * null if the line was shorter (not a fabricated 0). 2.6.32+ has 14 cols. */
		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "device", dev);
		add_major_minor(item, (int)major, (int)minor);
		cJSON_AddNumberToObject(item, "reads_completed",  (double)reads_completed);
		add_long_or_null(item, "writes_completed", n >= 8  ? writes_completed : -1);
		cJSON_AddNumberToObject(item, "sectors_read",     (double)sectors_read);
		add_long_or_null(item, "sectors_written",  n >= 10 ? sectors_written : -1);
		/* diskstats 필드: 7=time_reading 11=time_writing 13=io_ticks 14=weighted(ms).
		 * n>=7 은 위 continue 로 보장, 그 외는 짧은 라인이면 null. */
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

		/* /proc/net/dev rx/tx 각각: bytes packets errs drop [+중간 필드 skip].
		 * drop 은 포화 신호라 파싱한다. */
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

/* /proc/net/snmp Tcp: RetransSegs — 컬럼 수가 커널마다 달라 헤더행 인덱스로 값행 매칭. 없으면 -1. */
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

/* 네트워크 품질: TCP 재전송/TIME_WAIT 적체/conntrack 사용률. raw 카운터.
 * conntrack 모듈 미로드면 파일 부재 -> null. */
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

/* /proc/schedstat runqueue 대기 누적(ns) 합 — 적분값이라 procs_running 스냅샷보다
 * 표본 사이 스파이크를 안 놓친다. cpuN 행 8번째(0-idx 7)=wait time. 없으면 null. */
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

/* PSI (4.20+) — some total(us 누적)만 raw 발행, 관측용. 없으면 null. */
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

/* main.c 가 실제 sleep 하는 수집 주기(초)를 그대로 보고. interval>0 은 raw 값 그대로
 * (상한 clamp 금지 — 접으면 엔진 표본 기준이 오염). interval<=0(one-shot)은 엔진의
 * expected=86400/interval div-by-zero 회피로 기본 60 으로 보고. */
static int agent_interval_sec(void)
{
	int n = getenv_int_or("AGENT_INTERVAL_SEC", 60);
	return n > 0 ? n : 60;
}

/* sysfs 단일값 읽어 trim. 성공 1. */
static int read_sysfs_str(const char *path, char *out, size_t outsz)
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
static void disk_device_id(const char *dev, char *out, size_t outsz)
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

/* v2 system.disk: /proc/diskstats raw counters(base 단위). throughput+io_time(%util)+operation_time(await)+queue.
 * diskstats(name 이후): f1 reads f2 rd_merge f3 sectors_rd f4 ms_rd | f5 writes f6 wr_merge f7 sectors_wr f8 ms_wr | f9 in_flight f10 io_ticks f11 weighted. */
static void collect_v2_system_disk(cJSON *root)
{
	cJSON *ns    = ns_get(root, "system.disk");
	cJSON *m_io  = metric_new(ns, "disk.io",             "counter", "By");
	cJSON *m_ops = metric_new(ns, "disk.operations",     "counter", "operations");
	cJSON *m_iot = metric_new(ns, "disk.io_time",        "counter", "s");
	cJSON *m_opt = metric_new(ns, "disk.operation_time", "counter", "s");
	cJSON *m_pnd = metric_new(ns, "disk.pending_operations", "gauge", "operations");

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
		p = point_new(m_io);  point_attr(p, "device", id); point_attr(p, "direction", "read");  point_value(p, (double)sr * 512.0);
		if (n >= 10) { p = point_new(m_io);  point_attr(p, "device", id); point_attr(p, "direction", "write"); point_value(p, (double)sw * 512.0); }
		p = point_new(m_ops); point_attr(p, "device", id); point_attr(p, "direction", "read");  point_value(p, (double)rc);
		if (n >= 8)  { p = point_new(m_ops); point_attr(p, "device", id); point_attr(p, "direction", "write"); point_value(p, (double)wc); }
		p = point_new(m_iot); point_attr(p, "device", id);
		if (n >= 13) point_value(p, (double)ticks / 1000.0); else point_value_null(p);
		p = point_new(m_opt); point_attr(p, "device", id); point_attr(p, "direction", "read");  point_value(p, (double)tr / 1000.0);
		if (n >= 11) { p = point_new(m_opt); point_attr(p, "device", id); point_attr(p, "direction", "write"); point_value(p, (double)tw / 1000.0); }
		p = point_new(m_pnd); point_attr(p, "device", id);
		if (n >= 12) point_value(p, (double)inflight); else point_value_null(p);
	}
	free(content);
}

cJSON *collect_metrics_payload(const char *machine_id, const char *agent_version)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;
	add_common_metadata(root, "metrics", machine_id, agent_version);
	cJSON_AddNumberToObject(root, "collection_interval_sec", agent_interval_sec());

	/* P1: system.disk 채움. cpu/memory/network 는 후속 페이즈(스키마상 null 허용, 키만 필수). */
	cJSON_AddNullToObject(root, "system.cpu");
	cJSON_AddNullToObject(root, "system.memory");
	cJSON_AddNullToObject(root, "system.network");
	collect_v2_system_disk(root);

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
