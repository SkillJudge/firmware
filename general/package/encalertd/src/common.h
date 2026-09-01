/*
 * common.h — encalertd 公共类型与接口声明
 *
 * 编码器报警守护进程（原 system_monitor bash 脚本的 C 重写版）。
 * 设计文档：encoder/EncoderAlertdDesign.md
 * 报警协议：encoder/EncoderAlertProtocol.md（payload schema 完全兼容）
 */
#ifndef ENCALLOCERTD_COMMON_H
#define ENCALLOCERTD_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* 配置                                                                */
/* ------------------------------------------------------------------ */

#define ENC_DEFAULT_CONF        "/etc/encalertd.conf"
#define ENC_DEFAULT_STATE_DIR   "/root/encoder/runtime/state"
#define ENC_ENCODER_CONFIG      "/root/encoder/config.sh"
#define ENC_ACTIONS_DIR_DEFAULT "/root/encoder/actions"
#define ENC_SPOOL_DIR_DEFAULT   "/var/lib/encalertd"
#define ENC_LOG_FILE_DEFAULT    "/tmp/encalertd.log"

typedef struct {
    /* 基础 */
    int         interval_sec;
    int         dedup_sec;
    char        spool_dir[256];
    char        log_file[256];
    int         log_verbose;

    /* 状态与动作 */
    char        state_dir[256];
    char        actions_dir[256];
    int         action_timeout_sec;

    /* 模块开关 */
    bool        enable_wifi;
    bool        enable_battery;
    bool        enable_sdcard;
    bool        enable_sysres;
    bool        enable_process;
    bool        enable_stream;
    bool        enable_hw;         /* 硬件自检 (hw_watch 9101/9102, hw.c) */

    /* MQTT —— 凭据不落明文默认值：优先环境变量 ENC_MQTT_PASS，
     * 其次 /root/encoder/config.sh 继承，conf 覆盖最高优先级。 */
    char        mqtt_host[128];
    int         mqtt_port;
    char        mqtt_user[64];
    char        mqtt_pass[64];
    int         mqtt_qos;

    /* 阈值 */
    int         low_batt_mv;
    int         low_batt_pct;
    int         low_batt_confirm;
    int         sd_warn_pct;
    int         temp_warn_mc;      /* 摄氏毫单位 milli-celsius */
    int         temp_err_mc;
    double      load_factor_warn;  /* loadavg / nproc */
    long        mem_avail_min_kb;
    int         charge_window_sec;
    int         charge_drop_pct;
    int         charge_drop_mv;

    /* 磁盘空间保留（record_purge 空间模式 + 4003 判定） */
    long        record_min_free_mb;   /* 录像盘最低空闲 MB，默认 5120(=5GB) */

    /* 进程监控（proc_down 6103 + 风暴升级 6104） */
    char        monitor_procs[256];   /* 清单 name[:pidfile[:cmdpat]]，逗号分隔 */
    int         storm_window_sec;     /* 重启风暴统计窗口，默认 900s */
    int         storm_max_restarts;   /* 窗口内拉起次数上限，默认 3 */
    char        crash_dir[256];       /* 崩溃快照持久目录（SD 卡） */

    /* WiFi 原生检测（wifi.c） */
    char        wifi_iface[16];    /* 默认 wlan0 */

    /* 硬件自检（hw.c det_hw_watch）：周期独立于主循环节奏，
     * 用户场景先密集(3600s=1h)后调稀(86400s=1天) */
    int         hw_interval_sec;
} enc_cfg_t;

int  cfg_load(enc_cfg_t *c, const char *path);
/* device_id 解析（env 分区权威，见 alert.c 注释）：
 * 读到合法 DEVICE_ID 后缓存；未初始化时告警 topic 回退 ethaddr/unknown。 */
const char *device_id_get(const enc_cfg_t *c);
/* 设备是否未初始化（env 无合法 DEVICE_ID = factoryinit 未完成），现场重探 */
bool device_id_uninitialized(void);
/* ethaddr（冒号剔除），未初始化期的告警投递回退身份 */
bool device_ethaddr_get(char *out, size_t sz);

/* ------------------------------------------------------------------ */
/* 日志 / 工具                                                          */
/* ------------------------------------------------------------------ */

/* 注意：不能叫 LOG_*——与 <syslog.h> 的 LOG_INFO/LOG_WARNING 宏冲突 */
enum { ENC_LOG_DEBUG = 0, ENC_LOG_INFO, ENC_LOG_WARN, ENC_LOG_ERROR };

