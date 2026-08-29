/*
 * mqtt.c — 常驻 MQTT 3.1.1 客户端（订阅 + 发布 + keepalive + 自动重连）
 *
 * 相对 encalertd mqtt.c（单次连接发布）的扩展（设计 EncoderMainDesign.md §4）：
 *   - 常驻线程：CONNECT(clean session) → SUBSCRIBE +/+/encoder/<id>/#
 *     → 事件循环（入方向 PUBLISH 投递 / 出方向 ACK 匹配 / PINGREQ）
 *   - 断线自动重连（指数退避 1s~30s），重连后重发 SUBSCRIBE 并重发未确认消息
 *   - QoS1/2 完整握手；persist=true 的消息先落 outbox，PUBACK/PUBCOMP 后删除
 *     （必达语义，复用 encalertd spool 思路；崩溃重启后 outbox 文件续传）
 *   - 线程安全发布接口 mq_publish()，重连成功回调 MQEV_CONNECTED
 *     （上传线程借此触发断网重传扫描）
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"

#define MQ_CONNECT_TIMEOUT_SEC	3
#define MQ_ACK_WAIT_MAX		30	/* 每轮 select 等待上限（秒），必须 < keepalive */
#define MQ_INBUF_CHUNK		4096
#define MQ_INBUF_MAX		(256 * 1024)

/* MQTT 固定头类型 */
#define MQ_PKT_CONNECT		0x10
#define MQ_PKT_CONNACK		0x20
#define MQ_PKT_PUBLISH		0x30
#define MQ_PKT_PUBACK		0x40
#define MQ_PKT_PUBREC		0x50
#define MQ_PKT_PUBREL		0x60
#define MQ_PKT_PUBCOMP		0x70
#define MQ_PKT_SUBSCRIBE	0x82
#define MQ_PKT_SUBACK		0x90
#define MQ_PKT_PINGREQ		0xC0
#define MQ_PKT_PINGRESP		0xD0
#define MQ_PKT_DISCONNECT	0xE0

/* 出方向在途消息 */
typedef struct mq_inflight {
	struct mq_inflight *next;
	uint16_t    pid;
	int         qos;
	int         state;          /* 0=等 PUBACK/PUBREC  1=已收 PUBREC 等 PUBCOMP */
	char       *topic;
	char       *payload;
	char        file[320];      /* outbox 落盘路径，空串=未持久化 */
} mq_inflight_t;

struct mq_client {
	enc_cfg_t   cfg;                    /* 拷贝，避免生命周期问题 */
	char        filter[160];            /* +/+/encoder/<id>/# */
	char        cid[96];
	pthread_t   thread;
	bool        thread_started;
	pthread_mutex_t mu;
	int         fd;                     /* -1 = 未连接 */
	volatile bool stopping;
	volatile bool connected;
	mq_msg_cb   msg_cb;
	mq_event_cb ev_cb;
	void       *ud;
	mq_inflight_t *inflight;            /* 等确认链表（发送序） */
	char        outbox_dir[280];
	uint16_t    next_pid;
	long long   outbox_seq;
	/* rx 流缓冲 */
	uint8_t    *inbuf;
	size_t      in_len, in_cap;
	time_t      last_peer_act;          /* 上次收发活动（keepalive 依据） */
};

/* ------------------------------------------------------------------ */
/* 编解码小工具                                                         */
/* ------------------------------------------------------------------ */

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
			fcntl(fd, F_SETFL, flags);
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

/* ------------------------------------------------------------------ */
/* 报文构造与发送                                                       */
/* ------------------------------------------------------------------ */

/* 组 type+varlen+body 并发送；成功返回 0 */
static int mq_send_packet(mq_client_t *m, uint8_t type,
			  const uint8_t *body, size_t blen)
{
	uint8_t hdr[5];
	size_t rl = put_varlen(hdr + 1, (long)blen);
	ssize_t rc;

	hdr[0] = type;
	pthread_mutex_lock(&m->mu);
	if (m->fd < 0) {
		pthread_mutex_unlock(&m->mu);
		return -1;
	}
	rc = (sock_send_all(m->fd, hdr, rl + 1) == 0 &&
	      (blen == 0 || sock_send_all(m->fd, body, blen) == 0)) ? 0 : -1;
	if (rc == 0)
		m->last_peer_act = time(NULL);
	pthread_mutex_unlock(&m->mu);
	return rc == 0 ? 0 : -1;
}

