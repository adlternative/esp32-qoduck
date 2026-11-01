#include "qca_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ───────────────────────── 小工具：JSON 扫描 ─────────────────────────
 * 不用 cJSON：只需要顶层几个字符串字段，流式扫描省 RAM、也不会因为
 * 19KB 的 base64 帧被整棵树复制一遍。
 */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* 跳过一个 JSON 值（字符串 / 对象 / 数组 / 字面量），返回其后的位置 */
static const char *skip_value(const char *p) {
    p = skip_ws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p++;
            p++;
        }
        return *p ? p + 1 : p;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = skip_value(p); continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0) return p + 1;
            }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* 在对象 obj 里找 key。val_beg/val_len：字符串内容（未反转义）或子对象的原始范围 */
static bool json_field(const char *obj, const char *key, const char **val_beg, size_t *val_len) {
    const char *p = skip_ws(obj);
    if (*p != '{') return false;
    p++;
    size_t klen = strlen(key);
    for (;;) {
        p = skip_ws(p);
        if (*p == '}' || *p == '\0') return false;
        if (*p != '"') return false;
        p++;
        const char *kb = p;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        size_t k = (size_t)(p - kb);
        if (*p) p++;
        p = skip_ws(p);
        if (*p != ':') return false;
        p = skip_ws(p + 1);
        bool match = (k == klen) && strncmp(kb, key, klen) == 0;
        if (match && val_beg) {
            if (*p == '"') { *val_beg = p + 1; *val_len = (size_t)(skip_value(p) - p - 2); return true; }
            *val_beg = p;
            *val_len = (size_t)(skip_value(p) - p);
            return true;
        }
        p = skip_value(p);
        p = skip_ws(p);
        if (*p == ',') p++;
    }
}

/* 取字符串字段并反转义（只处理 \" \\ \/ \n \u00XX 里的前三种，够用） */
static bool json_str(const char *obj, const char *key, char *out, size_t cap) {
    const char *beg; size_t len;
    if (!json_field(obj, key, &beg, &len)) return false;
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < cap; i++) {
        char c = beg[i];
        if (c == '\\' && i + 1 < len) {
            char n = beg[++i];
            if (n == 'n') c = '\n';
            else if (n == 't') c = '\t';
            else if (n == 'r') c = '\r';
            else c = n;
        }
        out[o++] = c;
    }
    out[o] = '\0';
    return true;
}

static const char *json_obj(const char *obj, const char *key) {
    const char *beg; size_t len;
    if (!json_field(obj, key, &beg, &len)) return NULL;
    return (*beg == '{') ? beg : NULL;
}

/* ───────────────────────── base64 ───────────────────────── */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int8_t b64_rev(char c) {
    if (c >= 'A' && c <= 'Z') return (int8_t)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int8_t)(c - 'a' + 26);
    if (c >= '0' && c <= '9') return (int8_t)(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        char q[4] = { B64[(v >> 18) & 63], B64[(v >> 12) & 63],
                      (i + 1 < n) ? B64[(v >> 6) & 63] : '=',
                      (i + 2 < n) ? B64[v & 63] : '=' };
        if (o + 4 > cap) return 0;
        memcpy(out + o, q, 4);
        o += 4;
    }
    return o;
}

/* ───────────────────────── 24k → 16k ───────────────────────── */

#define RS_STEP 98304u   /* 24000/16000 的 Q16 定点：每个输出前进 1.5 个输入采样 */

typedef struct {
    int16_t prev;
    bool have;
    uint32_t seg_id;
    uint32_t phase;
    uint8_t pending_byte;
    bool have_byte;
} audio_in_t;

/* ───────────────────────── 主体 ───────────────────────── */

struct qca_seg {
    uint32_t id;
    bool started, done, played;
    char work_id[QCA_ID_LEN];
    char ann_id[QCA_ID_LEN];
};

struct qca_link {
    qca_link_io_t io;
    uint64_t (*now_ms)(void);
    qca_link_cfg_t cfg;
    qca_link_stats_t st;

