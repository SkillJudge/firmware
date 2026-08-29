/*
 * encdb.c — 单文件 JSONL 数据库（records 上传账本 + alarms）—— 需求 6 / 设计 §7
 *
 * 行格式（EncoderMainDesign.md §7.2，camelCase 键名）：
 *   {"t":"rec","op":"add","file":"..","kind":"record","recordId":"..","taskId":"..",
 *    "seg":N,"size":N,"mtime":N,"state":"pending","retry":N,"nextRetryTs":N,"err":"..","ts":N}
 *   {"t":"rec","op":"state","file":"..","state":"uploaded","retry":N,"nextRetryTs":N,"err":"..","ts":N}
 *   {"t":"rec","op":"del","file":"..","ts":N}
 *   {"t":"alm","code":6201,"type":"upload_failed","level":"error","desc":"..","detail":"..","ts":N}
 *
 * 内存模型：records 用链表（file 为主键）、alarms 用环形数组（上限 db_alarm_max）。
 * 写路径：全部追加到 JSONL 文件 + fsync；compaction 在无待上传任务时 tmp+rename 原子重写。
 * 加载：逐行 jv_parse，损坏尾行（断电截断）按最后一条完整行截断丢弃。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

#define ENCM_DB_COMPACT_PERIOD_SEC (24LL * 3600LL)
#define ENCM_DB_LINE_MAX           2048

/* ------------------------------------------------------------------ */
/* 内存结构                                                             */
/* ------------------------------------------------------------------ */

typedef struct db_rec_node {
	db_rec_t           rec;
	struct db_rec_node *next;
} db_rec_node_t;

typedef struct {
	long long  code;
	char       type[32];
	char       level[16];
	char       desc[128];
	char       detail[512];
	long long  ts;
} db_alm_t;

static db_rec_node_t  *g_recs;          /* records 链表（按插入序，file 主键） */
static db_rec_node_t **g_recs_tail;     /* 尾插指针（指向最后节点的 next） */
static int             g_rec_cnt;
static db_alm_t       *g_alarms;        /* alarms 环形数组（最新在后） */
static int             g_alm_cap;
static int             g_alm_head;
static int             g_alm_cnt;
static FILE           *g_fp;            /* 追加写句柄 */
static char            g_db_file[512];
static int             g_compact_records = 4096;
static long long       g_appends;        /* 距上次 compaction 的追加行数 */
static long long       g_last_compact;
static pthread_mutex_t g_db_mutex = PTHREAD_MUTEX_INITIALIZER;

const char *db_state_name(db_state_t s)
{
	switch (s) {
	case DB_PENDING:      return "pending";
	case DB_UPLOADED:     return "uploaded";
	case DB_FAILED:       return "failed";
	case DB_FAILED_FINAL: return "failed_final";
	case DB_LOST:         return "lost";
	default:              return "pending";
	}
}

static db_state_t state_from_name(const char *s)
{
	if (!s)
		return DB_PENDING;
	if (!strcmp(s, "uploaded"))     return DB_UPLOADED;
	if (!strcmp(s, "failed"))       return DB_FAILED;
	if (!strcmp(s, "failed_final")) return DB_FAILED_FINAL;
	if (!strcmp(s, "lost"))         return DB_LOST;
	return DB_PENDING;
}

static void copy_str(char *dst, size_t sz, const char *src)
{
	snprintf(dst, sz, "%s", src ? src : "");
}

static void parent_dir_ensure(const char *path)
{
	char dir[512];
	char *p;

	snprintf(dir, sizeof(dir), "%s", path);
	p = strrchr(dir, '/');
	if (!p || p == dir)
		return;
	*p = '\0';
	dir_ensure(dir);
}

/* ------------------------------------------------------------------ */
/* records 链表内部操作（调用方持锁）                                    */
/* ------------------------------------------------------------------ */

static db_rec_node_t *rec_find_locked(const char *file)
{
	db_rec_node_t *n;

	for (n = g_recs; n; n = n->next) {
		if (!strcmp(n->rec.file, file))
			return n;
	}
	return NULL;
}

/* 主键 upsert：存在则整体覆盖，不存在则尾插（保持时间序） */
static db_rec_node_t *rec_upsert_locked(const db_rec_t *r)
{
	db_rec_node_t *n = rec_find_locked(r->file);

	if (n) {
		n->rec = *r;
		return n;
	}
	n = (db_rec_node_t *)calloc(1, sizeof(*n));
	if (!n)
		return NULL;
	n->rec = *r;
	n->next = NULL;
	if (g_recs_tail)
		*g_recs_tail = n;
	else
		g_recs = n;
	g_recs_tail = &n->next;
	g_rec_cnt++;
	return n;
}

