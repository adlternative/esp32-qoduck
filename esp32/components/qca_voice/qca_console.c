/*
 * qca_console.c —— 串口控制台（改音量 / 配 Wi-Fi / 看状态，不用重刷固件）
 *
 *   vol 0-100        设置音量（并对当前播放立即生效）
 *   mute on|off      静音开关（等同 muse 的扬声器开关）
 *   wifi <ssid> <pass>  改 Wi-Fi 并写进 NVS（重启后生效）
 *   status           打印网络 / WSS / 回合 / 内存状态
 *   reboot           软重启
 *
 * 用 USB 串口（CH342，115200）直接敲；日志和命令行会混在一起。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

#include "muse_audio.h"
#include "muse_input.h"
#include "muse_pixel.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "qca_esp.h"
#include "qca_session.h"

static const char *TAG = "qca.cmd";

static int cmd_vol(int argc, char **argv) {
    if (argc < 2) { printf("用法: vol 0-100（当前 %d）\n", muse_settings_volume()); return 0; }
    int v = atoi(argv[1]);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    muse_settings_set_volume(v);         /* 存 NVS，下次开机保持 */
    muse_audio_set_volume(v);            /* 立即生效 */
    printf("音量 = %d%%\n", v);
    return 0;
}

static int cmd_mute(int argc, char **argv) {
    if (argc < 2) { printf("用法: mute on|off（当前 %s）\n", muse_settings_speaker_on() ? "on" : "off"); return 0; }
    bool on = strcmp(argv[1], "off") != 0;      /* mute on = 关扬声器 */
    muse_settings_set_speaker_on(!on);
    printf("扬声器 = %s\n", !on ? "on" : "off");
    return 0;
}

static int cmd_wifi(int argc, char **argv) {
    if (argc < 3) {
        char ssid[MUSE_SSID_MAX + 1] = {0}, pass[MUSE_PASS_MAX + 1] = {0};
        muse_settings_wifi(ssid, pass);
        printf("用法: wifi <ssid> <pass>（当前 %s）\n", ssid[0] ? ssid : "(空)");
        return 0;
    }
    muse_settings_set_wifi(argv[1], argv[2]);    /* 写 NVS，Home Link 会自己重连 */
    printf("已保存 Wi-Fi: %s（几秒内会尝试连接；不稳就 reboot）\n", argv[1]);
    return 0;
}

