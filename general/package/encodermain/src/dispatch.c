/*
 * dispatch.c — 命令队列 + L1 msgId / L2 taskid 幂等 + 命令 worker 线程
 *
 * 对齐 bash 版 app_service.sh（listener/dispatch 流程、命令缓存）与
 * EncoderMainDesign.md §6.1（L1/L2 幂等）：
 *   - dispatch_on_message：register_ack/heartbeat_ack 快速通道，其余入队
 *   - worker：proto_parse → L1 重放 → 状态机应答特判（先于 L2）→ L2 重放 →
 *     feat_execute → 组 ACK → 先写 L1/L2 缓存再 persist 发布
 *   - 状态/last_record 取值全部走 state_get_*（key = bash 状态文件名）
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "proto_internal.h"

/* L1 命令缓存上限（bash COMMAND_CACHE_MAX_ENTRIES=128） */
#define ENCM_CMD_CACHE_MAX      128
/* L2 executing 状态的陈旧判定等待（秒）：单 worker 下仅崩溃残留会命中 */
#define ENCM_DEDUP_EXEC_WAIT_SEC 10

typedef struct {
	char topic[256];
	char payload[2048];
} disp_msg_t;

/* ------------------------------------------------------------------ */
/* 模块状态                                                             */
/* ------------------------------------------------------------------ */

static disp_msg_t     *g_queue;
static int             g_queue_cap;
static int             g_q_head, g_q_tail, g_q_count;
static pthread_mutex_t g_q_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_q_cond;
static pthread_t       g_worker;
static bool            g_worker_running;
static volatile bool   g_stop;

/* registerAck 等待器（replyTo/msgId 匹配 → 记录 data → cond_signal） */
static pthread_mutex_t g_reg_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_reg_cond;
static bool            g_reg_waiting;
static long long       g_reg_msgid;
static bool            g_reg_hit;
static char            g_reg_data[1536];

static char g_dir_cache[512];   /* <state_dir>/command_cache */
static char g_dir_dedup[512];   /* <state_dir>/task_dedup    */

/* ------------------------------------------------------------------ */
/* state 读取辅助（key 与 bash 状态文件名一致）                          */
/* ------------------------------------------------------------------ */

static bool state_bool(const char *key, bool def)
{
	char buf[16];

	if (!state_get_str(key, buf, sizeof(buf)) || !buf[0])
		return def;
	return strcmp(buf, "true") == 0;
}

static long long state_ll(const char *key, long long def)
{
	long v;

	if (!state_get_int(key, &v))
		return def;
	return (long long)v;
}

/* ------------------------------------------------------------------ */
/* L1 命令缓存 / L2 task 去重（文件布局照 bash service_command_cache_*） */
/* ------------------------------------------------------------------ */

static void cache_path(char *dst, size_t sz, const char *dir,
		       const char *key, const char *suffix)
{
	snprintf(dst, sz, "%s/%s%s", dir, key, suffix);
}

/* LRU：按 .done mtime 淘汰最旧一组（bash ls -1t | tail 同效果） */
static void cache_evict(const char *dir, int max_entries);

/* 对 JSON 片段做数字字段的字符串替换（用于重放 ACK 时修正 msgId/replyTo）。
 * dst 必须有足够空间；匹配 `"${key}":${old}`（无引号包裹数字），替换成 `"${key}":${new}`。
 * 找不到就原样拷贝，返回 false；找到至少一处匹配返回 true。 */
