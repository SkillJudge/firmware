/*
 * protocol.c — topic 拆解 + ACK/事件 payload 组装 + msgid 取号
 *
 * 逐字对齐 bash 版 protocol.sh / app_service.sh：
 *   - proto_parse            ← protocol_parse_command 的 topic/payload 字段提取
 *   - proto_ack_build        ← protocol_build_command_result（含 data 字段名/类型/顺序）
 *   - proto_register/heartbeat_payload_build ← protocol_build_register_payload /
 *     protocol_build_heartbeat_payload
 *   - msgid_next             ← common.sh next_msgid（<state_dir>/msgid，起始 1000，先加后写）
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "proto_internal.h"

/* ------------------------------------------------------------------ */
/* topic 拆解 / 组装                                                     */
/* ------------------------------------------------------------------ */

int proto_topic_split(const char *topic, char seg[][PROTO_SEG_SZ])
{
	const char *p = topic;
	int n = 0;

	if (!topic)
		return -1;
	while (*p) {
		const char *q = strchr(p, '/');
		size_t l = q ? (size_t)(q - p) : strlen(p);

		/* bash IFS='/' 分词会丢弃空段导致段数变化，这里直接判失败 */
		if (n >= PROTO_SEG_NUM || l == 0 || l >= PROTO_SEG_SZ)
			return -1;
		memcpy(seg[n], p, l);
		seg[n][l] = '\0';
		n++;
		if (!q)
			break;
		p = q + 1;
	}
	return (n == PROTO_SEG_NUM) ? 0 : -1;
}

void proto_topic_build(char *out, size_t sz, const char *sender,
		       const char *sender_sub, const char *receiver,
		       const char *receiver_sub, const char *flow,
		       const char *action)
{
	snprintf(out, sz, "%s/%s/%s/%s/%s/%s",
		 sender ? sender : "", sender_sub ? sender_sub : "",
		 receiver ? receiver : "", receiver_sub ? receiver_sub : "",
		 flow ? flow : "", action ? action : "");
}

/* ------------------------------------------------------------------ */
/* payload 解析（proto_parse）                                          */
/* ------------------------------------------------------------------ */

static void cpy_str(char *dst, size_t sz, const char *s)
{
	snprintf(dst, sz, "%s", s ? s : "");
}

int proto_parse(const char *topic, const char *payload, cmd_t *c)
{
	char seg[PROTO_SEG_NUM][PROTO_SEG_SZ];
	const jv_t *v;
	jv_t *j;

	if (!c)
		return -1;
	memset(c, 0, sizeof(*c));

	/* 六段 topic：sender/senderSub/encoder/deviceId/flow/action */
	if (proto_topic_split(topic, seg) != 0)
		return -1;
	cpy_str(c->sender, sizeof(c->sender), seg[0]);
	cpy_str(c->sender_sub, sizeof(c->sender_sub), seg[1]);
	cpy_str(c->device_id, sizeof(c->device_id), seg[3]);
	cpy_str(c->flow, sizeof(c->flow), seg[4]);
	cpy_str(c->action, sizeof(c->action), seg[5]);

	cpy_str(c->raw_payload, sizeof(c->raw_payload), payload);

	/* payload 字段名照 protocol.sh：msgId/msg/data.recordId/captureId/
	 * taskId/streamUrl/duration/code（reason/fileName/filePath 不在 cmd_t，
	 * 由 feature 从 raw_payload 自取） */
	j = jv_parse(payload ? payload : "");
	if (!j)
		return -1;

	v = jv_path(j, "msgId");
	if (v)
		c->msg_id = jv_int(v, 0);
	v = jv_path(j, "msg");
	if (v && jv_str(v))
		cpy_str(c->msg, sizeof(c->msg), jv_str(v));

	v = jv_path(j, "data.taskId");
	if (v && jv_str(v))
		cpy_str(c->task_id, sizeof(c->task_id), jv_str(v));
	v = jv_path(j, "data.recordId");
	if (v && jv_str(v))
		cpy_str(c->record_id, sizeof(c->record_id), jv_str(v));
	v = jv_path(j, "data.captureId");
	if (v && jv_str(v))
		cpy_str(c->capture_id, sizeof(c->capture_id), jv_str(v));
	v = jv_path(j, "data.streamUrl");
	if (v && jv_str(v))
		cpy_str(c->stream_url, sizeof(c->stream_url), jv_str(v));
	v = jv_path(j, "data.duration");
	if (v) {
		c->duration = jv_int(v, 0);
		c->has_duration = true;
	}
	v = jv_path(j, "data.code");
	if (v)
		c->code = jv_int(v, 0);

	jv_free(j);
	return 0;
}

