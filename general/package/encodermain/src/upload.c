/*
 * upload.c — db 驱动上传线程（录像期分片扫描 + 断网重传）+ purge + 事件发布
 *
 * 设计：EncoderMainDesign.md §6.3（上传线程）/ §7.4（断网重传）/ §7.5（purge）。
 * bash 事实来源（逐字对齐）：
 *   - feature_engine.sh ftp_upload_file：curl -sS --ftp-pasv --ftp-create-dirs
 *     --connect-timeout 10 --max-time 60 -T <local> ftp://user:pass@host/<rel>，
 *     LED upload token 包裹，日志 URL 隐藏密码（***）；
 *   - feature_engine.sh feature_record_upload_pending_segments：录像期扫描
 *     record_root 下 mtime≥record_start_ts 且稳定 ≥SEGMENT_STABLE_SEC(20s) 的
 *     mp4（find+sort 路径升序），seg=segment_no+1，build_record_output_name /
 *     prepare_record_named_local_file / build_record_remote_path 命名后上传，
 *     成功才推进 segment_no 并发 segment_uploaded 事件；
 *   - protocol.sh protocol_publish_segment_uploaded：task →
 *     task/segment_uploaded {recordId,fileUrl,fileSize,segmentNo}；
 *     record → record/segment_uploaded {recordId,segmentNo,fileName,fileUrl,
 *     status:"success"}（字段名/类型/顺序逐字一致）；
 *   - record_purge.sh（行为参考）：单行 JSON 输出；C 版按设计 §7.5 只删
 *     state=uploaded 且 mtime 超龄的账本文件，新增 skippedFiles 计数。
 *
 * 与 bash 的差异（设计规定的升级点）：
 *   - 分片查重/账本走 encdb（segment_manifest 保留为 bash 工具兼容投影，
 *     启动时旧 manifest 逐行导入 db 后归档为 segment_manifest.imported）；
 *   - 新增空闲期断网重传：MQTT 重连 upload_kick() + 30s 周期兜底，
 *     pending/failed/failed_final 按 mtime 升序串行重传，退避 30s×2^n 上限
 *     30min，retry 超过 upload_retry_max 转 failed_final 并入 alarms(6201)；
 *     文件消失转 lost 并入 alarms(6202)，防账本悬挂。
 *
 * 说明：cfg 未继承 config.sh 的 FTP_HOST/PORT/USER/PASS 默认值，凭据一律以
 * registerAck 落盘的 runtime 文件为准（rt_load 恢复）；未注册时按
 * ftp_config_missing 跳过上传（bash 会回落 config.sh 默认值，C 版无该字段；
 * 注册前无业务上传场景）。分片号每次尝试重取 state segment_no+1（bash
 * 逐文件重取语义），成功才推进；重传事件按 recordId+segmentNo 幂等追加。
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

/* ---- config.sh 固定值（C 版 cfg 未包含，按固件默认硬编码） ---- */
#define ENCM_SEGMENT_SCAN_INTERVAL_SEC 10	/* SEGMENT_SCAN_INTERVAL_SEC */
#define ENCM_FTP_CONNECT_TIMEOUT_SEC   10	/* CURL_CONNECT_TIMEOUT_SEC */
#define ENCM_FTP_MAX_TIME_SEC          60	/* CURL_UPLOAD_MAX_TIME_SEC */
#define ENCM_RECORD_TIME_FORMAT        "%Y%m%d%H%M%S"
#define ENCM_RECORD_REMOTE_ROOT        "upload"	/* RECORD_REMOTE_ROOT */
#define ENCM_NAMED_DIR_NAME            "named_records"	/* RECORD_NAMED_LOCAL_DIR */

/* ---- 设计 §7.4 固定值 ---- */
#define ENCM_RETRY_BACKOFF_MAX_SEC     1800	/* 退避上限 30min */
#define ENCM_IDLE_SCAN_PERIOD_SEC      30	/* 心跳周期兜底重传扫描 */

/* record_stop 最终分片同步上传进行中（抑制重传扫描并发同一文件） */
static volatile bool g_final_syncing;

/* 上传线程唤醒（cond：录像期 10s 周期 / 空闲期 30s 周期 / kick 即刻） */
static pthread_mutex_t g_up_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_up_cond = PTHREAD_COND_INITIALIZER;

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

static const char *record_root(const enc_cfg_t *c)
{
	return c->record_root[0] ? c->record_root : ENCM_RECORD_ROOT;
}

/* ------------------------------------------------------------------ */
/* 命名 / 路径（逐字对齐 feature_engine.sh）                              */
/* ------------------------------------------------------------------ */

/* common.sh sanitize_name_part：[^A-Za-z0-9._-] → _，空串 → unknown */
static void sanitize_name_part(const char *in, char *out, size_t sz)
{
	size_t o = 0;
	const char *p;

	for (p = in ? in : ""; *p && o + 1 < sz; p++)
		out[o++] = (isalnum((unsigned char)*p) || *p == '.' ||
			    *p == '_' || *p == '-') ? *p : '_';
	out[o] = '\0';
	if (o == 0)
		snprintf(out, sz, "unknown");
}

