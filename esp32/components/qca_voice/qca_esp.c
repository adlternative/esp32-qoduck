/*
 * qca_esp.c —— qca_link 的 ESP32 胶水层
 *
 * 只做四件事（和宿主上的 demo/qca_device_demo.c 一一对应）：
 *   1) esp_websocket_client 收文本帧（自动拼接分片）→ qca_link_on_event()
 *   2) muse_audio_read() 拿 16k PCM → 带能量门控喂 qca_link_append_audio()
 *   3) qca_link 的 audio_out（16k）→ 环形缓冲 → muse_audio_write()
 *      某段样本吐完 + 收到该段 audio.done → qca_link_audio_played()
 *   4) 主循环里 qca_link_tick()
 *
 * 8 条协议规则全在 qca_link.c（有宿主单测），这里只有 IO。
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "muse_audio.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "sdkconfig.h"
#include "qca_esp.h"
#include "qca_link.h"
#include "qca_session.h"

#if __has_include("qca_config.h")
#include "qca_config.h"
#endif
/* 模板构建（没跑 apply.sh）时的兜底：能编译，但不会连任何服务器 */
#ifndef QCA_WS_URL
#define QCA_WS_URL "ws://127.0.0.1:1/realtime"
#endif
#ifndef QCA_TOKEN
#define QCA_TOKEN ""
#endif

static const char *TAG = "qca.voice";

#ifndef QCA_MIC_DBFS
#define QCA_MIC_DBFS (-50.0f)   /* 低于这个音量不上行 */
#endif
#ifndef QCA_TAIL_MS
#define QCA_TAIL_MS 1200        /* 说完后再送这么多静音，服务端 VAD 才收尾 */
#endif
#define WS_BUF 32768            /* 单个 WS 帧最大 25KB（19KB base64 音频） */

/* ───────── 下行环形缓冲（16k 单声道） ───────── */

#define RING_SAMPLES 64000      /* 4 秒（PSRAM，128KB）：WS 一帧 375ms，留足抖动余量 */
#define QSEG_MAX 8

typedef struct {
    uint32_t seg;
    size_t total;
    size_t played;
    bool closed;
} qseg_t;

static int16_t *s_ring;
static size_t s_head, s_tail;                 /* 单调递增的写/读计数 */
static SemaphoreHandle_t s_lock;
static qseg_t s_q[QSEG_MAX];
static int s_qn;
static uint32_t s_dropped;

static esp_websocket_client_handle_t s_ws;
static volatile bool s_net_up;      /* 拿到 IP 后才允许连 WSS */
static volatile bool s_new_conv;    /* 收到"开新会话"请求 */
static qca_link_t *s_link;
static volatile bool s_playing;
static volatile bool s_listening = true;
static volatile bool s_flush_req;      /* 丢掉环形缓冲里所有待播音频 */
static volatile float s_gate_dbfs = (float)QCA_MIC_DBFS;   /* 控制台 gate 命令可调 */

/* 告诉 muse_voice：麦克风归我们了，它的 idle_capture 别再读（两个读者会互相抢 I2S）。 */
bool qca_voice_owns_mic(void) { return true; }

static uint64_t qca_now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }
static size_t ring_used(void) { return s_head - s_tail; }

static size_t ring_push(const int16_t *pcm, size_t n) {
    size_t room = RING_SAMPLES - ring_used();
    if (n > room) n = room;
    for (size_t i = 0; i < n; i++) s_ring[(s_head + i) % RING_SAMPLES] = pcm[i];
    s_head += n;
    return n;
}

/* 丢掉所有待播音频和段记录：换轮/打断时用，避免上一轮的尾巴混进这一轮 */
static void drop_pending_playback(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_head = s_tail = 0;
    s_qn = 0;
    s_playing = false;
    xSemaphoreGive(s_lock);
}

static size_t ring_pop(int16_t *out, size_t n) {
    size_t used = ring_used();
    if (n > used) n = used;
    for (size_t i = 0; i < n; i++) out[i] = s_ring[(s_tail + i) % RING_SAMPLES];
    s_tail += n;
    return n;
}

/* ───────── qca_link 的 IO 回调 ───────── */

static void io_send(const char *json, size_t len, void *ctx) {
    (void)ctx;
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return;
    if (esp_websocket_client_send_text(s_ws, json, (int)len, pdMS_TO_TICKS(1000)) < 0)
        ESP_LOGW(TAG, "发送失败（%u 字节）", (unsigned)len);
}