/* ------------------------------------------------------------------ */
/* 命令映射（bash protocol_parse_command case 表）                       */
/* ------------------------------------------------------------------ */

proto_cmd_t proto_command_map(const cmd_t *c)
{
	if (!c)
		return ENCM_CMD_NONE;
	if (!strcmp(c->flow, "record")) {
		if (!strcmp(c->action, "start_record") &&
		    !strcmp(c->msg, "startRecord"))
			return ENCM_CMD_RECORD_START;
		if (!strcmp(c->action, "stop_record") &&
		    !strcmp(c->msg, "stopRecord"))
			return ENCM_CMD_RECORD_STOP;
	} else if (!strcmp(c->flow, "capture")) {
		if (!strcmp(c->action, "capture") && !strcmp(c->msg, "capture"))
			return ENCM_CMD_CAPTURE_TAKE;
	} else if (!strcmp(c->flow, "stream")) {
		if (!strcmp(c->action, "start_stream") &&
		    !strcmp(c->msg, "startStream"))
			return ENCM_CMD_STREAM_START;
		if (!strcmp(c->action, "stop_stream") &&
		    !strcmp(c->msg, "stopStream"))
			return ENCM_CMD_STREAM_STOP;
	} else if (!strcmp(c->flow, "task")) {
		if (!strcmp(c->action, "prepare_desk_recognition_voice") &&
		    !strcmp(c->msg, "prepareDeskRecognitionVoice"))
			return ENCM_CMD_TASK_PREPARE_DESK_VOICE;
		if (!strcmp(c->action, "start_stream") &&
		    !strcmp(c->msg, "startStream"))
			return ENCM_CMD_TASK_STREAM_START;
		if (!strcmp(c->action, "start_record") &&
		    !strcmp(c->msg, "startRecord"))
			return ENCM_CMD_TASK_RECORD_START;
		if (!strcmp(c->action, "stop_record") &&
		    !strcmp(c->msg, "stopRecord"))
			return ENCM_CMD_TASK_RECORD_STOP;
		if (!strcmp(c->action, "reset_encoder") &&
		    !strcmp(c->msg, "resetEncoder"))
			return ENCM_CMD_TASK_RESET;
	}
	return ENCM_CMD_NONE;
}

const char *proto_cmd_name(proto_cmd_t cc)
{
	switch (cc) {
	case ENCM_CMD_RECORD_START:          return "record_start";
	case ENCM_CMD_RECORD_STOP:           return "record_stop";
	case ENCM_CMD_CAPTURE_TAKE:          return "capture_take";
	case ENCM_CMD_STREAM_START:          return "stream_start";
	case ENCM_CMD_STREAM_STOP:           return "stream_stop";
	case ENCM_CMD_TASK_PREPARE_DESK_VOICE: return "task_prepare_desk_voice";
	case ENCM_CMD_TASK_STREAM_START:     return "task_stream_start";
	case ENCM_CMD_TASK_RECORD_START:     return "task_record_start";
	case ENCM_CMD_TASK_RECORD_STOP:      return "task_record_stop";
	case ENCM_CMD_TASK_RESET:            return "task_reset";
	default:                             return "none";
	}
}

/* ------------------------------------------------------------------ */
/* key 清洗 / URL 规范化                                                */
/* ------------------------------------------------------------------ */