void log_set_file(const char *path, int verbose);
void log_msg(int level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

long long   now_ms(void);
bool        read_int_file(const char *path, long *out);
bool        read_str_file(const char *path, char *buf, size_t sz);
/* 单行小写转义后的 JSON 字符串写入 dst（加引号），返回 dst */
char       *json_escape(char *dst, size_t dsz, const char *src);

/* ------------------------------------------------------------------ */
/* 极简 MQTT 客户端（同步、单次连接发布）                                */
/* ------------------------------------------------------------------ */

typedef enum {
    MQ_OK = 0,
    MQ_ERR_TCP,       /* 连不上主机                          */
    MQ_ERR_CONNACK,   /* 连上了但 broker 拒绝/握手异常        */
    MQ_ERR_PUBACK,    /* PUBLISH 后未在超时内收到 PUBACK      */
    MQ_ERR_INTERNAL,
} mq_result_t;

mq_result_t mqtt_publish_once(const enc_cfg_t *c, const char *topic,
			      const char *payload);

/* ------------------------------------------------------------------ */
/* 报警流水线：seq / spool / dedup / 补发                               */
/* ------------------------------------------------------------------ */

int  alert_init(const enc_cfg_t *c);
/*
 * level: "info" | "warn" | "error" | "fatal"
 * detail_json: 可为 NULL 或 "{}"；
 * 返回: 0=已发出  1=已落盘待发(spool)  2=去重跳过  <0=本地错误
 * dedup 文件名按 code 维度；发布失败也会计入去重窗，避免断网期间刷 spool。
 */
int  alert_raise(const enc_cfg_t *c, int code, const char *type,
		 const char *level, const char *desc,
		 const char *detail_json);
/* 把 spool 里所有滞留消息按 seq 升序补发；全发完才算成功。返回补发条数 */
int  alert_flush_pending(const enc_cfg_t *c);
/* 连续发布失败计数(供 1101 判定)，>0 返回 true 并把类型写进 err_kind */
bool alert_pub_failed_recently(const enc_cfg_t *c, int *out_count,
			       char *err_kind, size_t sz);

/* ------------------------------------------------------------------ */
/* 动作执行器                                                           */
/* ------------------------------------------------------------------ */

/*
 * 执行 ${actions_dir}/<name>.sh '<context_json>'。
 * out 收集脚本标准输出最后一行（可空），超时 SIGKILL 视为失败。
 * 返回 0 成功(exit 0)，非 0 失败。
 */
int  action_run(const enc_cfg_t *c, const char *name,
		const char *context_json, char *out, size_t osz);
/* 从脚本输出里抽取整数键值（如 "purgedFiles":12），找不到返回 fallback */
long action_out_num(const char *script_output, const char *key,
		    long fallback);

/* ------------------------------------------------------------------ */
/* 检测器                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int  code;                /* 告警码，如 4001 */
    char type[40];            /* alertType，如 "sdcard_missing" */
    char level[8];            /* warn/error/fatal/info */
    char desc_fmt[160];       /* 描述模板，%s = reason */
} alert_def_t;

typedef struct det_s det_t;

/* 检查返回 NULL 表示正常；否则填充 r 并返回 r（同一 det 固定用同一个 alert）*/
struct det_s {
    const char *name;
    int          every_sec;
    int          confirm_cnt;   /* 连续 N 次 fail 才真正告警          */
    int          recover_cnt;   /* 连续 N 次 ok 之后清零并允许再触发   */
    bool      (*enabled)(const enc_cfg_t *c);
    /* 返回 NULL=正常；否则返回 reason 并设置 active_alert */
    const char *(*check)(const enc_cfg_t *c, char *reason, size_t rsz);
    /* 确认故障时回调（低电关机、推流自愈等联动动作），可为 NULL */
    void      (*on_confirmed)(const enc_cfg_t *c, const struct det_s *d,
			      const char *reason);

    alert_def_t  alert_fail;    /* 异常告警                            */
    alert_def_t  alert_recover; /* 恢复事件，code==0 表示不发          */

    /* 运行时状态（scheduler 使用） */
    time_t       next_due;
    int          fail_streak;
    int          ok_streak;
    bool         failing;       /* 当前处于已确认故障态                 */

    det_t       *next;
};

det_t *detectors_registry(void);   /* 返回静态链表头                      */

#endif /* ENCALLOCERTD_COMMON_H */
