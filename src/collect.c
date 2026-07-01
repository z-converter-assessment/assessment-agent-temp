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
	cJSON_AddStringToObject(obj, "machine_id",
	                        machine_id && *machine_id ? machine_id : "");
	cJSON_AddStringToObject(obj, "composite_id", cached_composite_id(machine_id));
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

static void add_kb_or_null(cJSON *root, const char *key, long val)
{
	if (val < 0) cJSON_AddNullToObject(root, key);
	else         cJSON_AddNumberToObject(root, key, (double)val);
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

static char *try_cloud_instance_id(void)
{
	const char *vendor = detect_cloud_vendor();
	if (!vendor)
		return NULL;

	char *out = NULL;
	if (strcmp(vendor, "aws") == 0) {

		char *token = run_cmd(
			"curl -fsS -m 1 -X PUT "
			"-H 'X-aws-ec2-metadata-token-ttl-seconds: 60' "
			"http://169.254.169.254/latest/api/token 2>/dev/null");
		if (token && *token) {
			trim_inplace(token);
			char cmd[512];
			snprintf(cmd, sizeof cmd,
			         "curl -fsS -m 1 -H 'X-aws-ec2-metadata-token: %s' "
			         "http://169.254.169.254/latest/meta-data/instance-id 2>/dev/null",
			         token);
			out = run_cmd(cmd);
		}
		free(token);
	} else if (strcmp(vendor, "azure") == 0) {
		out = run_cmd(
			"curl -fsS -m 1 -H 'Metadata: true' "
			"'http://169.254.169.254/metadata/instance/compute/vmId"
			"?api-version=2021-02-01&format=text' 2>/dev/null");
	} else if (strcmp(vendor, "gcp") == 0) {
		out = run_cmd(
			"curl -fsS -m 1 -H 'Metadata-Flavor: Google' "
			"http://metadata.google.internal/computeMetadata/v1/instance/id 2>/dev/null");
	}

	if (out)
		trim_inplace(out);
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

static int add_os_release(cJSON *root)
{
	char *content = read_file_all("/etc/os-release");
	if (!content) {

		if (add_redhat_release_fallback(root))
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
	add_kb_or_null(root, "swap_total_kb", swap_total);
	return 1;
}

static cJSON *collect_disks_via_lsblk(void)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;

	char *out = run_cmd("lsblk -dn -b -e 7,11 -o NAME,MAJ:MIN,SIZE,TYPE -J 2>/dev/null");
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

		double size_bytes = 0;
		if (cJSON_IsNumber(size))
			size_bytes = size->valuedouble;
		else if (cJSON_IsString(size))
			size_bytes = strtod(size->valuestring, NULL);
		cJSON_AddNumberToObject(item, "size_bytes", size_bytes);

		cJSON_AddStringToObject(item, "type",
		                        cJSON_IsString(type) ? type->valuestring : "disk");
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
		if (is_excluded_fstype(fst)) {
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

static cJSON *collect_mounts_inventory(void)
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
		double freeb = (double)st.f_bfree  * (double)st.f_frsize;
		double avail = (double)st.f_bavail * (double)st.f_frsize;

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "mount", mounts[i].mount);
		add_major_minor(item, mounts[i].major, mounts[i].minor);
		cJSON_AddNumberToObject(item, "total_bytes", total);
		cJSON_AddNumberToObject(item, "free_bytes", freeb);
		cJSON_AddNumberToObject(item, "avail_bytes", avail);
		cJSON_AddStringToObject(item, "fstype", mounts[i].fstype);
		cJSON_AddItemToArray(arr, item);
	}
	free_mount_entries(mounts, n);
	return arr;
}

static cJSON *collect_mounts_metrics(void)
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
		double freeb = (double)st.f_bfree  * (double)st.f_frsize;
		double avail = (double)st.f_bavail * (double)st.f_frsize;

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "mount", mounts[i].mount);
		add_major_minor(item, mounts[i].major, mounts[i].minor);
		cJSON_AddNumberToObject(item, "total_bytes", total);
		cJSON_AddNumberToObject(item, "free_bytes", freeb);
		cJSON_AddNumberToObject(item, "avail_bytes", avail);
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