static bool json_replace_i64(char *dst, size_t sz, const char *src,
			     const char *key, long long new_val)
{
	char needle[128];
	snprintf(needle, sizeof(needle), "\"%s\":", key);
	size_t klen = strlen(needle);
	size_t si = 0, di = 0;
	bool hit = false;

	while (src[si] && di + 1 < sz) {
		if (strncmp(src + si, needle, klen) == 0) {
			size_t tj = si + klen;
			/* 读一个整数（可选负号） */
			long long old_val = 0;
			bool neg = false;
			if (src[tj] == '-') { neg = true; tj++; }
			if (src[tj] < '0' || src[tj] > '9') {
				/* 不是数字：原样拷 "key":*/
				if (di + klen >= sz) break;
				memcpy(dst + di, needle, klen);
				di += klen; si = si + klen;
				continue;
			}
			while (src[tj] >= '0' && src[tj] <= '9')
				old_val = old_val * 10 + (src[tj++] - '0');
			if (neg) old_val = -old_val;
			(void)old_val;
			/* 写入新值 */
			char sub[128];
			int sl = snprintf(sub, sizeof(sub), "%s%lld", needle, new_val);
			if (sl > 0 && (size_t)sl < sz - di) {
				memcpy(dst + di, sub, (size_t)sl);
				di += (size_t)sl;
				si = tj;
				hit = true;
				continue;
			}
			/* 空间不足：原样 */
			if (di + klen >= sz) break;
			memcpy(dst + di, needle, klen);
			di += klen; si = si + klen;
			continue;
		}
		dst[di++] = src[si++];
	}
	dst[di] = '\0';
	return hit;
}

/* 命中重放：.done 存在即视为重复命令，重放缓存 topic/payload（bash 同语义）。
 * new_msg_id != 0 时，将 ACK payload 顶层 msgId 与 data.replyTo 更新为新值，
 * 保证重发端（新 msgId 的那一条请求）能匹配对应 ACK（设计 §6.1 L1/L2 幂等语义）。 */
static bool cache_replay(const char *dir, const char *key, const char *what,
			 long long new_msg_id)
{
	char path[800], topic[256], payload[2048], new_payload[2048], val[32];

	cache_path(path, sizeof(path), dir, key, ".done");
	if (!read_str_file(path, val, sizeof(val)))
		return false;

	topic[0] = '\0';
	payload[0] = '\0';
	cache_path(path, sizeof(path), dir, key, ".topic");
	read_str_file(path, topic, sizeof(topic));
	cache_path(path, sizeof(path), dir, key, ".payload");
	read_str_file(path, payload, sizeof(payload));

	log_msg(ENCM_LOG_WARN, "[DISPATCH] duplicate %s replayed key=%s",
		what, key);
	if (topic[0] && payload[0]) {
		const char *out = payload;
		if (new_msg_id != 0) {
			char tmp[2048];
			json_replace_i64(tmp, sizeof(tmp), payload,
					 "replyTo", new_msg_id);
			json_replace_i64(new_payload, sizeof(new_payload),
					 tmp, "msgId", new_msg_id);
			out = new_payload;
		}
		mq_publish(g_app.mq, topic, out, true);
	}
	return true;
}

/* 先写缓存再发布（缩小重复执行窗口，bash service_dispatch 同序） */
static void cache_store(const char *dir, const char *key, const char *topic,
			const char *payload, int max_entries)
{
	char path[800], ts[32];

	if (topic[0]) {
		cache_path(path, sizeof(path), dir, key, ".topic");
		write_str_file(path, topic);
	}
	if (payload[0]) {
		cache_path(path, sizeof(path), dir, key, ".payload");
		write_str_file(path, payload);
	}
	cache_path(path, sizeof(path), dir, key, ".done");
	snprintf(ts, sizeof(ts), "%lld", (long long)time(NULL));
	write_str_file(path, ts);
	cache_evict(dir, max_entries);
}

/* LRU：按 .done mtime 淘汰最旧一组（bash ls -1t | tail 同效果） */
static void cache_evict(const char *dir, int max_entries)
{
	char oldest[768], path[800];

	if (max_entries <= 0)
		max_entries = 1;
	for (;;) {
		DIR *d;
		struct dirent *e;
		struct stat st;
		int count = 0;
		long oldest_mt = 0;

		oldest[0] = '\0';
		d = opendir(dir);
		if (!d)
			return;
		while ((e = readdir(d)) != NULL) {
			size_t l = strlen(e->d_name);

			if (l < 6 || strcmp(e->d_name + l - 5, ".done") != 0)
				continue;
			count++;
			snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
			if (stat(path, &st) == 0 &&
			    (oldest[0] == '\0' || st.st_mtime < oldest_mt)) {
				oldest_mt = st.st_mtime;
				snprintf(oldest, sizeof(oldest), "%s/%.*s",
					 dir, (int)(l - 5), e->d_name);
			}
		}
		closedir(d);

		if (count <= max_entries || !oldest[0])
			return;
		/* oldest = <dir>/<key>，删除同组三个文件 */
		snprintf(path, sizeof(path), "%s.done", oldest);
		unlink(path);
		snprintf(path, sizeof(path), "%s.topic", oldest);
		unlink(path);
		snprintf(path, sizeof(path), "%s.payload", oldest);
		unlink(path);
	}
}

