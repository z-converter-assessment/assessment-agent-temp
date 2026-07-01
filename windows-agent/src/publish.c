/**
 * @file publish.c
 * @brief librabbitmq-based publisher (Windows). Linux 와 동일 와이어 컨트랙트.
 *
 * Each call opens a fresh connection, publishes one message, and tears down.
 * v1 은 worker 가 없으므로 long-lived connection API (publish_conn_*) 는 제공
 * 하지 않는다.
 *
 * Topology declaration 은 컨슈머 책임. publisher 는 exchange passive declare
 * 로 존재 여부만 확인하고, 큐나 바인딩은 절대 선언하지 않는다.
 */

#include "publish.h"
#include "util.h"

#include <amqp.h>
#include <amqp_framing.h>
#include <amqp_tcp_socket.h>
#include <amqp_ssl_socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <winsock2.h>   /* struct timeval, on Windows lives here */

static int check_rpc(amqp_rpc_reply_t r, const char *ctx)
{
	switch (r.reply_type) {
	case AMQP_RESPONSE_NORMAL:
		return 0;
	case AMQP_RESPONSE_NONE:
		fprintf(stderr, "[publish] %s: missing RPC reply\n", ctx);
		return -1;
	case AMQP_RESPONSE_LIBRARY_EXCEPTION:
		fprintf(stderr, "[publish] %s: %s\n", ctx,
		        amqp_error_string2(r.library_error));
		return -1;
	case AMQP_RESPONSE_SERVER_EXCEPTION:
		fprintf(stderr, "[publish] %s: server exception (class=%u method=%u)\n",
		        ctx, r.reply.id >> 16, r.reply.id & 0xFFFF);
		return -1;
	}
	return -1;
}

/**
 * @brief Wait for broker ACK/NACK with a wall-clock deadline.
 *
 * GetTickCount64 (monotonic ms since boot) 로 deadline 추적. wait_frame_noblock
 * 의 per-call timeout 은 select 시간이지 deadline 이 아니므로, 무관한 frame
 * 이 와도 누적 대기 시간이 RABBITMQ_CONFIRM_TIMEOUT_SEC 를 넘지 않게 캡.
 */
static int wait_confirm(amqp_connection_state_t conn)
{
	int t = getenv_int_or("RABBITMQ_CONFIRM_TIMEOUT_SEC", 5);
	long limit_ms = (long)(t > 0 ? t : 5) * 1000;

	ULONGLONG start_ms = monotonic_ms();

	amqp_frame_t frame;
	for (;;) {
		ULONGLONG now_ms = monotonic_ms();
		long elapsed_ms = (long)(now_ms - start_ms);
		long remain_ms = limit_ms - elapsed_ms;
		if (remain_ms <= 0) {
			fprintf(stderr, "[publish] confirm: timed out waiting for broker ACK\n");
			return -1;
		}
		struct timeval tv;
		tv.tv_sec  = remain_ms / 1000;
		tv.tv_usec = (long)((remain_ms % 1000) * 1000);

		amqp_maybe_release_buffers(conn);
		int status = amqp_simple_wait_frame_noblock(conn, &frame, &tv);
		if (status == AMQP_STATUS_TIMEOUT)
			continue;
		if (status != AMQP_STATUS_OK) {
			fprintf(stderr, "[publish] confirm: frame error: %s\n",
			        amqp_error_string2(status));
			return -1;
		}
		if (frame.frame_type != AMQP_FRAME_METHOD || frame.channel != 1)
			continue;
		if (frame.payload.method.id == AMQP_BASIC_ACK_METHOD)
			return 0;
		if (frame.payload.method.id == AMQP_BASIC_NACK_METHOD) {
			fprintf(stderr, "[publish] confirm: broker NACK'd the message\n");
			return -1;
		}
	}
}

