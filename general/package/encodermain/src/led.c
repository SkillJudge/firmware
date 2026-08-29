/*
 * led.c — i2c-dev PCF8574 LED 控制 + 上传 token 闪烁（逐条对齐 led.sh）
 *
 * 关键参数（照 led.sh / config.sh 默认值）：
 *   /dev/i2c-1，PCF8574 地址 0x20；只有 3 个 bit 属于 LED：
 *     bit3 LED_MONO_MASK=0x08    单色录像灯
 *     bit4 LED_STREAM_MASK=0x10  双色灯红色推流灯
 *     bit5 LED_UPLOAD_MASK=0x20  双色灯绿色上传灯
 *   其余 bit 一律写 1 释放（LED_RELEASE_MASK = 0xC7），
 *   读-改-写全程持有 I2C 互斥锁。
 *   token 目录 <state_dir>/led_upload_tokens；闪烁线程 1s 周期：
 *     有上传 token → 绿灯 1s 亮 / 1s 灭（LED_UPLOAD_ON/OFF_SEC=1/1）
 *     无 → 空闲同步（推流/录像中灭绿灯，空闲常亮），1s 轮询
 *   结束上传时若闪烁不足 LED_UPLOAD_MIN_BLINK_SEC=8s，token 延迟到时再删。
 *
 * battery.c 也在同一颗 PCF8574 上读充电引脚，两模块共享
 * enc_pcf8574_mutex（定义于此，battery.c extern 引用）。
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

/* ---- config.sh LED 参数默认值 ---- */
#define LED_I2C_BUS              1
#define LED_I2C_ADDR             0x20
#define LED_MONO_MASK            0x08
#define LED_STREAM_MASK          0x10
#define LED_UPLOAD_MASK          0x20
#define LED_CONTROL_MASK         (LED_MONO_MASK | LED_STREAM_MASK | LED_UPLOAD_MASK)
#define LED_RELEASE_MASK         (0xFFu & ~(unsigned)LED_CONTROL_MASK)
#define LED_UPLOAD_ON_SEC        1
#define LED_UPLOAD_OFF_SEC       1
#define LED_UPLOAD_MIN_BLINK_SEC 8
#define LED_IDLE_POLL_SEC        1
#define LED_SLEEP_QUANTUM_MS     100	/* 可中断 sleep 切片粒度 */

/* 跨模块共享：battery.c 读充电引脚前也必须持有该锁 */
pthread_mutex_t enc_pcf8574_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 本模块内部状态 */
static pthread_mutex_t g_led_mu = PTHREAD_MUTEX_INITIALIZER;
static char     g_led_token_dir[300];
static pthread_t g_led_tid;
static bool     g_led_thread_running;
static volatile bool g_led_quit;

static long led_now_sec(void)
{
	return (long)time(NULL);
}

/* ------------------------------------------------------------------ */
/* I2C 读-改-写                                                         */
/* ------------------------------------------------------------------ */

