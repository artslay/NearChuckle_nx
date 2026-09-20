// ─── Controller-guide labelling ──────────────────────────────────────────────
// Draws "what this button does" onto the controller diagram that gets patched
// into a game's own help screen, so the picture shows the real mapping instead
// of leaving someone to work it out by pressing things.
//
// Two tables drive it, deliberately kept apart:
//
//   BUTTON POSITIONS depend on the DIAGRAM  — where R sits on a Pro Controller
//                                             is nothing to do with any game
//   BUTTON MEANINGS  depend on the GAME     — R is "gas" in Hill Climb Racing
//                                             and something else elsewhere
//
// so adding a game means one row of meanings, and adding a controller diagram
// means one row of positions, and neither disturbs the other.

#include "compat/loader.h"
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <cstring>
#include <cmath>

extern void compatLog(const char* msg);
extern void compatLogFmt(const char* fmt, ...);

namespace {

// Viridite accent, matching the launcher's C_OK / C_RIM.
const SDL_Color kInk    = { 17,  32,  24, 255};   // label text
const SDL_Color kAccent = {  0, 170,  80, 255};   // callout line + ring
const SDL_Color kPill   = {205, 244, 224, 240};   // label background

// Where each labelled control sits on a given diagram, in normalised 0..1
// coordinates so it survives the rescale to whatever size the game's original
// asset was. `side` picks which way the callout text runs, so labels don't
// spill off the edge of the image.
enum class Side { Left, Right };
struct ButtonSpot { GuideButton button; float nx, ny; Side side; };

struct DiagramLayout {
    GuideController controller;
    const ButtonSpot* spots;
    int count;
};

// Positions read off the bundled artwork. Shoulders sit on the top edge; face
// buttons and the D-pad are on the front.
const ButtonSpot kProSpots[] = {
    {GuideButton::L,      0.20f, 0.06f, Side::Left },
    {GuideButton::R,      0.80f, 0.06f, Side::Right},
    {GuideButton::DPad,   0.34f, 0.62f, Side::Left },
    {GuideButton::Face,   0.72f, 0.32f, Side::Right},
    {GuideButton::Plus,   0.60f, 0.24f, Side::Right},
};
const ButtonSpot kHandheldSpots[] = {
    {GuideButton::L,      0.06f, 0.05f, Side::Left },
    {GuideButton::R,      0.94f, 0.05f, Side::Right},
    {GuideButton::DPad,   0.07f, 0.55f, Side::Left },
    {GuideButton::Face,   0.93f, 0.30f, Side::Right},
    {GuideButton::Plus,   0.88f, 0.10f, Side::Right},
};
// Sideways Joy-Cons: SL/SR run along the top edge and stand in for L/R.
const ButtonSpot kJoyLeftSpots[] = {
    {GuideButton::L,      0.30f, 0.05f, Side::Left },
    {GuideButton::R,      0.70f, 0.05f, Side::Right},
    {GuideButton::DPad,   0.58f, 0.55f, Side::Right},
};
const ButtonSpot kJoyRightSpots[] = {
    {GuideButton::L,      0.30f, 0.05f, Side::Left },
    {GuideButton::R,      0.70f, 0.05f, Side::Right},
    {GuideButton::Face,   0.78f, 0.55f, Side::Right},
};
const ButtonSpot kJoyDualSpots[] = {
    {GuideButton::L,      0.12f, 0.04f, Side::Left },
    {GuideButton::R,      0.88f, 0.04f, Side::Right},
    {GuideButton::DPad,   0.16f, 0.55f, Side::Left },
    {GuideButton::Face,   0.84f, 0.35f, Side::Right},
};

const DiagramLayout kLayouts[] = {
    {GuideController::Pro,        kProSpots,      (int)(sizeof kProSpots      / sizeof *kProSpots)},
    {GuideController::Handheld,   kHandheldSpots, (int)(sizeof kHandheldSpots / sizeof *kHandheldSpots)},
    {GuideController::JoyLeft,    kJoyLeftSpots,  (int)(sizeof kJoyLeftSpots  / sizeof *kJoyLeftSpots)},
    {GuideController::JoyRight,   kJoyRightSpots, (int)(sizeof kJoyRightSpots / sizeof *kJoyRightSpots)},
    {GuideController::JoyDual,    kJoyDualSpots,  (int)(sizeof kJoyDualSpots  / sizeof *kJoyDualSpots)},
};

const DiagramLayout* layoutFor(GuideController c) {
    for (const auto& l : kLayouts) if (l.controller == c) return &l;
    return nullptr;
}

void fillRect(SDL_Surface* s, int x, int y, int w, int h, SDL_Color c) {
    SDL_Rect r{x, y, w, h};
    SDL_FillRect(s, &r, SDL_MapRGBA(s->format, c.r, c.g, c.b, c.a));
}

// Ring around the control being pointed at, so the label is unambiguous even
// where two controls sit close together.
void drawRing(SDL_Surface* s, int cx, int cy, int radius, SDL_Color c) {
    for (int t = 0; t < 3; t++) {                 // 3px stroke
        int rr = radius - t;
        if (rr <= 0) break;
        for (int deg = 0; deg < 360; deg++) {
            float a = (float)deg * 3.14159265f / 180.0f;
            int px = cx + (int)lrintf(cosf(a) * rr);
            int py = cy + (int)lrintf(sinf(a) * rr);
            if (px >= 0 && py >= 0 && px < s->w && py < s->h)
                fillRect(s, px, py, 1, 1, c);
        }
    }
}

}  // namespace