    bool ready, active, closing;
    uint32_t pending_work;
    uint64_t connect_guard_until;
    uint64_t last_audio_ms;
    uint64_t rotate_at;
    uint64_t linger_at;          /* 0 = 未布置 */
    uint64_t turn_deadline;      /* 0 = 未布置 */
    uint64_t reconnect_at;
    uint32_t reconnect_delay;
    bool muted_idle;
    uint64_t muted_until;

    /* 上行 */
    int16_t up_buf[QCA_FRAME_SAMPLES];
    size_t up_len;
    qca_sink_t sink;

    /* 下行音频管线 */
    audio_in_t ain;
    uint32_t seg_seq;
    uint32_t cur_seg;
    uint32_t cur_seg_done;      /* 最近一次 audio.done 的段号 */
    struct qca_seg segs[QCA_MAX_SEGS];

    /* quiet / 待发文本 */
    char pending_text[512];
    bool have_pending_text;
    bool quiet_interrupted;

    /* 去重 */
    uint32_t done_hash[16];
    size_t done_n;
};

static uint64_t now_of(qca_link_t *l) { return l->now_ms ? l->now_ms() : 0; }

static void emit(qca_link_t *l, const char *what, const char *detail) {
    if (l->io.event) l->io.event(what, detail ? detail : "", l->io.ctx);
}

static void send_raw(qca_link_t *l, const char *json, size_t len) {
    if (l->io.send) l->io.send(json, len, l->io.ctx);
}

static void send_interrupt(qca_link_t *l) {
    static const char k[] = "{\"version\":\"voice.realtime.v1\",\"type\":\"interrupt\",\"payload\":{}}";
    send_raw(l, k, sizeof(k) - 1);
}

static void send_receipt(qca_link_t *l, const char *type, const char *work_id, const char *ann_id) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "{\"version\":\"voice.realtime.v1\",\"type\":\"playback.%s\",\"payload\":{", type);
    if (work_id && work_id[0]) n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\"work_id\":\"%s\"", work_id);
    if (ann_id && ann_id[0])
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s\"announcement_id\":\"%s\"",
                      (work_id && work_id[0]) ? "," : "", ann_id);
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "}}");
    send_raw(l, buf, (size_t)n);
    l->st.receipts++;
    if (l->io.receipt) l->io.receipt(type, work_id, ann_id, l->io.ctx);
}

static void flush_uplink(qca_link_t *l) {
    if (l->up_len == 0) return;
    /* 这两个缓冲合计 ~8.6KB：绝不能放栈上（ESP 任务栈只有 6-8KB） */
    static char b64[QCA_FRAME_SAMPLES * 2 / 3 * 4 + 8];
    size_t n = b64_encode((const uint8_t *)l->up_buf, l->up_len * 2, b64, sizeof(b64));
    if (n == 0) { l->up_len = 0; return; }
    static const char head[] = "{\"version\":\"voice.realtime.v1\",\"type\":\"audio.append\",\"payload\":{\"audio\":\"";
    static const char tail[] = "\"}}";
    static char out[sizeof(head) + sizeof(b64) + sizeof(tail)];
    size_t o = 0;
    memcpy(out + o, head, sizeof(head) - 1); o += sizeof(head) - 1;
    memcpy(out + o, b64, n); o += n;
    memcpy(out + o, tail, sizeof(tail) - 1); o += sizeof(tail) - 1;
    send_raw(l, out, o);
    l->up_len = 0;
}

/* 24k int16 → 16k int16，逐样本输出 */
static void resample_push(qca_link_t *l, int16_t s) {
    audio_in_t *a = &l->ain;
    if (!a->have) { a->prev = s; a->have = true; return; }
    int16_t cur = s;
    int16_t out[2];
    size_t n = 0;
    while (a->phase < 65536u) {
        int32_t v = (int32_t)a->prev + (((int32_t)(cur - a->prev) * (int32_t)a->phase) >> 16);
        out[n++] = (int16_t)v;
        a->phase += RS_STEP;
        if (n == 2) {
            if (l->io.audio_out) l->io.audio_out(out, 2, a->seg_id, l->io.ctx);
            l->st.audio_frames_out += 2;
            n = 0;
        }
    }
    if (n && l->io.audio_out) { l->io.audio_out(out, n, a->seg_id, l->io.ctx); l->st.audio_frames_out += n; }
    a->phase -= 65536u;
    a->prev = cur;
}