int publish_message(const publish_config_t *cfg,
                    const char *routing_key,
                    const char *body,
                    size_t body_len)
{
	int rc = -1;
	amqp_connection_state_t conn = amqp_new_connection();
	if (!conn) {
		fprintf(stderr, "[publish] amqp_new_connection failed\n");
		return -1;
	}

	amqp_socket_t *socket = NULL;
	if (cfg->tls_enabled) {
		socket = amqp_ssl_socket_new(conn);
		if (!socket) {
			fprintf(stderr, "[publish] amqp_ssl_socket_new failed\n");
			goto out_destroy;
		}
		if (cfg->tls_ca_path && *cfg->tls_ca_path) {
			if (amqp_ssl_socket_set_cacert(socket, cfg->tls_ca_path) != AMQP_STATUS_OK) {
				fprintf(stderr, "[publish] set_cacert(%s) failed\n", cfg->tls_ca_path);
				goto out_destroy;
			}
		}
		amqp_ssl_socket_set_verify_peer(socket, cfg->tls_verify_peer ? 1 : 0);
		amqp_ssl_socket_set_verify_hostname(socket, cfg->tls_verify_hostname ? 1 : 0);
		if (cfg->tls_cert_path && *cfg->tls_cert_path &&
		    cfg->tls_key_path  && *cfg->tls_key_path) {
			if (amqp_ssl_socket_set_key(socket, cfg->tls_cert_path,
			                            cfg->tls_key_path) != AMQP_STATUS_OK) {
				fprintf(stderr, "[publish] set_key(%s) failed\n", cfg->tls_cert_path);
				goto out_destroy;
			}
		}
	} else {
		socket = amqp_tcp_socket_new(conn);
		if (!socket) {
			fprintf(stderr, "[publish] amqp_tcp_socket_new failed\n");
			goto out_destroy;
		}
	}

	if (amqp_socket_open(socket, cfg->host, cfg->port) != AMQP_STATUS_OK) {
		fprintf(stderr, "[publish] amqp_socket_open(%s:%d) failed\n",
		        cfg->host, cfg->port);
		goto out_destroy;
	}

	const char *vhost = (cfg->vhost && *cfg->vhost) ? cfg->vhost : "/";
	int heartbeat = cfg->heartbeat_sec > 0 ? cfg->heartbeat_sec : 60;
	amqp_rpc_reply_t login = amqp_login(
		conn, vhost, 0, 131072, heartbeat, AMQP_SASL_METHOD_PLAIN,
		cfg->user, cfg->password);
	if (check_rpc(login, "login") != 0)
		goto out_destroy;

	amqp_channel_open(conn, 1);
	if (check_rpc(amqp_get_rpc_reply(conn), "channel.open") != 0)
		goto out_close_conn;

	amqp_confirm_select(conn, 1);
	if (check_rpc(amqp_get_rpc_reply(conn), "confirm.select") != 0)
		goto out_close_channel;

	amqp_exchange_declare(conn, 1,
		amqp_cstring_bytes(cfg->exchange),
		amqp_cstring_bytes("direct"),
		1 /* passive */, 1 /* durable */, 0, 0, amqp_empty_table);
	if (check_rpc(amqp_get_rpc_reply(conn), "exchange.declare(passive)") != 0)
		goto out_close_channel;

	char msg_id[64];
	uuid_v4(msg_id, sizeof msg_id);

	amqp_basic_properties_t props;
	memset(&props, 0, sizeof props);
	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG
	             | AMQP_BASIC_DELIVERY_MODE_FLAG
	             | AMQP_BASIC_MESSAGE_ID_FLAG;
	props.content_type  = amqp_cstring_bytes("application/json");
	props.delivery_mode = 2; /* persistent */
	props.message_id    = amqp_cstring_bytes(msg_id);

	amqp_bytes_t body_bytes;
	body_bytes.len   = body_len;
	body_bytes.bytes = (void *)body;

	int pub = amqp_basic_publish(conn, 1,
		amqp_cstring_bytes(cfg->exchange),
		amqp_cstring_bytes(routing_key),
		0 /* mandatory */, 0 /* immediate */,
		&props, body_bytes);
	if (pub != AMQP_STATUS_OK) {
		fprintf(stderr, "[publish] basic.publish: %s\n",
		        amqp_error_string2(pub));
		goto out_close_channel;
	}

	if (wait_confirm(conn) != 0)
		goto out_close_channel;

	rc = 0;

out_close_channel:
	amqp_channel_close(conn, 1, AMQP_REPLY_SUCCESS);
out_close_conn:
	amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
out_destroy:
	amqp_destroy_connection(conn);
	return rc;
}

