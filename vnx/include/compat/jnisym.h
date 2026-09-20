#pragma once
#include <cstring>
#include <cstdio>

// ─── Matching JNI native-method symbol names ────────────────────────────────
// A cocos2d-x game exports its engine entry points as JNI long names:
//
//   Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender
//   └Java_┘└─── package ───┘└─── class ───┘└─ method ─┘
//
// The Core looked for that one string. It is right for Hill Climb Racing and
// for stock cocos2d-x, and wrong the moment a game ships the engine under its
// own package — which build tools do routinely, and which is why a title can
// be plainly cocos2d-x, load cleanly, and then have no render loop to call.
//
// The class and method names are the stable part: every cocos2d-x fork still
// calls the class Cocos2dxRenderer and the method nativeRender, whatever
// package it was compiled into. So the package is what gets ignored.
//
// Two JNI mangling rules matter here. An underscore inside a package or class
// name is escaped as "_1", so the separators this splits on are never
// ambiguous. And an overloaded native gets its argument signature appended
// after a double underscore ("..._nativeRender__II"), so a match may be
// followed by "__" as well as by end of string.
namespace jnisym {

// Does `symbol` name `method` on `class_name`, in any package?
inline bool matches(const char* symbol, const char* class_name, const char* method) {
    if (!symbol || !class_name || !method) return false;
    if (strncmp(symbol, "Java_", 5) != 0) return false;

    // What the tail must look like: _<Class>_<method>
    char needle[192];
    int n = snprintf(needle, sizeof(needle), "_%s_%s", class_name, method);
    if (n <= 0 || n >= (int)sizeof(needle)) return false;

    // Search from the end: a class name can legitimately appear inside a
    // package (com.cocos2dx.Cocos2dxRenderer.something), and the last
    // occurrence is the one that ends the symbol.
    const char* best = nullptr;
    for (const char* p = symbol + 5; (p = strstr(p, needle)) != nullptr; p++)
        best = p;
    if (!best) return false;

    const char* after = best + n;
    if (*after == '\0') return true;          // exact
    if (after[0] == '_' && after[1] == '_') return true;  // overload: __<signature>
    return false;
}

}  // namespace jnisym
