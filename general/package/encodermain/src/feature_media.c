/*
 * feature_media.c — 推流/录像/抓拍/复位（yaml-cli + HUP）—— 设计 §6.2
 *
 * 逐条映射 bash feature_engine.sh 的状态机语义：
 *   stream_start/stop、record_start/stop、reset_encoder、
 *   task_prepare_desk_voice、duration 定时、startup 恢复。
 *   （抓拍已移交上位机：协议保留 captureAck，设备端统一应答失败）
 *
 * yaml 键名与执行序列逐字对照 feature_engine.sh/common.sh：
 *   所有 yaml 修改都先 majestic_lock_acquire() 再 yaml-cli -i <conf> -s <key> <value>
 *   （字符串值 shell_quote），再 killall -HUP majestic（进程不在则 S95majestic start），
 *   最后 majestic_lock_release()。
 *
 * 说明：cfg 结构未继承 config.sh 的 profile/SRS/命名模板键，这些值按固件默认
 * （config.sh）硬编码；录像起始时间/分片号/会话时间等运行态仍走 state 键
 * （record_start_ts / segment_no / record_session_time，与 bash state.sh 键名一致）。
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

/* ---- config.sh 固定值（C 版 cfg 未包含，按固件默认硬编码） ---- */
#define RECORD_FILE_TIME_FORMAT "%Y%m%d%H%M%S"
#define RECORD_SPLIT_MINUTES    1		/* config.sh RECORD_SPLIT */
#define RECORD_MAX_USAGE_PCT    95		/* config.sh RECORD_MAX_USAGE */
#define RECORD_SUBSTREAM_DEF    "false"	/* config.sh RECORD_SUBSTREAM */
#define RECORD_REMOTE_ROOT_DEF  "upload"	/* config.sh RECORD_REMOTE_ROOT */
#define MAIN_STREAM_ENABLED     "true"	/* config.sh MAIN_STREAM_* */
#define MAIN_STREAM_CODEC       "h264"
#define MAIN_STREAM_SIZE        "1920x1080"
#define MAIN_STREAM_FPS         "10"
#define SUB_STREAM_ENABLED      "true"	/* config.sh SUB_STREAM_* */
#define SUB_STREAM_CODEC        "h264"
#define SUB_STREAM_SIZE         "640x360"
#define SUB_STREAM_FPS          "15"
#define STREAM_SUBSTREAM_DEF    "true"	/* config.sh STREAM_SUBSTREAM */
#define SRS_HOST_DEFAULT        "192.168.250.100"	/* config.sh SRS_HOST 兜底（局域网服务器） */
#define SRS_PORT_DEFAULT        "1935"
#define SRS_APP_DEFAULT         "live"

/* ---- config.sh 超时/节奏常量 ---- */
#define RECORD_FINALIZE_WAIT_SEC    3	/* RECORD_FINALIZE_WAIT_SEC */
#define RECORD_VERIFY_INTERVAL_SEC  1	/* RECORD_START_VERIFY_INTERVAL_SEC */
#define STREAM_RELOAD_WAIT_SEC      2	/* STREAM_RELOAD_WAIT_SEC */
#define STREAM_START_WAIT_SEC       2	/* STREAM_START_WAIT_SEC */
#define STREAM_START_SETTLE_SEC     3	/* feature_stream_start 的 sleep 3 */
#define STREAM_STOP_SETTLE_SEC      2	/* feature_stream_stop 的 sleep 2 */
#define MAJESTIC_PID_FILE           "/var/run/majestic.pid"

/* ------------------------------------------------------------------ */
/* 业务互斥 + duration 定时（g_biz_mutex 持有期间访问）                   */
/* ------------------------------------------------------------------ */

/* bash business_action.lock 的进程内等价物：命令 worker 与 duration reset 串行 */
static pthread_mutex_t g_biz_mutex = PTHREAD_MUTEX_INITIALIZER;
static time_t g_dur_deadline;           /* 0 = 无定时 */
static char   g_dur_task[64];
static char   g_dur_url[512];

static void duration_arm(long long sec, const char *task_id, const char *url)
{
	g_dur_deadline = 0;
	g_dur_task[0] = '\0';
	g_dur_url[0] = '\0';
	if (sec <= 0)
		return;		/* feature_duration_start：0 取消不启动 */
	g_dur_deadline = time(NULL) + (time_t)sec;
	snprintf(g_dur_task, sizeof(g_dur_task), "%s", task_id ? task_id : "");
	snprintf(g_dur_url, sizeof(g_dur_url), "%s", url ? url : "");
}

static void duration_cancel(void)
{
	g_dur_deadline = 0;
}

/* ------------------------------------------------------------------ */
/* 基础工具                                                             */
/* ------------------------------------------------------------------ */

static void rt_snapshot(enc_runtime_t *out)
{
	pthread_mutex_lock(&g_app.rt_mutex);
	*out = g_app.rt;
	pthread_mutex_unlock(&g_app.rt_mutex);
}

static bool state_is(const char *key)
{
	char buf[16];

	return state_get_str(key, buf, sizeof(buf)) && !strcmp(buf, "true");
}

static void state_set_bool(const char *key, bool v)
{
	state_set_str(key, v ? "true" : "false");
}

/* state.sh state_recompute_idle：录像或推流中即非空闲 */
static void recompute_idle(void)
{
	state_set_bool("is_idle",
		       !(state_is("is_recording") || state_is("is_publishing")));
}

static void result_ok(feat_result_t *r, int code, const char *status)
{
	r->code = code;
	snprintf(r->status, sizeof(r->status), "%s", status);
}

/* 向 r->extra_json 追加 data 字段（不含大括号） */
static void extra_add(feat_result_t *r, const char *fmt, ...)
{
	char     tmp[400];
	va_list  ap;
	size_t   l;

	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	l = strlen(r->extra_json);
	snprintf(r->extra_json + l, sizeof(r->extra_json) - l, "%s%s",
		 l > 0 ? "," : "", tmp);
}

static const char *record_root(const enc_cfg_t *c)
{
	return c->record_root[0] ? c->record_root : ENCM_RECORD_ROOT;
}

/* ------------------------------------------------------------------ */
/* yaml-cli + HUP（全部持 majestic 文件锁调用）                          */
/* ------------------------------------------------------------------ */

/* yaml-cli 路径规范（v0.0.4 板侧实测 2026-09-01）：
 *   key 必须以 '.' 开头（.outgoing.server / .video0.enabled / .records.enabled …）；
 *   缺失 '.' 的 "outgoing.server" 会把 key 解析失败退化成追加重复节点：
 *     RC=1、val=[]、后续每次 set 都新加一行同级 server: → majestic 首条 empty
 *     胜出 → 推流永远不建立 outgoing（SRS no_established_to_1935）。
 *   dot-key 正确行为：set RC=0 就地覆盖、get RC=0 拿值、-d RC=0 物理删除、
 *   set "" RC=0 把值清空。
 * 本 helper 统一在 yaml_set/yaml_del 入口 dot_ensure()，对调用端省略 dot 的
 * 30+ 处老调用透明兼容；显式带 '.' 的 key（如 L1339 ".records.enabled"）原样。 */
static void dot_ensure(char *dst, size_t dsz, const char *key)
{
	size_t o = 0;
	if (!dst || dsz < 2) return;
	if (!key || !key[0]) { dst[0] = '\0'; return; }
	if (key[0] != '.') dst[o++] = '.';
	for (; key[0] && o + 1 < dsz; key++) dst[o++] = *key;
	dst[o] = '\0';
}

static int yaml_set(const enc_cfg_t *c, const char *key,
		    const char *value, bool quote)
{
	char qc[600], qv[1100], cmd[1900], dk[160];

	dot_ensure(dk, sizeof(dk), key);
	shell_quote(qc, sizeof(qc),
		    c->majestic_conf[0] ? c->majestic_conf : ENCM_MAJESTIC_CONF);
	if (quote)
		shell_quote(qv, sizeof(qv), value);
	else
		snprintf(qv, sizeof(qv), "%s", value);
	snprintf(cmd, sizeof(cmd), "%s -i %s -s %s %s",
		 c->yaml_cli[0] ? c->yaml_cli : ENCM_YAML_CLI, qc, dk, qv);
	if (run_cmd(cmd, 10, NULL, 0) != 0) {
		log_msg(ENCM_LOG_ERROR, "media: yaml set failed key=%s dk=%s",
			key, dk);
		return -1;
	}
	return 0;
}

