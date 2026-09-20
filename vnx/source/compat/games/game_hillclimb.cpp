// ─── Hill Climb Racing (com.fingersoft.hillclimb) — Cocos2d-x ───────────────
// All HCR-specific fixups live here. game_registry.cpp decides (by package and
// library name) that a load is this game before calling in, so nothing here can
// fire for another title — e.g. the Unity path. Everything is additionally
// signature-gated against this exact libgame.so (v1.67.0,
// sha256 542c25e5…c07b150a), so even a differently-built Hill Climb Racing is
// left untouched rather than patched at addresses that mean something else.
#include "compat/games.h"
#include "compat/loader.h"
#include <cstring>

namespace {

constexpr uint32_t kNop = 0xd503201f;

bool inRange(uint64_t vaddr, uint64_t min_vaddr, size_t alloc_size) {
    return vaddr >= min_vaddr && vaddr + 4 <= min_vaddr + alloc_size;
}

// Is this NUL-terminated string anywhere in the staged image? Used to ask what
// a build exports without a symbol table to hand: an exported name lives in
// .dynstr, which is inside the image being staged. A plain byte search, so a
// hit is a hit whatever section it landed in — false positives cost nothing
// here, since the only consequence is preferring the correct fix over a patch.
bool imageHasString(const uint8_t* base, size_t alloc_size, const char* needle) {
    if (!base || !needle) return false;
    const size_t n = strlen(needle);
    if (n == 0 || alloc_size < n + 1) return false;
    const uint8_t* end = base + alloc_size - n;
    for (const uint8_t* p = base; p <= end; p++) {
        if (p[0] == (uint8_t)needle[0] && memcmp(p, needle, n) == 0) return true;
    }
    return false;
}

// Quirk 1 — Shop/IAP crash.
// The Google-Play shop builder populates its product vector from a store
// backend that doesn't exist here, leaving it empty, then unconditionally reads
// items[size-1] — a load from -8, a hard crash the moment the Shop opens. The
// populate call and its result are otherwise discarded, so NOPing it skips the
// doomed populate: the Shop opens empty (nothing is purchasable here anyway)
// instead of taking the app down. Call site 0x22bbc8:
//   mov x0,x19 / bl 0x232240 / ldr x0,[x19,#0x360]
void patchShopPopulate(uint8_t* base, uint64_t min_vaddr, size_t alloc_size) {
    constexpr uint64_t site = 0x22bbc8;
    if (!inRange(site, min_vaddr, alloc_size)) return;
    uint32_t* at = (uint32_t*)(base + site);
    if (at[-1] != 0xaa1303e0u ||   // mov x0, x19
        at[ 0] != 0x9400199eu ||   // bl 0x232240 (shop-populate)
        at[ 1] != 0xf941b260u) {   // ldr x0, [x19, #0x360]
        return;
    }
    at[0] = kNop;
    compatLog("quirk[HCR]: NOP'd Shop product-list populate at +0x22bbc8 "
              "(empty-vector crash; shop opens empty, no billing backend)");
}

// Quirk 2 — racing crash on the vehicle skin lookup.
// SkinProvider::find("jeep") (bl 0x387350) returns null when the skin map is
// empty, and the caller reads the returned std::string's size byte with no null
// check (ldrb w8,[x0] at 0x31354c) — a null deref the instant a race starts.
// After the length check the string is never touched again (the rest uses the
// vehicle object in x19), and the len!=6 path just sets "no skin matched", so
// turning the deref into `cbz x0, 0x31358c` makes a missing skin fall through
// gracefully (worst case: no custom skin) instead of crashing. Signature:
//   add x0,sp,#0x10 / bl 0x387350 / ldrb w8,[x0] / ldr x9,[x0,#8]
void patchSkinLookupGuard(uint8_t* base, uint64_t min_vaddr, size_t alloc_size) {
    constexpr uint64_t site = 0x31354c;
    if (!inRange(site, min_vaddr, alloc_size)) return;
    uint32_t* at = (uint32_t*)(base + site);
    if (at[-1] != 0x9401cf82u ||   // bl 0x387350 (SkinProvider::find)
        at[ 0] != 0x39400008u ||   // ldrb w8, [x0]   ← the crashing load
        at[ 1] != 0xf9400409u) {   // ldr  x9, [x0, #8]
        return;
    }
    at[0] = 0xb4000200u;           // cbz x0, #0x31358c  (skip to "no skin" path)
    compatLog("quirk[HCR]: guarded null SkinProvider::find at +0x31354c "
              "(cbz x0 → +0x31358c; racing no longer crashes on a missing skin)");
}

// Quirk 3 — second Shop crash, further into the same builder.
// Quirk 1 stops the product vector being populated from a store backend that
// isn't there, but this site still walks that (now empty) vector and calls a
// virtual method on whatever it finds:
//   ldr x10,[x19,#1664] / ldr x9,[x19,#1656]   ; end, begin
//   sub/asr #3/sub #1                          ; (count - 1)
//   cmp x10,x8 / b.cs                          ; UNSIGNED bounds check
//   ldr x0,[x8]   (0x22c020)                   ; element pointer
//   ldr x8,[x0]   (0x22c024)                   ; ← vtable load, x0 is null
//   ldr x8,[x8,#728] / blr x8                  ; virtual call
// With the vector empty, count-1 underflows to 0xFFFF…, so the unsigned
// compare passes and it reads past the end — hardware logs show exactly this,
// far=0x0 at +0x22c024 killing a session after a few minutes.
//
// The whole block is shop-item work that cannot succeed without a billing
// backend, so it's branched over rather than guarded.
//
// The first version of this patched 0x22c020 (the element load) and hardware
// showed it applied — yet the crash repeated at 0x22c024 with the branch
// plainly present one instruction earlier:
//   INSN: [pc-4]=14000005 [pc]=f9400008
// So something reaches the dereference without passing through 0x22c020;
// there is at least one more branch into this block than the disassembly
// around it revealed. Patching the FAULTING instruction instead covers every
// path into it, whether or not each one has been found — 0x22c024 becomes
// `b +0x10`, skipping the vtable read and the call regardless of how it was
// entered. 0x22c020 is left alone so the element load still behaves normally
// for any path where it matters.
void patchShopItemVirtualCall(uint8_t* base, uint64_t min_vaddr, size_t alloc_size) {
    constexpr uint64_t site = 0x22c020;      // signature anchor
    if (!inRange(site, min_vaddr, alloc_size)) return;
    uint32_t* at = (uint32_t*)(base + site);
    if (at[ 0] != 0xf9400100u ||   // ldr x0, [x8]
        at[ 1] != 0xf9400008u ||   // ldr x8, [x0]      ← the crashing load
        at[ 2] != 0x52800021u ||   // mov w1, #1
        at[ 3] != 0xf9416d08u ||   // ldr x8, [x8, #728]
        at[ 4] != 0xd63f0100u) {   // blr x8
        return;
    }
    at[1] = 0x14000004u;           // 0x22c024: b #+0x10 → 0x22c034, past the call
    compatLog("quirk[HCR]: skipped empty-shop virtual call at +0x22c024 "
              "(null vtable deref; branches over the call from every path)");
}

// Quirk 4 — the controller name printed beside the guide.
// The guide images are replaced with Switch artwork, but HCR draws its own
// caption next to them from a (name, file) table in .rodata:
//   0x6095ef "MOGA PRO"     0x6095f8 "Moga_Pro_Guide.png"
//   0x60960b "MOGA POCKET"  0x609617 "Moga_Pocket_Guide.png"
// so the screen showed a Switch Pro Controller labelled "MOGA PRO", which
// reads as the patch not having worked at all.
//
// Both are NUL-terminated in place, so a shorter replacement fits without
// moving anything: write the new text and re-terminate inside the original
// space. Nothing may be written past the original length — the next string
// starts immediately after.
void patchControllerNames(uint8_t* base, uint64_t min_vaddr, size_t alloc_size) {
    struct NamePatch { uint64_t vaddr; const char* from; const char* to; };
    static const NamePatch kNames[] = {
        {0x6095ef, "MOGA PRO",    "PRO CTRL"},   // 8 -> 8
        {0x60960b, "MOGA POCKET", "JOY-CON"},    // 11 -> 7, rest zeroed
    };
    for (const NamePatch& n : kNames) {
        size_t oldLen = strlen(n.from), newLen = strlen(n.to);
        if (newLen > oldLen) continue;                       // never overrun
        if (!inRange(n.vaddr, min_vaddr, alloc_size) ||
            !inRange(n.vaddr + oldLen, min_vaddr, alloc_size)) continue;
        char* at = (char*)(base + n.vaddr);
        if (memcmp(at, n.from, oldLen) != 0 || at[oldLen] != '\0') continue;
        memcpy(at, n.to, newLen);
        memset(at + newLen, 0, oldLen - newLen);             // re-terminate + pad
        compatLogFmt("quirk[HCR]: controller name \"%s\" -> \"%s\"", n.from, n.to);
    }
}

}  // namespace