/* 字节流 → int16 → 重采样 */
static void audio_byte(qca_link_t *l, uint8_t b) {
    audio_in_t *a = &l->ain;
    if (!a->have_byte) { a->pending_byte = b; a->have_byte = true; return; }
    int16_t s = (int16_t)((uint16_t)a->pending_byte | ((uint16_t)b << 8));
    a->have_byte = false;
    resample_push(l, s);
}

static void ingest_base64(qca_link_t *l, const char *p, size_t len) {
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        char c = p[i];
        if (c == '=') break;
        int8_t v = b64_rev(c);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            audio_byte(l, (uint8_t)((acc >> bits) & 0xFF));
        }
    }
}

/* ───────────────────────── 段与回合 ───────────────────────── */

static struct qca_seg *seg_find(qca_link_t *l, uint32_t id) {
    for (size_t i = 0; i < QCA_MAX_SEGS; i++)
        if (l->segs[i].id == id && l->segs[i].id != 0) return &l->segs[i];
    return NULL;
}

static struct qca_seg *seg_new(qca_link_t *l) {
    for (size_t i = 0; i < QCA_MAX_SEGS; i++) {
        if (l->segs[i].id == 0) {
            memset(&l->segs[i], 0, sizeof(l->segs[i]));
            l->segs[i].id = ++l->seg_seq;
            return &l->segs[i];
        }
    }
    return NULL;
}

static size_t segs_open(qca_link_t *l) {
    size_t n = 0;
    for (size_t i = 0; i < QCA_MAX_SEGS; i++) if (l->segs[i].id) n++;
    return n;
}

static void seg_finish(qca_link_t *l, struct qca_seg *s) {
    if (!s || s->done == false) return;
    send_receipt(l, "ended", s->work_id, s->ann_id);
    s->id = 0;
}

static void arm_linger(qca_link_t *l) {
    if (!l->active) return;
    if (segs_open(l) || l->pending_work) return;
    l->linger_at = now_of(l) + l->cfg.linger_ms;
}

static void end_turn(qca_link_t *l) {
    l->active = false;
    l->linger_at = 0;
    emit(l, "idle", "");
}

static uint32_t fnv1a(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n && i < 300; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h ? h : 1u;
}

static bool dedupe_seen(qca_link_t *l, const char *text) {
    uint32_t h = fnv1a(text, strlen(text));
    for (size_t i = 0; i < l->done_n; i++) if (l->done_hash[i] == h) return true;
    if (l->done_n < 16) l->done_hash[l->done_n++] = h;
    return false;
}

/* ───────────────────────── 公开 API ───────────────────────── */

static bool is_gated(const char *type) {
    static const char *gated[] = {
        "audio.delta", "audio.done", "work.accepted", "work.running", "work.progress",
        "work.milestone", "work.completed", "work.failed", "transcript.delta", "transcript.final",
    };
    for (size_t i = 0; i < sizeof(gated) / sizeof(gated[0]); i++)
        if (strcmp(type, gated[i]) == 0) return true;
    return false;
}

qca_link_t *qca_link_new(const qca_link_io_t *io, uint64_t (*now_ms)(void)) {
    qca_link_t *l = (qca_link_t *)calloc(1, sizeof(*l));   /* 固件里就一个；free 交回 */
    if (!l) return NULL;
    l->io = *io;
    l->now_ms = now_ms;
    l->cfg.connect_guard_ms = 200;
    l->cfg.linger_ms = 2500;
    l->cfg.quiet_settle_ms = 150;
    l->cfg.rotate_ms = 55u * 60u * 1000u;
    l->cfg.turn_timeout_ms = 120000;
    l->reconnect_delay = 1000;
    return l;
}

