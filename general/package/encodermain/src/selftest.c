/*
 * selftest.c — encodermain -t 自检（横幅 + 逐项结论行，无 FAIL 返回 0）
 *
 * 每行格式："[PASS]/[FAIL]/[DISABLED] <名称>: <详情>"
 * DISABLED 用于"功能未使能"场景（语音/抓拍 HTTP 探测失败、I2C 电量计缺失），
 * 不计入失败；返回 0 当且仅当无 FAIL。
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"

static int g_st_fail;

static void st_pass(const char *name, const char *fmt, ...)
{
	va_list ap;

	printf("[PASS] %s: ", name);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

static void st_fail_line(const char *name, const char *fmt, ...)
{
	va_list ap;

	g_st_fail++;
	printf("[FAIL] %s: ", name);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

static void st_disabled(const char *name, const char *fmt, ...)
{
	va_list ap;

	printf("[DISABLED] %s: ", name);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

/* ------------------------------------------------------------------ */
/* 检查项辅助                                                           */
/* ------------------------------------------------------------------ */

/* 目录存在且可写（dir_ensure + 探针文件写入删除） */
static void st_check_dir_writable(const char *name, const char *dir)
{
	char probe[512];

	snprintf(probe, sizeof(probe), "%s/.selftest.tmp", dir);
	if (dir_ensure(dir) == 0 && write_str_file(probe, "ok") &&
	    unlink(probe) == 0)
		st_pass(name, "%s 可写", dir);
	else
		st_fail_line(name, "%s 不可写", dir);
}

/* device_id 合法性：非空且仅 A-Za-z0-9._-（对齐 is_valid_device_id） */
static bool st_device_id_valid(const char *id)
{
	const char *p;

	if (!id || !id[0])
		return false;
	for (p = id; *p; p++) {
		unsigned char ch = (unsigned char)*p;

		if (!isalnum(ch) && ch != '.' && ch != '_' && ch != '-')
			return false;
	}
	return true;
}

/* MQTT broker TCP connect 探测：连上即关，不发任何业务报文 */
static bool st_mqtt_tcp_probe(const char *host, int port, int timeout_sec)
{
	char port_str[16];
	struct addrinfo hints, *res = NULL, *ai;
	int fd = -1;
	bool ok = false;

	snprintf(port_str, sizeof(port_str), "%d", port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
		return false;
	for (ai = res; ai; ai = ai->ai_next) {
		struct timeval tv;

		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		tv.tv_sec = timeout_sec;
		tv.tv_usec = 0;
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
			ok = true;
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd >= 0)
		close(fd);
	return ok;
}

/* Majestic HTTP 探测：200 → PASS；其余一律 DISABLED（功能可能未使能） */
static void st_check_http(const char *name, const char *path,
			  const char *feature, const enc_cfg_t *c)
{
	char body[128];
	int status = 0;
	int timeout = c->http_max_time_sec > 0 ? c->http_max_time_sec : 10;

	if (http_get("127.0.0.1", 80, path, timeout, body, sizeof(body),
		     &status) == 0 && status == 200)
		st_pass(name, "GET %s -> 200", path);
	else
		st_disabled(name, "GET %s 未就绪（%s 可能未使能）", path,
			    feature);
}

/* 去掉尾部的空白（which 输出） */
static void st_trim(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
			 s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = '\0';
}

/* ------------------------------------------------------------------ */
/* 主入口                                                               */
/* ------------------------------------------------------------------ */

int selftest_run(const enc_cfg_t *c)
{
	char buf[512];
	char out[256];
	const char *dev;

	printf("===== encodermain selftest v%s =====\n", ENCM_VERSION);

	/* 1. device_id 解析 */
	dev = device_id_get(c);
	if (st_device_id_valid(dev))
		st_pass("device_id", "id=%s", dev);
	else
		st_fail_line("device_id", "缺失或含非法字符（允许 A-Za-z0-9._-）");

	/* 2/3. state / runtime 目录可写 */
	st_check_dir_writable("state_dir", c->state_dir);
	st_check_dir_writable("runtime_dir", c->runtime_dir);

	/* 4. config.sh 继承解析：打印 mqtt 端点，凭据打码 */
	if (c->mqtt_host[0] && c->mqtt_port > 0)
		st_pass("config_sh", "version=%s mqtt=%s:%d user=%s pass=***",
			c->version, c->mqtt_host, c->mqtt_port, c->mqtt_user);
	else
		st_fail_line("config_sh", "mqtt 端点缺失 host='%s' port=%d",
			     c->mqtt_host, c->mqtt_port);

	/* 5. db 打开 + encdb_stats 单行 */
	if (encdb_open(c) == 0 && encdb_stats(buf, sizeof(buf)) == 0 &&
	    buf[0] != '\0' && strchr(buf, '\n') == NULL)
		st_pass("encdb", "%s", buf);
	else
		st_fail_line("encdb", "打开失败或统计行缺失");

	/* 6. yaml-cli 可用 */
	out[0] = '\0';
	if (run_cmd("which yaml-cli", 5, out, sizeof(out)) == 0 && out[0]) {
		st_trim(out);
		st_pass("yaml_cli", "path=%s", out);
	} else {
		st_fail_line("yaml_cli", "命令不可用");
	}

	/* 7. majestic 进程存活 */
	if (proc_running("majestic"))
		st_pass("majestic", "进程存活");
	else
		st_fail_line("majestic", "进程未运行");

	/* 8. HTTP 探测 /image.jpg 与 /play_audio（失败输出 DISABLED 而非 FAIL） */
	st_check_http("http_capture", "/image.jpg", "抓拍", c);
	st_check_http("http_voice", "/play_audio", "语音", c);

	/* 9. I2C 电量读（失败 DISABLED） */
	{
		long batt = 0, mv = 0;

		if (battery_refresh(c) == 0 &&
		    state_get_int("battery", &batt) &&
		    state_get_int("battery_voltage_mv", &mv))
			st_pass("battery_i2c", "battery=%ld%% voltage_mv=%ld",
				batt, mv);
		else
			st_disabled("battery_i2c", "I2C 电量计不可用（/dev/i2c-%d 0x%02x）",
				    1, 0x36);
	}

	/* 10. FTP 配置完整性（host/user/pass 非空，凭据打码） */
	if (g_app.rt.ftp_host[0] && g_app.rt.ftp_user[0] && g_app.rt.ftp_pass[0])
		st_pass("ftp_config", "host=%s user=%s pass=***",
			g_app.rt.ftp_host, g_app.rt.ftp_user);
	else
		st_fail_line("ftp_config",
			     "host/user/pass 存在空值（未注册或 runtime 配置缺失）");

	/* 11. MQTT TCP connect（连上即 close，不发业务） */
	if (st_mqtt_tcp_probe(c->mqtt_host, c->mqtt_port, 5))
		st_pass("mqtt_tcp", "%s:%d 连接成功（立即关闭）",
			c->mqtt_host, c->mqtt_port);
	else
		st_fail_line("mqtt_tcp", "%s:%d 连接失败",
			     c->mqtt_host, c->mqtt_port);

	/* 12. outbox 目录可写 */
	st_check_dir_writable("outbox_dir", c->outbox_dir);

	printf("----- selftest done: fail=%d -----\n", g_st_fail);
	return g_st_fail == 0 ? 0 : 1;
}
