/*
 * mini_json.c — 极简 JSON 解析与构建（内嵌，无外部依赖）
 *
 * 覆盖 encodermain 的全部 JSON 需求：
 *   1) 解析命令 payload（{msgId,msg,data:{...}} 两层为主，但实现为通用
 *      递归下降解析器，支持 object/array/string/number/bool/null）；
 *   2) 组装 ACK / 事件 payload（sb_t 动态字符串 builder）。
 * 相比引入 cJSON：零依赖、体积小；足够本项目 payload 尺寸（≤2KB）。
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

/* ------------------------------------------------------------------ */
/* 节点定义                                                             */
/* ------------------------------------------------------------------ */

enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_OBJ, JV_ARR };

struct jv {
    int      type;
    char    *key;       /* 父为 OBJ 时的键（owned） */
    char    *s;         /* JV_STR 值（owned，已反转义）；JV_NUM 用 fmt 存 */
    long long n;        /* JV_BOOL / JV_NUM */
    jv_t    *child;     /* OBJ/ARR 第一个子节点 */
    jv_t    *next;      /* 兄弟节点 */
};

static jv_t *jv_new(int type)
{
	jv_t *v = (jv_t *)calloc(1, sizeof(jv_t));

	if (v)
		v->type = type;
	return v;
}

void jv_free(jv_t *v)
{
	while (v) {
		jv_t *next = v->next;

		free(v->key);
		free(v->s);
		jv_free(v->child);
		free(v);
		v = next;
	}
}

/* ------------------------------------------------------------------ */
/* 解析器                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    bool        ok;
} jp_t;

static void jp_ws(jp_t *j)
{
	while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')
		j->p++;
}

static jv_t *jp_value(jp_t *j);

static char *jp_string_raw(jp_t *j)
{
	/* 进入时 *p == '"' */
	char *buf = NULL;
	size_t cap = 32, len = 0;

	j->p++;
	buf = (char *)malloc(cap);
	if (!buf) {
		j->ok = false;
		return NULL;
	}
	while (*j->p && *j->p != '"') {
		unsigned char ch = (unsigned char)*j->p;

		if (len + 8 >= cap) {
			char *nb;
			cap *= 2;
			nb = (char *)realloc(buf, cap);
			if (!nb)
				goto fail;
			buf = nb;
		}
		if (ch == '\\') {
			j->p++;
			switch (*j->p) {
			case '"': buf[len++] = '"'; break;
			case '\\': buf[len++] = '\\'; break;
			case '/': buf[len++] = '/'; break;
			case 'b': buf[len++] = '\b'; break;
			case 'f': buf[len++] = '\f'; break;
			case 'n': buf[len++] = '\n'; break;
			case 'r': buf[len++] = '\r'; break;
			case 't': buf[len++] = '\t'; break;
			case 'u': {
				unsigned int cp = 0;
				int i;
				for (i = 0; i < 4 && isxdigit((unsigned char)j->p[1]); i++) {
					char c = *++j->p;
					cp = cp * 16 + (unsigned int)(isdigit((unsigned char)c) ?
					     c - '0' : (tolower((unsigned char)c) - 'a' + 10));
				}
				/* UTF-8 编码（BMP 内） */
				if (cp < 0x80) {
					buf[len++] = (char)cp;
				} else if (cp < 0x800) {
					buf[len++] = (char)(0xC0 | (cp >> 6));
					buf[len++] = (char)(0x80 | (cp & 0x3F));
				} else {
					buf[len++] = (char)(0xE0 | (cp >> 12));
					buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
					buf[len++] = (char)(0x80 | (cp & 0x3F));
				}
				break;
			}
			default:
				goto fail;
			}
			if (*j->p)
				j->p++;
		} else {
			buf[len++] = (char)ch;
			j->p++;
		}
	}
	if (*j->p != '"')
		goto fail;
	j->p++;
	buf[len] = '\0';
	return buf;
fail:
	free(buf);
	j->ok = false;
	return NULL;
}

static void obj_append(jv_t *obj, jv_t *item)
{
	jv_t *c = obj->child;

	if (!c) {
		obj->child = item;
		return;
	}
	while (c->next)
		c = c->next;
	c->next = item;
}

