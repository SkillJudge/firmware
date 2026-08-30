/*
 * main.c — encodermain 入口
 *
 * 流程（对齐 bash encoder_main.sh）：
 *   参数 → 配置 → state/encdb 初始化 → 外设初始化（LED/电池/语音）
 *   → 启动恢复 Majestic 默认 profile → 常驻 MQTT → 命令 worker
 *   → register 重试循环（10s 等 ACK / 5s 重试）→ 心跳+上传线程
 *   → 守护主循环（Majestic 存活恢复 / duration 到期 / 低压关机兜底）
 *   → SIGTERM 清理（恢复 profile → 退出）
 *
 * 独立运行模式（不进守护流程）：
 *   -V 版本   -t 自检   -d [records|alarms] 数据库导出
 *   --purge [hours] 只删已上传超龄录像（对齐 encalertd record_purge.sh）
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"

/* 全局上下文定义（common.h extern app_t g_app） */
app_t g_app;

static void on_signal(int sig)
{
	(void)sig;
	/* volatile bool 写入是唯一动作，信号安全 */
	g_app.stopping = true;
}

static void on_mq_event(void *ud, mq_event_t ev)
{
	(void)ud;
	/* 设计 §7.4：MQTT 重连成功 = 网络恢复信号 → 触发断网重传扫描 */
	if (ev == MQEV_CONNECTED && g_app.cfg.upload_rescan_on_reconnect)
		upload_kick();
}

/* ------------------------------------------------------------------ */
/* 单实例 / 守护化                                                      */
/* ------------------------------------------------------------------ */

static void pidfile_path(char *out, size_t sz)
{
	snprintf(out, sz, "%s/encodermain.pid", g_app.cfg.state_dir);
}

static int single_instance_lock(void)
{
	char path[300];
	char buf[32];
	long pid = 0;
	FILE *fp;

	pidfile_path(path, sizeof(path));
	if (read_str_file(path, buf, sizeof(buf)) && buf[0]) {
		pid = strtol(buf, NULL, 10);
		if (pid > 0 && pid_alive(pid)) {
			log_msg(ENCM_LOG_ERROR,
				"another encodermain running pid=%ld", pid);
			return -1;
		}
	}
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp, "%ld\n", (long)getpid());
	fclose(fp);
	return 0;
}

static void single_instance_release(void)
{
	char path[300];

	pidfile_path(path, sizeof(path));
	unlink(path);
}

