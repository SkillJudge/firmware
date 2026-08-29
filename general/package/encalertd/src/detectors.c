/*
 * detectors.c — 检测器实现与注册表
 *
 * 契约：一个检测器只对应一个 fail 告警码（desc_fmt 中的 %s 由调度器用
 * reason 填充）；事件型逻辑（cron 兜底、录像清理）在 check 内直接调
 * alert_raise() 并恒返回 NULL，不进入 confirm 计数。
 *
 * WiFi 断线检测与 L1-L3 分级重连在 wifi.c（det_wifi_watch），
 * 取代原 bash wifi_watchdog.sh 的事件桥接（/tmp/wifi_watchdog_event 已废弃）。
 *
 * 原则：检测器只读 bash 维护的状态文件（单一写入方原则），
 *       恢复动作全部下沉到 ${actions_dir}/ 脚本由 C 调起
 *       （例外：wifi 恢复为 C 内置系统调用，见 wifi.c）。
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

#define ENC_SD_MP               "/mnt/mmcblk0p1"
#define CRON_EVENT_FILE         "/tmp/encalertd_event"
#define RECORD_PURGE_MIN_INT    18000    /* 与原 bash 版一致：两次清理最短间隔 */
#define DEFAULT_SRS_PORT        1935

/* wifi.c 提供 */
extern const char *det_wifi_watch(const enc_cfg_t *c,
				  char *reason, size_t rsz);

static void state_path(const enc_cfg_t *c, const char *name,
		       char *buf, size_t sz)
{
	snprintf(buf, sz, "%s/%s", c->state_dir, name);
}

static uint32_t now32(void)
{
	return (uint32_t)time(NULL);
}

/* ==================== SD 卡底层探测 ==================== */

static bool sd_is_mounted(void)
{
	FILE *f = fopen("/proc/mounts", "r");
	char line[512], mp[128];
	bool ok = false;

	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%*s %127s ", mp) == 1 &&
		    !strcmp(mp, ENC_SD_MP)) {
			ok = true;
			break;
		}
	}
	fclose(f);
	return ok;
}

static bool sd_is_ro(void)
{
	FILE *f = fopen("/proc/mounts", "r");
	char line[512], opts[128] = "";
	bool ro = false;
	char probe[160];
	int fd;

	if (!f)
		return true;
	while (fgets(line, sizeof(line), f)) {
		char mp[128];

		if (sscanf(line, "%*s %127s %*s %127[^\n ]", mp, opts) == 2 &&
		    !strcmp(mp, ENC_SD_MP))
			break;
	}
	fclose(f);

	/* 写探针：mount 标志可信但介质可能已写保护 */
	snprintf(probe, sizeof(probe), "%s/.enc_sd_probe", ENC_SD_MP);
	fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC);
	if (fd < 0) {
		ro = true;
	} else {
		close(fd);
		unlink(probe);
	}
	return ro;
}

static long sd_usage_pct(void)
{
	struct statvfs st;

	if (statvfs(ENC_SD_MP, &st) != 0 || st.f_blocks == 0)
		return 0;
	return ((long)(st.f_blocks - st.f_bavail) * 100L) /
	       (long)st.f_blocks;
}

/* 录像盘剩余空间 MB；读不到返回 -1 */
static long sd_free_mb(void)
{
	struct statvfs st;

	if (statvfs(ENC_SD_MP, &st) != 0 || st.f_blocks == 0)
		return -1;
	return (long)(((long long)st.f_bavail * (st.f_frsize / 1024)) / 1024);
}

/* ---------- 1a. SD 卡缺失 (4001)：确认前先自动重挂一次 ---------- */

static const char *det_sd_missing(const enc_cfg_t *c,
				  char *reason, size_t rsz)
{
	if (sd_is_mounted())
		return NULL;

	action_run(c, "sd_remount", "{\"what\":\"sdcard\"}", NULL, 0);
	if (sd_is_mounted()) {
		log_msg(ENC_LOG_INFO, "sd remount action succeeded");
		return NULL;
	}
	snprintf(reason, rsz, "not_mounted");
	return reason;
}

