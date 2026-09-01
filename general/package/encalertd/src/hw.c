/*
 * hw.c — 硬件自检检测器 (det_hw_watch, 9101 hw_fault / 9102 hw_recovered)
 *
 * 目的：无人值守的周期性硬件健康巡检（用户场景：先密集 1h/次，稳定后
 * 调稀到 1 天/次，周期由 conf hw_interval_sec 配置）。
 *
 * 证据源原则（2026-09-01 三板基线实测，hw_probe.sh v3/v4 探测结论）：
 *   - 运行期不用 dmesg/logread（环形缓冲会被冲掉，125 板曾因此假 FAIL）
 *   - 传感器运行期证据 = /proc/umap/vi PIPE STATUS FrameRate（传感器真实出帧）
 *   - MMZ 证据 = /proc/media-mem（/proc/umap/mmz 是错误路径）
 *   - I2C 器件探测用 ioctl+read（等效 i2cdetect -y -r；quick-write 模式
 *     探不到 0x20/PCF8574，用户实测）
 *   - L2 层（majestic SDK 状态）混合软硬件：进程在而 SDK 节点缺失才计入，
 *     进程死亡归 6103 管，不重复报警
 *
 * 检测项（板端 /usr/sbin/hw_probe.sh 为全量版，本检测器为其无人值守子集）：
 *   sensor_live    vi FrameRate>0（majestic 运行时）
 *   majestic_sdk   进程在但 /proc/umap/vi 缺失（混合软硬件，计入故障）
 *   mmz            /proc/media-mem 存在（majestic 运行时）
 *   sensor_config  /etc/sensors/imx347*.ini 存在（majestic 误检 imx347 的
 *                  配套 ini，缺失曾致三板推流链路失效）
 *   i2c_bus1       /dev/i2c-1 上 0x20(PCF8574) 与 0x36 应答
 *   audio_hw       /dev/acodec 存在
 *   wlan           c->wifi_iface 接口存在
 *   （SD 卡三态复用 4001/4002/4003，网络连通复用 wifi_watch，不重复）
 *
 * I2C 并发说明：与 majestic 共享总线，但本检测器周期为小时级、单次事务
 * 仅微秒级，且用户已在运行期实测 i2cdetect -r 探测安全，风险可忽略。
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"

/* 内核 uapi 稳定常量，避免依赖交叉工具链的 <linux/i2c-dev.h> */
#define I2C_SLAVE_REQ   0x0703

#define HW_VI_PROC      "/proc/umap/vi"
#define HW_MMZ_PROC     "/proc/media-mem"
#define HW_ACODEC_DEV   "/dev/acodec"
#define HW_SENSOR_INI_A "/etc/sensors/imx347_i2c_4M.ini"
#define HW_SENSOR_INI_B "/etc/sensors/imx347_i2c.ini"
#define HW_I2C_DEV      "/dev/i2c-1"
#define HW_I2C_ADDR_PCF 0x20    /* PCF8574 IO 扩展（LED/按键/关机电路） */
#define HW_I2C_ADDR_AUX 0x36

/* /proc/<pid>/comm 精确匹配（detectors.c 同语义，此为 hw 模块本地实现） */
static bool hw_majestic_alive(void)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	bool alive = false;

	if (!d)
		return false;
	while ((e = readdir(d))) {
		char path[48], comm[64];

		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		if (atoi(e->d_name) == getpid())
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
		if (read_str_file(path, comm, sizeof(comm)) &&
		    !strcmp(comm, "majestic")) {
			alive = true;
			break;
		}
	}
	closedir(d);
	return alive;
}

/* /proc/umap/vi "VI PIPE STATUS" 段数据行的 FrameRate（实测 goke SDK
 * 结构：表头行只有列名，数据行纯数字，第 4 个 token = FrameRate：
 *   PipeID  Enable    IntCnt FrameRate LostFrame  VbFail   Width  Height
 *        0       Y    118558        24         0       0    2592    1520
 * 文件缺失/SDK 未加载返回 -1；解析不到返回 -1。 */
static long hw_vi_framerate(void)
{
	FILE *f = fopen(HW_VI_PROC, "r");
	char line[512];
	bool in_status = false;
	long fr = -1;

	if (!f)
		return -1;
	while (fr < 0 && fgets(line, sizeof(line), f)) {
		unsigned pid;
		char en;
		unsigned long cnt;

		if (!in_status) {
			if (strstr(line, "VI PIPE STATUS"))
				in_status = true;
			continue;
		}
		/* 表头行 %u 解析 "PipeID" 失败 !=4；数据行才可能 ==4 */
		if (sscanf(line, " %u %c %lu %ld", &pid, &en, &cnt, &fr) == 4)
			break;
	}
	fclose(f);
	return fr;
}

