// ─── Hill Climb Racing — the shop, and why it used to crash ─────────────────
// On Android, HCR's shop is not filled in by the game. Java fills it: at
// startup NewBillingHandle.Init() calls the game's own native setInAppItem()
// once per product, 69 times, before Google Play is ever contacted. Only then
// does the native side have a product vector to index.
//
// Viridite runs no Dalvik bytecode, so that never happened, and the shop's
// product vector stayed empty. The native builder then read items[size-1] on
// an empty vector — a load from -8 — and later walked the same empty vector
// and called a virtual method on what it found there. Both were patched out
// at fixed addresses in game_hillclimb.cpp: the Shop opened empty instead of
// taking the app down, and every patch was pinned to one exact build of one
// exact version, because that is what an address is.
//
// The cause was never the instructions. It was the 69 calls that never came.
//
// This replays them. The catalog below is transcribed from xflipperkast's
// independent Switch port, HCR_NX (https://github.com/xflipperkast/HCR_NX),
// which read it out of the game's own classes2.dex and does exactly this —
// full credit to them for identifying the mechanism and recovering the data.
// The result is a shop that works rather than one that is merely survivable,
// and it is version-independent: product ids are content, not addresses, so
// the same replay serves 1.67.0 and 1.71.1 alike.
#include "compat/games.h"
#include "compat/loader.h"
#include <cstdio>
#include <cstring>

