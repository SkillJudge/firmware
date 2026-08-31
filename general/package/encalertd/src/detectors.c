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
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

#define ENC_SD_MP               "/mnt/mmcblk0p1"
#define CRON_EVENT_FILE         "/tmp/encalertd_event"
#define RECORD_PURGE_MIN_INT    18000    /* 与原 bash 版一致：两次清理最短间隔 */
#define DEFAULT_SRS_PORT        1935

#define ALERT_PROC_STORM        6104     /* 重启风暴升级 (fatal) */
#define REBOOT_MIN_UPTIME_SEC   600      /* 开机 10min 内不执行风暴 reboot */

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

/* ==================== 5. 服务进程死亡 (6103 + 风暴 6104) ====================
 * 监控清单 conf monitor_procs，项格式 name[:pidfile[:cmdpat]]：
 *   - pidfile：显式 pid 文件路径（如 majestic:/var/run/majestic.pid）；
 *     省略时依次尝试 <state_dir>/<name>.pid
 *   - cmdpat：/proc/<pid>/cmdline 子串匹配模式，用于 shell 脚本进程
 *     兜底（encoder_main.sh 家族 comm 为解释器名 "sh"，comm 精确匹配
 *     永远失效），如 listener::app_service.sh listener
 * 探测链（任一命中即存活）：
 *   1) pid 文件读 pid + kill(pid,0)；显式 pidfile 命中时做身份复核
 *      （/proc/<pid>/comm 或 cmdline 匹配 name/cmdpat，防 pid 回收复用）
 *   2) comm 精确匹配 name（C 二进制进程：majestic/ipc_server）
 *   3) cmdline 子串匹配 cmdpat（仅 cmdpat 非空时）
 * 确认死亡 → 6103 error → on_confirmed：快照 → 拉起(仅根进程) → 风暴升级
 */

/* /proc/<pid>/comm 精确匹配（跳过 init 与自身） */
static bool comm_alive(const char *name)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	bool alive = false;
	size_t nl = strlen(name);

	if (!d || nl == 0)
		return false;
	while ((e = readdir(d))) {
		char path[48], comm[64];

		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		if (atoi(e->d_name) <= 1 || atoi(e->d_name) == getpid())
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
		if (!read_str_file(path, comm, sizeof(comm)))
			continue;
		if (strlen(comm) == nl && !strncmp(comm, name, nl)) {
			alive = true;
			break;
		}
	}
	closedir(d);
	return alive;
}

/* 读取 /proc/<pid>/cmdline 并把 NUL 分隔的 argv 拼成空格串 */
static bool proc_cmdline(long pid, char *buf, size_t sz)
{
	char path[48];
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
	f = fopen(path, "rb");
	if (!f)
		return false;
	n = fread(buf, 1, sz - 1, f);
	fclose(f);
	buf[n] = '\0';
	for (size_t i = 0; i + 1 < n; i++)
		if (buf[i] == '\0')
			buf[i] = ' ';
	return n > 0;
}

/* /proc/<pid>/cmdline 全表子串匹配 cmdpat（跳过 init 与自身）。
 * shell 脚本进程 comm 为解释器名("sh")，只能靠 cmdline 识别。 */
static bool cmdline_alive(const char *pat)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	bool alive = false;

	if (!d || !pat[0])
		return false;
	while ((e = readdir(d))) {
		char buf[512];

		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		if (atoi(e->d_name) <= 1 || atoi(e->d_name) == getpid())
			continue;
		if (!proc_cmdline(atoi(e->d_name), buf, sizeof(buf)))
			continue;
		if (strstr(buf, pat)) {
			alive = true;
			break;
		}
	}
	closedir(d);
	return alive;
}

/* pid 身份复核：kill(pid,0) 只证明 pid 位被占用，pid 可能已被回收
 * 复用给无关进程。命中 comm==name、cmdpat 或约定脚本名 <name>.sh
 * 之一才认定是目标进程；cmdline 不可读（刚退出/内核线程）则放行。 */
