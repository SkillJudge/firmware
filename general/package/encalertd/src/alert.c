/*
 * alert.c — 报警事件流水线
 *
 *   raise → dedup 检查 → seq++ (fsync) → spool 落盘 (fsync) → 尝试发布
 *                                              │
 *   重连后 main 循环调用 flush_pending() ←─────┘ 失败则滞留，PUBACK 后删除
 *
 * 与 sh 版差异：
 *   - msgId 不再借用业务进程的 runtime/state/msgid（消除读改写竞态），
 *     使用自身 spooldir/msgid 计数器，初始 30000。
 *   - 同时维护单调递增 audit seq 作为 spool 文件名（审计报警连续性）。
 *   - 补发策略：原样重发完整 payload（payload 内已含原始 ts；
 *     接收端可用到达时间差识别补发，协议侧无需新增字段）。
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

#define ALERT_TOPIC_FMT "encoder/%s/ctrlsrv/0/alert/alert_event"
#define MSGID_BASE      30000UL
#define SPOOL_MAX_FILES 512

static struct {
    char     dir_spool[320];    /* <spool_dir>/spool        */
    char     dir_dedup[320];    /* <spool_dir>/dedup        */
    char     path_seq[320];
    char     path_msgid[320];
    uint32_t seq;
    int      pub_fail_streak;
    char     last_err_kind[20]; /* tcp | connack | puback   */
} g_al;

/* ---------------- device_id 解析（env 分区权威） ----------------
 *
 * DEVICE_ID 由 factoryinit 用硬件 id 向中心服务器换取后写入 env 分区
 * （固件升级只刷 rootfs，不刷 env）。解析规则：
 *   1. fw_printenv -n DEVICE_ID 读到合法值 = 唯一权威，缓存；
 *   2. 读不到 = 设备未正确初始化（device_id_uninitialized() == true，
 *      由 deviceid 检测器发 9001 告警）。此时告警 topic 设备段回退
 *      ethaddr（去冒号），保证未初始化设备的告警仍可按机投递；
 *      ethaddr 也读不到时用 "unknown"。
 *   3. 回退值不缓存：factoryinit 写入 DEVICE_ID 后自动切回权威值，
 *      无需重启进程。
 * 出厂 hostname 三台相同，与设备身份无关，不再作兜底；state/device_id
 * 为 bash 时代遗留缓存，同样不再信任（2026-08-31 口径收紧）。
 */

/* env DEVICE_ID 探测：读到且合法（对齐 encodermain is_valid_device_id）
 * 返回 true。未定义时 fw_printenv -n 输出空 + 退出码 1（真机实测）。 */
static bool env_devid_probe(char *out, size_t sz)
{
	FILE *f = popen("fw_printenv -n DEVICE_ID 2>/dev/null", "r");
	size_t n = 0;

	if (!f)
		return false;
	n = fread(out, 1, sz - 1, f);
	pclose(f);
	while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r'
	    || out[n-1] == ' ' || out[n-1] == '\t'))
		out[--n] = '\0';
	out[n] = '\0';
	if (!n)
		return false;
	for (size_t i = 0; i < n; i++) {
		char ch = out[i];

		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		      (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
		      ch == '-'))
			return false;
	}
	return true;
}

/* ethaddr（冒号剔除），仅作未初始化期的告警投递回退身份 */
bool device_ethaddr_get(char *out, size_t sz)
{
	FILE *f = popen("fw_printenv -n ethaddr 2>/dev/null", "r");
	size_t n = 0;

	if (!f)
		return false;
	n = fread(out, 1, sz - 1, f);
	pclose(f);
	while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r'))
		out[--n] = '\0';
	for (size_t i = 0; i < n; i++)
		if (out[i] == ':')
			memmove(&out[i], &out[i + 1], n - i), n--;
	out[n] = '\0';
	return n > 0;
}