/* build_record_output_name：模板 {device_id}-{task_id}-{timestamp}-{segment_no}.mp4。
 * timestamp 优先 state record_session_time（同一次录像不漂移，bash 语义）；
 * 缺失时（重传场景会话已清）回退文件 mtime——重试间命名保持稳定。 */
static void build_record_output_name(const enc_cfg_t *c, long seg_no,
				     const char *task_id, long long mtime,
				     char *out, size_t sz)
{
	char        session[32], s_dev[96], s_task[128], s_sess[32];
	struct tm   tmv;
	time_t      t;

	(void)c;
	if (!state_get_str("record_session_time", session, sizeof(session)) ||
	    session[0] == '\0') {
		t = mtime > 0 ? (time_t)mtime : time(NULL);
		localtime_r(&t, &tmv);
		strftime(session, sizeof(session), ENCM_RECORD_TIME_FORMAT,
			 &tmv);
	}
	sanitize_name_part(device_id_get(&g_app.cfg), s_dev, sizeof(s_dev));
	sanitize_name_part(task_id && task_id[0] ? task_id : "manual",
			   s_task, sizeof(s_task));
	sanitize_name_part(session, s_sess, sizeof(s_sess));
	snprintf(out, sz, "%s-%s-%s-%ld.mp4", s_dev, s_task, s_sess, seg_no);
}

/* build_record_remote_path：upload/<device_id>/<record_id>/<file_name> */
static void build_record_remote_path(const enc_cfg_t *c, const char *record_id,
				     const char *file_name, char *out,
				     size_t sz)
{
	snprintf(out, sz, "%s/%s/%s/%s", ENCM_RECORD_REMOTE_ROOT,
		 device_id_get(c), record_id, file_name);
}

/* build_capture_remote_path：capture/<device_id>/<capture_id>.jpg */
static void build_capture_remote_path(const enc_cfg_t *c,
				      const char *capture_id, char *out,
				      size_t sz)
{
	snprintf(out, sz, "capture/%s/%s.jpg", device_id_get(c), capture_id);
}

/* ftp_path（远端根目录，默认 /）与相对路径拼接 */
static void join_remote_path(const char *ftp_path, const char *rel,
			     char *out, size_t sz)
{
	char   base[256];
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

/* build_ftp_report_url：事件里上报的不带凭据 URL（rt 无端口字段，按 21 省略） */
static void ftp_report_url(const enc_runtime_t *rt, const char *rel,
			   char *out, size_t sz)
{
	char joined[768];

	join_remote_path(rt ? rt->ftp_path : "", rel, joined, sizeof(joined));
	snprintf(out, sz, "ftp://%s/%s",
		 rt && rt->ftp_host[0] ? rt->ftp_host : "", joined);
}

/* prepare_record_named_local_file：runtime/named_records 下建标准命名入口。
 * 先 rm -f 再 ln -s，软链失败降级 cp，仍失败回落源文件（返回 1）。 */
static int named_local_prepare(const enc_cfg_t *c, const char *source,
			       const char *output_name, char *out, size_t sz)
{
	char dir[512], named[768];

	snprintf(dir, sizeof(dir), "%s/%s",
		 c->runtime_dir[0] ? c->runtime_dir : ENCM_RUNTIME_DIR,
		 ENCM_NAMED_DIR_NAME);
	dir_ensure(dir);
	snprintf(named, sizeof(named), "%s/%s", dir, output_name);
	unlink(named);
	if (symlink(source, named) == 0) {
		snprintf(out, sz, "%s", named);
		return 0;
	}
	{
		/* cp 降级：块拷贝（与 bash cp 等价） */
		FILE   *rf = fopen(source, "rb");
		FILE   *wf;
		char    buf[65536];
		size_t  n;
		bool    ok = false;

		wf = fopen(named, "wb");
		if (rf && wf) {
			ok = true;
			while ((n = fread(buf, 1, sizeof(buf), rf)) > 0)
				if (fwrite(buf, 1, n, wf) != n) {
					ok = false;
					break;
				}
		}
		if (rf)
			fclose(rf);
		if (wf) {
			if (fflush(wf) != 0)
				ok = false;
			fclose(wf);
		}
		if (ok) {
			snprintf(out, sz, "%s", named);
			return 0;
		}
		unlink(named);
	}
	log_msg(ENCM_LOG_WARN,
		"named local file create failed source_file=%s named_file=%s, upload original file",
		source, named);
	snprintf(out, sz, "%s", source);
	return 1;
}

/* ------------------------------------------------------------------ */
/* segment_manifest（bash 兼容投影 + 启动迁移，设计 §6.3）                */
/* ------------------------------------------------------------------ */

static void manifest_path(char *out, size_t sz)
{
	snprintf(out, sz, "%s/segment_manifest",
		 g_app.cfg.state_dir[0] ? g_app.cfg.state_dir : ENCM_STATE_DIR);
}

/* 上传成功后追加（bash state_segment_manifest_add 等价） */
static void manifest_append(const char *file)
{
	char  path[600];
	FILE *f;

	manifest_path(path, sizeof(path));
	f = fopen(path, "a");
	if (!f)
		return;
	fprintf(f, "%s\n", file);
	fclose(f);
}

/* 启动迁移：旧 bash manifest 逐行导入 db（state=uploaded）后归档改名，
 * 保证 bash→C 升级不重复上传、purge 仍可按账本清理。 */
static void manifest_migrate(void)
{
	char       path[600], imp[620], line[1024];
	FILE      *f;
	bool       had_line = false;
	int        migrated = 0;

	manifest_path(path, sizeof(path));
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		size_t     l = strlen(line);
		db_rec_t   r, got;
		struct stat st;

		while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
			line[--l] = '\0';
		if (l == 0)
			continue;
		had_line = true;
		if (stat(line, &st) != 0 || encdb_rec_get(line, &got))
			continue;
		memset(&r, 0, sizeof(r));
		snprintf(r.file, sizeof(r.file), "%s", line);
		snprintf(r.kind, sizeof(r.kind), "record");
		r.size = (long long)st.st_size;
		r.mtime = (long long)st.st_mtime;
		r.state = DB_UPLOADED;
		r.ts = now_ms();
		if (encdb_rec_add(&r) == 0)
			migrated++;
	}
	fclose(f);
	if (!had_line)
		return;		/* 空文件留给 manifest_reset 清理 */
	snprintf(imp, sizeof(imp), "%s.imported", path);
	unlink(imp);
	if (rename(path, imp) == 0)
		log_msg(ENCM_LOG_INFO,
			"segment manifest imported records=%d archive=%s",
			migrated, imp);
	else
		log_msg(ENCM_LOG_WARN,
			"segment manifest archive failed path=%s", path);
}