void qca_link_free(qca_link_t *l) { free(l); }

size_t qca_link_sizeof(void) { return sizeof(qca_link_t); }

void qca_link_set_cfg(qca_link_t *l, const qca_link_cfg_t *cfg) { l->cfg = *cfg; }

bool qca_link_ready(const qca_link_t *l) { return l->ready; }
bool qca_link_turn_active(const qca_link_t *l) { return l->active; }
const qca_link_stats_t *qca_link_stats(const qca_link_t *l) { return &l->st; }

void qca_link_on_open(qca_link_t *l) {
    l->ready = false;
    l->active = false;
    l->closing = false;
    l->pending_work = 0;
    l->up_len = 0;
    memset(l->segs, 0, sizeof(l->segs));
    memset(&l->ain, 0, sizeof(l->ain));
    emit(l, "open", "");
}

void qca_link_on_close(qca_link_t *l) {
    l->ready = false;
    l->active = false;
    l->closing = false;
    l->linger_at = 0;
    l->reconnect_at = now_of(l) + l->reconnect_delay;
    emit(l, "close", "");
}

uint32_t qca_link_reconnect_delay_ms(qca_link_t *l) {
    uint32_t d = l->reconnect_delay;
    l->reconnect_delay = (l->reconnect_delay * 2 > 30000u) ? 30000u : l->reconnect_delay * 2;
    l->st.reconnects++;
    l->reconnect_at = 0;
    return d;
}

void qca_link_begin_turn(qca_link_t *l) {
    l->active = true;
    l->linger_at = 0;
    l->pending_work = 0;
    l->cur_seg = 0;
    l->done_n = 0;
    l->st.turns++;
    l->turn_deadline = now_of(l) + l->cfg.turn_timeout_ms;
    memset(l->segs, 0, sizeof(l->segs));
}

bool qca_link_turn_active(const qca_link_t *l);

void qca_link_interrupt(qca_link_t *l) {
    send_interrupt(l);
    for (size_t i = 0; i < QCA_MAX_SEGS; i++) {
        struct qca_seg *s = &l->segs[i];
        if (s->id && s->started && !s->played) {
            send_receipt(l, "cancelled", s->work_id, s->ann_id);
            s->played = true;
        }
        s->id = 0;
    }
    l->active = false;
    l->pending_work = 0;
    l->linger_at = 0;
    memset(&l->ain, 0, sizeof(l->ain));
    emit(l, "interrupted", "");
}

void qca_link_append_audio(qca_link_t *l, const int16_t *pcm16k, size_t samples) {
    for (size_t i = 0; i < samples; i++) {
        l->up_buf[l->up_len++] = pcm16k[i];
        if (l->up_len == QCA_FRAME_SAMPLES) flush_uplink(l);
    }
}

void qca_link_audio_played(qca_link_t *l, uint32_t seg_id) {
    struct qca_seg *s = seg_find(l, seg_id);
    if (!s || s->played) return;
    s->played = true;
    if (s->done) seg_finish(l, s);
    arm_linger(l);
}

void qca_link_send_text(qca_link_t *l, const char *text) {
    snprintf(l->pending_text, sizeof(l->pending_text), "%s", text);
    l->have_pending_text = true;
    l->quiet_interrupted = false;
}