const char *device_id_get(const enc_cfg_t *c)
{
	static char devid[64];
	static int  env_ok;

	(void)c;
	if (env_ok)
		return devid;
	if (env_devid_probe(devid, sizeof(devid))) {
		env_ok = 1;
		return devid;
	}
	/* 未初始化：回退 ethaddr / unknown（不缓存，等 DEVICE_ID 写入） */
	{
		static char fb[64];

		if (device_ethaddr_get(fb, sizeof(fb)))
			return fb;
	}
	return "unknown";
}

/* 设备是否未初始化（env 无合法 DEVICE_ID）。每次现场重探，调用方为
 * deviceid 检测器（30s 周期），无性能压力；不缓存失败结果。 */
bool device_id_uninitialized(void)
{
	char probe[64];

	return !env_devid_probe(probe, sizeof(probe));
}

/* ---------------- fs helpers ---------------- */

static void mkdir_p(const char *path)
{
	char tmp[400], *p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

static bool read_u32_file(const char *path, uint32_t *out)
{
	FILE *f = fopen(path, "r");

	*out = 0;
	if (!f)
		return false;
	if (fscanf(f, "%u", out) != 1)
		*out = 0;
	fclose(f);
	return true;
}

static void write_u32_fsync(const char *path, uint32_t v)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return;
	fprintf(f, "%u\n", v);
	fflush(f);
	fsync(fileno(f));
	fclose(f);
}

/* ---------------- init ---------------- */

int alert_init(const enc_cfg_t *c)
{
	snprintf(g_al.dir_spool, sizeof(g_al.dir_spool),
		 "%s/spool", c->spool_dir);
	snprintf(g_al.dir_dedup, sizeof(g_al.dir_dedup),
		 "%s/dedup", c->spool_dir);
	snprintf(g_al.path_seq, sizeof(g_al.path_seq),
		 "%s/seq", c->spool_dir);
	snprintf(g_al.path_msgid, sizeof(g_al.path_msgid),
		 "%s/msgid", c->spool_dir);

	mkdir_p(c->spool_dir);
	mkdir_p(g_al.dir_spool);
	mkdir_p(g_al.dir_dedup);

	read_u32_file(g_al.path_seq, &g_al.seq);
	log_msg(ENC_LOG_INFO, "alert pipeline ready: spool=%s seq=%u",
		g_al.dir_spool, g_al.seq);
	return 0;
}

/* ---------------- dedup ---------------- */

static bool dedup_active(int code, const enc_cfg_t *c)
{
	uint32_t last_ts, now = (uint32_t)time(NULL);
	char path[384];

	snprintf(path, sizeof(path), "%s/%d", g_al.dir_dedup, code);
	if (!read_u32_file(path, &last_ts))
		return false;
	if (last_ts == 0 || now < last_ts)
		return false; /* 时钟回拨视同过期 */
	return (now - last_ts) < (uint32_t)c->dedup_sec;
}

static void dedup_mark(int code)
{
	char path[384];

	snprintf(path, sizeof(path), "%s/%d", g_al.dir_dedup, code);
	write_u32_fsync(path, (uint32_t)time(NULL));
}

/* ---------------- payload 构建 ---------------- */

static void build_payload(char *out, size_t osz,
			  int code, const char *type, const char *level,
			  const char *desc, const char *detail_json)
{
	unsigned long msgid;
	char esc_desc[512];
	uint32_t mseq;

	read_u32_file(g_al.path_msgid, &mseq);
	msgid = (unsigned long)(mseq ? mseq : MSGID_BASE);
	msgid++;
	write_u32_fsync(g_al.path_msgid, (uint32_t)msgid);

	if (!detail_json || !detail_json[0])
		detail_json = "{}";

	json_escape(esc_desc, sizeof(esc_desc), desc);
	snprintf(out, osz,
		 "{\"msgId\":%lu,\"msg\":\"alert\",\"data\":"
		 "{\"alertType\":\"%s\",\"level\":\"%s\",\"ts\":%lld,"
		 "\"code\":%d,\"desc\":\"%s\",\"detail\":%s}}",
		 msgid, type, level, now_ms(), code, esc_desc, detail_json);
}

