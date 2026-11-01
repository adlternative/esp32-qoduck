/*
 * qca_session.h —— 在设备上开一个新的 Qoder realtime conversation
 *
 * 一次 conversation 就是一段"对话上下文"。上下文太长会变慢变贵，而且单条
 * WSS 连接最多挂 60 分钟，所以需要能随时换一段新的。
 *
 * 长期记忆不在这里：Qoder 的 Memory Store 是挂在 (identity, template) 上的，
 * 每段 conversation 都能读写同一个 memory 文件（沙箱里的
 * /data/.qoder/awareness/），所以换新会话不会丢记忆。
 *
 * 需要 identity_id / template_id：
 *   - 编译期默认见 qca_config.h / qca_config.local.h
 *   - 运行时改：串口 `ids idn_xxx tmpl_xxx`（存 NVS，重启还在）
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 新建一段 conversation。成功时把 conv_xxx 写进 out 并返回 true。
 * 阻塞（一次 TLS 握手 + HTTP POST，通常 2-6 秒），只应在语音任务里调。 */
bool qca_session_create(char *out, size_t cap);

/* 读/写 identity + template（NVS 优先，其次编译期默认）。 */
void qca_session_ids(char *idn, size_t idn_cap, char *tmpl, size_t tmpl_cap);
bool qca_session_set_ids(const char *idn, const char *tmpl);

/* 打印一行配置（给串口 status 用）。 */
void qca_session_print_config(void);