/* L2 命中检查。返回 0=未命中；1=命中且已重放；2=executing 等待超时（陈旧） */
static int dedup_check(const char *dir, const char *key, long long new_msg_id)
{
	char path[800], val[32];
	int i;

	cache_path(path, sizeof(path), dir, key, ".done");
	for (i = 0;; i++) {
		if (!read_str_file(path, val, sizeof(val)))
			return 0;
		if (strcmp(val, "executing") != 0)
			break;
		if (i >= ENCM_DEDUP_EXEC_WAIT_SEC)
			return 2;
		sleep(1);
	}
	cache_replay(dir, key, "task command", new_msg_id);
	return 1;
}

/* ------------------------------------------------------------------ */
/* L2 bizid 派生                                                        */
/* ------------------------------------------------------------------ */

/* 返回 false = 无 bizid，跳过 L2。norm_url 接收规范化后的推流 URL。 */
static bool task_dedup_key(const cmd_t *cmd, proto_cmd_t cc, char *key,
			   size_t sz, char *norm_url, size_t nsz)
{
	char raw[512];
	const char *biz = NULL;

	switch (cc) {
	case ENCM_CMD_TASK_PREPARE_DESK_VOICE:
	case ENCM_CMD_TASK_STREAM_START:
	case ENCM_CMD_TASK_RECORD_START:
	case ENCM_CMD_TASK_RECORD_STOP:
	case ENCM_CMD_TASK_RESET:
		biz = cmd->task_id;
		break;
	case ENCM_CMD_RECORD_START:
	case ENCM_CMD_RECORD_STOP:
		biz = cmd->record_id;
		break;
	case ENCM_CMD_STREAM_START:
	case ENCM_CMD_STREAM_STOP: {
		/* 规范化 streamUrl；为空时回退 rt.srs_url（rt 受 rt_mutex 保护） */
		char fb[300];
		const char *fbp = NULL;

		pthread_mutex_lock(&g_app.rt_mutex);
		snprintf(fb, sizeof(fb), "%s", g_app.rt.srs_url);
		pthread_mutex_unlock(&g_app.rt_mutex);
		if (fb[0])
			fbp = fb;
		if (proto_stream_url_normalize(cmd->stream_url, fbp,
					       cmd->device_id, norm_url, nsz))
			biz = norm_url;
		break;
	}
	case ENCM_CMD_CAPTURE_TAKE:
		biz = cmd->capture_id;
		break;
	default:
		break;
	}

	if (!biz || !biz[0])
		return false;
	snprintf(raw, sizeof(raw), "%s_%s_%s", cmd->flow, cmd->action, biz);
	proto_key_sanitize(key, sz, raw);
	return true;
}

/* ------------------------------------------------------------------ */
/* 状态机应答特判（bash 语义，优先级高于 L2：先查设备状态再查 L2）        */
/* 命中返回 true 并填充 r（含 extra_json 覆盖字段）                      */
/* ------------------------------------------------------------------ */

