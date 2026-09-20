#pragma once
// ─── The Viridite reveal ────────────────────────────────────────────────────
//
// Native port of the closing beat of the Origin Trailer (kept in the launcher
// repo as "Viridite Origin Trailer/Viridite Origin Trailer.dc.html", 24.5s
// onward): the mark turns over, drops, splits down its seam, throws shards,
// and the game's own icon rises out of the gap.
//
// The trailer is hand-drawn — flat facets, ink outlines, pencil hatching, and
// every line redrawn on a 12fps "boil" so nothing sits still. That look is the
// point, so this draws vectors rather than blitting a logo: polygons, strokes
// and hatch lines, jittered per boil frame from the same hash the trailer uses.
// It is also why there is no video file. A decoder plus a 1280x720 clip would
// cost more memory at the tightest moment of the run than the drawing does,
// could not carry the launching game's own icon, and would be locked to one
// length — this stretches to whatever the jingle turns out to be.
//
// Drawn into a 2x supersample target and scaled down, because none of SDL's
// 2D primitives are anti-aliased and a 3.6px ink line at 720p without it reads
// as a staircase. If that target cannot be allocated it draws direct, aliased
// but complete — this runs while the guest heap and the game's textures are
// both live, so it must survive being told no.

#include <SDL2/SDL.h>

namespace reveal {

// Draw one frame of the reveal. `t` runs 0..1 across the whole thing; `icon`
// is the launching game's icon, or null (the mark still breaks, nothing rises).
// Draws artwork only — the caption and wordmark are the caller's, so they can
// use its glyph cache and be drawn at native resolution rather than through
// the supersample.
void draw(SDL_Renderer* r, float t, SDL_Texture* icon);

// The caption fades in once the icon has finished rising, and holds until the
// dip to black. 0 while there is nothing to say yet.
float captionAlpha(float t);

// The last stretch dips to black, which bootFadeDraw() then lifts back off
// over the game's first frames.
float blackAlpha(float t);

// Where the caption sits, in the 1280x720 design space.
constexpr int CAPTION_Y = 512;

// Release the supersample target. The reveal happens once per run, and 14MB
// of render target is not worth holding for the rest of the session.
void release(void);

}  // namespace reveal