static void io_audio(const int16_t *pcm, size_t samples, uint32_t seg, void *ctx) {
    (void)ctx;
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_qn == 0 || s_q[s_qn - 1].seg != seg) {
        if (s_qn >= QSEG_MAX) {
            /* 段队列满了：丢最老的而不是丢新到的，不能把后面的话全堵死 */
            ESP_LOGW(TAG, "段队列满（%d），丢掉最老的段 %u", QSEG_MAX, (unsigned)s_q[0].seg);
            memmove(&s_q[0], &s_q[1], sizeof(qseg_t) * (size_t)(s_qn - 1));
            s_qn--;
        }
        s_q[s_qn].seg = seg;
        s_q[s_qn].total = 0;
        s_q[s_qn].played = 0;
        s_q[s_qn].closed = false;
        s_qn++;
    }
    /* 只有真正进了环形的样本才算 total：否则 played 永远追不上 total，
     * 段永远出不了队 → 不再回执 audio_played → 服务端就不再往下发音频。 */
    size_t pushed = ring_push(pcm, samples);
    s_q[s_qn - 1].total += pushed;
    if (pushed < samples) {
        s_dropped += (uint32_t)(samples - pushed);
        static int64_t last_warn;
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_warn > 1000000) {
            last_warn = now_us;
            ESP_LOGW(TAG, "环形缓冲满，丢 %u 采样（累计 %u）",
                     (unsigned)(samples - pushed), (unsigned)s_dropped);
        }
    }
    s_playing = ring_used() > 0;
    xSemaphoreGive(s_lock);
}

static void io_receipt(const char *type, const char *w, const char *a, void *ctx) {
    (void)ctx; (void)a;
    ESP_LOGD(TAG, "回执 %s%s", type, (w && *w) ? w : "");
}

static void io_event(const char *what, const char *detail, void *ctx) {
    (void)ctx;
    if (strcmp(what, "audio.done") == 0) {
        /* detail = 刚结束的段号 */
        uint32_t seg = (uint32_t)atoi(detail ? detail : "0");
        if (s_lock) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < s_qn; i++)
                if (s_q[i].seg == seg) s_q[i].closed = true;
            xSemaphoreGive(s_lock);
        }
        return;
    }
    if (strcmp(what, "other") == 0) return;
    ESP_LOGI(TAG, "%-14s %.60s", what, detail ? detail : "");
}

/* ───────── WSS ───────── */

static char *s_rx;          /* 拼帧缓冲 */
static size_t s_rx_len;

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WSS 已连接");
        qca_link_on_open(s_link);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WSS 断开");
        qca_link_on_close(s_link);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (e->op_code != 0x01 || !e->data_ptr || e->data_len <= 0) break;
        /* buffer_size 够大时一帧就完；仍按 payload_offset 拼，防分片 */
        if (e->payload_offset == 0) s_rx_len = 0;
        if (s_rx_len + (size_t)e->data_len >= WS_BUF) { s_rx_len = 0; break; }
        memcpy(s_rx + s_rx_len, e->data_ptr, (size_t)e->data_len);
        s_rx_len += (size_t)e->data_len;
        if (e->payload_offset + e->data_len >= e->payload_len) {
            s_rx[s_rx_len] = '\0';
            qca_link_on_event(s_link, s_rx, s_rx_len);
            s_rx_len = 0;
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WSS 错误");
        break;
    default:
        break;
    }
}

/* 诊断：把扫到的 2.4G AP 打出来（SSID / 信道 / 信号 / 加密方式） */
static void dump_scan(void) {
    wifi_scan_config_t sc = {0};
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 5; i++) {          /* muse 的重连会和扫描抢状态，重试几次 */
        err = esp_wifi_scan_start(&sc, true);
        if (err == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    if (err != ESP_OK) { ESP_LOGW(TAG, "扫描失败 0x%x", err); return; }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 32) n = 32;
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (!recs) return;
    if (n && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        ESP_LOGI(TAG, "附近 2.4G AP（%u 个）：", (unsigned)n);
        for (uint16_t i = 0; i < n; i++)
            ESP_LOGI(TAG, "  %-24s ch=%2u rssi=%3d auth=%d", (const char *)recs[i].ssid,
                     recs[i].primary, recs[i].rssi, (int)recs[i].authmode);
    }
    free(recs);
}