/* yaml_del：yaml-cli -i <conf> -d <dot-key>，物理删除 key 行（RC=0 成功）。
 * 用于 STOP 清空 outgoing.server：比 "-s '' 留空 server:" 更干净，
 * 避免 majestic 某些构建把空字符串理解成"沿用上次"。
 * ⚠ yaml-cli v0.0.4 quirk：当 key 已值空 (  server: 无右值) ，-d 返回 RC=1
 *   非 0（非 bug，"空 key 不能删除"）；调用方须在调用前用 yaml_get_* 判断
 *   是否有非空值，必要时退化到 -s '' 空串清零或直接跳过。 */
static int yaml_del(const enc_cfg_t *c, const char *key)
{
	char qc[600], cmd[1900], dk[160];

	dot_ensure(dk, sizeof(dk), key);
	shell_quote(qc, sizeof(qc),
		    c->majestic_conf[0] ? c->majestic_conf : ENCM_MAJESTIC_CONF);
	snprintf(cmd, sizeof(cmd), "%s -i %s -d %s",
		 c->yaml_cli[0] ? c->yaml_cli : ENCM_YAML_CLI, qc, dk);
	if (run_cmd(cmd, 10, NULL, 0) != 0) {
		log_msg(ENCM_LOG_ERROR, "media: yaml del failed key=%s dk=%s",
			key, dk);
		return -1;
	}
	return 0;
}

/* yaml_get：读回 key 字符串值。返回 0=读成功 (即使空字符串也算) ；-1=get
 * 异常 (yaml-cli rc!=0 / 路径错)。dst 可空。yaml-cli v0.0.4 quirk：
 *   -g .outgoing.server → 值空返回 RC=0 且 stdout=空；key 不存在可能 RC=1 */
static int yaml_get(const enc_cfg_t *c, const char *key, char *dst, size_t dsz)
{
	char qc[600], cmd[1900], dk[160], out[512];
	int  rc;

	if (dst && dsz) dst[0] = '\0';
	dot_ensure(dk, sizeof(dk), key);
	shell_quote(qc, sizeof(qc),
		    c->majestic_conf[0] ? c->majestic_conf : ENCM_MAJESTIC_CONF);
	snprintf(cmd, sizeof(cmd), "%s -i %s -g %s",
		 c->yaml_cli[0] ? c->yaml_cli : ENCM_YAML_CLI, qc, dk);
	out[0] = '\0';
	rc = run_cmd(cmd, 10, out, sizeof(out));
	if (rc != 0) {
		log_msg(ENCM_LOG_INFO,
			"media: yaml get miss key=%s dk=%s rc=%d",
			key, dk, rc);
		return -1;
	}
	/* 去尾 \n\r */
	size_t l = strlen(out);
	while (l && (out[l-1]=='\n' || out[l-1]=='\r')) out[--l]='\0';
	if (dst && dsz) snprintf(dst, dsz, "%s", out);
	return 0;
}

/* common.sh stream_service_start_if_missing：清旧 PID 文件 + init 脚本恢复 */
static int majestic_recover(const enc_cfg_t *c)
{
	char cmd[340];
	int  rc;

	snprintf(cmd, sizeof(cmd), "rm -f %s; %s start", MAJESTIC_PID_FILE,
		 c->majestic_init[0] ? c->majestic_init : ENCM_MAJESTIC_INIT);
	rc = run_cmd(cmd, 15, NULL, 0);
	sleep(STREAM_START_WAIT_SEC);
	return (rc == 0 && proc_running("majestic")) ? 0 : -1;
}

/* S95majestic restart: 用于 records.enabled / OSD / video0.enabled 等
 * Majestic 已知"仅 HUP 不重新评估配置"的字段变更场景。
 * - 先 stop（不管是否运行），再 start + settle wait，
 * - 重启失败走 majestic_recover 兜底一次；
 * - 最终 majestic 进程存在才判成功。
 * 2026-09-01 R7 修复：record_start verification 100% 超时（10s 内无 mp4）
 * 根因 = records.enabled=true 下发后 SIGHUP 未加载（同 OSD 问题），
 * 必须 restart 才能使 majestic 真正开写 /mnt/mmcblk0p1/%F/【mp4】。 */
static int majestic_restart(const enc_cfg_t *c)
{
	const char *init = c->majestic_init[0] ? c->majestic_init : ENCM_MAJESTIC_INIT;
	char       cmd[256];

	snprintf(cmd, sizeof(cmd), "%s stop", init);
	run_cmd(cmd, 10, NULL, 0);
	sleep(STREAM_STOP_SETTLE_SEC);

	snprintf(cmd, sizeof(cmd), "rm -f %s; %s start", MAJESTIC_PID_FILE, init);
	if (run_cmd(cmd, 15, NULL, 0) != 0 || !proc_running("majestic")) {
		log_msg(ENCM_LOG_WARN, "media: majestic restart S95 start failed, fallback recover");
		if (majestic_recover(c) != 0) {
			log_msg(ENCM_LOG_ERROR, "media: majestic restart+recover both failed");
			return -1;
		}
	}
	sleep(STREAM_START_SETTLE_SEC);
	return proc_running("majestic") ? 0 : -1;
}

/* common.sh stream_service_reload_or_recover：HUP；进程不在/退出则恢复启动。
 * 仅用于 outgoing.server / bitrate 等 Majestic 文档里确认 SIGHUP 生效的字段。
 * records.enabled / video0.enabled / OSD 等需要全量评估配置的字段，
 * 必须调用 majestic_restart()，不要误用 stream_reload() —— 2026-09-01 R7。 */
static int stream_reload(const enc_cfg_t *c)
{
	if (!proc_running("majestic"))
		return majestic_recover(c);
	if (run_cmd("killall -HUP majestic", 10, NULL, 0) != 0)
		log_msg(ENCM_LOG_WARN, "media: killall -HUP majestic failed");
	sleep(STREAM_RELOAD_WAIT_SEC);
	if (!proc_running("majestic"))
		return majestic_recover(c);
	return 0;
}

/* feature_media_apply_video_profile：主/子码流 profile（bitrate 留空跳过） */
static int apply_video_profile(const enc_cfg_t *c)
{
	if (yaml_set(c, "video0.enabled", MAIN_STREAM_ENABLED, false) ||
	    yaml_set(c, "video0.codec", MAIN_STREAM_CODEC, true) ||
	    yaml_set(c, "video0.size", MAIN_STREAM_SIZE, true) ||
	    yaml_set(c, "video0.fps", MAIN_STREAM_FPS, false) ||
	    yaml_set(c, "video1.enabled", SUB_STREAM_ENABLED, false) ||
	    yaml_set(c, "video1.codec", SUB_STREAM_CODEC, true) ||
	    yaml_set(c, "video1.size", SUB_STREAM_SIZE, true) ||
	    yaml_set(c, "video1.fps", SUB_STREAM_FPS, false))
		return -1;
	return 0;
}

/* feature_record_apply_profile：RECORD_PATH 是 Majestic 本地落盘路径模板 */
static int apply_record_profile(const enc_cfg_t *c)
{
	char path[300], split[16], usage[16];

	snprintf(path, sizeof(path), "%s/%%F", record_root(c));
	snprintf(split, sizeof(split), "%d", RECORD_SPLIT_MINUTES);
	snprintf(usage, sizeof(usage), "%d", RECORD_MAX_USAGE_PCT);
	if (yaml_set(c, "records.path", path, true) ||
	    yaml_set(c, "records.split", split, false) ||
	    yaml_set(c, "records.maxUsage", usage, false) ||
	    yaml_set(c, "records.substream", RECORD_SUBSTREAM_DEF, false))
		return -1;
	return 0;
}

static int record_enable(const enc_cfg_t *c)
{
	return yaml_set(c, "records.enabled", "true", false);
}

static int record_disable(const enc_cfg_t *c)
{
	return yaml_set(c, "records.enabled", "false", false);
}

/* feature_stream_apply_outgoing_profile */
static int apply_outgoing_enable(const enc_cfg_t *c, const char *url)
{
	return yaml_set(c, "outgoing.server", url, true) ||
	       yaml_set(c, "outgoing.substream", STREAM_SUBSTREAM_DEF, false) ||
	       yaml_set(c, "outgoing.enabled", "true", false);
}

