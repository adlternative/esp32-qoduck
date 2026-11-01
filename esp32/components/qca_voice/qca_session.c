/*
 * qca_session.c —— 见 qca_session.h
 *
 * 只有一件事：POST /api/v1/forward/realtime/conversations 拿一个新的 conv id。
 * 内存很紧（内部 RAM 稳态 20K 左右），所以调用方必须先断开 WSS 再调这里，
 * 否则 TLS 握手会失败（ESP_ERR_MBEDTLS_SSL_SETUP_FAILED / NO_MEM）。
 */
#include "qca_session.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#include "muse_settings.h"

#if __has_include("qca_config.h")
#include "qca_config.h"
#endif

#ifndef QCA_WS_URL
#define QCA_WS_URL ""
#endif
#ifndef QCA_TOKEN
#define QCA_TOKEN ""
#endif
#ifndef QCA_IDENTITY_ID
#define QCA_IDENTITY_ID ""
#endif
#ifndef QCA_TEMPLATE_ID
#define QCA_TEMPLATE_ID ""
#endif
#ifndef QCA_VOICE
#define QCA_VOICE "longanqian"   /* 默认音色，见文档里的预设列表 */
#endif

static const char *TAG = "qca.session";
#define NVS_NS "qca_cfg"

/* ───────── identity / template ───────── */

static bool nvs_read(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK || out[0] == '\0') {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool qca_session_set_ids(const char *idn, const char *tmpl)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = true;
    if (idn && idn[0]) ok = ok && nvs_set_str(h, "idn", idn) == ESP_OK;
    if (tmpl && tmpl[0]) ok = ok && nvs_set_str(h, "tmpl", tmpl) == ESP_OK;
    if (ok) ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void qca_session_ids(char *idn, size_t idn_cap, char *tmpl, size_t tmpl_cap)
{
    if (!nvs_read("idn", idn, idn_cap)) snprintf(idn, idn_cap, "%s", QCA_IDENTITY_ID);
    if (!nvs_read("tmpl", tmpl, tmpl_cap)) snprintf(tmpl, tmpl_cap, "%s", QCA_TEMPLATE_ID);
}

/* ───────── 地址 / token ───────── */

/* 设置页里填的是 api.qoder.com（域名）或者完整的 wss://.../realtime?conversation_id=…
 * 这里把它归成 REST 的基地址 https://api.qoder.com。 */
static void api_base(char *out, size_t cap)
{
    char host[MUSE_HOST_MAX + 1] = {0};
    char full[320];
    muse_settings_hatch_host(host);
    const char *src = host[0] ? host : QCA_WS_URL;
    out[0] = '\0';
    if (!src[0]) return;

    const char *rest = NULL;
    if (strncmp(src, "wss://", 6) == 0) rest = src + 6;
    else if (strncmp(src, "ws://", 5) == 0) rest = src + 5;

    if (!rest) {                                  /* 只给了域名 */
        snprintf(full, sizeof(full), "https://%s", src);
        strlcpy(out, full, cap);
        return;
    }
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", rest);
    char *slash = strstr(tmp, "/api/");            /* 砍掉 WSS 的路径 */
    if (slash) *slash = '\0';
    snprintf(full, sizeof(full), "https://%s", tmp);
    strlcpy(out, full, cap);
}

static void api_token(char *out, size_t cap)
{
    char tok[MUSE_TOKEN_MAX + 1] = {0};
    muse_settings_hatch_token(tok);
    snprintf(out, cap, "%s", tok[0] ? tok : QCA_TOKEN);
}

/* ───────── HTTP ───────── */

/* 响应很小（一个 conversation 对象，几百字节），静态缓冲就够，别占栈。 */
static char s_resp[1024];
static size_t s_resp_len;
static bool s_resp_busy;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0 && s_resp_busy) {
        size_t room = sizeof(s_resp) - 1 - s_resp_len;
        size_t n = (size_t)e->data_len < room ? (size_t)e->data_len : room;
        if (n) {
            memcpy(s_resp + s_resp_len, e->data, n);
            s_resp_len += n;
            s_resp[s_resp_len] = '\0';
        }
    }
    return ESP_OK;
}