void proto_key_sanitize(char *buf, size_t sz, const char *src)
{
	size_t o = 0;

	if (!buf || sz < 2)
		return;
	/* T1.1 2026-09-01: 跳过"所有前导 '.' 或 '_'"，避免在 ext4/overlayfs
	 * 上生成 `.xxx.done` 这类 Linux 隐藏文件。隐藏文件会导致:
	 *   1) ls dedup/ 默认不显示 → 调试"凭空消失"
	 *   2) *.done glob 匹配不到 → dedup_cleanup 清理不掉（泄露）
	 *   3) dedup_check 用 glob 也可能漏匹配 → 去重语义错乱
	 * 前导 '_' 同样删，语义不变 (只是分隔符)，与 bash cache_key 生成一致。 */
	if (src) {
		while (*src && (*src == '.' || *src == '_'))
			src++;
	}
	for (; src && *src && o + 1 < sz; src++) {
		unsigned char ch = (unsigned char)*src;

		if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
		    (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
		    ch == '-')
			buf[o++] = (char)ch;
		else
			buf[o++] = '_';
	}
	buf[o] = '\0';
}

/* SRS_APP_DEFAULT 与 feature_media.c build_stream_url 语义保持一致：
 * 4 段式 scheme://host:port/<app>/<stream_name>；如果控制端下发 URL 缺独立
 * app 段则强制插入 "live"（修复 2026-09-01 outgoing.server 为空 bug — 单级
 * path 被 majestic 解析成 app 段后 playpath 空 → SRS 2051 StreamNameEmpty） */
#define SRS_APP_DEFAULT_PROTO  "live"

/* ensure_srs_app_default_inplace: 与 feature_media.c stream_url_ensure_app_default
 * 语义完全相同，独立实现（避免跨 .o 符号耦合）。
 * 2026-09-01 ROUND2 精修：段数>=2 且 首段名 != SRS_APP_DEFAULT_PROTO("live") 时
 * 仍视为"伪两级缺app"，强制丢弃非末段，host 与末段 name 之间重新插入 live。*/
static void ensure_srs_app_default_inplace(char *url, size_t sz)
{
	const char *scheme_end, *path, *p, *slash;
	char rebuild[1024];
	char name_part[256];
	char first_seg[128];
	size_t n_seg, host_len, base_len, fs_len;

	if (!url || !url[0] || sz < 16)
		return;
	scheme_end = strstr(url, "://");
	if (!scheme_end)
		return;
	path = strchr(scheme_end + 3, '/');
	if (!path || !path[1])
		return;
	n_seg = 0;
	for (p = path + 1; *p; ) {
		n_seg++;
		p = strchr(p, '/');
		if (!p) break;
		p++;
		while (*p == '/') p++;
		if (!*p) break;
	}
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
	if (n_seg >= 2 && fs_len > 0 &&
	    strcmp(first_seg, SRS_APP_DEFAULT_PROTO) == 0) {
		return;
	}
	host_len = (size_t)(path - url);
	slash = strrchr(path + 1, '/');
	if (slash)
		snprintf(name_part, sizeof(name_part), "%s", slash + 1);
	else
		snprintf(name_part, sizeof(name_part), "%s", path + 1);
	base_len = strlen(name_part);
	while (base_len > 0 && name_part[base_len - 1] == '/')
		name_part[--base_len] = '\0';
	if (!name_part[0])
		return;
	snprintf(rebuild, sizeof(rebuild), "%.*s/%s/%s",
		 (int)host_len, url, SRS_APP_DEFAULT_PROTO, name_part);
	if (strlen(rebuild) + 1 > sz)
		return;
	snprintf(url, sz, "%s", rebuild);
}

