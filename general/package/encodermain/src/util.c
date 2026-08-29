/*
 * util.c — 日志 / 文件 / 时钟 / shell 工具（encodermain 基础层）
 *
 * 日志格式与 encalertd 对齐：YYYY-MM-DD HH:MM:SS.mmm [LEVEL] msg（毫秒时间戳），
 * 写日志文件 + stderr（level<=WARN 或 verbose）+ syslog；其余为 bash 工具对齐。
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

static FILE *g_logfp;
static int   g_verbose;

void log_set_file(const char *path, int verbose)
{
	g_verbose = verbose;
	if (g_logfp) {
		fclose(g_logfp);
		g_logfp = NULL;
	}
	if (path && path[0])
		g_logfp = fopen(path, "a");
}

static const char *lvl_tag(int lvl)
{
	static const char *tags[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };

	return (lvl >= ENCM_LOG_DEBUG && lvl <= ENCM_LOG_ERROR) ?
	       tags[lvl] : tags[ENCM_LOG_ERROR];
}

static int syslog_prio(int lvl)
{
	static const int prios[] = { LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERR };

	return (lvl >= ENCM_LOG_DEBUG && lvl <= ENCM_LOG_ERROR) ?
	       prios[lvl] : prios[ENCM_LOG_ERROR];
}

void log_msg(int level, const char *fmt, ...)
{
	char ts[32], line[1024], out[1152];
	struct timespec tsp;
	struct tm tmv;
	va_list ap;

	clock_gettime(CLOCK_REALTIME, &tsp);
	localtime_r(&tsp.tv_sec, &tmv);
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	snprintf(out, sizeof(out), "%s.%03ld [%s] %s",
		 ts, tsp.tv_nsec / 1000000L, lvl_tag(level), line);

	if (g_logfp) {
		fprintf(g_logfp, "%s\n", out);
		fflush(g_logfp);
	}
	/* stderr 默认无缓冲，无需 flush；日志未初始化时兜底输出避免丢日志 */
	if (!g_logfp || level <= ENCM_LOG_WARN || g_verbose)
		fprintf(stderr, "%s\n", out);
	openlog("encodermain", LOG_PID | LOG_NDELAY, LOG_DAEMON);
	syslog(syslog_prio(level), "%s", line);
	closelog();
}

long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

bool read_str_file(const char *path, char *buf, size_t sz)
{
	FILE *f;
	size_t n;

	if (!buf || sz == 0)
		return false;
	buf[0] = '\0';
	f = fopen(path, "r");
	if (!f)
		return false;
	n = fread(buf, 1, sz - 1, f);
	fclose(f);
	while (n > 0 && strchr("\n\r \t", buf[n - 1]))
		buf[--n] = '\0';
	return n > 0;
}

bool read_int_file(const char *path, long *out)
{
	char buf[64];
	const char *p;

	if (!read_str_file(path, buf, sizeof(buf)) || buf[0] == '\0')
		return false;
	/* 任何字母字符（大小写）视为损坏内容 */
	for (p = buf; *p; p++)
		if (isalpha((unsigned char)*p))
			return false;
	*out = strtol(buf, NULL, 10);
	return true;
}

/* 原子写：tmp+rename，内容为 value + '\n'（与 bash printf '%s\n' 对齐） */
bool write_str_file(const char *path, const char *val)
{
	char tmp[600];
	FILE *f;

	if (!path || !path[0])
		return false;
	snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
	f = fopen(tmp, "w");
	if (!f)
		return false;
	fprintf(f, "%s\n", val ? val : "");
	if (fflush(f) != 0 || fsync(fileno(f)) != 0)
		goto fail;
	fclose(f);
	if (rename(tmp, path) == 0)
		return true;
fail:
	unlink(tmp);
	return false;
}

/* 递归建目录（mkdir -p 语义），0 成功 */
int dir_ensure(const char *path)
{
	char tmp[512];
	size_t i, len;

	if (!path || !path[0])
		return -1;
	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	while (len > 1 && tmp[len - 1] == '/')
		tmp[--len] = '\0';
	for (i = 1; i <= len; i++) {
		char saved;
		struct stat st;

		if (tmp[i] != '/' && tmp[i] != '\0')
			continue;
		saved = tmp[i];
		tmp[i] = '\0';
		if (mkdir(tmp, 0755) != 0 && errno != EEXIST &&
		    (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode))) {
			tmp[i] = saved;
			return -1;
		}
		tmp[i] = saved;
	}
	return 0;
}

/* shell 单引号转义：value → 'value'，内部 ' → '\'' （对齐 bash shell_quote） */
void shell_quote(char *dst, size_t dsz, const char *src)
{
	size_t o = 0;

	if (!dst || dsz == 0)
		return;
	if (o + 1 < dsz)
		dst[o++] = '\'';
	for (; *src && o + 5 < dsz; src++) {
		if (*src != '\'') {
			dst[o++] = *src;
			continue;
		}
		dst[o++] = '\'';
		dst[o++] = '\\';
		dst[o++] = '\'';
		dst[o++] = '\'';
	}
	if (o + 1 < dsz)
		dst[o++] = '\'';
	dst[o] = '\0';
}

bool pid_alive(long pid)
{
	return pid > 0 && kill((pid_t)pid, 0) == 0;
}

