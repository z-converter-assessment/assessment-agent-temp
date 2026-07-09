#ifndef ASSESSMENT_AGENT_COLLECT_INTERNAL_H
#define ASSESSMENT_AGENT_COLLECT_INTERNAL_H

#include <cJSON.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* collect_*.c 공용 내부 선언 — wire 모델(collect_model.c) + 파싱 유틸(collect_util.c).
 * 공개 API 는 collect.h. 이 헤더는 collect_*.c 만 포함한다. */

#define TCP_STATE_LISTEN_HEX "0A"

struct mount_entry {
	int   major;
	int   minor;
	char *mount;
	char *fstype;
};

cJSON *build_default_gw_v4(void);
cJSON *collect_mac_addresses(void);
cJSON *wire_metric(cJSON *ns, const char *name, const char *type, const char *unit);
cJSON *wire_ns(cJSON *root, const char *ns);
cJSON *wire_or_empty_array(cJSON *arr);
cJSON *wire_or_null(cJSON *v);
cJSON *wire_point(cJSON *metric);
char *fetch_cloud_metadata(const char *aws_path, const char *azure_path,
                                  const char *gcp_path);
char *try_cloud_instance_id(void);
char *try_dbus_uuidgen(void);
char *try_machine_id_file(const char *path);
const char *cached_agent_started_at_iso(void);
const char *cached_boot_time_iso(void);
const char *detect_cloud_vendor(void);
const char *net_kind(const char *ifname);
int agent_interval_sec(void);
int fstype_is_nodev(const char *fstype);
int ipv4_netmask_prefix(uint32_t mask_n);
int ipv6_netmask_prefix(const struct sockaddr_in6 *mask);
int is_excluded_block_dev(const char *name);
int is_excluded_fstype(const char *fstype);
int is_kept_deviceless_fs(const char *fstype);
int is_machine_id(const char *s);
int mac_str_cmp(const void *a, const void *b);
int parse_major_minor(const char *s, int *major, int *minor);
int parse_mountinfo_line(const char *line,
                                int *major, int *minor,
                                char **mount_out, char **fstype_out);
int read_sysfs_str(const char *path, char *out, size_t outsz);
long meminfo_get_kb(const char *content, const char *key);
long snmp_tcp_retranssegs(void);
struct mount_entry *list_real_mounts(size_t *out_count);
void dedup_mounts(struct mount_entry *arr, size_t *count);
void disk_device_id(const char *dev, char *out, size_t outsz);
void free_mount_entries(struct mount_entry *arr, size_t n);
void net_device_id(const char *iface, char *out, size_t outsz);
void wire_add_envelope(cJSON *obj,
                                const char *message_type,
                                const char *machine_id,
                                const char *agent_version);
void wire_metric_scalar(cJSON *ns, const char *name, const char *type,
                          const char *unit, int have, double v);
void wire_point_attr(cJSON *point, const char *k, const char *v);
void wire_point_null(cJSON *point);
void wire_point_value(cJSON *point, double v);

void wire_point_dev_dir(cJSON *metric, const char *device, const char *direction, double value);

#endif
