// ─── Install status, published for the HOME-menu overlay ────────────────────
// The overlay is a separate process (an overlay applet under nx-ovlloader), so
// it cannot see any of the loader's state directly. This writes the little that
// it needs to a fixed path, and nothing else in the Core has to know it exists.
//
// The format is deliberately boring: a version line then key=value, one per
// line, ASCII. It is rewritten whole every time rather than appended, so the
// file never grows and a reader always gets a complete record or nothing.
//
// Writes are rate-limited to ~4/s and skipped when nothing changed. Install
// progress is reported from a loop that already competes with SD card I/O for
// the extraction itself, and this must not become a second source of stalls.
#include "compat/loader.h"

#include <switch.h>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace {

constexpr char kDir[]  = "sdmc:/switch/Viridite";
constexpr char kPath[] = "sdmc:/switch/Viridite/install_status.txt";
constexpr char kTmp[]  = "sdmc:/switch/Viridite/install_status.tmp";

char     g_last_stage[96] = {};
char     g_last_state[16] = {};
int      g_last_pct       = -1;
uint64_t g_last_write_t   = 0;

bool throttled() {
    uint64_t now  = armGetSystemTick();
    uint64_t freq = armGetSystemTickFreq();
    if (g_last_write_t && (now - g_last_write_t) < freq / 4) return true;   // 250ms
    g_last_write_t = now;
    return false;
}

}  // namespace

void installStatusWrite(const char* state, const char* pkg, const char* name,
                        const char* stage, int pct) {
    if (!state) return;
    if (!pkg)   pkg   = "";
    if (!name)  name  = "";
    if (!stage) stage = "";
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;

    // Nothing new to say — don't touch the card.
    const bool same = pct == g_last_pct &&
                      strncmp(stage, g_last_stage, sizeof(g_last_stage)) == 0 &&
                      strncmp(state, g_last_state, sizeof(g_last_state)) == 0;
    if (same) return;

    // A terminal state must always land, even if it arrives inside the window
    // of the previous write — it's the one the overlay waits for to stop
    // drawing, and dropping it would leave a progress card up forever.
    const bool terminal = strcmp(state, "done") == 0 || strcmp(state, "error") == 0 ||
                          strcmp(state, "idle") == 0;
    if (!terminal && throttled()) return;

    mkdir(kDir, 0777);

    // Write to a temp path and rename over the real one, so the overlay can
    // never read a half-written file: it either sees the previous record or
    // the new one.
    FILE* f = fopen(kTmp, "w");
    if (!f) return;
    fprintf(f,
            "v1\n"
            "state=%s\n"
            "pct=%d\n"
            "pkg=%s\n"
            "name=%s\n"
            "stage=%s\n",
            state, pct, pkg, name, stage);
    fclose(f);
    remove(kPath);
    rename(kTmp, kPath);

    snprintf(g_last_stage, sizeof(g_last_stage), "%s", stage);
    snprintf(g_last_state, sizeof(g_last_state), "%s", state);
    g_last_pct = pct;
}

void installStatusClear(void) {
    installStatusWrite("idle", "", "", "", 0);
}