/* ---------- 1b. SD 卡只读 (4002) ---------- */

static const char *det_sd_ro(const enc_cfg_t *c, char *reason, size_t rsz)
{
	(void)c;
	if (!sd_is_mounted())
		return NULL; /* 缺失归 4001 管 */
	if (!sd_is_ro())
		return NULL;
	snprintf(reason, rsz, "readonly");
	return reason;
}

/* ---------- 1c. SD 卡空间不足 (4003)：占用率超阈 或 空闲低于保留目标 ---------- */

static const char *det_sd_full(const enc_cfg_t *c, char *reason, size_t rsz)
{
	long pct, free_mb;
	bool short_on_space;

	if (!sd_is_mounted())
		return NULL;
	pct = sd_usage_pct();
	free_mb = sd_free_mb();
	short_on_space = free_mb >= 0 &&
			 (long long)free_mb < (long long)c->record_min_free_mb;
	if (pct < c->sd_warn_pct && !short_on_space)
		return NULL;
	if (short_on_space)
		snprintf(reason, rsz, "%ld%% free=%ldMB<%ldMB",
			 pct, free_mb, c->record_min_free_mb);
	else
		snprintf(reason, rsz, "%ld%%", pct);
	return reason;
}

/* ==================== 2. 低电关机 (5001 fatal + 回调关机) ==================== */

static void cb_low_batt_shutdown(const enc_cfg_t *c,
				 const struct det_s *d, const char *reason)
{
	(void)d;
	log_msg(ENC_LOG_WARN, "LOW BATTERY confirmed (%s), executing shutdown",
		reason);
	alert_flush_pending(c);          /* 抢先把 fatal 报警推出去 */
	action_run(c, "shutdown_lowbattery",
		   "{\"reason\":\"low_battery\"}", NULL, 0);
	/* poweroff 由脚本执行；本进程继续无害运行直至断电 */
}

static const char *det_low_battery(const enc_cfg_t *c,
				   char *reason, size_t rsz)
{
	long mv = -1, soc = -1;
	bool trig = false;
	char p[256];

	state_path(c, "battery_voltage_mv", p, sizeof(p));
	read_int_file(p, &mv);
	state_path(c, "battery", p, sizeof(p));
	read_int_file(p, &soc);

	if (mv > 100 && mv < c->low_batt_mv)
		trig = true;
	if (soc >= 0 && soc < c->low_batt_pct)
		trig = true;
	if (!trig)
		return NULL;

	snprintf(reason, rsz, "%ldmV/%ld%%", mv, soc);
	return reason;
}

/* ==================== 3a. 电量感知失效 (5201) ==================== */

static const char *det_battery_sensor(const enc_cfg_t *c,
				      char *reason, size_t rsz)
{
	long v;
	char p[256];

	(void)c;
	state_path(c, "battery_voltage_mv", p, sizeof(p));
	if (read_int_file(p, &v))
		return NULL;
	state_path(c, "battery", p, sizeof(p));
	if (read_int_file(p, &v))
		return NULL;
	snprintf(reason, rsz, "voltage/soc state files unreadable");
	return reason;
}

/* ==================== 3b. 充电故障 (2001) ==================== */

struct chg_row {
	uint32_t ts;
	long soc;
	long mv;
};

#define CHG_MAX_ROWS 512

