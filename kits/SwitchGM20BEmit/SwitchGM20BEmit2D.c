// SPDX-License-Identifier: MIT
#include "SwitchGM20BEmit2D.h"

#include "nv_push_cl902d.h"

static void destination(struct nv_push *p, const struct switch_gm20b_2d_surface *s)
{
    P_MTHD(p, NV902D, SET_DST_FORMAT);
    P_INLINE_DATA(p, s->format);
    if (s->pitch) {
        P_INLINE_DATA(p, NV902D_SET_DST_MEMORY_LAYOUT_V_PITCH);
        P_MTHD(p, NV902D, SET_DST_PITCH);
        P_INLINE_DATA(p, s->pitch_bytes);
    } else {
        P_INLINE_DATA(p, NV902D_SET_DST_MEMORY_LAYOUT_V_BLOCKLINEAR);
        P_INLINE_DATA(p, (uint32_t)s->block_height_log2 << 4);
        P_INLINE_DATA(p, 1);
        P_IMMD(p, NV902D, SET_DST_LAYER, 0);
        P_MTHD(p, NV902D, SET_DST_WIDTH);
    }
    P_INLINE_DATA(p, s->width);
    P_INLINE_DATA(p, s->height);
    P_INLINE_DATA(p, (uint32_t)(s->address >> 32));
    P_INLINE_DATA(p, (uint32_t)s->address);
}

void switch_gm20b_2d_fill(struct nv_push *p, const struct switch_gm20b_2d_surface *dst, uint32_t color, uint32_t x0,
                          uint32_t y0, uint32_t x1, uint32_t y1)
{
    P_IMMD(p, NV902D, SET_DST_COLOR_RENDER_TO_ZETA_SURFACE, V_FALSE);
    destination(p, dst);
    P_IMMD(p, NV902D, SET_OPERATION, V_SRCCOPY);
    P_IMMD(p, NV902D, SET_COLOR_KEY_ENABLE, V_FALSE);
    P_IMMD(p, NV902D, SET_RENDER_ENABLE_C, MODE_TRUE);
    P_MTHD(p, NV902D, SET_CLIP_X0);
    P_INLINE_DATA(p, 0);
    P_INLINE_DATA(p, 0);
    P_INLINE_DATA(p, dst->width);
    P_INLINE_DATA(p, dst->height);
    P_IMMD(p, NV902D, SET_CLIP_ENABLE, V_TRUE);
    P_MTHD(p, NV902D, RENDER_SOLID_PRIM_MODE);
    P_INLINE_DATA(p, NV902D_RENDER_SOLID_PRIM_MODE_V_RECTS);
    P_INLINE_DATA(p, dst->format);
    P_INLINE_DATA(p, color);
    P_MTHD(p, NV902D, RENDER_SOLID_PRIM_POINT_SET_X(0));
    P_INLINE_DATA(p, x0);
    P_INLINE_DATA(p, y0);
    P_INLINE_DATA(p, x1);
    P_INLINE_DATA(p, y1);
    P_IMMD(p, NV902D, SET_CLIP_ENABLE, V_FALSE);
}
