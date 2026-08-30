/*
 * config.c — /etc/encalertd.conf 解析 + encoder config.sh 继承 + device_id
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

/* 布尔值解析：conf 文档与 bash 开关沿用 "true"，同时兼容 1/on/yes */
static bool cfg_bool(const char *v)
{
	return !strcmp(v, "1") || !strcmp(v, "true") ||
	       !strcmp(v, "on") || !strcmp(v, "yes");
}

/* conf 键值对：读一行 key=value，忽略 # 注释与空行 */
static void cfg_set(enc_cfg_t *c, const char *k, const char *v)
{
	if      (!strcmp(k, "interval_sec"))       c->interval_sec = atoi(v);
	else if (!strcmp(k, "dedup_sec"))          c->dedup_sec = atoi(v);
	else if (!strcmp(k, "spool_dir"))   snprintf(c->spool_dir, sizeof(c->spool_dir), "%s", v);
	else if (!strcmp(k, "log_file"))    snprintf(c->log_file, sizeof(c->log_file), "%s", v);
	else if (!strcmp(k, "log_verbose"))        c->log_verbose = atoi(v);
	else if (!strcmp(k, "state_dir"))   snprintf(c->state_dir, sizeof(c->state_dir), "%s", v);
	else if (!strcmp(k, "actions_dir")) snprintf(c->actions_dir, sizeof(c->actions_dir), "%s", v);
	else if (!strcmp(k, "action_timeout_sec")) c->action_timeout_sec = atoi(v);
	else if (!strcmp(k, "enable_wifi"))        c->enable_wifi = cfg_bool(v);
	else if (!strcmp(k, "enable_battery"))     c->enable_battery = cfg_bool(v);
	else if (!strcmp(k, "enable_sdcard"))      c->enable_sdcard = cfg_bool(v);
	else if (!strcmp(k, "enable_sysres"))      c->enable_sysres = cfg_bool(v);
	else if (!strcmp(k, "enable_process"))     c->enable_process = cfg_bool(v);
	else if (!strcmp(k, "enable_stream"))      c->enable_stream = cfg_bool(v);
	else if (!strcmp(k, "mqtt_host"))   snprintf(c->mqtt_host, sizeof(c->mqtt_host), "%s", v);
	else if (!strcmp(k, "mqtt_port"))          c->mqtt_port = atoi(v);
	else if (!strcmp(k, "mqtt_user"))   snprintf(c->mqtt_user, sizeof(c->mqtt_user), "%s", v);
	else if (!strcmp(k, "mqtt_pass"))   snprintf(c->mqtt_pass, sizeof(c->mqtt_pass), "%s", v);
	else if (!strcmp(k, "mqtt_qos"))           c->mqtt_qos = atoi(v);
	else if (!strcmp(k, "low_batt_mv"))        c->low_batt_mv = atoi(v);
	else if (!strcmp(k, "low_batt_pct"))       c->low_batt_pct = atoi(v);
	else if (!strcmp(k, "low_batt_confirm"))   c->low_batt_confirm = atoi(v);
	else if (!strcmp(k, "sd_warn_pct"))        c->sd_warn_pct = atoi(v);
	else if (!strcmp(k, "temp_warn_mc"))       c->temp_warn_mc = atoi(v);
	else if (!strcmp(k, "temp_err_mc"))        c->temp_err_mc = atoi(v);
	else if (!strcmp(k, "load_factor_warn"))   c->load_factor_warn = atof(v);
	else if (!strcmp(k, "mem_avail_min_kb"))   c->mem_avail_min_kb = atol(v);
	else if (!strcmp(k, "charge_window_sec"))  c->charge_window_sec = atoi(v);
	else if (!strcmp(k, "charge_drop_pct"))    c->charge_drop_pct = atoi(v);
	else if (!strcmp(k, "charge_drop_mv"))     c->charge_drop_mv = atoi(v);
	else if (!strcmp(k, "record_min_free_mb")) c->record_min_free_mb = atol(v);
	else if (!strcmp(k, "monitor_procs")) snprintf(c->monitor_procs, sizeof(c->monitor_procs), "%s", v);
	else if (!strcmp(k, "storm_window_sec"))   c->storm_window_sec = atoi(v);
	else if (!strcmp(k, "storm_max_restarts")) c->storm_max_restarts = atoi(v);
	else if (!strcmp(k, "crash_dir"))   snprintf(c->crash_dir, sizeof(c->crash_dir), "%s", v);
	else if (!strcmp(k, "wifi_iface")) snprintf(c->wifi_iface, sizeof(c->wifi_iface), "%s", v);
}

