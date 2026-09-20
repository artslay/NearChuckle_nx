#include "compat/bootfade.h"
#include "compat/loader.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <switch.h>

namespace {

GLuint   g_prog = 0, g_vbo = 0;
GLint    g_uAlpha = -1;
uint64_t g_start = 0;
float    g_secs  = 0.0f;
bool     g_failed = false;

const char* kVert =
    "attribute vec2 aPos;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

const char* kFrag =
    "precision mediump float;\n"
    "uniform float uAlpha;\n"
    "void main() { gl_FragColor = vec4(0.0, 0.0, 0.0, uAlpha); }\n";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(s); return 0; }
    return s;
}

bool ensureProgram(void) {
    if (g_prog) return true;
    if (g_failed) return false;

    GLuint vs = compile(GL_VERTEX_SHADER, kVert);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) { g_failed = true; return false; }

    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs);
    glAttachShader(g_prog, fs);
    glBindAttribLocation(g_prog, 0, "aPos");
    glLinkProgram(g_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(g_prog); g_prog = 0; g_failed = true; return false; }

    g_uAlpha = glGetUniformLocation(g_prog, "uAlpha");

    static const GLfloat quad[] = { -1.f, -1.f,  1.f, -1.f, -1.f,  1.f,  1.f,  1.f };
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

}  // namespace

void bootFadeBegin(float seconds) {
    g_secs  = seconds;
    g_start = armGetSystemTick();
}

bool bootFadeActive(void) {
    if (g_secs <= 0.0f) return false;
    return armTicksToNs(armGetSystemTick() - g_start) < (uint64_t)(g_secs * 1e9);
}

void bootFadeDraw(void) {
    if (!bootFadeActive()) return;
    if (!ensureProgram()) { g_secs = 0.0f; return; }

    const float t = (float)(armTicksToNs(armGetSystemTick() - g_start) / 1e9) / g_secs;
    // Ease out, so the picture is clear well before the overlay is technically
    // gone — a linear fade reads as a grey haze lingering over the first
    // second of play.
    const float alpha = (1.0f - t) * (1.0f - t);

    // Everything this touches is saved and put back. The game is mid-frame and
    // has its own idea of the pipeline state; leaving any of it changed would
    // show up as a rendering bug much later, far from here.
    GLint  prevProg = 0, prevBuf = 0, prevArrEnabled = 0, prevSize = 0, prevType = 0;
    GLint  prevNorm = 0, prevStride = 0, prevVbo = 0;
    void*  prevPtr  = nullptr;
    GLint  blendSrcRGB = 0, blendDstRGB = 0, blendSrcA = 0, blendDstA = 0;
    GLint  viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstA);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &prevArrEnabled);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &prevSize);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &prevType);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &prevNorm);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &prevStride);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &prevVbo);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &prevPtr);
    const GLboolean wasBlend  = glIsEnabled(GL_BLEND);
    const GLboolean wasDepth  = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean wasCull   = glIsEnabled(GL_CULL_FACE);
    const GLboolean wasScis   = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean wasSten   = glIsEnabled(GL_STENCIL_TEST);

    glUseProgram(g_prog);
    glUniform1f(g_uAlpha, alpha);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // ── restore ──
    if (!prevArrEnabled) glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevVbo);
    if (prevArrEnabled)
        glVertexAttribPointer(0, prevSize, (GLenum)prevType, (GLboolean)prevNorm,
                              prevStride, prevPtr);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    glUseProgram((GLuint)prevProg);
    glBlendFuncSeparate((GLenum)blendSrcRGB, (GLenum)blendDstRGB,
                        (GLenum)blendSrcA,   (GLenum)blendDstA);
    if (!wasBlend) glDisable(GL_BLEND);
    if (wasDepth)  glEnable(GL_DEPTH_TEST);
    if (wasCull)   glEnable(GL_CULL_FACE);
    if (wasScis)   glEnable(GL_SCISSOR_TEST);
    if (wasSten)   glEnable(GL_STENCIL_TEST);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
}