/* 把 sdkconfig 里的 SSID/密码种进 muse 自己的设置，让 Home Link 正常持有 Wi-Fi。
 * （只靠 CONFIG_HOMEHUB_WIFI_SSID 只够连一次，glue 发现"没凭据"会立刻断开。） */
static void seed_wifi_settings(void) {
#if defined(CONFIG_HOMEHUB_WIFI_SSID) && defined(CONFIG_HOMEHUB_WIFI_PASSWORD)
    if (CONFIG_HOMEHUB_WIFI_SSID[0] == 0) return;
    static bool done = false;
    if (done) return;
    char ssid[MUSE_SSID_MAX + 1] = {0}, pass[MUSE_PASS_MAX + 1] = {0};
    muse_settings_wifi(ssid, pass);
    if (ssid[0] == 0) {
        muse_settings_set_wifi(CONFIG_HOMEHUB_WIFI_SSID, CONFIG_HOMEHUB_WIFI_PASSWORD);
        ESP_LOGI(TAG, "把 [%s] 写进 muse 的网络设置（之前是空的）", CONFIG_HOMEHUB_WIFI_SSID);
    }
    done = true;
#endif
}

/* TLS 要有正确时间：SDK 里没有 SNTP，时钟是 1970 → 证书校验必然 -0x2700 */
static void start_sntp_once(void) {
    static bool started = false;
    if (started) return;
    started = true;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_init();
    ESP_LOGI(TAG, "启动 SNTP（证书校验需要时间）");
}

static bool clock_ready(void) {
    time_t t = time(NULL);
    if (t > 1700000000) return true;
    return false;
}

/* 轮询网络状态：muse 用的是自己的 event loop，IP_EVENT 收不到 */
static bool netif_has_ip(void) {
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!n || !esp_netif_is_netif_up(n)) return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(n, &ip) != ESP_OK) return false;
    return ip.ip.addr != 0;
}

static void on_net_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)data;
    if (id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "拿到 IP，准备连 WSS");
        s_net_up = true;
    }
}

/* 优先用"设置 → Qoder"里配的地址/conversation/token，其次用编译期的 QCA_WS_URL。
 * 这样换 token 或 conversation 不用重刷固件。 */
static void build_target(char *url, size_t url_cap, char *token, size_t tok_cap) {
    char host[MUSE_HOST_MAX + 1] = {0}, conv[MUSE_VM_MAX + 1] = {0}, tok[MUSE_TOKEN_MAX + 1] = {0};
    muse_settings_hatch_host(host);
    muse_settings_hatch_vm(conv);
    muse_settings_hatch_token(tok);

    if (tok[0]) {
        snprintf(token, tok_cap, "%s", tok);
    } else {
        snprintf(token, tok_cap, "%s", QCA_TOKEN);
    }

    if (host[0] && strncmp(host, "ws://", 5) != 0 && strncmp(host, "wss://", 6) != 0) {
        /* 只给了域名：拼成 QCA 的 realtime 地址。设置页没填 conversation
         * 时用编译时那份 QCA_WS_URL 里的 conversation_id，别拼出空串。 */
        const char *use_conv = conv;
        if (!use_conv[0]) {
            const char *mark = strstr(QCA_WS_URL, "conversation_id=");
            if (mark) {
                static char def_conv[128];
                size_t n = strlen(mark + 16);
                if (n >= sizeof(def_conv)) {
                    n = sizeof(def_conv) - 1;
                }
                memcpy(def_conv, mark + 16, n);
                def_conv[n] = '\0';
                use_conv = def_conv;
            }
        }
        snprintf(url, url_cap, "wss://%s/api/v1/forward/realtime?conversation_id=%s", host, use_conv);
    } else if (host[0]) {
        snprintf(url, url_cap, "%s", host);
    } else {
        snprintf(url, url_cap, "%s", QCA_WS_URL);
    }
    ESP_LOGI(TAG, "目标: %s（token %s）", url, tok[0] ? "来自设置页" : "来自编译配置");
}