static void rec_unlink_locked(const char *file)
{
	db_rec_node_t **pp;

	for (pp = &g_recs; *pp; pp = &(*pp)->next) {
		db_rec_node_t *n = *pp;

		if (strcmp(n->rec.file, file))
			continue;
		if (g_recs_tail == &n->next)
			g_recs_tail = pp;
		*pp = n->next;
		free(n);
		g_rec_cnt--;
		return;
	}
}

static int rec_count_state_locked(db_state_t st)
{
	db_rec_node_t *n;
	int cnt = 0;

	for (n = g_recs; n; n = n->next) {
		if (st < 0 || n->rec.state == st)
			cnt++;
	}
	return cnt;
}

/* ------------------------------------------------------------------ */
/* 追加写 + compaction 触发（调用方持锁）                                */
/* ------------------------------------------------------------------ */

static int compact_locked(void);

/* 写路径计数触发：追加行数超阈值或每 24h 且当前无待上传任务时全量重写 */
static void compact_if_idle_locked(void)
{
	long long now = time(NULL);
	bool due;

	if (!g_db_file[0])
		return;
	due = (g_compact_records > 0 && g_appends > g_compact_records) ||
	      (now - g_last_compact >= ENCM_DB_COMPACT_PERIOD_SEC);
	if (!due)
		return;
	/* 设计 §7.3：选在无上传任务时执行，避免与上传关键路径并发 */
	if (rec_count_state_locked(DB_PENDING) > 0 ||
	    rec_count_state_locked(DB_FAILED) > 0 ||
	    rec_count_state_locked(DB_FAILED_FINAL) > 0)
		return;
	if (compact_locked() == 0)
		log_msg(ENCM_LOG_INFO, "db: compaction done records=%d alarms=%d",
			g_rec_cnt, g_alm_cnt);
}

static int db_append_locked(const char *line)
{
	if (!line || !line[0])
		return 0;
	if (!g_fp) {
		g_fp = fopen(g_db_file, "a");
		if (!g_fp)
			return -1;
	}
	fputs(line, g_fp);
	fputc('\n', g_fp);
	fflush(g_fp);
	fsync(fileno(g_fp));    /* 上传状态变更是关键数据，落盘再返回 */
	g_appends++;
	compact_if_idle_locked();
	return 0;
}

static int compact_locked(void)
{
	/* 复用 sb_t：手动清空避免反复 malloc（mini_json 无 sb_reset） */
	char            tmp[640];
	FILE            *f;
	db_rec_node_t   *n;
	int             i;
	sb_t            b;

	snprintf(tmp, sizeof(tmp), "%s.tmp", g_db_file);
	f = fopen(tmp, "w");
	if (!f)
		return -1;
	sb_init(&b);
	for (n = g_recs; n; n = n->next) {
		b.len = 0;
		b.s[0] = '\0';
		sb_puts(&b, "{\"t\":\"rec\",\"op\":\"add\",\"file\":");
		sb_json_str(&b, n->rec.file);
		sb_puts(&b, ",\"kind\":");
		sb_json_str(&b, n->rec.kind);
		sb_puts(&b, ",\"recordId\":");
		sb_json_str(&b, n->rec.record_id);
		sb_puts(&b, ",\"taskId\":");
		sb_json_str(&b, n->rec.task_id);
		sb_fmt(&b, ",\"seg\":%d,\"size\":%lld,\"mtime\":%lld,\"state\":\"%s\",\"retry\":%d,\"nextRetryTs\":%lld,\"err\":",
		       n->rec.seg, n->rec.size, n->rec.mtime,
		       db_state_name(n->rec.state), n->rec.retry,
		       n->rec.next_retry_ts);
		sb_json_str(&b, n->rec.err);
		sb_fmt(&b, ",\"ts\":%lld}", n->rec.ts);
		fputs(b.s, f);
		fputc('\n', f);
	}
	for (i = 0; i < g_alm_cnt; i++) {
		const db_alm_t *a = &g_alarms[(g_alm_head + i) % g_alm_cap];

		b.len = 0;
		b.s[0] = '\0';
		sb_fmt(&b, "{\"t\":\"alm\",\"code\":%lld,\"type\":", a->code);
		sb_json_str(&b, a->type);
		sb_puts(&b, ",\"level\":");
		sb_json_str(&b, a->level);
		sb_puts(&b, ",\"desc\":");
		sb_json_str(&b, a->desc);
		sb_puts(&b, ",\"detail\":");
		sb_json_str(&b, a->detail);
		sb_fmt(&b, ",\"ts\":%lld}", a->ts);
		fputs(b.s, f);
		fputc('\n', f);
	}
	sb_free(&b);
	fflush(f);
	fsync(fileno(f));
	if (ferror(f) || fclose(f) != 0) {
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, g_db_file) != 0) {
		unlink(tmp);
		return -1;
	}
	/* 重新打开追加句柄（旧句柄指向被替换的 inode） */
	if (g_fp)
		fclose(g_fp);
	g_fp = fopen(g_db_file, "a");
	g_appends = 0;
	g_last_compact = time(NULL);
	return 0;
}

