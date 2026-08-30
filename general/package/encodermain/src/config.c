/*
 * config.c — 配置三级合并 + device_id 解析（encodermain 基础层）
 *
 * 合并顺序（后者覆盖前者）：
 *   1) 内置默认值（common.h 注释口径）
 *   2) /root/encoder/config.sh 全键继承（KEY="value" 格式，键存在才映射）
 *   3) 环境变量 DEVICE_ID / ENC_MQTT_PASS（凭据不落明文 conf）
 *   4) conf 文件覆盖（最高优先级，键名 = common.h 字段名）
 * device_id 解析（env 分区权威）：cfg 显式配置 → fw_printenv DEVICE_ID；
 * 均缺失 = 设备未初始化，返回空串（main 门卫等待，见 main.c）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common.h"

#define ENC_VERSION_FILE "/etc/version"

static void set_str(char *dst, size_t sz, const char *v)
{
	snprintf(dst, sz, "%s", v);
}

static bool parse_bool(const char *v)
{
	return !strcasecmp(v, "true") || !strcmp(v, "1") ||
	       !strcasecmp(v, "yes") || !strcasecmp(v, "on");
}

/* 解析一行 KEY=value / KEY="value"；就地改写 line，剥离行尾 " # 注释" 与成对引号。
 * 成功时 *key 与 *val 指向行内片段（val 可为空串）。 */
static bool parse_kv_line(char *line, char **key, char **val)
{
	char *eq, *hash, *k, *v;
	size_t n;

	n = strlen(line);
	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
		line[--n] = '\0';
	eq = strchr(line, '=');
	if (!eq)
		return false;
	*eq = '\0';
	k = line;
	while (*k == ' ' || *k == '\t')
		k++;
	v = eq + 1;
	while (*v == ' ' || *v == '\t')
		v++;
	/* 行尾注释：config.sh 行格式为 KEY="value" # 注释 */
	hash = strstr(v, " #");
	if (hash)
		*hash = '\0';
	n = strlen(v);
	while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t'))
		v[--n] = '\0';
	/* 去掉成对引号 */
	if (n >= 2 && ((v[0] == '"' && v[n - 1] == '"') ||
		       (v[0] == '\'' && v[n - 1] == '\''))) {
		memmove(v, v + 1, n - 2);
		v[n - 2] = '\0';
	}
	n = strlen(k);
	while (n > 0 && (k[n - 1] == ' ' || k[n - 1] == '\t'))
		k[--n] = '\0';
	if (!k[0])
		return false;
	*key = k;
	*val = v;
	return true;
}

/* conf 键名 = common.h 字段名 */
static void cfg_set(enc_cfg_t *c, const char *k, const char *v)
{
	if      (!strcmp(k, "device_id"))
		set_str(c->device_id, sizeof(c->device_id), v);
	else if (!strcmp(k, "version"))
		set_str(c->version, sizeof(c->version), v);
	else if (!strcmp(k, "log_verbose"))
		c->log_verbose = parse_bool(v) ? 1 : 0;
	else if (!strcmp(k, "log_file"))
		set_str(c->log_file, sizeof(c->log_file), v);
	else if (!strcmp(k, "conf_path"))
		set_str(c->conf_path, sizeof(c->conf_path), v);
	else if (!strcmp(k, "work_dir"))
		set_str(c->work_dir, sizeof(c->work_dir), v);
	else if (!strcmp(k, "state_dir"))
		set_str(c->state_dir, sizeof(c->state_dir), v);
	else if (!strcmp(k, "runtime_dir"))
		set_str(c->runtime_dir, sizeof(c->runtime_dir), v);
	else if (!strcmp(k, "record_root"))
		set_str(c->record_root, sizeof(c->record_root), v);
	else if (!strcmp(k, "db_file"))
		set_str(c->db_file, sizeof(c->db_file), v);
	else if (!strcmp(k, "outbox_dir"))
		set_str(c->outbox_dir, sizeof(c->outbox_dir), v);
	else if (!strcmp(k, "voice_pcm"))
		set_str(c->voice_pcm, sizeof(c->voice_pcm), v);
	else if (!strcmp(k, "majestic_conf"))
		set_str(c->majestic_conf, sizeof(c->majestic_conf), v);
	else if (!strcmp(k, "majestic_init"))
		set_str(c->majestic_init, sizeof(c->majestic_init), v);
	else if (!strcmp(k, "yaml_cli"))
		set_str(c->yaml_cli, sizeof(c->yaml_cli), v);
	else if (!strcmp(k, "mqtt_host"))
		set_str(c->mqtt_host, sizeof(c->mqtt_host), v);
	else if (!strcmp(k, "mqtt_port"))
		c->mqtt_port = atoi(v);
	else if (!strcmp(k, "mqtt_user"))
		set_str(c->mqtt_user, sizeof(c->mqtt_user), v);
	else if (!strcmp(k, "mqtt_pass"))
		set_str(c->mqtt_pass, sizeof(c->mqtt_pass), v);
	else if (!strcmp(k, "mqtt_qos"))
		c->mqtt_qos = atoi(v);
	else if (!strcmp(k, "heartbeat_sec"))
		c->heartbeat_sec = atoi(v);
	else if (!strcmp(k, "majestic_lock_wait_sec"))
		c->majestic_lock_wait_sec = atoi(v);
	else if (!strcmp(k, "command_queue_max"))
		c->command_queue_max = atoi(v);
	else if (!strcmp(k, "task_dedup_max"))
		c->task_dedup_max = atoi(v);
	else if (!strcmp(k, "db_compact_records"))
		c->db_compact_records = atoi(v);
	else if (!strcmp(k, "db_alarm_max"))
		c->db_alarm_max = atoi(v);
	else if (!strcmp(k, "upload_retry_max"))
		c->upload_retry_max = atoi(v);
	else if (!strcmp(k, "upload_retry_backoff_sec"))
		c->upload_retry_backoff_sec = atoi(v);
	else if (!strcmp(k, "upload_rescan_on_reconnect"))
		c->upload_rescan_on_reconnect = parse_bool(v);
	else if (!strcmp(k, "http_connect_timeout_sec"))
		c->http_connect_timeout_sec = atoi(v);
	else if (!strcmp(k, "http_max_time_sec"))
		c->http_max_time_sec = atoi(v);
	else if (!strcmp(k, "record_verify_timeout_sec"))
		c->record_verify_timeout_sec = atoi(v);
	else if (!strcmp(k, "segment_stable_sec"))
		c->segment_stable_sec = atoi(v);
}

