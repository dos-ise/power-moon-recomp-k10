// The setup's background (backdrop.h): a calm night sky for the menu - a charcoal gradient, a few twinkling stars and
// the project's crescent moon (theme.h) in the accent red, drawn faintly. Original artwork for this project (no game
// assets); scales with the window, no image files.
#include "backdrop.h"
#include "theme.h"

#include <math.h>

namespace {

const int W = 320, H = 180;

struct Px {
    ImDrawList *dl;
    float s, ox, oy;  // scale, origin
    void rect(float x, float y, float w, float h, ImU32 c) const {
        if (w <= 0 || h <= 0) return;
        dl->AddRectFilled(ImVec2(ox + floorf(x) * s, oy + floorf(y) * s), ImVec2(ox + floorf(x + w) * s, oy + floorf(y + h) * s), c);
    }
};

ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

// the sky's two ends (theme.h's panel colours)
const ImU32 SKY_TOP = rgb(18, 19, 22), SKY_BOTTOM = rgb(30, 31, 36);

// stars: (x, y, size, twinkle phase); a fixed field, kept off the card's middle
const float STARS[][4] = {
    {16, 20, 1, 0.0f}, {48, 10, 1, 1.3f}, {86, 28, 2, 2.1f}, {118, 12, 1, 0.7f},
    {150, 24, 1, 1.9f}, {196, 8, 2, 0.4f}, {234, 22, 1, 2.6f}, {300, 14, 1, 1.1f},
    {312, 44, 1, 0.2f}, {8, 58, 1, 2.9f}, {42, 46, 1, 1.6f}, {70, 64, 2, 0.9f},
    {286, 66, 1, 2.3f}, {22, 92, 1, 0.5f}, {300, 98, 1, 1.8f}, {6, 128, 1, 2.4f},
    {38, 148, 1, 0.8f}, {312, 150, 1, 1.4f}, {94, 118, 1, 3.0f}, {252, 128, 1, 0.1f},
};

}  // namespace

void backdrop_draw(ImDrawList *dl, ImVec2 p0, ImVec2 size, double t) {
    float s = fmaxf(size.x / W, size.y / H);
    Px p{dl, s, p0.x + (size.x - W * s) * 0.5f, p0.y + (size.y - H * s) * 0.5f};
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);

    // the sky: a smooth charcoal gradient, top to bottom
    dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + size.x, p0.y + size.y), SKY_TOP, SKY_TOP, SKY_BOTTOM, SKY_BOTTOM);

    // stars, twinkling slowly
    for (auto &st : STARS) {
        float a = 0.30f + 0.25f * (0.5f + 0.5f * sinf((float)t * 1.4f + st[3]));
        p.rect(st[0], st[1], st[2], st[2], rgb(242, 242, 245, (int)(255 * a)));
    }

    // the crescent moon (theme.h), faint, top right - behind the card
    float ms = 130 * s;
    ImVec2 mc(p.ox + 280 * s, p.oy + 42 * s);
    // the sky's colour at the moon's centre, so the cut-out blends into the gradient
    float ky = (mc.y - p0.y) / size.y;
    ImU32 sky = rgb((int)(18 + 12 * ky), (int)(19 + 12 * ky), (int)(22 + 14 * ky));
    theme_crescent(dl, ImVec2(mc.x - ms / 2, mc.y - ms / 2), ms, theme_u32(kAccentV, 0.32f), sky);

    dl->PopClipRect();
}