static void ws_start(void) {
    static char url[512];
    static char token[256];
    build_target(url, sizeof(url), token, sizeof(token));
    if (!url[0] || !token[0]) {          /* 没配就别连，UI 显示"未配置" */
        ESP_LOGW(TAG, "跳过连接：Qoder 地址或 token 为空");
        return;
    }

    esp_websocket_client_config_t cfg = {
        .uri = url,
        .reconnect_timeout_ms = 2000,
        .network_timeout_ms = 10000,
        .buffer_size = WS_BUF,
        .headers = NULL,
    };
#ifdef QCA_CA_PEM
    cfg.cert_pem = QCA_CA_PEM;
#else
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    static char hdr[320];
    snprintf(hdr, sizeof(hdr), "Authorization: Bearer %s\r\n", token);
    cfg.headers = hdr;
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) { ESP_LOGE(TAG, "ws init 失败"); return; }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_websocket_client_start(s_ws);
}

/* ───────── 开新会话 ───────── */

void qca_voice_new_conversation(void)
{
    s_new_conv = true;
    ESP_LOGI(TAG, "收到开新会话请求");
}

/* 先断 WSS（腾出内部 RAM 和 TLS 上下文），再开新 conversation，
 * 成功就写进设置页的 Conversation，返回后主循环会重连。 */
static void switch_conversation(void)
{
    muse_state_set_caption("正在开新会话…");
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    drop_pending_playback();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_qn = 0;
    s_head = s_tail = 0;
    xSemaphoreGive(s_lock);

    char conv[MUSE_VM_MAX + 1] = {0};
    if (!qca_session_create(conv, sizeof(conv))) {
        muse_state_set_caption("换会话失败，沿用旧的");
        return;
    }
    muse_settings_set_hatch_vm(conv);
    muse_state_set_caption("新会话，说吧");
}

/* ───────── 主循环：播放 + 采集 + tick ───────── */

/* 设置就绪后只跑一次：调整自动息屏、打印目标地址。 */
static void once_settings_ready(void) {
    static bool done;
    if (done) return;
    done = true;

    /* 常听模式要一直占用 mic：muse_voice 的 rest 已经通过 qca_voice_owns_mic()
     * 跳过 codec 断电，所以这里可以留着正常的息屏（省电、省屏）。 */
    if (muse_settings_sleep_s() == 0 || muse_settings_sleep_s() > 120) {
        muse_settings_set_sleep_s(60);
        ESP_LOGI(TAG, "自动息屏 = 60s（息屏后麦克风仍然工作）");
    }

    char u[512], t[256];
    build_target(u, sizeof(u), t, sizeof(t));
    if (u[0]) ESP_LOGI(TAG, "启动：%s", u);
    else ESP_LOGW(TAG, "Qoder 未配置：在设置页填 API 地址 / Conversation / Token");
}

