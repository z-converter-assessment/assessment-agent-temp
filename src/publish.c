#define _POSIX_C_SOURCE 200809L

#include "publish.h"
#include "util.h"

#include <amqp.h>
#include <amqp_framing.h>
#include <amqp_tcp_socket.h>
#include <amqp_ssl_socket.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

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

static int wait_confirm(amqp_connection_state_t conn)
{
	int t = getenv_int_or("RABBITMQ_CONFIRM_TIMEOUT_SEC", 5);
	long limit_ms = (long)(t > 0 ? t : 5) * 1000;

	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);

	amqp_frame_t frame;
	for (;;) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000
		                + (now.tv_nsec - start.tv_nsec) / 1000000;
		long remain_ms = limit_ms - elapsed_ms;
		if (remain_ms <= 0) {
			fprintf(stderr, "[publish] confirm: timed out waiting for broker ACK\n");
			return -1;
		}
		struct timeval tv = {
			.tv_sec  = remain_ms / 1000,
			.tv_usec = (remain_ms % 1000) * 1000,
		};

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

/* AMQP 연결 + (TLS)소켓 + open + FD_CLOEXEC + login + 채널 1 open + confirm.select.
 * oneshot 발행(publish_message)과 worker 상시 연결(open_amqp_connection)이 이 하나를 공유해
 * TLS verify/cacert/cert 설정과 소켓 보안(FD_CLOEXEC)이 두 경로에서 동일함을 보장한다 —
 * 한쪽에만 하드닝 옵션을 더해 보안 스탠스가 갈리는 드리프트를 막는다. who 는 로그 접두사.
 * 성공 시 *out_conn 을 채우고 0, 실패 시 연결을 정리하고 -1. */
static int amqp_connect_open(const publish_config_t *cfg, const char *who,
                            amqp_connection_state_t *out_conn)
{
	char ctx[64];
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
			goto fail;
		}
		if (cfg->tls_ca_path && *cfg->tls_ca_path &&
		    amqp_ssl_socket_set_cacert(socket, cfg->tls_ca_path) != AMQP_STATUS_OK) {
			fprintf(stderr, "[publish] set_cacert(%s) failed\n", cfg->tls_ca_path);
			goto fail;
		}
		amqp_ssl_socket_set_verify_peer(socket, cfg->tls_verify_peer ? 1 : 0);
		amqp_ssl_socket_set_verify_hostname(socket, cfg->tls_verify_hostname ? 1 : 0);
		if (cfg->tls_cert_path && *cfg->tls_cert_path &&
		    cfg->tls_key_path  && *cfg->tls_key_path &&
		    amqp_ssl_socket_set_key(socket, cfg->tls_cert_path,
		                            cfg->tls_key_path) != AMQP_STATUS_OK) {
			fprintf(stderr, "[publish] set_key(%s) failed\n", cfg->tls_cert_path);
			goto fail;
		}
	} else {
		socket = amqp_tcp_socket_new(conn);
		if (!socket) {
			fprintf(stderr, "[publish] amqp_tcp_socket_new failed\n");
			goto fail;
		}
	}

	if (amqp_socket_open(socket, cfg->host, cfg->port) != AMQP_STATUS_OK) {
		fprintf(stderr, "[publish] amqp_socket_open(%s:%d) failed\n",
		        cfg->host, cfg->port);
		goto fail;
	}

	int sockfd = amqp_get_sockfd(conn);
	if (sockfd >= 0) {
		int fl = fcntl(sockfd, F_GETFD, 0);
		if (fl >= 0) (void)fcntl(sockfd, F_SETFD, fl | FD_CLOEXEC);
	} else {
		fprintf(stderr, "[publish] WARN: amqp_get_sockfd returned -1; FD_CLOEXEC not set "
		                "(install.sh fd-sweep is sole protection)\n");
	}

	const char *vhost = (cfg->vhost && *cfg->vhost) ? cfg->vhost : "/";
	int heartbeat = cfg->heartbeat_sec > 0 ? cfg->heartbeat_sec : 60;
	amqp_rpc_reply_t login = amqp_login(
		conn, vhost, 0, 131072, heartbeat, AMQP_SASL_METHOD_PLAIN,
		cfg->user, cfg->password);
	snprintf(ctx, sizeof ctx, "%slogin", who);
	if (check_rpc(login, ctx) != 0)
		goto fail;

	amqp_channel_open(conn, 1);
	snprintf(ctx, sizeof ctx, "%schannel.open", who);
	if (check_rpc(amqp_get_rpc_reply(conn), ctx) != 0)
		goto fail_close_conn;

	amqp_confirm_select(conn, 1);
	snprintf(ctx, sizeof ctx, "%sconfirm.select", who);
	if (check_rpc(amqp_get_rpc_reply(conn), ctx) != 0)
		goto fail_close_channel;

	*out_conn = conn;
	return 0;