/* ------------------------------------------------------------------ */
/* 加载                                                                 */
/* ------------------------------------------------------------------ */

static void str_field(const jv_t *v, const char *key, char *dst, size_t sz)
{
	const char *s = jv_str(jv_path(v, key));

	if (s)
		copy_str(dst, sz, s);
}

/* 解析一行并应用到内存；返回 0 = 有效行 */
static int db_load_line(const char *line)
{
	jv_t        *v;
	const char  *t, *op, *file;

	v = jv_parse(line);
	if (!v)
		return -1;
	t = jv_str(jv_path(v, "t"));
	if (!t) {
		jv_free(v);
		return -1;
	}
	if (!strcmp(t, "rec")) {
		file = jv_str(jv_path(v, "file"));
		op = jv_str(jv_path(v, "op"));
		if (!file || !op) {
			jv_free(v);
			return -1;
		}
		if (!strcmp(op, "add")) {
			db_rec_t r;

			memset(&r, 0, sizeof(r));
			copy_str(r.file, sizeof(r.file), file);
			str_field(v, "kind", r.kind, sizeof(r.kind));
			str_field(v, "recordId", r.record_id, sizeof(r.record_id));
			str_field(v, "taskId", r.task_id, sizeof(r.task_id));
			r.seg = (int)jv_int(jv_path(v, "seg"), 0);
			r.size = jv_int(jv_path(v, "size"), 0);
			r.mtime = jv_int(jv_path(v, "mtime"), 0);
			{
				const char *s = jv_str(jv_path(v, "state"));

				r.state = state_from_name(s);
			}
			r.retry = (int)jv_int(jv_path(v, "retry"), 0);
			r.next_retry_ts = jv_int(jv_path(v, "nextRetryTs"), 0);
			str_field(v, "err", r.err, sizeof(r.err));
			r.ts = jv_int(jv_path(v, "ts"), now_ms());
			rec_upsert_locked(&r);
		} else if (!strcmp(op, "state")) {
			db_rec_node_t *n = rec_find_locked(file);

			if (n) {
				const char *s = jv_str(jv_path(v, "state"));

				if (s)
					n->rec.state = state_from_name(s);
				n->rec.retry = (int)jv_int(jv_path(v, "retry"),
							   n->rec.retry);
				n->rec.next_retry_ts =
					jv_int(jv_path(v, "nextRetryTs"),
					       n->rec.next_retry_ts);
				str_field(v, "err", n->rec.err, sizeof(n->rec.err));
				n->rec.ts = jv_int(jv_path(v, "ts"), now_ms());
			}
		} else if (!strcmp(op, "del")) {
			rec_unlink_locked(file);
		}
	} else if (!strcmp(t, "alm")) {
		db_alm_t a;

		memset(&a, 0, sizeof(a));
		a.code = jv_int(jv_path(v, "code"), 0);
		str_field(v, "type", a.type, sizeof(a.type));
		str_field(v, "level", a.level, sizeof(a.level));
		str_field(v, "desc", a.desc, sizeof(a.desc));
		str_field(v, "detail", a.detail, sizeof(a.detail));
		a.ts = jv_int(jv_path(v, "ts"), now_ms());
		if (g_alarms) {
			if (g_alm_cnt < g_alm_cap) {
				g_alarms[(g_alm_head + g_alm_cnt) % g_alm_cap] = a;
				g_alm_cnt++;
			} else {
				g_alarms[g_alm_head] = a;
				g_alm_head = (g_alm_head + 1) % g_alm_cap;
			}
		}
	}
	jv_free(v);
	return 0;
}