static void voice_task(void *arg) {
    (void)arg;
    int16_t *mic = malloc(320 * sizeof(int16_t));      /* 20ms @16k：小块读，别饿死播放 */
    int16_t *out = malloc(1024 * sizeof(int16_t));
    if (!mic || !out) { ESP_LOGE(TAG, "OOM"); vTaskDelete(NULL); return; }

    int64_t last_loud = 0;
    int64_t head_wait = 0;         /* 队头段等待 done 的起点（15s 兜底） */
    bool ws_started = false;
    for (;;) {
        /* 0) 等网络 + 起 WSS（只起一次；断了交给 esp_websocket_client 自动重连） */
        if (!s_net_up && netif_has_ip()) {
            start_sntp_once();
            if (clock_ready()) {
                s_net_up = true;
                ESP_LOGI(TAG, "网络 + 时间就绪（%lld），开始连 WSS", (long long)time(NULL));
            }
        }
        if (!ws_started && s_net_up) { ws_started = true; ws_start(); }
        /* 连不上时扫一次 AP，方便确认 SSID / 加密方式 */
        {
            static bool scanned = false;
            if (!scanned && !s_net_up && esp_timer_get_time() > 22 * 1000 * 1000) {
                scanned = true;
                dump_scan();
            }
        }

        if (!muse_settings_ready()) {
        /* muse_app_run 在 boot_task 里异步跑，muse_settings_init 之前读设置会拿
         * NULL 互斥量直接断言重启。等它就绪再碰代码里的任何 muse_settings_*。 */
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
    }

    /* 音频还没初始化（muse 的 app 在 app_run 里才开 codec）：别碰 I2S */
        if (!muse_audio_ready()) {
            /* muse 空闲时会关 codec 省电；我们要一直听，就把它打开 */
            static int64_t last_open;
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_open > 1000000) {
                last_open = now_us;
                ESP_LOGI(TAG, "音频没开，主动打开（muse_audio_power(true)）");
                muse_audio_power(true);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            qca_link_tick(s_link);
            continue;
        }
        seed_wifi_settings();
        once_settings_ready();

        if (s_new_conv) {
            s_new_conv = false;
            switch_conversation();
            ws_started = false;        /* 下一圈用新的 conv id 重连 */
            continue;
        }

        if (s_flush_req) {
            s_flush_req = false;
            drop_pending_playback();
            ESP_LOGI(TAG, "换轮：丢弃残留待播音频");
        }

        /* 1) 播放 */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t n = ring_pop(out, 1024);
        s_playing = ring_used() > 0;
        xSemaphoreGive(s_lock);

        if (n) {
            int64_t w0 = esp_timer_get_time();
            muse_audio_write(out, n);
            int64_t wdt = esp_timer_get_time() - w0;
            if (wdt > 500 * 1000)
                ESP_LOGW(TAG, "I2S 写 入卡了 %.1fs（%u 采样）", (double)wdt / 1e6, (unsigned)n);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool finished = false;
            uint32_t done_seg = 0;
            if (s_qn > 0) {
                if (!s_q[0].closed) {
                    if (!head_wait) head_wait = esp_timer_get_time();
                    else if (esp_timer_get_time() - head_wait > 15 * 1000 * 1000) {
                        /* done 迟迟不来：强行出队并回执，否则段队列会被这一个段永久堵死
                         * （后面所有音频都发不进来，也就是“声音卡住/迟到”）。 */
                        ESP_LOGW(TAG, "段 %u 等 15s 没 done（已播 %u/%u），强制出队并回执",
                                 (unsigned)s_q[0].seg, (unsigned)s_q[0].played, (unsigned)s_q[0].total);
                        done_seg = s_q[0].seg;
                        memmove(&s_q[0], &s_q[1], sizeof(qseg_t) * (size_t)(s_qn - 1));
                        s_qn--;
                        finished = true;
                        head_wait = 0;
                    }
                } else {
                    head_wait = 0;
                }
                if (!finished) {
                    s_q[0].played += n;
                    if (s_q[0].closed && s_q[0].played >= s_q[0].total) {
                        done_seg = s_q[0].seg;
                        memmove(&s_q[0], &s_q[1], sizeof(qseg_t) * (size_t)(s_qn - 1));
                        s_qn--;
                        finished = true;
                    }
                }
            }
            xSemaphoreGive(s_lock);
            if (finished) qca_link_audio_played(s_link, done_seg);   /* 播完才回执 */
        }

        /* 2) 采集（半双工：正在播就不上行） */
        if (s_link && qca_link_ready(s_link) && s_listening && !s_playing) {
            esp_err_t rr = muse_audio_read(mic, 320);
#ifdef QCA_DEBUG_MIC
            {
                static int64_t last_log;
                int64_t now_us = esp_timer_get_time();
                if (now_us - last_log > 2000000) {
                    last_log = now_us;
                    ESP_LOGI(TAG, "mic read=%d %.1f dBFS gate=%.0f turn=%d",
                             rr, rr == ESP_OK ? (double)muse_audio_dbfs(mic, 320) : -99.0,
                             (double)s_gate_dbfs, (int)qca_link_turn_active(s_link));
                }
            }
#endif
            if (rr != ESP_OK) {
                /* muse 的省电逻辑会在我们读到一半时关掉 codec（s_open 还是 true，
                 * 但 I2S 已经关了）。连续失败就把它整个重开一次。 */
                static int fails;
                if (++fails >= 3) {
                    fails = 0;
                    ESP_LOGW(TAG, "读音频连续失败，重开 codec");
                    muse_audio_power(false);
                    vTaskDelay(pdMS_TO_TICKS(60));
                    muse_audio_power(true);
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            } else if (rr == ESP_OK) {
                float db = muse_audio_dbfs(mic, 320);
                int64_t now = esp_timer_get_time();
                if (db > s_gate_dbfs) {
                    last_loud = now;
                    if (!qca_link_turn_active(s_link)) {
                        s_flush_req = true;          /* 上一轮的尾巴丢掉 */
                        qca_link_begin_turn(s_link);
                    }
                }
                /* 一直上行（静音也发）：服务端 VAD 靠连续的静音帧判定句尾。
                 * 只在有声音时才发，回合永远不会结束。 */
                qca_link_append_audio(s_link, mic, 320);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));   /* 播报时别空转 I2S 读 */
        }

        /* 3) linger / 换连 / 重连 */
        qca_link_tick(s_link);

        /* 4) 每 5 秒一行体检：内存 / 环形 / 段 / 丢弃 / LVGL 帧 / WS。
         * 「UI 卡死」和「声音迟到」都要靠这行定位：UI帧 不涨 = LVGL 停了，
         * 环形 满 + 丢弃 涨 = 播不过来，段 一直不降 = 回执链断了。 */
        {
            static int64_t last_diag;
            static uint32_t last_frames;
            static int ui_stuck;
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_diag > 5 * 1000 * 1000) {
                last_diag = now_us;
                uint32_t fr = muse_ui_frame_seq();
                uint32_t dfr = fr - last_frames;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                size_t used = ring_used();
                int qn = s_qn;
                xSemaphoreGive(s_lock);
                ESP_LOGI(TAG, "diag 内存 %uK/大块 %uK 环形 %ums 段 %d 丢弃 %u UI帧 +%u ws=%d",
                         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                         (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                         (unsigned)(used * 1000 / 16000), qn, (unsigned)s_dropped,
                         (unsigned)dfr,
                         (s_ws && esp_websocket_client_is_connected(s_ws)) ? 1 : 0);
                if (dfr == 0 && ++ui_stuck >= 2)
                    ESP_LOGW(TAG, "UI 停摆：LVGL 帧数 %u 已 %d 次体检没涨", (unsigned)fr, ui_stuck);
                else if (dfr) ui_stuck = 0;
                last_frames = fr;
            }
        }
    }
}

