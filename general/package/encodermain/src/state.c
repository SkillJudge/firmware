/*
 * state.c — state 文件读写 + registerAck 运行时(runtime) 应用（encodermain 基础层）
 *
 * 键名/默认值严格对齐 bash state.sh + config.sh 的 STATE_*_FILE 清单
 * （一个字段一个文件，路径与格式保持不变，供 encalertd/config_page.sh 共读）；
 * runtime 文件名对齐 runtime.sh / app_service.sh：runtime_ftp_*、runtime_srs_*、
 * time_offset_ms、server_timestamp_ms；另增 C 侧持久化文件 runtime_ftp_path /
 * runtime_srs_url / runtime_applied（bash 无对应键，不冲突）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

/* state_init/rt_* 刷新缓存；未初始化时回落默认路径 */
static char g_state_dir[256];

static const char *state_dir_now(void)
{
	return g_state_dir[0] ? g_state_dir : ENCM_STATE_DIR;
}

static void state_dir_set(const enc_cfg_t *c)
{
	snprintf(g_state_dir, sizeof(g_state_dir), "%s",
		 (c && c->state_dir[0]) ? c->state_dir : ENCM_STATE_DIR);
}

static void state_path(char *buf, size_t sz, const char *key)
{
	snprintf(buf, sz, "%s/%s", state_dir_now(), key);
}

/* state 键与默认值：严格照 bash state.sh state_init / common.sh ensure_layout */
static const struct { const char *key; const char *def; } g_state_defs[] = {
	{ "is_idle",              "true"  },
	{ "is_recording",         "false" },
	{ "is_publishing",        "false" },
	{ "is_charging",          "false" },
	{ "battery",              "100"   },
	{ "battery_voltage_mv",   "0"     },
	{ "signal",               "0"     },
	{ "current_task_id",      ""      },
	{ "current_record_id",    ""      },
	{ "current_record_flow",  ""      },
	{ "current_stream_url",   ""      },
	{ "record_start_ts",      "0"     },
	{ "record_session_time",  ""      },
	{ "segment_no",           "0"     },
	{ "segment_manifest",     ""      },
	{ "last_record_id",       ""      },
	{ "last_record_task_id",  ""      },
	{ "last_record_file_name", ""     },
	{ "last_record_file_url", ""      },
	{ "last_record_file_size", "0"    },
	{ "last_record_segment_no", "0"   },
	{ "msgid",                "1000"  },	/* MSGID_FILE，bash 默认 1000 */
};

int state_init(const enc_cfg_t *c)
{
	size_t i, n = sizeof(g_state_defs) / sizeof(g_state_defs[0]);
	char path[512];

	state_dir_set(c);
	if (dir_ensure(state_dir_now()) != 0)
		return -1;
	/* 与 bash ensure_layout 对齐的运行目录（state 之外的基础目录） */
	if (c && c->runtime_dir[0]) {
		snprintf(path, sizeof(path), "%s/logs", c->runtime_dir);
		dir_ensure(path);
		snprintf(path, sizeof(path), "%s/tmp", c->runtime_dir);
		dir_ensure(path);
	}
	/* 只补缺失键默认值，不覆盖已有值（重启不丢业务状态） */
	for (i = 0; i < n; i++) {
		state_path(path, sizeof(path), g_state_defs[i].key);
		if (access(path, F_OK) != 0)
			write_str_file(path, g_state_defs[i].def);
	}
	return 0;
}

void state_set_str(const char *key, const char *val)
{
	char path[512];

	if (!key || !key[0])
		return;
	state_path(path, sizeof(path), key);
	write_str_file(path, val ? val : "");
}

void state_set_int(const char *key, long v)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%ld", v);
	state_set_str(key, buf);
}

bool state_get_str(const char *key, char *buf, size_t sz)
{
	char path[512];

	if (!key || !key[0] || !buf || sz == 0)
		return false;
	state_path(path, sizeof(path), key);
	if (access(path, F_OK) != 0)
		return false;
	/* 空文件返回空串（bash state_read 对空文件返回空） */
	read_str_file(path, buf, sz);
	return true;
}