fail_close_channel:
	amqp_channel_close(conn, 1, AMQP_REPLY_SUCCESS);
fail_close_conn:
	amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
fail:
	amqp_destroy_connection(conn);
	return -1;
}

int publish_message(const publish_config_t *cfg,
                    const char *routing_key,
                    const char *body,
                    size_t body_len)
{
	int rc = -1;
	amqp_connection_state_t conn;
	if (amqp_connect_open(cfg, "", &conn) != 0)
		return -1;

	amqp_exchange_declare(conn, 1,
		amqp_cstring_bytes(cfg->exchange),
		amqp_cstring_bytes("direct"),
		1 , 1 , 0, 0, amqp_empty_table);
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
	props.delivery_mode = 2;
	props.message_id    = amqp_cstring_bytes(msg_id);

	amqp_bytes_t body_bytes = { .len = body_len, .bytes = (void *)body };

	int pub = amqp_basic_publish(conn, 1,
		amqp_cstring_bytes(cfg->exchange),
		amqp_cstring_bytes(routing_key),
		0 , 0 ,
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
	amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
	amqp_destroy_connection(conn);
	return rc;
}

struct publish_conn_s {
	amqp_connection_state_t conn;
};

static int open_amqp_connection(const publish_config_t *cfg,
                                amqp_connection_state_t *out_conn)
{
	return amqp_connect_open(cfg, "worker ", out_conn);
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

/* basic.get 이 존재하지 않는 큐를 폴링하면 브로커가 404 NOT_FOUND 로 채널을 닫는다. task 큐
 * (agent.tasks.{agent_id})는 engine 이 첫 task 발행 시점에 lazy 생성하고 worker 는 declare 권한이
 * 없어, 그 전까지는 이 404 가 정상적으로 뜬다 — 에러가 아니라 "큐 아직 없음(대기)" 상태다.
 * 이 특정 채널 예외만 가려내 조용히 처리한다(다른 서버 예외는 그대로 에러). */
static int is_queue_not_found(amqp_rpc_reply_t r)
{
	if (r.reply_type != AMQP_RESPONSE_SERVER_EXCEPTION) return 0;
	if (r.reply.id != AMQP_CHANNEL_CLOSE_METHOD)        return 0;
	amqp_channel_close_t *m = (amqp_channel_close_t *)r.reply.decoded;
	return m && m->reply_code == 404;  /* AMQP NOT_FOUND */
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
		amqp_cstring_bytes(queue), 0 );
	if (r.reply_type != AMQP_RESPONSE_NORMAL) {
		if (is_queue_not_found(r))
			return 2;  /* 큐 아직 없음 — 경고 없이, 호출자가 채널만 복구 */
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

int publish_conn_recover_channel(publish_conn_t *c)
{
	if (!c || !c->conn) return -1;
	/* 채널 레벨 예외(큐 404 등) 후 채널 1 을 다시 연다. 연결은 살아있으므로 전체 reconnect
	 * 없이 폴링을 이어간다. worker result 발행이 confirm 모드라 confirm.select 재적용.
	 * 조용히(로그 없이) 판정 — 실패하면 호출자가 전체 reconnect 로 폴백한다. */
	amqp_channel_open(c->conn, 1);
	if (amqp_get_rpc_reply(c->conn).reply_type != AMQP_RESPONSE_NORMAL)
		return -1;
	amqp_confirm_select(c->conn, 1);
	if (amqp_get_rpc_reply(c->conn).reply_type != AMQP_RESPONSE_NORMAL)
		return -1;
	return 0;
}

int publish_conn_ack(publish_conn_t *c, uint64_t delivery_tag)
{
	if (!c || !c->conn) return -1;
	if (amqp_basic_ack(c->conn, 1, delivery_tag, 0 ) != AMQP_STATUS_OK)
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