static const char *det_charge_fault(const enc_cfg_t *c,
				    char *reason, size_t rsz)
{
	char histp[320], chgp[256];
	char chg[32] = "";
	struct chg_row rows[CHG_MAX_ROWS];
	int nrows = 0, keep = 0;
	uint32_t nowt = now32();
	uint32_t win = (uint32_t)c->charge_window_sec > 0 ?
		       (uint32_t)c->charge_window_sec : 300u;
	FILE *rf;

	snprintf(histp, sizeof(histp), "%s/charge_history", c->spool_dir);
	state_path(c, "is_charging", chgp, sizeof(chgp));
	read_str_file(chgp, chg, sizeof(chg));

	if (strcmp(chg, "true") != 0) {
		unlink(histp);               /* 非充电态清空窗口 */
		return NULL;
	}

	/* 读入历史采样 */
	rf = fopen(histp, "r");
	if (rf) {
		while (nrows < CHG_MAX_ROWS) {
			struct chg_row r;

			if (fscanf(rf, "%u|%ld|%ld\n",
				   &r.ts, &r.soc, &r.mv) != 3)
				break;
			rows[nrows++] = r;
		}
		fclose(rf);
	}
	for (int i = 0; i < nrows; i++)
		if (rows[i].ts >= nowt - win && rows[i].ts <= nowt)
			rows[keep++] = rows[i];
	nrows = keep;

	{
		long mv = -1, soc = -1;
		char vp[256], sp[256];

		state_path(c, "battery_voltage_mv", vp, sizeof(vp));
		read_int_file(vp, &mv);
		state_path(c, "battery", sp, sizeof(sp));
		read_int_file(sp, &soc);

		if (nrows < CHG_MAX_ROWS && (soc >= 0 || mv > 100)) {
			rows[nrows].ts  = nowt;
			rows[nrows].soc = soc;
			rows[nrows].mv  = mv;
			nrows++;
		}
	}

	/* 重写窗口内数据 */
	rf = fopen(histp, "w");
	if (rf) {
		for (int i = 0; i < nrows; i++)
			fprintf(rf, "%u|%ld|%ld\n",
				rows[i].ts, rows[i].soc, rows[i].mv);
		fclose(rf);
	}

	if (nrows >= 3) {
		struct chg_row first = rows[0];
		struct chg_row last  = rows[nrows - 1];
		long dp = first.soc - last.soc;      /* 充电中应上升 */
		long dv = first.mv - last.mv;

		if ((first.soc >= 0 && last.soc >= 0 && dp >= c->charge_drop_pct) ||
		    (first.mv > 100 && last.mv > 100 && dv >= c->charge_drop_mv)) {
			snprintf(reason, rsz,
				 "window=%us dropPct=%+ld dropMv=%+ld",
				 (unsigned)win, dp, dv);
			return reason;
		}
	}
	return NULL;
}

/* ==================== 4a. SoC 过热 (8101) ==================== */

static const char *det_overheat(const enc_cfg_t *c,
				char *reason, size_t rsz)
{
	const char *TZ = "/sys/class/thermal/thermal_zone0/temp";
	long mc = 0;

	if (!read_int_file(TZ, &mc))
		return NULL;                 /* 读不到温度计属环境差异，静默跳过 */
	if (mc < c->temp_warn_mc)
		return NULL;
	snprintf(reason, rsz, "%ld.%01ldC%s",
		 mc / 1000, (mc % 1000) / 100,
		 mc >= c->temp_err_mc ? " CRITICAL" : "");
	return reason;
}

/* ==================== 4b. CPU 过载 (8102) ==================== */

static const char *det_cpu_overload(const enc_cfg_t *c,
				    char *reason, size_t rsz)
{
	double l1 = 0;
	int ncpu;

	{
		char buf[64];

		if (!read_str_file("/proc/loadavg", buf, sizeof(buf)))
			return NULL;
		l1 = atof(buf);
	}
	ncpu = get_nprocs();
	if (ncpu < 1)
		ncpu = 1;
	if (l1 / ncpu <= c->load_factor_warn)
		return NULL;
	snprintf(reason, rsz, "load %.2f / %d cpu", l1, ncpu);
	return reason;
}

/* ==================== 4c. 内存压力 (8103) ==================== */