/* ------------------------------------------------------------------ */
/* FTP 上传（bash ftp_upload_file 对齐）                                 */
/* ------------------------------------------------------------------ */

int upload_file(const enc_cfg_t *c, const enc_runtime_t *rt,
		const char *local, const char *remote_rel)
{
	char  url[1100], log_url[256], rel[600];
	char  qlocal[2200], qurl[2300], cmd[4800], errout[256], tag[48];
	struct stat st;
	time_t start;
	int   rc, elapsed;

	(void)c;
	if (stat(local, &st) != 0 || !S_ISREG(st.st_mode)) {
		log_msg(ENCM_LOG_ERROR,
			"ftp upload skipped local_file_missing local_file=%s remote_path=%s",
			local, remote_rel);
		return -1;
	}
	if (!rt || !rt->ftp_host[0] || !rt->ftp_user[0] || !rt->ftp_pass[0]) {
		log_msg(ENCM_LOG_ERROR,
			"ftp upload skipped ftp_config_missing host=%s user=%s pass_configured=%s remote_path=%s",
			rt && rt->ftp_host[0] ? rt->ftp_host : "empty",
			rt && rt->ftp_user[0] ? rt->ftp_user : "empty",
			(rt && rt->ftp_pass[0]) ? "true" : "false",
			remote_rel);
		return -1;
	}

	/* build_ftp_url：含凭据完整 URL；凭据不做 URL 编码（与 bash 一致），
	 * 整体经 shell_quote 传给 sh -c。日志 URL 隐藏密码。 */
	snprintf(rel, sizeof(rel), "%s",
		 remote_rel[0] == '/' ? remote_rel + 1 : remote_rel);
	snprintf(url, sizeof(url), "ftp://%s:%s@%s/%s",
		 rt->ftp_user, rt->ftp_pass, rt->ftp_host, rel);
	snprintf(log_url, sizeof(log_url), "ftp://%s:***@%s/%s",
		 rt->ftp_user, rt->ftp_host, rel);

	snprintf(tag, sizeof(tag), "u%lld", now_ms());
	led_upload_token_set(tag, true);
	start = time(NULL);
	shell_quote(qlocal, sizeof(qlocal), local);
	shell_quote(qurl, sizeof(qurl), url);
	snprintf(cmd, sizeof(cmd),
		 "curl -sS --ftp-pasv --ftp-create-dirs --connect-timeout %d"
		 " --max-time %d -T %s %s 2>&1",
		 ENCM_FTP_CONNECT_TIMEOUT_SEC, ENCM_FTP_MAX_TIME_SEC,
		 qlocal, qurl);
	errout[0] = '\0';
	/* run_cmd 超时留裕量：curl 自身受 --max-time 限制并输出错误码 */
	rc = run_cmd(cmd, ENCM_FTP_MAX_TIME_SEC + ENCM_FTP_CONNECT_TIMEOUT_SEC + 15,
		     errout, sizeof(errout));
	elapsed = (int)(time(NULL) - start);
	led_upload_token_set(tag, false);

	if (rc == 0) {
		log_msg(ENCM_LOG_DEBUG,
			"ftp upload success protocol=ftp host=%s user=%s local_file=%s local_size=%lld remote_path=%s url=%s elapsed=%ds",
			rt->ftp_host, rt->ftp_user, local,
			(long long)st.st_size, remote_rel, log_url, elapsed);
		return 0;
	}
	log_msg(ENCM_LOG_ERROR,
		"ftp upload failed protocol=ftp host=%s user=%s local_file=%s local_size=%lld remote_path=%s url=%s elapsed=%ds rc=%d curl_error=%s",
		rt->ftp_host, rt->ftp_user, local, (long long)st.st_size,
		remote_rel, log_url, elapsed, rc,
		errout[0] ? errout : "empty");
	return rc;
}

