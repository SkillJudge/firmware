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
 *   L0  wifi_bringup: 重新加载驱动 + ifup wlan0（仅 iface_missing 时）
 *       链路：/etc/wifi.conf → /etc/wireless/usb → modprobe
 *             → ifup wlan0 → wpa_supplicant 从 env 读 SSID/密码连 skilljudge
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

/* ---------------- L0: 接口缺失时主动拉起 WiFi ----------------
 *
 * iface_missing 说明 wlan0 不存在（驱动未加载或硬件复位）。
 * 此时 L1-L3（ifconfig/wpa_cli/udhcpc）对不存在的接口毫无意义，
 * 需要 L0 重新加载驱动 + ifup 触发 wpa_supplicant 连接 skilljudge。
 *
 * 链路：/etc/wifi.conf (WIFI_DEVICE) → /etc/wireless/usb → modprobe
 *       → ifup wlan0 → pre-up wpa_passphrase(fw_printenv wlanssid/wlanpass)
 *       → wpa_supplicant 连 AP
 * encalertd 不需要自己知道 SSID/密码，flash env 是唯一权威源。
 */
#define WIFI_BRINGUP_INTERVAL   60   /* L0 重试间隔，与 WIFI_RECOVER_MIN_INT 对齐 */
#define WIFI_DEVICE_MAX_LEN     64

/* 前向声明：sh() 和 iface_exists() 定义在后面，L0 先用到 */
static void sh(const char *fmt, ...);
static bool iface_exists(const enc_cfg_t *c);

static bool wifi_bringup(const enc_cfg_t *c)
{
	char device[WIFI_DEVICE_MAX_LEN] = {0};
	FILE *f;

	/* 读 /etc/wifi.conf 拿 WIFI_DEVICE（驱动加载脚本需要的参数） */
	f = fopen("/etc/wifi.conf", "r");
	if (f) {
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			char *eq = strchr(line, '=');
			if (!eq) continue;
			*eq = '\0';
			/* 去首尾空白 */
			char *k = line; while (*k == ' ' || *k == '\t') k++;
			char *ke = k + strlen(k);
			while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = '\0';
			char *v = eq + 1; while (*v == ' ' || *v == '\t') v++;
			size_t vl = strlen(v);
			while (vl > 0 && (v[vl-1] == '\n' || v[vl-1] == '\r' ||
			                  v[vl-1] == ' '  || v[vl-1] == '\t'))
				v[--vl] = '\0';
			/* 去掉引号 */
			if (vl >= 2 && ((v[0] == '\'' && v[vl-1] == '\'') ||
			                (v[0] == '"'  && v[vl-1] == '"'))) {
				v++; vl -= 2;
				v[vl] = '\0';
			}
			if (strcmp(k, "WIFI_DEVICE") == 0) {
				snprintf(device, sizeof(device), "%s", v);
				break;
			}
		}
		fclose(f);
	}

	if (device[0] == '\0') {
		log_msg(ENC_LOG_WARN, "wifi L0 bringup: WIFI_DEVICE not found in /etc/wifi.conf");
		return false;
	}

	log_msg(ENC_LOG_WARN, "wifi L0 bringup: loading driver for %s", device);

	/* 1. 重新加载无线驱动（modprobe 会处理已加载情况，无害） */
	sh("/etc/wireless/usb \"%s\" >/dev/null 2>&1", device);

	/* 2. 等待 wlan0 出现（最多 5s） */
	for (int i = 0; i < 5; i++) {
		if (iface_exists(c))
			break;
		sleep(1);
	}

	if (!iface_exists(c)) {
		log_msg(ENC_LOG_ERROR, "wifi L0 bringup: %s still missing after driver load",
			c->wifi_iface);
		return false;
	}

	/* 3. ifup wlan0 触发 wpa_supplicant（pre-up 从 env 读 SSID/密码） */
	log_msg(ENC_LOG_INFO, "wifi L0 bringup: ifup %s (wpa_supplicant will connect to skilljudge)",
		c->wifi_iface);
	sh("ifup %s >/dev/null 2>&1", c->wifi_iface);

	return true;
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
			/*
			 * iface_missing: wlan0 不存在，L1-L3 恢复动作
			 * （ifconfig/wpa_cli/udhcpc）对不存在的接口无意义。
			 * 改为 L0 主动拉起：重新加载驱动 + ifup wlan0，
			 * 触发 wpa_supplicant 从 env 读 SSID/密码连 skilljudge。
			 * 每 60s 重试一次，持续连下去直到 wlan0 出现。
			 */
			if (strcmp(why, "iface_missing") == 0) {
				bool ok = wifi_bringup(c);
				last_rec = now;
				rec_tries++;
				snprintf(reason, rsz,
					 "%s (L0 bringup %s)",
					 why, ok ? "initiated" : "failed");
			} else {
				int lvl = rec_tries + 1;

				if (lvl > WIFI_RECOVER_MAX_LVL)
					lvl = WIFI_RECOVER_MAX_LVL; /* L3 持续重试 */
				wifi_recover(c, lvl);
				last_rec = now;
				rec_tries++;
				snprintf(reason, rsz, "%s (L%d recovery applied)",
					 why, lvl);
			}
		} else {
			snprintf(reason, rsz, "%s (recovery pending L%d)",
				 why,
				 rec_tries >= WIFI_RECOVER_MAX_LVL ?
				 WIFI_RECOVER_MAX_LVL : rec_tries + 1);
		}
	}
	return reason;
}