namespace {

// One product, in the shape NewBillingHandle.Init() passes to the game.
// `unknown` is genuinely that: a flag the Java side sets whose meaning is not
// established here, kept because dropping a field from a replay of someone
// else's initialisation is how a replay stops being one.
struct ShopItem {
    const char* id;
    const char* bonus;      // "+25%" badge text, or ""
    const char* icon;       // sprite name, e.g. "coin0"
    int         amount;     // coins/gems/paints granted
    unsigned char ad_free, unknown, special, gems, paints;
    int         bundle;     // bundle id, 0 when not a bundle
};

// Exactly the 69 entries NewBillingHandle.Init() sets, in its order. The count
// is asserted below: a short catalog is a shop missing items, and a long one
// means a transcription slipped.
constexpr ShopItem kCatalog[] = {
    {"com.fingersoft.hillclimb.adfree_t1",
     "", "coin0", 100000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.adfree_t2",
     "", "coin1", 300000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.adfree_t3",
     "", "coin2", 500000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.adfree_150000coins",
     "", "coin0", 150000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.adfree_300000coins",
     "+25%", "coin1", 300000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.adfree_750000coins",
     "+99%", "coin2", 750000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.adfree_2000000coins",
     "+166%", "coin3", 2000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.adfree_4000000coins",
     "+212%", "coin4", 4000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.adfree_8000000coins",
     "+308%", "coin5", 8000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.adfree_20000000coins",
     "+431%", "coin6", 20000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap2.adfree_300000coins",
     "", "coin0", 300000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap2.adfree_600000coins",
     "+25%", "coin1", 600000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap2.adfree_1500000coins",
     "+99%", "coin2", 1500000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap2.adfree_4000000coins",
     "+166%", "coin3", 4000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap2.adfree_8000000coins",
     "+212%", "coin4", 8000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap2.adfree_16000000coins",
     "+308%", "coin5", 16000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap2.adfree_40000000coins",
     "+431%", "coin6", 40000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap3.adfree_600000coins",
     "", "coin1", 600000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap3.adfree_1200000coins",
     "+25%", "coin1", 1200000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap3.adfree_3000000coins",
     "+99%", "coin2", 3000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap3.adfree_8000000coins",
     "+166%", "coin3", 8000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap3.adfree_16000000coins",
     "+212%", "coin4", 16000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap3.adfree_32000000coins",
     "+308%", "coin5", 32000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap3.adfree_80000000coins",
     "+431%", "coin6", 80000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap4.adfree_1200000coins",
     "", "coin1", 1200000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap4.adfree_2400000coins",
     "+25%", "coin1", 2400000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap4.adfree_6000000coins",
     "+99%", "coin2", 6000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap4.adfree_16000000coins",
     "+166%", "coin3", 16000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap4.adfree_32000000coins",
     "+212%", "coin4", 32000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap4.adfree_64000000coins",
     "+308%", "coin5", 64000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap4.adfree_160000000coins",
     "+431%", "coin6", 160000000, 0, 0, 0, 0, 0, 0},
    {"com.fingersoft.hillclimb.iap2.adfree_500gems",
     "", "diamond1", 500, 0, 0, 0, 1, 0, 0},
    {"com.fingersoft.hillclimb.iap2.adfree_2000gems",
     "+132%", "diamond2", 2000, 0, 0, 0, 1, 0, 0},
    {"com.fingersoft.hillclimb.iap2.adfree_5000gems",
     "+199%", "diamond3", 5000, 0, 0, 0, 1, 0, 0},
    {"com.fingersoft.hillclimb.iap2.adfree_11000gems",
     "+219%", "diamond4", 11000, 0, 0, 0, 1, 0, 0},
    {"com.fingersoft.hillclimb.iap2.adfree_23000gems",
     "+305%", "diamond5", 23000, 0, 0, 0, 1, 0, 0},
    {"com.fingersoft.hillclimb.iap2.adfree_40000gems",
     "+318%", "diamond6", 40000, 0, 0, 0, 1, 0, 0},
    {"com.fingersoft.hillclimb.iap1.adfree_1000gems",
     "", "diamond1", 1000, 0, 0, 0, 1, 0, 0},
    {"com.fingersoft.hillclimb.iap1.adfree_3000gems",
     "+99%", "diamond2", 3000, 0, 0, 0, 1, 0, 0},
    {"com.fingersoft.hillclimb.iap1.adfree_6000gems",
     "+119%", "diamond3", 6000, 0, 0, 0, 1, 0, 0},
    {"com.fingersoft.hillclimb.iap1.adfree_15000gems",
     "+149%", "diamond4", 15000, 0, 0, 0, 1, 0, 0},
    {"com.fingersoft.hillclimb.iap1.adfree_26000gems",
     "+172%", "diamond5", 26000, 0, 0, 0, 1, 0, 0},
    {"com.fingersoft.hillclimb.iap1.adfree_50000gems",
     "+199%", "diamond6", 50000, 0, 0, 0, 1, 0, 0},
    {"com.fingersoft.hillclimb.paint1",
     "", "paint1", 60, 0, 0, 0, 0, 1, 0},
    {"com.fingersoft.hillclimb.paint2",
     "", "paint2", 160, 0, 0, 0, 0, 1, 0},
    {"com.fingersoft.hillclimb.paint3",
     "", "paint3", 330, 0, 0, 0, 0, 1, 0},
    {"com.fingersoft.hillclimb.specialoffer1",
     "+8212%", "coin2", 100000000, 0, 0, 1, 0, 0, 0},
    {"com.fingersoft.hillclimb.specialoffer2",
     "+1562%", "coin2", 20000000, 0, 0, 1, 0, 0, 0},
    {"com.fingersoft.hillclimb.specialoffer3",
     "+316%", "coin2", 5000000, 0, 0, 1, 0, 0, 0},
    {"com.fingersoft.hillclimb.specialoffer1gems",
     "+232%", "diamond2", 30000, 0, 0, 1, 0, 0, 0},
    {"com.fingersoft.hillclimb.special_garage1",
     "", "bundle1", 0, 0, 0, 0, 1, 0, 1},
    {"com.fingersoft.hillclimb.special_garage2",
     "", "bundle1", 0, 0, 0, 0, 1, 0, 2},
    {"com.fingersoft.hillclimb.bundle10",
     "", "", 0, 0, 0, 0, 1, 0, 10},
    {"com.fingersoft.hillclimb.bundle11",
     "", "", 0, 0, 0, 0, 1, 0, 11},
    {"com.fingersoft.hillclimb.bundle12",
     "", "", 0, 0, 0, 0, 1, 0, 12},
    {"com.fingersoft.hillclimb.bundle13",
     "", "", 0, 0, 0, 0, 1, 0, 13},
    {"com.fingersoft.hillclimb.bundle14",
     "", "", 0, 0, 0, 0, 1, 0, 14},
    {"com.fingersoft.hillclimb.bundle15",
     "", "", 0, 0, 0, 0, 1, 0, 15},
    {"com.fingersoft.hillclimb.bundle16",
     "", "", 0, 0, 0, 0, 1, 0, 16},
    {"com.fingersoft.hillclimb.bundle17",
     "", "", 0, 0, 0, 0, 1, 0, 17},
    {"com.fingersoft.hillclimb.bundle18",
     "", "", 0, 0, 0, 0, 1, 0, 18},
    {"com.fingersoft.hillclimb.bundle19",
     "", "", 0, 0, 0, 0, 1, 0, 19},
    {"com.fingersoft.hillclimb.bundle20",
     "", "", 0, 0, 0, 0, 1, 0, 20},
    {"com.fingersoft.hillclimb.bundle21",
     "", "", 0, 0, 0, 0, 1, 0, 21},
    {"com.fingersoft.hillclimb.bundle22",
     "", "", 0, 0, 0, 0, 0, 0, 22},
    {"com.fingersoft.hillclimb.bundle23",
     "", "", 0, 0, 0, 0, 0, 0, 23},
    {"com.fingersoft.hillclimb.vehicles141021",
     "", "", 0, 0, 0, 0, 1, 0, 996},
    {"com.fingersoft.hillclimb.stages141021",
     "", "", 0, 0, 0, 0, 1, 0, 997},
    {"com.fingersoft.hillclimb.vehiclesstages141021",
     "", "", 0, 0, 0, 0, 1, 0, 998},
};
constexpr size_t kCatalogCount = sizeof(kCatalog) / sizeof(kCatalog[0]);
static_assert(kCatalogCount == 69, "HCR's Init() catalog is 69 products");

}  // namespace