/* ------------------------------------------------------------------ */
/* segment_uploaded 事件（protocol.sh protocol_publish_segment_uploaded）  */
/* ------------------------------------------------------------------ */

int upload_publish_segment(mq_client_t *mq, const enc_cfg_t *c,
			   const char *flow, const char *task_id,
			   const char *record_id, const char *file_name,
			   long long file_size, int segment_no)
{
	char          topic[256], rel[600], url[800];
	enc_runtime_t rt;
	long long     msgid;
	sb_t          d, b;
	bool          is_task;
	int           rc;

	(void)task_id;		/* bash payload 不含 taskId，仅日志参考 */
	rt_snapshot(&rt);
	build_record_remote_path(c, record_id, file_name, rel, sizeof(rel));
	ftp_report_url(&rt, rel, url, sizeof(url));
	msgid = msgid_next();
	is_task = flow && !strcmp(flow, "task");

	/* data 字段名/顺序照 protocol.sh：task 与 record 两种形态 */
	sb_init(&d);
	sb_puts(&d, "\"recordId\":");
	sb_json_str(&d, record_id ? record_id : "");
	if (is_task) {
		sb_puts(&d, ",\"fileUrl\":");
		sb_json_str(&d, url);
		sb_fmt(&d, ",\"fileSize\":%lld,\"segmentNo\":%d",
		       file_size, segment_no);
	} else {
		sb_fmt(&d, ",\"segmentNo\":%d", segment_no);
		sb_puts(&d, ",\"fileName\":");
		sb_json_str(&d, file_name ? file_name : "");
		sb_puts(&d, ",\"fileUrl\":");
		sb_json_str(&d, url);
		sb_puts(&d, ",\"status\":\"success\"");
	}
	sb_init(&b);
	sb_fmt(&b, "{\"msgId\":%lld,\"msg\":", msgid);
	sb_json_str(&b, "segmentUploaded");
	sb_puts(&b, ",\"data\":{");
	if (d.ok)
		sb_puts(&b, d.s);
	sb_puts(&b, "}}");

	snprintf(topic, sizeof(topic), "encoder/%s/ctrlsrv/0/%s/segment_uploaded",
		 device_id_get(c), is_task ? "task" : "record");

	if (!mq) {
		log_msg(ENCM_LOG_WARN,
			"segment event dropped (mqtt not ready) topic=%s segment_no=%d",
			topic, segment_no);
		sb_free(&d);
		sb_free(&b);
		return -1;
	}
	/* persist=true：outbox 必达语义，断网时自动补发（设计 §7.4.3） */
	rc = mq_publish(mq, topic, b.s, true);
	log_msg(ENCM_LOG_INFO,
		"[MQTT-PUB] topic=%s msg=segmentUploaded task_id=%s record_id=%s segment_no=%d file_url=%s",
		topic, task_id ? task_id : "", record_id ? record_id : "",
		segment_no, url);
	sb_free(&d);
	sb_free(&b);
	return rc;
}

/* ------------------------------------------------------------------ */
/* 账本状态迁移（设计 §7.2 / §7.4）                                       */
/* ------------------------------------------------------------------ */

/* 失败：retry++ / nextRetryTs=now+30s×2^(n-1)（上限 30min）；
 * retry 超过 upload_retry_max → failed_final + alarms(6201) */
