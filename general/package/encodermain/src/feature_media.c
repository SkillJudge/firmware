/*
 * feature_media.c — 推流/录像/抓拍/复位（yaml-cli + HUP）—— 设计 §6.2
 *
 * 逐条映射 bash feature_engine.sh 的状态机语义：
 *   stream_start/stop、record_start/stop、capture、reset_encoder、
 *   task_prepare_desk_voice、duration 定时、startup 恢复。
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
#define SRS_HOST_DEFAULT        "123.60.51.11"	/* config.sh SRS_HOST 兜底 */
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

/* common.sh feature_yaml_set：yaml-cli -i <conf> -s <key> <value>；
 * 字符串值先 shell_quote（feature_cli_set_string），bool/number 原样 */
static int yaml_set(const enc_cfg_t *c, const char *key,
		    const char *value, bool quote)
{
	char qc[600], qv[1100], cmd[1900];

	shell_quote(qc, sizeof(qc),
		    c->majestic_conf[0] ? c->majestic_conf : ENCM_MAJESTIC_CONF);
	if (quote)
		shell_quote(qv, sizeof(qv), value);
	else
		snprintf(qv, sizeof(qv), "%s", value);
	snprintf(cmd, sizeof(cmd), "%s -i %s -s %s %s",
		 c->yaml_cli[0] ? c->yaml_cli : ENCM_YAML_CLI, qc, key, qv);
	if (run_cmd(cmd, 10, NULL, 0) != 0) {
		log_msg(ENCM_LOG_ERROR, "media: yaml set failed key=%s", key);
		return -1;
	}
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

/* common.sh stream_service_reload_or_recover：HUP；进程不在/退出则恢复启动 */
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
	if (yaml_set(c, ".video0.enabled", MAIN_STREAM_ENABLED, false) ||
	    yaml_set(c, ".video0.codec", MAIN_STREAM_CODEC, true) ||
	    yaml_set(c, ".video0.size", MAIN_STREAM_SIZE, true) ||
	    yaml_set(c, ".video0.fps", MAIN_STREAM_FPS, false) ||
	    yaml_set(c, ".video1.enabled", SUB_STREAM_ENABLED, false) ||
	    yaml_set(c, ".video1.codec", SUB_STREAM_CODEC, true) ||
	    yaml_set(c, ".video1.size", SUB_STREAM_SIZE, true) ||
	    yaml_set(c, ".video1.fps", SUB_STREAM_FPS, false))
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
	if (yaml_set(c, ".records.path", path, true) ||
	    yaml_set(c, ".records.split", split, false) ||
	    yaml_set(c, ".records.maxUsage", usage, false) ||
	    yaml_set(c, ".records.substream", RECORD_SUBSTREAM_DEF, false))
		return -1;
	return 0;
}

static int record_enable(const enc_cfg_t *c)
{
	return yaml_set(c, ".records.enabled", "true", false);
}

static int record_disable(const enc_cfg_t *c)
{
	return yaml_set(c, ".records.enabled", "false", false);
}

/* feature_stream_apply_outgoing_profile */
static int apply_outgoing_enable(const enc_cfg_t *c, const char *url)
{
	return yaml_set(c, ".outgoing.server", url, true) ||
	       yaml_set(c, ".outgoing.substream", STREAM_SUBSTREAM_DEF, false) ||
	       yaml_set(c, ".outgoing.enabled", "true", false);
}

/* feature_stream_disable_outgoing：server 必须清空防旧地址残留 */
static int apply_outgoing_disable(const enc_cfg_t *c)
{
	return yaml_set(c, ".outgoing.server", "", true) ||
	       yaml_set(c, ".outgoing.substream", STREAM_SUBSTREAM_DEF, false) ||
	       yaml_set(c, ".outgoing.enabled", "false", false);
}

/* feature_media_disable_all_video：仅 stopStream/reset 关闭视频链路 */
static int disable_all_video(const enc_cfg_t *c)
{
	return yaml_set(c, ".video0.enabled", "false", false) ||
	       yaml_set(c, ".video1.enabled", "false", false);
}

/* ------------------------------------------------------------------ */
/* 推流 URL / 路径工具                                                   */
/* ------------------------------------------------------------------ */

/* build_stream_name：SRS_STREAM_PREFIX 默认 stream_<device_id> */
static void build_stream_name(const enc_cfg_t *c, char *out, size_t sz)
{
	snprintf(out, sz, "stream_%s", device_id_get(c));
}

