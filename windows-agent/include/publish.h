/**
 * @file publish.h
 * @brief RabbitMQ publisher (Windows). Same wire contract as Linux agent.
 *
 * Topology contract:
 *   - Exchange : `assessment` (direct, durable)
 *   - Routing keys: `server.inventory`, `server.metrics`, `server.error`
 *   - Vhost   : `/` for local dev, `/assessment` in production
 *
 * Collector 는 publish_message() 한 함수로 open-publish-close 패턴. worker (v2,
 * task.install consumer) 는 long-lived publish_conn_* API 로 동일 채널에서
 * basic_get / basic_publish / basic_ack 를 처리한다 (broker delivery_tag 가
 * 채널 lifetime 동안만 유효하므로).
 */

#ifndef ASSESSMENT_AGENT_PUBLISH_H
#define ASSESSMENT_AGENT_PUBLISH_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	const char *host;
	int         port;       /**< 5672 AMQP, 5671 AMQPS */
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

/**
 * @brief Publish a single JSON message to the broker.
 *
 * Internally: open socket → optional TLS → login → channel.open
 *             → confirm.select → exchange.declare(passive)
 *             → basic.publish(persistent, JSON) → wait broker ACK → close
 *
 * @return 0 on success, negative on failure. Diagnostic to stderr.
 */
int publish_message(const publish_config_t *cfg,
                    const char *routing_key,
                    const char *body,
                    size_t      body_len);

/* ============================================================
 * Long-lived connection (worker role — CM2 second connection)
 * ============================================================ */

typedef struct publish_conn_s publish_conn_t;

publish_conn_t *publish_conn_open(const publish_config_t *cfg);
void            publish_conn_close(publish_conn_t *c);

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
