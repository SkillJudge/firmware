/*
 * actions.c — 恢复动作执行器：fork/exec /bin/sh <script> '<json_ctx>'
 *
 * 契约见设计文档 §3.2：
 *   - 位置 ${actions_dir}/<name>.sh
 *   - 超时 SIGKILL（默认 30s）
 *   - 成功判据 exit 0
 *   - 标准输出可选一行 JSON，回传给调用方解析计数键值
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

int action_run(const enc_cfg_t *c, const char *name,
	       const char *context_json, char *out, size_t osz)
{
	char script[400];
	int pipefd[2];
	pid_t pid;
	time_t deadline, now;
	bool timed_out = false;
	int status = -1;

	if (out && osz)
		out[0] = '\0';
	snprintf(script, sizeof(script), "%s/%s.sh", c->actions_dir, name);

	if (access(script, X_OK) != 0) {
		log_msg(ENC_LOG_ERROR, "action script missing/not executable: %s",
			script);
		return -1;
	}
	if (pipe(pipefd) < 0)
		return -1;

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		/* 子进程：独立进程组便于超时整组击杀 */
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		setpgid(0, 0);
		execl("/bin/sh", "sh", script, context_json, (char *)NULL);
		_exit(127);
	}

	close(pipefd[1]);
	setpgid(pid, pid);
	deadline = time(NULL) + c->action_timeout_sec;

	{
		/* 带超时的读管道 + 收尸 */
		size_t acc = 0;
		static char accbuf[2048];

		while (1) {
			fd_set rf;
			struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
			ssize_t r;
			int rc;

			now = time(NULL);
			if (now >= deadline)
				break;

			FD_ZERO(&rf);
			FD_SET(pipefd[0], &rf);
			rc = select(pipefd[0] + 1, &rf, NULL, NULL, &tv);
			if (rc > 0 && FD_ISSET(pipefd[0], &rf)) {
				r = read(pipefd[0],
					 accbuf + acc, sizeof(accbuf) - 1 - acc);
				if (r <= 0)
					break;
				acc += (size_t)r;
				accbuf[acc] = '\0';
			} else if (rc == 0) {
				/* 顺便探测退出 */
				rc = waitpid(pid, &status, WNOHANG);
				if (rc == pid)
					break;
			}
		}
		/* 超时处理 */
		now = time(NULL);
		if (now >= deadline) {
			timed_out = true;
			log_msg(ENC_LOG_ERROR,
				"action %s exceeded %ds timeout, killing group",
				name, c->action_timeout_sec);
			kill(-pid, SIGKILL);
			waitpid(pid, &status, 0);
		}

		/* 抽取最后一行输出（脚本约定单行 JSON）。
		 * 注意：echo 输出末尾带 '\n'，strrchr('\n') 会命中末尾空段，
		 * 必须先剥掉尾部所有换行符再反向找分隔符。
		 */
		if (out && osz > 0) {
			size_t tot = acc;
			const char *start = accbuf;
			const char *sep;

			/* 跳过尾部换行符 */
			while (tot > 0 &&
			       (accbuf[tot - 1] == '\n' ||
				accbuf[tot - 1] == '\r'))
				tot--;
			if (tot == 0) {
				out[0] = '\0';
			} else {
				sep = NULL;
				for (size_t i = 0; i < tot; i++)
					if (accbuf[i] == '\n')
						sep = accbuf + i;
				start = sep ? sep + 1 : accbuf;
				size_t len = (size_t)(accbuf + tot - start);
				if (len >= osz)
					len = osz - 1;
				memcpy(out, start, len);
				out[len] = '\0';
			}
		}
	}

	close(pipefd[0]);

	if (!timed_out && waitpid(pid, &status, 0) < 0)
		status = -1;

	if (timed_out)
		return -2;
	if (WIFEXITED(status))
		return WEXITSTATUS(status); /* 0=成功，非 0=恢复失败 */
	return -3;
}

long action_out_num(const char *script_output, const char *key, long fallback)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\":", key);
	p = strstr(script_output ? script_output : "", pat);
	if (!p)
		return fallback;
	p += strlen(pat);
	while (*p == ' ')
		p++;
	return strtol(p, NULL, 10);
}