static bool state_special_case(const cmd_t *cmd, proto_cmd_t cc,
			       feat_result_t *r)
{
	char last_id[64], last_fn[256], last_fu[256], cur_url[256];

	switch (cc) {
	case ENCM_CMD_RECORD_START:
	case ENCM_CMD_TASK_RECORD_START:
		/* 录像中收到新 msgId 的重复开始：code=1 status=duplicate，
		 * 绝不覆盖当前活动录像任务 */
		if (!state_bool("is_recording", false))
			return false;
		r->code = 1;
		snprintf(r->status, sizeof(r->status), "duplicate");
		log_msg(ENCM_LOG_WARN,
			"record start rejected as duplicate requested_record_id=%s",
			cmd->record_id);
		return true;

	case ENCM_CMD_RECORD_STOP:
	case ENCM_CMD_TASK_RECORD_STOP:
		/* 无活动录像但 recordId==last_record_id：用 last_record_* 重建成功 ACK */
		if (state_bool("is_recording", false))
			return false;
		if (!state_get_str("last_record_id", last_id, sizeof(last_id)) ||
		    !last_id[0] || strcmp(cmd->record_id, last_id) != 0)
			return false;
		state_get_str("last_record_file_name", last_fn,
			      sizeof(last_fn));
		state_get_str("last_record_file_url", last_fu,
			      sizeof(last_fu));
		r->code = 0;
		snprintf(r->status, sizeof(r->status),
			 cc == ENCM_CMD_TASK_RECORD_STOP ? "success" :
							   "streaming");
		snprintf(r->last_file, sizeof(r->last_file), "%s", last_fn);
		r->last_size = state_ll("last_record_file_size", 0);
		/* lastSegmentUploaded 的 fileUrl/segmentNo 走 extra 覆盖 */
		{
			sb_t e;

			sb_init(&e);
			proto_data_put_str(&e, "fileUrl", last_fu);
			proto_data_put_int(&e, "segmentNo",
					   state_ll("last_record_segment_no",
						    0));
			snprintf(r->extra_json, sizeof(r->extra_json), "%s",
				 e.s ? e.s : "");
			sb_free(&e);
		}
		log_msg(ENCM_LOG_WARN,
			"record stop duplicate reused cached result record_id=%s",
			cmd->record_id);
		return true;

	case ENCM_CMD_STREAM_START:
	case ENCM_CMD_TASK_STREAM_START:
		/* 录像中同 URL：回 status=streaming，不刷新 duration/不重载。
		 * URL 比较用 bash build_stream_url_with_name 规范化结果
		 * （requested 非空时与 feature 落盘的 current_stream_url 同规则） */
		if (!state_bool("is_recording", false) ||
		    !state_bool("is_publishing", false))
			return false;
		if (!cmd->stream_url[0])
			return false;
		if (!state_get_str("current_stream_url", cur_url,
				   sizeof(cur_url)))
			return false;
		{
			char norm[300];
			sb_t e;

			if (!proto_stream_url_normalize(cmd->stream_url, NULL,
							cmd->device_id, norm,
							sizeof(norm)) ||
			    strcmp(norm, cur_url) != 0)
				return false;
			sb_init(&e);
			proto_data_put_str(&e, "streamUrl", norm);
			snprintf(r->extra_json, sizeof(r->extra_json), "%s",
				 e.s ? e.s : "");
			sb_free(&e);
		}
		r->code = 0;
		snprintf(r->status, sizeof(r->status), "streaming");
		log_msg(ENCM_LOG_WARN,
			"stream start ignored during active record stream_url=%s",
			cmd->stream_url);
		return true;

	default:
		return false;
	}
}

/* ------------------------------------------------------------------ */
/* registerAck 快速通道                                                 */
/* ------------------------------------------------------------------ */

/* registerAck data 重建：仅重建 app_service.sh service_apply_register_runtime
 * 消费的字段（code、desc、timestamp、ftp 与 srs 各四项），结构保持嵌套 */
static void reg_field(sb_t *b, const jv_t *data, const char *path,
		      const char *name)
{
	const jv_t *v = jv_path(data, path);

	if (!v)
		return;
	if (jv_str(v))
		proto_data_put_str(b, name, jv_str(v));
	else if (jv_is_num(v))
		proto_data_put_int(b, name, jv_int(v, 0));
}

static void reg_obj(sb_t *b, const jv_t *data, const char *prefix,
		    const char *name)
{
	static const char *fields[] = { "host", "port", "username",
					"password" };
	sb_t inner;
	size_t i;
	bool any = false;

	sb_init(&inner);
	for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		char path[64];

		snprintf(path, sizeof(path), "%s.%s", prefix, fields[i]);
		reg_field(&inner, data, path, fields[i]);
		if (inner.len > 0)
			any = true;
	}
	if (any) {
		if (b->len > 0 && b->s[b->len - 1] != '{')
			sb_putc(b, ',');
		sb_fmt(b, "\"%s\":{%s}", name, inner.s ? inner.s : "");
	}
	sb_free(&inner);
}

