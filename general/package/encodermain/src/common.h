/*
 * common.h — encodermain 公共类型与接口声明
 *
 * 编码器主控进程（原 /root/encoder/encoder_main.sh bash 脚本群的 C 重写版）。
 * 设计文档：encoder/EncoderMainDesign.md（§3.2 模块分解 / §6 业务 / §7 encdb / §8 锁模型）
 * 线上协议：encoder/EncoderProtocal.md（topic/payload 逐字段兼容 bash 版）
 *
 * 实现模块：
 *   util.c      日志/文件/时钟/转义工具
 *   config.c    配置三级合并 + device_id 解析
 *   state.c     state 文件读写 + registerAck 运行时(runtime) 应用
 *   lock.c      Majestic 配置文件锁（阻塞等待 + 失活抢占）
 *   mini_json.c 极简 JSON 解析/构建（内嵌，无外部依赖）
 *   mqtt.c      常驻 MQTT 3.1.1 客户端（订阅/发布/keepalive/重连/outbox）
 *   encdb.c     单文件 JSONL 数据库（records 上传账本 + alarms）
 *   protocol.c  topic 拆解 + ACK/事件 payload 组装（bash protocol.sh 对齐）
 *   dispatch.c  命令队列 + L1 msgId / L2 taskid 幂等 + 命令 worker 线程
 *   feature_media.c 推流/录像/抓拍/复位（yaml-cli + HUP）
 *   upload.c    db 驱动上传线程（断网重传）+ purge
 *   voice.c     Majestic 语音 init/ready/desk
 *   http_client.c 极简 HTTP GET/POST（Majestic 本机 80 端口）
 *   battery.c   i2c-dev 电量/充电/低压关机兜底
 *   led.c       i2c-dev PCF8574 LED + 上传闪烁
 *   selftest.c  -t 自检
 */
#ifndef ENCODERMAIN_COMMON_H
#define ENCODERMAIN_COMMON_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define ENCM_VERSION "1.0.0"

/* ------------------------------------------------------------------ */
/* 默认路径（与 bash 版一致，conf 可覆盖）                              */
/* ------------------------------------------------------------------ */

#define ENCM_DEFAULT_CONF       "/etc/encodermain.conf"
#define ENCM_ENCODER_CONFIG     "/root/encoder/config.sh"
#define ENCM_WORK_DIR           "/root/encoder"
#define ENCM_STATE_DIR          "/root/encoder/runtime/state"
#define ENCM_RUNTIME_DIR        "/root/encoder/runtime"
#define ENCM_LOG_FILE           "/root/encoder/runtime/logs/encoder.log"
#define ENCM_RECORD_ROOT        "/mnt/mmcblk0p1"
#define ENCM_DB_FILE            "/root/encoder/runtime/enc.db"
#define ENCM_MAJESTIC_CONF      "/etc/majestic.yaml"
#define ENCM_MAJESTIC_INIT      "/etc/init.d/S95majestic"
#define ENCM_VOICE_PCM          "/root/resources/desk_8k.pcm"
#define ENCM_YAML_CLI           "yaml-cli"

/* ------------------------------------------------------------------ */
/* 配置                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    /* 基础 */
    char    device_id[64];
    char    version[32];        /* config.sh VERSION 继承 */
    int     log_verbose;
    char    log_file[256];
    char    conf_path[256];

    /* 路径（测试可整体重定向） */
    char    work_dir[256];
    char    state_dir[256];
    char    runtime_dir[256];
    char    record_root[256];
    char    db_file[256];
    char    outbox_dir[256];
    char    voice_pcm[256];
    char    majestic_conf[256];
    char    majestic_init[256];
    char    yaml_cli[64];

    /* MQTT —— 凭据不落明文默认值：优先环境变量 ENC_MQTT_PASS，
     * 其次 /root/encoder/config.sh 继承，conf 覆盖最高优先级。 */
    char    mqtt_host[128];
    int     mqtt_port;
    char    mqtt_user[64];
    char    mqtt_pass[64];
    int     mqtt_qos;           /* 默认 2，兼容 bash mosquitto_pub/sub -q 2 */

    /* 行为 */
    int     heartbeat_sec;          /* 默认 30 */
    int     majestic_lock_wait_sec; /* 默认 60（需求 4：阻塞等待） */
    int     command_queue_max;      /* 默认 32 */
    int     task_dedup_max;         /* 默认 256 */
    int     db_compact_records;     /* 默认 4096 */
    int     db_alarm_max;           /* 默认 512 */
    int     upload_retry_max;       /* 默认 5 */
    int     upload_retry_backoff_sec;   /* 默认 30 */
    bool    upload_rescan_on_reconnect; /* 默认 true（需求 6） */
    int     http_connect_timeout_sec;   /* 默认 3 */
    int     http_max_time_sec;          /* 默认 10 */
    int     record_verify_timeout_sec;  /* 默认 10 */
    int     segment_stable_sec;         /* 默认 20 */
} enc_cfg_t;

