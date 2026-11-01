---
name: qoduck-setup
description: >-
  Bring a Qoduck voice gadget online: flash the firmware, put it on Wi-Fi, create a Qoder
  Cloud Agents token plus conversation, and verify the voice loop. Use for first-time setup,
  re-provisioning a device after a reset, or when the device says it cannot reach Qoder.
---

# Qoduck setup

Everything below is host-side work. The device itself has no config UI beyond its scroll-wheel
menu, so the order is: flash → Wi-Fi → Qoder credentials → verify.

## 1. Prerequisites

- ESP-IDF **v6.0.1** (`~/esp/esp-idf-v6`, `./install.sh esp32s3`, `. ./export.sh`)
- Node.js 20+ (only for `esp32/tools/qca/provision.mjs`)
- A Qoder account — https://qoder.com (international) or https://qoder.cn (China)
- Chrome on a desktop or Android phone (Web Bluetooth is not available in Safari)

Credentials are **never** committed. They live in NVS on the device, in
`esp32/components/qca_voice/qca_config.local.h` (git-ignored) or in a `-DQCA_TOKEN=` build flag.

## 2. Flash the firmware

```sh
cd esp32
idf.py -B build-muse-sensecap-watcher -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-sensecap-watcher/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-sensecap-watcher" \
  build
idf.py -B build-muse-sensecap-watcher -p /dev/tty.usbmodemXXXX flash monitor
```

First boot: the screen shows the boot animation, then a duck face, and the serial log shows
`hb t=...s setup=advertising wifi=down ws=down ble=advertising`. No Wi-Fi yet — that is expected.

## 3. Create the Qoder token and conversation

**Token.** Sign in to Qoder, open <https://qoder.com/cloud/pat-keys> (China site:
<https://qoder.com.cn/cloud/pat-keys>), and create a Personal Access Token (`pt-...`). Treat it like
a password: it bills to your account. Tokens are host-specific — one minted on `.com` will not
authenticate against the `.cn` endpoint.

**Conversation.** A device talks to one Qoder Cloud Agents *realtime conversation*. Create it once:

```sh
cd esp32/tools/qca
QODER_ACCESS_TOKEN=pt-... node provision.mjs
```

It creates (or reuses) a voice template with a spoken-answer system prompt and web tools, an
identity, and a conversation, then writes `qca-conversation.json` and prints the three values you
need. Creating a conversation costs credits (242 at the time of writing; reconnecting to an
existing one is much cheaper), so keep that file.

## 4. Put the device on Wi-Fi

### Option A — BLE web page (no cable, recommended)

```sh
esp32/tools/serve_setup.sh          # serves ble_setup.html on :8765
```

1. Open the printed URL in Chrome (`http://127.0.0.1:8765` on the desktop, or the LAN address on a
   phone).
2. *Connect device* → pick `Qoduck-xxxx` in the chooser. The page filters by **name prefix**
   because the setup characteristics are not in the BLE advertisement; if the list is empty, use
   *Show all devices*.
3. Fill SSID / password → *Send to device*. Then paste the Qoder token (values over ~180 bytes are
   chunked automatically) and the conversation id.

The phone/desktop needs a secure origin for Web Bluetooth. Over plain `http://` add the URL to
`chrome://flags/#unsafely-treat-insecure-origin-as-secure` and relaunch Chrome.

### Option B — on the device

Swipe to *Settings → WIRELESS NETWORK* for SSID/password, and *Settings → QODER* for the API URL
(`wss://api.qoder.com/api/v1/forward/realtime`), conversation id and token. This is also how you
rotate a token without reflashing.

### Option C — build time

```c
// esp32/components/qca_voice/qca_config.local.h  (git-ignored)
#define QCA_TOKEN  "pt-..."
#define QCA_WS_URL "wss://api.qoder.com/api/v1/forward/realtime?conversation_id=conv_..."
```

## 5. Verify

Serial log after the device joins Wi-Fi:

```
wifi=up ws=up          # connected to Qoder
hb t=42s setup=done wifi=up ws=up ble=advertising
```

On screen, *Settings → QODER* shows 已连接 / 未连接 / 未配置. Then just talk — the duck sways while
audio streams back. Expect 1.0–2.5 s to the first spoken frame.

## 6. Controls

| Gesture | Action |
|---|---|
| speak | conversation (always-listening) |
| tap wheel | pet the duck — hops once, hearts float up |
| double-tap wheel | dizzy — spins, star orbit, X eyes |
| turn wheel | look around |
| hold 1.2 s | dozing off (closed eyes, Zzz) |
| hold 2.4 s | asleep (screen off) |
| hold 4.2 s | power off, after the goodbye animation |
| double-turn wheel | toggle BLE phone pairing |
| swipe left / right | switch screens (status, menu) |
| wheel click in menu | open / select item |
| menu item 新会话 | start a new conversation, memory kept |

The wheel emotes are cosmetic only — they never touch the audio path, so it is safe to play with
them while a conversation is open. `qca> emote pet|dizzy|look|sleepy` replays one from the serial
console.

