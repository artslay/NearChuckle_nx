#pragma once
// ─── Fade from the Viridite reveal into the game ────────────────────────────
//
// The boot sequence ends on black, and the game's own first frame appears
// abruptly on the next swap. This draws a black quad over the game's frame at
// a falling alpha for the first fraction of a second, so the hand-off reads as
// one continuous shot rather than two screens spliced together.
//
// It runs inside the game's GL context, mid-frame, with the game's pipeline
// state live — so every piece of state it touches is saved and restored.

#include <stdint.h>

// Start fading in. Call once, immediately before the game's first frame.
void bootFadeBegin(float seconds);

// True while there is still something to draw.
bool bootFadeActive(void);

// Draw the overlay, if active. Call just before presenting each frame; cheap
// and self-disabling once the fade is over.
void bootFadeDraw(void);