static int cmd_status(int argc, char **argv) {
    (void)argc; (void)argv;
    char ssid[MUSE_SSID_MAX + 1] = {0}, pass[MUSE_PASS_MAX + 1] = {0};
    muse_settings_wifi(ssid, pass);
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    if (n) esp_netif_get_ip_info(n, &ip);

    printf("Wi-Fi : %s（%s）\n", ssid[0] ? ssid : "(空)",
           (n && esp_netif_is_netif_up(n)) ? "已连接" : "未连接");
    printf("IP    : " IPSTR "\n", IP2STR(&ip.ip));
    printf("音量  : %d%%   扬声器: %s\n", muse_settings_volume(),
           muse_settings_speaker_on() ? "on" : "off");
    printf("音频  : %s\n", muse_audio_ready() ? "已打开" : "未打开");
    muse_power_t p = muse_state_power();
    if (p.battery_pct < 0) {
        printf("电池  : 未接电池/USB 供电\n");
    } else {
        printf("电池  : %d%%%s%s\n", p.battery_pct, p.charging ? "（充电中）" : "",
               p.battery_mv ? "" : "");
    }
    qca_esp_print_status();
    qca_session_print_config();
    printf("内存  : internal %u B free / PSRAM %u B free\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static int cmd_gate(int argc, char **argv) {
    if (argc < 2) { printf("用法: gate -45（当前 %.0f dBFS，越大越不灵敏）\n", (double)qca_voice_gate()); return 0; }
    qca_voice_set_gate((float)atof(argv[1]));
    printf("门限 = %.0f dBFS\n", (double)qca_voice_gate());
    return 0;
}

static int cmd_int(int argc, char **argv) {
    (void)argc; (void)argv;
    qca_voice_interrupt();
    printf("已打断/复位当前回合\n");
    return 0;
}

static int cmd_new(int argc, char **argv)
{
    (void)argc; (void)argv;
    qca_voice_new_conversation();
    printf("已排入队列：断 WSS → 开新会话 → 重连（看日志 qca.session / qca.voice）\n");
    return 0;
}

static int cmd_ids(int argc, char **argv)
{
    if (argc < 3) {
        char idn[80] = {0}, tmpl[80] = {0};
        qca_session_ids(idn, sizeof(idn), tmpl, sizeof(tmpl));
        printf("用法: ids idn_xxx tmpl_xxx\n当前 identity=%s template=%s\n",
               idn[0] ? idn : "(空)", tmpl[0] ? tmpl : "(空)");
        return 0;
    }
    if (!qca_session_set_ids(argv[1], argv[2])) {
        printf("写入失败（NVS）\n");
        return 1;
    }
    printf("已保存：identity=%s template=%s（开新会话会用它）\n", argv[1], argv[2]);
    return 0;
}

/* bye：只播关机告别动画、不断电，边播边打 ASCII 帧（验收动效用）。 */
static int cmd_bye(int argc, char **argv) {
    (void)argc; (void)argv;
    static const int at_ms[] = { 40, 250, 600, 950, 1250, 1450 };
    muse_state_set_mode(MUSE_MODE_OFF);
    muse_state_set_caption("再见！");
    muse_pixel_goodbye_begin();
    int prev = 0;
    for (int i = 0; i < (int)(sizeof(at_ms) / sizeof(at_ms[0])); i++) {
        vTaskDelay(pdMS_TO_TICKS(at_ms[i] - prev));
        prev = at_ms[i];
        printf("@bye %dms (k=%.2f)\n", at_ms[i], muse_pixel_goodbye_progress());
        muse_pixel_dump_ascii();
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    muse_pixel_goodbye_end();
    muse_state_set_mode(MUSE_MODE_IDLE);
    muse_state_set_caption("关机动画预览结束");
    return 0;
}

/* off：真关机，会把告别动画完整走一遍再断电。 */
static int cmd_off(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("关机中（1.7s 动画后断电）\n");
    muse_input_request_power_off();
    return 0;
}

static int cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("重启…\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

/* emote [pet|dizzy|look|sleepy]：预览滚轮彩蛋动效，边播边打 ASCII 帧。 */
static int cmd_emote(int argc, char **argv) {
    static const struct { const char *name; int id; int ms; } kinds[] = {
        { "pet", MUSE_EMOTE_PET, 1000 },
        { "dizzy", MUSE_EMOTE_DIZZY, 1400 },
        { "look", MUSE_EMOTE_LOOK, 1000 },
        { "sleepy", MUSE_EMOTE_SLEEPY, 1600 },
    };
    const char *want = argc > 1 ? argv[1] : "pet";
    int id = -1, dur = 1000;
    for (int i = 0; i < (int)(sizeof(kinds) / sizeof(kinds[0])); i++) {
        if (strcmp(want, kinds[i].name) == 0) {
            id = kinds[i].id;
            dur = kinds[i].ms;
            break;
        }
    }
    if (id < 0) {
        printf("用法: emote pet|dizzy|look|sleepy\n");
        return 1;
    }
    printf("@emote %s %dms\n", want, dur);
    muse_pixel_emote(id);
    for (int t = 0; t <= dur; t += dur / 4) {
        vTaskDelay(pdMS_TO_TICKS(dur / 4));
        printf("@emote %s t=%dms active=%d\n", want, t + dur / 4, muse_pixel_emote_active());
        muse_pixel_dump_ascii();
    }
    printf("@emote %s 结束（剩下 %.2f）\n", want, 0.0f);
    return 0;
}

void qca_console_start(void) {
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "qca>";
    cfg.max_cmdline_length = 160;
    esp_console_dev_uart_config_t uart = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    if (esp_console_new_repl_uart(&uart, &cfg, &repl) != ESP_OK) {
        ESP_LOGW(TAG, "串口控制台起不来");
        return;
    }
    esp_console_register_help_command();
    static const esp_console_cmd_t cmds[] = {
        { .command = "vol",    .help = "vol 0-100  设置音量",            .func = cmd_vol },
        { .command = "mute",   .help = "mute on|off 扬声器开关",          .func = cmd_mute },
        { .command = "wifi",   .help = "wifi <ssid> <pass> 改 Wi-Fi",     .func = cmd_wifi },
        { .command = "status", .help = "status     看网络/WSS/内存状态",   .func = cmd_status },
        { .command = "gate",   .help = "gate -45   麦克风门限(dBFS)",      .func = cmd_gate },
        { .command = "int",    .help = "int        打断/复位当前回合",      .func = cmd_int },
        { .command = "new",    .help = "new        开一段新的 conversation",  .func = cmd_new },
        { .command = "ids",    .help = "ids idn_xxx tmpl_xxx  身份/模板",     .func = cmd_ids },
        { .command = "bye",    .help = "bye        预览关机告别动画",       .func = cmd_bye },
        { .command = "emote",  .help = "emote      预览滚轮彩蛋 pet|dizzy|look|sleepy", .func = cmd_emote },
        { .command = "off",    .help = "off        关机（含告别动画）",      .func = cmd_off },
        { .command = "reboot", .help = "reboot     软重启",               .func = cmd_reboot },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) esp_console_cmd_register(&cmds[i]);
    esp_console_start_repl(repl);
    ESP_LOGI(TAG, "串口控制台就绪：vol / mute / wifi / status / reboot");
}
