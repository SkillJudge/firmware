/*
 * mqtt.c — 极简 MQTT 3.1.1 客户端（同步、一次性连接发布）
 *
 * 设计取舍（相对 EncoderAlertdDesign.md §5.2 状态机的简化）：
 *   每条报警独立走「connect → connack → publish(qos1) → 等 puback → disconnect」，
 *   与 sh 版 mosquitto_pub 行为等价。报警频率受 dedup 约束（默认 10 分钟/码），
 *   局域网内握手开销 <10ms，换取：无常驻状态机 / 无 keepalive 线程 / 无粘包残留。
 *   失败分类返回 ERR_TCP / ERR_CONNACK / ERR_PUBACK，供 1101 差分诊断。
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "common.h"

#define CONNECT_TIMEOUT_SEC 3
#define ACK_TIMEOUT_SEC     5
#define KEEPALIVE_SEC       60

/* ---------------- varlen / buffer helpers ---------------- */

static size_t put_varlen(uint8_t *buf, long len)
{
	size_t n = 0;

	do {
		uint8_t d = (uint8_t)(len % 128);
		len /= 128;
		if (len > 0)
			d |= 0x80;
		buf[n++] = d;
	} while (len > 0);
	return n;
}

static void put_u16(uint8_t *buf, uint16_t v)
{
	buf[0] = (uint8_t)(v >> 8);
	buf[1] = (uint8_t)(v & 0xff);
}

/* ---------------- socket ---------------- */

static int sock_send_all(int fd, const uint8_t *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t r = send(fd, buf + off, len - off, MSG_NOSIGNAL);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)r;
	}
	return 0;
}

/* 收满 want 字节；失败返回 -1 */
static int sock_recv_full(int fd, uint8_t *buf, size_t want)
{
	size_t off = 0;

	while (off < want) {
		ssize_t r = recv(fd, buf + off, want - off, 0);

		if (r <= 0)
			return -1;
		off += (size_t)r;
	}
	return 0;
}

static int sock_read_packet(int fd, uint8_t *hdr, uint8_t hdr_first,
			    uint8_t **body, size_t *body_len,
			    int timeout_sec)
{
	struct timeval tv;
	long remlen = 0;
	int mult = 1;
	int i;

	tv.tv_sec = timeout_sec;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	if (sock_recv_full(fd, hdr, 1) < 0 || hdr[0] != hdr_first)
		return -1;

	for (i = 0; i < 4; i++) {
		uint8_t b;

		if (sock_recv_full(fd, &b, 1) < 0)
			return -1;
		remlen += (long)(b & 0x7f) * mult;
		mult *= 128;
		if (!(b & 0x80))
			break;
	}
	*body = (uint8_t *)malloc(remlen ? (size_t)remlen : 1);
	if (!*body)
		return -1;
	if (remlen > 0 && sock_recv_full(fd, *body, (size_t)remlen) < 0) {
		free(*body);
		*body = NULL;
		return -1;
	}
	*body_len = (size_t)(remlen < 0 ? 0 : remlen);
	return 0;
}

/* ---------------- host resolve + connect ---------------- */

static int tcp_connect_timeout(const char *host, int port, int timeout_sec)
{
	struct addrinfo hints, *res = NULL, *ai;
	char portstr[8];
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(portstr, sizeof(portstr), "%d", port);

	if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
		return -1;

	for (ai = res; ai; ai = ai->ai_next) {
		struct timeval tv;
		int flags, err = 0;
		socklen_t elen = sizeof(err);

		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;

		flags = fcntl(fd, F_GETFL, 0);
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);

		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
			fcntl(fd, F_SETFL, flags); /* restore blocking */
			break;
		}
		if (errno != EINPROGRESS) {
			close(fd);
			fd = -1;
			continue;
		}

		tv.tv_sec = timeout_sec;
		tv.tv_usec = 0;
		{
			fd_set wf;

			FD_ZERO(&wf);
			FD_SET(fd, &wf);
			/* 必须把 fd 放进 writefds，否则 select 不监控任何
			 * 描述符、只会空等到超时，connect 永远判失败 */
			if (select(fd + 1, NULL, &wf, NULL, &tv) <= 0 ||
			    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 ||
			    err != 0) {
				close(fd);
				fd = -1;
				continue;
			}
		}
		fcntl(fd, F_SETFL, flags);
		break;
	}

	freeaddrinfo(res);
	return fd;
}

/* ---------------- public entry ---------------- */