static bool pid_matches(long pid, const char *name, const char *cmdpat)
{
	char commp[48], comm[64], buf[512], shpat[72];

	snprintf(commp, sizeof(commp), "/proc/%ld/comm", pid);
	if (read_str_file(commp, comm, sizeof(comm)) &&
	    !strcmp(comm, name))
		return true;
	if (!proc_cmdline(pid, buf, sizeof(buf)))
		return true;            /* 无法取证时保守放行 */
	if (cmdpat[0] && strstr(buf, cmdpat))
		return true;
	snprintf(shpat, sizeof(shpat), "%s.sh", name);
	return strstr(buf, shpat) != NULL;
}

static const char *det_proc_down(const enc_cfg_t *c,
				 char *reason, size_t rsz)
{
	char list[256];
	char *item, *save;
	int dead = 0;

	reason[0] = '\0';
	snprintf(list, sizeof(list), "%s", c->monitor_procs);
	for (item = strtok_r(list, ",", &save); item;
	     item = strtok_r(NULL, ",", &save)) {
		char name[64], pidfile[128], cmdpat[128];
		char *colon = strchr(item, ':');
		char *colon2 = colon ? strchr(colon + 1, ':') : NULL;
		long pid = -1;
		bool alive = false;

		/* 项格式 name[:pidfile[:cmdpat]]，允许空字段（listener::pat） */
		if (colon)
			*colon = '\0';
		if (colon2)
			*colon2 = '\0';
		snprintf(name, sizeof(name), "%s", item);
		snprintf(pidfile, sizeof(pidfile), "%s",
			 colon ? colon + 1 : "");
		snprintf(cmdpat, sizeof(cmdpat), "%s",
			 colon2 ? colon2 + 1 : "");

		/* 1) pid 文件探测：显式路径优先，省略时用 state 目录约定 */
		{
			char p[256];

			if (pidfile[0])
				snprintf(p, sizeof(p), "%s", pidfile);
			else
				snprintf(p, sizeof(p), "%s/%s.pid",
					 c->state_dir, name);
			if (read_int_file(p, &pid) && pid > 1 &&
			    kill((pid_t)pid, 0) == 0)
				/* kill(pid,0) 只证明 pid 位被占用；pidfile 可能
				 * 陈旧/pid 被回收复用给无关进程，一律身份复核，
				 * 复核不过回落 comm/cmdline 探测（堵假存活漏报） */
				alive = pid_matches(pid, name, cmdpat);
		}
		/* 2) comm 精确匹配（C 二进制进程） */
		if (!alive)
			alive = comm_alive(name);
		/* 3) cmdline 子串匹配（shell 脚本进程兜底） */
		if (!alive)
			alive = cmdline_alive(cmdpat);

		if (!alive) {
			strncat(reason, dead ? "," : "", rsz - strlen(reason));
			strncat(reason, name, rsz - strlen(reason));
			dead++;
		}
	}
	return dead ? reason : NULL;
}

/* 递归建目录（crash_dir 在 SD 卡，可能不存在） */
static void mkdir_p_local(const char *path)
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

/* 风暴状态：<window_start> <count>，持久于 crash_dir（reboot 不丢） */
static void storm_state_read(const enc_cfg_t *c, uint32_t *win, int *cnt)
{
	char path[300], buf[64];

	*win = 0; *cnt = 0;
	snprintf(path, sizeof(path), "%s/storm_state", c->crash_dir);
	if (!read_str_file(path, buf, sizeof(buf)))
		return;
	sscanf(buf, "%u %d", win, cnt);
}

static void storm_state_write(const enc_cfg_t *c, uint32_t win, int cnt)
{
	char path[300];
	FILE *f;

	mkdir_p_local(c->crash_dir);
	snprintf(path, sizeof(path), "%s/storm_state", c->crash_dir);
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "%u %d\n", win, cnt);
	fclose(f);
}

static long uptime_sec(void)
{
	char buf[64];
	long up = 0;

	if (read_str_file("/proc/uptime", buf, sizeof(buf)))
		up = strtol(buf, NULL, 10);
	return up;
}