int         cfg_load(enc_cfg_t *c, const char *conf_path);
const char *device_id_get(const enc_cfg_t *c);  /* 带缓存三级解析 */

/* ------------------------------------------------------------------ */
/* 日志 / 工具（util.c）                                                */
/* ------------------------------------------------------------------ */

/* 注意：不能叫 LOG_*——与 <syslog.h> 的 LOG_INFO/LOG_WARNING 宏冲突 */
enum { ENCM_LOG_DEBUG = 0, ENCM_LOG_INFO, ENCM_LOG_WARN, ENCM_LOG_ERROR };

void log_set_file(const char *path, int verbose);
void log_msg(int level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

long long now_ms(void);
bool        read_int_file(const char *path, long *out);
bool        read_str_file(const char *path, char *buf, size_t sz);
bool        write_str_file(const char *path, const char *val); /* 原子 tmp+rename */
int         dir_ensure(const char *path);      /* 递归建目录，0 成功 */
void        shell_quote(char *dst, size_t dsz, const char *src);
bool        pid_alive(long pid);
bool        proc_running(const char *name);    /* pidof 语义 */
/* 单行小写转义后的 JSON 字符串写入 dst（加引号），返回 dst */
char       *json_escape(char *dst, size_t dsz, const char *src);
/* fork+exec 单命令（sh -c 语义），stdout 最后一行写入 out，超时 SIGKILL。
 * 返回 0 = exit code 0 */
int         run_cmd(const char *cmd, int timeout_sec, char *out, size_t osz);

/* ------------------------------------------------------------------ */
/* state 文件 / 运行时（state.c）                                       */
/* ------------------------------------------------------------------ */

/* registerAck 应用后的运行时配置（bash runtime.sh 语义） */
typedef struct {
    char    ftp_host[128];
    char    ftp_user[64];
    char    ftp_pass[64];
    char    ftp_path[128];      /* 远端根目录，默认 / */
    char    srs_url[256];       /* registerAck data.srs */
    long long time_offset_ms;   /* 服务器时间 - 本地时间 */
    bool    applied;
} enc_runtime_t;

int  state_init(const enc_cfg_t *c);
void state_set_str(const char *key, const char *val);
void state_set_int(const char *key, long v);
bool state_get_str(const char *key, char *buf, size_t sz);
bool state_get_int(const char *key, long *out);

/* 启动时从 runtime 文件恢复（断电重启后 FTP 不丢） */
int  rt_load(const enc_cfg_t *c, enc_runtime_t *rt);
/* 应用 registerAck data JSON：写 runtime 文件 + 计算时间偏移（不直接改系统时钟） */
int  rt_apply_register_ack(const enc_cfg_t *c, enc_runtime_t *rt,
			   const char *data_json);

/* ------------------------------------------------------------------ */
/* Majestic 配置锁（lock.c）—— 需求 4                                  */
/* ------------------------------------------------------------------ */

/* mkdir 目录锁 state/majestic_config.lock，锁内写 pid。
 * 阻塞等待至多 wait_sec；持有者 pid 失活则抢占清理。
 * 返回 0 = 获锁；-1 = 超时/失败 */
int  majestic_lock_acquire(int wait_sec);
void majestic_lock_release(void);

/* ------------------------------------------------------------------ */
/* 极简 JSON（mini_json.c）                                             */
/* ------------------------------------------------------------------ */

typedef struct jv jv_t;
jv_t       *jv_parse(const char *s);       /* 失败返回 NULL */
void        jv_free(jv_t *v);
const jv_t *jv_path(const jv_t *obj, const char *path); /* "data.taskId" */
const char *jv_str(const jv_t *v);         /* 非 string 返回 NULL */
long long   jv_int(const jv_t *v, long long def);
bool        jv_bool(const jv_t *v, bool def);
bool        jv_is_num(const jv_t *v);

/* 字符串 builder（JSON 组装） */
typedef struct { char *s; size_t len, cap; bool ok; } sb_t;
void sb_init(sb_t *b);
void sb_free(sb_t *b);
void sb_putc(sb_t *b, char ch);
void sb_puts(sb_t *b, const char *s);
void sb_fmt(sb_t *b, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
/* 带引号转义的 JSON 字符串字面量 */
void sb_json_str(sb_t *b, const char *s);

/* ------------------------------------------------------------------ */
/* 常驻 MQTT 客户端（mqtt.c）                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    MQEV_CONNECTED = 0,     /* 首连或重连成功（已重新订阅） */
    MQEV_DISCONNECTED,
} mq_event_t;

typedef struct mq_client mq_client_t;
typedef void (*mq_msg_cb)(void *ud, const char *topic, const char *payload);
typedef void (*mq_event_cb)(void *ud, mq_event_t ev);

/* 启动常驻客户端线程：CONNECT(clean session) → SUBSCRIBE +/+/encoder/<id>/#。
 * 断线自动重连（指数退避 1s~30s）。失败返回 NULL。 */
mq_client_t *mq_start(const enc_cfg_t *c, mq_msg_cb msg_cb,
		      mq_event_cb ev_cb, void *ud);
/* 线程安全发布。persist=true：先落 outbox，PUBACK/PUBCOMP 后删除，
 * 重连后未确认消息自动重发（必达语义）。返回 0 = 已确认或已入队 */
int  mq_publish(mq_client_t *m, const char *topic, const char *payload,
		bool persist);
void mq_stop(mq_client_t *m);

/* ------------------------------------------------------------------ */
/* encdb：单文件 JSONL 数据库（encdb.c）—— 需求 6                       */
/* ------------------------------------------------------------------ */

typedef enum {
    DB_PENDING = 0, DB_UPLOADED, DB_FAILED, DB_FAILED_FINAL, DB_LOST,
} db_state_t;
const char *db_state_name(db_state_t s);

typedef struct {
    char     file[512];
    char     kind[8];           /* record / capture */
    char     record_id[64];
    char     task_id[64];
    int      seg;
    long long size;
    long long mtime;            /* 秒 */
    db_state_t state;
    int      retry;
    long long next_retry_ts;    /* 秒，退避重传时间点 */
    char     err[64];
    long long ts;               /* 毫秒，最后变更时间 */
} db_rec_t;

int  encdb_open(const enc_cfg_t *c);            /* 加载 + 尾行截断恢复 */
int  encdb_rec_add(const db_rec_t *r);          /* file 为主键，存在则覆盖 */
int  encdb_rec_state(const char *file, db_state_t st,
		     const char *err, int retry, long long next_retry_ts);
bool encdb_rec_get(const char *file, db_rec_t *out);
/* 遍历：cursor 从 0 开始递增；无更多返回 0；找到返回 1 并填充 out */
int  encdb_rec_next(int *cursor, db_rec_t *out);
int  encdb_rec_del(const char *file);           /* compaction 时物理清除 */
int  encdb_rec_count(db_state_t st);            /* st<0 = 全部 */
int  encdb_alarm(int code, const char *type, const char *level,
		 const char *desc, const char *detail_json);
/* 导出 -d：table = records | alarms，CSV 到 stdout */
int  encdb_dump(FILE *out, const char *table);
int  encdb_stats(char *buf, size_t sz);
int  encdb_compact(void);                       /* tmp+rename 原子重写 */

/* ------------------------------------------------------------------ */
/* 命令解析与派发（protocol.c / dispatch.c）                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char      sender[64];       /* topic 第 1 段 */
    char      sender_sub[32];   /* 第 2 段 */
    char      device_id[64];    /* 第 4 段 */
    char      flow[32];         /* 第 5 段：record/stream/capture/task */
    char      action[48];       /* 第 6 段 */
    char      msg[48];          /* payload.msg */
    long long msg_id;
    char      raw_payload[2048];

    /* data 常用字段（解析后填充，可能为空） */
    char      task_id[64];
    char      record_id[64];
    char      capture_id[64];
    char      stream_url[256];
    long long duration;
    bool      has_duration;
    long long code;             /* data.code（若带） */
} cmd_t;

