/* QCA 语音的编译期配置。
 *
 * 这里不能放凭据：QCA_TOKEN 默认留空，由下列任一方式提供，都不进仓库：
 *   1) 本地 esp32/components/qca_voice/qca_config.local.h（已在 .gitignore）
 *   2) idf.py build 时 -DQCA_TOKEN=\"pt-...\"
 * 运行时也可以在「设置 → QODER → Access token」里填，优先级高于编译期。
 */
#pragma once

#if __has_include("qca_config.local.h")
#include "qca_config.local.h"   /* 本地私有配置，不入库 */
#endif

#define QCA_VOICE_ENABLE 1
#define QCA_DEBUG_MIC 1
#define QCA_MIC_DBFS (-55.0f)

/* conversation / 地址可以入库，不含凭据。 */
/* Leave empty in the public repo: everyone runs their own Qoder conversation.
 * Fill QCA_WS_URL here (or in qca_config.local.h), or type it into
 * Settings -> QODER at runtime, which wins over this default. */
#ifndef QCA_WS_URL
#define QCA_WS_URL ""
#endif

#ifndef QCA_TOKEN
#define QCA_TOKEN ""            /* 没有就运行时配，别写死 */
#endif

/* 设备端"开新会话"要用 identity + template（POST /realtime/conversations）。
 * 不是凭据，但和你的账号绑定，所以默认留空：串口 `ids idn_xxx tmpl_xxx`
 * 写进 NVS，或者填在 qca_config.local.h 里。 */
#ifndef QCA_IDENTITY_ID
#define QCA_IDENTITY_ID ""
#endif
#ifndef QCA_TEMPLATE_ID
#define QCA_TEMPLATE_ID ""
#endif
#ifndef QCA_VOICE
#define QCA_VOICE "longanqian"   /* longanqian/longanlingxin/longanlingxi/longanxiaoxin/longanlufeng */
#endif

/* api.qoder.com: SHECA DV TLS RSA CA A1 -> UCA Global G2 Root */
#define QCA_CA_PEM \
  "-----BEGIN CERTIFICATE-----\n" \
  "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\n" \
  "UB+K+wb1whnw0A==\n" \
  "-----END CERTIFICATE-----\n" \
  ""