static void attempt_mark_failed(const db_rec_t *row, int curl_rc)
{
	char       err[64], detail[512];
	int        retry = row->retry + 1;
	int        rmax = g_app.cfg.upload_retry_max > 0 ?
			      g_app.cfg.upload_retry_max : 5;
	long long  base = g_app.cfg.upload_retry_backoff_sec > 0 ?
			      g_app.cfg.upload_retry_backoff_sec : 30;
	long long  backoff = base, next, now = (long long)time(NULL);
	int        i;
	sb_t       b;

	snprintf(err, sizeof(err), "curl(%d)", curl_rc);
	for (i = 1; i < retry && backoff < ENCM_RETRY_BACKOFF_MAX_SEC; i++) {
		backoff <<= 1;
		if (backoff > ENCM_RETRY_BACKOFF_MAX_SEC)
			backoff = ENCM_RETRY_BACKOFF_MAX_SEC;
	}
	next = now + backoff;

	if (retry > rmax) {
		encdb_rec_state(row->file, DB_FAILED_FINAL, err, retry, next);
		sb_init(&b);
		sb_puts(&b, "{\"file\":");
		sb_json_str(&b, row->file);
		sb_fmt(&b, ",\"retry\":%d,\"err\":", retry);
		sb_json_str(&b, err);
		sb_putc(&b, '}');
		snprintf(detail, sizeof(detail), "%s", b.s ? b.s : "");
		sb_free(&b);
		encdb_alarm(6201, "upload_failed", "error",
			    "upload retry exhausted", detail);
		log_msg(ENCM_LOG_ERROR,
			"upload retry exhausted file=%s record_id=%s retry=%d err=%s",
			row->file, row->record_id, retry, err);
		return;
	}
	encdb_rec_state(row->file, DB_FAILED, err, retry, next);
	log_msg(ENCM_LOG_WARN,
		"upload scheduled for retry file=%s retry=%d next_retry_ts=%lld",
		row->file, retry, next);
}

/* 上传单个账本文件（录像期扫描 / 重传 / 最终分片共用）。
 * row 传入 file/kind/record_id/task_id/retry；分片号按 bash 语义每次尝试
 * 重取 state segment_no+1，成功才推进。publish_event 仅 record 分片为真。 */
static int attempt_upload(const enc_cfg_t *c, const enc_runtime_t *rt,
			  mq_client_t *mq, db_rec_t *row,
			  const char *mode, bool publish_event)
{
	char       name[300], named[1100], rel[640];
	long       seg_no = 0, seg = 0;
	struct stat st;
	int        rc, is_record;

	is_record = !strcmp(row->kind, "record");
	if (stat(row->file, &st) != 0)
		return -1;	/* 上传前瞬间消失：交重传扫描转 lost */

	if (is_record) {
		state_get_int("segment_no", &seg_no);
		seg = seg_no + 1;
		build_record_output_name(c, seg, row->task_id,
					 (long long)st.st_mtime, name,
					 sizeof(name));
		named_local_prepare(c, row->file, name, named, sizeof(named));
		build_record_remote_path(c, row->record_id, name, rel,
					 sizeof(rel));
		row->seg = (int)seg;
		log_msg(ENCM_LOG_DEBUG,
			"segment upload attempt mode=%s record_id=%s local_file=%s named_file=%s remote_name=%s remote_path=%s",
			mode, row->record_id, row->file, named, name, rel);
	} else {
		snprintf(name, sizeof(name), "%s.jpg", row->record_id);
		named[0] = '\0';
		build_capture_remote_path(c, row->record_id, rel, sizeof(rel));
	}

	/* 先登记 pending（进程崩溃后由重传扫描接手），再上传 */
	row->size = (long long)st.st_size;
	row->mtime = (long long)st.st_mtime;
	row->state = DB_PENDING;
	row->next_retry_ts = 0;
	row->err[0] = '\0';
	row->ts = now_ms();
	encdb_rec_add(row);

	rc = upload_file(c, rt, named[0] ? named : row->file, rel);
	if (rc == 0) {
		row->state = DB_UPLOADED;
		encdb_rec_add(row);
		if (is_record) {
			state_set_int("segment_no", seg);
			manifest_append(row->file);
			if (publish_event)
				upload_publish_segment(mq, c, mode,
						       row->task_id,
						       row->record_id, name,
						       row->size, (int)seg);
		}
		log_msg(ENCM_LOG_INFO,
			"upload success kind=%s mode=%s record_id=%s segment_no=%ld remote_path=%s size=%lld",
			row->kind, mode, row->record_id, seg, rel, row->size);
		return 0;
	}
	attempt_mark_failed(row, rc);
	log_msg(ENCM_LOG_ERROR,
		"upload failed kind=%s mode=%s record_id=%s local_file=%s remote_path=%s rc=%d retry=%d",
		row->kind, mode, row->record_id, row->file, rel, rc,
		row->retry + 1);
	return -1;
}

/* ------------------------------------------------------------------ */
/* mp4 文件收集（bash list_record_files = find + sort 路径升序）          */
/* ------------------------------------------------------------------ */

typedef struct {
	char *path;
} up_file_t;

static bool mp4_suffix(const char *name)
{
	size_t l = strlen(name);

	return l > 4 && !strcasecmp(name + l - 4, ".mp4");
}