/* 业务执行结果（dispatch 据此组装 ACK） */
typedef struct {
    int       code;             /* 0 = 成功路径 */
    char      status[16];       /* success/streaming/recording/idle/duplicate/fail */
    char      last_file[256];
    long long last_size;
    char      extra_json[512];  /* 追加 data 字段（不含大括号），可空 */
} feat_result_t;

/* topic/payload 解析：成功返回 0（msgId 缺失也成功，msg_id=0） */
int  proto_parse(const char *topic, const char *payload, cmd_t *c);
/* 业务入口：cmd → feat_result（feature_media.c 实现） */
int  feat_execute(const cmd_t *c, feat_result_t *r);
/* 启动时恢复 Majestic 默认 profile（records 关/outgoing 开/视频档复位） */
int  feat_restore_startup_media(const enc_cfg_t *c);
/* 主循环周期调用：duration 到期触发 reset；返回 1 表示已执行 reset */
int  feat_duration_check(void);

/* dispatch：mq 收包入口（register_ack/heartbeat_ack 快速通道，其余入队） */
void dispatch_on_message(const char *topic, const char *payload);
int  dispatch_init(void);               /* 启动 worker 线程 */
void dispatch_shutdown(void);
/* register 流程：发布 register（自增 msgId），等 registerAck(replyTo 匹配) */
long long msgid_next(void);             /* 原子取号，state/msgid 持久化 */
int  dispatch_send_register(const enc_cfg_t *c);
/* 等待 replyTo==msgid 的 registerAck；超时返回 false，命中时回 data JSON */
bool dispatch_wait_register_ack(long long msgid, int timeout_ms,
				char *out_data, size_t sz);