/* ---------------- publish / spool ---------------- */

static mq_result_t try_publish(const enc_cfg_t *c, const char *topic,
			       const char *payload)
{
	mq_result_t r = mqtt_publish_once(c, topic, payload);

	switch (r) {
	case MQ_OK:
		g_al.pub_fail_streak = 0;
		g_al.last_err_kind[0] = '\0';
		break;
	default:
		g_al.pub_fail_streak++;
		snprintf(g_al.last_err_kind, sizeof(g_al.last_err_kind),
			 "%s",
			 r == MQ_ERR_TCP     ? "tcp" :
			 r == MQ_ERR_CONNACK ? "connack" : "puback");
		log_msg(ENC_LOG_WARN, "publish failed: kind=%s streak=%d",
			g_al.last_err_kind, g_al.pub_fail_streak);
		break;
	}
	return r;
}

static long count_spool_files(void)
{
	DIR *d = opendir(g_al.dir_spool);
	struct dirent *e;
	long n = 0;
	size_t nl;

	if (!d)
		return 0;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		nl = strlen(e->d_name);
		if (nl < 4 ||
		    strcmp(e->d_name + nl - 4, ".msg") != 0)
			continue;
		n++;
	}
	closedir(d);
	return n;
}

/* 删最小 seq 的 .msg 文件；返回被删除的 seq（>0），失败 -1。
 * 注意：直接用 e->d_name 保存原文件名（不重新 "%u.msg" 拼），
 * 避免 "%08u.msg"（写侧补零）/ "7000001.msg"（预置无补零）
 * 格式不统一导致 unlink 找不到文件的静默失败漏洞。
 */
static int drop_oldest_spool_file(void)
{
	DIR *d = opendir(g_al.dir_spool);
	struct dirent *e;
	uint32_t oldest = UINT32_MAX;
	char pick[128] = {0};

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		uint32_t s;

		if (sscanf(e->d_name, "%u.msg", &s) == 1 &&
		    s != UINT32_MAX && s < oldest) {
			oldest = s;
			snprintf(pick, sizeof(pick), "%s", e->d_name);
		}
	}
	closedir(d);
	if (!pick[0] || oldest == UINT32_MAX)
		return -1;
	{
		char path[448];

		snprintf(path, sizeof(path), "%s/%s", g_al.dir_spool, pick);
		if (unlink(path) < 0)
			return -1;
	}
	return (int)oldest;
}

static int spool_append_and_try(const enc_cfg_t *c, const char *payload)
{
	char path[448];
	char topic[192];
	int fd;
	mq_result_t r;

	/* 写前预删：当前已达到或超过上限时，删除最旧文件为新写入预留 1 个槽位。
	 * 用 >= 而非 > 保证 count=SPOOL_MAX 时就先删 1 个（留位），
	 * 避免写后 count 短暂超标，也消除计数口径漂移时的漏删。
	 */
	while (count_spool_files() >= SPOOL_MAX_FILES) {
		if (drop_oldest_spool_file() < 0)
			break;
		log_msg(ENC_LOG_WARN,
			"spool overflow, dropped oldest (pre-write reserve slot)");
	}

	g_al.seq++;
	write_u32_fsync(g_al.path_seq, g_al.seq);

	snprintf(path, sizeof(path), "%s/%08u.msg", g_al.dir_spool, g_al.seq);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	{
		size_t len = strlen(payload);
		ssize_t w = write(fd, payload, len);

		fsync(fd);
		close(fd);
		if (w != (ssize_t)len)
			return -1;
	}

	/* 写后强制收敛：保证最终 .msg 文件数 ≤ SPOOL_MAX_FILES。
	 * 用 while 循环 + drop_oldest 直到达标，彻底消除计数口径漂移
	 * （readdir 偶发丢 entry / 非 .msg artifact / 同时进入 raise 多路径等）
	 * 导致断言侧 spool count > cap 的 off-by-one。
	 */
	{
		int guard = 0;

		while (count_spool_files() > SPOOL_MAX_FILES && guard++ < 16) {
			if (drop_oldest_spool_file() < 0)
				break;
			log_msg(ENC_LOG_WARN,
				"spool overflow, dropped oldest seq (post-write trim)");
		}
	}

	snprintf(topic, sizeof(topic), ALERT_TOPIC_FMT, device_id_get(c));
	r = try_publish(c, topic, payload);
	if (r == MQ_OK) {
		unlink(path);
		log_msg(ENC_LOG_INFO, "alert published: %08u.msg", g_al.seq);
	} else {
		log_msg(ENC_LOG_WARN, "alert queued for replay: %08u.msg (%s)",
			g_al.seq, g_al.last_err_kind);
	}
	return (r == MQ_OK) ? 0 : 1;
}