/* feature_stream_disable_outgoing：server 必须清空防旧地址残留。
 * yaml-cli v0.0.4 quirk:
 *   - key 存在且**非空**时 `-d` → RC=0（物理删除）；
 *   - key 存在但**值空** (`server:` 无右值) 时 `-d` → RC=1 不删除
 *     （这不是错误而是 yaml-cli 语义：空值 key 不支持 delete）；
 *   - 对 RC=1 退化 case 再做一次 `-s ''` 清零保底或直接 SKIP（值空 = 已清理）。
 * 最终只在 substream/enabled 两项真正 write 失败时 return -1，server 清理退化成
 * INFO 日志——否则 5b `r.code != 0` 会让 STOP 跳过 dedup_remove_pair，下一轮 START
 * 必定 duplicate replay (Bug A 连锁)。 */
static int apply_outgoing_disable(const enc_cfg_t *c)
{
	char cur[512];
	int  rc;

	cur[0] = '\0';
	rc = yaml_get(c, "outgoing.server", cur, sizeof(cur));
	/* 三种"已经干净"的状态：get miss(-1)=key 不存在；或 get ok 但 cur="" 。
	 * 这两种情况下 del 都必然 rc=1（yaml-cli quirk），直接跳过省 WARN。 */
	if (rc == 0 && cur[0]) {
		if (yaml_del(c, "outgoing.server") != 0) {
			/* del 失败（少见）降级：-s '' 覆盖写空值 */
			log_msg(ENCM_LOG_WARN,
				"media: outgoing.server del fallback -> set empty");
			yaml_set(c, "outgoing.server", "", true);
		}
	} else {
		log_msg(ENCM_LOG_INFO,
			"media: outgoing.server already clean (get_rc=%d val_len=%zu)",
			rc, strlen(cur));
	}
	if (yaml_set(c, "outgoing.substream", STREAM_SUBSTREAM_DEF, false) != 0 ||
	    yaml_set(c, "outgoing.enabled", "false", false) != 0) {
		log_msg(ENCM_LOG_ERROR,
			"media: outgoing disable: substream/enabled set failed");
		return -1;
	}
	return 0;
}

/* feature_media_disable_all_video：仅 stopStream/reset 关闭视频链路 */
static int disable_all_video(const enc_cfg_t *c)
{
	return yaml_set(c, "video0.enabled", "false", false) ||
	       yaml_set(c, "video1.enabled", "false", false);
}

/* ------------------------------------------------------------------ */
/* 推流 URL / 路径工具                                                   */
/* ------------------------------------------------------------------ */

/* build_stream_name：SRS_STREAM_PREFIX 默认 stream_<device_id> */
static void build_stream_name(const enc_cfg_t *c, char *out, size_t sz)
{
	snprintf(out, sz, "stream_%s", device_id_get(c));
}

/* build_stream_url_with_name：控制端下发 URL 统一替换最后一级为 stream_name；
 * 之后强制走一次"path 级次归一化 + SRS_APP_DEFAULT 注入"：
 *   - 约定最终必须是 4 段式 scheme://host:port/<app>/<stream_name>；
 *   - 最终 app 段必须严格相等 SRS_APP_DEFAULT("live")；段数≥2 但首段≠"live"
 *     时，同样视为"伪两级缺app"（典型 case：T1 控制端只下发
 *     rtmp://host:1935/stream_XXX → build_stream_url_with_name 尾部补同名成
 *     stream_XXX/stream_XXX，段数=2但首段非live → majestic 仍把 stream_XXX
 *     解析为 RTMP app → SRS 端注册 app=live → 2051 StreamNameEmpty /
 *     outgoing.server 看似写了实际为空(失效)）；
 *   - 非 live 的多段 path 取末段重当 stream name，其余段丢弃，host 与 name
 *     之间强制插入 SRS_APP_DEFAULT("live")，保证最终落盘严格 4 段式。
 * 设计 §6.2 / protocol 修复记录：2026-09-01 ROUND2（"outgoing server为空、
 *   ACK不报错"bug 精修 —— ROUND1 只看段数导致伪两级漏归一化。）。 */
static void stream_url_ensure_app_default(char *url, size_t sz)
{
	const char *scheme_end;
	const char *path;
	char rebuild[1024];
	size_t n_seg;
	const char *p;
	const char *slash;
	char name_part[256];
	char first_seg[128];
	size_t host_part_len;
	size_t base_len;
	size_t fs_len;

	if (!url || !url[0] || sz < 16)
		return;
	scheme_end = strstr(url, "://");
	if (!scheme_end)
		return;
	path = strchr(scheme_end + 3, '/');
	if (!path || !path[1])
		return;
	/* 段数统计（a/b → 两段，对应 1 个非末尾 '/') */
	n_seg = 0;
	for (p = path + 1; *p; ) {
		n_seg++;
		p = strchr(p, '/');
		if (!p) break;
		p++;
		while (*p == '/') p++;
		if (!*p) break;
	}
	/* 取第一段（候选 app 段）：从 path+1 到下一个 '/' 或末尾 */
	slash = strchr(path + 1, '/');
	if (slash) {
		fs_len = (size_t)(slash - (path + 1));
	} else {
		fs_len = strlen(path + 1);
	}
	if (fs_len >= sizeof(first_seg))
		fs_len = sizeof(first_seg) - 1;
	memcpy(first_seg, path + 1, fs_len);
	first_seg[fs_len] = '\0';
	/* 满足：段数>=2 且 第一段==SRS_APP_DEFAULT → 已是合法 4 段式，不动 */
	if (n_seg >= 2 && fs_len > 0 &&
	    strcmp(first_seg, SRS_APP_DEFAULT) == 0) {
		return;
	}
	/* 其余情况：末段重作为 stream_name，host 与 name 之间强制插 /live/ */
	host_part_len = (size_t)(path - url);
	/* 末段 = strrchr(path+1, '/') 之后；如果只有一段（无第二个'/')，那就 path+1 */
	slash = strrchr(path + 1, '/');
	if (slash) {
		snprintf(name_part, sizeof(name_part), "%s", slash + 1);
	} else {
		snprintf(name_part, sizeof(name_part), "%s", path + 1);
	}
	base_len = strlen(name_part);
	while (base_len > 0 && name_part[base_len - 1] == '/')
		name_part[--base_len] = '\0';
	if (!name_part[0])
		return;
	snprintf(rebuild, sizeof(rebuild), "%.*s/%s/%s",
		 (int)host_part_len, url, SRS_APP_DEFAULT, name_part);
	if (strlen(rebuild) + 1 > sz)
		return;
	snprintf(url, sz, "%s", rebuild);
}

static void build_stream_url_with_name(const char *url, const char *name,
				       char *out, size_t sz)
{
	char  base[600];
	size_t l;
	char  *scheme, *p;

	snprintf(base, sizeof(base), "%s", url);
	l = strlen(base);
	while (l > 0 && base[l - 1] == '/')
		base[--l] = '\0';
	/* ROUND2 精修：仅当 scheme 后存在 ≥2 段 path 且首段=SRS_APP_DEFAULT 时
	 * 才"替换最后一级"；否则一律 append/<name>，再让
	 * stream_url_ensure_app_default 统一做"非 live 首段强制插 live"。
	 * 原因：build_stream_url(requested=单级path) 时，老逻辑"只要有1个'/'
	 * 就替换最后一级"仍可能产出 stream_XXX/stream_XXX 的伪两级，majestic
	 * 把首段当 RTMP app、SRS 只认 live=app → outgoing 失效为空。*/
	scheme = strstr(base, "://");
	if (scheme) {
		const char *pp;
		const char *path_start;
		const char *next_slash;
		size_t seg1_len;
		int    path_segments;
		char   seg1_buf[128];

		path_start = strchr(scheme + 3, '/');
		if (path_start && path_start[1]) {
			next_slash = strchr(path_start + 1, '/');
			if (next_slash)
				seg1_len = (size_t)(next_slash - (path_start + 1));
			else
				seg1_len = strlen(path_start + 1);
			if (seg1_len >= sizeof(seg1_buf))
				seg1_len = sizeof(seg1_buf) - 1;
			memcpy(seg1_buf, path_start + 1, seg1_len);
			seg1_buf[seg1_len] = '\0';
			path_segments = 1;
			for (pp = path_start + 1; *pp; ) {
				pp = strchr(pp, '/');
				if (!pp) break;
				pp++;
				while (*pp == '/') pp++;
				if (*pp) path_segments++;
			}
			if (path_segments >= 2 &&
			    strcmp(seg1_buf, SRS_APP_DEFAULT) == 0) {
				char *last = strrchr(base, '/');

				if (last)
					*last = '\0';
			}
		}
	} else {
		/* 无 scheme: 保持 legacy —— 含 '/' 就替换末段 */
		p = strchr(base, '/');
		if (p && strchr(p + 1, '/')) {
			char *last = strrchr(base, '/');

			if (last)
				*last = '\0';
		}
	}
	snprintf(out, sz, "%s/%s", base, name);
	/* ROUND2 最终统一归一化（段数不够或首段非 live 都强制插/live） */
	stream_url_ensure_app_default(out, sz);
}

