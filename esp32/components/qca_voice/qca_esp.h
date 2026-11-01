/*
 * qca_esp.h —— 把 qca_link 接到 ESP32 上（WSS + muse_audio 的麦克风/喇叭）
 *
 * 配置来自 esp32/main/qca_config.h（apply.sh 生成）：
 *   QCA_WS_URL      wss://.../api/v1/forward/realtime?conversation_id=conv_xxx
 *   QCA_TOKEN       Authorization: Bearer <PAT>
 *   QCA_CA_PEM      （可选）直连 api.qoder.com 时钉的根证书
 *   QCA_MIC_DBFS    可选，默认 -50：低于这个音量不上行
 */
#pragma once

/* 串口控制台（vol / mute / wifi / status / reboot） */
void qca_console_start(void);

/* 打印一行状态（给 status 命令用） */
void qca_esp_print_status(void);

/* 启动 QCA 语音任务（在 app_main 里调一次）。不依赖 QCA_VOICE_ENABLE，由调用点决定。 */
void qca_voice_start(void);

/* 可选：手动开/关听（以后接滚轮用），默认一直听。 */
void qca_voice_set_listening(int on);

/* 麦克风门限（dBFS，越大越不灵敏），运行时可用串口 gate 命令改 */
void qca_voice_set_gate(float dbfs);
float qca_voice_gate(void);

/* 可选：打断当前播报。 */
void qca_voice_interrupt(void);

/* 开一段新 conversation：断掉当前 WSS → 在建新的 → 写入设置页的 Conversation。
 * 异步：真正干活在语音任务里（避开内部 RAM 双 TLS 的冲突）。 */
void qca_voice_new_conversation(void);

/* 供设置页显示：有没有配置（token），以及当前链路状态。 */
bool qca_voice_configured(void);
const char *qca_voice_status_text(void);