/* 确认死亡联动：先取证（崩溃快照到 SD）再拉起（仅根进程），并做风暴升级 */
static void cb_proc_restart(const enc_cfg_t *c, const det_t *d,
			    const char *reason)
{
	char ctx[256], out[256];
	uint32_t win = 0, now = (uint32_t)time(NULL);
	int cnt = 0;
	long ups = uptime_sec();

	(void)d;

	/* 1. 死亡快照：dmesg/业务日志/core 打包到 crash_dir（脚本幂等） */
	snprintf(ctx, sizeof(ctx), "{\"procs\":\"%s\"}", reason ? reason : "");
	action_run(c, "crash_snapshot", ctx, out, sizeof(out));
	if (out[0])
		log_msg(ENC_LOG_INFO, "crash snapshot: %s", out);

	/* 2. 拉起：脚本内映射根进程（majestic/encoder_main 家族/ipc_server） */
	action_run(c, "proc_restart", ctx, out, sizeof(out));
	if (out[0])
		log_msg(ENC_LOG_INFO, "proc restart: %s", out);

	/* 3. 风暴统计（窗口外清零重计） */
	storm_state_read(c, &win, &cnt);
	if (!win || now < win || now - win >= (uint32_t)c->storm_window_sec) {
		win = now;
		cnt = 0;
	}
	cnt++;
	storm_state_write(c, win, cnt);
	log_msg(ENC_LOG_WARN, "proc restart storm count=%d/%d win=%us",
		cnt, c->storm_max_restarts, c->storm_window_sec);
	if (cnt < c->storm_max_restarts)
		return;

	/* 4. 达到上限 → 6104 fatal；开机 10min 内不 reboot，保持现场 */
	{
		char detail[256];

		storm_state_write(c, win, 0);   /* 从新窗口重新计数 */
		if (ups < REBOOT_MIN_UPTIME_SEC) {
			snprintf(detail, sizeof(detail),
				 "{\"count\":%d,\"uptimeSec\":%ld,"
				 "\"action\":\"hold\"}", cnt, ups);
			alert_raise(c, ALERT_PROC_STORM, "proc_restart_storm",
				    "fatal",
				    "进程反复崩溃且重启后仍未恢复，保持现场等待人工处理",
				    detail);
			log_msg(ENC_LOG_ERROR,
				"ALERT %d storm hold: uptime=%lds too short to reboot",
				ALERT_PROC_STORM, ups);
		} else {
			snprintf(detail, sizeof(detail),
				 "{\"count\":%d,\"uptimeSec\":%ld,"
				 "\"action\":\"reboot\"}", cnt, ups);
			alert_raise(c, ALERT_PROC_STORM, "proc_restart_storm",
				    "fatal",
				    "进程反复崩溃，执行整机重启自愈",
				    detail);
			/* fatal 已入 spool/pipeline；reboot 前把滞留的推出去 */
			alert_flush_pending(c);
			log_msg(ENC_LOG_ERROR,
				"ALERT %d storm reboot now: count=%d uptime=%lds",
				ALERT_PROC_STORM, cnt, ups);
			action_run(c, "reboot", "{}", out, sizeof(out));
		}
	}
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

/* ==================== 0. 设备初始化监护 (9001 error / 9002 info) ====================
 * env 分区无 DEVICE_ID = factoryinit 未完成（出厂烧写后 env 无此变量，
 * 须由 factoryinit 连接服务器换取后写入；固件升级不刷 env 分区）。
 * 业务完全不可用，confirm=1 立即告警；factoryinit 写入后发 9002 恢复。
 * reason 附带 ethaddr，便于多机场景定位具体是哪台未初始化。
 */
static const char *det_deviceid(const enc_cfg_t *c, char *reason, size_t rsz)
{
	char eth[32];

	(void)c;
	if (!device_id_uninitialized())
		return NULL;
	eth[0] = '\0';
	device_ethaddr_get(eth, sizeof(eth));
	if (eth[0])
		snprintf(reason, rsz, "DEVICE_ID missing (eth=%s)", eth);
	else
		snprintf(reason, rsz, "DEVICE_ID missing");
	return reason;
}

/* ==================== 使能开关包装 ==================== */

static bool en_always(const enc_cfg_t *c) { (void)c; return true; }
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
	{ "deviceid",     30,  1, 1,  en_always, det_deviceid,      NULL,
	  { 9001, "device_uninitialized", "error", "设备未初始化(%s)，等待 factoryinit 写入 DEVICE_ID" },
	  { 9002, "device_initialized", "info", "设备初始化完成，DEVICE_ID 已就绪" } },

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

	{ "proc_down",    15,  2, 3,  en_process, det_proc_down,    cb_proc_restart,
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