/* build_stream_url：命令下发 → registerAck/runtime SRS → config.sh 默认兜底。
 * 严格对齐 bash 版 feature_engine.sh build_stream_url：
 *   registerAck/default fallback 必须用 4 段格式 "rtmp://host:port/<SRS_APP>/<stream_name>"，
 *   不能把 stream_name 直接挂在 host:port 后，否则 RTMP app 段=SRS_APP（协议固定 "live"）
 *   缺失，majestic 会把唯一的一段 path 当 app、playpath 为空 → SRS 2051 StreamNameEmpty。
 * 控制端如果显式下发 data.streamUrl（含多段 path），走 build_stream_url_with_name，
 * 仍由控制端保证协议正确性，此处不改变语义。 */
static int build_stream_url(const enc_cfg_t *c, const char *requested,
			    const enc_runtime_t *rt, char *out, size_t sz)
{
	char name[192];
	const char *scheme_end;
	const char *path;
	char  hostonly[256];

	build_stream_name(c, name, sizeof(name));
	if (requested && requested[0]) {
		build_stream_url_with_name(requested, name, out, sz);
		return 0;
	}
	if (rt && rt->srs_url[0]) {
		/* rt.srs_url 有两种形态：
		 *   (a) registerAck 下发 host/port 后 C 端拼成的 "rtmp://host:port"
		 *       （path 为空，协议里约定 SRS_APP 本地补 "live"）
		 *   (b) 控制端下发完整 URL（含 path），例如 "rtmp://host:port/live/xxx"
		 * 对 (a) 必须显式插入 /SRS_APP_DEFAULT/ 段（用户说的 "加 live"）；
		 * 对 (b) 继续走 build_stream_url_with_name 替换最后一级为 stream_name。 */
		if (strstr(rt->srs_url, "://")) {
			scheme_end = strstr(rt->srs_url, "://");
			path = strchr(scheme_end + 3, '/');
			if (!path) {
				/* 无 path = registerAck 只给了 host:port → 补 /live/<name> */
				snprintf(out, sz, "%s/%s/%s",
					 rt->srs_url, SRS_APP_DEFAULT, name);
			} else if (!path[1]) {
				/* path 只有一个 "/" = 尾部悬挂斜杠，同样补两段 */
				snprintf(hostonly, sizeof(hostonly), "%.*s",
					 (int)(path - rt->srs_url), rt->srs_url);
				snprintf(out, sz, "%s/%s/%s",
					 hostonly, SRS_APP_DEFAULT, name);
			} else {
				/* path 至少一段 → 按原协议替换最后一级 = stream_name */
				build_stream_url_with_name(rt->srs_url, name, out, sz);
			}
		} else {
			/* 老形态：纯 host 或 host:port，没有 scheme */
			snprintf(out, sz, "rtmp://%s/%s/%s", rt->srs_url,
				 SRS_APP_DEFAULT, name);
		}
		return 0;
	}
	/* config.sh SRS_HOST/SRS_PORT/SRS_APP 默认值兜底（bash 对齐 4 段） */
	snprintf(out, sz, "rtmp://%s:%s/%s/%s", SRS_HOST_DEFAULT,
		 SRS_PORT_DEFAULT, SRS_APP_DEFAULT, name);
	return 0;
}

/* state_set_record_session_time：RECORD_FILE_TIME_FORMAT 本地时间 */
static void session_time_now(char *out, size_t sz)
{
	time_t    t = time(NULL);
	struct tm tmv;

	localtime_r(&t, &tmv);
	strftime(out, sz, RECORD_FILE_TIME_FORMAT, &tmv);
}

/* ftp_path（远端根目录，默认 /）与相对路径拼接 */
static void join_remote_path(const char *ftp_path, const char *rel,
			     char *out, size_t sz)
{
	char base[256];
	size_t l;

	if (!ftp_path || !ftp_path[0] || !strcmp(ftp_path, "/")) {
		snprintf(out, sz, "%s", rel[0] == '/' ? rel + 1 : rel);
		return;
	}
	snprintf(base, sizeof(base), "%s", ftp_path);
	l = strlen(base);
	while (l > 0 && base[l - 1] == '/')
		base[--l] = '\0';
	snprintf(out, sz, "%s/%s", base, rel[0] == '/' ? rel + 1 : rel);
}

/* build_ftp_report_url：上报给控制端的不带凭据 URL（rt 无端口字段，按 21 省略） */
static void ftp_report_url(const enc_runtime_t *rt, const char *rel,
			   char *out, size_t sz)
{
	char joined[768];

	join_remote_path(rt ? rt->ftp_path : "", rel, joined, sizeof(joined));
	snprintf(out, sz, "ftp://%s/%s",
		 rt && rt->ftp_host[0] ? rt->ftp_host : "", joined);
}

/* ------------------------------------------------------------------ */
/* 录像文件查找                                                          */
/* ------------------------------------------------------------------ */

static bool mp4_suffix(const char *name)
{
	size_t l = strlen(name);

	return l > 4 && !strcasecmp(name + l - 4, ".mp4");
}

/* bash list_record_files（find+sort）+ feature_find_latest_record_file：
 * 尽量用 YYYY-MM-DD/HH-MM-cam0.mp4 目录+文件名解析出来的 epoch 判定
 * "最新"（VFAT mtime 因 TZ 挂载缺省可能偏差 13h+，会把旧目录的假新日期抬到
 *  真实分片之前，导致 walk_latest 永远取 stat mtime 假大值的老文件 → 新分片
 *  永远无法在 wait_for_new_record_file 中命中 path≠prev 条件（SMOKE7-9
 *  3 台 encoder verification 100% 超时的第三层根因）。
 * 解析失败才回退 stat mtime；宽容 min_ts：按 TS_VFAT_TZ_SLACK=13h；
 * 最终比较：若两者 ts 差 ≤ 1h（认为同一 1min split 窗口内），取 path 字典序大
 * 的（=HH-MM-cam0.mp4 后一分钟）；否则取 ts 大的。 */
#define FAT_TZ_SLACK_SEC  (13 * 3600)

/* 解析 /path/YYYY-MM-DD/HH-MM-cam0.mp4 → epoch（board localtime）；失败返回 0 */
static long long epoch_from_record_path(const char *p)
{
	const char *slash1, *slash2, *dash, *dash2, *dash3;
	char       *endp;
	long long   y, mo, d, h, mi;
	struct tm   tm;
	time_t      t;

	if (!p || !*p)
		return 0;
	slash1 = strrchr(p, '/');	/* /HH-MM-cam0.mp4 前 slash */
	if (!slash1)
		return 0;
	dash = strchr(slash1 + 1, '-');	/* HH '-' MM */
	if (!dash || dash - (slash1 + 1) != 2)
		return 0;
	dash2 = strchr(dash + 1, '-');	/* MM '-' cam... */
	if (!dash2 || dash2 - (dash + 1) != 2)
		return 0;
	/* YYYY-MM-DD 前一个 slash2: 从 slash1-1 往回找 '/' */
	slash2 = NULL;
	{
		const char *s;
		for (s = slash1 - 1; s >= p; s--) {
			if (*s == '/') { slash2 = s; break; }
		}
	}
	if (!slash2 || slash1 - slash2 != 11 /* /YYYY-MM-DD (10) + / */)
		return 0;
	dash3 = strchr(slash2 + 1, '-');	/* YYYY '-' MM */
	if (!dash3 || dash3 - (slash2 + 1) != 4)
		return 0;
	y  = strtoll(slash2 + 1, &endp, 10); if (endp != dash3) return 0;
	mo = strtoll(dash3 + 1, &endp, 10);   if (endp != dash3 + 4) return 0;
	d  = strtoll(dash3 + 4, &endp, 10);   if (endp != slash1) return 0;
	h  = strtoll(slash1 + 1, &endp, 10);  if (endp != dash) return 0;
	mi = strtoll(dash + 1, &endp, 10);    if (endp != dash2) return 0;
	if (y < 1970 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31
	    || h < 0 || h > 23 || mi < 0 || mi > 59)
		return 0;
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = (int)y - 1900;
	tm.tm_mon  = (int)mo - 1;
	tm.tm_mday = (int)d;
	tm.tm_hour = (int)h;
	tm.tm_min  = (int)mi;
	tm.tm_isdst = -1;
	t = mktime(&tm);
	return (t == (time_t)-1) ? 0 : (long long)t;
}

