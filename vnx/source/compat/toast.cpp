#include "compat/toast.h"
#include "compat/loader.h"

#include <switch.h>
#include <GLES2/gl2.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

namespace {

// ─── Layout, in the game's 1280x720 screen space ────────────────────────────
constexpr int   PANEL_W  = 460;
constexpr int   PANEL_H  = 108;
constexpr int   ICON_PX  = 76;
constexpr int   PAD      = 16;
constexpr float SHOW_SECS = 4.2f;
constexpr float SLIDE     = 0.42f;      // in and out, seconds

// Bottom-right. Games put their HUD top-left and centre far more often than
// they put it here, and the bottom-right corner is also where the Switch's own
// system notifications appear — so it is the corner a player already reads as
// "the console talking, not the game".
constexpr int MARGIN_X = 28, MARGIN_Y = 28;

struct Pending {
    std::string title, subtitle, icon;
};

std::mutex           g_lock;
std::deque<Pending>  g_queue;           // written by JNI threads, read by GL

struct Panel {
    bool     ready = false;
    bool     failed = false;
    GLuint   prog = 0, tex = 0, vbo = 0;
    GLint    aPos = 8, aUV = 9, uRect = -1, uScreen = -1, uTex = -1, uAlpha = -1;
    int      texW = 0, texH = 0;
    uint64_t start = 0;
};
Panel g_p;

TTF_Font* openFont(int pt, bool bold) {
    PlFontData fd = {};
    TTF_Font*  f  = nullptr;
    if (plGetSharedFontByType(&fd, PlSharedFontType_Standard) == 0 && fd.size > 0) {
        SDL_RWops* rw = SDL_RWFromConstMem(fd.address, (int)fd.size);
        f = TTF_OpenFontRW(rw, 1, pt);
    }
    if (!f) { romfsInit(); f = TTF_OpenFont("romfs:/fonts/DejaVuSans.ttf", pt); }
    if (f && bold) TTF_SetFontStyle(f, TTF_STYLE_BOLD);
    return f;
}

void blitText(SDL_Surface* dst, TTF_Font* f, const char* s, SDL_Color c, int x, int y,
              int maxW) {
    if (!f || !s || !*s) return;
    SDL_Surface* t = TTF_RenderUTF8_Blended(f, s, c);
    if (!t) return;
    SDL_SetSurfaceBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_Rect src = {0, 0, t->w > maxW ? maxW : t->w, t->h};
    SDL_Rect d   = {x, y, src.w, src.h};
    SDL_BlitSurface(t, &src, dst, &d);
    SDL_FreeSurface(t);
}

void roundedPlate(SDL_Surface* s, SDL_Color body, int rad) {
    // Drawn by hand rather than through SDL_Renderer: there is no renderer here
    // — the game owns the GL context, and this is a CPU surface on its way to
    // being one texture.
    const Uint32 col = SDL_MapRGBA(s->format, body.r, body.g, body.b, body.a);
    for (int y = 0; y < s->h; y++) {
        int inset = 0;
        const int dy = (y < rad) ? rad - y : (y >= s->h - rad ? y - (s->h - rad) + 1 : 0);
        if (dy > 0) inset = rad - (int)sqrtf((float)(rad * rad - dy * dy > 0 ? rad * rad - dy * dy : 0));
        Uint32* row = (Uint32*)((Uint8*)s->pixels + y * s->pitch);
        for (int x = inset; x < s->w - inset; x++) row[x] = col;
    }
}

// Build the panel surface and upload it. Runs on the GL thread only.
bool build(const Pending& p) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, PANEL_W, PANEL_H, 32,
                                                    SDL_PIXELFORMAT_ABGR8888);
    if (!s) return false;
    SDL_memset(s->pixels, 0, (size_t)s->h * s->pitch);
    SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_NONE);

    roundedPlate(s, {12, 26, 18, 236}, 18);

    // A green edge down the leading side — enough Viridite to be recognisable
    // without a wordmark taking room the achievement's own name needs.
    //
    // Kept inside the corner radius. Run to y=4 and the bar's ends sit outside
    // the plate's rounded corners, which reads as two green pixels floating in
    // the game's frame.
    for (int y = 18; y < PANEL_H - 18; y++) {
        Uint32* row = (Uint32*)((Uint8*)s->pixels + y * s->pitch);
        for (int x = 0; x < 5; x++)
            row[x + 10] = SDL_MapRGBA(s->format, 0, 230, 118, 255);
    }

    int textX = PAD + 14;
    if (!p.icon.empty()) {
        if (SDL_Surface* ic = IMG_Load(p.icon.c_str())) {
            SDL_Surface* cv = SDL_ConvertSurfaceFormat(ic, SDL_PIXELFORMAT_ABGR8888, 0);
            SDL_FreeSurface(ic);
            if (cv) {
                SDL_SetSurfaceBlendMode(cv, SDL_BLENDMODE_BLEND);
                SDL_Rect d = {textX, (PANEL_H - ICON_PX) / 2, ICON_PX, ICON_PX};
                SDL_BlitScaled(cv, nullptr, s, &d);
                SDL_FreeSurface(cv);
                textX += ICON_PX + PAD;
            }
        }
    }

    TTF_Font* fT = openFont(24, true);
    TTF_Font* fS = openFont(17, false);
    const int wrapW = PANEL_W - textX - PAD;

    blitText(s, fS, "ACHIEVEMENT UNLOCKED", {0, 230, 118, 255}, textX, 18, wrapW);
    blitText(s, fT, p.title.c_str(),        {255, 255, 255, 255}, textX, 40, wrapW);
    if (!p.subtitle.empty())
        blitText(s, fS, p.subtitle.c_str(), {170, 200, 182, 255}, textX, 72, wrapW);

    if (fT) TTF_CloseFont(fT);
    if (fS) TTF_CloseFont(fS);

    if (!g_p.tex) glGenTextures(1, &g_p.tex);
    glBindTexture(GL_TEXTURE_2D, g_p.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s->w, s->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, s->pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_p.texW = s->w; g_p.texH = s->h;
    SDL_FreeSurface(s);
    return true;
}

