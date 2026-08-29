/*
 * http_client.c — 极简 HTTP GET/POST 客户端（仅 HTTP/1.1，无 TLS）
 *
 * 用途：Majestic 本机接口（127.0.0.1:80）——/play_audio 就绪探测、PCM 上传、
 *       /image.jpg 抓拍探测。语义对齐 bash 版 curl 调用：
 *   - GET：读状态行 + 响应体；响应体结束方式支持 Content-Length 与连接关闭
 *     （请求固定带 Connection: close，与 curl 默认行为一致）两种；
 *   - POST：文件整块读入后发送，Content-Type: application/octet-stream，
 *     Content-Length 为文件字节数；
 *   - body 一律截断到调用方 osz。
 * 返回值约定：0 = 收到合法 HTTP 响应（status_code 已填充）；-1 = 连接/协议失败。
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
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

/* 响应缓冲上限：正常只有几 KB～几百 KB，超限按截断处理，防异常响应撑爆内存 */
#define HTTP_BUF_MAX (8u * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* TCP 连接                                                            */
/* ------------------------------------------------------------------ */

/* 非阻塞 connect + select 实现连接超时（等价 curl --connect-timeout），
 * 成功返回已连接 fd（恢复阻塞并设置收发超时），失败 -1 */
static int http_tcp_connect(const char *host, int port, int timeout_sec)
{
	char port_str[16];
	struct addrinfo hints, *res = NULL, *ai;
	struct timeval tv;
	int fd = -1;

	snprintf(port_str, sizeof(port_str), "%d", port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
		return -1;

	for (ai = res; ai; ai = ai->ai_next) {
		fd_set wset;
		int err = 0;
		socklen_t elen = sizeof(err);

		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		if (errno != EINPROGRESS) {
			close(fd);
			fd = -1;
			continue;
		}
		FD_ZERO(&wset);
		FD_SET(fd, &wset);
		tv.tv_sec = timeout_sec > 0 ? timeout_sec : 3;
		tv.tv_usec = 0;
		if (select(fd + 1, NULL, &wset, NULL, &tv) <= 0) {
			close(fd);
			fd = -1;
			continue;
		}
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
			close(fd);
			fd = -1;
			continue;
		}
		break;
	}
	freeaddrinfo(res);

	if (fd >= 0) {
		/* 连接成功后恢复阻塞模式，用 SO_RCVTIMEO/SO_SNDTIMEO 控制读写超时 */
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
		tv.tv_sec = timeout_sec > 0 ? timeout_sec : 10;
		tv.tv_usec = 0;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}
	return fd;
}

/* ------------------------------------------------------------------ */
/* 请求发送 / 响应接收                                                  */
/* ------------------------------------------------------------------ */

/* 循环写满整个缓冲，处理 EINTR 与部分写 */
static int http_write_all(int fd, const char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = send(fd, buf + off, len - off, 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

/* 从响应头里提取 Content-Length（大小写不敏感）；无该头返回 -1 */
static long long http_content_length(const char *hdr, size_t hdr_len)
{
	const char *p = hdr;
	const char *end = hdr + hdr_len;

	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));

		if (!eol)
			eol = end;
		if ((size_t)(eol - p) > 15 &&
		    strncasecmp(p, "content-length:", 15) == 0) {
			const char *v = p + 15;
			char tmp[32];
			size_t n = 0;

			while (v < eol && n + 1 < sizeof(tmp) &&
			       (*v == ' ' || *v == '\t'))
				v++;
			while (v < eol && n + 1 < sizeof(tmp) &&
			       isdigit((unsigned char)*v))
				tmp[n++] = *v++;
			tmp[n] = '\0';
			if (n > 0)
				return strtoll(tmp, NULL, 10);
			return -1;
		}
		p = eol + 1;
	}
	return -1;
}

/* 发送请求并接收完整响应。结束方式：
 *   - 头部带 Content-Length：收满 CL 字节即止（不足视为协议失败）；
 *   - 无 Content-Length：收到对端关闭（EOF）为止（连接关闭语义）。
 * 成功返回 malloc 缓冲（调用方 free），总长写 *out_len；失败 NULL。 */
static char *http_exchange(int fd, const char *req, size_t reqlen,
			   size_t *out_len)
{
	size_t cap = 16384, len = 0;
	size_t body_start = 0;
	long long cl = -1;
	bool hdr_parsed = false;
	char *buf = (char *)malloc(cap);

	*out_len = 0;
	if (!buf)
		return NULL;
	if (http_write_all(fd, req, reqlen) != 0) {
		free(buf);
		return NULL;
	}

	for (;;) {
		ssize_t n;

		if (len + 4096 + 1 > cap) {
			char *nb;
			size_t ncap = cap * 2;

			if (cap >= HTTP_BUF_MAX)
				break;			/* 上限：丢弃后续数据 */
			if (ncap > HTTP_BUF_MAX)
				ncap = HTTP_BUF_MAX;
			nb = (char *)realloc(buf, ncap);
			if (!nb)
				break;
			buf = nb;
			cap = ncap;
		}
		n = recv(fd, buf + len, cap - len - 1, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;				/* 超时/错误：按已收数据处理 */
		}
		if (n == 0)
			break;				/* 对端关闭 */
		len += (size_t)n;
		buf[len] = '\0';

		if (!hdr_parsed) {
			char *hend = memmem(buf, len, "\r\n\r\n", 4);

			if (hend) {
				hdr_parsed = true;
				body_start = (size_t)(hend - buf) + 4;
				cl = http_content_length(buf, body_start);
				if (cl >= 0 &&
				    (long long)(len - body_start) >= cl)
					break;		/* CL 模式收满 */
			}
		} else if (cl >= 0 &&
			   (long long)(len - body_start) >= cl) {
			break;				/* CL 模式收满 */
		}
	}

	if (len == 0 || !hdr_parsed) {
		free(buf);
		return NULL;
	}
	/* CL 模式下响应体不足：视为被截断的非法响应 */
	if (cl >= 0 && (long long)(len - body_start) < cl) {
		free(buf);
		return NULL;
	}
	buf[len] = '\0';
	*out_len = len;
	return buf;
}