static void cfg_defaults(enc_cfg_t *c)
{
	memset(c, 0, sizeof(*c));
	set_str(c->conf_path, sizeof(c->conf_path), ENCM_DEFAULT_CONF);
	set_str(c->log_file, sizeof(c->log_file), ENCM_LOG_FILE);
	set_str(c->work_dir, sizeof(c->work_dir), ENCM_WORK_DIR);
	set_str(c->state_dir, sizeof(c->state_dir), ENCM_STATE_DIR);
	set_str(c->runtime_dir, sizeof(c->runtime_dir), ENCM_RUNTIME_DIR);
	set_str(c->record_root, sizeof(c->record_root), ENCM_RECORD_ROOT);
	set_str(c->db_file, sizeof(c->db_file), ENCM_DB_FILE);
	set_str(c->outbox_dir, sizeof(c->outbox_dir),
		ENCM_RUNTIME_DIR "/outbox");
	set_str(c->voice_pcm, sizeof(c->voice_pcm), ENCM_VOICE_PCM);
	set_str(c->majestic_conf, sizeof(c->majestic_conf), ENCM_MAJESTIC_CONF);
	set_str(c->majestic_init, sizeof(c->majestic_init),
		ENCM_MAJESTIC_INIT);
	set_str(c->yaml_cli, sizeof(c->yaml_cli), ENCM_YAML_CLI);

	/* MQTT 凭据默认空，由 config.sh 继承或环境变量/conf 提供 */
	c->mqtt_port = 1883;
	c->mqtt_qos  = 2;

	c->heartbeat_sec            = 30;
	c->majestic_lock_wait_sec   = 60;
	c->command_queue_max        = 32;
	c->task_dedup_max           = 256;
	c->db_compact_records       = 4096;
	c->db_alarm_max             = 512;
	c->upload_retry_max         = 5;
	c->upload_retry_backoff_sec = 30;
	c->upload_rescan_on_reconnect = true;
	c->http_connect_timeout_sec = 3;
	c->http_max_time_sec        = 10;
	c->record_verify_timeout_sec = 10;
	c->segment_stable_sec       = 20;
}

/* 继承 /root/encoder/config.sh：与 common.sh 读取的全局配置对齐。
 * 键存在才映射，找不到保持默认值。 */
static void cfg_inherit_encoder_config(enc_cfg_t *c)
{
	FILE *f = fopen(ENCM_ENCODER_CONFIG, "r");
	char line[512], *k, *v;
	long n;

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (!parse_kv_line(line, &k, &v))
			continue;
		if (!strcmp(k, "VERSION") && v[0])
			set_str(c->version, sizeof(c->version), v);
		else if (!strcmp(k, "DEVICE_ID") && v[0])
			set_str(c->device_id, sizeof(c->device_id), v);
		else if (!strcmp(k, "MQTT_HOST") && v[0])
			set_str(c->mqtt_host, sizeof(c->mqtt_host), v);
		else if (!strcmp(k, "MQTT_PORT")) {
			n = strtol(v, NULL, 10);
			if (n >= 1 && n <= 65535)
				c->mqtt_port = (int)n;
		} else if (!strcmp(k, "MQTT_USER") && v[0])
			set_str(c->mqtt_user, sizeof(c->mqtt_user), v);
		else if (!strcmp(k, "MQTT_PASS") && v[0])
			set_str(c->mqtt_pass, sizeof(c->mqtt_pass), v);
		else if (!strcmp(k, "MQTT_QOS")) {
			n = strtol(v, NULL, 10);
			if (n >= 0 && n <= 2)
				c->mqtt_qos = (int)n;
		} else if (!strcmp(k, "HEARTBEAT_INTERVAL_SEC")) {
			n = strtol(v, NULL, 10);
			if (n > 0)
				c->heartbeat_sec = (int)n;
		} else if (!strcmp(k, "SEGMENT_STABLE_SEC")) {
			n = strtol(v, NULL, 10);
			if (n > 0)
				c->segment_stable_sec = (int)n;
		} else if (!strcmp(k, "RECORD_START_VERIFY_TIMEOUT_SEC")) {
			n = strtol(v, NULL, 10);
			if (n > 0)
				c->record_verify_timeout_sec = (int)n;
		} else if (!strcmp(k, "LOG_VERBOSE")) {
			c->log_verbose = parse_bool(v) ? 1 : 0;
		}
	}
	fclose(f);
}