GLuint compile(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(sh); return 0; }
    return sh;
}

bool ensureProgram(void) {
    if (g_p.prog) return true;
    if (g_p.failed) return false;

    static const char* kVS =
        "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;\n"
        "uniform vec4 uRect; uniform vec2 uScreen;\n"
        "void main() {\n"
        "  vec2 pix = uRect.xy + aPos * uRect.zw;\n"
        "  vec2 ndc = vec2(pix.x / uScreen.x * 2.0 - 1.0, 1.0 - pix.y / uScreen.y * 2.0);\n"
        "  gl_Position = vec4(ndc, 0.0, 1.0); vUV = aUV;\n"
        "}\n";
    static const char* kFS =
        "precision mediump float; varying vec2 vUV;\n"
        "uniform sampler2D uTex; uniform float uAlpha;\n"
        "void main() { gl_FragColor = texture2D(uTex, vUV) * uAlpha; }\n";

    GLuint vs = compile(GL_VERTEX_SHADER, kVS), fs = compile(GL_FRAGMENT_SHADER, kFS);
    if (!vs || !fs) { g_p.failed = true; return false; }
    g_p.prog = glCreateProgram();
    glAttachShader(g_p.prog, vs);
    glAttachShader(g_p.prog, fs);
    // High, unusual indices on purpose — see the header.
    glBindAttribLocation(g_p.prog, 8, "aPos");
    glBindAttribLocation(g_p.prog, 9, "aUV");
    glLinkProgram(g_p.prog);
    GLint linked = 0;
    glGetProgramiv(g_p.prog, GL_LINK_STATUS, &linked);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!linked) { glDeleteProgram(g_p.prog); g_p.prog = 0; g_p.failed = true; return false; }

    g_p.uRect   = glGetUniformLocation(g_p.prog, "uRect");
    g_p.uScreen = glGetUniformLocation(g_p.prog, "uScreen");
    g_p.uTex    = glGetUniformLocation(g_p.prog, "uTex");
    g_p.uAlpha  = glGetUniformLocation(g_p.prog, "uAlpha");

    static const float verts[] = { 0,0, 0,0,  1,0, 1,0,  0,1, 0,1,  1,1, 1,1 };
    glGenBuffers(1, &g_p.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_p.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

float elapsedSecs(void) {
    return (float)(armTicksToNs(armGetSystemTick() - g_p.start) / 1e9);
}

}  // namespace