/* I2C read 探测（等效 i2cdetect -y -r 的单地址探测）。
 * open_ok=false 表示总线节点本身不可用；应答返回 true。 */
static bool hw_i2c_ack(const char *dev, int addr, bool *open_ok)
{
	int fd = open(dev, O_RDWR);
	unsigned char b;
	bool ack = false;

	*open_ok = false;
	if (fd < 0)
		return false;
	*open_ok = true;
	if (ioctl(fd, I2C_SLAVE_REQ, addr) >= 0 && read(fd, &b, 1) == 1)
		ack = true;
	close(fd);
	return ack;
}

static void hw_fail_append(char *buf, size_t sz, const char *item)
{
	if (buf[0])
		strncat(buf, ";", sz - strlen(buf) - 1);
	strncat(buf, item, sz - strlen(buf) - 1);
}

/*
 * 聚合检测：正常返回 NULL；异常返回 reason（分号连接失败项清单）。
 * confirm=1（周期小时级，瞬时探测直接报，无需多轮确认）。
 */
const char *det_hw_watch(const enc_cfg_t *c, char *reason, size_t rsz)
{
	char fails[256] = "";
	bool maj = hw_majestic_alive();
	bool i2c_open = false;
	bool ack20, ack36;

	/* 开机宽容期：SDK 加载/传感器预热期间 vi 帧率=0、/proc/umap/vi 缺失
	 * 均为瞬态（2026-09-01 板 116 实测开机窗口连发两次 9101 误报）。
	 * uptime < 300s 内不判定；真硬件故障 5min 后仍会被抓到。 */
	{
		char buf[64];
		long up = 0;

		if (read_str_file("/proc/uptime", buf, sizeof(buf))) {
			up = strtol(buf, NULL, 10);
			if (up < 300)
				return NULL;
		}
	}

	/* 1. 传感器/SDK/MMZ —— 仅 majestic 运行时判（进程死亡归 6103） */
	if (maj) {
		long fr = hw_vi_framerate();

		if (fr < 0)
			hw_fail_append(fails, sizeof(fails),
				       "majestic_sdk:vi_missing");
		else if (fr == 0)
			hw_fail_append(fails, sizeof(fails),
				       "sensor_live:framerate_0");
		if (access(HW_MMZ_PROC, F_OK) != 0)
			hw_fail_append(fails, sizeof(fails),
				       "mmz:media_mem_missing");
	}

	/* 2. sensor 配置（与运行态无关，开机即应存在） */
	if (access(HW_SENSOR_INI_A, F_OK) != 0 &&
	    access(HW_SENSOR_INI_B, F_OK) != 0)
		hw_fail_append(fails, sizeof(fails),
			       "sensor_config:imx347_ini_missing");

	/* 3. I2C bus1 器件应答 */
	ack20 = hw_i2c_ack(HW_I2C_DEV, HW_I2C_ADDR_PCF, &i2c_open);
	ack36 = hw_i2c_ack(HW_I2C_DEV, HW_I2C_ADDR_AUX, &i2c_open);
	if (!i2c_open)
		hw_fail_append(fails, sizeof(fails), "i2c:bus_open_fail");
	else if (!ack20 && !ack36)
		hw_fail_append(fails, sizeof(fails),
			       "i2c:no_ack_0x20_0x36");
	else if (!ack20)
		hw_fail_append(fails, sizeof(fails), "i2c:0x20_nack");
	else if (!ack36)
		hw_fail_append(fails, sizeof(fails), "i2c:0x36_nack");

	/* 4. 音频编解码器件节点 */
	if (access(HW_ACODEC_DEV, F_OK) != 0)
		hw_fail_append(fails, sizeof(fails), "audio:acodec_missing");

	/* 5. 无线接口存在性（连通性归 wifi_watch） */
	{
		char wp[80];

		snprintf(wp, sizeof(wp), "/sys/class/net/%s",
			 c->wifi_iface[0] ? c->wifi_iface : "wlan0");
		if (access(wp, F_OK) != 0)
			hw_fail_append(fails, sizeof(fails),
				       "wlan:iface_missing");
	}

	if (!fails[0])
		return NULL;
	snprintf(reason, rsz, "%s", fails);
	return reason;
}