static void walk_collect(const char *dir, up_file_t **arr, int *n, int *cap,
			 int depth)
{
	DIR           *d;
	struct dirent *e;

	if (depth > 8 || !(d = opendir(dir)))
		return;
	while ((e = readdir(d)) != NULL) {
		char       path[1024];
		struct stat st;
		up_file_t *tmp;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (lstat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			walk_collect(path, arr, n, cap, depth + 1);
			continue;
		}
		if (!S_ISREG(st.st_mode) || !mp4_suffix(e->d_name))
			continue;
		if (*n >= *cap) {
			*cap = *cap > 0 ? *cap * 2 : 32;
			tmp = (up_file_t *)realloc(*arr,
						   (size_t)*cap * sizeof(**arr));
			if (!tmp)
				break;
			*arr = tmp;
		}
		(*arr)[*n].path = strdup(path);
		if ((*arr)[*n].path)
			(*n)++;
	}
	closedir(d);
}

static int file_cmp_path(const void *a, const void *b)
{
	return strcmp(((const up_file_t *)a)->path,
		      ((const up_file_t *)b)->path);
}

static int collect_mp4(const char *root, up_file_t **out)
{
	int n = 0, cap = 0;

	*out = NULL;
	walk_collect(root, out, &n, &cap, 0);
	if (*out)
		qsort(*out, (size_t)n, sizeof(**out), file_cmp_path);
	return n;
}

static void free_files(up_file_t **arr, int n)
{
	int i;

	for (i = 0; i < n; i++)
		free((*arr)[i].path);
	free(*arr);
	*arr = NULL;
}

/* ------------------------------------------------------------------ */
/* 录像期分片扫描（bash feature_record_upload_pending_segments 对齐）      */
/* ------------------------------------------------------------------ */

static void scan_recording_once(const enc_cfg_t *c)
{
	enc_runtime_t rt;
	char        record_id[64], cur_id[64], mode[16], task_id[64];
	long        start_ts = 0;
	long long   stable, now;
	struct stat st;
	up_file_t  *files = NULL;
	int         nfiles, i;

	if (!state_get_str("current_record_id", record_id, sizeof(record_id)) ||
	    record_id[0] == '\0')
		return;
	if (!state_get_str("current_record_flow", mode, sizeof(mode)) ||
	    mode[0] == '\0')
		snprintf(mode, sizeof(mode), "record");
	state_get_str("current_task_id", task_id, sizeof(task_id));
	state_get_int("record_start_ts", &start_ts);
	stable = g_app.cfg.segment_stable_sec > 0 ?
		 g_app.cfg.segment_stable_sec : 20;

	rt_snapshot(&rt);
	nfiles = collect_mp4(record_root(c), &files);
	for (i = 0; i < nfiles; i++) {
		const char *fp = files[i].path;
		db_rec_t    got, row;

		if (g_app.stopping)
			break;
		/* bash segment_worker 循环条件：recording 且 record_id 未变
		 * （逐文件复检，stop 后立即退出本轮） */
		if (!state_is("is_recording"))
			break;
		if (!state_get_str("current_record_id", cur_id,
				   sizeof(cur_id)) ||
		    strcmp(cur_id, record_id) != 0)
			break;

		if (stat(fp, &st) != 0)
			continue;
		if ((long long)st.st_mtime < (long long)start_ts)
			continue;
		now = (long long)time(NULL);
		if (now - (long long)st.st_mtime < stable)
			continue;

		memset(&got, 0, sizeof(got));
		if (encdb_rec_get(fp, &got)) {
			/* 账本查重：uploaded/lost/failed_final 不再处理 */
			if (got.state != DB_PENDING && got.state != DB_FAILED)
				continue;
			if (got.state == DB_FAILED && now < got.next_retry_ts)
				continue;
		}

		row = got;		/* 保留 retry 计数 */
		snprintf(row.file, sizeof(row.file), "%s", fp);
		snprintf(row.kind, sizeof(row.kind), "record");
		snprintf(row.record_id, sizeof(row.record_id), "%s", record_id);
		snprintf(row.task_id, sizeof(row.task_id), "%s", task_id);
		attempt_upload(c, &rt, g_app.mq, &row, mode, true);
	}
	free_files(&files, nfiles);
}

/* ------------------------------------------------------------------ */
/* 空闲期断网重传（设计 §7.4：MQTT 重连 kick + 心跳周期兜底）              */
/* ------------------------------------------------------------------ */

static int rec_cmp_mtime(const void *a, const void *b)
{
	const db_rec_t *x = (const db_rec_t *)a;
	const db_rec_t *y = (const db_rec_t *)b;

	if (x->mtime < y->mtime)
		return -1;
	if (x->mtime > y->mtime)
		return 1;
	return strcmp(x->file, y->file);
}