// ─── Entry point ────────────────────────────────────────────────────────────
// Called only from the dispatcher in game_registry.cpp, which has already
// established that this is Hill Climb Racing's libgame.so.

void hcrApplyQuirks(const char* soname, uint8_t* stage_base,
                    uint64_t min_vaddr, size_t alloc_size) {
    (void)soname;

    // Quirks 1 and 3 exist because the shop's product vector was empty: the 69
    // setInAppItem() calls Android's Java side makes before the engine starts
    // never happened here, so the builder indexed an empty vector and the only
    // answer available was to patch the instructions that did the indexing.
    //
    // game_hillclimb_shop.cpp makes those calls now. With products in the
    // vector both crashes are gone at the cause, and the patches become
    // actively wrong: quirk 3 branches over a virtual call on a shop item,
    // which is exactly the call that draws one. So they apply only to a build
    // that cannot be given a catalogue.
    //
    // Whether it can is decided here, at load time, by looking for the name of
    // the native the replay calls. It is in .dynstr if the build exports it,
    // and searching the staged image for it needs nothing plumbed through from
    // the loader.
    const bool canPopulateShop = imageHasString(
        stage_base, alloc_size, "Java_com_fingersoft_game_MainActivity_setInAppItem");

    if (canPopulateShop) {
        compatLog("quirk[HCR]: shop crash patches skipped — this build exports "
                  "setInAppItem, so the product list gets filled properly instead");
    } else {
        patchShopPopulate(stage_base, min_vaddr, alloc_size);
        patchShopItemVirtualCall(stage_base, min_vaddr, alloc_size);
    }

    patchSkinLookupGuard(stage_base, min_vaddr, alloc_size);
    patchControllerNames(stage_base, min_vaddr, alloc_size);
}
