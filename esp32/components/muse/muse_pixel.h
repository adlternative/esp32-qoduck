/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "muse_state.h"

/*
 * Procedural pixel-art renderer for the Muse character.
 *
 * Muse is drawn on a coarse MUSE_PX_W x MUSE_PX_H grid with a small palette,
 * ordered dithering and hard outlines, then blown up to the screen with
 * nearest-neighbour blocks so the pixels stay chunky.
 */

#define MUSE_PX_W 64
#define MUSE_PX_H 64

typedef struct {
    muse_mode_t mode;
    float t;         /* seconds since boot */
    float mode_t;    /* seconds in current mode */
    float level;     /* 0..1 live audio level */
    float happy;     /* 0..1 pet reaction */
} muse_pose_t;

/* Accent colour of a mode (for the surrounding UI), as 0xRRGGBB. */
uint32_t muse_pixel_accent(muse_mode_t mode);

/* Render one frame into Muse's own MUSE_PX_W x MUSE_PX_H grid. */
void muse_pixel_render(const muse_pose_t *pose);

/* Size (square, in screen pixels) muse_pixel_scale() blows the grid up to. */
void muse_pixel_set_size(int px);
/*
 * Write screen pixels [x0, x1] x [y0, y1] of the blown-up frame as RGB565,
 * stride_px apart. Cheap enough to call per display strip, so the full-size
 * image never has to exist in RAM.
 */
void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);

/*
 * Dump the current grid as ASCII ('#' lit, '.' empty), every other cell, so a
 * serial console can eyeball an animation without a screen or a screenshot.
 * Console-only debug aid; costs nothing when unused.
 */
void muse_pixel_dump_ascii(void);

/*
 * 关机告别动画（闭眼 → 低头 → 散布）。begin 后 render 会自行播 1.5 s；
 * progress 返回 0..1，-1 表示没在播。背光和头像要一起变暗，所以两边读同一个钟。
 */
void muse_pixel_goodbye_begin(void);
void muse_pixel_goodbye_end(void);
float muse_pixel_goodbye_progress(void);

/*
 * 滚轮彩蛋：按一下 / 双击 / 转动会给鸭子加一段小动作，叠在正常那一帧上，
 * 各自有自己的时钟，播完自动结束。没实现这个头像（回退到默认的）时
 * 调用方用弱符号判空即可。
 */
typedef enum {
    MUSE_EMOTE_NONE = 0,
    MUSE_EMOTE_PET,      /* 撸一下：蹦一下 + 两角飘心 */
    MUSE_EMOTE_DIZZY,    /* 转一圈 + 星星 + X 眼 */
    MUSE_EMOTE_LOOK,     /* 东张西望 */
    MUSE_EMOTE_SLEEPY,   /* 打瞌睡：闭眼 + Zzz */
} muse_emote_t;

void muse_pixel_emote(int emote);
int muse_pixel_emote_active(void);
