/*
 * battery.c — i2c-dev 电量/充电（逐条对齐 battery.sh）
 *
 * 关键参数（照 battery.sh / config.sh 默认值）：
 *   库仑计 MAX170xx：/dev/i2c-1，地址 0x36，SMBus 字读 +
 *   字节交换（battery_swap_word）后按寄存器逻辑值换算：
 *     SOC   0x04  百分比 = 交换值 / 256（高字节整数部分），钳位 0..100
 *     VCELL 0x02  12bit 电压：count = 交换值 >> 4，mV = (count*125+50)/100
 *     CRATE 0x16  有符号充放电速率，≥32768 减 65536
 *   充电引脚 PCF8574(0x20)：先读 → 写回 raw|0x07 释放 P0/P1/P2 → sleep 1s
 *   → 回读。CHRG=P0(0x01)/STDBY=P1(0x02) 低有效：
 *     chrg=0,stdby=1 → 充电；chrg=1 → 未充电；0:0 歧义 → 回退 CRATE
 *   CRATE 回退滞回：≥+5 → 充电，≤-5 → 放电，0 附近保持原值
 *   （低压关机已移交 encalertd，本模块不再做关机判定）
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

/* ---- battery.sh 参数默认值 ---- */
#define BATTERY_I2C_BUS              1
#define BATTERY_I2C_ADDR             0x36
#define BATTERY_VCELL_REG            0x02
#define BATTERY_SOC_REG              0x04
#define BATTERY_CRATE_REG            0x16
#define BATTERY_CHARGE_GPIO_I2C_BUS  1
#define BATTERY_CHARGE_GPIO_I2C_ADDR 0x20
#define BATTERY_CHRG_GPIO_MASK       0x01
#define BATTERY_STDBY_GPIO_MASK      0x02
#define BATTERY_PROTECT_GPIO_MASK    0x07
#define BATTERY_CHARGING_THRESHOLD_RAW     5
#define BATTERY_DISCHARGING_THRESHOLD_RAW  (-5)
#define BATTERY_GPIO_SETTLE_SEC     1

/* battery.c 与 led.c 操作同一颗 PCF8574（0x20），共享这把跨模块互斥锁
 * （定义在 led.c，见 led.c 头部注释） */
extern pthread_mutex_t enc_pcf8574_mutex;

/* ------------------------------------------------------------------ */
/* i2c-dev 基础操作                                                     */
/* ------------------------------------------------------------------ */