static jv_t *jp_value(jp_t *j)
{
	jv_t *v = NULL;

	jp_ws(j);
	switch (*j->p) {
	case '{': {
		v = jv_new(JV_OBJ);
		if (!v)
			goto oom;
		j->p++;
		jp_ws(j);
		if (*j->p == '}') {
			j->p++;
			return v;
		}
		for (;;) {
			jv_t *item;
			jp_ws(j);
			if (*j->p != '"')
				goto fail;
			item = jv_new(JV_NULL);
			if (!item)
				goto oom;
			item->key = jp_string_raw(j);
			if (!item->key) {
				jv_free(item);
				goto fail;
			}
			jp_ws(j);
			if (*j->p != ':') {
				jv_free(item);
				goto fail;
			}
			j->p++;
			{
				jv_t *val = jp_value(j);

				if (!val) {
					jv_free(item);
					goto fail;
				}
				/* 挂链：先接值再入表，保证 val.key 归属 */
				item->type = val->type;
				item->s = val->s;
				item->n = val->n;
				item->child = val->child;
				free(val);
			}
			obj_append(v, item);
			jp_ws(j);
			if (*j->p == ',') {
				j->p++;
				continue;
			}
			if (*j->p == '}') {
				j->p++;
				return v;
			}
			goto fail;
		}
	}
	case '[': {
		v = jv_new(JV_ARR);
		if (!v)
			goto oom;
		j->p++;
		jp_ws(j);
		if (*j->p == ']') {
			j->p++;
			return v;
		}
		for (;;) {
			jv_t *item = jp_value(j);

			if (!item)
				goto fail;
			obj_append(v, item);
			jp_ws(j);
			if (*j->p == ',') {
				j->p++;
				continue;
			}
			if (*j->p == ']') {
				j->p++;
				return v;
			}
			goto fail;
		}
	}
	case '"': {
		v = jv_new(JV_STR);
		if (!v)
			goto oom;
		v->s = jp_string_raw(j);
		if (!v->s)
			goto fail;
		return v;
	}
	case 't':
		if (!strncmp(j->p, "true", 4)) {
			v = jv_new(JV_BOOL);
			if (v)
				v->n = 1;
			j->p += 4;
			return v;
		}
		goto fail;
	case 'f':
		if (!strncmp(j->p, "false", 5)) {
			j->p += 5;
			v = jv_new(JV_BOOL);
			if (v)
				v->n = 0;
			return v;
		}
		goto fail;
	case 'n':
		if (!strncmp(j->p, "null", 4)) {
			j->p += 4;
			return jv_new(JV_NULL);
		}
		goto fail;
	default: {
		char *end;
		long long n = strtoll(j->p, &end, 10);

		if (end == j->p ||
		    (*end != '\0' && *end != ',' && *end != '}' &&
		     *end != ']' && *end != ' ' && *end != '.' &&
		     *end != 'e' && *end != 'E'))
			goto fail;
		v = jv_new(JV_NUM);
		if (!v)
			goto oom;
		v->n = n;
		v->s = NULL;
		/* 保留原始文本（浮点安全直通） */
		{
			size_t l = (size_t)(end - j->p);
			v->s = (char *)malloc(l + 1);
			if (!v->s)
				goto oom;
			memcpy(v->s, j->p, l);
			v->s[l] = '\0';
		}
		j->p = end;
		return v;
	}
	}
oom:
	j->ok = false;
	return NULL;
fail:
	j->ok = false;
	jv_free(v);
	return NULL;
}

jv_t *jv_parse(const char *s)
{
	jp_t j = { s, true };
	jv_t *v;

	if (!s || !s[0])
		return NULL;
	v = jp_value(&j);
	if (!j.ok)
	{
		jv_free(v);
		return NULL;
	}
	return v;
}

/* ------------------------------------------------------------------ */
/* 访问器                                                              */
/* ------------------------------------------------------------------ */

static const jv_t *jv_child(const jv_t *obj, const char *key)
{
	const jv_t *c;

	if (!obj || obj->type != JV_OBJ)
		return NULL;
	for (c = obj->child; c; c = c->next) {
		if (c->key && !strcmp(c->key, key))
			return c;
	}
	return NULL;
}