static int mq_send_connect(mq_client_t *m)
{
	const char *pass = m->cfg.mqtt_pass[0] ? m->cfg.mqtt_pass : NULL;
	const char *user = m->cfg.mqtt_user[0] ? m->cfg.mqtt_user : NULL;
	size_t clen = strlen(m->cid);
	sb_t b;
	uint8_t flags;
	int rc;

	sb_init(&b);
	/* variable header */
	sb_puts(&b, "\x00\x04MQTT");
	sb_putc(&b, (char)0x04);                    /* protocol level 3.1.1 */
	flags = 0x02 | (user ? 0x80 : 0) | (pass ? 0x40 : 0);
	sb_putc(&b, (char)flags);
	sb_putc(&b, (char)0x00);                    /* keepalive HI */
	sb_putc(&b, (char)60);                      /* keepalive=60s */
	sb_putc(&b, (char)(clen >> 8));
	sb_putc(&b, (char)(clen & 0xff));
	sb_puts(&b, m->cid);
	if (user) {
		size_t l = strlen(user);

		sb_putc(&b, (char)(l >> 8));
		sb_putc(&b, (char)(l & 0xff));
		sb_puts(&b, user);
	}
	if (pass) {
		size_t l = strlen(pass);

		sb_putc(&b, (char)(l >> 8));
		sb_putc(&b, (char)(l & 0xff));
		sb_puts(&b, pass);
	}
	rc = mq_send_packet(m, MQ_PKT_CONNECT,
			    (const uint8_t *)b.s, b.len);
	sb_free(&b);
	return rc;
}

/* SUBSCRIBE 固定头 0x82（已含 QoS1 位） */
static int mq_send_subscribe(mq_client_t *m, uint16_t pid)
{
	size_t tlen = strlen(m->filter);
	uint8_t body[512];
	size_t off = 0;
	size_t tl = tlen;

	if (5 + 2 + tl + 3 > sizeof(body))
		return -1;
	put_u16(body + off, pid); off += 2;
	put_u16(body + off, (uint16_t)tl); off += 2;
	memcpy(body + off, m->filter, tl); off += tl;
	body[off++] = (uint8_t)m->cfg.mqtt_qos;
	return mq_send_packet(m, MQ_PKT_SUBSCRIBE, body, off);
}

static int mq_send_publish(mq_client_t *m, const mq_inflight_t *inf)
{
	size_t tlen = strlen(inf->topic), plen = strlen(inf->payload);
	uint8_t *body;
	size_t off = 0, cap = 2 + tlen + 2 + plen;
	uint8_t type = (uint8_t)(MQ_PKT_PUBLISH | (inf->qos << 1));
	int rc;

	body = (uint8_t *)malloc(cap);
	if (!body)
		return -1;
	put_u16(body + off, (uint16_t)tlen); off += 2;
	memcpy(body + off, inf->topic, tlen); off += tlen;
	if (inf->qos > 0) {
		put_u16(body + off, inf->pid); off += 2;
	}
	memcpy(body + off, inf->payload, plen); off += plen;
	rc = mq_send_packet(m, type, body, off);
	free(body);
	return rc;
}

/* ------------------------------------------------------------------ */
/* in-flight / outbox 管理                                             */
/* ------------------------------------------------------------------ */

static void outbox_file_delete(mq_client_t *m, const char *file)
{
	if (file && file[0])
		unlink(file);
}

/* 完成（收到 ACK）：移链表 + 删 outbox 文件 + 释放 */
static void inflight_complete_locked(mq_client_t *m, uint16_t pid)
{
	mq_inflight_t **pp = &m->inflight;

	while (*pp) {
		mq_inflight_t *inf = *pp;

		if (inf->pid == pid) {
			*pp = inf->next;
			outbox_file_delete(m, inf->file);
			free(inf->topic);
			free(inf->payload);
			free(inf);
			return;
		}
		pp = &inf->next;
	}
}

