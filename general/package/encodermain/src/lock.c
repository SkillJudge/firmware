/*
 * lock.c — Majestic 配置文件锁（encodermain 基础层）—— 需求 4
 *
 * mkdir 目录锁 <state_dir>/majestic_config.lock，语义严格照 bash common.sh：
 *   - mkdir 成功 → 写 pid 文件，获锁；
 *   - 已存在 → 读 pid；pid 为空睡 1s 再读一次；pid 失活/仍为空 → 抢占清理
 *     （删 pid + rmdir）后立即重试；持有者存活 → 睡 1s 重试直到 deadline；
 *   - 超时返回 -1。
 * 等待语义从 bash 的"有限重试"改为阻塞等待 wait_sec（默认 60，可配）。
 * 释放只允许锁记录的持有者进程（pid 匹配），避免误删他人锁。
 * 进程内并发误用由静态 mutex 挡住；业务串行化仍由 business 层负责。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"

static pthread_mutex_t g_lock_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 解析锁路径：state 目录取 g_app.cfg（未初始化时回落默认路径） */
static void lock_paths(char *state_dir, size_t ssz,
		       char *dir, size_t dsz,
		       char *pidfile, size_t psz)
{
	const char *sdir = ENCM_STATE_DIR;

	if (g_app.cfg.state_dir[0])
		sdir = g_app.cfg.state_dir;
	snprintf(state_dir, ssz, "%s", sdir);
	snprintf(dir, dsz, "%s/majestic_config.lock", sdir);
	if (pidfile && psz)
		snprintf(pidfile, psz, "%s/pid", dir);
}

int majestic_lock_acquire(int wait_sec)
{
	char state_dir[256], dir[320], pidfile[360];
	long long deadline;

	pthread_mutex_lock(&g_lock_mutex);
	lock_paths(state_dir, sizeof(state_dir),
		   dir, sizeof(dir), pidfile, sizeof(pidfile));
	if (dir_ensure(state_dir) != 0) {
		log_msg(ENCM_LOG_ERROR, "majestic lock state dir missing: %s",
			state_dir);
		pthread_mutex_unlock(&g_lock_mutex);
		return -1;
	}

	deadline = now_ms() + (wait_sec > 0 ? (long long)wait_sec * 1000LL : 0);
	for (;;) {
		if (mkdir(dir, 0755) == 0) {
			char pidbuf[32];

			snprintf(pidbuf, sizeof(pidbuf), "%ld", (long)getpid());
			write_str_file(pidfile, pidbuf);
			pthread_mutex_unlock(&g_lock_mutex);
			return 0;
		}
		if (errno != EEXIST) {
			log_msg(ENCM_LOG_ERROR,
				"majestic lock mkdir failed: %s (%s)",
				dir, strerror(errno));
			pthread_mutex_unlock(&g_lock_mutex);
			return -1;
		}
		if (wait_sec <= 0 || now_ms() >= deadline)
			break;

		{
			long owner = 0;
			bool have = read_int_file(pidfile, &owner);

			if (!have) {
				/* pid 未写完：睡 1s 再读一次（bash 语义） */
				sleep(1);
				have = read_int_file(pidfile, &owner);
			}
			if (!have || !pid_alive(owner)) {
				/* 持有者失活/未写 pid：抢占清理后立即重试 */
				unlink(pidfile);
				rmdir(dir);
				continue;
			}
			sleep(1);
		}
	}

	log_msg(ENCM_LOG_ERROR,
		"majestic config lock timeout wait_sec=%d dir=%s",
		wait_sec, dir);
	pthread_mutex_unlock(&g_lock_mutex);
	return -1;
}

void majestic_lock_release(void)
{
	char state_dir[256], dir[320], pidfile[360];
	long owner = 0;

	pthread_mutex_lock(&g_lock_mutex);
	lock_paths(state_dir, sizeof(state_dir),
		   dir, sizeof(dir), pidfile, sizeof(pidfile));
	if (access(dir, F_OK) != 0) {
		pthread_mutex_unlock(&g_lock_mutex);
		return;
	}
	/* 只有 pid 记录的是当前进程时才删除，避免误删别人的锁 */
	if (!read_int_file(pidfile, &owner) || owner != (long)getpid()) {
		pthread_mutex_unlock(&g_lock_mutex);
		return;
	}
	unlink(pidfile);
	rmdir(dir);
	pthread_mutex_unlock(&g_lock_mutex);
}