int encdb_open(const enc_cfg_t *c)
{
	FILE    *f;
	char    *line = NULL;
	size_t  cap = 0;
	ssize_t n;
	long long off = 0, good = 0;

	if (!c)
		return -1;
	pthread_mutex_lock(&g_db_mutex);
	if (g_fp) {             /* 已打开 */
		pthread_mutex_unlock(&g_db_mutex);
		return 0;
	}
	snprintf(g_db_file, sizeof(g_db_file), "%s",
		 c->db_file[0] ? c->db_file : ENCM_DB_FILE);
	g_alm_cap = c->db_alarm_max > 0 ? c->db_alarm_max : 512;
	g_compact_records = c->db_compact_records > 0 ? c->db_compact_records : 4096;
	g_alm_head = g_alm_cnt = 0;
	free(g_alarms);
	g_alarms = (db_alm_t *)calloc((size_t)g_alm_cap, sizeof(db_alm_t));
	if (!g_alarms) {
		g_alm_cap = 0;
		pthread_mutex_unlock(&g_db_mutex);
		return -1;
	}
	parent_dir_ensure(g_db_file);
	g_fp = fopen(g_db_file, "a");
	if (!g_fp) {
		log_msg(ENCM_LOG_ERROR, "db: open failed file=%s errno=%d",
			g_db_file, errno);
		pthread_mutex_unlock(&g_db_mutex);
		return -1;
	}
	/* 逐行加载；损坏尾行（断电截断）按最后一条完整行截断丢弃（设计 §7.1） */
	f = fopen(g_db_file, "r");
	if (f) {
		while ((n = getline(&line, &cap, f)) > 0) {
			off += n;
			while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
				line[--n] = '\0';
			if (n == 0 || db_load_line(line) == 0) {
				good = off;
				continue;
			}
			log_msg(ENCM_LOG_WARN, "db: skip corrupt line off=%lld", off);
		}
		fclose(f);
		if (good != off) {
			log_msg(ENCM_LOG_WARN,
				"db: truncate corrupt tail file=%s keep=%lld drop=%lld",
				g_db_file, good, off - good);
			if (truncate(g_db_file, good) != 0)
				log_msg(ENCM_LOG_ERROR, "db: truncate failed errno=%d",
					errno);
		}
	}
	free(line);
	g_appends = 0;
	g_last_compact = time(NULL);
	/* 启动 compaction：加载后重写为干净快照（仅无待上传任务时执行） */
	if (rec_count_state_locked(DB_PENDING) == 0 &&
	    rec_count_state_locked(DB_FAILED) == 0 &&
	    rec_count_state_locked(DB_FAILED_FINAL) == 0)
		compact_locked();
	pthread_mutex_unlock(&g_db_mutex);
	return 0;
}

/* ------------------------------------------------------------------ */
/* records API                                                          */
/* ------------------------------------------------------------------ */

int encdb_rec_add(const db_rec_t *r)
{
	sb_t b;
	int  rc = -1;

	if (!r || !r->file[0])
		return -1;
	pthread_mutex_lock(&g_db_mutex);
	if (rec_upsert_locked(r)) {
		/* file 为主键：存在 → 覆盖内存 + 追加 add 行 */
		sb_init(&b);
		sb_puts(&b, "{\"t\":\"rec\",\"op\":\"add\",\"file\":");
		sb_json_str(&b, r->file);
		sb_puts(&b, ",\"kind\":");
		sb_json_str(&b, r->kind);
		sb_puts(&b, ",\"recordId\":");
		sb_json_str(&b, r->record_id);
		sb_puts(&b, ",\"taskId\":");
		sb_json_str(&b, r->task_id);
		sb_fmt(&b, ",\"seg\":%d,\"size\":%lld,\"mtime\":%lld,\"state\":\"%s\",\"retry\":%d,\"nextRetryTs\":%lld,\"err\":",
		       r->seg, r->size, r->mtime, db_state_name(r->state),
		       r->retry, r->next_retry_ts);
		sb_json_str(&b, r->err);
		sb_fmt(&b, ",\"ts\":%lld}", r->ts);
		rc = db_append_locked(b.s);
		sb_free(&b);
	}
	pthread_mutex_unlock(&g_db_mutex);
	return rc;
}