/* 新建 in-flight（必要时落 outbox） */
static mq_inflight_t *inflight_new(mq_client_t *m, const char *topic,
				   const char *payload, bool persist)
{
	mq_inflight_t *inf = (mq_inflight_t *)calloc(1, sizeof(*inf));
	sb_t b;

	if (!inf)
		return NULL;
	inf->qos = m->cfg.mqtt_qos > 0 ? m->cfg.mqtt_qos : 0;
	inf->topic = strdup(topic);
	inf->payload = strdup(payload);
	if (!inf->topic || !inf->payload) {
		free(inf->topic);
		free(inf->payload);
		free(inf);
		return NULL;
	}
	if (inf->qos > 0 && persist) {
		FILE *fp;

		snprintf(inf->file, sizeof(inf->file), "%s/%010lld.msg",
			 m->outbox_dir, ++m->outbox_seq);
		fp = fopen(inf->file, "w");
		if (fp) {
			fprintf(fp, "%s\n%s", topic, payload);
			fclose(fp);
		} else {
			inf->file[0] = '\0';    /* 落盘失败退化为内存重发 */
		}
	}
	inf->pid = m->next_pid ? m->next_pid : 1;
	m->next_pid = (uint16_t)(m->next_pid + 1);
	(void)b;
	return inf;
}

/* 启动时把 outbox 目录残留文件装回内存（崩溃恢复，按文件名序） */
static void outbox_load(mq_client_t *m)
{
	DIR *d = opendir(m->outbox_dir);
	struct dirent *de;

	if (!d)
		return;
	while ((de = readdir(d)) != NULL) {
		char path[512];
		FILE *fp;
		char topic[256];
		sb_t payload;
		int c;
		bool have_topic = false;

		if (de->d_name[0] == '.' ||
		    strlen(de->d_name) < 5 ||
		    strcmp(de->d_name + strlen(de->d_name) - 4, ".msg"))
			continue;
		snprintf(path, sizeof(path), "%s/%s", m->outbox_dir,
			 de->d_name);
		fp = fopen(path, "r");
		if (!fp)
			continue;
		/* 首行 topic，其余整体为 payload */
		{
			size_t ti = 0;

			while ((c = fgetc(fp)) != EOF && c != '\n' &&
			       ti + 1 < sizeof(topic))
				topic[ti++] = (char)c;
			topic[ti] = '\0';
			have_topic = ti > 0;
		}
		sb_init(&payload);
		while ((c = fgetc(fp)) != EOF)
			sb_putc(&payload, (char)c);
		fclose(fp);
		if (have_topic && payload.ok && payload.len > 0) {
			mq_inflight_t *inf =
				(mq_inflight_t *)calloc(1, sizeof(*inf));
			char *base = de->d_name;

			if (inf) {
				inf->qos = m->cfg.mqtt_qos > 0 ?
					   m->cfg.mqtt_qos : 0;
				inf->topic = strdup(topic);
				inf->payload = strdup(payload.s);
				snprintf(inf->file, sizeof(inf->file), "%s",
					 path);
				inf->pid = m->next_pid ? m->next_pid : 1;
				m->next_pid = (uint16_t)(m->next_pid + 1);
				{
					long long seq = strtoll(base, NULL, 10);

					if (seq >= m->outbox_seq)
						m->outbox_seq = seq;
				}
				/* 追加到链表尾，保持发送序 */
				{
					mq_inflight_t **pp = &m->inflight;

					while (*pp)
						pp = &(*pp)->next;
					*pp = inf;
				}
				if (!inf->topic || !inf->payload) {
					/* 分配失败则放弃该文件（下轮重启再试） */
					;
				}
			}
		} else {
			unlink(path);   /* 空/损坏文件直接清掉 */
		}
		sb_free(&payload);
	}
	closedir(d);
}

/* 重连后按序重发全部未确认消息 */
static void inflight_flush(mq_client_t *m)
{
	mq_inflight_t *inf;

	/* 实际重发在锁外逐条执行，避免长时间持锁 */
	for (inf = m->inflight; inf; inf = inf->next) {
		if (mq_send_publish(m, inf) != 0)
			break;      /* 发送失败 → 下轮重连再试 */
		inf->state = 0;     /* 重新进入等 ACK 态 */
	}
}

/* ------------------------------------------------------------------ */
/* 入方向报文处理                                                       */
/* ------------------------------------------------------------------ */

