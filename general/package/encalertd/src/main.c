/*
 * main.c — encalertd 入口：参数解析、守护化、信号、调度主循环
 *
 * 用法：
 *   encalertd [-c conf] [-b] [-t] [-V]
 *     -c  配置文件路径（默认 /etc/encalertd.conf）
 *     -b  后台守护化运行（init.d 启动用）
 *     -t  自检模式：全部检测器跑一轮打印结果后退出，不发 MQTT
 *     -V  调试日志
 *
 * 调度模型：单线程，每轮检查到期的检测器 → 刷新 spool 补发 → 有界睡眠。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

#define PID_FILE "/var/run/encalertd.pid"

static enc_cfg_t                g_cfg;
static volatile sig_atomic_t    g_stop;

/* ------------------------------------------------------------------ */

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

/* 带错误上下文的 raise：对 fire_confirmed_fail / fire_recovered / 1101 写结构化 detail */
static void fire_confirmed_fail(det_t *d, const char *reason);
static void fire_recovered(det_t *d);
static void build_fail_detail(char *buf, size_t sz,
			      const det_t *d, const char *reason)
{
	char esc_r[256], esc_n[64];

	json_escape(esc_r, sizeof(esc_r), reason ? reason : "");
	json_escape(esc_n, sizeof(esc_n), d->name);
	snprintf(buf, sz,
		 "{\"detector\":\"%s\",\"streak\":%u,\"confirmAt\":%u,"
		 "\"reason\":\"%s\"}",
		 esc_n, (unsigned)(d->fail_streak + 1),
		 (unsigned)d->confirm_cnt, esc_r);
}

static void build_recover_detail(char *buf, size_t sz, const det_t *d)
{
	char esc_n[64];

	json_escape(esc_n, sizeof(esc_n), d->name);
	snprintf(buf, sz, "{\"detector\":\"%s\"}", esc_n);
}

static void install_signals(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	sa.sa_handler = SIG_IGN;
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);
}

static void daemonize(void)
{
	pid_t pid = fork();

	if (pid < 0) {
		log_msg(ENC_LOG_ERROR, "fork failed: %s", strerror(errno));
		exit(1);
	}
	if (pid > 0)
		_exit(0);

	setsid();
	if (fork() > 0)
		_exit(0);
	chdir("/");
	umask(022);
	{
		int fd = open("/dev/null", O_RDWR);

		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > STDERR_FILENO)
				close(fd);
		}
	}
}

static void write_pidfile(void)
{
	FILE *f = fopen(PID_FILE, "w");

	if (f) {
		fprintf(f, "%d\n", (int)getpid());
		fclose(f);
	}
}

/* 确认故障 → 组 desc（desc_fmt 的 %s 填 reason）并 raise + 回调 */
static void fire_confirmed_fail(det_t *d, const char *reason)
{
	alert_def_t *a = &d->alert_fail;
	char desc[460];
	char detail[512];

	if (!a->code)                 /* 事件型检测器不经此管道 */
		return;
	if (strstr(a->desc_fmt, "%s"))
		snprintf(desc, sizeof(desc), a->desc_fmt, reason ? reason : "");
	else
		snprintf(desc, sizeof(desc), "%s", a->desc_fmt);

	build_fail_detail(detail, sizeof(detail), d, reason);
	alert_raise(&g_cfg, a->code, a->type, a->level, desc, detail);
	log_msg(ENC_LOG_WARN, "ALERT %d %s (%s): %s",
		a->code, a->type, a->level, desc);

	if (d->on_confirmed)
		d->on_confirmed(&g_cfg, d, reason);
}

static void fire_recovered(det_t *d)
{
	const alert_def_t *a = &d->alert_recover;
	char desc[460];
	char detail[512];

	if (!a->code)
		return;
	if (strstr(a->desc_fmt, "%s"))
		snprintf(desc, sizeof(desc), a->desc_fmt, "-");
	else
		snprintf(desc, sizeof(desc), "%s", a->desc_fmt);
	build_recover_detail(detail, sizeof(detail), d);
	alert_raise(&g_cfg, a->code, a->type, a->level, desc, detail);
	log_msg(ENC_LOG_INFO, "RECOVERED %d %s", a->code, a->type);
}

/* MQTT 连续失败 ≥4 次且未恢复 → 合成 1101（带失败分类，5 分钟节流） */
static void check_broker_1101(void)
{
	static uint32_t last_fire;
	int cnt;
	char kind[20];

	if (!alert_pub_failed_recently(&g_cfg, &cnt, kind, sizeof(kind)))
		return;
	if (cnt < 4)
		return;
	{
		uint32_t now = (uint32_t)time(NULL);
		char detail[64];

		if (last_fire && now - last_fire < 300)
			return;
		last_fire = now;
		snprintf(detail, sizeof(detail), "{\"kind\":\"%s\"}",
			 kind[0] ? kind : "tcp");
		alert_raise(&g_cfg, 1101, "mqtt_broker_unreachable", "error",
			    "MQTT 连续发布失败，broker 可能不可达", detail);
	}
}