// Draws every label a game defines onto `img`, in place. Missing font or an
// unknown controller just means no labels — the diagram is still correct, it
// simply isn't annotated, which is far better than refusing to patch it.
void guideDrawLabels(SDL_Surface* img, GuideController controller,
                     const GuideLabel* labels, int labelCount) {
    if (!img || !labels || labelCount <= 0) return;
    const DiagramLayout* layout = layoutFor(controller);
    if (!layout) { compatLog("guide: no label layout for this controller — diagram left unannotated"); return; }

    // Scale type to the image so labels stay readable whatever size the game's
    // original asset was.
    int pt = std::max(11, img->h / 16);
    TTF_Font* font = nullptr;
    PlFontData fd = {};
    if (plGetSharedFontByType(&fd, PlSharedFontType_Standard) == 0 && fd.address && fd.size) {
        SDL_RWops* rw = SDL_RWFromConstMem(fd.address, (int)fd.size);
        if (rw) font = TTF_OpenFontRW(rw, 1, pt);
    }
    if (!font) font = TTF_OpenFont("romfs:/fonts/DejaVuSans.ttf", pt);
    if (!font) { compatLog("guide: no font — diagram left unannotated"); return; }
    TTF_SetFontStyle(font, TTF_STYLE_BOLD);

    int drawn = 0;
    for (int i = 0; i < labelCount; i++) {
        const GuideLabel& lb = labels[i];
        if (!lb.text || !lb.text[0]) continue;

        const ButtonSpot* spot = nullptr;
        for (int k = 0; k < layout->count; k++)
            if (layout->spots[k].button == lb.button) { spot = &layout->spots[k]; break; }
        if (!spot) continue;                      // this diagram has no such control

        int cx = (int)(spot->nx * img->w);
        int cy = (int)(spot->ny * img->h);
        int ring = std::max(8, img->h / 22);
        drawRing(img, cx, cy, ring, kAccent);

        SDL_Surface* txt = TTF_RenderUTF8_Blended(font, lb.text, kInk);
        if (!txt) continue;

        // Pill behind the text, offset to whichever side keeps it on-image.
        const int padX = 8, padY = 4, gap = ring + 6;
        int pw = txt->w + padX * 2, ph = txt->h + padY * 2;
        int px = (spot->side == Side::Right) ? cx + gap : cx - gap - pw;
        int py = cy - ph / 2;
        if (px < 2) px = 2;
        if (px + pw > img->w - 2) px = img->w - 2 - pw;
        if (py < 2) py = 2;
        if (py + ph > img->h - 2) py = img->h - 2 - ph;

        fillRect(img, px, py, pw, ph, kPill);
        fillRect(img, px, py, pw, 2, kAccent);            // accent edge, top
        fillRect(img, px, py + ph - 2, pw, 2, kAccent);   // and bottom

        SDL_Rect dst{px + padX, py + padY, txt->w, txt->h};
        SDL_SetSurfaceBlendMode(txt, SDL_BLENDMODE_BLEND);
        SDL_BlitSurface(txt, nullptr, img, &dst);
        SDL_FreeSurface(txt);
        drawn++;
    }

    TTF_CloseFont(font);
    compatLogFmt("guide: annotated %d control(s) on the controller diagram", drawn);
}