/* 心跳：组装并发布（字段照 app_service.sh heartbeat） */
int  dispatch_send_heartbeat(const enc_cfg_t *c);

/* ------------------------------------------------------------------ */
/* 上传 / purge（upload.c）—— 需求 6                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const enc_cfg_t  *cfg;
    enc_runtime_t    *rt;               /* 受 registerAck 更新 */
    mq_client_t      *mq;
    pthread_mutex_t  *rt_mutex;         /* 保护 rt */
} upload_ctx_t;

/* 单文件上传：remote_rel 形如 "upload/<device>/<record_id>/<name>.mp4"。
 * 返回 0 成功 */
int  upload_file(const enc_cfg_t *c, const enc_runtime_t *rt,
		 const char *local, const char *remote_rel);
/* 上传线程主函数（录像期扫描 + 事件驱动重传 + 补发 segment_uploaded） */
void *upload_thread(void *arg);
/* 唤醒上传线程（MQTT 重连成功 / record_stop 后调用） */
void upload_kick(void);
/* record_stop 同步上传最终分片：成功返回 0 并填充远端文件名 */
int  upload_sync_final(const enc_cfg_t *c, const enc_runtime_t *rt,
		       const char *local, const char *record_id,
		       const char *task_id, long long start_ts,
		       char *out_name, size_t osz);
/* --purge：只删 db 中 state=uploaded 且 mtime 超龄的文件（需求 6 对齐
 * encalertd record_purge.sh）。输出单行 JSON：
 * {"purgedFiles":N,"freedBytes":B,"skippedFiles":M} */
int  purge_run(const enc_cfg_t *c, int older_than_hours,
	       char *out_json, size_t sz);

/* segment_uploaded 事件发布（字段照 feature_engine.sh） */
int  upload_publish_segment(mq_client_t *mq, const enc_cfg_t *c,
			    const char *flow, const char *task_id,
			    const char *record_id, const char *file_name,
			    long long file_size, int segment_no);

/* ------------------------------------------------------------------ */
/* 语音（voice.c）—— 需求 2                                             */
/* ------------------------------------------------------------------ */

int  voice_init(const enc_cfg_t *c);    /* 锁内 yaml+HUP，重试 3；0 成功 */
int  voice_ready(const enc_cfg_t *c);   /* GET /play_audio 探测 200 */
void voice_desk_async(const enc_cfg_t *c, const char *task_id);
void voice_stop_current(void);
bool voice_playing(void);

/* ------------------------------------------------------------------ */
/* HTTP（http_client.c）                                                */
/* ------------------------------------------------------------------ */

int http_get(const char *host, int port, const char *path,
	     int timeout_sec, char *out, size_t osz, int *status_code);
int http_post_file(const char *host, int port, const char *path,
		   const char *filepath, int timeout_sec, int *status_code);

/* ------------------------------------------------------------------ */
/* 电池 / LED（battery.c / led.c）                                      */
/* ------------------------------------------------------------------ */

int  battery_refresh(const enc_cfg_t *c);   /* 刷新 state 三键，0 成功 */
bool battery_low_shutdown_check(const enc_cfg_t *c); /* 低压关机兜底 */
int  led_init(const enc_cfg_t *c);
void led_upload_token_set(const char *tag, bool on); /* token 目录驱动闪烁 */
void led_shutdown(void);

/* ------------------------------------------------------------------ */
/* 自检（selftest.c）                                                   */
/* ------------------------------------------------------------------ */

int selftest_run(const enc_cfg_t *c);       /* 返回 0 = 全部通过 */

/* ------------------------------------------------------------------ */
/* 全局上下文                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    enc_cfg_t        cfg;
    enc_runtime_t    rt;
    mq_client_t     *mq;
    pthread_mutex_t  rt_mutex;      /* 保护 rt */
    volatile bool    stopping;      /* SIGTERM 置位 */
} app_t;

extern app_t g_app;

#endif /* ENCODERMAIN_COMMON_H */
