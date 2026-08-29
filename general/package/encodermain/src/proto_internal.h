/*
 * proto_internal.h — protocol.c / dispatch.c 内部共享原型
 *
 * 仅这两个模块使用，不对外发布（对外契约一律走 common.h）。
 * 所有字段名与 topic/payload 布局逐字对齐 bash 版：
 *   firmware/general/overlay/root/encoder/protocol.sh
 *   firmware/general/overlay/root/encoder/app_service.sh
 */
#ifndef ENCM_PROTO_INTERNAL_H
#define ENCM_PROTO_INTERNAL_H

#include "common.h"

/* topic 六段拆解后的段缓冲（sender/sender_sub/receiver/receiver_sub/flow/action） */
#define PROTO_SEG_NUM 6
#define PROTO_SEG_SZ  64

/* 命令映射（bash protocol_parse_command 的 case 表，含 msg 三元组校验） */
typedef enum {
	ENCM_CMD_NONE = 0,
	ENCM_CMD_RECORD_START,          /* record/start_record/startRecord */
	ENCM_CMD_RECORD_STOP,           /* record/stop_record/stopRecord */
	ENCM_CMD_CAPTURE_TAKE,          /* capture/capture/capture */
	ENCM_CMD_STREAM_START,          /* stream/start_stream/startStream */
	ENCM_CMD_STREAM_STOP,           /* stream/stop_stream/stopStream */
	ENCM_CMD_TASK_PREPARE_DESK_VOICE, /* task/prepare_desk_recognition_voice/... */
	ENCM_CMD_TASK_STREAM_START,     /* task/start_stream/startStream */
	ENCM_CMD_TASK_RECORD_START,     /* task/start_record/startRecord */
	ENCM_CMD_TASK_RECORD_STOP,      /* task/stop_record/stopRecord */
	ENCM_CMD_TASK_RESET,            /* task/reset_encoder/resetEncoder */
} proto_cmd_t;

/* topic 按 '/' 拆 6 段（空段/段数不为 6 返回 -1）；成功 0 */
int proto_topic_split(const char *topic, char seg[][PROTO_SEG_SZ]);

/* 六段 topic 组装：sender/sender_sub/receiver/receiver_sub/flow/action */
void proto_topic_build(char *out, size_t sz, const char *sender,
		       const char *sender_sub, const char *receiver,
		       const char *receiver_sub, const char *flow,
		       const char *action);

/* flow/action/msg 三元组 → 内部命令；不匹配返回 ENCM_CMD_NONE */
proto_cmd_t proto_command_map(const cmd_t *c);
const char *proto_cmd_name(proto_cmd_t cc);

/* key 清洗：[^A-Za-z0-9._-] → '_'（bash service_command_cache_key 同规则） */
void proto_key_sanitize(char *buf, size_t sz, const char *src);

/* 推流 URL 规范化（bash build_stream_url/build_stream_url_with_name 对齐）：
 * 非空 URL 去尾 '/' 后，若路径 ≥2 级则替换最后一级为 stream_<device_id>，
 * 否则追加；requested 为空时回退 fallback_url（rt.srs_url），再空则失败。 */
bool proto_stream_url_normalize(const char *requested, const char *fallback_url,
				const char *device_id, char *out, size_t sz);

/* data 对象字段追加（自动管理逗号；字符串带引号转义 / 数字原样） */
void proto_data_put_str(sb_t *b, const char *key, const char *val);
void proto_data_put_int(sb_t *b, const char *key, long long v);

/* 通用 payload 包装（bash protocol_build_payload）：
 * {"msgId":N,"msg":"...","data":{...}}；data 为已组好的字段串（可空） */
bool proto_payload_wrap(sb_t *b, long long msg_id, const char *msg,
			const sb_t *data);

/* register payload（bash protocol_build_register_payload）：
 * {"msgId":N,"msg":"register","data":{"version":"..."}} */
bool proto_register_payload_build(sb_t *b, long long msg_id,
				  const char *version);

/* heartbeat payload（bash protocol_build_heartbeat_payload，字段顺序一致）：
 * data:{is_idle,is_recording,is_publishing,is_charging,version,signal,
 *       battery,voltage_mv}（布尔/数字均不加引号） */
bool proto_heartbeat_payload_build(sb_t *b, long long msg_id,
				   const char *version, bool is_idle,
				   bool is_recording, bool is_publishing,
				   bool is_charging, long signal, long battery,
				   long voltage_mv);

/* ACK / lastSegmentUploaded 组装（bash protocol_build_command_result 对齐）。
 * 除 lastSegmentUploaded 外 data 首字段均为数字 replyTo（= 本次 msgId）；
 * r->extra_json 中的同名键覆盖默认取值（fileUrl/segmentNo/streamUrl 等由
 * feature 填入）。无需应答的命令（task_prepare_desk_voice）out_topic 为空串。 */
void proto_ack_build(const cmd_t *c, const feat_result_t *r,
		     char *out_topic, size_t tsz,
		     char *out_payload, size_t psz);

#endif /* ENCM_PROTO_INTERNAL_H */
