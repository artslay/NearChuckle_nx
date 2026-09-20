#pragma once
// ─── The Viridite boot jingle ───────────────────────────────────────────────
//
// A standard MIDI file in romfs, rendered to PCM at launch and played through
// SDL's audio device. MIDI rather than a recording because the jingle is three
// seconds of struck bells: as notes it is a few hundred bytes and can be
// rewritten by editing tools/make_jingle.py, where as audio it would be a
// megabyte of WAV nobody could adjust.
//
// There is no soundfont involved. Bells are one of the few instruments that
// synthesise convincingly from first principles — a handful of inharmonic
// partials, each decaying at its own rate — so the renderer here *is* the
// instrument, and the MIDI program number selects which set of partials to
// use.

#include <stdint.h>

namespace jingle {

// Load romfs:/audio/viridite.mid, render it, and begin playback. Returns false
// if audio could not be opened or the file is missing — never fatal, the boot
// sequence just runs silently.
bool play(void);

// Length of the rendered audio in seconds, including the decay tail. 0 before
// play() succeeds. The boot animation uses this to match its own duration, so
// the picture and the sound finish together.
float length(void);

// Seconds elapsed since playback began, clamped to length().
float elapsed(void);

// Release the device and buffer. Safe to call if play() never succeeded.
void stop(void);

}  // namespace jingle