/* bash build_stream_url_with_name：统一替换最后一级为本设备推流名 */
static bool url_with_stream_name(const char *url, const char *name,
				 char *out, size_t sz)
{
	char base[512];
	const char *scheme;
	size_t l;
	int slashes;

	if (!url || !url[0] || !name || !name[0])
		return false;
	snprintf(base, sizeof(base), "%s", url);
	l = strlen(base);
	while (l > 0 && base[l - 1] == '/')
		base[--l] = '\0';
	if (!base[0])
		return false;

	scheme = strstr(base, "://");
	/* ROUND2 精修（与 feature_media.c build_stream_url_with_name 对齐）：
	 * 仅当 scheme 存在 且 path 段数 >=2 且 第一(path)段 == "live" 时才
	 * 执行"替换最后一级"；其他情况一律 append/<name>，然后交给
	 * ensure_srs_app_default_inplace 统一插 /live 段。避免"伪两级"
	 *（段数=2 但首段非 live）漏归一化导致 SRS 2051 StreamNameEmpty。*/
	if (scheme) {
		const char *path_start = strchr(scheme + 3, '/');
		if (path_start && path_start[1]) {
			const char *q;
			const char *slash2 = strchr(path_start + 1, '/');
			size_t s1len;
			char   seg1[128];
			int    segs = 1;

			if (slash2)
				s1len = (size_t)(slash2 - (path_start + 1));
			else
				s1len = strlen(path_start + 1);
			if (s1len >= sizeof(seg1)) s1len = sizeof(seg1) - 1;
			memcpy(seg1, path_start + 1, s1len);
			seg1[s1len] = '\0';
			for (q = path_start + 1; *q; ) {
				q = strchr(q, '/');
				if (!q) break;
				q++;
				while (*q == '/') q++;
				if (*q) segs++;
			}
			if (segs >= 2 && strcmp(seg1, SRS_APP_DEFAULT_PROTO) == 0) {
				char *cut = strrchr(base, '/');

				if (cut) {
					snprintf(out, sz, "%.*s/%s",
						 (int)(cut - base), base, name);
					ensure_srs_app_default_inplace(out, sz);
					return true;
				}
			}
			(void)slashes;
		}
	}
	/* 无 scheme 或 scheme 但段数不够/首段非 live：直接 append 后归一化 */
	snprintf(out, sz, "%s/%s", base, name);
	ensure_srs_app_default_inplace(out, sz);
	return true;
}

bool proto_stream_url_normalize(const char *requested, const char *fallback_url,
				const char *device_id, char *out, size_t sz)
{
	char name[96];

	if (!device_id || !device_id[0])
		return false;
	/* SRS_STREAM_PREFIX 未纳入 C 配置，按 bash 默认 stream_<DEVICE_ID> */
	snprintf(name, sizeof(name), "stream_%s", device_id);
	if (requested && requested[0])
		return url_with_stream_name(requested, name, out, sz);
	if (fallback_url && fallback_url[0])
		return url_with_stream_name(fallback_url, name, out, sz);
	return false;
}

/* ------------------------------------------------------------------ */
/* payload 组装                                                         */
/* ------------------------------------------------------------------ */

void proto_data_put_str(sb_t *b, const char *key, const char *val)
{
	if (b->len > 0 && b->s && b->s[b->len - 1] != '{')
		sb_putc(b, ',');
	sb_putc(b, '"');
	sb_puts(b, key);
	sb_puts(b, "\":");
	sb_json_str(b, val ? val : "");
}

void proto_data_put_int(sb_t *b, const char *key, long long v)
{
	if (b->len > 0 && b->s && b->s[b->len - 1] != '{')
		sb_putc(b, ',');
	sb_fmt(b, "\"%s\":%lld", key, v);
}

bool proto_payload_wrap(sb_t *b, long long msg_id, const char *msg,
			const sb_t *data)
{
	sb_init(b);
	sb_fmt(b, "{\"msgId\":%lld,\"msg\":", msg_id);
	sb_json_str(b, msg ? msg : "");
	sb_puts(b, ",\"data\":{");
	if (data && data->ok && data->len > 0)
		sb_puts(b, data->s);
	sb_puts(b, "}}");
	return b->ok;
}

bool proto_register_payload_build(sb_t *b, long long msg_id,
				  const char *version)
{
	sb_t d;

	sb_init(&d);
	proto_data_put_str(&d, "version", version ? version : "unknown");
	proto_payload_wrap(b, msg_id, "register", &d);
	sb_free(&d);
	return b->ok;
}

bool proto_heartbeat_payload_build(sb_t *b, long long msg_id,
				   const char *version, bool is_idle,
				   bool is_recording, bool is_publishing,
				   bool is_charging, long signal, long battery,
				   long voltage_mv)
{
	sb_t d;

	sb_init(&d);
	/* 字段名与顺序逐字对齐 protocol.sh protocol_build_heartbeat_payload，
	 * 布尔与数字均不加引号 */
	sb_puts(&d, "\"is_idle\":");
	sb_puts(&d, is_idle ? "true" : "false");
	sb_fmt(&d, ",\"is_recording\":%s,\"is_publishing\":%s,\"is_charging\":%s,\"version\":",
	       is_recording ? "true" : "false",
	       is_publishing ? "true" : "false",
	       is_charging ? "true" : "false");
	sb_json_str(&d, version ? version : "unknown");
	sb_fmt(&d, ",\"signal\":%ld,\"battery\":%ld,\"voltage_mv\":%ld",
	       signal, battery, voltage_mv);
	proto_payload_wrap(b, msg_id, "heartbeat", &d);
	sb_free(&d);
	return b->ok;
}