/* ---------------- 自检模式 ---------------- */

static void selftest(void)
{
	printf("== encalertd selftest ==\n");
	printf("device_id    : %s\n", device_id_get(&g_cfg));
	printf("mqtt         : %s:%d user=%s qos=%d\n",
	       g_cfg.mqtt_host[0] ? g_cfg.mqtt_host : "(empty)",
	       g_cfg.mqtt_port,
	       g_cfg.mqtt_user[0] ? "***" : "(none)",
	       g_cfg.mqtt_qos);
	printf("state_dir    : %s\n", g_cfg.state_dir);
	printf("spool_dir    : %s\n", g_cfg.spool_dir);
	printf("actions_dir  : %s\n\n", g_cfg.actions_dir);

	for (det_t *head = detectors_registry(); head; head = head->next) {
		char reason[256];

		if (!head->enabled(&g_cfg)) {
			printf("[DISABLED] %-16s\n", head->name);
			continue;
		}
		reason[0] = '\0';
		const char *r = head->check(&g_cfg, reason, sizeof(reason));
		if (r == NULL)
			printf("[PASS]     %-16s\n", head->name);
		else
			printf("[FAIL]     %-16s %s (streak→%d, "
			       "confirm at %d)\n",
			       head->name, r, head->fail_streak + 1,
			       head->confirm_cnt);
		head->next_due = time(NULL) + head->every_sec;
		head->fail_streak = 0;
		head->ok_streak   = 0;
	}
	printf("\n(selftest finished, no alerts were published)\n");
}

/* ---------------- 主循环 ---------------- */

static int run_loop(void)
{
	while (!g_stop) {
		time_t now = time(NULL);

		for (det_t *d = detectors_registry(); d; d = d->next) {
			char reason[256];

			if (!d->enabled(&g_cfg))
				continue;
			if (now < d->next_due)
				continue;

			reason[0] = '\0';
			const char *r = d->check(&g_cfg, reason,
						 sizeof(reason));
			if (r != NULL) {
				d->fail_streak++;
				d->ok_streak = 0;
				log_msg(g_cfg.log_verbose ? ENC_LOG_DEBUG :
							  ENC_LOG_INFO,
					"[%s] anomaly #%d: %s",
					d->name, d->fail_streak, r);
				if (!d->failing &&
				    d->fail_streak >= d->confirm_cnt) {
					d->failing = true;
					fire_confirmed_fail(d, r);
				}
			} else {
				d->ok_streak++;
				d->fail_streak = 0;
				if (d->failing &&
				    d->ok_streak >= d->recover_cnt) {
					d->failing  = false;
					d->ok_streak = 0;
					fire_recovered(d);
				}
			}
			d->next_due = time(NULL) + d->every_sec;
		}

		alert_flush_pending(&g_cfg);
		check_broker_1101();

		/* 有界睡眠：最小到期时间驱动，最多睡 5s，逐秒可打断 */
		for (int i = 0; i < 5 && !g_stop; i++)
			sleep(1);
	}

	unlink(PID_FILE);
	return 0;
}

/* ---------------- 入口 ---------------- */

int main(int argc, char **argv)
{
	const char *conf_path = ENC_DEFAULT_CONF;
	bool background = false, self_test = false;
	int opt;

	while ((opt = getopt(argc, argv, "c:btVh")) != -1) {
		switch (opt) {
		case 'c': conf_path = optarg; break;
		case 'b': background = true; break;
		case 't': self_test = true; break;
		case 'V': g_cfg.log_verbose = true; break;
		default:
			fprintf(stderr,
				"usage: encalertd [-c conf] [-b|-t] [-V]\n");
			return 2;
		}
	}

	cfg_load(&g_cfg, conf_path);

	if (background)
		daemonize();
	log_set_file(g_cfg.log_file, g_cfg.log_verbose);

	install_signals();

	/* 配置驱动的动态覆盖 */
	for (det_t *d = detectors_registry(); d; d = d->next)
		if (!strcmp(d->name, "low_battery"))
			d->confirm_cnt = g_cfg.low_batt_confirm;

	write_pidfile();
	device_id_get(&g_cfg);              /* 预热并记录解析结果 */
	log_msg(ENC_LOG_INFO, "encalertd started pid=%d devid=%s mqtt=%s:%d "
			  "state=%s actions=%s",
		(int)getpid(), device_id_get(&g_cfg),
		g_cfg.mqtt_host[0] ? g_cfg.mqtt_host : "(unset)",
		g_cfg.mqtt_port, g_cfg.state_dir, g_cfg.actions_dir);

	alert_init(&g_cfg);

	if (self_test) {
		selftest();
		unlink(PID_FILE);
		return 0;
	}

	return run_loop();
}