static void handle_publish(mq_client_t *m, uint8_t hdr,
			   const uint8_t *body, size_t blen)
{
	uint16_t tlen;
	size_t off = 0;
	char topic[256];
	char payload[4096];
	int qos = (hdr >> 1) & 0x03;
	uint16_t pid = 0;

	if (blen < 2)
		return;
	tlen = (uint16_t)((body[0] << 8) | body[1]);
	off = 2;
	if (tlen >= sizeof(topic) || off + tlen > blen)
		return;
	memcpy(topic, body + off, tlen);
	topic[tlen] = '\0';
	off += tlen;
	if (qos > 0) {
		if (off + 2 > blen)
			return;
		pid = (uint16_t)((body[off] << 8) | body[off + 1]);
		off += 2;
	}
	{
		size_t plen = blen - off;

		if (plen >= sizeof(payload))
			plen = sizeof(payload) - 1;
		memcpy(payload, body + off, plen);
		payload[plen] = '\0';
	}

	if (qos == 1) {
		uint8_t ab[2];

		put_u16(ab, pid);
		(void)mq_send_packet(m, MQ_PKT_PUBACK, ab, 2);
	} else if (qos == 2) {
		uint8_t ab[2];

		put_u16(ab, pid);
		(void)mq_send_packet(m, MQ_PKT_PUBREC, ab, 2);
		/* PUBREL 到达时回 PUBCOMP（见 handle_pubrel） */
	}
	if (m->msg_cb)
		m->msg_cb(m->ud, topic, payload);
}

static void handle_pubrel(mq_client_t *m, const uint8_t *body, size_t blen)
{
	uint8_t ab[2];

	if (blen < 2)
		return;
	memcpy(ab, body, 2);
	(void)mq_send_packet(m, MQ_PKT_PUBCOMP, ab, 2);
}

/* 从流缓冲解析并处理所有完整报文；返回 -1 表示协议错误需断线 */
static int parse_packets(mq_client_t *m)
{
	for (;;) {
		size_t avail = m->in_len;
		long remlen = 0;
		int mult = 1, i;
		size_t hdr_len, need, body_off;
		uint8_t type;

		if (avail < 2)
			return 0;
		/* 解剩余长度（varlen） */
		remlen = 0;
		mult = 1;
		hdr_len = 1;
		for (i = 0; i < 4; i++) {
			uint8_t b;

			if (hdr_len >= avail)
				return 0;   /* varlen 未到齐 */
			b = m->inbuf[hdr_len++];
			remlen += (long)(b & 0x7f) * mult;
			mult *= 128;
			if (!(b & 0x80))
				break;
		}
		need = hdr_len + (size_t)remlen;
		if (need > avail)
			return 0;           /* 包体未到齐 */
		type = m->inbuf[0] & 0xf0;
		body_off = hdr_len;

		switch (type) {
		case MQ_PKT_PUBLISH:
			handle_publish(m, m->inbuf[0],
				       m->inbuf + body_off,
				       (size_t)remlen);
			break;
		case MQ_PKT_PUBACK:
		case MQ_PKT_PUBCOMP:
			if (remlen >= 2) {
				uint16_t pid = (uint16_t)((m->inbuf[body_off] << 8) |
							  m->inbuf[body_off + 1]);

				pthread_mutex_lock(&m->mu);
				inflight_complete_locked(m, pid);
				pthread_mutex_unlock(&m->mu);
			}
			break;
		case MQ_PKT_PUBREC:
			if (remlen >= 2) {
				uint16_t pid = (uint16_t)((m->inbuf[body_off] << 8) |
							  m->inbuf[body_off + 1]);
				uint8_t ab[2];
				mq_inflight_t *inf;

				pthread_mutex_lock(&m->mu);
				for (inf = m->inflight; inf; inf = inf->next) {
					if (inf->pid == pid) {
						inf->state = 1;
						break;
					}
				}
				pthread_mutex_unlock(&m->mu);
				put_u16(ab, pid);
				(void)mq_send_packet(m, MQ_PKT_PUBREL, ab, 2);
			}
			break;
		case MQ_PKT_PUBREL:
			handle_pubrel(m, m->inbuf + body_off, (size_t)remlen);
			break;
		case MQ_PKT_PINGRESP:
			break;
		case MQ_PKT_CONNACK:
			/* 事件循环外不应再收到；忽略 */
			break;
		default:
			return -1;          /* 未知类型，安全起见重连 */
		}
		/* 消费已处理字节 */
		memmove(m->inbuf, m->inbuf + need, m->in_len - need);
		m->in_len -= need;
	}
}