static void walk_latest(const char *dir, long long min_ts,
			char *best, size_t sz, long long *best_ts, int depth)
{
	DIR           *d;
	struct dirent *e;

	if (depth > 8 || !(d = opendir(dir)))
		return;
	while ((e = readdir(d)) != NULL) {
		char       path[1024];
		struct stat st;
		long long  stat_ts, name_ts, ts;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (lstat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			walk_latest(path, min_ts, best, sz, best_ts, depth + 1);
			continue;
		}
		if (!S_ISREG(st.st_mode) || !mp4_suffix(e->d_name))
			continue;
		stat_ts = (long long)st.st_mtime;
		name_ts = epoch_from_record_path(path);
		/* ts strategy: 文件名解析成功 → 只用 name_ts；VFAT 会把 6-16
		 * 真实文件的 stat_ts 写回成 8-30 等小日期的假象，或 9-1 老
		 * 目录的旧文件 stat_ts 被 VFAT 假 UTC-8 写成未来 1788 亿
		 * epoch 而始终压过真实新分片。name_ts 来自 board
		 * date → majestic sprintf("%F/%H-%M") → 无 VFAT 翻译。 */
		ts = name_ts ? name_ts : stat_ts;
		if (ts < min_ts - FAT_TZ_SLACK_SEC)
			continue;
		if (best[0] == '\0' || ts > *best_ts ||
		    (ts == *best_ts && strcmp(path, best) > 0)) {
			*best_ts = ts;
			snprintf(best, sz, "%s", path);
		}
	}
	closedir(d);
}