/* 从 {"id":"conv_xxx",…} 里抠出 conv id */
static bool pick_id(const char *json, char *out, size_t cap)
{
    const char *p = strstr(json, "\"id\":\"");
    if (!p) p = strstr(json, "conv_");
    else p += 6;
    if (!p) return false;
    size_t n = 0;
    while (p[n] && p[n] != '"' && p[n] != '\\' && n + 1 < cap) n++;
    if (n < 5) return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return strncmp(out, "conv_", 5) == 0;
}

bool qca_session_create(char *out, size_t cap)
{
    char base[192], token[256], idn[80], tmpl[80];
    api_base(base, sizeof(base));
    api_token(token, sizeof(token));
    qca_session_ids(idn, sizeof(idn), tmpl, sizeof(tmpl));

    if (!base[0] || !token[0] || !idn[0] || !tmpl[0]) {
        ESP_LOGE(TAG, "开新会话缺配置：地址 %s / token %s / identity \"%s\" / template \"%s\"",
                 base[0] ? "有" : "无", token[0] ? "有" : "无", idn, tmpl);
        if (!idn[0] || !tmpl[0]) {
            ESP_LOGE(TAG, "补一下：串口 ids idn_xxx tmpl_xxx（或写进 qca_config.local.h）");
        }
        return false;
    }

    char url[300];
    snprintf(url, sizeof(url), "%s/api/v1/forward/realtime/conversations", base);

    char body[420];
    snprintf(body, sizeof(body),
             "{\"identity_id\":\"%s\",\"template_id\":\"%s\",\"title\":\"Qoduck %08lx\","
             "\"config\":{\"audio\":{\"output\":{\"voice\":\"%s\"}}}}",
             idn, tmpl, (unsigned long)(esp_random() & 0xffffff), QCA_VOICE);

    char idem[48], auth[300];
    snprintf(idem, sizeof(idem), "qoduck-%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    snprintf(auth, sizeof(auth), "Bearer %s", token);

    s_resp[0] = '\0';
    s_resp_len = 0;
    s_resp_busy = true;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 25000,
        .buffer_size = 1024,
        .event_handler = on_http,
    };
#ifdef QCA_CA_PEM
    cfg.cert_pem = QCA_CA_PEM;
#else
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    ESP_LOGI(TAG, "开新会话：POST %s（内部 RAM %uK / 大块 %uK）", url,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        s_resp_busy = false;
        ESP_LOGE(TAG, "http client init 失败（内存不够？）");
        return false;
    }
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Idempotency-Key", idem);
    esp_http_client_set_post_field(c, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    s_resp_busy = false;

    if (err != ESP_OK || status / 100 != 2) {
        ESP_LOGE(TAG, "开新会话失败：%s HTTP %d %s", esp_err_to_name(err), status,
                 s_resp_len ? s_resp : "(无响应体)");
        return false;
    }
    if (!pick_id(s_resp, out, cap)) {
        ESP_LOGE(TAG, "开新会话：响应里没有 conv id：%s", s_resp);
        return false;
    }
    ESP_LOGI(TAG, "新会话 = %s", out);
    return true;
}

void qca_session_print_config(void)
{
    char base[192], idn[80], tmpl[80];
    api_base(base, sizeof(base));
    qca_session_ids(idn, sizeof(idn), tmpl, sizeof(tmpl));
    printf("接口  : %s（token %s）\n", base[0] ? base : "(未配置)",
           (muse_settings_hatch_token_len() ? "来自设置页" : (QCA_TOKEN[0] ? "来自编译配置" : "未配置")));
    printf("身份  : %s\n", idn[0] ? idn : "(未配置，串口 ids idn_xxx tmpl_xxx)");
    printf("模板  : %s\n", tmpl[0] ? tmpl : "(未配置)");
}