static void handle_register_ack(const char *payload)
{
	jv_t *j;
	const jv_t *v;
	long long reply_to = 0, ack_id = 0, expect;
	bool have;
	char data_json[1536];
	sb_t d;

	j = jv_parse(payload ? payload : "");
	if (!j)
		return;
	v = jv_path(j, "data.replyTo");
	if (v)
		reply_to = jv_int(v, 0);
	v = jv_path(j, "msgId");
	if (v)
		ack_id = jv_int(v, 0);

	pthread_mutex_lock(&g_reg_mutex);
	expect = g_reg_msgid;
	have = g_reg_waiting;
	pthread_mutex_unlock(&g_reg_mutex);

	/* bash service_register_ack_listener：replyTo 或 msgId 匹配即可 */
	if (!have || (reply_to != expect && ack_id != expect)) {
		jv_free(j);
		return;
	}

	v = jv_path(j, "data");
	sb_init(&d);
	if (v) {
		/* jv_path 仅对 object 生效；字段缺失时自动跳过 */
		reg_field(&d, v, "code", "code");
		reg_field(&d, v, "desc", "desc");
		reg_field(&d, v, "timestamp", "timestamp");
		reg_obj(&d, v, "ftp", "ftp");
		reg_obj(&d, v, "srs", "srs");
	}
	snprintf(data_json, sizeof(data_json), "%s", d.s ? d.s : "");
	sb_free(&d);

	pthread_mutex_lock(&g_reg_mutex);
	g_reg_hit = true;
	snprintf(g_reg_data, sizeof(g_reg_data), "%s", data_json);
	pthread_cond_broadcast(&g_reg_cond);
	pthread_mutex_unlock(&g_reg_mutex);
	log_msg(ENCM_LOG_INFO, "[MQTT-RECV] register_ack matched msgid=%lld",
		expect);
	jv_free(j);
}

/* ------------------------------------------------------------------ */
/* 命令队列                                                             */
/* ------------------------------------------------------------------ */

static void enqueue(const char *topic, const char *payload)
{
	pthread_mutex_lock(&g_q_mutex);
	if (g_q_count >= g_queue_cap) {
		/* 满丢最旧 + ERROR（防异常洪泛拖垮内存，设计 §8.3） */
		g_q_head = (g_q_head + 1) % g_queue_cap;
		g_q_count--;
		log_msg(ENCM_LOG_ERROR,
			"command queue overflow, drop oldest topic=%s",
			g_queue[g_q_head].topic);
	}
	snprintf(g_queue[g_q_tail].topic, sizeof(g_queue[g_q_tail].topic),
		 "%s", topic ? topic : "");
	snprintf(g_queue[g_q_tail].payload, sizeof(g_queue[g_q_tail].payload),
		 "%s", payload ? payload : "");
	g_q_tail = (g_q_tail + 1) % g_queue_cap;
	g_q_count++;
	pthread_cond_signal(&g_q_cond);
	pthread_mutex_unlock(&g_q_mutex);
}

/* mq_msg_cb 适配：MQTT 线程回调带 ud 首参，剥离后进业务入口 */
void dispatch_mq_cb(void *ud, const char *topic, const char *payload)
{
	(void)ud;
	dispatch_on_message(topic, payload);
}

void dispatch_on_message(const char *topic, const char *payload)
{
	char seg[PROTO_SEG_NUM][PROTO_SEG_SZ];

	if (!topic || proto_topic_split(topic, seg) != 0) {
		log_msg(ENCM_LOG_DEBUG, "ignore invalid topic=%s",
			topic ? topic : "");
		return;
	}
	if (!strcmp(seg[4], "heartbeat")) {
		/* 快速通道：register_ack 走等待器；heartbeat_ack 直接忽略 */
		if (!strcmp(seg[5], "register_ack")) {
			handle_register_ack(payload);
			return;
		}
		if (!strcmp(seg[5], "heartbeat_ack"))
			return;
		return;
	}
	enqueue(topic, payload);
}