static int daemonize(void)
{
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid > 0)
		_exit(0);       /* 父进程退出 */
	if (setsid() < 0)
		return -1;
	signal(SIGHUP, SIG_IGN);
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid > 0)
		_exit(0);
	chdir("/");
	/* stdio 重定向：错误/告警进日志文件，其余进 /dev/null */
	{
		int fd = open("/dev/null", O_RDWR);

		if (fd >= 0) {
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
			if (fd > 2)
				close(fd);
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* 守护主循环                                                           */
/* ------------------------------------------------------------------ */

/* Majestic 存活守护（bash 守护循环 2s 周期同语义） */
static void majestic_recover(void)
{
	char cmd[300];

	if (proc_running("majestic"))
		return;
	log_msg(ENCM_LOG_WARN, "majestic not running, trying to start");
	if (majestic_lock_acquire(g_app.cfg.majestic_lock_wait_sec) != 0) {
		log_msg(ENCM_LOG_ERROR, "majestic recover: lock timeout");
		return;
	}
	snprintf(cmd, sizeof(cmd), "%s start", g_app.cfg.majestic_init);
	if (run_cmd(cmd, 30, NULL, 0) != 0)
		log_msg(ENCM_LOG_ERROR, "majestic start failed via %s",
			g_app.cfg.majestic_init);
	majestic_lock_release();
}

static void *heartbeat_thread(void *arg)
{
	(void)arg;
	log_msg(ENCM_LOG_INFO, "heartbeat thread started");
	while (!g_app.stopping) {
		int left = g_app.cfg.heartbeat_sec;

		while (left-- > 0 && !g_app.stopping)
			sleep(1);
		if (g_app.stopping)
			break;
		battery_refresh(&g_app.cfg);
		if (dispatch_send_heartbeat(&g_app.cfg) != 0)
			log_msg(ENCM_LOG_WARN, "heartbeat publish failed");
	}
	log_msg(ENCM_LOG_INFO, "heartbeat thread exit");
	return NULL;
}

/* 守护主循环（主线程） */
static void supervisor_loop(void)
{
	while (!g_app.stopping) {
		int left = 2;

		while (left-- > 0 && !g_app.stopping)
			sleep(1);
		if (g_app.stopping)
			break;
		majestic_recover();
		feat_duration_check();
	}
}

/* ------------------------------------------------------------------ */
/* register 重试循环（bash service_register 同语义）                     */
/* ------------------------------------------------------------------ */

static int register_loop(void)
{
	while (!g_app.stopping) {
		char data[1024];
		int msgid = dispatch_send_register(&g_app.cfg);
		int left;

		if (msgid > 0 &&
		    dispatch_wait_register_ack(msgid, 10000, data,
					       sizeof(data))) {
			pthread_mutex_lock(&g_app.rt_mutex);
			rt_apply_register_ack(&g_app.cfg, &g_app.rt, data);
			pthread_mutex_unlock(&g_app.rt_mutex);
			log_msg(ENCM_LOG_INFO, "registered, runtime applied");
			return 0;
		}
		if (g_app.stopping)
			return -1;
		/* 5s 重试（切片响应停止信号） */
		left = 5;
		while (left-- > 0 && !g_app.stopping)
			sleep(1);
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* 独立模式                                                             */
/* ------------------------------------------------------------------ */

static int mode_db_dump(const char *table)
{
	if (encdb_open(&g_app.cfg) != 0) {
		fprintf(stderr, "encdb open failed: %s\n", g_app.cfg.db_file);
		return 1;
	}
	if (!table || !table[0] || !strcmp(table, "records"))
		return encdb_dump(stdout, "records");
	if (!strcmp(table, "alarms"))
		return encdb_dump(stdout, "alarms");
	fprintf(stderr, "unknown table '%s' (records|alarms)\n", table);
	return 2;
}

static int mode_purge(int hours)
{
	char out[256];

	if (encdb_open(&g_app.cfg) != 0) {
		fprintf(stderr, "encdb open failed: %s\n", g_app.cfg.db_file);
		return 1;
	}
	if (purge_run(&g_app.cfg, hours, out, sizeof(out)) != 0) {
		fprintf(stderr, "purge failed\n");
		return 1;
	}
	printf("%s\n", out);
	return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

static void usage(void)
{
	printf("encodermain %s — 编码器主控进程（C 版）\n\n"
	       "用法: encodermain [选项]\n"
	       "  -c <conf>     配置文件（默认 %s）\n"
	       "  -b            后台守护运行\n"
	       "  -t            自检（不发 MQTT 业务，退出码 0=全部通过）\n"
	       "  -d [table]    数据库导出 records|alarms（默认 records）\n"
	       "  --purge [h]   清理已上传且超龄(默认 24h)的录像/抓拍文件\n"
	       "  -V            打印版本\n",
	       ENCM_VERSION, ENCM_DEFAULT_CONF);
}

int main(int argc, char **argv)
{
	const char *conf = ENCM_DEFAULT_CONF;
	const char *dump_table = NULL;
	bool bg = false, selftest = false;
	int purge_hours = -1;
	pthread_t heartbeat_tid, upload_tid;
	bool heartbeat_started = false, upload_started = false;
	int opt;
	int rc = 0;
	struct option longopts[] = {
		{ "purge", optional_argument, NULL, 'p' },
		{ NULL, 0, NULL, 0 },
	};

	memset(&g_app, 0, sizeof(g_app));
	while ((opt = getopt_long(argc, argv, "c:btVd::", longopts,
				  NULL)) != -1) {
		switch (opt) {
		case 'c': conf = optarg; break;
		case 'b': bg = true; break;
		case 't': selftest = true; break;
		case 'V':
			printf("encodermain %s\n", ENCM_VERSION);
			return 0;
		case 'd':
			dump_table = optarg ? optarg : "records";
			break;
		case 'p':
			purge_hours = optarg ? atoi(optarg) : 24;
			break;
		default:
			usage();
			return 2;
		}
	}
	/* --purge 与 getopt_long：optional_argument 需紧贴（--purge=24），
	 * 裸 --purge 取下一个参数（若为数字） */
	if (purge_hours < 0 && optind < argc && argv[optind] &&
	    argv[optind][0] >= '0' && argv[optind][0] <= '9')
		purge_hours = atoi(argv[optind++]);

	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	/* 1. 配置 */
	if (cfg_load(&g_app.cfg, conf) != 0) {
		fprintf(stderr, "config load failed\n");
		return 1;
	}
	snprintf(g_app.cfg.conf_path, sizeof(g_app.cfg.conf_path), "%s",
		 conf);
	pthread_mutex_init(&g_app.rt_mutex, NULL);

	/* 2. 独立模式（自检/导出/purge 不进守护流程，不受 -b 影响） */
	if (selftest) {
		log_set_file(NULL, 1);
		return selftest_run(&g_app.cfg) == 0 ? 0 : 1;
	}
	if (dump_table)
		return mode_db_dump(dump_table);
	if (purge_hours >= 0)
		return mode_purge(purge_hours);

	/* 3. 守护化 + 日志 */
	if (bg && daemonize() != 0) {
		fprintf(stderr, "daemonize failed\n");
		return 1;
	}
	log_set_file(g_app.cfg.log_file, g_app.cfg.log_verbose);
	log_msg(ENCM_LOG_INFO, "=== encodermain %s start (pid=%ld) ===",
		ENCM_VERSION, (long)getpid());

	/* 4. 单实例 */
	if (single_instance_lock() != 0)
		return 1;

	/* 5. 基础初始化（顺序对齐 bash：state → 外设 → 恢复默认档 → MQTT） */
	state_init(&g_app.cfg);
	rt_load(&g_app.cfg, &g_app.rt);
	if (encdb_open(&g_app.cfg) != 0)
		log_msg(ENCM_LOG_WARN, "encdb open failed: %s",
			g_app.cfg.db_file);
	led_init(&g_app.cfg);
	if (battery_refresh(&g_app.cfg) != 0)
		log_msg(ENCM_LOG_WARN, "battery refresh failed (i2c?)");
	if (voice_init(&g_app.cfg) != 0)
		log_msg(ENCM_LOG_WARN,
			"voice init failed (audio output not ready?)");
	feat_restore_startup_media(&g_app.cfg);

	/* 6. MQTT 常驻客户端 + 命令 worker */
	g_app.mq = mq_start(&g_app.cfg, dispatch_mq_cb, on_mq_event,
			    NULL);
	if (!g_app.mq) {
		log_msg(ENCM_LOG_ERROR, "mqtt client start failed");
		rc = 1;
		goto out;
	}
	if (dispatch_init() != 0) {
		log_msg(ENCM_LOG_ERROR, "dispatch init failed");
		rc = 1;
		goto out;
	}

	/* 7. register 重试循环 */
	if (register_loop() != 0)
		goto stopped_early;

	/* 8. 心跳 + 上传线程 */
	if (pthread_create(&heartbeat_tid, NULL, heartbeat_thread,
			   NULL) == 0)
		heartbeat_started = true;
	if (pthread_create(&upload_tid, NULL, upload_thread, NULL) == 0)
		upload_started = true;
	dispatch_send_heartbeat(&g_app.cfg);    /* 注册后立即一跳 */

	/* 9. 守护主循环（阻塞至 SIGTERM） */
	supervisor_loop();

stopped_early:
	/* 10. 清理（bash exit_handler 同语义：恢复默认档 → 退出） */
	log_msg(ENCM_LOG_INFO, "shutting down");
	dispatch_shutdown();
	if (heartbeat_started)
		pthread_join(heartbeat_tid, NULL);
	if (upload_started)
		pthread_join(upload_tid, NULL);
	led_shutdown();
	feat_restore_startup_media(&g_app.cfg);
out:
	if (g_app.mq)
		mq_stop(g_app.mq);
	single_instance_release();
	log_msg(ENCM_LOG_INFO, "=== encodermain exit ===");
	return rc;
}