/* 从状态行 "HTTP/1.1 200 OK" 提取状态码；失败 -1 */
static int http_status_code(const char *buf)
{
	const char *p = strstr(buf, " ");
	int code;

	if (!p || strncmp(buf, "HTTP/", 5) != 0)
		return -1;
	while (*p == ' ')
		p++;
	code = atoi(p);
	return code > 0 ? code : -1;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

int http_get(const char *host, int port, const char *path,
	     int timeout_sec, char *out, size_t osz, int *status_code)
{
	char req[1024];
	char *resp = NULL;
	size_t resp_len = 0;
	const char *hend;
	size_t body_start, body_len;
	long long cl;
	int fd, code, rc = -1;

	if (!host || !path || !out || osz == 0)
		return -1;
	out[0] = '\0';
	if (status_code)
		*status_code = 0;
	if (timeout_sec <= 0)
		timeout_sec = 10;

	snprintf(req, sizeof(req),
		 "GET %s HTTP/1.1\r\n"
		 "Host: %s:%d\r\n"
		 "Connection: close\r\n"
		 "User-Agent: encodermain/%s\r\n"
		 "\r\n",
		 path, host, port, ENCM_VERSION);

	fd = http_tcp_connect(host, port, timeout_sec);
	if (fd < 0)
		return -1;
	resp = http_exchange(fd, req, strlen(req), &resp_len);
	close(fd);
	if (!resp)
		return -1;

	code = http_status_code(resp);
	hend = memmem(resp, resp_len, "\r\n\r\n", 4);
	if (code < 0 || !hend)
		goto out;
	body_start = (size_t)(hend - resp) + 4;
	body_len = resp_len - body_start;
	cl = http_content_length(resp, body_start);
	if (cl >= 0 && (size_t)cl < body_len)
		body_len = (size_t)cl;			/* CL 收满后可能带尾随数据 */

	if (body_len > osz - 1)
		body_len = osz - 1;			/* body 截断到 osz */
	memcpy(out, resp + body_start, body_len);
	out[body_len] = '\0';
	if (status_code)
		*status_code = code;
	rc = 0;
out:
	free(resp);
	return rc;
}

int http_post_file(const char *host, int port, const char *path,
		   const char *filepath, int timeout_sec, int *status_code)
{
	FILE *f;
	struct stat st;
	char *data = NULL;
	long long fsize;
	char hdr[512];
	char *resp = NULL;
	size_t resp_len = 0;
	int fd, code;

	if (!host || !path || !filepath)
		return -1;
	if (status_code)
		*status_code = 0;
	if (timeout_sec <= 0)
		timeout_sec = 10;

	f = fopen(filepath, "rb");
	if (!f)
		return -1;
	if (fstat(fileno(f), &st) != 0 || st.st_size < 0) {
		fclose(f);
		return -1;
	}
	fsize = (long long)st.st_size;
	data = (char *)malloc(fsize > 0 ? (size_t)fsize : 1);
	if (!data) {
		fclose(f);
		return -1;
	}
	if (fsize > 0 && fread(data, 1, (size_t)fsize, f) != (size_t)fsize) {
		free(data);
		fclose(f);
		return -1;
	}
	fclose(f);

	snprintf(hdr, sizeof(hdr),
		 "POST %s HTTP/1.1\r\n"
		 "Host: %s:%d\r\n"
		 "Content-Type: application/octet-stream\r\n"
		 "Content-Length: %lld\r\n"
		 "Connection: close\r\n"
		 "User-Agent: encodermain/%s\r\n"
		 "\r\n",
		 path, host, port, fsize, ENCM_VERSION);

	fd = http_tcp_connect(host, port, timeout_sec);
	if (fd < 0) {
		free(data);
		return -1;
	}
	/* 先发头再发文件体，避免再拼一块大缓冲 */
	if (http_write_all(fd, hdr, strlen(hdr)) == 0 &&
	    (fsize == 0 || http_write_all(fd, data, (size_t)fsize) == 0))
		resp = http_exchange(fd, hdr, 0, &resp_len);	/* 只收不发 */
	close(fd);
	free(data);
	if (!resp)
		return -1;

	code = http_status_code(resp);
	free(resp);
	if (code < 0)
		return -1;
	if (status_code)
		*status_code = code;
	return 0;
}