int alert_raise(const enc_cfg_t *c, int code, const char *type,
		const char *level, const char *desc, const char *detail_json)
{
	char payload[1600];
	bool is_info = !strcmp(level, "info");

	if (!is_info && dedup_active(code, c))
		return 2;

	build_payload(payload, sizeof(payload),
		      code, type, level, desc, detail_json);
	/* 无论是否发出，先进入去重窗：断网期间同一故障不刷 spool */
	dedup_mark(code);

	return spool_append_and_try(c, payload);
}

struct seq_entry {
	uint32_t seq;
	char     name[64];
};

static int cmp_seq_entry(const struct seq_entry *a,
			 const struct seq_entry *b)
{
	if (a->seq < b->seq) return -1;
	if (a->seq > b->seq) return 1;
	return 0;
}

int alert_flush_pending(const enc_cfg_t *c)
{
	DIR *d = opendir(g_al.dir_spool);
	struct dirent *e;
	char topic[192];
	int sent = 0;
	/* 按 seq 升序补发：先收集再 qsort，避免 readdir 返回顺序未定义 */
	struct seq_entry *arr = NULL;
	size_t n = 0, cap = 0;

	if (!d)
		return 0;
	snprintf(topic, sizeof(topic), ALERT_TOPIC_FMT, device_id_get(c));

	while ((e = readdir(d))) {
		uint32_t s = UINT32_MAX;

		if (sscanf(e->d_name, "%u.msg", &s) != 1 || s == UINT32_MAX)
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			struct seq_entry *tmp = realloc(arr,
							cap * sizeof(*tmp));

			if (!tmp)
				break;
			arr = tmp;
		}
		arr[n].seq = s;
		snprintf(arr[n].name, sizeof(arr[n].name), "%s", e->d_name);
		n++;
	}
	closedir(d);

	/* qsort 按 seq 升序 */
	if (arr && n > 1) {
		qsort(arr, n, sizeof(*arr),
		      (int (*)(const void *, const void *))cmp_seq_entry);
	}

	for (size_t i = 0; i < n; i++) {
		char path[448], body[1800] = "";
		size_t got = 0;
		FILE *f;

		snprintf(path, sizeof(path), "%s/%s",
			 g_al.dir_spool, arr[i].name);
		f = fopen(path, "r");
		if (!f)
			continue;
		got = fread(body, 1, sizeof(body) - 1, f);
		fclose(f);
		body[got] = '\0';

		if (try_publish(c, topic, body) == MQ_OK) {
			unlink(path);
			sent++;
			log_msg(ENC_LOG_INFO,
				"replayed spooled alert %s", arr[i].name);
		} else {
			break; /* 网络仍不通，下轮再来 */
		}
	}
	free(arr);
	return sent;
}

bool alert_pub_failed_recently(const enc_cfg_t *c, int *out_count,
			       char *err_kind, size_t sz)
{
	(void)c;
	if (out_count)
		*out_count = g_al.pub_fail_streak;
	if (err_kind)
		snprintf(err_kind, sz, "%s", g_al.last_err_kind);
	return g_al.pub_fail_streak > 0;
}