static void retransmit_scan(const enc_cfg_t *c)
{
	enc_runtime_t rt;
	db_rec_t     *arr = NULL;
	char          mode[16], detail[512], esc[600];
	int           n = 0, cap = 0, cursor = 0, i;
	db_rec_t      r;
	sb_t          b;

	/* record_stop 最终分片同步上传进行中：本轮让路，防并发同一文件 */
	if (g_final_syncing)
		return;
	while (encdb_rec_next(&cursor, &r) == 1) {
		db_rec_t *tmp;

		if (r.state != DB_PENDING && r.state != DB_FAILED &&
		    r.state != DB_FAILED_FINAL)
			continue;
		if (n >= cap) {
			cap = cap > 0 ? cap * 2 : 32;
			tmp = (db_rec_t *)realloc(arr,
						  (size_t)cap * sizeof(*arr));
			if (!tmp)
				break;
			arr = tmp;
		}
		arr[n++] = r;
	}
	if (n == 0) {
		free(arr);
		return;
	}
	qsort(arr, (size_t)n, sizeof(*arr), rec_cmp_mtime);

	rt_snapshot(&rt);
	if (!state_get_str("current_record_flow", mode, sizeof(mode)) ||
	    mode[0] == '\0')
		snprintf(mode, sizeof(mode), "record");

	for (i = 0; i < n; i++) {
		db_rec_t   row = arr[i];
		struct stat st;
		long long  now;

		if (g_app.stopping)
			break;
		/* 新录像开始后交回录像期扫描，两条路径不并发同一文件 */
		if (state_is("is_recording"))
			break;

		if (stat(row.file, &st) != 0) {
			/* 文件消失（如 Majestic maxUsage 滚动删除）：转 lost
			 * + 6202，防账本悬挂（设计 §7.4.5） */
			encdb_rec_state(row.file, DB_LOST, "file_missing",
					row.retry, 0);
			sb_init(&b);
			sb_puts(&b, "{\"file\":");
			sb_json_str(&b, row.file);
			sb_puts(&b, ",\"recordId\":");
			sb_json_str(&b, row.record_id);
			sb_putc(&b, '}');
			snprintf(detail, sizeof(detail), "%s", b.s ? b.s : "");
			sb_free(&b);
			encdb_alarm(6202, "record_lost", "error",
				    "record file lost before upload", detail);
			json_escape(esc, sizeof(esc), row.record_id);
			log_msg(ENCM_LOG_ERROR,
				"record file lost before upload file=%s record_id=%s",
				row.file, row.record_id);
			continue;
		}
		now = (long long)time(NULL);
		if (row.state != DB_PENDING && now < row.next_retry_ts)
			continue;
		attempt_upload(c, &rt, g_app.mq, &row, mode,
			       !strcmp(row.kind, "record"));
	}
	free(arr);
}

/* ------------------------------------------------------------------ */
/* 线程主体 / kick                                                       */
/* ------------------------------------------------------------------ */

static void wait_up(int sec)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += sec;
	pthread_mutex_lock(&g_up_mu);
	pthread_cond_timedwait(&g_up_cond, &g_up_mu, &ts);
	pthread_mutex_unlock(&g_up_mu);
}

void upload_kick(void)
{
	pthread_mutex_lock(&g_up_mu);
	pthread_cond_broadcast(&g_up_cond);
	pthread_mutex_unlock(&g_up_mu);
}

void *upload_thread(void *arg)
{
	/* cfg/rt/mq 统一经 g_app 访问（feature_media/upload 共享全局上下文） */
	(void)arg;
	manifest_migrate();
	log_msg(ENCM_LOG_INFO, "upload thread started");
	while (!g_app.stopping) {
		if (state_is("is_recording")) {
			scan_recording_once(&g_app.cfg);
			wait_up(ENCM_SEGMENT_SCAN_INTERVAL_SEC);
		} else {
			retransmit_scan(&g_app.cfg);
			/* 设计 §7.4：重连事件 upload_kick() 之外，心跳周期
			 * 30s 兜底触发一次重传扫描 */
			wait_up(ENCM_IDLE_SCAN_PERIOD_SEC);
		}
	}
	log_msg(ENCM_LOG_INFO, "upload thread exit");
	return NULL;
}

/* ------------------------------------------------------------------ */
/* record_stop 最终分片同步上传（bash feature_record_stop 对齐）          */
/* ------------------------------------------------------------------ */

/* 成功返回 0 并填充远端文件名（lastFile）；不发布 segment_uploaded 事件
 * （bash 语义：最终分片结果由 stop ACK / lastSegmentUploaded 携带）。 */
