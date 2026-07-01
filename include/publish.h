/**
 * @file publish.h
 * @brief RabbitMQ publisher (direct exchange, durable, persistent).
 *
 * Topology contract:
 *   - Exchange : `assessment` (direct, durable)
 *   - Routing keys: `server.inventory`, `server.metrics`, `server.error`
 *   - Vhost   : `/` for local dev, `/assessment` in production
 *
 * The agent does not declare queues or bindings. The consumer
 * (`assessment-engine`) owns topology declaration via its `topology-admin`
 * user. The agent is `agent-publisher` with publish-only permission.
 */

#ifndef ASSESSMENT_AGENT_PUBLISH_H
#define ASSESSMENT_AGENT_PUBLISH_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Broker connection settings.
 *
 * All string fields must outlive any @ref publish_message call that uses
 * them. The struct is plain-old-data and is not freed by this module.
 */
typedef struct {
	const char *host;       /**< Broker hostname or IP (internal network). */
	int         port;       /**< 5672 for AMQP, 5671 for AMQPS. */
	const char *vhost;      /**< AMQP vhost. `/` for dev, `/assessment` in prod. */
	const char *user;       /**< SASL PLAIN user (`agent-publisher` in prod). */
	const char *password;   /**< SASL PLAIN password. */
	const char *exchange;   /**< Direct exchange name (default `assessment`). */

	/**
	 * AMQP heartbeat interval in seconds.
	 * RabbitMQ recommends 60 (typical 10–60; 0 disables).
	 * https://www.rabbitmq.com/heartbeats.html
	 */
	int         heartbeat_sec;

	/* TLS (AMQPS). When @c tls_enabled is non-zero, port should be 5671. */
	int         tls_enabled;
	const char *tls_ca_path;     /**< Path to internal CA pem (required if TLS). */
	int         tls_verify_peer;
	int         tls_verify_hostname;
	const char *tls_cert_path;   /**< Optional mTLS client cert pem. */
	const char *tls_key_path;    /**< Optional mTLS client key pem. */
} publish_config_t;

/**
 * @brief Publish a single JSON message to the broker.
 *
 * Internally:
 *   open socket → optional TLS handshake → login → channel.open
 *     → confirm.select → exchange.declare(passive)
 *     → basic.publish(persistent, JSON) → wait broker ACK → close
 *
 * `exchange.declare` is called passively (only verifies existence). Queue
 * declaration is the consumer's responsibility.
 *
 * @param cfg         Connection settings.
 * @param routing_key One of `server.inventory` / `server.metrics` / `server.error`.
 * @param body        UTF-8 JSON serialized payload.
 * @param body_len    Byte length of @p body.
 * @return 0 on success, negative on failure. Diagnostic written to stderr.
 */
int publish_message(const publish_config_t *cfg,
                    const char *routing_key,
                    const char *body,
                    size_t      body_len);

/* ============================================================
 * Long-lived connection (worker role — CM2 second connection)
 *
 * The collector uses publish_message() above, which opens-publishes-closes
 * per call. The worker needs a persistent channel because the broker's
 * delivery_tag for a `basic_get`'d message stays valid only until the
 * channel closes — and the worker holds a message unacked across the
 * minutes-long install. A separate connection (different credentials,
 * agent-worker) also enforces CM2's privilege split.
 * ============================================================ */

typedef struct publish_conn_s publish_conn_t;

/**
 * @brief Open a persistent AMQP connection + channel with publisher confirms.
 *
 * The connection stays alive until publish_conn_close(). On any error the
 * function returns NULL after logging to stderr.
 */
publish_conn_t *publish_conn_open(const publish_config_t *cfg);

/**
 * @brief Close the connection and free the handle.
 */
void publish_conn_close(publish_conn_t *c);

/**
 * @brief Publish a JSON body via this connection, waiting for confirm.
 *
 * Re-uses the same channel as `_get` / `_ack`. The exchange must already
 * exist (declared by topology bootstrap). Returns 0 on success.
 */
int publish_conn_publish(publish_conn_t *c,
                         const char *exchange,
                         const char *routing_key,
                         const char *body,
                         size_t      body_len);

/**
 * @brief `basic.get` one message from @p queue (no_ack=false).
 *
 * @param c                Connection handle.
 * @param queue            Queue name (e.g. `agent.tasks.<machine_id>`).
 * @param out_body         Receives malloc'd body buffer on PUBLISH_GET_OK.
 *                         Caller must free(). On empty/error, set to NULL.
 * @param out_len          Body length in bytes on PUBLISH_GET_OK.
 * @param out_delivery_tag Receives delivery_tag for the subsequent ack.
 *
 * @return 0 if a message was returned (body + delivery_tag valid),
 *         1 if the queue is empty (no message),
 *        -1 on transport / channel error (caller should reopen).
 */
int publish_conn_get(publish_conn_t *c,
                     const char *queue,
                     char      **out_body,
                     size_t     *out_len,
                     uint64_t   *out_delivery_tag);

/**
 * @brief Ack a previously fetched delivery_tag on the same channel.
 *
 * Returns 0 on success, -1 on send failure. After failure the connection
 * is likely broken; caller should close and reopen.
 */
int publish_conn_ack(publish_conn_t *c, uint64_t delivery_tag);

/**
 * @brief Heartbeat tick — called from the main loop while idle.
 *
 * Triggers librabbitmq's heartbeat send (if due) and drains any
 * incoming frames (broker heartbeats / async errors). Uses a 1ms
 * select wait so `amqp_try_send_heartbeat` runs.
 *
 * Returns 0 if the connection looks alive, -1 if a transport-level
 * failure (heartbeat timeout, socket error) was observed; caller should
 * mark the connection dead and reconnect on the next tick.
 */
int publish_conn_pump(publish_conn_t *c);

#endif