bool state_get_int(const char *key, long *out)
{
	char buf[64];

	if (!state_get_str(key, buf, sizeof(buf)) || buf[0] == '\0')
		return false;
	*out = strtol(buf, NULL, 10);
	return true;
}

/* ------------------------------------------------------------------ */
/* registerAck 运行时（bash runtime.sh 语义）                            */
/* ------------------------------------------------------------------ */

/* 读 state 文件：存在且内容非空才拷贝，否则不动 buf */
static bool rt_read(const char *key, char *buf, size_t sz)
{
	char path[512], tmp[256];

	state_path(path, sizeof(path), key);
	if (!read_str_file(path, tmp, sizeof(tmp)) || tmp[0] == '\0')
		return false;
	snprintf(buf, sz, "%s", tmp);
	return true;
}

/* 空 val 不写（bash：空字段不覆盖本地已有值） */
static void rt_write(const char *key, const char *val)
{
	if (val && val[0])
		state_set_str(key, val);
}

/* 从 registerAck data JSON 取字段：string 或 number 都转成字符串 */
static bool jv_get_str(const jv_t *obj, const char *path, char *out, size_t sz)
{
	const jv_t *v = jv_path(obj, path);
	const char *s;

	if (!v)
		return false;
	s = jv_str(v);
	if (s && s[0]) {
		snprintf(out, sz, "%s", s);
		return true;
	}
	if (jv_is_num(v)) {
		snprintf(out, sz, "%lld", jv_int(v, 0));
		return true;
	}
	return false;
}

static bool flag_file_true(const char *key)
{
	char path[512], buf[16];

	state_path(path, sizeof(path), key);
	if (!read_str_file(path, buf, sizeof(buf)))
		return false;
	return !strcmp(buf, "true") || !strcmp(buf, "1");
}


/* 是不是可以不需要保存这些参数到 runtime 文件，因为每次启动都会从 registerAck 中获取。请你思考以后修改代码，避免重复保存。那么问题是，重启以后，重新register，会丢丢掉之前的录像任务，不能恢复现场。录像文件就失败了。
然而，我工作环境里的这些 常量参数都是写死的，不需要变更。是不是可以写死，测试环境跟未来的生产环境是一致的*/

/* 启动时从 runtime 文件恢复（断电重启后 FTP/SRS/时间偏移不丢） */
int rt_load(const enc_cfg_t *c, enc_runtime_t *rt)
{
	char host[128], port[32];
	char path[512];
	long off = 0;

	if (!rt)
		return -1;
	state_dir_set(c);
	memset(rt, 0, sizeof(*rt));
	snprintf(rt->ftp_path, sizeof(rt->ftp_path), "/");	/* 远端根目录默认 / */

	rt_read("runtime_ftp_host", rt->ftp_host, sizeof(rt->ftp_host));
	rt_read("runtime_ftp_user", rt->ftp_user, sizeof(rt->ftp_user));
	rt_read("runtime_ftp_pass", rt->ftp_pass, sizeof(rt->ftp_pass));
	rt_read("runtime_ftp_path", rt->ftp_path, sizeof(rt->ftp_path));

	/* srs_url：优先 C 侧直存 URL，否则按 bash runtime_srs_host/port 拼接 */
	if (!rt_read("runtime_srs_url", rt->srs_url, sizeof(rt->srs_url))) {
		host[0] = '\0';
		port[0] = '\0';
		rt_read("runtime_srs_host", host, sizeof(host));
		rt_read("runtime_srs_port", port, sizeof(port));
		if (host[0])
			snprintf(rt->srs_url, sizeof(rt->srs_url),
				 "rtmp://%s:%s", host,
				 port[0] ? port : "1935");
	}

	state_path(path, sizeof(path), "time_offset_ms");
	if (read_int_file(path, &off))
		rt->time_offset_ms = (long long)off;
	rt->applied = flag_file_true("runtime_applied");
	return 0;
}

/* 应用 registerAck data JSON：写 runtime 文件 + 计算时间偏移（不直接改系统时钟）。
 * data_json 兼容两种形态：完整 payload（含 data 包裹）或 data 对象本身。
 * 字段照 bash app_service.sh service_apply_register_runtime：
 *   ftp.host/username/password(/path)、srs.host/port/username/password、timestamp */
