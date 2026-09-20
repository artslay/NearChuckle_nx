#include "compat/achievements.h"
#include "compat/loader.h"
#include "compat/toast.h"

#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ─── On-card layout ─────────────────────────────────────────────────────────
//
//   sdmc:/Viridite/achievements/<pkg>.catalogue   written by the launcher
//   sdmc:/Viridite/achievements/<pkg>.map         written by the launcher
//   sdmc:/Viridite/achievements/<pkg>.unlocks     written by us
//   sdmc:/Viridite/achievements/<pkg>/<n>.png     icons, written by the launcher
//
// Tab-separated text, not JSON. The Core has no JSON parser and does not want
// one: these files are written by one program and read by another, both in
// this project, and a format a human can read in a text editor on the card is
// worth more here than a format a library can validate. It is also the same
// choice the rest of the project already made for .launch_apk and .launch_input.

namespace {

constexpr const char* DIR = "sdmc:/Viridite/achievements";

struct Def {                        // one row of the catalogue
    std::string id;                 // GPGS id, empty until the map supplies it
    std::string name, desc, icon;
    int   points = 0;
    float rarity = 0.0f;
    bool  secret = false;
    int   totalSteps = 0;           // 0 = not a stepped achievement
};

struct State {
    int  steps = 0;
    bool unlocked = false;
    bool revealed = false;
};

std::string          g_pkg;
std::vector<Def>     g_defs;
std::vector<State>   g_state;       // parallel to g_defs
bool                 g_dirty = false;
bool                 g_ready = false;

// Ids the game unlocked that are not in the catalogue. Kept so the unlock is
// still recorded and still shown — an earned achievement is not allowed to
// vanish because our metadata was incomplete.
std::vector<Def>     g_unknown;
std::vector<State>   g_unknownState;

std::string pathFor(const char* ext) {
    return std::string(DIR) + "/" + g_pkg + "." + ext;
}

// Split a tab-separated line in place. Returns the field count.
int split(char* line, char** out, int max) {
    int n = 0;
    char* p = line;
    out[n++] = p;
    while (*p && n < max) {
        if (*p == '\t') { *p = '\0'; out[n++] = p + 1; }
        p++;
    }
    // Trim the trailing newline off the last field.
    for (int i = 0; i < n; i++) {
        char* e = out[i] + strlen(out[i]);
        while (e > out[i] && (e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
    }
    return n;
}

// catalogue: index <TAB> name <TAB> description <TAB> points <TAB> rarity
//            <TAB> secret <TAB> totalSteps <TAB> iconFile
void loadCatalogue(void) {
    FILE* f = fopen(pathFor("catalogue").c_str(), "r");
    if (!f) {
        compatLog("achievements: no catalogue on the card — unlocks will show their id");
        return;
    }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char* fld[8] = {};
        const int n = split(line, fld, 8);
        if (n < 3) continue;
        Def d;
        d.name   = fld[1];
        d.desc   = n > 2 ? fld[2] : "";
        d.points = n > 3 ? atoi(fld[3]) : 0;
        d.rarity = n > 4 ? (float)atof(fld[4]) : 0.0f;
        d.secret = n > 5 && fld[5][0] == '1';
        d.totalSteps = n > 6 ? atoi(fld[6]) : 0;
        if (n > 7 && fld[7][0])
            d.icon = std::string(DIR) + "/" + g_pkg + "/" + fld[7];
        g_defs.push_back(std::move(d));
    }
    fclose(f);
    g_state.resize(g_defs.size());
    compatLogFmt("achievements: catalogue has %zu entries", g_defs.size());
}

// map: gpgsId <TAB> catalogueIndex  (1-based, matching the catalogue's own)
void loadMap(void) {
    FILE* f = fopen(pathFor("map").c_str(), "r");
    if (!f) {
        compatLog("achievements: no id map — the catalogue cannot be matched to unlocks");
        return;
    }
    char line[512];
    int  joined = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char* fld[2] = {};
        if (split(line, fld, 2) < 2) continue;
        const int idx = atoi(fld[1]) - 1;
        if (idx >= 0 && idx < (int)g_defs.size()) { g_defs[idx].id = fld[0]; joined++; }
    }
    fclose(f);
    compatLogFmt("achievements: %d of %zu entries have a GPGS id", joined, g_defs.size());
}

// unlocks: gpgsId <TAB> steps <TAB> unlocked <TAB> revealed
void loadUnlocks(void) {
    FILE* f = fopen(pathFor("unlocks").c_str(), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char* fld[4] = {};
        const int n = split(line, fld, 4);
        if (n < 3) continue;

        State st;
        st.steps    = atoi(fld[1]);
        st.unlocked = fld[2][0] == '1';
        st.revealed = n > 3 && fld[3][0] == '1';

        bool placed = false;
        for (size_t i = 0; i < g_defs.size(); i++)
            if (g_defs[i].id == fld[0]) { g_state[i] = st; placed = true; break; }
        if (!placed) {
            Def d; d.id = fld[0];
            g_unknown.push_back(std::move(d));
            g_unknownState.push_back(st);
        }
    }
    fclose(f);
}

// Resolve an id to a slot. Adds an "unknown" slot rather than failing, so the
// game is never told its unlock did not happen.
bool slotFor(const char* id, Def** def, State** st) {
    if (!id || !*id) return false;
    for (size_t i = 0; i < g_defs.size(); i++)
        if (g_defs[i].id == id) { *def = &g_defs[i]; *st = &g_state[i]; return true; }
    for (size_t i = 0; i < g_unknown.size(); i++)
        if (g_unknown[i].id == id) { *def = &g_unknown[i]; *st = &g_unknownState[i]; return true; }

    Def d; d.id = id;
    g_unknown.push_back(std::move(d));
    g_unknownState.push_back(State{});
    compatLogFmt("achievements: '%s' is not in the catalogue — recording it anyway", id);
    *def = &g_unknown.back();
    *st  = &g_unknownState.back();
    return true;
}

// Hand it straight to the on-screen panel. Deliberately not a queue this
// module owns and somebody else drains — a queue with no guaranteed reader is
// a leak waiting for the one game that unlocks a hundred things at once.
// toast::show is safe from the JNI thread we are on; it builds and uploads
// later, on whichever thread holds the GL context.
void raiseToast(const Def* d) {
    char sub[160];
    if (d->points > 0 && d->rarity > 0.0f)
        snprintf(sub, sizeof sub, "%d pts  \u00b7  %.1f%% of players", d->points, (double)d->rarity);
    else if (d->points > 0)
        snprintf(sub, sizeof sub, "%d pts", d->points);
    else
        snprintf(sub, sizeof sub, "%s", d->desc.c_str());

    toast::show(d->name.empty() ? d->id.c_str() : d->name.c_str(),
                sub, d->icon.empty() ? nullptr : d->icon.c_str());
}

bool doUnlock(Def* d, State* st) {
    if (st->unlocked) return false;         // GPGS is idempotent; so are we
    st->unlocked = true;
    st->revealed = true;
    g_dirty = true;
    raiseToast(d);
    compatLogFmt("achievements: unlocked '%s' (%s)",
                 d->name.empty() ? d->id.c_str() : d->name.c_str(), d->id.c_str());
    achievements::flush();
    return true;
}

}  // namespace