/* ------------------------------------------------------------------ */
/* ACK 组装（bash protocol_build_command_result 对齐）                   */
/* ------------------------------------------------------------------ */

/* extra_json（feature 追加字段）优先覆盖默认值 */
static const jv_t *ex_get(const jv_t *ex, const char *key)
{
	return ex ? jv_path(ex, key) : NULL;
}

static void ack_str(sb_t *d, const jv_t *ex, const char *key, const char *val)
{
	const jv_t *v = ex_get(ex, key);

	if (v && jv_str(v))
		val = jv_str(v);
	proto_data_put_str(d, key, val);
}

static void ack_int(sb_t *d, const jv_t *ex, const char *key, long long val)
{
	const jv_t *v = ex_get(ex, key);

	if (v)
		val = jv_int(v, val);
	proto_data_put_int(d, key, val);
}

/* bash RESULT_TASK_ID="${task_id:-$(state_get_current_task_id)}" */
static void ack_task_id(sb_t *d, const jv_t *ex, const cmd_t *c)
{
	char cur[64];
	const char *tid = c->task_id;

	if ((!tid || !tid[0]) &&
	    state_get_str("current_task_id", cur, sizeof(cur)) && cur[0])
		tid = cur;
	ack_str(d, ex, "taskId", tid);
}