static const char *det_mem_pressure(const enc_cfg_t *c,
				    char *reason, size_t rsz)
{
	long avail_kb = -1;
	char line[128];
	FILE *f = fopen("/proc/meminfo", "r");

	if (f) {
		while (fgets(line, sizeof(line), f))
			if (sscanf(line, "MemAvailable: %ld kB", &avail_kb) == 1)
				break;
		fclose(f);
	}
	if (avail_kb < 0)
		return NULL;
	if (avail_kb >= c->mem_avail_min_kb)
		return NULL;
	snprintf(reason, rsz, "MemAvailable=%ldkB", avail_kb);
	return reason;
}

/* ==================== 5. 服务进程死亡 (6103) ==================== */

static const char *det_proc_down(const enc_cfg_t *c,
				 char *reason, size_t rsz)
{
	static const struct { const char *label; char path[96]; } procs[] = {
		{ "majestic",  "/var/run/majestic.pid" },
	};
	size_t base = sizeof(procs) / sizeof(procs[0]);
	static char extra[3][32] = { "encoder_main.pid", "listener.pid",
				     "heartbeat.pid" };
	int dead = 0;

	reason[0] = '\0';
	for (size_t i = 0; i < base; i++) {
		long pid = -1;
		bool alive;

		if (!read_int_file(procs[i].path, &pid)) {
			alive = false;
		} else if (pid > 1 && kill((pid_t)pid, 0) == 0) {
			alive = true;
		} else {
			alive = false;
		}
		if (!alive) {
			strncat(reason, dead ? "," : "", rsz - strlen(reason));
			strncat(reason, procs[i].label, rsz - strlen(reason));
			dead++;
		}
	}
	for (size_t i = 0; i < 3; i++) {
		char p[256], label[32];
		char *dot;
		long pid = -1;
		bool alive;

		state_path(c, extra[i], p, sizeof(p));
		alive = read_int_file(p, &pid) &&
			pid > 1 && kill((pid_t)pid, 0) == 0;
		if (!alive) {
			snprintf(label, sizeof(label), "%s", extra[i]);
			dot = strrchr(label, '.');
			if (dot)
				*dot = '\0';     /* 去掉 .pid 后缀 */
			strncat(reason, dead ? "," : "", rsz - strlen(reason));
			strncat(reason, label, rsz - strlen(reason));
			dead++;
		}
	}
	(void)c;
	return dead ? reason : NULL;
}

/* ==================== 6. 推流健康 (7001/7002) ==================== */

/* 解析 rtmp://host[:port]/... 的端口，失败默认 1935 */
static int stream_port_from_url(const enc_cfg_t *c)
{
	char url[192] = "";
	char hostpart[160];
	const char *hp, *colon;

	state_path(c, "current_stream_url", url, sizeof(url));
	hp = strstr(url, "//");
	if (!hp)
		return DEFAULT_SRS_PORT;
	hp += 2;
	{
		size_t n = strcspn(hp, "/");
		if (n == 0 || n >= sizeof(hostpart))
			return DEFAULT_SRS_PORT;
		memcpy(hostpart, hp, n);
		hostpart[n] = '\0';
	}
	colon = strchr(hostpart, ':');
	if (colon && colon[1])
		return atoi(colon + 1);
	return DEFAULT_SRS_PORT;
}

static bool tcp_established_to_port(int port)
{
	FILE *f = fopen("/proc/net/tcp", "r");
	char line[512];
	bool ok = false;

	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		char rem[80];
		unsigned st;

		if (sscanf(line, "%*x:%*x %79s %x", rem, &st) != 2)
			continue;
		if (st != 0x01)
			continue;
		{
			char *col = strrchr(rem, ':');
			int rp;

			if (!col)
				continue;
			rp = (int)strtol(col + 1, NULL, 16);
			if (rp == port) {
				ok = true;
				break;
			}
		}
	}
	fclose(f);
	return ok;
}

static long long net_tx_total(void)
{
	static const char *ifs[] = {
		"/sys/class/net/wlan0/statistics/tx_bytes",
		"/sys/class/net/eth0/statistics/tx_bytes",
	};
	long long total = 0;

	for (size_t i = 0; i < sizeof(ifs) / sizeof(ifs[0]); i++) {
		long v;

		if (read_int_file(ifs[i], &v))
			total += v;
	}
	return total;
}