static cJSON *collect_internal_ips(void)
{
	cJSON *arr = cJSON_CreateArray();
	struct ifaddrs *ifap = NULL;
	if (getifaddrs(&ifap) != 0)
		return arr;

	for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr)
			continue;
		if (ifa->ifa_flags & IFF_LOOPBACK)
			continue;
		if (ifa->ifa_addr->sa_family != AF_INET)
			continue;
		char ip[INET_ADDRSTRLEN];
		struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
		if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip))
			continue;

		int prefix = 0;
		if (ifa->ifa_netmask && ifa->ifa_netmask->sa_family == AF_INET) {
			struct sockaddr_in *mask = (struct sockaddr_in *)ifa->ifa_netmask;
			prefix = ipv4_netmask_prefix(mask->sin_addr.s_addr);
		}
		char cidr[INET_ADDRSTRLEN + 4];
		snprintf(cidr, sizeof cidr, "%s/%d", ip, prefix);
		cJSON_AddItemToArray(arr, cJSON_CreateString(cidr));
	}
	freeifaddrs(ifap);
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

	const char *vendor = detect_cloud_vendor();
	if (!vendor)
		return cJSON_CreateNull();

	char *out = NULL;
	if (strcmp(vendor, "aws") == 0) {
		char *token = run_cmd(
			"curl -fsS -m 1 -X PUT "
			"-H 'X-aws-ec2-metadata-token-ttl-seconds: 60' "
			"http://169.254.169.254/latest/api/token 2>/dev/null");
		if (token && *token) {
			trim_inplace(token);
			char cmd[512];
			snprintf(cmd, sizeof cmd,
			         "curl -fsS -m 1 -H 'X-aws-ec2-metadata-token: %s' "
			         "http://169.254.169.254/latest/meta-data/public-ipv4 2>/dev/null",
			         token);
			out = run_cmd(cmd);
		}
		free(token);
	} else if (strcmp(vendor, "azure") == 0) {
		out = run_cmd(
			"curl -fsS -m 1 -H 'Metadata: true' "
			"'http://169.254.169.254/metadata/instance/network/interface/0/"
			"ipv4/ipAddress/0/publicIpAddress?api-version=2021-02-01&format=text' 2>/dev/null");
	} else if (strcmp(vendor, "gcp") == 0) {
		out = run_cmd(
			"curl -fsS -m 1 -H 'Metadata-Flavor: Google' "
			"http://metadata.google.internal/computeMetadata/v1/instance/"
			"network-interfaces/0/access-configs/0/external-ip 2>/dev/null");
	}

	if (!out)
		return cJSON_CreateNull();
	trim_inplace(out);
	if (!*out) {
		free(out);
		return cJSON_CreateArray();
	}
	cJSON *arr = cJSON_CreateArray();
	cJSON_AddItemToArray(arr, cJSON_CreateString(out));
	free(out);
	return arr;
}