/* build_stream_url_with_name：控制端下发 URL 统一替换最后一级为 stream_name */
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
	/* bash case 匹配 scheme://host/a/b 形态：路径有两级以上才替换
	 * 最后一级，否则直接追加。注意注释内不得出现星号斜杠序列。 */
	scheme = strstr(base, "://");
	p = scheme ? strchr(scheme + 3, '/') : strchr(base, '/');
	if (p && strchr(p + 1, '/')) {
		char *last = strrchr(p, '/');

		*last = '\0';
	}
	snprintf(out, sz, "%s/%s", base, name);
}

/* build_stream_url：命令下发 → registerAck/runtime SRS → config.sh 默认兜底 */
static int build_stream_url(const enc_cfg_t *c, const char *requested,
			    const enc_runtime_t *rt, char *out, size_t sz)
{
	char name[192];

	build_stream_name(c, name, sizeof(name));
	if (requested && requested[0]) {
		build_stream_url_with_name(requested, name, out, sz);
		return 0;
	}
	if (rt && rt->srs_url[0]) {
		/* rt.srs_url：完整 rtmp URL 或 host[:port] 形态均可 */
		if (strstr(rt->srs_url, "://"))
			build_stream_url_with_name(rt->srs_url, name, out, sz);
		else
			snprintf(out, sz, "rtmp://%s/%s/%s", rt->srs_url,
				 SRS_APP_DEFAULT, name);
		return 0;
	}
	/* config.sh SRS_HOST/SRS_PORT/SRS_APP 默认值兜底 */
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
 * mtime ≥ min_ts 的最新 mp4；同 mtime 取文件名排序靠后者 */
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
		if ((long long)st.st_mtime < min_ts)
			continue;
		if ((long long)st.st_mtime > *best_ts ||
		    ((long long)st.st_mtime == *best_ts &&
		     (best[0] == '\0' || strcmp(path, best) > 0))) {
			*best_ts = (long long)st.st_mtime;
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
	int  timeout = c->record_verify_timeout_sec > 0 ?
		       c->record_verify_timeout_sec : 10;
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
#define CAPTURE_SNAPSHOT_URL     "http://127.0.0.1/image.jpg"
#define CAPTURE_CURL_TIMEOUT_SEC 30
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

/* build_capture_remote_path：capture/<device_id>/<capture_id>.jpg */
static void build_capture_remote_path(const enc_cfg_t *c, const char *capture_id,
				      char *out, size_t sz)
{
	snprintf(out, sz, "capture/%s/%s.jpg", device_id_get(c), capture_id);
}

/* config.sh CAPTURE_LOCAL_DIR（${APP_HOME}/media/capture） */
static void capture_local_dir(const enc_cfg_t *c, char *out, size_t sz)
{
	snprintf(out, sz, "%s/media/capture",
		 c->work_dir[0] ? c->work_dir : ENCM_WORK_DIR);
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
	    record_enable(c) == 0 && stream_reload(c) == 0) {
		/* bash：首个新 mp4 验证在 Majestic 锁内进行（最多 10s） */
		if (wait_for_new_record_file(c, (time_t)start_ts, prev_file,
					     prev_size, verified,
					     sizeof(verified)) != 0) {
			log_msg(ENCM_LOG_ERROR,
				"media: record start verification failed record_id=%s timeout=%ds",
				cmd->record_id,
				c->record_verify_timeout_sec > 0 ?
					c->record_verify_timeout_sec : 10);
			record_disable(c);
			stream_reload(c);
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
	if (record_disable(c) != 0 || stream_reload(c) != 0) {
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
	result_ok(r, 0, !strcmp(mode, "task") ? "success" : "streaming");

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
/* 抓拍（feature_capture_take）                                          */
/* ------------------------------------------------------------------ */

static int capture_take(const enc_cfg_t *c, const cmd_t *cmd, feat_result_t *r)
{
	char        capture_id[64], dir[512], local[600], rel[600], report[800];
	char        esc[800], qlocal[700], qurl[128], cmdbuf[1600];
	enc_runtime_t rt;
	struct stat st;
	db_rec_t    rec;

	if (cmd->capture_id[0])
		snprintf(capture_id, sizeof(capture_id), "%s", cmd->capture_id);
	else
		snprintf(capture_id, sizeof(capture_id), "%lld", now_ms());
	capture_local_dir(c, dir, sizeof(dir));
	dir_ensure(dir);
	snprintf(local, sizeof(local), "%s/%s.jpg", dir, capture_id);

	build_capture_remote_path(c, capture_id, rel, sizeof(rel));
	rt_snapshot(&rt);
	ftp_report_url(&rt, rel, report, sizeof(report));
	/* bash：RESULT_CAPTURE_ID/FILE_NAME/FILE_URL 先置，失败再清 fileUrl */
	json_escape(esc, sizeof(esc), capture_id);
	extra_add(r, "\"captureId\":%s", esc);
	snprintf(esc, sizeof(esc), "%s.jpg", capture_id);
	{
		char name_esc[800];

		json_escape(name_esc, sizeof(name_esc), esc);
		extra_add(r, "\"fileName\":%s", name_esc);
	}
	json_escape(esc, sizeof(esc), report);
	extra_add(r, "\"fileUrl\":%s", esc);
	snprintf(r->last_file, sizeof(r->last_file), "%s.jpg", capture_id);

	log_msg(ENCM_LOG_INFO,
		"media: capture request capture_id=%s snapshot_url=%s",
		capture_id, CAPTURE_SNAPSHOT_URL);

	shell_quote(qlocal, sizeof(qlocal), local);
	shell_quote(qurl, sizeof(qurl), CAPTURE_SNAPSHOT_URL);
	snprintf(cmdbuf, sizeof(cmdbuf), "curl -sS -o %s %s", qlocal, qurl);
	if (run_cmd(cmdbuf, CAPTURE_CURL_TIMEOUT_SEC, NULL, 0) == 0 &&
	    stat(local, &st) == 0 && st.st_size > 0 &&
	    upload_file(c, &rt, local, rel) == 0) {
		/* db 登记 kind=capture（上传成功 → uploaded 终态） */
		memset(&rec, 0, sizeof(rec));
		snprintf(rec.file, sizeof(rec.file), "%s", local);
		snprintf(rec.kind, sizeof(rec.kind), "capture");
		snprintf(rec.record_id, sizeof(rec.record_id), "%s", capture_id);
		rec.size = (long long)st.st_size;
		rec.mtime = (long long)st.st_mtime;
		rec.state = DB_UPLOADED;
		rec.ts = now_ms();
		encdb_rec_add(&rec);
		result_ok(r, 0, "success");
		log_msg(ENCM_LOG_INFO,
			"media: capture success capture_id=%s local_file=%s remote_path=%s",
			capture_id, local, rel);
		return 0;
	}

	unlink(local);
	/* bash：失败清空 RESULT_FILE_URL，保留 captureId/fileName */
	r->extra_json[0] = '\0';
	json_escape(esc, sizeof(esc), capture_id);
	extra_add(r, "\"captureId\":%s", esc);
	snprintf(esc, sizeof(esc), "%s.jpg", capture_id);
	{
		char name_esc[800];

		json_escape(name_esc, sizeof(name_esc), esc);
		extra_add(r, "\"fileName\":%s", name_esc);
	}
	r->last_file[0] = '\0';
	result_ok(r, -1, "fail");
	log_msg(ENCM_LOG_ERROR, "media: capture failed capture_id=%s", capture_id);
	return -1;
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
	else if (!strcmp(c->flow, "capture") && !strcmp(c->action, "capture"))
		rc = capture_take(&g_app.cfg, c, r);
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
	    yaml_set(c, ".records.path", STARTUP_RECORDS_PATH, true) ||
	    yaml_set(c, ".records.split", STARTUP_RECORDS_SPLIT, false) ||
	    yaml_set(c, ".records.maxUsage", STARTUP_RECORDS_MAXUSAGE, false))
		rc = -1;
	if (yaml_set(c, ".outgoing.server", "", true) ||
	    yaml_set(c, ".outgoing.substream", STREAM_SUBSTREAM_DEF, false) ||
	    yaml_set(c, ".outgoing.enabled", "true", false))
		rc = -1;
	if (yaml_set(c, ".video0.enabled", "true", false) ||
	    yaml_set(c, ".video0.codec", MAIN_STREAM_CODEC, true) ||
	    yaml_set(c, ".video0.size", MAIN_STREAM_SIZE, true) ||
	    yaml_set(c, ".video0.fps", STARTUP_VIDEO0_FPS, false) ||
	    yaml_set(c, ".video0.bitrate", STARTUP_VIDEO0_BITRATE, false) ||
	    yaml_set(c, ".video1.enabled", "true", false) ||
	    yaml_set(c, ".video1.codec", SUB_STREAM_CODEC, true) ||
	    yaml_set(c, ".video1.size", STARTUP_VIDEO1_SIZE, true) ||
	    yaml_set(c, ".video1.fps", STARTUP_VIDEO1_FPS, false) ||
	    yaml_set(c, ".video1.bitrate", STARTUP_VIDEO1_BITRATE, false))
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