/* ------------------------------------------------------------------ */
/* worker                                                               */
/* ------------------------------------------------------------------ */

static void worker_handle(const disp_msg_t *m)
{
	cmd_t cmd;
	feat_result_t r;
	proto_cmd_t cc;
	char l1raw[256], l1key[320], l2key[320], norm_url[300];
	const char *ours;

	/* 1. 解析：失败 log 不回包（与 bash dispatch exit 1 一致） */
	if (proto_parse(m->topic, m->payload, &cmd) != 0) {
		log_msg(ENCM_LOG_ERROR,
			"dispatch parse failed topic=%s payload=%s",
			m->topic, m->payload);
		return;
	}

	/* 接收方校验（bash：receiver!=encoder 或 sub!=DEVICE_ID → ignore） */
	ours = device_id_get(&g_app.cfg);
	if (!ours || strcmp(cmd.device_id, ours) != 0) {
		log_msg(ENCM_LOG_DEBUG, "ignore topic=%s", m->topic);
		return;
	}
	if (!strcmp(cmd.flow, "heartbeat")) {
		/* register_ack/heartbeat_ack 不属业务命令（正常不会到这里） */
		return;
	}

	cc = proto_command_map(&cmd);
	if (cc == ENCM_CMD_NONE) {
		log_msg(ENCM_LOG_ERROR,
			"dispatch unsupported request flow=%s action=%s msg=%s",
			cmd.flow, cmd.action, cmd.msg);
		return;
	}
	if (cmd.msg_id == 0) {
		log_msg(ENCM_LOG_ERROR, "dispatch missing msgId topic=%s",
			m->topic);
		return;
	}
	log_msg(ENCM_LOG_INFO,
		"[DISPATCH] command=%s msg_id=%lld task_id=%s record_id=%s stream_url=%s",
		proto_cmd_name(cc), cmd.msg_id, cmd.task_id, cmd.record_id,
		cmd.stream_url);

	/* 2. L1 幂等：key = sender_senderSub_flow_action_msgId */
	snprintf(l1raw, sizeof(l1raw), "%s_%s_%s_%s_%lld", cmd.sender,
		 cmd.sender_sub, cmd.flow, cmd.action, cmd.msg_id);
	proto_key_sanitize(l1key, sizeof(l1key), l1raw);
	if (cache_replay(g_dir_cache, l1key, "command", cmd.msg_id))
		return;

	/* 3. 状态机应答特判（bash 语义，先于 L2） */
	memset(&r, 0, sizeof(r));
	r.code = -1;
	snprintf(r.status, sizeof(r.status), "fail");
	if (state_special_case(&cmd, cc, &r)) {
		char topic[256], payload[2048];

		proto_ack_build(&cmd, &r, topic, sizeof(topic), payload,
				sizeof(payload));
		cache_store(g_dir_cache, l1key, topic, payload,
			    ENCM_CMD_CACHE_MAX);
		if (topic[0]) {
			mq_publish(g_app.mq, topic, payload, true);
			log_msg(ENCM_LOG_INFO,
				"[MQTT-PUB] topic=%s status=%s code=%d",
				topic, r.status, r.code);
		}
		return;
	}

	/* 4. L2 taskid 幂等：key = flow_action_bizid */
	l2key[0] = '\0';
	norm_url[0] = '\0';
	if (cc != ENCM_CMD_TASK_PREPARE_DESK_VOICE &&
	    task_dedup_key(&cmd, cc, l2key, sizeof(l2key), norm_url,
			   sizeof(norm_url))) {
		char done_path[800];

		switch (dedup_check(g_dir_dedup, l2key, cmd.msg_id)) {
		case 1:
			return;         /* 命中已重放，跳过业务 */
		case 2:
			log_msg(ENCM_LOG_WARN,
				"task dedup executing stale, re-execute key=%s",
				l2key);
			break;
		default:
			break;
		}
		/* 登记 executing；完成后改写为最终回包（设计 §6.1） */
		cache_path(done_path, sizeof(done_path), g_dir_dedup, l2key,
			   ".done");
		write_str_file(done_path, "executing");
	}

	/* 5. 执行业务（feature_media.c 实现；内部自带 Majestic 锁） */
	feat_execute(&cmd, &r);

	/* 6. 组 ACK → 先写 L1/L2 缓存 → persist 发布 */
	{
		char topic[256], payload[2048];

		proto_ack_build(&cmd, &r, topic, sizeof(topic), payload,
				sizeof(payload));
		cache_store(g_dir_cache, l1key, topic, payload,
			    ENCM_CMD_CACHE_MAX);
		if (l2key[0] && topic[0])
			cache_store(g_dir_dedup, l2key, topic, payload,
				    g_app.cfg.task_dedup_max);
		if (topic[0]) {
			mq_publish(g_app.mq, topic, payload, true);
			log_msg(ENCM_LOG_INFO,
				"[MQTT-PUB] topic=%s status=%s code=%d",
				topic, r.status, r.code);
		}
	}
}