static struct {
	bool     have_prev;
	long long prev_tx;
} g_tx;

static void cb_stream_hup(const enc_cfg_t *c, const struct det_s *d,
			  const char *reason)
{
	(void)d;
	log_msg(ENC_LOG_WARN, "stream dead confirmed (%s), HUP majestic", reason);
	g_tx.have_prev = false;
	g_tx.prev_tx   = 0;
	action_run(c, "stream_hup_majestic", "{\"why\":\"stream_dead\"}",
		   NULL, 0);
}

static bool det_stream_publishing(const enc_cfg_t *c)
{
	char p[256], v[16] = "";

	state_path(c, "is_publishing", p, sizeof(p));
	read_str_file(p, v, sizeof(v));
	return !strcmp(v, "true");
}

static const char *det_stream_dead(const enc_cfg_t *c,
				   char *reason, size_t rsz)
{
	bool publishing = det_stream_publishing(c);
	int port = stream_port_from_url(c);
	bool est = tcp_established_to_port(port);
	long long tx = net_tx_total();
	long long delta = 0;

	if (g_tx.have_prev)
		delta = tx - g_tx.prev_tx;
	g_tx.have_prev = true;
	g_tx.prev_tx   = tx;

	/*
	 * 判定语义：
	 * - 未推流           → 恒正常（只维持流量基线）
	 * - 无 ESTABLISHED   → 立即异常（连接都没了）
	 * - 有连接零流量增量 → 视为 stall：连续 confirm_cnt(3) 轮
	 *                      delta<=0 时由调度器升级为 7001 故障
	 */
	if (!publishing)
		return NULL;
	if (!est) {
		snprintf(reason, rsz, "no_established_to_%d", port);
		return reason;
	}
	if (delta <= 0) {
		snprintf(reason, rsz, "tx_stalled_delta=%lld", delta);
		return reason;
	}
	return NULL;
}

/* ==================== 7. cron 兜底事件直通 (6001) ==================== */

static void events_process_one(const enc_cfg_t *c, const char *path)
{
	char body[4096] = "";
	char tmp_path[512];
	FILE *f;
	int rc;

	/* 原子消费：先 rename 到临时路径再读，避免多实例抢读/读删竞态。
	 * 读成功与否都会删临时文件，原文件一旦 rename 成功就已不存在。 */
	snprintf(tmp_path, sizeof(tmp_path), "%s.%d.%ld.tmp",
		 path, (int)getpid(), (long)time(NULL));
	rc = rename(path, tmp_path);
	if (rc < 0)
		return;          /* 没有新事件 */

	f = fopen(tmp_path, "r");
	if (!f) {
		unlink(tmp_path);
		return;
	}
	{
		size_t got = fread(body, 1, sizeof(body) - 1, f);

		body[got] = '\0';
	}
	fclose(f);

	char *save = NULL;
	char *line;

	for (line = body; ; line = NULL) {
		char *tok = strtok_r(line, "\r\n", &save);

		if (!tok)
			break;
		char type[40] = "";

		sscanf(tok, "%39[^|]", type);
		if (!type[0])
			continue;

		if (!strcmp(type, "watchdog_missing")) {
			alert_raise(c, 6001, "watchdog_missing", "error",
				    "cron 守护发现进程缺失并已拉起",
				    "{\"by\":\"crontab\"}");
		}
	}
	unlink(tmp_path);
}

static const char *det_events(const enc_cfg_t *c, char *reason, size_t rsz)
{
	(void)rsz;
	events_process_one(c, CRON_EVENT_FILE);
	return NULL;                 /* 直通式：不经 confirm 管道 */
}

/* ==================== 8. 录像清理任务 (3001 info) ====================
 * 双模式：
 *   空间模式：空闲 < record_min_free_mb → 每周期必查必清（不受闸门），
 *             删最旧录像直至恢复保留水位；清理动作由 record_purge.sh 执行
 *   时间模式：空闲充足时仅按 5h 闸门清理 >24h 旧录像（防 SD 写满老化）
 * 两种模式都发 3001 info，detail.mode 区分。
 */