// Group digits the way the game's own UI does: "1 000 000", space-separated.
// Exposed for the host test — this runs 69 times before the shop is built and
// a wrong separator is visible on every price in it.
void hcrFormatShopAmount(char out[24], int value) {
    char raw[16];
    snprintf(raw, sizeof(raw), "%d", value);
    const size_t len = strlen(raw);
    size_t dst = 0;
    for (size_t src = 0; src < len && dst + 1 < 24; src++) {
        if (src && (len - src) % 3 == 0) out[dst++] = ' ';
        out[dst++] = raw[src];
    }
    out[dst] = '\0';
}

size_t hcrShopCatalogCount(void) { return kCatalogCount; }
const char* hcrShopCatalogId(size_t i) {
    return i < kCatalogCount ? kCatalog[i].id : nullptr;
}

bool hcrPopulateShop(void* env, void* thiz, HcrSymResolver resolve) {
    if (!resolve) return false;

    // In this JNI a jstring is the C string itself (see GetStringUTFChars in
    // jni_env.cpp), which is why these are passed straight through — the same
    // way nativeSetPaths is called.
    typedef void (*SetItem_fn)(void*, void*, const char*, const char*, const char*,
                              const char*, const char*, unsigned char, unsigned char,
                              unsigned char, unsigned char, unsigned char, int);
    typedef void (*SetStr_fn)(void*, void*, const char*, const char*);

    auto setItem = (SetItem_fn)resolve("Java_com_fingersoft_game_MainActivity_setInAppItem");
    auto setPrice = (SetStr_fn)resolve("Java_com_fingersoft_game_MainActivity_setInAppItemPrice");
    auto setCurrency = (SetStr_fn)resolve("Java_com_fingersoft_game_MainActivity_setInAppItemCurrencyCode");
    if (!setItem) {
        compatLog("shop[HCR]: setInAppItem not exported by this build — leaving the "
                  "product list to the crash patches");
        return false;
    }

    // The formatter is what every price in the shop goes through; if it is
    // wrong, 69 wrong strings are already in the game before anything shows.
    char check[24];
    hcrFormatShopAmount(check, 1000000);
    if (strcmp(check, "1 000 000") != 0) {
        compatLogFmt("shop[HCR]: amount formatter produced \"%s\" — not replaying", check);
        return false;
    }

    for (size_t i = 0; i < kCatalogCount; i++) {
        const ShopItem& it = kCatalog[i];
        char amount[24];
        hcrFormatShopAmount(amount, it.amount);
        setItem(env, thiz, it.id, "FREE", amount, it.bonus, it.icon,
                it.ad_free, it.unknown, it.special, it.gems, it.paints, it.bundle);
        // Android fills these in later from Google Play's async product query,
        // which never answers here. Supplying the game's own documented offline
        // fallback immediately keeps every item priced rather than blank.
        if (setPrice)    setPrice(env, thiz, it.id, "FREE");
        if (setCurrency) setCurrency(env, thiz, it.id, "");
    }
    compatLogFmt("shop[HCR]: replayed %zu products from NewBillingHandle.Init() — "
                 "the product vector is populated, as it is on Android",
                 kCatalogCount);
    return true;
}