static void cfg_defaults(enc_cfg_t *c)
{
	memset(c, 0, sizeof(*c));
	c->interval_sec   = 10;
	c->dedup_sec      = 600;
	snprintf(c->spool_dir, sizeof(c->spool_dir), ENC_SPOOL_DIR_DEFAULT);
	snprintf(c->log_file, sizeof(c->log_file), ENC_LOG_FILE_DEFAULT);
	c->log_verbose    = 0;
	snprintf(c->state_dir, sizeof(c->state_dir), ENC_DEFAULT_STATE_DIR);
	snprintf(c->actions_dir, sizeof(c->actions_dir), ENC_ACTIONS_DIR_DEFAULT);
	c->action_timeout_sec = 30;
	c->enable_wifi = c->enable_battery = c->enable_sdcard =
	    c->enable_sysres = c->enable_process = c->enable_stream = true;

	/* MQTT 地址/凭据：默认空，由 config.sh 继承或环境变量/conf 提供。
	 * 全部为空时报警只落 spool 不发送（保证不硬编码任何凭据）。 */
	c->mqtt_port = 1883;
	c->mqtt_qos  = 1;

	c->low_batt_mv        = 3200;
	c->low_batt_pct       = 3;
	c->low_batt_confirm   = 3;
	c->sd_warn_pct        = 95;
	c->temp_warn_mc       = 85000;
	c->temp_err_mc        = 95000;
	c->load_factor_warn   = 3.0;
	c->mem_avail_min_kb   = 8192;
	c->charge_window_sec  = 300;
	c->charge_drop_pct    = 2;
	c->charge_drop_mv     = 80;        /* 充电尾段实测波动 ~74mV，30 会误报 2001 */
	c->record_min_free_mb = 5120;      /* 5GB：足够一轮应急录像的底线 */

	/* 进程监控（2026-08-31 起 C 版 encodermain 进固件，bash 家族名移除）：
	 * majestic 固定 pid 文件路径；encodermain 用 lock.c 单实例 pid
	 * （state_dir/pid，显式 pidfile 命中后做身份复核防 pid 回收）；
	 * ipc_server(=factoryinit 服务) 无 pid 文件，走 /proc comm 扫描 */
	snprintf(c->monitor_procs, sizeof(c->monitor_procs),
		 "majestic:/var/run/majestic.pid,"
		 "encodermain:/root/encoder/runtime/state/pid,"
		 "ipc_server");
	c->storm_window_sec   = 900;       /* 15min 窗口 */
	c->storm_max_restarts = 3;         /* 窗口内拉起 ≥3 次升级 reboot */
	snprintf(c->crash_dir, sizeof(c->crash_dir),
		 "/mnt/mmcblk0p1/logs/crash");

	snprintf(c->wifi_iface, sizeof(c->wifi_iface), "wlan0");
}

/* 继承 /root/encoder/config.sh 中的 MQTT_HOST/PORT/USER/PASS/QOS，
 * 与原 bash 版 system_monitor 的 try_load_encoder_env 行为一致。 */
