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

const char *cached_boot_time_iso(void)
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

const char *cached_agent_started_at_iso(void)
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
cJSON *wire_ns(cJSON *root, const char *ns)
{
	cJSON *o = cJSON_GetObjectItemCaseSensitive(root, ns);
	if (!o) { o = cJSON_CreateObject(); cJSON_AddItemToObject(root, ns, o); }
	return o;
}

cJSON *wire_metric(cJSON *ns, const char *name, const char *type, const char *unit)
{
	cJSON *m = cJSON_CreateObject();
	cJSON_AddStringToObject(m, "type", type);
	cJSON_AddStringToObject(m, "unit", unit);
	cJSON_AddItemToObject(m, "points", cJSON_CreateArray());
	cJSON_AddItemToObject(ns, name, m);
	return m;
}

cJSON *wire_point(cJSON *metric)
{
	cJSON *p = cJSON_CreateObject();
	cJSON_AddItemToObject(p, "attr", cJSON_CreateObject());
	cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(metric, "points"), p);
	return p;
}

void wire_point_attr(cJSON *point, const char *k, const char *v)
{
	if (v && *v)
		cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(point, "attr"), k, v);
}

void wire_point_value(cJSON *point, double v) { cJSON_AddNumberToObject(point, "value", v); }

void wire_point_null(cJSON *point) { cJSON_AddNullToObject(point, "value"); }


/* device(+direction) 데이터포인트 한 줄 발행 — 가장 흔한 패턴의 헬퍼. direction 이 NULL 이면 device 만. */
void wire_point_dev_dir(cJSON *metric, const char *device, const char *direction, double value)
{
	cJSON *p = wire_point(metric);
	wire_point_attr(p, "device", device);
	if (direction) wire_point_attr(p, "direction", direction);
	wire_point_value(p, value);
}
/* 단일 무-attr scalar point 발행 헬퍼(gauge/counter 공통). */
void wire_metric_scalar(cJSON *ns, const char *name, const char *type,
                          const char *unit, int have, double v)
{
	cJSON *m = wire_metric(ns, name, type, unit);
	cJSON *p = wire_point(m);
	if (have) wire_point_value(p, v); else wire_point_null(p);
}

void wire_add_envelope(cJSON *obj,
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

cJSON *wire_or_empty_array(cJSON *arr)
{
	return arr ? arr : cJSON_CreateArray();
}

/* Keep a legitimately-unmeasurable field (e.g. services) present as null instead
 * of dropping the key; cJSON silently ignores a NULL item add. */
cJSON *wire_or_null(cJSON *v)
{
	return v ? v : cJSON_CreateNull();
}

int is_machine_id(const char *s)
{
	size_t n = 0;
	for (const char *p = s; *p; p++) {
		if (!isxdigit((unsigned char)*p))
			return 0;
		n++;
	}
	return n == 32;
}

char *try_machine_id_file(const char *path)
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

char *try_dbus_uuidgen(void)
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

const char *detect_cloud_vendor(void)
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
char *fetch_cloud_metadata(const char *aws_path, const char *azure_path,
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

char *try_cloud_instance_id(void)
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

int mac_str_cmp(const void *a, const void *b)
{
	return strcmp(*(const char * const *)a, *(const char * const *)b);
}

cJSON *collect_mac_addresses(void)
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