void proto_ack_build(const cmd_t *c, const feat_result_t *r,
		     char *out_topic, size_t tsz, char *out_payload,
		     size_t psz)
{
	proto_cmd_t cc = proto_command_map(c);
	jv_t *ex = NULL;
	sb_t d, b;
	char wrap[600];
	const char *status;
	long long code;

	out_topic[0] = '\0';
	out_payload[0] = '\0';
	if (!c || !r)
		return;
	/* bash REPLY_REQUIRED=false：task_prepare_desk_voice 不回包 */
	if (cc == ENCM_CMD_NONE || cc == ENCM_CMD_TASK_PREPARE_DESK_VOICE)
		return;

	if (r->extra_json[0]) {
		snprintf(wrap, sizeof(wrap), "{%s}", r->extra_json);
		ex = jv_parse(wrap);
	}
	status = r->status;
	code = r->code;

	sb_init(&d);
	/* bash 所有 ACK data 首字段为 replyTo（--argjson replyTo $PROTO_MSG_ID，
	 * 数字）；仅 task_record_stop 的 lastSegmentUploaded 无 replyTo */
	if (cc != ENCM_CMD_TASK_RECORD_STOP)
		proto_data_put_int(&d, "replyTo", c->msg_id);
	switch (cc) {
	case ENCM_CMD_RECORD_START:
		proto_topic_build(out_topic, tsz, "encoder", c->device_id,
				  c->sender, c->sender_sub, "record",
				  "start_record_ack");
		ack_str(&d, ex, "recordId", c->record_id);
		ack_str(&d, ex, "status", status);
		proto_payload_wrap(&b, c->msg_id, "startRecordAck", &d);
		break;
	case ENCM_CMD_RECORD_STOP:
		proto_topic_build(out_topic, tsz, "encoder", c->device_id,
				  c->sender, c->sender_sub, "record",
				  "stop_record_ack");
		ack_str(&d, ex, "recordId", c->record_id);
		ack_str(&d, ex, "lastFile", r->last_file);
		ack_str(&d, ex, "status", status);
		proto_payload_wrap(&b, c->msg_id, "stopRecordAck", &d);
		break;
	case ENCM_CMD_CAPTURE_TAKE:
		proto_topic_build(out_topic, tsz, "encoder", c->device_id,
				  c->sender, c->sender_sub, "capture",
				  "capture_ack");
		ack_str(&d, ex, "captureId", c->capture_id);
		if (ex_get(ex, "fileName") || c->capture_id[0]) {
			char fn[300];

			snprintf(fn, sizeof(fn), "%s.jpg", c->capture_id);
			ack_str(&d, ex, "fileName", fn);
		} else {
			ack_str(&d, ex, "fileName", "");
		}
		ack_str(&d, ex, "fileUrl", "");
		ack_str(&d, ex, "status", status);
		proto_payload_wrap(&b, c->msg_id, "captureAck", &d);
		break;
	case ENCM_CMD_STREAM_START:
		proto_topic_build(out_topic, tsz, "encoder", c->device_id,
				  c->sender, c->sender_sub, "stream",
				  "start_stream_ack");
		ack_str(&d, ex, "streamUrl", c->stream_url);
		ack_str(&d, ex, "status", status);
		proto_payload_wrap(&b, c->msg_id, "startStreamAck", &d);
		break;
	case ENCM_CMD_STREAM_STOP:
		proto_topic_build(out_topic, tsz, "encoder", c->device_id,
				  c->sender, c->sender_sub, "stream",
				  "stop_stream_ack");
		ack_str(&d, ex, "status", status);
		proto_payload_wrap(&b, c->msg_id, "stopStreamAck", &d);
		break;
	case ENCM_CMD_TASK_STREAM_START:
		proto_topic_build(out_topic, tsz, "encoder", c->device_id,
				  c->sender, c->sender_sub, "task",
				  "start_stream_ack");
		ack_task_id(&d, ex, c);
		ack_int(&d, ex, "code", code);
		proto_payload_wrap(&b, c->msg_id, "startStreamAck", &d);
		break;
	case ENCM_CMD_TASK_RECORD_START:
		proto_topic_build(out_topic, tsz, "encoder", c->device_id,
				  c->sender, c->sender_sub, "task",
				  "start_record_ack");
		ack_task_id(&d, ex, c);
		ack_str(&d, ex, "recordId", c->record_id);
		ack_int(&d, ex, "code", code);
		proto_payload_wrap(&b, c->msg_id, "startRecordAck", &d);
		break;
	case ENCM_CMD_TASK_RECORD_STOP:
		/* task 录像停止成功回包为 lastSegmentUploaded（无 replyTo），
		 * fileSize/segmentNo 必须是数字 */
		proto_topic_build(out_topic, tsz, "encoder", c->device_id,
				  c->sender, c->sender_sub, "task",
				  "last_segment_uploaded");
		ack_str(&d, ex, "recordId", c->record_id);
		ack_int(&d, ex, "fileSize", r->last_size);
		ack_str(&d, ex, "fileUrl", "");
		ack_int(&d, ex, "segmentNo", 0);
		proto_payload_wrap(&b, c->msg_id, "lastSegmentUploaded", &d);
		break;
	case ENCM_CMD_TASK_RESET:
		proto_topic_build(out_topic, tsz, "encoder", c->device_id,
				  c->sender, c->sender_sub, "task",
				  "reset_encoder_ack");
		ack_task_id(&d, ex, c);
		ack_int(&d, ex, "code", code);
		proto_payload_wrap(&b, c->msg_id, "resetEncoderAck", &d);
		break;
	default:
		sb_free(&d);
		jv_free(ex);
		return;
	}

	if (b.ok && b.len < psz)
		memcpy(out_payload, b.s, b.len + 1);
	else
		snprintf(out_payload, psz, "%s", b.s ? b.s : "");
	sb_free(&d);
	sb_free(&b);
	jv_free(ex);
}

/* ------------------------------------------------------------------ */
/* msgid 取号（bash common.sh next_msgid 对齐）                          */
/* ------------------------------------------------------------------ */

long long msgid_next(void)
{
	static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
	char path[512], buf[32];
	long long n = 1000;
	long v;

	/* 路径/起始值/步进照 bash：<state_dir>/msgid，缺失按 1000，先加后写 */
	snprintf(path, sizeof(path), "%s/msgid", g_app.cfg.state_dir);
	pthread_mutex_lock(&mu);
	if (read_int_file(path, &v) && v > 0)
		n = v;
	n += 1;
	snprintf(buf, sizeof(buf), "%lld", n);
	write_str_file(path, buf);   /* 原子 tmp+rename */
	pthread_mutex_unlock(&mu);
	return n;
}