const jv_t *jv_path(const jv_t *obj, const char *path)
{
	char buf[128];
	const char *dot;
	const jv_t *cur = obj;

	if (!obj || !path)
		return NULL;
	while ((dot = strchr(path, '.')) != NULL) {
		size_t l = (size_t)(dot - path);

		if (l >= sizeof(buf))
			return NULL;
		memcpy(buf, path, l);
		buf[l] = '\0';
		cur = jv_child(cur, buf);
		if (!cur)
			return NULL;
		path = dot + 1;
	}
	return jv_child(cur, path);
}

const char *jv_str(const jv_t *v)
{
	return (v && v->type == JV_STR) ? v->s : NULL;
}

long long jv_int(const jv_t *v, long long def)
{
	if (!v)
		return def;
	if (v->type == JV_NUM)
		return v->n;
	if (v->type == JV_STR && v->s)
		return strtoll(v->s, NULL, 10);
	return def;
}

bool jv_bool(const jv_t *v, bool def)
{
	if (!v)
		return def;
	if (v->type == JV_BOOL)
		return v->n != 0;
	if (v->type == JV_NUM)
		return v->n != 0;
	return def;
}

bool jv_is_num(const jv_t *v)
{
	return v && v->type == JV_NUM;
}

/* ------------------------------------------------------------------ */
/* 字符串 builder                                                       */
/* ------------------------------------------------------------------ */

void sb_init(sb_t *b)
{
	b->cap = 256;
	b->len = 0;
	b->ok = true;
	b->s = (char *)malloc(b->cap);
	if (b->s)
		b->s[0] = '\0';
	else
		b->ok = false;
}

void sb_free(sb_t *b)
{
	free(b->s);
	b->s = NULL;
	b->len = b->cap = 0;
}

static void sb_reserve(sb_t *b, size_t extra)
{
	if (!b->ok)
		return;
	if (b->len + extra + 1 > b->cap) {
		size_t ncap = b->cap * 2;
		char *ns;

		while (b->len + extra + 1 > ncap)
			ncap *= 2;
		ns = (char *)realloc(b->s, ncap);
		if (!ns) {
			b->ok = false;
			return;
		}
		b->s = ns;
		b->cap = ncap;
	}
}

void sb_putc(sb_t *b, char ch)
{
	sb_reserve(b, 1);
	if (!b->ok)
		return;
	b->s[b->len++] = ch;
	b->s[b->len] = '\0';
}

void sb_puts(sb_t *b, const char *s)
{
	size_t l = strlen(s);

	sb_reserve(b, l);
	if (!b->ok)
		return;
	memcpy(b->s + b->len, s, l);
	b->len += l;
	b->s[b->len] = '\0';
}

void sb_fmt(sb_t *b, const char *fmt, ...)
{
	va_list ap;
	int need;
	char tmp[512];

	va_start(ap, fmt);
	need = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (need < 0) {
		b->ok = false;
		return;
	}
	if ((size_t)need < sizeof(tmp)) {
		sb_puts(b, tmp);
		return;
	}
	/* 超长：两段式 */
	{
		char *big = (char *)malloc((size_t)need + 1);

		if (!big) {
			b->ok = false;
			return;
		}
		va_start(ap, fmt);
		vsnprintf(big, (size_t)need + 1, fmt, ap);
		va_end(ap);
		sb_puts(b, big);
		free(big);
	}
}

void sb_json_str(sb_t *b, const char *s)
{
	sb_putc(b, '"');
	for (; *s; s++) {
		unsigned char ch = (unsigned char)*s;

		switch (ch) {
		case '"':  sb_puts(b, "\\\""); break;
		case '\\': sb_puts(b, "\\\\"); break;
		case '\n': sb_puts(b, "\\n"); break;
		case '\r': sb_puts(b, "\\r"); break;
		case '\t': sb_puts(b, "\\t"); break;
		default:
			if (ch < 0x20)
				sb_fmt(b, "\\u%04x", ch);
			else
				sb_putc(b, (char)ch);
		}
	}
	sb_putc(b, '"');
}
