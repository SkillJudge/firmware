/*
 * voice.c — Majestic 语音 init/ready/desk（逐条对齐 voice.sh）
 *
 * 关键参数（照 voice.sh / config.sh 默认值）：
 *   播放接口   http://127.0.0.1/play_audio（GET 200 = 就绪）
 *   PCM        /root/resources/desk_8k.pcm（cfg.voice_pcm）
 *   重复次数   VOICE_REPEAT_COUNT=3
 *   节奏       单次预留 VOICE_AUDIO_DURATION_SEC=4s + 间隔 VOICE_GAP_SEC=2s
 *   init       ready 未就绪 → Majestic 锁内写 6 个 .audio.* 键 → killall -HUP
 *              → 等 2s 探测；最多 VOICE_RELOAD_RETRY_COUNT=3 轮，轮间多睡 3s
 *   desk 互斥  同 task_id 播放中 → 忽略；不同 task_id → 置 stop + 等旧线程
 *              退出（至多 VOICE_PREEMPT_WAIT_SEC=1s）
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

/* ---- voice.sh 关键参数（与 bash 默认值一致） ---- */
#define VOICE_HOST                 "127.0.0.1"
#define VOICE_PORT                 80
#define VOICE_PLAY_PATH            "/play_audio"
#define VOICE_SAMPLE_RATE          "8000"
#define VOICE_CODEC                "opus"
#define VOICE_INPUT_VOLUME         "30"
#define VOICE_OUTPUT_VOLUME        "80"
#define VOICE_REPEAT_COUNT         3
#define VOICE_AUDIO_DURATION_SEC   4
#define VOICE_GAP_SEC              2
#define VOICE_RELOAD_WAIT_SEC      2
#define VOICE_RELOAD_RETRY_SEC     3
#define VOICE_RELOAD_RETRY_COUNT   3
#define VOICE_PREEMPT_WAIT_SEC     1
#define VOICE_SLEEP_QUANTUM_MS     100	/* 可中断 sleep 切片粒度 */

/* ---- 播放线程共享状态（g_voice_mu 保护） ---- */
static pthread_mutex_t g_voice_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_voice_tid;
static bool            g_voice_running;		/* 线程存活且未收尾 */
static bool            g_voice_tid_stale;		/* 线程已退出待 join 回收 */
static char            g_voice_task[64];
static volatile bool   g_voice_stop;			/* 抢占/停止标志 */

/* ------------------------------------------------------------------ */
/* 就绪探测（照 voice_output_is_ready：GET /play_audio 期待 200）        */
/* ------------------------------------------------------------------ */