/* ============================================================
 * Long-lived connection (worker role — CM2 second connection)
 *
 * Linux publish.c 의 동일 영역 포팅. Windows 차이:
 *   - fcntl(FD_CLOEXEC) 미사용 — CreateProcessA(bInheritHandles=TRUE) 에
 *     명시 상속하는 핸들은 stdin/stdout/stderr 뿐. AMQP socket 은 winsock
 *     기본 정책상 inheritable 마킹 안 된 채 자식이 못 받음.
 * ============================================================ */

struct publish_conn_s {
	amqp_connection_state_t conn;
};

static int open_amqp_connection(const publish_config_t *cfg,
                                amqp_connection_state_t *out_conn)
{
	amqp_connection_state_t conn = amqp_new_connection();
	if (!conn) return -1;

	amqp_socket_t *socket = NULL;
	if (cfg->tls_enabled) {
		socket = amqp_ssl_socket_new(conn);
		if (!socket) { amqp_destroy_connection(conn); return -1; }
		if (cfg->tls_ca_path && *cfg->tls_ca_path &&
		    amqp_ssl_socket_set_cacert(socket, cfg->tls_ca_path) != AMQP_STATUS_OK) {
			amqp_destroy_connection(conn); return -1;
		}
		amqp_ssl_socket_set_verify_peer(socket, cfg->tls_verify_peer ? 1 : 0);
		amqp_ssl_socket_set_verify_hostname(socket, cfg->tls_verify_hostname ? 1 : 0);
		if (cfg->tls_cert_path && *cfg->tls_cert_path &&
		    cfg->tls_key_path  && *cfg->tls_key_path &&
		    amqp_ssl_socket_set_key(socket, cfg->tls_cert_path, cfg->tls_key_path) != AMQP_STATUS_OK) {
			amqp_destroy_connection(conn); return -1;
		}
	} else {
		socket = amqp_tcp_socket_new(conn);
		if (!socket) { amqp_destroy_connection(conn); return -1; }
	}

	if (amqp_socket_open(socket, cfg->host, cfg->port) != AMQP_STATUS_OK) {
		amqp_destroy_connection(conn); return -1;
	}

	const char *vhost = (cfg->vhost && *cfg->vhost) ? cfg->vhost : "/";
	int heartbeat = cfg->heartbeat_sec > 0 ? cfg->heartbeat_sec : 60;
	amqp_rpc_reply_t login = amqp_login(
		conn, vhost, 0, 131072, heartbeat, AMQP_SASL_METHOD_PLAIN,
		cfg->user, cfg->password);
	if (check_rpc(login, "worker login") != 0) {
		amqp_destroy_connection(conn); return -1;
	}

	amqp_channel_open(conn, 1);
	if (check_rpc(amqp_get_rpc_reply(conn), "worker channel.open") != 0) {
		amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
		amqp_destroy_connection(conn); return -1;
	}

	amqp_confirm_select(conn, 1);
	if (check_rpc(amqp_get_rpc_reply(conn), "worker confirm.select") != 0) {
		amqp_channel_close(conn, 1, AMQP_REPLY_SUCCESS);
		amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
		amqp_destroy_connection(conn); return -1;
	}

	*out_conn = conn;
	return 0;
}

publish_conn_t *publish_conn_open(const publish_config_t *cfg)
{
	if (!cfg) return NULL;
	publish_conn_t *c = (publish_conn_t *)calloc(1, sizeof *c);
	if (!c) return NULL;
	if (open_amqp_connection(cfg, &c->conn) != 0) {
		free(c); return NULL;
	}
	return c;
}

void publish_conn_close(publish_conn_t *c)
{
	if (!c) return;
	if (c->conn) {
		amqp_channel_close(c->conn, 1, AMQP_REPLY_SUCCESS);
		amqp_connection_close(c->conn, AMQP_REPLY_SUCCESS);
		amqp_destroy_connection(c->conn);
	}
	free(c);
}

