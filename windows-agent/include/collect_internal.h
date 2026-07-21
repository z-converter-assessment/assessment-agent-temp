#ifndef ASSESSMENT_AGENT_COLLECT_INTERNAL_H
#define ASSESSMENT_AGENT_COLLECT_INTERNAL_H

#include <cJSON.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* collect_*.c 공용 내부 선언 — wire 모델(collect_model.c) + 파싱 유틸(collect_util.c).
 * 공개 API 는 collect.h. 이 헤더는 collect_*.c 만 포함한다. */

#define inet_ntop agent_inet_ntop
#define HTTP_GET_MAX_BYTES 256

struct http_sink {
	char  *buf;
	size_t len;
	size_t cap;
};

BYTE *perf_query(const char *value_name);
DWORD perf_index(const char *want);
HANDLE open_physical_drive(int i);
PERF_COUNTER_DEFINITION *perf_counter(PERF_OBJECT_TYPE *o, DWORD idx);
PERF_OBJECT_TYPE *perf_object(PERF_DATA_BLOCK *db, DWORD idx);
cJSON *wire_metric(cJSON *ns, const char *name, const char *type, const char *unit);
cJSON *wire_ns(cJSON *root, const char *ns);
cJSON *wire_or_empty_array(cJSON *arr);
cJSON *wire_or_null(cJSON *v);
cJSON *wire_point(cJSON *metric);
void wire_str_or_null(cJSON *o, const char *key, const char *v);
void wire_num_or_null(cJSON *o, const char *key, int have, double v);
void wire_bool_or_null(cJSON *o, const char *key, int have, int v);
char *fetch_imds_chain(const char *aws_metadata_url,
                              const char *azure_url, const char *gcp_url);
char *http_get_short(const char *url, const char *header, int put_request);
const char *agent_inet_ntop(int af, const void *src, char *dst, size_t size);
const char *win_id_type(const char *full);
const char *win_id_value(const char *full);
const char *win_net_kind(ULONG if_type);
int agent_interval_sec(void);
int agent_is_nt6(void);
int iface_is_hardware(DWORD if_index);
int legacy_ipv4_gateway(DWORD if_index, char *out, size_t out_sz);
int legacy_ipv4_prefix(DWORD if_index, unsigned addr_be, int *out_prefix);
int perf_disk_num(const wchar_t *name);
int query_system_io(unsigned long long *read_ops, unsigned long long *write_ops,
                           unsigned long long *read_bytes, unsigned long long *write_bytes);
long long query_free_kb(void);
size_t http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata);
unsigned long long perf_read(PERF_COUNTER_BLOCK *cb, PERF_COUNTER_DEFINITION *c);
void mac_to_devid(const unsigned char *mac, unsigned len, const char *fallback, char *out, size_t outsz);
void win_disk_id(int i, char *out, size_t outsz);
void wire_add_envelope(cJSON *root, const char *msg_type,
                                const char *machine_id, const char *agent_version);
void wire_metric_scalar(cJSON *ns, const char *name, const char *type,
                              const char *unit, int have, double v);
void wire_point_attr(cJSON *point, const char *k, const char *v);
void wire_point_null(cJSON *point);
void wire_point_value(cJSON *point, double v);

void wire_point_dev_dir(cJSON *metric, const char *device, const char *direction, double value);

#endif