int encdb_rec_state(const char *file, db_state_t st,
		    const char *err, int retry, long long next_retry_ts)
{
	db_rec_node_t *n;
	sb_t          b;
	int           rc = -1;

	if (!file || !file[0])
		return -1;
	pthread_mutex_lock(&g_db_mutex);
	n = rec_find_locked(file);
	if (n) {
		/* 追加 state 行：状态迁移是关键数据，fsync 持久化 */
		n->rec.state = st;
		n->rec.retry = retry;
		n->rec.next_retry_ts = next_retry_ts;
		copy_str(n->rec.err, sizeof(n->rec.err), err);
		n->rec.ts = now_ms();
		sb_init(&b);
		sb_puts(&b, "{\"t\":\"rec\",\"op\":\"state\",\"file\":");
		sb_json_str(&b, file);
		sb_fmt(&b, ",\"state\":\"%s\",\"retry\":%d,\"nextRetryTs\":%lld,\"err\":",
		       db_state_name(st), retry, next_retry_ts);
		sb_json_str(&b, n->rec.err);
		sb_fmt(&b, ",\"ts\":%lld}", n->rec.ts);
		rc = db_append_locked(b.s);
		sb_free(&b);
	}
	pthread_mutex_unlock(&g_db_mutex);
	return rc;
}

bool encdb_rec_get(const char *file, db_rec_t *out)
{
	db_rec_node_t *n;
	bool          found = false;

	if (!file || !out)
		return false;
	pthread_mutex_lock(&g_db_mutex);
	n = rec_find_locked(file);
	if (n) {
		*out = n->rec;
		found = true;
	}
	pthread_mutex_unlock(&g_db_mutex);
	return found;
}

int encdb_rec_next(int *cursor, db_rec_t *out)
{
	db_rec_node_t *n;
	int           i;

	if (!cursor || !out)
		return 0;
	pthread_mutex_lock(&g_db_mutex);
	n = g_recs;
	for (i = 0; n && i < *cursor; i++)
		n = n->next;
	if (n) {
		*out = n->rec;
		(*cursor)++;
	}
	pthread_mutex_unlock(&g_db_mutex);
	return n ? 1 : 0;
}

int encdb_rec_del(const char *file)
{
	db_rec_node_t **pp;
	sb_t          b;
	int           rc = -1;

	if (!file || !file[0])
		return -1;
	pthread_mutex_lock(&g_db_mutex);
	for (pp = &g_recs; *pp; pp = &(*pp)->next) {
		db_rec_node_t *n = *pp;

		if (strcmp(n->rec.file, file))
			continue;
		/* compaction 时物理清除：内存删除 + 追加 del 行 */
		if (g_recs_tail == &n->next)
			g_recs_tail = pp;
		*pp = n->next;
		free(n);
		g_rec_cnt--;
		sb_init(&b);
		sb_puts(&b, "{\"t\":\"rec\",\"op\":\"del\",\"file\":");
		sb_json_str(&b, file);
		sb_fmt(&b, ",\"ts\":%lld}", now_ms());
		db_append_locked(b.s);
		sb_free(&b);
		rc = 0;
		break;
	}
	pthread_mutex_unlock(&g_db_mutex);
	return rc;
}

int encdb_rec_count(db_state_t st)
{
	int cnt;

	pthread_mutex_lock(&g_db_mutex);
	cnt = rec_count_state_locked(st);
	pthread_mutex_unlock(&g_db_mutex);
	return cnt;
}

/* ------------------------------------------------------------------ */
/* alarms API                                                           */
/* ------------------------------------------------------------------ */