static void record_purge_run(const enc_cfg_t *c, const char *ctx,
			     long free_mb, const char *mode)
{
	char out[512] = "";
	long files = 0, bytes = 0, after = free_mb;

	action_run(c, "record_purge", ctx, out, sizeof(out));
	files = action_out_num(out, "purgedFiles", 0);
	bytes = action_out_num(out, "freedBytes", 0);
	after = action_out_num(out, "freeMb", free_mb);

	if (files <= 0)
		return;                  /* 没有可清理内容，不打扰 */

	{
		char desc[192];
		char detail[256];

		if (!strcmp(mode, "space"))
			snprintf(desc, sizeof(desc),
				 "磁盘剩余低于 %ldMB，已清理最旧录像 %ld 个文件"
				 "(剩余 %ldMB)",
				 c->record_min_free_mb, files, after);
		else
			snprintf(desc, sizeof(desc),
				 "已清理过期录像 %ld 个文件 (%ld bytes)",
				 files, bytes);
		snprintf(detail, sizeof(detail),
			 "{\"mode\":\"%s\",\"purgedFiles\":%ld,"
			 "\"freedBytes\":%ld,\"freeMb\":%ld}",
			 mode, files, bytes, after);
		alert_raise(c, 3001, "record_purged", "info", desc, detail);
		log_msg(ENC_LOG_INFO, "record purge[%s]: %ld files, "
			"free %ld->%ldMB", mode, files, free_mb, after);
	}
}

static const char *det_record_purge(const enc_cfg_t *c,
				    char *reason, size_t rsz)
{
	char stamp_path[320];
	uint32_t last_ts = 0;
	uint32_t nowt = now32();
	long free_mb = sd_free_mb();

	(void)reason; (void)rsz;

	/* 1) 空间模式：低于保留水位时每个周期(600s)立即清理 */
	if (free_mb >= 0 && free_mb < c->record_min_free_mb) {
		char ctx[64];

		snprintf(ctx, sizeof(ctx), "{\"minFreeMb\":%ld}",
			 c->record_min_free_mb);
		record_purge_run(c, ctx, free_mb, "space");
		return NULL;
	}

	/* 2) 时间模式：5h 闸门 */
	snprintf(stamp_path, sizeof(stamp_path), "%s/record_purge_last",
		 c->spool_dir);
	{
		long v = 0;

		read_int_file(stamp_path, &v);
		last_ts = (uint32_t)v;
	}
	if (last_ts && nowt - last_ts < RECORD_PURGE_MIN_INT)
		return NULL;

	record_purge_run(c, "{\"olderThanHours\":24}", free_mb, "time");
	{
		FILE *sf = fopen(stamp_path, "w");

		if (sf) {
			fprintf(sf, "%u\n", nowt);
			fclose(sf);
		}
	}
	return NULL;
}

/* ==================== 使能开关包装 ==================== */

static bool en_sdcard(const enc_cfg_t *c) { return c->enable_sdcard; }
static bool en_battery(const enc_cfg_t *c) { return c->enable_battery; }
static bool en_sysres(const enc_cfg_t *c) { return c->enable_sysres; }
static bool en_process(const enc_cfg_t *c) { return c->enable_process; }
static bool en_stream(const enc_cfg_t *c) { return c->enable_stream; }
static bool en_wifi(const enc_cfg_t *c) { return c->enable_wifi; }

/* ==================== 注册表 ==================== */