/* pidof 语义：扫描 /proc/<pid>/comm 精确匹配进程名 */
bool proc_running(const char *name)
{
	DIR *d;
	struct dirent *e;
	bool found = false;

	if (!name || !name[0])
		return false;
	d = opendir("/proc");
	if (!d)
		return false;
	while ((e = readdir(d)) != NULL) {
		char comm_path[300], comm[64];	/* /proc/<d_name>/comm 最长 268 */
		const char *p;

		for (p = e->d_name; *p >= '0' && *p <= '9'; p++)
			;
		if (p == e->d_name || *p != '\0')
			continue;
		snprintf(comm_path, sizeof(comm_path), "/proc/%s/comm",
			 e->d_name);
		if (read_str_file(comm_path, comm, sizeof(comm)) &&
		    strcmp(comm, name) == 0) {
			found = true;
			break;
		}
	}
	closedir(d);
	return found;
}

/* 单行小写转义后的 JSON 字符串写入 dst（带引号的字面量），返回 dst */
char *json_escape(char *dst, size_t dsz, const char *src)
{
	size_t o = 0;

	if (!dst || dsz == 0)
		return dst;
	if (o + 1 < dsz)
		dst[o++] = '"';
	for (; *src && o + 7 < dsz; src++) {
		unsigned char ch = (unsigned char)*src;

		if (ch == '"' || ch == '\\') {
			dst[o++] = '\\';
			dst[o++] = ch;
		} else if (ch == '\n' || ch == '\r' || ch == '\t') {
			dst[o++] = '\\';
			dst[o++] = ch == '\n' ? 'n' : ch == '\r' ? 'r' : 't';
		} else if (ch < 0x20) {
			o += (size_t)snprintf(dst + o, dsz - o, "\\u%04x", ch);
		} else {
			dst[o++] = (char)ch;
		}
	}
	if (o + 1 < dsz)
		dst[o++] = '"';
	dst[o] = '\0';
	return dst;
}

/* 追加捕获数据，超出容量时保留尾部（只需要最后一行） */
static void acc_append(char *acc, size_t *alen, size_t cap,
		       const char *data, size_t n)
{
	if (*alen + n > cap) {
		size_t keep = cap / 2 < *alen ? cap / 2 : *alen;
		memmove(acc, acc + *alen - keep, keep);
		*alen = keep;
	}
	memcpy(acc + *alen, data, n);
	*alen += n;
}

/* 取 acc 中最后一行（去掉 \r\n）写入 out */
static void acc_last_line(const char *acc, size_t alen, char *out, size_t osz)
{
	size_t end = alen, start, len;

	if (!out || osz == 0)
		return;
	out[0] = '\0';
	while (end > 0 && (acc[end - 1] == '\n' || acc[end - 1] == '\r'))
		end--;
	for (start = end; start > 0 && acc[start - 1] != '\n'; start--)
		;
	len = end - start < osz - 1 ? end - start : osz - 1;
	memcpy(out, acc + start, len);
	out[len] = '\0';
}

/* fork 子进程：stdout 接管道，stderr 丢弃；成功经 out_fd/out_pid 返回 */
static int spawn_capture(const char *cmd, int *out_fd, pid_t *out_pid)
{
	int fds[2];
	pid_t pid;

	if (pipe(fds) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (pid == 0) {
		int devnull;

		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[1]);
		devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	*out_fd = fds[0];
	*out_pid = pid;
	return 0;
}

/* fork+exec 单命令（sh -c 语义），捕获 stdout 最后一行，超时 SIGKILL。
 * 返回 0 = exit code 0；否则返回 exit code / -1（超时、信号、内部错误） */
int run_cmd(const char *cmd, int timeout_sec, char *out, size_t osz)
{
	char acc[4096], buf[1024];
	int fd, status = 0, rc = -1;
	pid_t pid;
	size_t alen = 0;
	ssize_t n;
	long long deadline;
	bool eof = false, got = false, unknown = false, timed_out = false;

	if (out && osz)
		out[0] = '\0';
	if (!cmd || !cmd[0] || spawn_capture(cmd, &fd, &pid) != 0)
		return -1;

	deadline = now_ms() + (timeout_sec > 0 ? (long long)timeout_sec * 1000LL : 0);
	while (!got && !timed_out) {
		if (!eof) {
			fd_set rf;
			struct timeval tv;
			long long left = timeout_sec > 0 ? deadline - now_ms() : 1000;

			timed_out = timeout_sec > 0 && left <= 0;
			if (timed_out)
				break;
			FD_ZERO(&rf);
			FD_SET(fd, &rf);
			tv.tv_sec = (long)(left / 1000);
			tv.tv_usec = (long)(left % 1000) * 1000;
			/* EINTR/未到超时都重试；无 timeout_sec 时 1s 轮询 */
			if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0) {
				timed_out = timeout_sec > 0 && now_ms() >= deadline;
				continue;
			}
			n = read(fd, buf, sizeof(buf));
			if (n > 0)
				acc_append(acc, &alen, sizeof(acc), buf,
					   (size_t)n);
			else
				eof = true;	/* EOF 或非 EINTR 读错误 */
		}
		{
			pid_t r = waitpid(pid, &status, WNOHANG);

			if (r == pid)
				got = true;
			else if (r < 0)
				unknown = got = true;	/* 退出码未知 */
			else
				usleep(20000);
		}
		timed_out = timeout_sec > 0 && now_ms() >= deadline;
	}
	close(fd);
	if (out && osz)
		acc_last_line(acc, alen, out, osz);
	if (timed_out) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		log_msg(ENCM_LOG_WARN, "run_cmd timeout (%ds): %s", timeout_sec, cmd);
	} else if (got && !unknown && WIFEXITED(status)) {
		rc = WEXITSTATUS(status);
	}
	return rc;
}