static int battery_i2c_open(int bus, int addr)
{
	char path[32];
	int fd;

	snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
	fd = open(path, O_RDWR);
	if (fd < 0)
		return -1;
	if (ioctl(fd, I2C_SLAVE, addr) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* SMBus 字读：与 i2cget -y <bus> <addr> <reg> w 完全等价 */
static int battery_read_word(int fd, unsigned char reg, unsigned *out)
{
	union i2c_smbus_data data;
	struct i2c_smbus_ioctl_data args;

	memset(&data, 0, sizeof(data));
	args.read_write = I2C_SMBUS_READ;
	args.command = reg;
	args.size = I2C_SMBUS_WORD_DATA;
	args.data = &data;
	if (ioctl(fd, I2C_SMBUS, &args) < 0)
		return -1;
	*out = data.word;
	return 0;
}

/* battery_swap_word：i2cget 打印的字与寄存器逻辑序高低字节相反，
 * 交换后才是寄存器逻辑值 */
static unsigned battery_swap_word(unsigned w)
{
	return ((w & 0xFFu) << 8) | ((w >> 8) & 0xFFu);
}

/* ------------------------------------------------------------------ */
/* 三项电量读取（照 battery.sh 换算公式）                                */
/* ------------------------------------------------------------------ */

static int battery_read_percent(int *out)
{
	int fd = battery_i2c_open(BATTERY_I2C_BUS, BATTERY_I2C_ADDR);
	unsigned raw, soc;
	int percent;

	if (fd < 0)
		return -1;
	if (battery_read_word(fd, BATTERY_SOC_REG, &raw) != 0) {
		close(fd);
		return -1;
	}
	close(fd);

	/* SOC 为定点数：高字节整数百分比，低字节小数 */
	soc = battery_swap_word(raw);
	percent = (int)(soc / 256);
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;
	*out = percent;
	return 0;
}

static int battery_read_voltage_mv(int *out)
{
	int fd = battery_i2c_open(BATTERY_I2C_BUS, BATTERY_I2C_ADDR);
	unsigned raw, vcell;
	long count, mv;

	if (fd < 0)
		return -1;
	if (battery_read_word(fd, BATTERY_VCELL_REG, &raw) != 0) {
		close(fd);
		return -1;
	}
	close(fd);

	/* VCELL 为 12bit 电压，1 count = 1.25mV */
	vcell = battery_swap_word(raw);
	count = (long)(vcell >> 4);
	mv = (count * 125 + 50) / 100;
	*out = (int)mv;
	return 0;
}

static int battery_read_crate_raw(int *out)
{
	int fd = battery_i2c_open(BATTERY_I2C_BUS, BATTERY_I2C_ADDR);
	unsigned raw;
	int crate;

	if (fd < 0)
		return -1;
	if (battery_read_word(fd, BATTERY_CRATE_REG, &raw) != 0) {
		close(fd);
		return -1;
	}
	close(fd);

	/* CRATE 为有符号速率，正值充电 */
	crate = (int)battery_swap_word(raw);
	if (crate >= 32768)
		crate -= 65536;
	*out = crate;
	return 0;
}

/* ------------------------------------------------------------------ */
/* 充电状态判定                                                         */
/* ------------------------------------------------------------------ */

/* 读 PCF8574 充电引脚：写 1 释放 P0/P1/P2 → sleep 1s → 回读组合判定。
 * 返回 0 = 判定成功（charging 已填充）；1 = 读取失败；2 = 0:0 歧义 */
static int battery_read_charging_gpio(bool *charging)
{
	int fd;
	unsigned char raw, release;
	int chrg, stdby;
	int rc = 1;

	pthread_mutex_lock(&enc_pcf8574_mutex);
	fd = battery_i2c_open(BATTERY_CHARGE_GPIO_I2C_BUS,
			      BATTERY_CHARGE_GPIO_I2C_ADDR);
	if (fd < 0)
		goto out;
	if (read(fd, &raw, 1) != 1) {
		close(fd);
		goto out;
	}
	/* PCF8574 输入脚必须先写 1 再读；释放 P0/P1/P2，
	 * 否则之前的低电平锁存会让 CHRG/STDBY 看起来恒低 */
	release = raw | BATTERY_PROTECT_GPIO_MASK;
	if (write(fd, &release, 1) != 1) {
		close(fd);
		goto out;
	}
	sleep(BATTERY_GPIO_SETTLE_SEC);
	if (read(fd, &raw, 1) != 1) {
		close(fd);
		goto out;
	}
	close(fd);

	chrg = (raw & BATTERY_CHRG_GPIO_MASK) ? 1 : 0;
	stdby = (raw & BATTERY_STDBY_GPIO_MASK) ? 1 : 0;
	/* CHRG/STDBY 均为低有效状态脚，仅使用无歧义组合 */
	if (chrg == 0 && stdby == 1) {
		*charging = true;
		rc = 0;
	} else if (chrg == 1) {
		/* 1:0 / 1:1 → 未充电 */
		*charging = false;
		rc = 0;
	} else {
		rc = 2;		/* 0:0：同时有效不是合法充电状态 */
	}
out:
	pthread_mutex_unlock(&enc_pcf8574_mutex);
	return rc;
}

/* GPIO 不可用/歧义时回退 CRATE（滞回阈值照 bash，0 附近保持原值防抖动） */
static void battery_refresh_charging_state(void)
{
	bool charging = false;
	char prev[16];
	int crate;
	int rc = battery_read_charging_gpio(&charging);

	if (rc == 0) {
		state_set_str("is_charging", charging ? "true" : "false");
		log_msg(ENCM_LOG_DEBUG, "[BATTERY] charge GPIO is_charging=%s",
			charging ? "true" : "false");
		return;
	}
	log_msg(ENCM_LOG_DEBUG,
		"[BATTERY] charge GPIO unavailable/ambiguous rc=%d, fallback to CRATE",
		rc);

	if (battery_read_crate_raw(&crate) != 0) {
		log_msg(ENCM_LOG_DEBUG,
			"[BATTERY] CRATE read failed, keep is_charging");
		return;
	}

	charging = false;
	if (state_get_str("is_charging", prev, sizeof(prev)) &&
	    strcmp(prev, "true") == 0)
		charging = true;
	if (crate >= BATTERY_CHARGING_THRESHOLD_RAW)
		charging = true;
	else if (crate <= BATTERY_DISCHARGING_THRESHOLD_RAW)
		charging = false;
	state_set_str("is_charging", charging ? "true" : "false");
	log_msg(ENCM_LOG_DEBUG, "[BATTERY] CRATE raw=%d is_charging=%s",
		crate, charging ? "true" : "false");
}

/* ------------------------------------------------------------------ */
/* 刷新 state（键名照 state.sh：battery / battery_voltage_mv / is_charging） */
/* ------------------------------------------------------------------ */

int battery_refresh(const enc_cfg_t *c)
{
	int percent = 0, mv = 0;
	int ok = 0;

	(void)c;		/* 低压关机判定已移交 encalertd */

	if (battery_read_percent(&percent) == 0) {
		state_set_int("battery", percent);
		log_msg(ENCM_LOG_DEBUG, "[BATTERY] battery=%d", percent);
		ok++;
	} else {
		log_msg(ENCM_LOG_DEBUG,
			"[BATTERY] read failed, keep battery state");
	}

	if (battery_read_voltage_mv(&mv) == 0) {
		state_set_int("battery_voltage_mv", mv);
		log_msg(ENCM_LOG_DEBUG, "[BATTERY] voltage_mv=%d", mv);
		ok++;
	} else {
		log_msg(ENCM_LOG_DEBUG,
			"[BATTERY] voltage read failed, keep voltage_mv state");
	}

	battery_refresh_charging_state();
	return ok == 2 ? 0 : -1;
}