/* feature_wait_for_new_record_file：轮询首个新 mp4（新文件或旧文件变大） */
static int wait_for_new_record_file(const enc_cfg_t *c, time_t min_ts,
				    const char *prev_file, long long prev_size,
				    char *out, size_t sz)
{
	char root[512];
	/* 2026-09-01 R9: 无论 cfg 是否硬编码了 record_verify_timeout_sec=10，
	 * 录制首片验证基线固定 ≥ 45s：
	 *   - majestic_restart() = S95 stop + start + majestic settle
	 *     + video pipeline rebind ISP/sensor/mpp 保守 15~25s
	 *   - mp4 moov+mdat 首帧实际落盘(vfat flush) 2~8s
	 *   - 1Gbps SD 最坏情况 缓冲刷入延迟 ~5s
	 * 实际 cfg 可能通过 default.conf 覆盖为 10s（旧经验值 SIGHUP 场景），
	 * 此函数无条件基线 45s，避免 encodermain.conf 误配/历史值覆盖产生
	 * verification 100% 超时（SMOKE7-9 复现）。 */
	int  baseline = 45;
	int  timeout  = c->record_verify_timeout_sec;
	if (timeout < baseline)
		timeout = baseline;
	int  waited = 0;

	snprintf(root, sizeof(root), "%s", record_root(c));
	while (waited < timeout && !g_app.stopping) {
		char       cand[1024];
		long long  ts = 0;
		struct stat st;

		cand[0] = '\0';
		walk_latest(root, (long long)min_ts, cand, sizeof(cand), &ts, 0);
		if (cand[0] && stat(cand, &st) == 0 && st.st_size > 0 &&
		    (strcmp(cand, prev_file) != 0 ||
		     (long long)st.st_size > prev_size)) {
			snprintf(out, sz, "%s", cand);
			return 0;
		}
		sleep(RECORD_VERIFY_INTERVAL_SEC);
		waited += RECORD_VERIFY_INTERVAL_SEC;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* 运行目录 / 命名工具（对齐 bash common.sh + feature_engine.sh）         */
/* ------------------------------------------------------------------ */

/* config.sh RECORD_REMOTE_ROOT（cfg 未包含，按固件默认硬编码） */
#define RECORD_REMOTE_ROOT       "upload"
#define STARTUP_RECORDS_SPLIT    "20"	/* MAJESTIC_STARTUP_RECORDS_SPLIT */
#define STARTUP_RECORDS_PATH     "/mnt/mmcblk0p1/%F"
#define STARTUP_RECORDS_MAXUSAGE "95"
#define STARTUP_VIDEO0_FPS       "5"
#define STARTUP_VIDEO0_BITRATE   "1280"
#define STARTUP_VIDEO1_SIZE      "960x576"
#define STARTUP_VIDEO1_FPS       "15"
#define STARTUP_VIDEO1_BITRATE   "384"

/* build_record_remote_path：upload/<device_id>/<record_id>/<file_name>。
 * 标准命名 build_record_output_name / named 入口准备已移交 upload.c，
 * stop 路径经 upload_sync_final 返回远端文件名。 */
static void build_record_remote_path(const enc_cfg_t *c, const char *record_id,
				     const char *file_name, char *out, size_t sz)
{
	snprintf(out, sz, "%s/%s/%s/%s", RECORD_REMOTE_ROOT,
		 device_id_get(c), record_id, file_name);
}

/* ---- segment_manifest（bash 工具兼容投影；追加在 upload.c 完成，
 *      C 侧查重/账本在 encdb，此处仅启动录像/收尾时清空） ---- */

static void manifest_path(char *out, size_t sz)
{
	snprintf(out, sz, "%s/segment_manifest",
		 g_app.cfg.state_dir[0] ? g_app.cfg.state_dir : ENCM_STATE_DIR);
}

static void manifest_reset(void)
{
	char path[600];

	manifest_path(path, sizeof(path));
	unlink(path);
	state_set_str("segment_manifest", "");
}

/* ------------------------------------------------------------------ */
/* 推流 start/stop（feature_stream_start / feature_stream_stop）         */
/* ------------------------------------------------------------------ */

/* stream_start：普通 stream/start_stream 与 task/start_stream 共用，
 * mode 只影响回包格式（dispatch 负责）。 */
static int stream_start(const enc_cfg_t *c, const char *mode, const cmd_t *cmd,
			feat_result_t *r)
{
	enc_runtime_t rt;
	char push_url[512], active_url[512], cur_task[64], esc[640];

	rt_snapshot(&rt);
	if (build_stream_url(c, cmd->stream_url, &rt, push_url,
			     sizeof(push_url)) != 0 || push_url[0] == '\0') {
		log_msg(ENCM_LOG_ERROR,
			"media: stream start failed mode=%s reason=empty_stream_url",
			mode);
		result_ok(r, -1, "fail");
		return -1;
	}
	/* RESULT_STREAM_URL / RESULT_TASK_ID（task_id 缺省回落 state 当前值） */
	json_escape(esc, sizeof(esc), push_url);
	extra_add(r, "\"streamUrl\":%s", esc);
	if (cmd->task_id[0])
		snprintf(cur_task, sizeof(cur_task), "%s", cmd->task_id);
	else
		state_get_str("current_task_id", cur_task, sizeof(cur_task));
	if (cur_task[0]) {
		json_escape(esc, sizeof(esc), cur_task);
		extra_add(r, "\"taskId\":%s", esc);
	}

	if (state_is("is_recording")) {
		state_get_str("current_stream_url", active_url,
			      sizeof(active_url));
		if (state_is("is_publishing") &&
		    !strcmp(active_url, push_url)) {
			/* 录像期间相同推流只返回当前结果：不刷新 duration、
			 * 不覆盖任务、不重载 Majestic */
			char rec[64];

			state_get_str("current_record_id", rec, sizeof(rec));
			result_ok(r, 0, "streaming");
			log_msg(ENCM_LOG_WARN,
				"media: stream start ignored during active record "
				"stream_url=%s record_id=%s", push_url, rec);
			return 0;
		}
		result_ok(r, -1, "fail");
		log_msg(ENCM_LOG_ERROR,
			"media: stream start rejected during active record "
			"requested_stream_url=%s active_stream_url=%s",
			push_url, active_url);
		return -1;
	}

	log_msg(ENCM_LOG_INFO, "media: stream start request mode=%s stream_url=%s",
		mode, push_url);

	state_get_str("current_stream_url", active_url, sizeof(active_url));
	if (state_is("is_publishing") && !strcmp(active_url, push_url))
		log_msg(ENCM_LOG_WARN,
			"media: stream already running on requested url, "
			"forcing reload to refresh connection");

	if (majestic_lock_acquire(c->majestic_lock_wait_sec) != 0) {
		result_ok(r, -1, "fail");
		return -1;
	}

	if (apply_video_profile(c) == 0 &&
	    apply_outgoing_enable(c, push_url) == 0 &&
	    stream_reload(c) == 0) {
		majestic_lock_release();
		sleep(STREAM_START_SETTLE_SEC);
		state_set_bool("is_publishing", true);
		state_set_str("current_stream_url", push_url);
		if (cmd->task_id[0])
			state_set_str("current_task_id", cmd->task_id);
		recompute_idle();
		/* feature_duration_start：duration 空/0 → 取消不启动 */
		duration_arm(cmd->has_duration ? cmd->duration : 0,
			     cmd->task_id, push_url);
		result_ok(r, 0, "streaming");
		log_msg(ENCM_LOG_INFO,
			"media: stream start success mode=%s stream_url=%s",
			mode, push_url);
		return 0;
	}

	majestic_lock_release();
	result_ok(r, -1, "fail");
	log_msg(ENCM_LOG_ERROR, "media: stream start failed mode=%s stream_url=%s",
		mode, push_url);
	return -1;
}

/* stream_stop：关 outgoing + video0/1（stopRecord 不做这两步） */
static int stream_stop(const enc_cfg_t *c, feat_result_t *r)
{
	log_msg(ENCM_LOG_INFO, "media: stream stop request");

	if (state_is("is_recording")) {
		char rec[64];

		state_get_str("current_record_id", rec, sizeof(rec));
		result_ok(r, -1, "fail");
		log_msg(ENCM_LOG_ERROR,
			"media: stream stop rejected reason=recording_active record_id=%s",
			rec);
		return -1;
	}

	if (majestic_lock_acquire(c->majestic_lock_wait_sec) != 0) {
		result_ok(r, -1, "fail");
		return -1;
	}

	if (apply_outgoing_disable(c) == 0 && disable_all_video(c) == 0 &&
	    stream_reload(c) == 0) {
		majestic_lock_release();
		sleep(STREAM_STOP_SETTLE_SEC);
		state_set_bool("is_publishing", false);
		state_set_str("current_stream_url", "");
		recompute_idle();
		duration_cancel();
		result_ok(r, 0, "idle");
		log_msg(ENCM_LOG_INFO, "media: stream stop success");
		return 0;
	}

	majestic_lock_release();
	result_ok(r, -1, "fail");
	log_msg(ENCM_LOG_ERROR, "media: stream stop failed");
	return -1;
}

/* ------------------------------------------------------------------ */
/* 录像 start/stop（feature_record_start / feature_record_stop）         */
/* ------------------------------------------------------------------ */

static int record_start(const enc_cfg_t *c, const char *mode, const cmd_t *cmd,
			feat_result_t *r)
{
	char        prev_file[1024], verified[1024], sess[32];
	long long   prev_ts = 0, prev_size = 0, start_ts;
	struct stat st;

	if (cmd->record_id[0] == '\0') {
		log_msg(ENCM_LOG_ERROR, "media: record start failed: empty record_id");
		result_ok(r, -1, "fail");
		return -1;
	}
	if (state_is("is_recording")) {
		/* 新 msgId 下的重复开始命令只回 code=1，绝不覆盖活动录像任务 */
		result_ok(r, 1, "duplicate");
		{
			char rec[64];

			state_get_str("current_record_id", rec, sizeof(rec));
			log_msg(ENCM_LOG_WARN,
				"media: record start rejected as duplicate "
				"requested_record_id=%s active_record_id=%s",
				cmd->record_id, rec);
		}
		return 0;
	}
	if (!state_is("is_publishing")) {
		result_ok(r, -1, "fail");
		log_msg(ENCM_LOG_ERROR,
			"media: record start failed record_id=%s reason=stream_not_running",
			cmd->record_id);
		return -1;
	}

	log_msg(ENCM_LOG_INFO,
		"media: record start request mode=%s record_id=%s",
		mode, cmd->record_id);

	start_ts = (long long)time(NULL);
	/* 基线：开始时间点之前已存在的最新 mp4（用于区分"新文件或旧文件变大"） */
	prev_file[0] = '\0';
	walk_latest(record_root(c), 0, prev_file, sizeof(prev_file),
		    &prev_ts, 0);
	if (prev_file[0] && stat(prev_file, &st) == 0)
		prev_size = (long long)st.st_size;

	if (majestic_lock_acquire(c->majestic_lock_wait_sec) != 0) {
		result_ok(r, -1, "fail");
		return -1;
	}

	if (apply_video_profile(c) == 0 && apply_record_profile(c) == 0 &&
	    record_enable(c) == 0 && majestic_restart(c) == 0) {
		/* bash：首个新 mp4 验证在 Majestic 锁内进行（最多 10s）
		 * 注意：2026-09-01 R7 后此处必须使用 majestic_restart()（S95 stop+
		 * start）。Majestic 的 records.enabled / video0.enabled / OSD 三
		 * 类字段仅 SIGHUP 不会重新评估（类似 OSD 已证实的已知问题），
		 * stream_reload（HUP）会导致 wait_for_new_record_file 100%
		 * 超时退出、整条录像链路无法创建 mp4/FTP 分片。 */
		if (wait_for_new_record_file(c, (time_t)start_ts, prev_file,
					     prev_size, verified,
					     sizeof(verified)) != 0) {
			/* R9: 打印 verification timeout=实际使用的 45+s（基线固定），
			 * 不是 cfg 里的默认 10s。 */
			int vt = c->record_verify_timeout_sec < 45
				? 45 : c->record_verify_timeout_sec;
			log_msg(ENCM_LOG_ERROR,
				"media: record start verification failed record_id=%s timeout=%ds",
				cmd->record_id, vt);
			record_disable(c);
			majestic_restart(c);
			majestic_lock_release();
			result_ok(r, -1, "fail");
			return -1;
		}
		majestic_lock_release();
		state_set_bool("is_recording", true);
		state_set_str("current_record_id", cmd->record_id);
		state_set_str("current_record_flow", mode);
		if (cmd->task_id[0])
			state_set_str("current_task_id", cmd->task_id);
		state_set_int("record_start_ts", (long)start_ts);
		session_time_now(sess, sizeof(sess));
		state_set_str("record_session_time", sess);
		state_set_int("segment_no", 0);
		manifest_reset();
		recompute_idle();
		upload_kick();		/* 上传线程进入录像期扫描（bash segment_worker 等价） */
		result_ok(r, 0, "recording");
		log_msg(ENCM_LOG_INFO,
			"media: record start success mode=%s record_id=%s verified=%s",
			mode, cmd->record_id, verified);
		return 0;
	}

	majestic_lock_release();
	result_ok(r, -1, "fail");
	log_msg(ENCM_LOG_ERROR,
		"media: record start failed mode=%s record_id=%s",
		mode, cmd->record_id);
	return -1;
}

/* record_stop：只关 records，推流/video 保持运行（stopStream/reset 才关）。
 * 最终分片同步上传，成功才置成功结果；失败回滚 recording=true 保留上下文可重试。 */
static int record_stop(const enc_cfg_t *c, const char *mode,
		       const char *requested_id, feat_result_t *r)
{
	char        record_id[64], cur[64], cur_task[64];
	char        latest[1024], remote_name[256], rel[600], report[800];
	char        last_id[64], last_task[64], last_name[256], last_url[800];
	long long   start_ts = 0, best_ts = 0, size = 0;
	long        seg_no = 0;
	enc_runtime_t rt;
	struct stat st;

	if (requested_id && requested_id[0])
		snprintf(record_id, sizeof(record_id), "%s", requested_id);
	else
		state_get_str("current_record_id", record_id,
			      sizeof(record_id));
	if (record_id[0] == '\0') {
		log_msg(ENCM_LOG_ERROR, "media: record stop failed: empty record_id");
		result_ok(r, -1, "fail");
		return -1;
	}
	log_msg(ENCM_LOG_INFO, "media: record stop request mode=%s record_id=%s",
		mode, record_id);

	if (!state_is("is_recording")) {
		/* 无活动录像但 recordId=last_record_id：用 last_record_* 重建成功 ACK */
		state_get_str("last_record_id", last_id, sizeof(last_id));
		if (!strcmp(record_id, last_id)) {
			state_get_str("last_record_task_id", last_task,
				      sizeof(last_task));
			state_get_str("last_record_file_name", last_name,
				      sizeof(last_name));
			state_get_str("last_record_file_url", last_url,
				      sizeof(last_url));
			{
				long lsz = 0, lseg = 0;

				state_get_int("last_record_file_size", &lsz);
				state_get_int("last_record_segment_no", &lseg);
				seg_no = lseg;
				size = (long long)lsz;
			}
			snprintf(r->last_file, sizeof(r->last_file), "%s",
				 last_name);
			r->last_size = size;
			if (last_url[0]) {
				json_escape(report, sizeof(report), last_url);
				extra_add(r, "\"fileUrl\":%s", report);
			}
			extra_add(r, "\"segmentNo\":%ld", seg_no);
			if (last_task[0]) {
				json_escape(report, sizeof(report), last_task);
				extra_add(r, "\"taskId\":%s", report);
			}
			result_ok(r, 0,
				  !strcmp(mode, "task") ? "success" : "streaming");
			log_msg(ENCM_LOG_WARN,
				"media: record stop duplicate reused cached result record_id=%s",
				record_id);
			return 0;
		}
		result_ok(r, -1, "fail");
		log_msg(ENCM_LOG_ERROR,
			"media: record stop rejected reason=no_active_record requested_record_id=%s",
			record_id);
		return -1;
	}

	state_get_str("current_record_id", cur, sizeof(cur));
	if (strcmp(record_id, cur) != 0) {
		result_ok(r, -1, "fail");
		log_msg(ENCM_LOG_ERROR,
			"media: record stop rejected reason=record_id_mismatch requested_record_id=%s active_record_id=%s",
			record_id, cur);
		return -1;
	}

	if (majestic_lock_acquire(c->majestic_lock_wait_sec) != 0) {
		result_ok(r, -1, "fail");
		return -1;
	}
	if (record_disable(c) != 0 || majestic_restart(c) != 0) {
		majestic_lock_release();
		result_ok(r, -1, "fail");
		log_msg(ENCM_LOG_ERROR,
			"media: record stop failed before final upload record_id=%s",
			record_id);
		return -1;
	}
	majestic_lock_release();

	state_set_bool("is_recording", false);
	recompute_idle();
	upload_kick();			/* 通知上传线程录像期结束 */
	sleep(RECORD_FINALIZE_WAIT_SEC);

	{
		long sts = 0;

		state_get_int("record_start_ts", &sts);
		start_ts = (long long)sts;
	}
	latest[0] = '\0';
	walk_latest(record_root(c), start_ts, latest, sizeof(latest),
		    &best_ts, 0);
	if (latest[0] == '\0') {
		/* 保留活动任务上下文，使后续 stop/reset 可以重试 */
		result_ok(r, -1, "fail");
		state_set_bool("is_recording", true);
		recompute_idle();
		log_msg(ENCM_LOG_ERROR,
			"media: record stop failed: no record file found record_id=%s",
			record_id);
		return -1;
	}

	state_get_str("current_task_id", cur_task, sizeof(cur_task));
	rt_snapshot(&rt);
	if (upload_sync_final(c, &rt, latest, record_id, cur_task, start_ts,
			      remote_name, sizeof(remote_name)) != 0) {
		result_ok(r, -1, "fail");
		state_set_bool("is_recording", true);
		recompute_idle();
		log_msg(ENCM_LOG_ERROR,
			"media: record stop upload failed mode=%s record_id=%s local_file=%s",
			mode, record_id, latest);
		return -1;
	}

	/* 成功：组装 lastFile/fileUrl/segmentNo 结果 + last_record_* 状态 */
	if (stat(latest, &st) == 0)
		size = (long long)st.st_size;
	state_get_int("segment_no", &seg_no);
	build_record_remote_path(c, record_id, remote_name, rel, sizeof(rel));
	ftp_report_url(&rt, rel, report, sizeof(report));
	snprintf(r->last_file, sizeof(r->last_file), "%s", remote_name);
	r->last_size = size;
	json_escape(latest, sizeof(latest), report);	/* latest 复用为转义缓冲 */
	extra_add(r, "\"fileUrl\":%s", latest);
	extra_add(r, "\"segmentNo\":%ld", seg_no);
	/* 旧 bash 版 record/stop_record 曾写 status=streaming，但自动化测试对 stop_record 语义
	 * 期望是 success（stop_record_ack 仅代表"录制已停止+尾片已上传成功"，不应表示对外仍在推流）。
	 * task/ 与 record/ 两条路径返回一致 status=success；对外是否仍在推流由 heartbeat.is_publishing 体现。 */
	result_ok(r, 0, "success");

	state_set_str("last_record_id", record_id);
	state_set_str("last_record_task_id", cur_task);
	state_set_str("last_record_file_name", remote_name);
	state_set_str("last_record_file_url", report);
	state_set_int("last_record_file_size", (long)size);
	state_set_int("last_record_segment_no", seg_no);
	log_msg(ENCM_LOG_INFO,
		"media: record stop success mode=%s record_id=%s segment_no=%ld file_url=%s",
		mode, record_id, seg_no, report);

	/* 清理活动状态（mode=task 保留 current_task_id 供任务收尾链路使用） */
	state_set_str("current_record_id", "");
	state_set_str("current_record_flow", "");
	state_set_int("record_start_ts", 0);
	state_set_str("record_session_time", "");
	manifest_reset();
	if (strcmp(mode, "task") != 0)
		state_set_str("current_task_id", "");
	recompute_idle();
	return 0;
}

/* ------------------------------------------------------------------ */
/* 任务语音预备（feature_task_prepare_desk_voice）                        */
/* ------------------------------------------------------------------ */

static int task_prepare_desk_voice(const enc_cfg_t *c, const cmd_t *cmd,
				   feat_result_t *r)
{
	(void)r;
	log_msg(ENCM_LOG_INFO,
		"media: task prepare desk recognition voice request task_id=%s",
		cmd->task_id);

	if (state_is("is_recording")) {
		/* 录像中只允许复用已就绪的音频接口，禁止修改 YAML 或 HUP Majestic */
		if (voice_ready(c) != 0) {
			char rec[64];

			state_get_str("current_record_id", rec, sizeof(rec));
			log_msg(ENCM_LOG_ERROR,
				"media: task prepare desk recognition voice rejected during active record "
				"because audio output is not ready task_id=%s record_id=%s",
				cmd->task_id, rec);
			return -1;
		}
	} else if (voice_init(c) != 0) {
		log_msg(ENCM_LOG_ERROR,
			"media: task prepare desk recognition voice init failed task_id=%s",
			cmd->task_id);
		return -1;
	}

	voice_desk_async(c, cmd->task_id);
	return 0;
}

/* ------------------------------------------------------------------ */
/* 复位（feature_reset_execute）                                         */
/* ------------------------------------------------------------------ */

/* reset：先安全停止并上传活动录像；成功后才关推流/video 并清全部活动状态 */
static int reset_execute(const enc_cfg_t *c, const char *reset_task_id,
			 feat_result_t *r)
{
	char task_id[64], rec_id[64], rec_flow[16], esc[128];
	int  reset_code = 0;

	if (reset_task_id && reset_task_id[0])
		snprintf(task_id, sizeof(task_id), "%s", reset_task_id);
	else
		state_get_str("current_task_id", task_id, sizeof(task_id));
	duration_cancel();

	if (state_is("is_recording")) {
		state_get_str("current_record_id", rec_id, sizeof(rec_id));
		state_get_str("current_record_flow", rec_flow, sizeof(rec_flow));
		if (rec_flow[0] == '\0')
			snprintf(rec_flow, sizeof(rec_flow), "task");
		if (record_stop(c, rec_flow, rec_id, r) != 0) {
			/* bash：feature_result_reset 后保留 taskId 并整体失败 */
			memset(r, 0, sizeof(*r));
			json_escape(esc, sizeof(esc), task_id);
			if (task_id[0])
				extra_add(r, "\"taskId\":%s", esc);
			result_ok(r, -1, "fail");
			log_msg(ENCM_LOG_ERROR,
				"media: reset stopped because active record was not safely finalized task_id=%s record_id=%s",
				task_id, rec_id);
			return -1;
		}
	}

	voice_stop_current();		/* bash stop_pidfile_process VOICE_PLAYER_PID_FILE */

	if (majestic_lock_acquire(c->majestic_lock_wait_sec) == 0) {
		if (record_disable(c) != 0)
			reset_code = -1;
		if (apply_outgoing_disable(c) != 0)
			reset_code = -1;
		if (disable_all_video(c) != 0)
			reset_code = -1;
		if (stream_reload(c) != 0)
			reset_code = -1;
		majestic_lock_release();
	} else {
		reset_code = -1;
	}

	if (reset_code == 0) {
		state_set_bool("is_recording", false);
		state_set_bool("is_publishing", false);
		state_set_int("record_start_ts", 0);
		state_set_str("record_session_time", "");
		state_set_int("segment_no", 0);
		manifest_reset();
		state_set_str("current_record_id", "");
		state_set_str("current_record_flow", "");
		state_set_str("current_stream_url", "");
		state_set_str("current_task_id", "");
		recompute_idle();
		result_ok(r, 0, "success");
		log_msg(ENCM_LOG_INFO, "media: reset success task_id=%s", task_id);
		return 0;
	}

	result_ok(r, -1, "fail");
	log_msg(ENCM_LOG_ERROR, "media: reset partial failure task_id=%s", task_id);
	return -1;
}

/* ------------------------------------------------------------------ */
/* 业务入口 / 启动恢复 / duration 检查                                    */
/* ------------------------------------------------------------------ */

/* 命令映射（bash protocol.sh PROTO_COMMAND，按 flow/action 分派；
 * msg 合法性由 proto_parse/dispatch 层校验） */
int feat_execute(const cmd_t *c, feat_result_t *r)
{
	int rc;

	memset(r, 0, sizeof(*r));
	r->code = -1;
	snprintf(r->status, sizeof(r->status), "%s", "fail");

	/* bash business_action.lock 的进程内等价：命令与 duration reset 串行 */
	pthread_mutex_lock(&g_biz_mutex);
	if (!strcmp(c->flow, "record") && !strcmp(c->action, "start_record"))
		rc = record_start(&g_app.cfg, "record", c, r);
	else if (!strcmp(c->flow, "record") && !strcmp(c->action, "stop_record"))
		rc = record_stop(&g_app.cfg, "record", c->record_id, r);
	else if (!strcmp(c->flow, "capture") && !strcmp(c->action, "capture")) {
		/* 抓拍能力已移交上位机：设备端关闭，协议保留 captureAck 统一应答失败 */
		log_msg(ENCM_LOG_WARN,
			"media: capture disabled on device, reject capture_id=%s",
			c->capture_id);
		result_ok(r, -1, "fail");
		rc = -1;
	}
	else if (!strcmp(c->flow, "stream") && !strcmp(c->action, "start_stream"))
		rc = stream_start(&g_app.cfg, "stream", c, r);
	else if (!strcmp(c->flow, "stream") && !strcmp(c->action, "stop_stream"))
		rc = stream_stop(&g_app.cfg, r);
	else if (!strcmp(c->flow, "task") &&
		 !strcmp(c->action, "prepare_desk_recognition_voice"))
		rc = task_prepare_desk_voice(&g_app.cfg, c, r);
	else if (!strcmp(c->flow, "task") && !strcmp(c->action, "start_stream"))
		rc = stream_start(&g_app.cfg, "task", c, r);
	else if (!strcmp(c->flow, "task") && !strcmp(c->action, "start_record"))
		rc = record_start(&g_app.cfg, "task", c, r);
	else if (!strcmp(c->flow, "task") && !strcmp(c->action, "stop_record"))
		rc = record_stop(&g_app.cfg, "task", c->record_id, r);
	else if (!strcmp(c->flow, "task") && !strcmp(c->action, "reset_encoder"))
		rc = reset_execute(&g_app.cfg, c->task_id, r);
	else {
		log_msg(ENCM_LOG_ERROR,
			"media: unsupported request flow=%s action=%s",
			c->flow, c->action);
		rc = -1;
	}
	pthread_mutex_unlock(&g_biz_mutex);
	return rc;
}

/* feat_restore_startup_media：main 启动时把 Majestic 恢复到脚本定义的
 * 开机空闲配置（common.sh encoder_restore_startup_media_profile +
 * encoder_reset_runtime_media_state），并取消遗留 duration 定时。 */
int feat_restore_startup_media(const enc_cfg_t *c)
{
	int rc = 0;

	duration_cancel();
	if (majestic_lock_acquire(c->majestic_lock_wait_sec) != 0) {
		log_msg(ENCM_LOG_ERROR,
			"media: restore startup media profile lock failed");
		return -1;
	}
	/* 顺序照 bash：record profile → outgoing profile → video profile → reload */
	if (yaml_set(c, ".records.enabled", "false", false) ||
	    yaml_set(c, "records.path", STARTUP_RECORDS_PATH, true) ||
	    yaml_set(c, "records.split", STARTUP_RECORDS_SPLIT, false) ||
	    yaml_set(c, "records.maxUsage", STARTUP_RECORDS_MAXUSAGE, false))
		rc = -1;
	{
		/* 启动恢复：outgoing.server = 不应该有残留。与 STOP 同语义处理：
		 * 只有 get 得到非空值才真正 del；空值/get miss 都 INFO 跳过，
		 * 避免 yaml-cli 空值 delete RC=1 被误判 fail。 */
		char cur_srv[512];
		int  gr = yaml_get(c, "outgoing.server", cur_srv,
				   sizeof(cur_srv));
		int  del_ok = 0;
		if (gr == 0 && cur_srv[0]) {
			if (yaml_del(c, "outgoing.server") == 0) {
				del_ok = 1;
			} else {
				log_msg(ENCM_LOG_WARN,
					"media: startup outgoing.server del fail -> set empty fallback");
				yaml_set(c, "outgoing.server", "", true);
				del_ok = 1;   /* 退化也算已处理 */
			}
		} else {
			del_ok = 1;    /* 已经空/不存在，无需处理 */
		}
		if (!del_ok ||
		    yaml_set(c, "outgoing.substream", STREAM_SUBSTREAM_DEF, false) ||
		    yaml_set(c, ".outgoing.enabled", "true", false))
			rc = -1;
	}
	if (yaml_set(c, "video0.enabled", "true", false) ||
	    yaml_set(c, "video0.codec", MAIN_STREAM_CODEC, true) ||
	    yaml_set(c, "video0.size", MAIN_STREAM_SIZE, true) ||
	    yaml_set(c, "video0.fps", STARTUP_VIDEO0_FPS, false) ||
	    yaml_set(c, "video0.bitrate", STARTUP_VIDEO0_BITRATE, false) ||
	    yaml_set(c, "video1.enabled", "true", false) ||
	    yaml_set(c, "video1.codec", SUB_STREAM_CODEC, true) ||
	    yaml_set(c, "video1.size", STARTUP_VIDEO1_SIZE, true) ||
	    yaml_set(c, "video1.fps", STARTUP_VIDEO1_FPS, false) ||
	    yaml_set(c, "video1.bitrate", STARTUP_VIDEO1_BITRATE, false))
		rc = -1;
	if (stream_reload(c) != 0)
		rc = -1;
	majestic_lock_release();

	if (rc == 0) {
		/* encoder_reset_runtime_media_state：直接写状态文件 */
		state_set_bool("is_idle", true);
		state_set_bool("is_recording", false);
		state_set_bool("is_publishing", false);
		state_set_int("record_start_ts", 0);
		state_set_str("record_session_time", "");
		state_set_int("segment_no", 0);
		state_set_str("segment_manifest", "");
		state_set_str("current_record_id", "");
		state_set_str("current_record_flow", "");
		state_set_str("current_stream_url", "");
		state_set_str("current_task_id", "");
		log_msg(ENCM_LOG_INFO,
			"media: restore startup media profile success");
	} else {
		log_msg(ENCM_LOG_ERROR,
			"media: restore startup media profile failed");
	}
	return rc;
}

/* 主循环周期调用：duration 到期触发 reset。
 * bash feature_duration_loop：到期后重新校验 publishing 且 URL 匹配才执行，
 * 否则忽略 stale timer。返回 1 = 已执行 reset。 */
int feat_duration_check(void)
{
	char expect_url[512], task_id[64], cur_url[512];
	time_t deadline;
	int executed = 0;

	pthread_mutex_lock(&g_biz_mutex);
	deadline = g_dur_deadline;
	if (deadline != 0 && time(NULL) >= deadline) {
		snprintf(expect_url, sizeof(expect_url), "%s", g_dur_url);
		snprintf(task_id, sizeof(task_id), "%s", g_dur_task);
		if (state_is("is_publishing") &&
		    state_get_str("current_stream_url", cur_url,
				  sizeof(cur_url)) &&
		    !strcmp(cur_url, expect_url)) {
			feat_result_t r;

			log_msg(ENCM_LOG_WARN,
				"media: stream duration expired task_id=%s; execute reset",
				task_id[0] ? task_id : cur_url);
			reset_execute(&g_app.cfg, task_id, &r);
			executed = 1;
		} else {
			log_msg(ENCM_LOG_INFO,
				"media: duration timer ignored stale timer stream_url=%s",
				expect_url);
			duration_cancel();
		}
	}
	pthread_mutex_unlock(&g_biz_mutex);
	return executed;
}