void qca_link_on_event(qca_link_t *l, const char *json, size_t len) {
    (void)len;
    char type[48] = {0};
    if (!json_str(json, "type", type, sizeof(type))) return;

    if (strcmp(type, "voice.ready") == 0) {
        l->ready = true;
        uint64_t n = now_of(l);
        l->connect_guard_until = n + l->cfg.connect_guard_ms;
        l->last_audio_ms = 0;
        l->rotate_at = n + l->cfg.rotate_ms;
        l->reconnect_delay = 1000;
        emit(l, "ready", "");
        return;
    }
    if (strcmp(type, "connection.closed") == 0) {
        l->closing = false;
        emit(l, "closed", "");
        return;
    }
    if (strcmp(type, "playback.interrupt") == 0) {
        l->last_audio_ms = 0;       /* 服务端确认打断：不用再等静默 */
        emit(l, "audio.cancel", "");
        return;
    }
    if (strcmp(type, "ping") == 0) {
        static const char pong[] = "{\"version\":\"voice.realtime.v1\",\"type\":\"pong\",\"payload\":{}}";
        send_raw(l, pong, sizeof(pong) - 1);
        return;
    }

    const char *wo = NULL, *ao = NULL; size_t wn = 0, an = 0;
    json_field(json, "work_id", &wo, &wn);
    json_field(json, "announcement_id", &ao, &an);
    char work_id[QCA_ID_LEN] = {0}, ann_id[QCA_ID_LEN] = {0};
    if (wo) snprintf(work_id, sizeof(work_id), "%.*s", (int)(wn < sizeof(work_id) - 1 ? wn : sizeof(work_id) - 1), wo);
    if (ao) snprintf(ann_id, sizeof(ann_id), "%.*s", (int)(an < sizeof(ann_id) - 1 ? an : sizeof(ann_id) - 1), ao);

    const char *payload = json_obj(json, "payload");

    /* ── R1/R5：回合门控 ─────────────────────────────── */
    if (is_gated(type)) {
        bool guard = now_of(l) < l->connect_guard_until;
        bool audio = (strcmp(type, "audio.delta") == 0);
        if (!l->active || (guard && audio)) {
            l->st.dropped_history++;
            if (audio) {
                l->last_audio_ms = now_of(l);
                if (!l->muted_idle || now_of(l) > l->muted_until) {   /* R4 */
                    l->muted_idle = true;
                    l->muted_until = now_of(l) + 500;
                    l->st.muted_idle++;
                    send_interrupt(l);
                    emit(l, "muted", "idle audio");
                }
            }
            return;
        }
        if (audio) l->last_audio_ms = now_of(l);
        /* 还有事件进来就说明"没安静"：把收尾计时器往后推（否则播报会被 linger 砍断） */
        if (l->linger_at) l->linger_at = now_of(l) + l->cfg.linger_ms;
    }

    /* ── audio.delta ─────────────────────────────────── */
    if (strcmp(type, "audio.delta") == 0) {
        struct qca_seg *s = l->cur_seg ? seg_find(l, l->cur_seg) : NULL;
        if (!s) {
            s = seg_new(l);
            if (!s) return;
            l->cur_seg = s->id;
            s->started = true;
            snprintf(s->work_id, sizeof(s->work_id), "%s", work_id);
            snprintf(s->ann_id, sizeof(s->ann_id), "%s", ann_id);
            send_receipt(l, "started", s->work_id, s->ann_id);
        }
        l->ain.seg_id = s->id;
        const char *a; size_t alen;
        if (payload && json_field(payload, "audio", &a, &alen)) ingest_base64(l, a, alen);
        return;
    }
    if (strcmp(type, "audio.done") == 0) {
        struct qca_seg *s = l->cur_seg ? seg_find(l, l->cur_seg) : NULL;
        l->cur_seg_done = s ? s->id : 0;
        l->cur_seg = 0;
        if (s) { s->done = true; if (s->played) seg_finish(l, s); }
        arm_linger(l);
        char segbuf[16];
        snprintf(segbuf, sizeof(segbuf), "%u", (unsigned)l->cur_seg_done);
        emit(l, "audio.done", segbuf);   /* detail = 刚结束的段号，播放层据此回报 audio_played */
        return;
    }

    /* ── work.* ──────────────────────────────────────── */
    if (strcmp(type, "work.accepted") == 0) {
        l->pending_work++;
        l->linger_at = 0;
        emit(l, "work.accepted", "");
        return;
    }
    if (strcmp(type, "work.completed") == 0) {
        if (l->pending_work) l->pending_work--;
        char result[384] = {0};
        if (payload) json_str(payload, "result", result, sizeof(result));
        if (dedupe_seen(l, result)) {      /* R2 */
            l->st.deduped++;
            emit(l, "deduped", "work.completed");
            return;
        }
        emit(l, "work.completed", result);
        arm_linger(l);
        return;
    }
    if (strcmp(type, "work.failed") == 0) {
        if (l->pending_work) l->pending_work--;
        emit(l, "work.failed", "");
        return;
    }
    if (strcmp(type, "work.progress") == 0 || strcmp(type, "work.running") == 0 ||
        strcmp(type, "work.milestone") == 0 || strcmp(type, "transcript.delta") == 0 ||
        strcmp(type, "transcript.final") == 0) {
        const char *t; size_t tn;
        char detail[160] = {0};
        if (payload && (json_field(payload, "text", &t, &tn) || json_field(payload, "detail", &t, &tn)))
            snprintf(detail, sizeof(detail), "%.*s", (int)(tn < sizeof(detail) - 1 ? tn : sizeof(detail) - 1), t);
        emit(l, type, detail);
        return;
    }
    if (strcmp(type, "voice.state") == 0) {
        char state[24] = {0};
        if (payload) json_str(payload, "state", state, sizeof(state));
        emit(l, "state", state);
        if (strcmp(state, "idle") == 0) arm_linger(l);
        return;
    }
    emit(l, "other", type);
}