static void *worker_main(void *arg)
{
	(void)arg;
	for (;;) {
		disp_msg_t msg;

		pthread_mutex_lock(&g_q_mutex);
		while (g_q_count == 0 && !g_stop)
			pthread_cond_wait(&g_q_cond, &g_q_mutex);
		if (g_stop) {
			/* 退出不排空：bash SIGTERM 语义，未执行命令直接丢弃 */
			pthread_mutex_unlock(&g_q_mutex);
			break;
		}
		msg = g_queue[g_q_head];
		g_q_head = (g_q_head + 1) % g_queue_cap;
		g_q_count--;
		pthread_mutex_unlock(&g_q_mutex);

		worker_handle(&msg);
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                             */
/* ------------------------------------------------------------------ */

int dispatch_init(void)
{
	char path[512];

	if (g_worker_running)
		return 0;

	dir_ensure(g_app.cfg.state_dir);
	snprintf(g_dir_cache, sizeof(g_dir_cache), "%s/command_cache",
		 g_app.cfg.state_dir);
	snprintf(g_dir_dedup, sizeof(g_dir_dedup), "%s/task_dedup",
		 g_app.cfg.state_dir);
	dir_ensure(g_dir_cache);
	dir_ensure(g_dir_dedup);

	g_queue_cap = g_app.cfg.command_queue_max > 0 ?
		      g_app.cfg.command_queue_max : 32;
	g_queue = (disp_msg_t *)calloc((size_t)g_queue_cap, sizeof(disp_msg_t));
	if (!g_queue)
		return -1;
	g_q_head = g_q_tail = g_q_count = 0;
	g_stop = false;
	pthread_cond_init(&g_q_cond, NULL);
	pthread_cond_init(&g_reg_cond, NULL);

	if (pthread_create(&g_worker, NULL, worker_main, NULL) != 0) {
		free(g_queue);
		g_queue = NULL;
		return -1;
	}
	g_worker_running = true;
	snprintf(path, sizeof(path), "%s/msgid", g_app.cfg.state_dir);
	log_msg(ENCM_LOG_INFO,
		"dispatch worker started queue_max=%d task_dedup_max=%d msgid_file=%s",
		g_queue_cap,
		g_app.cfg.task_dedup_max > 0 ? g_app.cfg.task_dedup_max : 256,
		path);
	return 0;
}

void dispatch_shutdown(void)
{
	if (!g_worker_running)
		return;
	pthread_mutex_lock(&g_q_mutex);
	g_stop = true;
	pthread_cond_broadcast(&g_q_cond);
	pthread_mutex_unlock(&g_q_mutex);
	pthread_join(g_worker, NULL);
	g_worker_running = false;

	/* 唤醒仍挂在 registerAck 等待上的线程（如有） */
	pthread_mutex_lock(&g_reg_mutex);
	g_reg_hit = false;
	pthread_cond_broadcast(&g_reg_cond);
	pthread_mutex_unlock(&g_reg_mutex);

	free(g_queue);
	g_queue = NULL;
}

/* ------------------------------------------------------------------ */
/* register / heartbeat                                                 */
/* ------------------------------------------------------------------ */

int dispatch_send_register(const enc_cfg_t *c)
{
	char topic[256];
	long long msgid;
	sb_t b;

	msgid = msgid_next();
	proto_register_payload_build(&b, msgid,
				     c->version[0] ? c->version : "unknown");
	if (!b.ok) {
		sb_free(&b);
		return -1;
	}
	proto_topic_build(topic, sizeof(topic), "encoder", device_id_get(c),
			  "ctrlsrv", "0", "heartbeat", "register");

	/* 先登记等待再发布：ACK 抢先到达时才能命中（bash 先挂 listener 同理） */
	pthread_mutex_lock(&g_reg_mutex);
	g_reg_waiting = true;
	g_reg_msgid = msgid;
	g_reg_hit = false;
	g_reg_data[0] = '\0';
	pthread_mutex_unlock(&g_reg_mutex);

	if (mq_publish(g_app.mq, topic, b.s, true) != 0) {
		pthread_mutex_lock(&g_reg_mutex);
		g_reg_waiting = false;
		pthread_mutex_unlock(&g_reg_mutex);
		sb_free(&b);
		log_msg(ENCM_LOG_ERROR, "register publish failed topic=%s",
			topic);
		return -1;
	}
	log_msg(ENCM_LOG_INFO, "[MQTT-PUB] topic=%s msg=register msgid=%lld",
		topic, msgid);
	sb_free(&b);
	/* 返回本次取号 msgid（>0），供 dispatch_wait_register_ack 匹配 */
	return (msgid > 0x7fffffffLL) ? -1 : (int)msgid;
}

bool dispatch_wait_register_ack(long long msgid, int timeout_ms,
				char *out_data, size_t sz)
{
	struct timespec ts;
	bool hit = false;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += timeout_ms / 1000;
	ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec += 1;
		ts.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&g_reg_mutex);
	/* send_register 已登记则沿用（保留其间到达的 ACK）；否则重新登记 */
	if (!g_reg_waiting || g_reg_msgid != msgid) {
		g_reg_waiting = true;
		g_reg_msgid = msgid;
		g_reg_hit = false;
		g_reg_data[0] = '\0';
	}
	while (!g_reg_hit && !g_stop) {
		int rc = pthread_cond_timedwait(&g_reg_cond, &g_reg_mutex,
						&ts);

		if (rc == ETIMEDOUT)
			break;
	}
	hit = g_reg_hit;
	if (hit && out_data && sz > 0)
		snprintf(out_data, sz, "%s", g_reg_data);
	g_reg_waiting = false;
	g_reg_hit = false;
	pthread_mutex_unlock(&g_reg_mutex);
	return hit;
}

int dispatch_send_heartbeat(const enc_cfg_t *c)
{
	char topic[256];
	long long msgid;
	long signal = 0, battery = 100, voltage_mv = 0;
	sb_t b;

	/* 字段值全部来自 state 文件（bash state_get_* 同名） */
	state_get_int("signal", &signal);
	state_get_int("battery", &battery);
	state_get_int("battery_voltage_mv", &voltage_mv);

	msgid = msgid_next();
	proto_heartbeat_payload_build(&b, msgid,
				      c->version[0] ? c->version : "unknown",
				      state_bool("is_idle", true),
				      state_bool("is_recording", false),
				      state_bool("is_publishing", false),
				      state_bool("is_charging", false),
				      signal, battery, voltage_mv);
	if (!b.ok) {
		sb_free(&b);
		return -1;
	}
	proto_topic_build(topic, sizeof(topic), "encoder", device_id_get(c),
			  "ctrlsrv", "0", "heartbeat", "heartbeat");
	/* 心跳不落 outbox（bash mosquitto_pub 直发语义），失败由下一轮重发 */
	{
		int rc = mq_publish(g_app.mq, topic, b.s, false);

		if (rc == 0)
			log_msg(ENCM_LOG_INFO,
				"[MQTT-PUB] topic=%s msg=heartbeat msgid=%lld",
				topic, msgid);
		sb_free(&b);
		return rc;
	}
}
