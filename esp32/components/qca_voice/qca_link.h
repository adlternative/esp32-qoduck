/*
 * qca_link.h —— QCA Forward Realtime 设备端状态机（纯 C11，无 ESP-IDF 依赖）
 *
 * 把在真机上验证过的 8 条规则做成可测的状态机，固件只负责：
 *   1) 用 esp_websocket_client 收发文本帧，收到就喂 qca_link_on_event()
 *   2) 把麦克风 16k PCM 喂 qca_link_append_audio()
 *   3) 把 audio_out 回调里的 16k PCM 写进 I2S
 *   4) 每 50–100ms 调一次 qca_link_tick()
 *
 * 规则（每条都有单测）：
 *   R1 回合门控：连上后服务端会重放整段历史，非本轮的 work/audio/transcript 全部丢弃
 *   R2 work.completed 去重（服务端会发两次，内容相同、event_id 不同）
 *   R3 linger：voice.state idle 之后服务端还会补发 work.accepted，收尾要等 2.5s
 *   R4 空闲期音频：丢弃并回一次 interrupt（省 48KB/s）
 *   R5 连接静默期 200ms + quiet()：等 150ms 音频静默再开轮，避免串音
 *   R6 回执：每段 started 一次；played && done 都满足才 ended；打断发 cancelled
 *   R7 55 分钟主动 connection.close → connection.closed → 复用同一 conversation 重连
 *   R8 断线指数退避重连 1s→30s
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QCA_MAX_SEGS 8
#define QCA_ID_LEN 72
#define QCA_FRAME_SAMPLES 1600          /* 100ms @ 16k，上行分帧粒度 */

typedef struct {
    /* 向 WS 发送一帧文本 JSON（固件里是 esp_websocket_client_send_text） */
    void (*send)(const char *json, size_t len, void *ctx);
    /* 24k→16k 之后的 PCM，写进 I2S。seg_id 用于播完后回报 qca_link_audio_played() */
    void (*audio_out)(const int16_t *pcm16k, size_t samples, uint32_t seg_id, void *ctx);
    /* 回执：type = "started" | "ended" | "cancelled" */
    void (*receipt)(const char *type, const char *work_id, const char *announcement_id, void *ctx);
    /* 状态/日志：ready|idle|dropped|deduped|muted|quiet|rotate|reconnect|work … */
    void (*event)(const char *what, const char *detail, void *ctx);
    void *ctx;
} qca_link_io_t;

typedef struct {
    uint32_t dropped_history;   /* 门控丢掉的事件数 */
    uint32_t deduped;           /* 去重掉的 work.completed */
    uint32_t muted_idle;        /* 空闲期被打断的音频流次数 */
    uint32_t receipts;          /* 发出的回执数 */
    uint32_t turns;             /* 回合数 */
    uint32_t reconnects;        /* 重连次数 */
    uint32_t audio_frames_out;  /* 输出的 16k 采样数（不是帧） */
} qca_link_stats_t;

typedef enum {
    QCA_SINK_NONE = 0,
    QCA_SINK_APPEND,   /* 上行分帧中 */
} qca_sink_t;

typedef struct qca_link qca_link_t;

/* now_ms：单调毫秒时钟（固件里 esp_timer_get_time()/1000） */
qca_link_t *qca_link_new(const qca_link_io_t *io, uint64_t (*now_ms)(void));
size_t qca_link_sizeof(void);   /* 固件里算 heap 用 */
void qca_link_free(qca_link_t *l);

/* —— 连接生命周期 —— */
void qca_link_on_open(qca_link_t *l);                              /* WS 已连上 */
void qca_link_on_close(qca_link_t *l);                             /* WS 断开（内部算退避） */
uint32_t qca_link_reconnect_delay_ms(qca_link_t *l);               /* 该等多久再连 */
void qca_link_on_event(qca_link_t *l, const char *json, size_t len);

/* —— 回合 —— */
void qca_link_send_text(qca_link_t *l, const char *text);           /* 内部先 quiet */
void qca_link_begin_turn(qca_link_t *l);                            /* 语音上行前 */
void qca_link_append_audio(qca_link_t *l, const int16_t *pcm16k, size_t samples);
void qca_link_interrupt(qca_link_t *l);
void qca_link_audio_played(qca_link_t *l, uint32_t seg_id);         /* 喇叭播空 */
void qca_link_tick(qca_link_t *l);                                  /* 50–100ms 调一次 */

bool qca_link_ready(const qca_link_t *l);
bool qca_link_turn_active(const qca_link_t *l);
const qca_link_stats_t *qca_link_stats(const qca_link_t *l);

/* 仅供单测/调参：默认 200/2500/150/3300000ms */
typedef struct {
    uint32_t connect_guard_ms;
    uint32_t linger_ms;
    uint32_t quiet_settle_ms;
    uint32_t rotate_ms;
    uint32_t turn_timeout_ms;    /* 一轮最长多久强制收尾（后台卡住时兜底） */
} qca_link_cfg_t;
void qca_link_set_cfg(qca_link_t *l, const qca_link_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