### Shutdown animation, and how to see it without powering off

Powering off plays a 1.5 s goodbye on the avatar (blink, sink, then the pixels dissolve with an
ordered dither) while the backlight fades on the same clock; the rail is cut 0.2 s after it finishes.
Two serial console commands help when you cannot watch the screen:

| Command | What it does |
|---|---|
| `bye` | plays the goodbye animation and dumps the 64×64 avatar grid as ASCII (`@dump k=… cut=… lit=…/4096` plus a 32×32 picture: `#` full, `+` partly dithered out, `.` dark). Does not power off. |
| `off` | the real thing — animation, then power off. |
| `emote pet\|dizzy\|look\|sleepy` | replays one wheel emote and dumps ASCII frames at 4 points (`active=` is the emote id, 0 once it has ended). |

`k` is the animation clock (0 → 1), `cut` the dither threshold; `lit` reaching 0 is what "fully
dissolved" looks like. Only the qca console sees these: muse's own console and the qca REPL share the
same UART and the REPL wins.

## 7. Memory and new sessions

A conversation id is a working context, not a memory. Memory lives in a Qoder **Memory Store** bound
to the `(identity, template)` pair, so it survives new conversations and reboots as long as you keep
using the same identity and template.

* **New conversation on the device**: *Settings → QODER → 开新会话*, the wheel menu item **新会话**,
or the serial console `new`. The firmware stops the WebSocket first (a TLS handshake for the REST call
needs ~20 KB of contiguous internal RAM, which is what is left while realtime is up), POSTs
`/api/v1/forward/realtime/conversations`, stores the new id in NVS, then reconnects.
* **Identity / template**: `ids` prints them, `ids idn_… tmpl_…` writes them to NVS. They are only
needed for *creating* conversations — an existing conversation id works without them.
* **Make sure memory can be written**: the template needs `Read` and `Write` in `enabled_tools`, and
the system prompt should ask it to record durable facts. `provision.mjs` does both. Verify with
`curl -H "Authorization: Bearer $QODER_PAT" https://api.qoder.com/api/v1/forward/templates/tmpl_…`.
* **Check the store**: `GET /api/v1/forward/identities/idn_…/templates/tmpl_…/memory_stores` lists it,
and `GET /api/v1/forward/memory_stores/memstore_…` shows `entry_count` / `total_size` / `updated_at`
so you can see it grow. The read-a-single-entry endpoint is not documented publicly; ask the agent
"你还记得我叫什么吗" instead. A conversation that just ended may still be queued for
consolidation, so recent facts can take a minute to appear.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| BLE chooser is empty | page filters by name prefix; the setup service UUID is not advertised. Use *Show all devices*, or check `CONFIG_HOMEHUB_BLE_NAME_PREFIX` |
| `跳过连接：Qoder 地址或 token 为空` | no token or URL: fill Settings → QODER |
| `ws=down` forever | wrong conversation id, expired PAT, or the device cannot reach `api.qoder.com` on 443 (check captive portal / proxy) |
| Token rejected | PATs are host-specific: a token minted for `api.qoder.com` does not work against the `.cn` endpoint |
| Audio but no reply | server-side VAD never fired: raise mic gain (*Settings → MIC GAIN*) or speak closer |
| Reply cut off | screen sleep is fine, but Wi-Fi power save can drop frames: keep the device on 5 V USB |
| `开新会话` fails, `开新会话缺配置` | identity/template missing: run `ids idn_… tmpl_…` over the console |
| `开新会话失败 … HTTP 4xx` | PAT expired, or the identity/template pair does not belong to it |
| It forgot everything | memory needs the template's `Read`/`Write` tools; also confirm both sessions used the same identity *and* template |

## Cost

Hardware only: SenseCAP Watcher ≈ **$54.90–$62.34**. Everything in this repository is free to build
and flash. The voice service is metered by Qoder on your own account — check the dashboard after the
first session instead of trusting an estimate.

## 中文速记

1. 刷固件：`idf.py -B build-muse-sensecap-watcher ... build / flash`。
2. 建 token：打开 <https://qoder.com/cloud/pat-keys>（国内站 <https://qoder.com.cn/cloud/pat-keys>），新建 PAT（`pt-...`）。
3. 建会话：`QODER_ACCESS_TOKEN=pt-... node esp32/tools/qca/provision.mjs`，记下 conversation id。
4. 配网：`esp32/tools/serve_setup.sh` 起页面 → 手机 Chrome 连 `Qoduck-xxxx` → 填 SSID/密码/Token。
5. 验证：串口看 `wifi=up ws=up`，设置页显示“已连接”，直接说话即可。
6. 换会话：设置 → QODER → 开新会话，或菜单「新会话」，或串口 `new`；记忆不会丢（挂在身份+模板上）。
7. 花费：只有硬件钱，Watcher 约 $55–62；语音服务按 Qoder 账户实际用量计费，看后台。