static void cfg_inherit_encoder_config(enc_cfg_t *c)
{
	FILE *f = fopen(ENC_ENCODER_CONFIG, "r");
	char line[512], val[448];
	char *eq;
	size_t n;

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		/* strip \n */ n = strlen(line);
		while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r'))
			line[--n] = '\0';
		eq = strchr(line, '=');
		if (!eq)
			continue;
		val[0] = '\0';
		{
			const char *src = eq + 1;
			char *hash;
			size_t i = 0;
			while (*src == ' ' || *src == '\t') src++;
			while (*src && i + 1 < sizeof(val))
				val[i++] = *src++;
			val[i] = '\0';
			/* 行尾注释剥离：config.sh 行格式为 KEY="v" # 注释。 */
			hash = strstr(val, " #");
			if (hash)
				*hash = '\0';
		}
		/* 去尾部空白（引号后到注释前的间隙） */
		n = strlen(val);
		while (n > 0 && (val[n-1] == ' ' || val[n-1] == '\t'))
			val[--n] = '\0';
		/* 去掉成对引号 */
		if (n >= 2 && ((val[0] == '"' && val[n-1] == '"') ||
			       (val[0] == '\'' && val[n-1] == '\''))) {
			memmove(val, val + 1, n - 2);
			val[n - 2] = '\0';
		}
		*eq = '\0';
		{
			char *kp = line;
			long v;
			while (*kp == ' ' || *kp == '\t') kp++;
			if (!strcmp(kp, "MQTT_HOST"))
				snprintf(c->mqtt_host, sizeof(c->mqtt_host),
					 "%s", val);
			else if (!strcmp(kp, "MQTT_PORT")) {
				v = strtol(val, NULL, 10);
				if (v >= 1 && v <= 65535)
					c->mqtt_port = (int)v;
			} else if (!strcmp(kp, "MQTT_USER"))
				snprintf(c->mqtt_user, sizeof(c->mqtt_user),
					 "%s", val);
			else if (!strcmp(kp, "MQTT_PASS"))
				snprintf(c->mqtt_pass, sizeof(c->mqtt_pass),
					 "%s", val);
			else if (!strcmp(kp, "MQTT_QOS")) {
				v = strtol(val, NULL, 10);
				if (v >= 0 && v <= 2)
					c->mqtt_qos = (int)v;
			}
		}
	}
	fclose(f);
}

int cfg_load(enc_cfg_t *c, const char *path)
{
	cfg_defaults(c);

	/* 1. /root/encoder/config.sh 继承（低优先级） */
	cfg_inherit_encoder_config(c);

	/* 2. 环境变量 ENC_MQTT_PASS（中优先级，避免明文落 conf） */
	{
		const char *env = getenv("ENC_MQTT_PASS");
		if (env && env[0])
			snprintf(c->mqtt_pass, sizeof(c->mqtt_pass), "%s", env);
	}

	/* 3. conf 覆盖（最高优先级） */
	if (path && path[0]) {
		FILE *f = fopen(path, "r");
		char line[512], *hash, *eq, *k, *v;
		if (!f) {
			log_msg(ENC_LOG_WARN,
				"conf file not readable: %s (using inherited defaults)",
				path);
			goto out;
		}
		while (fgets(line, sizeof(line), f)) {
			hash = strchr(line, '#');
			if (hash)
				*hash = '\0';
			eq = strchr(line, '=');
			if (!eq)
				continue;
			*eq = '\0';
			k = line;
			while (*k == ' ' || *k == '\t') k++;
			v = eq + 1;
			while (*v == ' ' || *v == '\t') v++;
			{ size_t n = strlen(v);
			  while (n > 0 && isspace((unsigned char)v[n-1]))
				  v[--n] = '\0'; }
			{ size_t n = strlen(k);
			  while (n > 0 && isspace((unsigned char)k[n-1]))
				  k[--n] = '\0'; }
			if (*k && *v)
				cfg_set(c, k, v);
		}
		fclose(f);
	}
out:
	if (!c->mqtt_host[0])
		log_msg(ENC_LOG_WARN,
			"no mqtt host resolved (config.sh/conf/env all empty); "
			"alerts will spool locally only");
	return 0;
}

const char *cfg_get_str(const enc_cfg_t *c, void *field_dummy)
{
	(void)c; (void)field_dummy;
	return "";
}