namespace achievements {

void init(const char* pkg) {
    if (g_ready) return;
    g_ready = true;
    g_pkg = pkg ? pkg : "";
    if (g_pkg.empty()) return;

    mkdir("sdmc:/Viridite", 0777);
    mkdir(DIR, 0777);

    loadCatalogue();
    loadMap();
    loadUnlocks();
    compatLogFmt("achievements: %d of %d unlocked for %s",
                 unlockedCount(), totalCount(), g_pkg.c_str());
}

bool unlock(const char* gpgsId) {
    Def* d; State* st;
    if (!g_ready || !slotFor(gpgsId, &d, &st)) return false;
    return doUnlock(d, st);
}

bool increment(const char* gpgsId, int steps) {
    Def* d; State* st;
    if (!g_ready || steps <= 0 || !slotFor(gpgsId, &d, &st)) return false;
    if (st->unlocked) return false;
    st->steps += steps;
    g_dirty = true;
    if (d->totalSteps > 0 && st->steps >= d->totalSteps) return doUnlock(d, st);
    flush();
    return true;
}

bool setSteps(const char* gpgsId, int steps) {
    Def* d; State* st;
    if (!g_ready || !slotFor(gpgsId, &d, &st)) return false;
    if (st->unlocked || steps <= st->steps) return false;   // never moves backwards
    st->steps = steps;
    g_dirty = true;
    if (d->totalSteps > 0 && st->steps >= d->totalSteps) return doUnlock(d, st);
    flush();
    return true;
}

bool reveal(const char* gpgsId) {
    Def* d; State* st;
    if (!g_ready || !slotFor(gpgsId, &d, &st)) return false;
    if (st->revealed) return false;
    st->revealed = true;
    g_dirty = true;
    flush();
    return true;
}

bool isUnlocked(const char* gpgsId) {
    Def* d; State* st;
    if (!g_ready || !slotFor(gpgsId, &d, &st)) return false;
    return st->unlocked;
}

int unlockedCount(void) {
    int n = 0;
    for (const State& s : g_state)        if (s.unlocked) n++;
    for (const State& s : g_unknownState) if (s.unlocked) n++;
    return n;
}

int totalCount(void) { return (int)g_defs.size(); }

void flush(void) {
    if (!g_dirty || g_pkg.empty()) return;
    const std::string path = pathFor("unlocks");
    const std::string tmp  = path + ".tmp";

    // Written to a temporary and renamed. A console losing power mid-write is
    // ordinary here, and a half-written unlocks file would cost the player
    // everything they had earned rather than the one line in flight.
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) { compatLogFmt("achievements: cannot write %s", tmp.c_str()); return; }
    fprintf(f, "# gpgsId\tsteps\tunlocked\trevealed\n");
    for (size_t i = 0; i < g_defs.size(); i++) {
        if (g_defs[i].id.empty()) continue;
        if (!g_state[i].unlocked && !g_state[i].steps && !g_state[i].revealed) continue;
        fprintf(f, "%s\t%d\t%d\t%d\n", g_defs[i].id.c_str(), g_state[i].steps,
                g_state[i].unlocked ? 1 : 0, g_state[i].revealed ? 1 : 0);
    }
    for (size_t i = 0; i < g_unknown.size(); i++)
        fprintf(f, "%s\t%d\t%d\t%d\n", g_unknown[i].id.c_str(), g_unknownState[i].steps,
                g_unknownState[i].unlocked ? 1 : 0, g_unknownState[i].revealed ? 1 : 0);
    fclose(f);

    remove(path.c_str());
    rename(tmp.c_str(), path.c_str());
    g_dirty = false;
}

}  // namespace achievements