int upload_sync_final(const enc_cfg_t *c, const enc_runtime_t *rt,
		      const char *local, const char *record_id,
		      const char *task_id, long long start_ts,
		      char *out_name, size_t osz)
{
	char        name[300], named[1100], rel[640];
	long        seg_no = 0, seg;
	db_rec_t    cur, row;
	struct stat st;
	int         rc;

	(void)start_ts;		/* latest 文件已由 feature_media 按 start_ts 找出 */
	if (out_name && osz)
		out_name[0] = '\0';
	if (!local || !local[0] || !record_id || !record_id[0] ||
	    stat(local, &st) != 0)
		return -1;

	g_final_syncing = true;

	/* 竞态保护：分片扫描已完整上传同文件 → 复用其远端命名与账本，
	 * 不再重复上传（此时 state segment_no 已被扫描推进） */
	if (encdb_rec_get(local, &cur) && cur.state == DB_UPLOADED) {
		build_record_output_name(c, cur.seg, task_id,
					 (long long)st.st_mtime, name,
					 sizeof(name));
		snprintf(out_name, osz, "%s", name);
		log_msg(ENCM_LOG_INFO,
			"record stop final segment already uploaded by scanner record_id=%s segment=%d remote_name=%s",
			record_id, cur.seg, name);
		g_final_syncing = false;
		return 0;
	}

	state_get_int("segment_no", &seg_no);
	seg = seg_no + 1;
	build_record_output_name(c, seg, task_id, (long long)st.st_mtime,
				 name, sizeof(name));
	named_local_prepare(c, local, name, named, sizeof(named));
	build_record_remote_path(c, record_id, name, rel, sizeof(rel));
	log_msg(ENCM_LOG_DEBUG,
		"record stop upload attempt record_id=%s local_file=%s named_file=%s remote_name=%s remote_path=%s",
		record_id, local, named, name, rel);

	memset(&row, 0, sizeof(row));
	snprintf(row.file, sizeof(row.file), "%s", local);
	snprintf(row.kind, sizeof(row.kind), "record");
	snprintf(row.record_id, sizeof(row.record_id), "%s", record_id);
	snprintf(row.task_id, sizeof(row.task_id), "%s",
		 task_id ? task_id : "");
	row.seg = (int)seg;
	row.size = (long long)st.st_size;
	row.mtime = (long long)st.st_mtime;
	row.state = DB_PENDING;
	row.retry = cur.file[0] ? cur.retry : 0;
	row.ts = now_ms();
	encdb_rec_add(&row);		/* 失败也留账本，交重传扫描接手 */

	rc = upload_file(c, rt, named, rel);
	if (rc == 0) {
		row.state = DB_UPLOADED;
		encdb_rec_add(&row);
		state_set_int("segment_no", seg);
		manifest_append(local);
		snprintf(out_name, osz, "%s", name);
		log_msg(ENCM_LOG_INFO,
			"record stop final upload success record_id=%s segment_no=%ld remote_name=%s remote_path=%s size=%lld",
			record_id, seg, name, rel, row.size);
		g_final_syncing = false;
		return 0;
	}
	attempt_mark_failed(&row, rc);
	log_msg(ENCM_LOG_ERROR,
		"record stop final upload failed record_id=%s local_file=%s rc=%d",
		record_id, local, rc);
	g_final_syncing = false;
	return -1;
}

/* ------------------------------------------------------------------ */
/* purge（--purge / record_purge.sh 换芯，设计 §7.5）                     */
/* ------------------------------------------------------------------ */

/* 只删 db 中 state=uploaded 且 mtime 超龄的文件（含 kind=capture）；
 * 未上传/未到龄的跳过并计数。文件已不在盘上的 uploaded 记录直接清账本。
 * 输出单行 JSON：{"purgedFiles":N,"freedBytes":B,"skippedFiles":M} */
int purge_run(const enc_cfg_t *c, int older_than_hours,
	      char *out_json, size_t sz)
{
	db_rec_t   *arr = NULL;
	int         n = 0, cap = 0, cursor = 0, i;
	long long   cutoff, purged = 0, freed = 0, skipped = 0;
	db_rec_t    r;
	struct stat st;

	(void)c;
	if (out_json && sz)
		out_json[0] = '\0';
	cutoff = (long long)time(NULL) -
		 (older_than_hours > 0 ? (long long)older_than_hours * 3600LL
				       : 0);

	while (encdb_rec_next(&cursor, &r) == 1) {
		db_rec_t *tmp;

		if (n >= cap) {
			cap = cap > 0 ? cap * 2 : 32;
			tmp = (db_rec_t *)realloc(arr,
						  (size_t)cap * sizeof(*arr));
			if (!tmp)
				break;
			arr = tmp;
		}
		arr[n++] = r;
	}

	for (i = 0; i < n; i++) {
		const db_rec_t *rec = &arr[i];

		if (rec->state == DB_UPLOADED && rec->mtime > 0 &&
		    rec->mtime <= cutoff) {
			if (stat(rec->file, &st) == 0) {
				if (unlink(rec->file) == 0) {
					encdb_rec_del(rec->file);
					purged++;
					freed += (long long)st.st_size;
				} else {
					skipped++;
				}
			} else {
				/* 文件已消失：仅清账本，不计删除/释放 */
				encdb_rec_del(rec->file);
			}
		} else if (stat(rec->file, &st) == 0) {
			skipped++;	/* 未上传或未到龄：保留 */
		}
	}
	free(arr);

	if (out_json && sz)
		snprintf(out_json, sz,
			 "{\"purgedFiles\":%lld,\"freedBytes\":%lld,\"skippedFiles\":%lld}",
			 purged, freed, skipped);
	log_msg(ENCM_LOG_INFO,
		"purge done older_than_hours=%d purged=%lld freed_bytes=%lld skipped=%lld",
		older_than_hours, purged, freed, skipped);
	return 0;
}