int rt_apply_register_ack(const enc_cfg_t *c, enc_runtime_t *rt,
			  const char *data_json)
{
	jv_t *root;
	const jv_t *d;
	const jv_t *srs;
	const char *s;
	char val[256], host[128], port[32];
	long long server_ms = 0, off;

	if (!rt || !data_json || !data_json[0])
		return -1;
	state_dir_set(c);
	root = jv_parse(data_json);
	if (!root) {
		log_msg(ENCM_LOG_WARN, "registerAck data json parse failed");
		return -1;
	}
	d = jv_path(root, "data");
	if (!d)
		d = root;

	/* FTP：空字段不覆盖本地已有值（bash save_runtime_ftp_config 语义）。
	 * 密码等字段不做日志，避免泄露账号。 */
	if (jv_get_str(d, "ftp.host", val, sizeof(val))) {
		snprintf(rt->ftp_host, sizeof(rt->ftp_host), "%.*s",
			 (int)sizeof(rt->ftp_host) - 1, val);
		rt_write("runtime_ftp_host", rt->ftp_host);
	}
	if (jv_get_str(d, "ftp.username", val, sizeof(val))) {
		snprintf(rt->ftp_user, sizeof(rt->ftp_user), "%.*s",
			 (int)sizeof(rt->ftp_user) - 1, val);
		rt_write("runtime_ftp_user", rt->ftp_user);
	}
	if (jv_get_str(d, "ftp.password", val, sizeof(val))) {
		snprintf(rt->ftp_pass, sizeof(rt->ftp_pass), "%.*s",
			 (int)sizeof(rt->ftp_pass) - 1, val);
		rt_write("runtime_ftp_pass", rt->ftp_pass);
	}
	if (jv_get_str(d, "ftp.path", val, sizeof(val))) {
		snprintf(rt->ftp_path, sizeof(rt->ftp_path), "%.*s",
			 (int)sizeof(rt->ftp_path) - 1, val);
		rt_write("runtime_ftp_path", rt->ftp_path);
	}

	/* SRS：整体字符串 URL 或对象 {host,port,username,password} */
	srs = jv_path(d, "srs");
	s = jv_str(srs);
	if (s && s[0]) {
		snprintf(rt->srs_url, sizeof(rt->srs_url), "%s", s);
		rt_write("runtime_srs_url", rt->srs_url);
	} else if (srs) {
		host[0] = '\0';
		port[0] = '\0';
		jv_get_str(srs, "host", host, sizeof(host));
		jv_get_str(srs, "port", port, sizeof(port));
		if (host[0]) {
			snprintf(rt->srs_url, sizeof(rt->srs_url),
				 "rtmp://%s:%s", host,
				 port[0] ? port : "1935");
			rt_write("runtime_srs_host", host);
			rt_write("runtime_srs_port", port[0] ? port : "1935");
			rt_write("runtime_srs_url", rt->srs_url);
		}
		if (jv_get_str(srs, "username", val, sizeof(val)))
			rt_write("runtime_srs_user", val);
		if (jv_get_str(srs, "password", val, sizeof(val)))
			rt_write("runtime_srs_pass", val);
	}

	/* 服务器时间戳：毫秒，兼容 10 位秒级（bash normalize_timestamp_to_ms） */
	if (jv_get_str(d, "timestamp", val, sizeof(val)) && val[0]) {
		long long n = strtoll(val, NULL, 10);

		if (n > 0) {
			server_ms = (n < 100000000000LL) ? n * 1000LL : n;
			off = server_ms - now_ms();
			rt->time_offset_ms = off;
			snprintf(val, sizeof(val), "%lld", off);
			state_set_str("time_offset_ms", val);
			snprintf(val, sizeof(val), "%lld", server_ms);
			state_set_str("server_timestamp_ms", val);
			log_msg(ENCM_LOG_INFO,
				"time sync applied server_ms=%lld offset_ms=%lld",
				server_ms, off);
		}
	} else {
		log_msg(ENCM_LOG_WARN,
			"register ack has no timestamp, skip time sync");
	}

	rt->applied = true;
	state_set_str("runtime_applied", "true");
	jv_free(root);
	return 0;
}