static cJSON *collect_services(void)
{
	char *out = run_cmd(
		"systemctl list-units --type=service --type=socket "
		"--state=running --state=listening "
		"--no-pager --plain --no-legend 2>/dev/null");
	if (!out)
		return cJSON_CreateNull();

	cJSON *arr = cJSON_CreateArray();
	if (!arr) { free(out); return NULL; }

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
	}
	free(out);
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
	cJSON_AddItemToObject(root, "mounts",      or_empty_array(collect_mounts_inventory()));

	cJSON_AddItemToObject(root, "services",     collect_services());
	cJSON_AddItemToObject(root, "listen_ports", or_empty_array(collect_listen_ports()));
	cJSON_AddItemToObject(root, "ip_internal",   or_empty_array(collect_internal_ips()));
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
	long u = 0, n = 0, s = 0, i = 0, w = 0, q = 0, sq = 0, st = 0;
	int got = sscanf(content, "cpu  %ld %ld %ld %ld %ld %ld %ld %ld",
	                 &u, &n, &s, &i, &w, &q, &sq, &st);
	free(content);
	if (got < 4)
		return 0;
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddNumberToObject(obj, "user",    (double)u);
	cJSON_AddNumberToObject(obj, "nice",    (double)n);
	cJSON_AddNumberToObject(obj, "system",  (double)s);
	cJSON_AddNumberToObject(obj, "idle",    (double)i);
	cJSON_AddNumberToObject(obj, "iowait",  (double)w);
	cJSON_AddNumberToObject(obj, "irq",     (double)q);
	cJSON_AddNumberToObject(obj, "softirq", (double)sq);
	cJSON_AddNumberToObject(obj, "steal",   (double)st);
	cJSON_AddItemToObject(root, "cpu_stat", obj);
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

	if (mem_available < 0)
		mem_available = derive_mem_available_kb(content, mem_free);
	if (mem_available < 0 && mem_free >= 0 && mem_buffers >= 0 && mem_cached >= 0)
		mem_available = mem_free + mem_buffers + mem_cached;

	free(content);

	if (mem_total < 0)
		return 0;

	cJSON_AddNumberToObject(root, "mem_total_kb", (double)mem_total);
	add_kb_or_null(root, "mem_free_kb",      mem_free);
	add_kb_or_null(root, "mem_buffers_kb",   mem_buffers);
	add_kb_or_null(root, "mem_cached_kb",    mem_cached);
	add_kb_or_null(root, "swap_total_kb",    swap_total);
	add_kb_or_null(root, "swap_free_kb",     swap_free);
	add_kb_or_null(root, "mem_available_kb", mem_available);
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

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "device", dev);
		add_major_minor(item, (int)major, (int)minor);
		cJSON_AddNumberToObject(item, "reads_completed",  (double)reads_completed);
		cJSON_AddNumberToObject(item, "writes_completed", (double)writes_completed);
		cJSON_AddNumberToObject(item, "sectors_read",     (double)sectors_read);
		cJSON_AddNumberToObject(item, "sectors_written",  (double)sectors_written);
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

		long rx_bytes = 0, rx_packets = 0, rx_errors = 0;
		long tx_bytes = 0, tx_packets = 0, tx_errors = 0;

		int n = sscanf(colon + 1,
		               "%ld %ld %ld %*d %*d %*d %*d %*d "
		               "%ld %ld %ld",
		               &rx_bytes, &rx_packets, &rx_errors,
		               &tx_bytes, &tx_packets, &tx_errors);
		if (n < 6)
			continue;

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "interface", iface);
		cJSON_AddNumberToObject(item, "rx_bytes",   (double)rx_bytes);
		cJSON_AddNumberToObject(item, "tx_bytes",   (double)tx_bytes);
		cJSON_AddNumberToObject(item, "rx_packets", (double)rx_packets);
		cJSON_AddNumberToObject(item, "tx_packets", (double)tx_packets);
		cJSON_AddNumberToObject(item, "rx_errors",  (double)rx_errors);
		cJSON_AddNumberToObject(item, "tx_errors",  (double)tx_errors);
		cJSON_AddItemToArray(arr, item);
	}
	free(content);
	return arr;
}

cJSON *collect_metrics_payload(const char *machine_id, const char *agent_version)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;
	add_common_metadata(root, "metrics", machine_id, agent_version);

	int ok = 1;
	if (!add_cpu_stat(root))     ok = 0;
	if (!add_meminfo_full(root)) ok = 0;
	if (!add_loadavg(root))      ok = 0;

	cJSON_AddItemToObject(root, "disk_io", or_empty_array(collect_disk_io()));
	cJSON_AddItemToObject(root, "mounts",  or_empty_array(collect_mounts_metrics()));
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

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	char ms_buf[32];
	iso8601_utc_ms(ts, ms_buf, sizeof ms_buf);
	cJSON_DeleteItemFromObject(root, "collected_at");
	cJSON_AddStringToObject(root, "collected_at", ms_buf);

	cJSON_AddStringToObject(root, "error_code",       error_code       ? error_code       : "UNKNOWN");
	cJSON_AddStringToObject(root, "error_message",    error_message    ? error_message    : "");
	cJSON_AddStringToObject(root, "failed_component", failed_component ? failed_component : "collect");

	if (retry_count >= 0)
		cJSON_AddNumberToObject(root, "retry_count", (double)retry_count);
	if (first_failed_at)
		cJSON_AddStringToObject(root, "first_failed_at", first_failed_at);
	if (recovered_at)
		cJSON_AddStringToObject(root, "recovered_at", recovered_at);

	return root;
}