namespace toast {

void show(const char* title, const char* subtitle, const char* iconPath) {
    Pending p;
    p.title    = title    ? title    : "";
    p.subtitle = subtitle ? subtitle : "";
    p.icon     = iconPath ? iconPath : "";
    if (p.title.empty()) return;
    std::lock_guard<std::mutex> g(g_lock);
    // A game that fires ten unlocks in one frame should not queue ten panels
    // deep and then narrate them for forty seconds over live play.
    if (g_queue.size() < 4) g_queue.push_back(std::move(p));
}

bool active(void) {
    std::lock_guard<std::mutex> g(g_lock);
    return g_p.ready || !g_queue.empty();
}

void draw(void) {
    if (g_p.failed) return;

    // Retire a finished panel and take the next, if any.
    if (g_p.ready && elapsedSecs() > SHOW_SECS) g_p.ready = false;
    if (!g_p.ready) {
        Pending next;
        {
            std::lock_guard<std::mutex> g(g_lock);
            if (g_queue.empty()) return;
            next = std::move(g_queue.front());
            g_queue.pop_front();
        }
        if (!ensureProgram()) return;
        if (!build(next)) { g_p.failed = true; return; }
        g_p.start = armGetSystemTick();
        g_p.ready = true;
    }

    const float t = elapsedSecs();
    // Slides in from off the right edge, holds, slides back out.
    float k = 1.0f;
    if (t < SLIDE)                   k = t / SLIDE;
    else if (t > SHOW_SECS - SLIDE)  k = (SHOW_SECS - t) / SLIDE;
    if (k < 0.0f) k = 0.0f; else if (k > 1.0f) k = 1.0f;
    const float eased = 1.0f - (1.0f - k) * (1.0f - k);

    const float w = (float)g_p.texW, h = (float)g_p.texH;
    const float restX = 1280.0f - MARGIN_X - w;     // parked
    const float offX  = 1280.0f;                    // fully off the right edge
    const float x = offX + (restX - offX) * eased;
    const float y = 720.0f - MARGIN_Y - h;

    // ── Save every piece of state this touches ──
    GLint prevProgram = 0, prevArrayBuf = 0, prevTex = 0, prevActiveTex = 0;
    GLint bSrcRGB = 0, bDstRGB = 0, bSrcA = 0, bDstA = 0;
    const GLboolean prevBlend = glIsEnabled(GL_BLEND);
    const GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean prevCull  = glIsEnabled(GL_CULL_FACE);
    const GLboolean prevScis  = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean prevSten  = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_BLEND_SRC_RGB, &bSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &bDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &bSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &bDstA);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    // Premultiplied: the fragment shader already multiplied by uAlpha, so the
    // source factor must be ONE or the panel darkens as it fades.
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(g_p.prog);
    glUniform4f(g_p.uRect, x, y, w, h);
    glUniform2f(g_p.uScreen, 1280.0f, 720.0f);
    glUniform1f(g_p.uAlpha, eased);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_p.tex);
    glUniform1i(g_p.uTex, 0);

    glBindBuffer(GL_ARRAY_BUFFER, g_p.vbo);
    glEnableVertexAttribArray(g_p.aPos);
    glEnableVertexAttribArray(g_p.aUV);
    glVertexAttribPointer(g_p.aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glVertexAttribPointer(g_p.aUV,  2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void*)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(g_p.aPos);
    glDisableVertexAttribArray(g_p.aUV);

    // ── Put it all back, exactly ──
    glUseProgram((GLuint)prevProgram);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArrayBuf);
    glActiveTexture((GLenum)prevActiveTex);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glBlendFuncSeparate((GLenum)bSrcRGB, (GLenum)bDstRGB, (GLenum)bSrcA, (GLenum)bDstA);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevCull)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
    if (prevScis)  glEnable(GL_SCISSOR_TEST);
    if (prevSten)  glEnable(GL_STENCIL_TEST);
}

void shutdown(void) {
    if (g_p.tex)  { glDeleteTextures(1, &g_p.tex);  g_p.tex  = 0; }
    if (g_p.vbo)  { glDeleteBuffers(1, &g_p.vbo);   g_p.vbo  = 0; }
    if (g_p.prog) { glDeleteProgram(g_p.prog);      g_p.prog = 0; }
    g_p.ready = false;
    std::lock_guard<std::mutex> g(g_lock);
    g_queue.clear();
}

}  // namespace toast