static int led_i2c_read(unsigned char *val)
{
	int fd;
	char path[32];

	snprintf(path, sizeof(path), "/dev/i2c-%d", LED_I2C_BUS);
	fd = open(path, O_RDWR);
	if (fd < 0)
		return -1;
	if (ioctl(fd, I2C_SLAVE, LED_I2C_ADDR) < 0) {
		close(fd);
		return -1;
	}
	if (read(fd, val, 1) != 1) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static int led_i2c_write(unsigned char val)
{
	int fd;
	char path[32];

	snprintf(path, sizeof(path), "/dev/i2c-%d", LED_I2C_BUS);
	fd = open(path, O_RDWR);
	if (fd < 0)
		return -1;
	if (ioctl(fd, I2C_SLAVE, LED_I2C_ADDR) < 0) {
		close(fd);
		return -1;
	}
	if (write(fd, &val, 1) != 1) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/* set_mask/clear_mask 只作用于 LED 三 bit，其余 bit 恒为写 1 释放。
 * 与 bash led_update_bits 公式逐项一致：
 *   next = (((cur & CTRL) | set) & (~clear & CTRL)) | RELEASE */
static int led_update_bits(unsigned set_mask, unsigned clear_mask)
{
	unsigned char cur, next;
	int rc = -1;

	set_mask &= LED_CONTROL_MASK;
	clear_mask &= LED_CONTROL_MASK;

	pthread_mutex_lock(&enc_pcf8574_mutex);
	if (led_i2c_read(&cur) == 0) {
		next = (unsigned char)((((cur & LED_CONTROL_MASK) | set_mask) &
					(~clear_mask & LED_CONTROL_MASK)) |
				       LED_RELEASE_MASK);
		rc = (next == cur) ? 0 : led_i2c_write(next);
	}
	pthread_mutex_unlock(&enc_pcf8574_mutex);

	if (rc != 0)
		log_msg(ENCM_LOG_WARN,
			"[LED] I2C update failed set_mask=0x%02x clear_mask=0x%02x",
			set_mask, clear_mask);
	return rc;
}

/* ------------------------------------------------------------------ */
/* token 目录扫描                                                       */
/* ------------------------------------------------------------------ */

static bool led_is_numeric(const char *s)
{
	if (!s || !s[0])
		return false;
	for (; *s; s++) {
		if (!isdigit((unsigned char)*s))
			return false;
	}
	return true;
}

/* 目录里是否存在 upload_* 常规文件 */
static bool led_has_tokens(void)
{
	DIR *d;
	struct dirent *e;
	bool found = false;

	if (!g_led_token_dir[0])
		return false;
	d = opendir(g_led_token_dir);
	if (!d)
		return false;
	while ((e = readdir(d)) != NULL) {
		char path[512];
		struct stat st;

		if (strncmp(e->d_name, "upload_", 7) != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s",
			 g_led_token_dir, e->d_name);
		if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
			found = true;
			break;
		}
	}
	closedir(d);
	return found;
}

/* 删除内容为纯数字且已到期的 token（"active:N" 非纯数字，不在此回收） */
static void led_prune_expired_tokens(void)
{
	DIR *d;
	struct dirent *e;
	long now = led_now_sec();

	if (!g_led_token_dir[0])
		return;
	d = opendir(g_led_token_dir);
	if (!d)
		return;
	while ((e = readdir(d)) != NULL) {
		char path[512];
		char content[64];
		struct stat st;

		if (strncmp(e->d_name, "upload_", 7) != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s",
			 g_led_token_dir, e->d_name);
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		if (!read_str_file(path, content, sizeof(content)))
			continue;
		if (led_is_numeric(content) && now >= strtol(content, NULL, 10))
			unlink(path);
	}
	closedir(d);
}

/* 启动/复位时清空全部 token */
static void led_clear_tokens(void)
{
	DIR *d;
	struct dirent *e;

	if (!g_led_token_dir[0])
		return;
	d = opendir(g_led_token_dir);
	if (!d)
		return;
	while ((e = readdir(d)) != NULL) {
		char path[512];

		if (strncmp(e->d_name, "upload_", 7) != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s",
			 g_led_token_dir, e->d_name);
		unlink(path);
	}
	closedir(d);
}

/* 空闲指示同步（led_idle_indicator_sync）：无上传任务时，
 * 推流/录像中灭绿灯，空闲常亮绿灯 */
static void led_idle_indicator_sync(void)
{
	char publishing[16], recording[16];
	bool busy = false;

	if (state_get_str("is_publishing", publishing, sizeof(publishing)) &&
	    strcmp(publishing, "true") == 0)
		busy = true;
	if (state_get_str("is_recording", recording, sizeof(recording)) &&
	    strcmp(recording, "true") == 0)
		busy = true;

	if (busy)
		led_update_bits(0, LED_UPLOAD_MASK);
	else
		led_update_bits(LED_UPLOAD_MASK, 0);
}

/* ------------------------------------------------------------------ */
/* 闪烁线程（1s 周期）                                                  */
/* ------------------------------------------------------------------ */

static bool led_sleep_stoppable(int sec)
{
	int loops = sec * 1000 / LED_SLEEP_QUANTUM_MS;

	while (loops-- > 0) {
		if (g_led_quit)
			return true;
		usleep(LED_SLEEP_QUANTUM_MS * 1000);
	}
	return g_led_quit;
}

static void *led_blink_thread(void *arg)
{
	(void)arg;
	log_msg(ENCM_LOG_DEBUG, "[LED] upload blink worker start");

	while (!g_led_quit) {
		led_prune_expired_tokens();
		if (led_has_tokens()) {
			/* 上传中：绿灯按 1s 亮 / 1s 灭节奏闪烁 */
			led_update_bits(LED_UPLOAD_MASK, 0);
			led_sleep_stoppable(LED_UPLOAD_ON_SEC);
			if (g_led_quit)
				break;
			led_update_bits(0, LED_UPLOAD_MASK);
			led_sleep_stoppable(LED_UPLOAD_OFF_SEC);
		} else {
			led_idle_indicator_sync();
			led_sleep_stoppable(LED_IDLE_POLL_SEC);
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                             */
/* ------------------------------------------------------------------ */

int led_init(const enc_cfg_t *c)
{
	pthread_mutex_lock(&g_led_mu);
	if (g_led_thread_running) {
		pthread_mutex_unlock(&g_led_mu);
		return 0;
	}
	pthread_mutex_unlock(&g_led_mu);

	snprintf(g_led_token_dir, sizeof(g_led_token_dir),
		 "%s/led_upload_tokens", c->state_dir);
	if (dir_ensure(g_led_token_dir) != 0) {
		log_msg(ENCM_LOG_ERROR,
			"[LED] create token dir failed: %s", g_led_token_dir);
		return -1;
	}

	/* 对齐 led_runtime_start：清残留 token → 空闲态 → 启动闪烁线程 */
	led_clear_tokens();
	led_update_bits(LED_UPLOAD_MASK, LED_MONO_MASK | LED_STREAM_MASK);

	g_led_quit = false;
	if (pthread_create(&g_led_tid, NULL, led_blink_thread, NULL) != 0) {
		log_msg(ENCM_LOG_ERROR,
			"[LED] create blink thread failed errno=%d", errno);
		return -1;
	}
	pthread_mutex_lock(&g_led_mu);
	g_led_thread_running = true;
	pthread_mutex_unlock(&g_led_mu);
	return 0;
}

void led_upload_token_set(const char *tag, bool on)
{
	char path[512];
	char content[64];

	if (!tag || !tag[0] || !g_led_token_dir[0])
		return;
	/* tag 拼进文件名，先挡住路径注入 */
	if (strchr(tag, '/') || strchr(tag, '\\') ||
	    strstr(tag, "..") != NULL) {
		log_msg(ENCM_LOG_WARN, "[LED] invalid upload token tag: %s", tag);
		return;
	}
	snprintf(path, sizeof(path), "%s/upload_%s", g_led_token_dir, tag);

	if (on) {
		/* 记 active:<now>，最小闪烁由结束路径保证 */
		snprintf(content, sizeof(content), "active:%ld", led_now_sec());
		if (!write_str_file(path, content)) {
			log_msg(ENCM_LOG_WARN,
				"[LED] upload token create failed path=%s", path);
			return;
		}
		/* 立即点亮，不等闪烁线程下一轮 */
		led_update_bits(LED_UPLOAD_MASK, 0);
		return;
	}

	/* 结束：闪烁不足 MIN_BLINK 时改写释放时间，交给线程到时回收 */
	{
		char state[64];
		long release_at = led_now_sec();

		if (read_str_file(path, state, sizeof(state))) {
			if (strncmp(state, "active:", 7) == 0 &&
			    led_is_numeric(state + 7)) {
				release_at = strtol(state + 7, NULL, 10) +
					     LED_UPLOAD_MIN_BLINK_SEC;
			} else if (led_is_numeric(state)) {
				release_at = strtol(state, NULL, 10);
			}
		}
		if (release_at <= led_now_sec())
			unlink(path);
		else {
			snprintf(content, sizeof(content), "%ld", release_at);
			write_str_file(path, content);
		}
	}

	led_prune_expired_tokens();
	if (!led_has_tokens())
		led_idle_indicator_sync();
}

void led_shutdown(void)
{
	pthread_t tid;
	bool join_needed = false;

	pthread_mutex_lock(&g_led_mu);
	if (g_led_thread_running) {
		g_led_quit = true;
		tid = g_led_tid;
		g_led_thread_running = false;
		join_needed = true;
	}
	pthread_mutex_unlock(&g_led_mu);

	if (join_needed)
		pthread_join(tid, NULL);
}