int encdb_alarm(int code, const char *type, const char *level,
		const char *desc, const char *detail_json)
{
	db_alm_t a;
	sb_t     b;

	memset(&a, 0, sizeof(a));
	a.code = code;
	a.ts = now_ms();
	copy_str(a.type, sizeof(a.type), type);
	copy_str(a.level, sizeof(a.level), level ? level : "error");
	copy_str(a.desc, sizeof(a.desc), desc);
	if (detail_json)
		copy_str(a.detail, sizeof(a.detail), detail_json);

	pthread_mutex_lock(&g_db_mutex);
	/* 环形数组：满时覆盖最旧（保留最近 db_alarm_max 条） */
	if (g_alarms) {
		if (g_alm_cnt < g_alm_cap) {
			g_alarms[(g_alm_head + g_alm_cnt) % g_alm_cap] = a;
			g_alm_cnt++;
		} else {
			g_alarms[g_alm_head] = a;
			g_alm_head = (g_alm_head + 1) % g_alm_cap;
		}
	}
	sb_init(&b);
	sb_fmt(&b, "{\"t\":\"alm\",\"code\":%lld,\"type\":", a.code);
	sb_json_str(&b, a.type);
	sb_puts(&b, ",\"level\":");
	sb_json_str(&b, a.level);
	sb_puts(&b, ",\"desc\":");
	sb_json_str(&b, a.desc);
	sb_puts(&b, ",\"detail\":");
	if (a.detail[0] == '{')
		sb_puts(&b, a.detail);      /* 本模块自构造的 JSON 对象直通 */
	else
		sb_json_str(&b, a.detail);
	sb_fmt(&b, ",\"ts\":%lld}", a.ts);
	db_append_locked(b.s);
	sb_free(&b);
	pthread_mutex_unlock(&g_db_mutex);
	return 0;
}

/* ------------------------------------------------------------------ */
/* 导出 / 统计                                                          */
/* ------------------------------------------------------------------ */

/* 极简 CSV 字段转义：含逗号/引号/换行时加引号 */
static void csv_field(FILE *out, const char *s)
{
	const char *p;

	if (!s)
		return;
	if (!strpbrk(s, ",\"\n\r")) {
		fputs(s, out);
		return;
	}
	fputc('"', out);
	for (p = s; *p; p++) {
		if (*p == '"')
			fputs("\"\"", out);
		else
			fputc(*p, out);
	}
	fputc('"', out);
}

int encdb_dump(FILE *out, const char *table)
{
	int records;
	int i;

	if (!out || !table)
		return -1;
	records = !strcmp(table, "records");
	if (!records && strcmp(table, "alarms"))
		return -1;
	pthread_mutex_lock(&g_db_mutex);
	if (records) {
		db_rec_node_t *n;

		fputs("file,kind,recordId,taskId,seg,size,mtime,state,retry,err,ts\n",
		      out);
		for (n = g_recs; n; n = n->next) {
			const db_rec_t *r = &n->rec;

			csv_field(out, r->file);
			fputc(',', out);
			csv_field(out, r->kind);
			fputc(',', out);
			csv_field(out, r->record_id);
			fputc(',', out);
			csv_field(out, r->task_id);
			fprintf(out, ",%d,%lld,%lld,%s,%d,",
				r->seg, r->size, r->mtime,
				db_state_name(r->state), r->retry);
			csv_field(out, r->err);
			fprintf(out, ",%lld\n", r->ts);
		}
	} else {
		fputs("ts,code,type,level,desc,detail\n", out);
		for (i = 0; i < g_alm_cnt; i++) {
			const db_alm_t *a =
				&g_alarms[(g_alm_head + i) % g_alm_cap];

			fprintf(out, "%lld,%lld,", a->ts, a->code);
			csv_field(out, a->type);
			fputc(',', out);
			csv_field(out, a->level);
			fputc(',', out);
			csv_field(out, a->desc);
			fputc(',', out);
			csv_field(out, a->detail);
			fputc('\n', out);
		}
	}
	pthread_mutex_unlock(&g_db_mutex);
	return 0;
}

int encdb_stats(char *buf, size_t sz)
{
	int        cnt[5] = { 0, 0, 0, 0, 0 };
	int        total = 0;
	int        alm_cnt;
	struct stat st;
	long long  bytes = 0;
	db_rec_node_t *n;

	pthread_mutex_lock(&g_db_mutex);
	for (n = g_recs; n; n = n->next) {
		cnt[n->rec.state]++;
		total++;
	}
	alm_cnt = g_alm_cnt;
	pthread_mutex_unlock(&g_db_mutex);
	if (g_db_file[0] && stat(g_db_file, &st) == 0)
		bytes = (long long)st.st_size;
	snprintf(buf, sz,
		 "records=%d pending=%d uploaded=%d failed=%d failed_final=%d lost=%d alarms=%d db_bytes=%lld",
		 total, cnt[DB_PENDING], cnt[DB_UPLOADED], cnt[DB_FAILED],
		 cnt[DB_FAILED_FINAL], cnt[DB_LOST], alm_cnt, bytes);
	return 0;
}

int encdb_compact(void)
{
	int rc;

	pthread_mutex_lock(&g_db_mutex);
	rc = compact_locked();
	pthread_mutex_unlock(&g_db_mutex);
	return rc;
}