int publish_conn_publish(publish_conn_t *c,
                         const char *exchange,
                         const char *routing_key,
                         const char *body,
                         size_t      body_len)
{
	if (!c || !c->conn || !exchange || !routing_key) return -1;

	char msg_id[64];
	uuid_v4(msg_id, sizeof msg_id);

	amqp_basic_properties_t props;
	memset(&props, 0, sizeof props);
	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG
	             | AMQP_BASIC_DELIVERY_MODE_FLAG
	             | AMQP_BASIC_MESSAGE_ID_FLAG;
	props.content_type  = amqp_cstring_bytes("application/json");
	props.delivery_mode = 2;
	props.message_id    = amqp_cstring_bytes(msg_id);

	amqp_bytes_t body_bytes = { .len = body_len, .bytes = (void *)body };
	int pub = amqp_basic_publish(c->conn, 1,
		amqp_cstring_bytes(exchange),
		amqp_cstring_bytes(routing_key),
		0, 0, &props, body_bytes);
	if (pub != AMQP_STATUS_OK) {
		fprintf(stderr, "[worker-publish] basic.publish: %s\n",
		        amqp_error_string2(pub));
		return -1;
	}
	return wait_confirm(c->conn);
}

int publish_conn_get(publish_conn_t *c,
                     const char *queue,
                     char      **out_body,
                     size_t     *out_len,
                     uint64_t   *out_delivery_tag)
{
	if (!c || !c->conn || !queue || !out_body || !out_len || !out_delivery_tag)
		return -1;
	*out_body = NULL;
	*out_len  = 0;
	*out_delivery_tag = 0;

	amqp_rpc_reply_t r = amqp_basic_get(c->conn, 1,
		amqp_cstring_bytes(queue), 0 /* no_ack=false */);
	if (r.reply_type != AMQP_RESPONSE_NORMAL) {
		check_rpc(r, "basic.get");
		return -1;
	}
	if (r.reply.id == AMQP_BASIC_GET_EMPTY_METHOD)
		return 1;
	if (r.reply.id != AMQP_BASIC_GET_OK_METHOD)
		return -1;

	amqp_basic_get_ok_t *get_ok = (amqp_basic_get_ok_t *)r.reply.decoded;
	uint64_t tag = get_ok->delivery_tag;

	amqp_message_t msg;
	amqp_rpc_reply_t rr = amqp_read_message(c->conn, 1, &msg, 0);
	if (rr.reply_type != AMQP_RESPONSE_NORMAL) {
		check_rpc(rr, "read_message");
		return -1;
	}

	char *buf = (char *)malloc(msg.body.len + 1);
	if (!buf) { amqp_destroy_message(&msg); return -1; }
	memcpy(buf, msg.body.bytes, msg.body.len);
	buf[msg.body.len] = '\0';

	*out_body = buf;
	*out_len  = msg.body.len;
	*out_delivery_tag = tag;
	amqp_destroy_message(&msg);
	return 0;
}

int publish_conn_ack(publish_conn_t *c, uint64_t delivery_tag)
{
	if (!c || !c->conn) return -1;
	if (amqp_basic_ack(c->conn, 1, delivery_tag, 0) != AMQP_STATUS_OK)
		return -1;
	return 0;
}

int publish_conn_pump(publish_conn_t *c)
{
	if (!c || !c->conn) return -1;
	struct timeval tv = { .tv_sec = 0, .tv_usec = 1000 };
	amqp_frame_t frame;
	amqp_maybe_release_buffers(c->conn);
	int s = amqp_simple_wait_frame_noblock(c->conn, &frame, &tv);
	if (s == AMQP_STATUS_OK || s == AMQP_STATUS_TIMEOUT) return 0;
	if (s == AMQP_STATUS_HEARTBEAT_TIMEOUT ||
	    s == AMQP_STATUS_CONNECTION_CLOSED ||
	    s == AMQP_STATUS_SOCKET_ERROR ||
	    s == AMQP_STATUS_TCP_ERROR) {
		fprintf(stderr, "[worker-publish] heartbeat pump detected dead connection: %s\n",
		        amqp_error_string2(s));
		return -1;
	}
	return 0;
}