/* ------------------------------------------------------------------ */
/* 连接与事件循环                                                       */
/* ------------------------------------------------------------------ */

static void mq_fire_event(mq_client_t *m, mq_event_t ev)
{
	if (m->ev_cb)
		m->ev_cb(m->ud, ev);
}

static void mq_close_conn(mq_client_t *m)
{
	pthread_mutex_lock(&m->mu);
	if (m->fd >= 0) {
		close(m->fd);
		m->fd = -1;
	}
	pthread_mutex_unlock(&m->mu);
}

/* 一次完整会话（连接成功到断线），返回前保证 fd 已关闭 */
static void mq_session(mq_client_t *m)
{
	uint16_t sub_pid = m->next_pid ? m->next_pid : 1;
	int i;

	m->next_pid = (uint16_t)(m->next_pid + 1);
	m->in_len = 0;
	m->last_peer_act = time(NULL);

	if (mq_send_connect(m) != 0) {
		log_msg(ENCM_LOG_WARN, "mqtt session: CONNECT send failed");
		return;
	}
	/* 等 CONNACK（同步，短超时） */
	for (i = 0; i < MQ_ACK_WAIT_MAX; i++) {
		struct timeval tv = { 1, 0 };
		fd_set rf;
		ssize_t n;

		FD_ZERO(&rf);
		FD_SET(m->fd, &rf);
		if (select(m->fd + 1, &rf, NULL, NULL, &tv) <= 0)
			continue;
		if (m->in_cap - m->in_len < MQ_INBUF_CHUNK) {
			uint8_t *nb = (uint8_t *)realloc(m->inbuf, m->in_cap + MQ_INBUF_CHUNK);

			if (!nb)
				return;
			m->inbuf = nb;
			m->in_cap += MQ_INBUF_CHUNK;
		}
		n = recv(m->fd, m->inbuf + m->in_len,
			 MQ_INBUF_CHUNK, 0);
		if (n <= 0) {
			log_msg(ENCM_LOG_WARN,
				"mqtt session: closed during CONNACK (n=%zd errno=%d)",
				n, errno);
			return;
		}
		m->in_len += (size_t)n;
		m->last_peer_act = time(NULL);
		if (m->in_len >= 4 &&
		    m->inbuf[0] == MQ_PKT_CONNACK) {
			long remlen = m->inbuf[1];

			if ((int)m->in_len < 2 + remlen)
				continue;
			if (remlen < 2 || m->inbuf[3] != 0) {
				log_msg(ENCM_LOG_WARN,
					"mqtt session: CONNACK rejected rc=%d (0=ok 2=id 4=cred 5=auth)",
					remlen >= 2 ? m->inbuf[3] : -1);
				return;     /* broker 拒绝 */
			}
			break;
		}
	}
	if (i >= MQ_ACK_WAIT_MAX) {
		log_msg(ENCM_LOG_WARN,
			"mqtt session: CONNACK timeout after %ds", i);
		return;
	}

	/* SUBSCRIBE */
	if (mq_send_subscribe(m, sub_pid) != 0) {
		log_msg(ENCM_LOG_WARN, "mqtt session: SUBSCRIBE send failed");
		return;
	}
	for (i = 0; i < MQ_ACK_WAIT_MAX; i++) {
		struct timeval tv = { 1, 0 };
		fd_set rf;
		ssize_t n;

		FD_ZERO(&rf);
		FD_SET(m->fd, &rf);
		if (select(m->fd + 1, &rf, NULL, NULL, &tv) <= 0)
			continue;
		if (m->in_cap - m->in_len < MQ_INBUF_CHUNK) {
			uint8_t *nb = (uint8_t *)realloc(m->inbuf, m->in_cap + MQ_INBUF_CHUNK);

			if (!nb)
				return;
			m->inbuf = nb;
			m->in_cap += MQ_INBUF_CHUNK;
		}
		n = recv(m->fd, m->inbuf + m->in_len, MQ_INBUF_CHUNK, 0);
		if (n <= 0) {
			log_msg(ENCM_LOG_WARN,
				"mqtt session: closed during SUBACK (n=%zd)",
				n);
			return;
		}
		m->in_len += (size_t)n;
		m->last_peer_act = time(NULL);
		if (m->in_len >= 5 && m->inbuf[0] == MQ_PKT_SUBACK) {
			long remlen = m->inbuf[1];

			if ((int)m->in_len < 2 + remlen)
				continue;
			{
				uint16_t pid = (uint16_t)((m->inbuf[2] << 8) |
							  m->inbuf[3]);
				uint8_t rc = m->inbuf[4];

				if (pid != sub_pid || rc >= 0x80) {
					log_msg(ENCM_LOG_WARN,
						"mqtt session: SUBACK rejected pid=%u rc=%d",
						pid, rc);
					return; /* 订阅失败 → 重连 */
				}
			}
			/* 消费 SUBACK */
			memmove(m->inbuf, m->inbuf + 2 + remlen,
				m->in_len - (size_t)(2 + remlen));
			m->in_len -= (size_t)(2 + remlen);
			break;
		}
	}
	if (i >= MQ_ACK_WAIT_MAX) {
		log_msg(ENCM_LOG_WARN,
			"mqtt session: SUBACK timeout after %ds", i);
		return;
	}

	/* 连接就绪 */
	m->connected = true;
	inflight_flush(m);
	mq_fire_event(m, MQEV_CONNECTED);
	log_msg(ENCM_LOG_INFO, "mqtt connected host=%s:%d filter=%s",
		m->cfg.mqtt_host, m->cfg.mqtt_port, m->filter);

	/* 事件循环 */
	while (!m->stopping) {
		struct timeval tv = { 1, 0 };
		fd_set rf;
		int r;

		FD_ZERO(&rf);
		FD_SET(m->fd, &rf);
		r = select(m->fd + 1, &rf, NULL, NULL, &tv);
		if (m->stopping)
			break;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r > 0) {
			ssize_t n;

			if (m->in_cap - m->in_len < MQ_INBUF_CHUNK) {
				uint8_t *nb = (uint8_t *)realloc(m->inbuf,
						m->in_cap + MQ_INBUF_CHUNK);

				if (!nb)
					break;
				m->inbuf = nb;
				m->in_cap += MQ_INBUF_CHUNK;
			}
			if (m->in_len >= MQ_INBUF_MAX)
				break;      /* 异常膨胀，重连 */
			n = recv(m->fd, m->inbuf + m->in_len,
				 m->in_cap - m->in_len, 0);
			if (n <= 0)
				break;
			m->in_len += (size_t)n;
			m->last_peer_act = time(NULL);
			if (parse_packets(m) < 0)
				break;
		}
		/* keepalive：60s 无活动发 PINGREQ；90s 无响应判死 */
		if (time(NULL) - m->last_peer_act > 90)
			break;
		if (time(NULL) - m->last_peer_act > 30) {
			if (mq_send_packet(m, MQ_PKT_PINGREQ, NULL, 0) != 0)
				break;
		}
	}
	m->connected = false;
	mq_fire_event(m, MQEV_DISCONNECTED);
	log_msg(ENCM_LOG_WARN, "mqtt disconnected");
	mq_close_conn(m);
}