mq_result_t mqtt_publish_once(const enc_cfg_t *c, const char *topic,
			      const char *payload)
{
	const char *pass = c->mqtt_pass[0] ? c->mqtt_pass : NULL;
	const char *user = c->mqtt_user[0] ? c->mqtt_user : NULL;
	char cid[96];
	size_t clen, tlen, plen, rl;
	static uint16_t s_pid = 1;
	uint16_t pid;
	uint8_t fixedhdr[5];
	uint8_t *buf, *body = NULL;
	size_t blen, off, bufsize;
	uint8_t rhdr[2];
	int fd;
	mq_result_t ret = MQ_ERR_INTERNAL;
	FILE *fp;

	if (!c->mqtt_host[0])
		return MQ_ERR_TCP; /* 未配置主机视同不可达 */

	fp = fopen("/etc/hostname", "r");
	cid[0] = '\0';
	if (fp) {
		char h[64];
		size_t n;

		n = fread(h, 1, sizeof(h) - 1, fp);
		fclose(fp);
		while (n > 0 && (h[n-1] == '\n' || h[n-1] == '\r'))
			h[--n] = '\0';
		if (h[0])
			snprintf(cid, sizeof(cid), "encalertd-%.40s", h);
	}
	if (!cid[0])
		snprintf(cid, sizeof(cid), "encalertd-%ld", (long)getpid());
	clen = strlen(cid);
	tlen = strlen(topic);
	plen = strlen(payload);
	pid = s_pid++;

	/* ---- CONNECT ---- */
	blen = 10 + 4 + clen + (user ? 2 + strlen(user) : 0) +
	       (pass ? 2 + strlen(pass) : 0);
	bufsize = blen + 5 + 128;
	buf = (uint8_t *)malloc(bufsize);
	if (!buf)
		return MQ_ERR_INTERNAL;

	off = 0;
	buf[off++] = 0x00; buf[off++] = 0x04;
	memcpy(buf + off, "MQTT", 4); off += 4;
	buf[off++] = 0x04;                              /* protocol level 3.1.1 */
	buf[off++] = (uint8_t)(0x02 |                 /* clean session      */
			       (user ? 0x80 : 0) |
			       (pass ? 0x40 : 0));
	put_u16(buf + off, KEEPALIVE_SEC); off += 2;
	put_u16(buf + off, (uint16_t)clen); off += 2;
	memcpy(buf + off, cid, clen); off += clen;
	if (user) {
		size_t l = strlen(user);
		put_u16(buf + off, (uint16_t)l); off += 2;
		memcpy(buf + off, user, l); off += l;
	}
	if (pass) {
		size_t l = strlen(pass);
		put_u16(buf + off, (uint16_t)l); off += 2;
		memcpy(buf + off, pass, l); off += l;
	}
	/* 固定头 = 类型字节 + varlen；CONNECT 类型为 0x10 */
	fixedhdr[0] = 0x10;
	rl = put_varlen(fixedhdr + 1, (long)off);
	/* 前部留 rl+1 字节余量后整体后移，再回填固定头 */
	memmove(buf + rl + 1, buf, off);
	memcpy(buf, fixedhdr, rl + 1);
	off += rl + 1;

	fd = tcp_connect_timeout(c->mqtt_host, c->mqtt_port,
				 CONNECT_TIMEOUT_SEC);
	if (fd < 0) {
		free(buf);
		return MQ_ERR_TCP;
	}

	if (sock_send_all(fd, buf, off) < 0)
		goto out_disconnect;
	if (sock_read_packet(fd, rhdr, 0x20, &body, &blen, ACK_TIMEOUT_SEC) < 0) {
		ret = MQ_ERR_CONNACK;
		goto out_disconnect;
	}
	if (blen < 2 || body[1] != 0) {
		ret = MQ_ERR_CONNACK;
		goto out_disconnect;
	}
	free(body);
	body = NULL;

	/* ---- PUBLISH QoS1 ---- */
	blen = 2 + tlen + 2 + plen;
	if (5 + blen > bufsize) {
		uint8_t *nb = realloc(buf, 5 + blen);
		if (!nb)
			goto out_disconnect;
		buf = nb;
	}
	off = 0;
	put_u16(buf, (uint16_t)tlen); off += 2;
	memcpy(buf + off, topic, tlen); off += tlen;
	put_u16(buf + off, pid); off += 2;
	memcpy(buf + off, payload, plen); off += plen;
	rl = put_varlen(fixedhdr + 1, (long)off);
	memmove(buf + rl + 1, buf, off);
	memcpy(buf, fixedhdr, rl + 1);
	/* PUBLISH QoS1, dup=0, retain=0；类型字节必须在 varlen 之前 */
	buf[0] = 0x32;
	off += rl + 1;

	if (sock_send_all(fd, buf, off) < 0) {
		ret = MQ_ERR_PUBACK;
		goto out_disconnect;
	}

	/* ---- wait PUBACK (固定只认匹配 pid 的 PUBACK 包) ---- */
	{
		mq_result_t ack = MQ_ERR_PUBACK;
		int tries;

		for (tries = 0; tries < 4; tries++) {
			uint8_t h2;

			body = NULL;
			if (sock_read_packet(fd, &h2, 0x40, &body, &blen,
					     tries == 0 ? ACK_TIMEOUT_SEC : 1) < 0) {
				ack = MQ_ERR_PUBACK;
				break;
			}
			if (blen >= 2 &&
			    ((body[0] << 8) | body[1]) == pid) {
				ack = MQ_OK;
				free(body);
				body = NULL; /* 防 out_disconnect 二次释放 */
				break;
			}
			free(body);
			body = NULL;
		}
		ret = ack;
	}

out_disconnect:
	{
		uint8_t dis[2] = { 0xE0, 0x00 };
		sock_send_all(fd, dis, 2); /* best effort */
	}
	if (fd >= 0)
		close(fd);
	free(body);
	free(buf);
	return ret;
}
