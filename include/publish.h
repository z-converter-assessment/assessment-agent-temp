#ifndef ASSESSMENT_AGENT_PUBLISH_H
#define ASSESSMENT_AGENT_PUBLISH_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	const char *host;
	int         port;
	const char *vhost;
	const char *user;
	const char *password;
	const char *exchange;

	int         heartbeat_sec;

	int         tls_enabled;
	const char *tls_ca_path;
	int         tls_verify_peer;
	int         tls_verify_hostname;
	const char *tls_cert_path;
	const char *tls_key_path;
} publish_config_t;

int publish_message(const publish_config_t *cfg,
                    const char *routing_key,
                    const char *body,
                    size_t      body_len);

typedef struct publish_conn_s publish_conn_t;

publish_conn_t *publish_conn_open(const publish_config_t *cfg);

void publish_conn_close(publish_conn_t *c);

int publish_conn_publish(publish_conn_t *c,
                         const char *exchange,
                         const char *routing_key,
                         const char *body,
                         size_t      body_len);

int publish_conn_get(publish_conn_t *c,
                     const char *queue,
                     char      **out_body,
                     size_t     *out_len,
                     uint64_t   *out_delivery_tag);

int publish_conn_ack(publish_conn_t *c, uint64_t delivery_tag);

int publish_conn_pump(publish_conn_t *c);

#endif
