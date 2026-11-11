// SPDX-License-Identifier: MIT
// See raw_turned_pass.h.
#include "raw_turned_pass.h"

namespace raw {
namespace {

// The scratch backings' tags: a mirror's at + slot, a colour stand-in's at + 8 + slot, the depth stand-in's at + 16
constexpr uint32_t kTurnTag = 48;

}  // namespace

Texture *TurnedPass::standIn(CommandBuffer &rc, Texture &t, uint32_t tag, bool loaded, const char **why)
{
    Texture *s = rc.turnScratch(t.zeta ? t.format : turnCopyFormat(t.bpp), rot_.planeWidth(), rot_.planeHeight(), tag);
    if (!s) {
        *why = "no plane-shaped stand-in for an attachment beside a turned one";
        return nullptr;
    }
    if (loaded && !rc.turnCopy(*s, t, frameToPlane(rot_), why))
        return nullptr;
    return s;
}

bool TurnedPass::begin(CommandBuffer &rc, PassDesc &p, bool draws, const char **why)
{
    backs_.clear();
    held_.clear();
    rot_ = {};
    for (uint32_t j = 0; j < p.count && j < 8 && !rot_.turned(); j++)
        if (const Texture *t = p.color[j].texture; t && t->rotation)
            rot_ = frameOf(*t);
    if (!rot_.turned())
        return true;
    if (rot_.turn == 4)
        rot_.turn = 1;
    for (uint32_t j = 0; j < p.count && j < 8; j++) {
        ColorAttachment &a = p.color[j];
        Texture *t = a.texture;
        if (!t)
            continue;
        if (t->rotation == 4) {
            if (draws && a.load == ColorAttachment::Load) {
                Texture *temp = rc.turnScratch(turnCopyFormat(t->bpp), t->width, t->height, kTurnTag + j);
                if (!temp) {
                    *why = "no scratch for a rotation-4 plane's load";
                    return false;
                }
                BlitEncoder *b = rc.blit();
                b->copyTexels(*t, 0, 0, 0, *temp, 0, 0, 0, t->width, t->height);
                b->end();
                if (!rc.turnCopy(*t, *temp, planeMirror(t->width), why))
                    return false;
            }
            held_.push_back(*t);
            held_.back().rotation = 1;
            a.texture = &held_.back();
            continue;
        }
        if (t->rotation || t->memoryless || (a.load != ColorAttachment::Load && !a.store))
            continue;
        Texture *s = standIn(rc, *t, kTurnTag + 8 + j, a.load == ColorAttachment::Load, why);
        if (!s)
            return false;
        held_.push_back(*s);
        held_.back().format = t->format;
        held_.back().rotation = rot_.turn;
        a.texture = &held_.back();
        if (a.store)
            backs_.push_back({t, s});
    }
    DepthStencilAttachment &z = p.depthStencil;
    if (z.texture && !z.texture->memoryless && (z.loaded || z.stored)) {
        Texture *s = standIn(rc, *z.texture, kTurnTag + 16, z.loaded, why);
        if (!s)
            return false;
        if (z.stored)
            backs_.push_back({z.texture, s});
        held_.push_back(*s);
        held_.back().rotation = rot_.turn;
        z.texture = &held_.back();
    }
    return true;
}

bool TurnedPass::end(CommandBuffer &rc, const char **why)
{
    for (const Back &b : backs_)
        if (!rc.turnCopy(*b.into, *b.standIn, planeToFrame(rot_), why))
            return false;
    backs_.clear();
    held_.clear();
    return true;
}

}  // namespace raw