void qca_link_tick(qca_link_t *l) {
    uint64_t n = now_of(l);

    /* R5：连接静默期内不急着发（服务端可能正准备推招呼/历史播报） */
    if (l->have_pending_text && n < l->connect_guard_until) {
        return;
    }

    /* R5：quiet 之后才真正发文本 */
    if (l->have_pending_text) {
        bool quiet = (l->last_audio_ms == 0) || (n - l->last_audio_ms >= l->cfg.quiet_settle_ms);
        if (!quiet && !l->quiet_interrupted) {
            send_interrupt(l);
            l->quiet_interrupted = true;
            emit(l, "quiet", "interrupt");
        }
        if (quiet) {
            l->have_pending_text = false;
            qca_link_begin_turn(l);
            char buf[640];
            /* 极简转义：文本里只会有普通字符 */
            int k = snprintf(buf, sizeof(buf),
                             "{\"version\":\"voice.realtime.v1\",\"type\":\"text.message\",\"payload\":{\"text\":\"%s\"}}",
                             l->pending_text);
            send_raw(l, buf, (size_t)k);
            emit(l, "turn.submit", l->pending_text);
            l->last_audio_ms = 0;
        }
    }

    /* R3：linger 到期才收尾（还在播 / 还有活就继续等） */
    if (l->active && l->linger_at && n >= l->linger_at) {
        if (segs_open(l) == 0 && l->pending_work == 0) end_turn(l);
        else l->linger_at = n + l->cfg.linger_ms;
    }

    /* 兜底：一轮最长 120s（后台卡死时别把门控一直开着） */
    if (l->active && l->turn_deadline && n >= l->turn_deadline) {
        for (size_t i = 0; i < QCA_MAX_SEGS; i++) l->segs[i].id = 0;
        l->pending_work = 0;
        emit(l, "turn.timeout", "");
        end_turn(l);
    }

    /* R7：55 分钟主动换连 */
    if (l->ready && !l->closing && l->rotate_at && n >= l->rotate_at) {
        l->closing = true;
        char buf[128];
        int k = snprintf(buf, sizeof(buf),
                         "{\"version\":\"voice.realtime.v1\",\"type\":\"connection.close\",\"payload\":{\"request_id\":\"rotate_%llu\"}}",
                         (unsigned long long)n);
        send_raw(l, buf, (size_t)k);
        emit(l, "rotate", "approaching-60min");
    }

    /* R8：到了重连时间就通知上层 */
    if (!l->ready && l->reconnect_at && n >= l->reconnect_at) {
        l->reconnect_at = 0;
        emit(l, "reconnect", "");
    }
}