static void *mq_thread_main(void *arg)
{
	mq_client_t *m = (mq_client_t *)arg;
	int backoff = 1;
	bool was_connected = false;

	while (!m->stopping) {
		int fd = tcp_connect_timeout(m->cfg.mqtt_host,
					     m->cfg.mqtt_port,
					     MQ_CONNECT_TIMEOUT_SEC);

		if (m->stopping)
			break;
		if (fd < 0) {
			if (was_connected) {
				was_connected = false;
			}
			log_msg(ENCM_LOG_WARN,
				"mqtt connect failed host=%s:%d errno=%d(%s) retry_in=%ds",
				m->cfg.mqtt_host, m->cfg.mqtt_port,
				errno, strerror(errno), backoff);
			sleep((unsigned int)backoff);
			backoff = backoff * 2 > 30 ? 30 : backoff * 2;
			continue;
		}
		backoff = 1;
		pthread_mutex_lock(&m->mu);
		m->fd = fd;
		pthread_mutex_unlock(&m->mu);
		{
			int one = 1;

			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one,
				   sizeof(one));
		}
		mq_session(m);
		was_connected = true;
		/* session 返回后 fd 必已关闭；兜底 */
		mq_close_conn(m);
		if (m->stopping)
			break;
		sleep((unsigned int)backoff);
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* 公共接口                                                             */
/* ------------------------------------------------------------------ */

mq_client_t *mq_start(const enc_cfg_t *c, mq_msg_cb msg_cb,
		      mq_event_cb ev_cb, void *ud)
{
	mq_client_t *m = (mq_client_t *)calloc(1, sizeof(*m));
	FILE *fp;

	if (!m)
		return NULL;
	m->cfg = *c;
	m->msg_cb = msg_cb;
	m->ev_cb = ev_cb;
	m->ud = ud;
	m->fd = -1;
	m->next_pid = 1;
	pthread_mutex_init(&m->mu, NULL);
	snprintf(m->filter, sizeof(m->filter), "+/+/encoder/%s/#",
		 device_id_get(c));
	fp = fopen("/etc/hostname", "r");
	if (fp) {
		char h[64];
		size_t n = fread(h, 1, sizeof(h) - 1, fp);

		fclose(fp);
		while (n > 0 && (h[n - 1] == '\n' || h[n - 1] == '\r'))
			h[--n] = '\0';
		if (h[0])
			snprintf(m->cid, sizeof(m->cid), "encmain-%.40s", h);
	}
	if (!m->cid[0])
		snprintf(m->cid, sizeof(m->cid), "encmain-%ld",
			 (long)getpid());
	snprintf(m->outbox_dir, sizeof(m->outbox_dir), "%s", c->outbox_dir);
	dir_ensure(m->outbox_dir);
	outbox_load(m);
	m->inbuf = (uint8_t *)malloc(MQ_INBUF_CHUNK);
	if (!m->inbuf) {
		free(m);
		return NULL;
	}
	m->in_cap = MQ_INBUF_CHUNK;
	m->in_len = 0;
	{
		size_t ob = 0;
		mq_inflight_t *p;

		for (p = m->inflight; p; p = p->next)
			ob++;
		log_msg(ENCM_LOG_INFO,
			"mqtt client start host=%s:%d user='%s' pass_len=%zu filter=%s cid=%s outbox=%zu",
			m->cfg.mqtt_host, m->cfg.mqtt_port, m->cfg.mqtt_user,
			strlen(m->cfg.mqtt_pass), m->filter, m->cid, ob);
	}
	if (pthread_create(&m->thread, NULL, mq_thread_main, m) != 0) {
		free(m->inbuf);
		free(m);
		return NULL;
	}
	m->thread_started = true;
	return m;
}

int mq_publish(mq_client_t *m, const char *topic, const char *payload,
	       bool persist)
{
	mq_inflight_t *inf;
	int rc = 0;

	if (!m || m->stopping || !topic || !payload)
		return -1;

	pthread_mutex_lock(&m->mu);
	inf = inflight_new(m, topic, payload, persist);
	if (!inf) {
		pthread_mutex_unlock(&m->mu);
		return -1;
	}
	if (inf->qos == 0) {
		/* QoS0 即发即忘 */
		if (m->fd >= 0)
			rc = mq_send_publish(m, inf) == 0 ? 0 : -1;
		else
			rc = -1;
		free(inf->topic);
		free(inf->payload);
		free(inf);
		pthread_mutex_unlock(&m->mu);
		return rc;
	}
	/* QoS1/2：入链表（发送序） */
	{
		mq_inflight_t **pp = &m->inflight;

		while (*pp)
			pp = &(*pp)->next;
		*pp = inf;
	}
	if (m->fd >= 0) {
		if (mq_send_publish(m, inf) != 0)
			rc = 0;     /* 发送失败留在链表，重连后补发 */
	} else if (!persist && inf->file[0] == '\0') {
		rc = 0;             /* 未连接：已入队，重连后补发 */
	}
	pthread_mutex_unlock(&m->mu);
	return rc;
}

void mq_stop(mq_client_t *m)
{
	if (!m)
		return;
	m->stopping = true;
	pthread_mutex_lock(&m->mu);
	if (m->fd >= 0) {
		uint8_t dis[2] = { MQ_PKT_DISCONNECT, 0x00 };

		(void)sock_send_all(m->fd, dis, 2);
		shutdown(m->fd, SHUT_RDWR);
	}
	pthread_mutex_unlock(&m->mu);
	if (m->thread_started)
		pthread_join(m->thread, NULL);
	mq_close_conn(m);
	/* in-flight 不释放：outbox 文件保留，下次启动续传 */
	pthread_mutex_destroy(&m->mu);
	free(m->inbuf);
	free(m);
}