/* 环境变量：DEVICE_ID 与 ENC_MQTT_PASS（中优先级，凭据不落明文 conf） */
static void cfg_env_apply(enc_cfg_t *c)
{
	const char *env = getenv("DEVICE_ID");

	if (env && env[0])
		set_str(c->device_id, sizeof(c->device_id), env);
	env = getenv("ENC_MQTT_PASS");
	if (env && env[0])
		set_str(c->mqtt_pass, sizeof(c->mqtt_pass), env);
}

/* conf 覆盖（最高优先级）：key=value，忽略 # 注释与空行 */
static void cfg_conf_load(enc_cfg_t *c, const char *path)
{
	FILE *f = fopen(path, "r");
	char line[512], *hash, *k, *v;

	if (!f) {
		log_msg(ENCM_LOG_WARN,
			"conf file not readable: %s (using inherited defaults)",
			path);
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		hash = strchr(line, '#');
		if (hash)
			*hash = '\0';
		if (!parse_kv_line(line, &k, &v))
			continue;
		if (k[0] && v[0])
			cfg_set(c, k, v);
	}
	fclose(f);
}

/* bash config.sh get_device_version：/etc/version 首行，缺省 unknown */
static void cfg_version_fallback(enc_cfg_t *c)
{
	char buf[64];

	if (c->version[0])
		return;
	if (read_str_file(ENC_VERSION_FILE, buf, sizeof(buf)) && buf[0])
		set_str(c->version, sizeof(c->version), buf);
	else
		set_str(c->version, sizeof(c->version), "unknown");
}

int cfg_load(enc_cfg_t *c, const char *conf_path)
{
	cfg_defaults(c);

	/* 1. /root/encoder/config.sh 全键继承（低优先级） */
	cfg_inherit_encoder_config(c);

	/* 2. 环境变量 DEVICE_ID / ENC_MQTT_PASS */
	cfg_env_apply(c);

	/* 3. conf 覆盖（最高优先级） */
	if (conf_path && conf_path[0]) {
		set_str(c->conf_path, sizeof(c->conf_path), conf_path);
		cfg_conf_load(c, conf_path);
	}

	cfg_version_fallback(c);
	return 0;
}

/* bash is_valid_device_id：非空且只含 A-Za-z0-9._- */
static bool device_id_valid(const char *id)
{
	const char *p;

	if (!id || !id[0])
		return false;
	for (p = id; *p; p++) {
		unsigned char ch = (unsigned char)*p;

		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		      (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
		      ch == '-'))
			return false;
	}
	return true;
}

/* device_id 解析（env 分区权威）：
 *   1) cfg 显式配置（config.sh 继承 / 环境变量 / conf，测试与部署覆盖用）
 *   2) fw_printenv -n DEVICE_ID（start_encoder.sh 同源；factoryinit 用硬件 id
 *      向中心服务器换取后写入 env 分区，固件升级只刷 rootfs 不刷 env）
 * 两级都取不到 = 设备未正确初始化，返回空串且不缓存——调用方可周期重探，
 * factoryinit 写入后无需重启进程即可就绪。hostname 不再兜底：出厂 hostname
 * 三台相同且与设备身份无关（2026-08-30 多机联调结论）。 */
const char *device_id_get(const enc_cfg_t *c)
{
	static char cached[64];
	static bool resolved;
	char buf[128];

	if (resolved)
		return cached;

	if (c && c->device_id[0] && device_id_valid(c->device_id)) {
		resolved = true;
		set_str(cached, sizeof(cached), c->device_id);
		return cached;
	}
	/* U-Boot 环境变量 DEVICE_ID（start_encoder.sh: fw_printenv -n DEVICE_ID） */
	if (run_cmd("fw_printenv -n DEVICE_ID", 5, buf, sizeof(buf)) == 0 &&
	    device_id_valid(buf)) {
		resolved = true;
		set_str(cached, sizeof(cached), buf);
		return cached;
	}
	log_msg(ENCM_LOG_ERROR,
		"device not initialized: U-Boot DEVICE_ID missing "
		"(factoryinit not finished?)");
	return cached;	/* 空串；resolved 保持 false，下次调用重探 */
}
