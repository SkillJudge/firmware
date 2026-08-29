/*
 * wifi.c — C 原生 WiFi 断线检测与分级自动重连
 *
 * 取代原 bash wifi_watchdog.sh 的事件桥接（/tmp/wifi_watchdog_event 已废弃）。
 *
 * 探测链（任一环节失败即视为断线）：
 *   1. 接口存在         /sys/class/net/<iface>
 *   2. 接口有 IPv4      ioctl SIOCGIFADDR
 *   3. 可达性           ping 默认网关 || ping MQTT broker
 *      （现场存在静态 IP 无默认路由的组网；broker 可达即业务在线）
 *
 * 分级恢复（按连续断线轮次自动升级，探测成功后归零）：
 *   L1  ifconfig <iface> down/up            接口复位
 *   L2  wpa_cli -i <iface> reassociate      重关联 AP
 *   L3  重启 DHCP（kill udhcpc + 后台重跑） IP 层重建
 *   L4  保持 L3 周期重试（60s），不自动 reboot
 *       —— 避免打断录像任务，由 1001 告警（dedup 600s 节流）引导运维介入
 *
 * 首次探测失败立即执行 L1；此后每 60s 升一级。确认告警由调度器
 * confirm_cnt（3 轮 × 30s）控制，此前已自动尝试 L1~L2。
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

#define WIFI_PROBE_TIMEOUT_S    2     /* ping -W 超时 */
#define WIFI_RECOVER_MIN_INT    60    /* 两次恢复动作最短间隔 */
#define WIFI_RECOVER_MAX_LVL    3     /* L3 封顶，L4=持续重试 */

/* ---------------- 探测底层 ---------------- */

static bool iface_exists(const enc_cfg_t *c)
{
	char path[64];

	snprintf(path, sizeof(path), "/sys/class/net/%s", c->wifi_iface);
	return access(path, F_OK) == 0;
}

static bool iface_has_ipv4(const enc_cfg_t *c)
{
	struct ifreq ifr;
	int fd, ok;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return false;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", c->wifi_iface);
	ok = ioctl(fd, SIOCGIFADDR, &ifr) == 0;
	close(fd);
	return ok;
}

/* 从 /proc/net/route 取默认网关（Destination=0 且 RTF_GATEWAY） */
static bool gw_get(char *ip, size_t sz)
{
	FILE *f = fopen("/proc/net/route", "r");
	char line[256];
	bool ok = false;

	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		char iface[64], dest[16], gw_hex[16];

		if (sscanf(line, "%63s %*s %15s %15s %*s %*s %*s %*s %*s %*s %*s",
			   iface, dest, gw_hex) != 3)
			continue;
		if (strcmp(dest, "00000000") != 0)
			continue;
		{
			uint32_t gw = (uint32_t)strtoul(gw_hex, NULL, 16);
			struct in_addr a;

			if (gw == 0)
				continue;         /* 链路路由无网关 */
			a.s_addr = gw;
			snprintf(ip, sz, "%s", inet_ntoa(a));
			ok = true;
			break;
		}
	}
	fclose(f);
	return ok;
}

/* fork+exec ping（PATH 可被测试环境 mock） */
static bool ping_ok(const char *ip)
{
	pid_t pid;
	int st = -1;

	pid = fork();
	if (pid < 0)
		return false;
	if (pid == 0) {
		char *argv[] = {
			(char *)"ping", (char *)"-c", (char *)"1",
			(char *)"-W", (char *)"2", (char *)ip, NULL,
		};
		execvp("ping", argv);
		_exit(127);
	}
	if (waitpid(pid, &st, 0) < 0)
		return false;
	return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* ---------------- L1-L3 恢复动作 ---------------- */

static void sh(const char *fmt, ...)
{
	char cmd[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	system(cmd);
}

static void wifi_recover(const enc_cfg_t *c, int level)
{
	const char *ifn = c->wifi_iface;

	switch (level) {
	case 1:
		log_msg(ENC_LOG_WARN, "wifi recovery L1: ifconfig %s down/up",
			ifn);
		sh("ifconfig %s down; sleep 1; ifconfig %s up", ifn, ifn);
		break;
	case 2:
		log_msg(ENC_LOG_WARN, "wifi recovery L2: wpa reassociate %s",
			ifn);
		/* wpa_cli 可能不存在（不同固件），失败无害 */
		sh("wpa_cli -i %s reassociate >/dev/null 2>&1 || "
		   "wpa_cli -i %s reconnect >/dev/null 2>&1 || true",
		   ifn, ifn);
		break;
	default:
		log_msg(ENC_LOG_WARN, "wifi recovery L3: restart DHCP %s",
			ifn);
		sh("killall udhcpc 2>/dev/null; "
		   "udhcpc -i %s -b -q >/dev/null 2>&1 &", ifn);
		break;
	}
}

/* ---------------- 检测器入口（detectors.c 注册） ---------------- */

/*
 * 返回 NULL=连通；否则 reason 携带故障环节与已施加的恢复等级。
 * 恢复状态（rec_tries/last_rec）为模块级静态——单线程调度器下安全。
 *
 * 可达性判定：默认网关（若存在）或 MQTT broker 任一 ping 通即在线。
 * 现场存在静态 IP 无默认路由的组网（局域网自足，业务只需可达 broker），
 * 且部分 AP 禁 ICMP——单一网关探测会误判断线并触发无谓的 L1 断网恢复。
 */
const char *det_wifi_watch(const enc_cfg_t *c, char *reason, size_t rsz)
{
	static int    rec_tries;             /* 已执行的恢复次数 */
	static time_t last_rec;              /* 上次恢复动作时刻 */
	char gw[32] = "";
	const char *why = NULL;

	if (!iface_exists(c)) {
		why = "iface_missing";
	} else if (!iface_has_ipv4(c)) {
		why = "no_ipv4";
	} else {
		bool gw_ok = gw_get(gw, sizeof(gw)) && ping_ok(gw);

		if (gw_ok || ping_ok(c->mqtt_host)) {
			if (!gw_ok)
				log_msg(ENC_LOG_DEBUG,
					"gw %s unreachable but broker %s ok",
					gw, c->mqtt_host);
		} else {
			why = "gw_unreachable";
		}
	}

	if (!why) {
		if (rec_tries > 0)
			log_msg(ENC_LOG_INFO,
				"wifi back online after %d recovery attempt(s)",
				rec_tries);
		rec_tries = 0;
		last_rec  = 0;
		return NULL;
	}

	{
		time_t now = time(NULL);

		if (last_rec == 0 ||
		    now - last_rec >= WIFI_RECOVER_MIN_INT) {
			int lvl = rec_tries + 1;

			if (lvl > WIFI_RECOVER_MAX_LVL)
				lvl = WIFI_RECOVER_MAX_LVL; /* L3 持续重试 */
			wifi_recover(c, lvl);
			last_rec = now;
			rec_tries++;
			snprintf(reason, rsz, "%s (L%d recovery applied)",
				 why, lvl);
		} else {
			snprintf(reason, rsz, "%s (recovery pending L%d)",
				 why,
				 rec_tries >= WIFI_RECOVER_MAX_LVL ?
				 WIFI_RECOVER_MAX_LVL : rec_tries + 1);
		}
	}
	return reason;
}