int voice_ready(const enc_cfg_t *c)
{
	char body[128];
	int status = 0;
	int timeout = c->http_max_time_sec > 0 ? c->http_max_time_sec : 10;

	if (http_get(VOICE_HOST, VOICE_PORT, VOICE_PLAY_PATH, timeout,
		     body, sizeof(body), &status) != 0)
		return -1;
	return status == 200 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* init：锁内 yaml + HUP，重试 3 轮                                     */
/* ------------------------------------------------------------------ */

/* 单条 yaml 写入（键名严格照 voice.sh，值经 shell 拼接） */
static int voice_yaml_set(const enc_cfg_t *c, const char *key, const char *val)
{
	char quoted[320];
	char cmd[512];
	char out[256];

	shell_quote(quoted, sizeof(quoted), c->majestic_conf);
	snprintf(cmd, sizeof(cmd), "%s -i %s -s %s %s",
		 c->yaml_cli, quoted, key, val);
	return run_cmd(cmd, 10, out, sizeof(out));
}

/* voice_set_output_enabled true 的全部键：outputEnabled 负责开 HTTP 播放口，
 * audio.enabled 负责创建底层 ADEC/AO 通道，两者必须同时开启 */
static int voice_apply_audio_yaml(const enc_cfg_t *c)
{
	if (voice_yaml_set(c, "audio.enabled", "true") != 0 ||
	    voice_yaml_set(c, "audio.volume", VOICE_INPUT_VOLUME) != 0 ||
	    voice_yaml_set(c, "audio.srate", VOICE_SAMPLE_RATE) != 0 ||
	    voice_yaml_set(c, "audio.codec", VOICE_CODEC) != 0 ||
	    voice_yaml_set(c, "audio.outputVolume", VOICE_OUTPUT_VOLUME) != 0)
		return -1;
	/* outputEnabled 必须最后写，避免中间态被 Majestic 读取 */
	return voice_yaml_set(c, "audio.outputEnabled", "true");
}

int voice_init(const enc_cfg_t *c)
{
	int attempt;

	/* 快路径：输出通道已就绪则不触碰 Majestic（voice_initialize_output） */
	if (voice_ready(c) == 0)
		return 0;

	for (attempt = 1; attempt <= VOICE_RELOAD_RETRY_COUNT; attempt++) {
		char out[256];
		int rc;

		if (majestic_lock_acquire(c->majestic_lock_wait_sec) != 0) {
			log_msg(ENCM_LOG_ERROR,
				"[VOICE] majestic config lock timeout wait_sec=%d",
				c->majestic_lock_wait_sec);
			return -1;
		}
		rc = voice_apply_audio_yaml(c);
		if (rc == 0)
			rc = run_cmd("killall -HUP majestic", 10, out, sizeof(out));
		majestic_lock_release();
		if (rc != 0) {
			log_msg(ENCM_LOG_ERROR,
				"[VOICE] audio yaml/HUP failed rc=%d", rc);
			return -1;
		}

		/* HUP 被 Majestic 防抖忽略时需重试：先等重载生效再探测 */
		sleep(VOICE_RELOAD_WAIT_SEC);
		if (voice_ready(c) == 0)
			return 0;
		log_msg(ENCM_LOG_WARN,
			"[VOICE] audio output not ready after reload attempt=%d",
			attempt);
		if (attempt < VOICE_RELOAD_RETRY_COUNT)
			sleep(VOICE_RELOAD_RETRY_SEC);
	}

	log_msg(ENCM_LOG_ERROR,
		"[VOICE] audio output did not become ready retries=%d url=%s",
		VOICE_RELOAD_RETRY_COUNT, "http://" VOICE_HOST "/play_audio");
	return -1;
}

/* ------------------------------------------------------------------ */
/* desk：后台播放线程                                                   */
/* ------------------------------------------------------------------ */

/* 分片可中断 sleep；被 stop 置位打断时返回 true */
static bool voice_sleep_stoppable(int sec)
{
	int loops = sec * 1000 / VOICE_SLEEP_QUANTUM_MS;

	while (loops-- > 0) {
		if (g_voice_stop)
			return true;
		usleep(VOICE_SLEEP_QUANTUM_MS * 1000);
	}
	return g_voice_stop;
}

/* 播放线程：POST PCM × 3，每次后等 4s（最后那次也等），非最后再等 2s 间隔。
 * 任一次提交失败即终止（对齐 bash curl -f 语义）。 */
static void *voice_play_thread(void *arg)
{
	const enc_cfg_t *c = (const enc_cfg_t *)(uintptr_t)arg;
	int repeat;

	for (repeat = 1; repeat <= VOICE_REPEAT_COUNT; repeat++) {
		int status = 0;
		int timeout = c->http_max_time_sec > 0 ? c->http_max_time_sec : 10;

		if (g_voice_stop)
			goto out;
		if (http_post_file(VOICE_HOST, VOICE_PORT, VOICE_PLAY_PATH,
				   c->voice_pcm, timeout, &status) != 0 ||
		    status < 200 || status >= 400) {
			log_msg(ENCM_LOG_ERROR,
				"[VOICE] PCM submit failed status=%d repeat=%d",
				status, repeat);
			goto out;
		}
		log_msg(ENCM_LOG_DEBUG,
			"[VOICE] desk voice submitted repeat=%d", repeat);

		/* 先等本次播放完毕，再叠加空白间隔（voice_wait_with_main_guard） */
		if (voice_sleep_stoppable(VOICE_AUDIO_DURATION_SEC))
			goto out;
		if (repeat < VOICE_REPEAT_COUNT &&
		    voice_sleep_stoppable(VOICE_GAP_SEC))
			goto out;
	}
	log_msg(ENCM_LOG_INFO,
		"[VOICE] desk voice finished task_id=%s repeat=%d",
		g_voice_task, VOICE_REPEAT_COUNT);
out:
	pthread_mutex_lock(&g_voice_mu);
	g_voice_running = false;
	g_voice_tid_stale = true;
	pthread_mutex_unlock(&g_voice_mu);
	return NULL;
}

void voice_desk_async(const enc_cfg_t *c, const char *task_id)
{
	pthread_t join_tid;
	bool do_join = false;		/* 抢占旧线程 */
	bool do_reap = false;		/* 回收自然退出的旧线程 */

	if (!task_id || !task_id[0]) {
		log_msg(ENCM_LOG_ERROR, "[VOICE] desk voice task_id missing");
		return;
	}
	if (access(c->voice_pcm, R_OK) != 0) {
		log_msg(ENCM_LOG_ERROR, "[VOICE] desk pcm file missing: %s",
			c->voice_pcm);
		return;
	}

	pthread_mutex_lock(&g_voice_mu);
	if (g_voice_running) {
		if (strcmp(g_voice_task, task_id) == 0) {
			/* 同 task_id 播放中：忽略重复任务 */
			pthread_mutex_unlock(&g_voice_mu);
			log_msg(ENCM_LOG_WARN,
				"[VOICE] same desk voice task already playing, "
				"ignore duplicate task_id=%s", task_id);
			return;
		}
		/* 不同 task_id：抢占 —— 置 stop 标志，锁外等待旧线程退出 */
		char old_task[64];

		snprintf(old_task, sizeof(old_task), "%s", g_voice_task);
		g_voice_stop = true;
		join_tid = g_voice_tid;
		do_join = true;
		pthread_mutex_unlock(&g_voice_mu);
		log_msg(ENCM_LOG_WARN,
			"[VOICE] desk voice busy, preempt old task "
			"old_task_id=%s new_task_id=%s", old_task, task_id);
	} else if (g_voice_tid_stale) {
		/* 上次播放已自然结束：先回收线程资源再启动新播放 */
		join_tid = g_voice_tid;
		g_voice_tid_stale = false;
		do_reap = true;
		pthread_mutex_unlock(&g_voice_mu);
	} else {
		pthread_mutex_unlock(&g_voice_mu);
	}

	if (do_join) {
		struct timespec ts;

		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += VOICE_PREEMPT_WAIT_SEC;
		if (pthread_timedjoin_np(join_tid, NULL, &ts) == 0) {
			pthread_mutex_lock(&g_voice_mu);
			g_voice_tid_stale = false;
			pthread_mutex_unlock(&g_voice_mu);
		} else {
			/* 旧线程可能正阻塞在 HTTP 提交里，1s 内无法响应 stop。
			 * 兜底改为阻塞 join：宁可多等，也绝不让两个播放线程
			 * 并发写共享状态（对应 bash kill -9 的强杀语义）。 */
			log_msg(ENCM_LOG_WARN,
				"[VOICE] old worker not exited within %ds, join blocking",
				VOICE_PREEMPT_WAIT_SEC);
			pthread_join(join_tid, NULL);
			pthread_mutex_lock(&g_voice_mu);
			g_voice_tid_stale = false;
			pthread_mutex_unlock(&g_voice_mu);
		}
	} else if (do_reap) {
		pthread_join(join_tid, NULL);
	}

	pthread_mutex_lock(&g_voice_mu);
	g_voice_stop = false;
	snprintf(g_voice_task, sizeof(g_voice_task), "%s", task_id);
	if (pthread_create(&g_voice_tid, NULL, voice_play_thread,
			   (void *)(uintptr_t)c) == 0) {
		g_voice_running = true;
		log_msg(ENCM_LOG_INFO,
			"[VOICE] desk voice start task_id=%s repeat=%d file=%s",
			task_id, VOICE_REPEAT_COUNT, c->voice_pcm);
	} else {
		log_msg(ENCM_LOG_ERROR,
			"[VOICE] create play thread failed errno=%d", errno);
	}
	pthread_mutex_unlock(&g_voice_mu);
}

void voice_stop_current(void)
{
	pthread_mutex_lock(&g_voice_mu);
	if (g_voice_running)
		g_voice_stop = true;
	pthread_mutex_unlock(&g_voice_mu);
}

bool voice_playing(void)
{
	bool playing;

	pthread_mutex_lock(&g_voice_mu);
	playing = g_voice_running;
	pthread_mutex_unlock(&g_voice_mu);
	return playing;
}