det_t *detectors_registry(void)
{
	static det_t D[] = {
	/* name           ev   cf rc  enabled    check              cb                    fail-alert                     recover                   */
	{ "sd_missing",   20,  2, 2,  en_sdcard, det_sd_missing,    NULL,
	  { 4001, "sdcard_missing",  "error", "SD 卡未挂载，自动重挂仍失败(%s)" },
	  { 0, "", "", "" } },

	{ "sd_ro",        30,  2, 3,  en_sdcard, det_sd_ro,         NULL,
	  { 4002, "sdcard_readonly", "error", "SD 卡只读疑似介质损坏(%s)" },
	  { 0, "", "", "" } },

	{ "sd_full",      60,  2, 5,  en_sdcard, det_sd_full,       NULL,
	  { 4003, "sdcard_full",     "warn",  "SD 卡空间不足(%s)，自动清理最旧录像中" },
	  { 0, "", "", "" } },

	{ "low_battery",  20,  3, 30, en_battery, det_low_battery,  cb_low_batt_shutdown,
	  { 5001, "low_battery_shutdown", "fatal",
	    "电池电压/电量过低(%s)，系统即将关机防过放" },
	  { 0, "", "", "" } },

	{ "battery_sensor", 60, 5, 5,  en_battery, det_battery_sensor, NULL,
	  { 5201, "battery_sensor_unknown", "warn",
	    "电量状态持续不可读(%s)，低电保护存在失效风险" },
	  { 0, "", "", "" } },

	{ "charge_fault", 20,  3, 5,  en_battery, det_charge_fault,  NULL,
	  { 2001, "charge_fault", "error",
	    "充电中电压/电量持续下降疑似接触不良或电池故障(%s)" },
	  { 0, "", "", "" } },

	{ "overheat",     30,  3, 3,  en_sysres, det_overheat,      NULL,
	  { 8101, "soc_overheat", "warn", "SoC 温度过高(%s)" },
	  { 0, "", "", "" } },

	{ "cpu_overload", 60,  5, 5,  en_sysres, det_cpu_overload,  NULL,
	  { 8102, "cpu_overload", "warn", "CPU 负载过高(%s) 持续 5 分钟" },
	  { 0, "", "", "" } },

	{ "mem_pressure", 30,  5, 5,  en_sysres, det_mem_pressure,  NULL,
	  { 8103, "mem_pressure", "error", "可用内存不足(%s)，OOM 风险" },
	  { 0, "", "", "" } },

	{ "proc_down",    15,  2, 3,  en_process, det_proc_down,    NULL,
	  { 6103, "service_down", "error", "关键服务进程消亡(%s)" },
	  { 0, "", "", "" } },

	{ "stream_dead",  10,  3, 2,  en_stream, det_stream_dead,   cb_stream_hup,
	  { 7001, "stream_dead", "error", "推流链路失效(%s)，尝试自愈" },
	  { 7002, "stream_selfhealed", "info", "推流链路已恢复(%s)" } },

	{ "wifi_watch",   30,  3, 2,  en_wifi, det_wifi_watch,    NULL,
	  { 1001, "wifi_disconnected", "warn", "WiFi 断线(%s)，自动重连中" },
	  { 1002, "wifi_reconnected", "info", "WiFi 已恢复(%s)" } },

	{ "cron_events",   5,  1, 1,  en_wifi, det_events,        NULL,
	  { 0, "", "", "" },
	  { 0, "", "", "" } },

	{ "record_purge", 600, 1, 1,  en_sdcard, det_record_purge,  NULL,
	  { 0, "", "", "" },
	  { 0, "", "", "" } },
	};

	size_t n = sizeof(D) / sizeof(D[0]);
	/* 仅首次调用初始化：run_loop 每轮都会调本函数，
	 * 若重复重置 next_due 会导致检测器永远无法到期（错峰逻辑被破坏）。 */
	static bool inited;

	if (!inited) {
		time_t t0 = time(NULL);

		for (size_t i = 0; i < n; i++) {
			D[i].next = (i + 1 < n) ? &D[i + 1] : NULL;
			D[i].next_due = t0 + (time_t)i; /* 错峰首检 */
			D[i].fail_streak = 0;
			D[i].ok_streak = 0;
			D[i].failing = false;
		}
		inited = true;
	}
	return D;
}
