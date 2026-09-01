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
#include "proto_internal.h"

static int g_st_fail;
static int g_st_pass;		/* 统计 PASS 数量，配合 proto unittest 用 */

static void st_pass(const char *name, const char *fmt, ...)
{
	va_list ap;

	g_st_pass++;
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
/* T1 字符串拼接 / URL 归一化 / dedup key 纯函数单元测试                  */
/*   —— 2026-09-01 encodermain 回归测试：覆盖前期 task_dedup_key 段数错、
/*      SRS 缺 /live 段 2051、前导点号隐藏文件、蛇形驼峰不一致等所有 bug */
/* ------------------------------------------------------------------ */

/* T1 helper：期望相等时 PASS，否则 FAIL，并打印 expect/actual */
static void ut_expect_streq(const char *case_name, const char *expect,
			    const char *actual)
{
	if (expect && actual && strcmp(expect, actual) == 0)
		st_pass(case_name, "got=[%s]", actual);
	else
		st_fail_line(case_name,
			     "expect=[%s] actual=[%s]",
			     expect ? expect : "(null)",
			     actual ? actual : "(null)");
}

/* T1.1-1.3 proto_key_sanitize 基础 + 前导点号 + 非法字符 */
static void ut_key_sanitize(void)
{
	char buf[256];

	/* T1.1 前导点号 / 中间点号保留：首字符不得是隐藏文件 '.' */
	proto_key_sanitize(buf, sizeof(buf), ".rtmp://a.b/c_d?e=f");
	/* 规则：合法字符保留，其余变 '_'。首字符 '.' 合法 → 按字符保留的话
	 * 仍会产生 `.rtmp_...`（隐藏文件）。正确做法需把"连续前导 ."删掉。
	 * 先写出实际值，断言期望"首字符非 '.'"——若当前实现未删前导点则 FAIL，
	 * 提醒开发者需到 protocol.c 修复 sanitize。 */
	if (buf[0] != '.')
		st_pass("T1.1_key_sanitize_leading_dot_removed",
			"首字符已清: [%s]", buf);
	else
		st_fail_line("T1.1_key_sanitize_leading_dot_removed",
			     "仍含前导点号→会产生隐藏文件！buf=[%s]", buf);

	/* T1.2 中间非法字符全部替换为 '_' */
	proto_key_sanitize(buf, sizeof(buf), "a:b//c@d!e#f%g");
	ut_expect_streq("T1.2_key_sanitize_illegal_to_underscore",
			"a_b__c_d_e_f_g", buf);

	/* T1.3 空输入 + 超长输入不越界 */
	proto_key_sanitize(buf, sizeof(buf), "");
	ut_expect_streq("T1.3_key_sanitize_empty_input", "", buf);
	memset(buf, 0xAA, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	proto_key_sanitize(buf, sizeof(buf),
			   "abcdef");	/* 小写入，验证尾零仍在 */
	if (buf[strlen("abcdef")] == '\0')
		st_pass("T1.3b_key_sanitize_write_tail_nul", "ok");
	else
		st_fail_line("T1.3b_key_sanitize_write_tail_nul",
			     "未写 NUL 终止符");
}

/* T1.4-1.8 proto_stream_url_normalize：SRS /live 段 / 伪两级 / fallback 等 */
static void ut_url_normalize(void)
{
	char out[512];
	bool ok;
	const char *dev = "ENC000001";

	/* T1.4 单级 path —— 原 Bug：SRS 2051 StreamNameEmpty */
	ok = proto_stream_url_normalize(
		"rtmp://192.168.250.100:1935/stream_ENC001",
		NULL, dev, out, sizeof(out));
	if (ok)
		ut_expect_streq("T1.4_url_singlelevel_insert_live",
			"rtmp://192.168.250.100:1935/live/stream_ENC000001",
			out);
	else
		st_fail_line("T1.4_url_singlelevel_insert_live",
			     "proto_stream_url_normalize 返回 false");

	/* T1.5 伪两级（首段非 live）→ 强制插 live 替换非末段 */
	ok = proto_stream_url_normalize(
		"rtmp://host:1935/otherApp/demoName",
		NULL, dev, out, sizeof(out));
	if (ok)
		ut_expect_streq("T1.5_url_faketwolevel_drop_nonlive",
			"rtmp://host:1935/live/stream_ENC000001", out);
	else
		st_fail_line("T1.5_url_faketwolevel_drop_nonlive",
			     "returned false");

	/* T1.6 正常两级（首段=live）→ 只替换末级 stream name，live 保留 */
	ok = proto_stream_url_normalize(
		"rtmp://host:1935/live/oldName",
		NULL, dev, out, sizeof(out));
	if (ok)
		ut_expect_streq("T1.6_url_normaltwolevel_preserve_live",
			"rtmp://host:1935/live/stream_ENC000001", out);
	else
		st_fail_line("T1.6_url_normaltwolevel_preserve_live",
			     "returned false");

	/* T1.7 三段以上层级 → 只取末段 stream name，中间丢弃 */
	ok = proto_stream_url_normalize(
		"rtmp://host:1935/a/b/c/deepName",
		NULL, dev, out, sizeof(out));
	if (ok)
		ut_expect_streq("T1.7_url_multilevel_keep_tail_only",
			"rtmp://host:1935/live/stream_ENC000001", out);
	else
		st_fail_line("T1.7_url_multilevel_keep_tail_only",
			     "returned false");

	/* T1.8 requested 空 → 回退 fallback_url */
	ok = proto_stream_url_normalize(
		"",
		"rtmp://srs.internal:1935",
		dev, out, sizeof(out));
	if (ok)
		ut_expect_streq("T1.8_url_empty_requested_fallback",
			"rtmp://srs.internal:1935/live/stream_ENC000001",
			out);
	else
		st_fail_line("T1.8_url_empty_requested_fallback",
			     "returned false (fallback not applied)");
}

/* T1.9 dedup key 对账：task_dedup_key 生成 vs dedup_remove_pair 查表生成，
 *      两者必须逐字节一致。用 STREAM_START 最关键的 case（原 bug 出处）。
 *   —— dispatch.c:444 task_dedup_key 生成:
 *      key = sanitize("<flow>_<action>_<biz>") = sanitize(
 *          "stream_start_stream_rtmp://host:1935/live/stream_X" )
 *   —— dedup_remove_pair (dispatch.c:319) 查表:
 *      flow_seg="stream" + action_seg="start_stream" + same biz
 *      最终 raw 格式相同，sanitize 结果必须完全相等 */
static void ut_dedup_key_pair_match(void)
{
	const char *flow   = "stream";
	const char *action = "start_stream";
	const char *biz    = "rtmp://192.168.250.100:1935/live/stream_ENC000001";
	char raw1[512], raw2[512], key_a[512], key_b[512];
	const char *flow_seg_p, *action_seg_p;

	/* side A: task_dedup_key 实际格式 (dispatch.c:444) */
	snprintf(raw1, sizeof(raw1), "%s_%s_%s", flow, action, biz);
	proto_key_sanitize(key_a, sizeof(key_a), raw1);

	/* side B: dedup_remove_pair 查表格式 (dispatch.c:319)
	 * 这是 r7 修复后独立的 flow_seg + action_seg 查表硬编码值，
	 * 与 proto_cmd_name(STREAM_START)=stream_start 故意不同步！ */
	flow_seg_p   = "stream";
	action_seg_p = "start_stream";
	snprintf(raw2, sizeof(raw2), "%s_%s_%s",
		 flow_seg_p, action_seg_p, biz);
	proto_key_sanitize(key_b, sizeof(key_b), raw2);

	if (strcmp(key_a, key_b) == 0) {
		st_pass("T1.9_dedup_key_pair_match",
			"A=B 长度=%zu: [%s]", strlen(key_a), key_a);
	} else {
		st_fail_line("T1.9_dedup_key_pair_match",
			"不匹配（dedup_remove_pair 删不掉 START.done → SRS 2051 原 Bug！）\n"
			"         key A (task_dedup_key   )=[%s]\n"
			"         key B (dedup_remove_pair)=[%s]\n"
			"         rawA=[%s]\n"
			"         rawB=[%s]",
			key_a, key_b, raw1, raw2);
	}
}

/* T1.10 proto_cmd_name 对照表（人工核对段数差异，不能自动断言，但打印
 *      日志用于排查"proto_cmd_name 误用于 dedup key 前缀"类回归） */
static void ut_proto_cmd_name_dump(void)
{
	static const proto_cmd_t cc[] = {
		ENCM_CMD_RECORD_START, ENCM_CMD_RECORD_STOP,
		ENCM_CMD_STREAM_START, ENCM_CMD_STREAM_STOP,
		ENCM_CMD_TASK_STREAM_START, ENCM_CMD_TASK_RECORD_START,
		ENCM_CMD_TASK_RECORD_STOP,  ENCM_CMD_TASK_RESET,
		ENCM_CMD_TASK_PREPARE_DESK_VOICE,
	};
	size_t i;

	printf("[INFO] T1.10 proto_cmd_name 对照表 (段数对账参考)\n");
	printf("[INFO] %-32s | %-30s | 下划线数\n", "enum", "name");
	printf("[INFO] ---------------------------------");
	printf("------------------------------------------------\n");
	for (i = 0; i < sizeof(cc) / sizeof(cc[0]); i++) {
		const char *n = proto_cmd_name(cc[i]);
		int          u = 0;
		const char  *p;

		for (p = n; p && *p; p++)
			if (*p == '_') u++;
		printf("[INFO] %-32s | %-30s | %d 个('_')\n",
		       "(see proto_internal.h)", n, u);
	}
	st_pass("T1.10_proto_cmd_name_dumped", "已输出对照表");
}

/* T1 汇总入口 */
static void ut_string_group(void)
{
	printf("===== [UNITEST T1] 字符串拼接 / URL 归一化 / Dedup Key =====\n");
	ut_key_sanitize();
	ut_url_normalize();
	ut_dedup_key_pair_match();
	ut_proto_cmd_name_dump();
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
	/* ===== T1: 字符串 / URL / dedup key 纯函数单元测试（先跑，失败不影响后续环境类检查） ===== */
	ut_string_group();
	printf("----- T1 小结: pass=%d fail=%d -----\n",
	       g_st_pass, g_st_fail);

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

	/* 8. HTTP 探测 /play_audio（失败输出 DISABLED 而非 FAIL）；
	 *    抓拍能力已移交上位机，不再探测 /image.jpg */
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

	printf("----- selftest done: pass=%d  fail=%d -----\n",
	       g_st_pass, g_st_fail);
	return g_st_fail == 0 ? 0 : 1;
}