/* ───────── 对外 API ───────── */

void qca_voice_start(void) {
    if (s_link) return;
    s_lock = xSemaphoreCreateMutex();
    /* 环形缓冲放 PSRAM：内部 RAM 只有 20 多 K 了，4 秒音频（128K）放不进去 */
    s_ring = heap_caps_malloc(RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) s_ring = malloc(RING_SAMPLES * sizeof(int16_t));
    s_rx = malloc(WS_BUF);
    if (!s_lock || !s_ring || !s_rx) { ESP_LOGE(TAG, "OOM"); return; }

    qca_link_io_t io = { io_send, io_audio, io_receipt, io_event, NULL };
    s_link = qca_link_new(&io, qca_now_ms);
    if (!s_link) { ESP_LOGE(TAG, "link OOM"); return; }

    /* 别在这里连：app_main 阶段 lwIP 还没起来，getaddrinfo 会在 xQueueSemaphoreTake 里挂掉。
       等 IP_EVENT_STA_GOT_IP，由任务去连。 */
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_net_event, NULL, NULL);
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_is_netif_up(sta)) s_net_up = true;

    qca_console_start();
    xTaskCreate(voice_task, "qca_voice", 8192, NULL, 5, NULL);
}

/* 设置页要显示的两件事：配好了没有、连上了没有。 */
bool qca_voice_configured(void) {
    if (!muse_settings_ready()) return false;   /* 设置没就绪：读取会拿 NULL 互斥量 */
    char url[320], tok[256];
    build_target(url, sizeof(url), tok, sizeof(tok));
    return tok[0] != '\0';
}

const char *qca_voice_status_text(void) {
    if (s_ws && esp_websocket_client_is_connected(s_ws)) {
        return "已连接";
    }
    return qca_voice_configured() ? "未连接" : "未配置";
}

void qca_esp_print_status(void) {
    const qca_link_stats_t *st = qca_link_stats(s_link);
    printf("QCA   : ws=%s turn=%d  丢弃 %u / 去重 %u / 回执 %u / 重连 %u\n",
           (s_ws && esp_websocket_client_is_connected(s_ws)) ? "已连接" : "未连接",
           (int)qca_link_turn_active(s_link), (unsigned)st->dropped_history, (unsigned)st->deduped,
           (unsigned)st->receipts, (unsigned)st->reconnects);
}

void qca_voice_set_listening(int on) { s_listening = on != 0; }

void qca_voice_set_gate(float dbfs) { s_gate_dbfs = dbfs; }
float qca_voice_gate(void) { return s_gate_dbfs; }

void qca_voice_interrupt(void) {
    if (s_link) qca_link_interrupt(s_link);
    s_flush_req = true;
}
