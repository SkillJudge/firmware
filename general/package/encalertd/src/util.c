/*
 * util.c — 日志 / 文件读取 / 时钟 / JSON 转义
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "common.h"

static FILE  *g_logfp;
static int    g_verbose;

void log_set_file(const char *path, int verbose)
{
	g_verbose = verbose;
	if (g_logfp) {
		fclose(g_logfp);
		g_logfp = NULL;
	}
	if (path && path[0]) {
		g_logfp = fopen(path, "a");
	}
}

static const char *lvl_tag(int lvl)
{
	switch (lvl) {
	case ENC_LOG_DEBUG: return "DEBUG";
	case ENC_LOG_INFO:  return "INFO ";
	case ENC_LOG_WARN:  return "WARN ";
	default:            return "ERROR";
	}
}

static int syslog_prio(int lvl)
{
	switch (lvl) {
	case ENC_LOG_DEBUG: return LOG_DEBUG;
	case ENC_LOG_INFO:  return LOG_INFO;
	case ENC_LOG_WARN:  return LOG_WARNING;
	default:            return LOG_ERR;
	}
}

void log_msg(int level, const char *fmt, ...)
{
	char ts[32];
	struct timespec tsp;
	struct tm tmv;
	va_list ap;
	char line[1024];

	clock_gettime(CLOCK_REALTIME, &tsp);
	localtime_r(&tsp.tv_sec, &tmv);
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	if (g_logfp) {
		fprintf(g_logfp, "%s.%03ld [%s] %s\n",
			ts, tsp.tv_nsec / 1000000L, lvl_tag(level), line);
		fflush(g_logfp);
	}
	if (level <= ENC_LOG_WARN || g_verbose || !g_logfp) {
		fprintf(stderr, "%s.%03ld [%s] %s\n",
			ts, tsp.tv_nsec / 1000000L, lvl_tag(level), line);
		fflush(stderr);
	}
	openlog("encalertd", LOG_PID | LOG_NDELAY, LOG_DAEMON);
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
	FILE *f = fopen(path, "r");
	size_t n;

	buf[0] = '\0';
	if (!f)
		return false;
	n = fread(buf, 1, sz - 1, f);
	fclose(f);
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
			 buf[n - 1] == ' ' || buf[n - 1] == '\t'))
		buf[--n] = '\0';
	return n > 0;
}

bool read_int_file(const char *path, long *out)
{
	char buf[64];
	const char *p;

	if (!read_str_file(path, buf, sizeof(buf)))
		return false;
	/* 空串视为非法 */
	if (buf[0] == '\0')
		return false;
	/* 任何字母字符（大小写）视为损坏 —— 电池检测 5201 场景会
	 * 注入 "CORRUPTED"（全大写）这类非数字值。
	 */
	for (p = buf; *p; p++) {
		unsigned char ch = (unsigned char)*p;

		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))
			return false;
	}
	*out = strtol(buf, NULL, 10);
	return true;
}

char *json_escape(char *dst, size_t dsz, const char *src)
{
	size_t o = 0;

	for (; *src && o + 7 < dsz; src++) {
		unsigned char ch = (unsigned char)*src;
		if (ch == '"' || ch == '\\') {
			dst[o++] = '\\';
			dst[o++] = ch;
		} else if (ch == '\n') {
			dst[o++] = '\\'; dst[o++] = 'n';
		} else if (ch == '\r') {
			dst[o++] = '\\'; dst[o++] = 'r';
		} else if (ch == '\t') {
			dst[o++] = '\\'; dst[o++] = 't';
		} else if (ch < 0x20) {
			o += (size_t)snprintf(dst + o, dsz - o, "\\u%04x", ch);
		} else {
			dst[o++] = (char)ch;
		}
	}
	dst[o] = '\0';
	return dst;
}
