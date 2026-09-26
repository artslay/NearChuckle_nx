#include "compat/loader.h"
#include <switch.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <vector>
#include <unordered_map>
#include <setjmp.h>
#include <signal.h>
#include <switch/arm/thread_context.h>
#include <switch/runtime/env.h>

// Log helpers — declared before the exception handler so it can use them.
extern void compatLog(const char* msg);
extern void compatLogFmt(const char* fmt, ...);
extern "C" void compatSetActiveFarCryProfile(const char* profile);
extern "C" bool compatSaveFarCryConfiguration();
extern void compatLogFlush();
extern void compatUiLog(const char* msg);
extern void compatUiSetPct(int pct);
extern "C" unsigned compatGuestGetFileSize(void* self, const char* path, unsigned flags);
extern "C" void compatGuestSetCallbackTimeQuota(void* self, int nMicroseconds);
extern "C" bool compatGuestActivateReadStream(void* self);
extern "C" uint32_t compatGuestCallReadFileEx(void* self);
extern "C" void* g_near_original_refstream_activate;
extern "C" void* g_near_original_refstream_call_read;
extern "C" void* g_near_refstream_on_io_complete;

// ─── Shared crash recovery ────────────────────────────────────────────────────
// libnx's default exception stack is 0x400 — one kilobyte. Our handler runs on
// it every single time a constructor faults, which for this game is 155 times
// in a few seconds, and whatever it overruns lands in whichever .bss happens to
// sit below it. That is a plausible source of damage to state we never touch
// directly, and the symptom fits: the main thread stops at the moment of the
// first fault, in a different draw call each run, and never recovers even after
// the faults stop.
//
// Both symbols are weak in libnx precisely so applications can size this
// themselves. 32KB costs nothing and removes the question.
extern "C" {
    alignas(16) u8 __nx_exception_stack[0x8000];
    u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
}

jmp_buf           g_recover_jmp;
volatile bool     g_in_recover  = false;
volatile void*    g_recover_owner = nullptr;  // thread that armed the jmp_buf
volatile int      g_recover_sig = 0;
volatile uint32_t g_recover_esr = 0;
volatile uint64_t g_recover_pc  = 0;
volatile uint64_t g_recover_far = 0;  // Fault Address Register
// Captured alongside the fault so a crash log can answer "how did we GET here",
// not just "where did it stop". Without the link register a jump into the
// middle of a block is invisible: a shop crash was patched at the entry point
// that the disassembly showed, applied correctly, and still fired — because
// something branched straight past it, and nothing in the log could say what.
volatile uint64_t g_recover_lr  = 0;   // x30, the return address
volatile uint64_t g_recover_x0  = 0;   // first operand, usually the null one
volatile uint64_t g_recover_x8  = 0;
// x6 holds the chunk _free_r is unlinking at the faulting instruction. Whether
// it points into the heap is the whole question: a real chunk means the walk
// is missing something, and a wild pointer means free() was handed rubbish.
volatile uint64_t g_recover_x6  = 0;
// _free_r stashes its mem argument at [sp+40] on entry (str x1,[sp,#40] at
// +0x14) and never reuses that slot. Captured in the handler because the
// logging below runs on frames that sit exactly where _free_r's was, so by
// then it is gone.
volatile uint64_t g_recover_freearg = 0;
// Counted in the handler so the watchdog can print it next to the main
// thread's phase — the two only mean something together. Kept a plain volatile
// int rather than an atomic: this runs on libnx's shared exception stack, and
// the handler's recovery path must stay as close to nothing as possible.
volatile int      g_ctor_faults = 0;
// What the loader is doing right now, published for the integrity monitor.
// Checking between constructors can only ever say "one of these 421 did it";
// a monitor sampling continuously can say which, and this is what it reads.
volatile int      g_cur_ctor = -1;
volatile const char* g_cur_module = "";
int  elfCurrentCtor(void)         { return g_cur_ctor; }
const char* elfCurrentModule(void){ return (const char*)g_cur_module; }
volatile uint64_t g_recover_fp  = 0;   // x29 — the frame chain the walk starts from
volatile uint64_t g_recover_sp  = 0;

// ─── Heap canaries ───────────────────────────────────────────────────────────
// Brain It On dies with 155 faults inside newlib's free(), unlinking a chunk
// through a NULL forward pointer — a corrupted free list (see
// docs/BRAIN_IT_ON_FINDINGS.md). The corrupting write is NOT in the obvious
// places: the segment copy, the relocation loop and the symtab/strtab copies
// are each bounds-checked, and all three were verified before adding this.
//
// So rather than keep guessing at candidates, allocate small blocks around the
// loader's big ones and verify their contents at each stage. Whichever
// checkpoint first reports damage localises the write to one phase, which is
// the thing the log currently cannot say.
namespace {
struct HeapCanary {
    static const size_t kSize  = 64;
    static const uint8_t kByte = 0xC9;
    uint8_t* p = nullptr;
    void arm() {
        if (!p) p = (uint8_t*)malloc(kSize);
        if (p) memset(p, kByte, kSize);
    }
    // Returns the offset of the first damaged byte, or -1 if intact.
    int check() const {
        if (!p) return -1;
        for (size_t i = 0; i < kSize; i++) if (p[i] != kByte) return (int)i;
        return -1;
    }
};
HeapCanary g_canary_lo, g_canary_hi;
}  // namespace

// Arms canaries either side of the next big allocation.
void elfHeapCanaryArm() { g_canary_lo.arm(); g_canary_hi.arm(); }

// Reports (once per stage) whether anything has trampled them.
void elfHeapCanaryCheck(const char* stage) {
    int lo = g_canary_lo.check(), hi = g_canary_hi.check();
    if (lo < 0 && hi < 0) return;
    compatLogFmt("HEAP: canary damaged after %s (lo=%d hi=%d) — "
                 "something wrote past a heap allocation during this phase",
                 stage ? stage : "?", lo, hi);
    compatLogFlush();
    g_canary_lo.arm();      // re-arm so the NEXT damaged phase is also reported
    g_canary_hi.arm();
}

static void logUnrecoveredFault(ThreadExceptionDump* ctx);
extern void shimLastAllocatorEvent(uint32_t* kind, uint32_t* phase, uint64_t* caller,
                                   uint64_t* ptr, uint64_t* size, uint64_t* size2);
void elfDescribePc(uint64_t pc, char* buf, size_t sz);   // defined below

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    uint32_t esr = ctx->esr;

    // Only longjmp on the thread that armed the jmp_buf — game worker threads
    // are real now, and unwinding another thread's setjmp would corrupt both.
    if (g_in_recover && (void*)threadGetSelf() == g_recover_owner) {
        g_recover_sig = (int)ctx->error_desc;
        g_recover_esr = esr;
        g_recover_pc  = ctx->pc.x;
        g_recover_far = ctx->far.x;
        g_recover_lr  = ctx->lr.x;
        g_recover_x0  = ctx->cpu_gprs[0].x;
        g_recover_x8  = ctx->cpu_gprs[8].x;
        g_recover_x6  = ctx->cpu_gprs[6].x;
        g_recover_fp  = ctx->fp.x;
        g_recover_sp  = ctx->sp.x;
        g_recover_freearg = 0;
        if (ctx->sp.x && (ctx->sp.x & 7) == 0)
            g_recover_freearg = *(volatile uint64_t*)(ctx->sp.x + 40);
        g_ctor_faults++;
        longjmp(g_recover_jmp, 1);
    }
    logUnrecoveredFault(ctx);
    // This fires for a fault on any thread OTHER than the one the main game
    // loop armed recovery on (e.g. the game's own background asset-loader
    // thread) — svcReturnFromException(0xf801) tells Horizon "unhandled,
    // kill the process", which it does, but asynchronously: the main thread
    // isn't told to stop, so it can keep polling/rendering for a bit while
    // the OS is mid-way through tearing the process down. That race window
    // — rendering against a display surface that's actively being torn down
    // — fits a reported flicker that happened outside the main-loop crash
    // path this build's exit(0) fix was verified against. Call exit()
    // directly instead: it terminates the whole process immediately and
    // synchronously from here, so there's no window for the main thread to
    // render even one more frame after a fault anywhere in the process.
    extern ThreadExceptionDump __nx_exceptiondump;
    __nx_exceptiondump = *ctx;
    exit(0);
}

// Name the caller by scanning the stack, not by walking frame pointers.
//
// The frame-chain attempt returned an address in .data: newlib is built
// without frame pointers, so x29 inside _free_r is not a frame pointer at all
// and the chain is meaningless. A stack scan needs no such cooperation — any
// return address pushed by a bl is still sitting there as a word that happens
// to point into executable code, and that is a property we can test directly.
//
// Every read is bounds-checked against the stack's own mapping first. A fault
// in here is unrecoverable, since the ctor's jmp_buf has already been consumed
// by the time it runs, and would take the process down with it.
static bool addrIsCode(uint64_t a, char* out, size_t outsz) {
    if (!a || (a & 3)) return false;                  // instructions are 4-aligned
    MemoryInfo mi = {}; u32 pi = 0;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return false;
    if (!(mi.perm & Perm_X)) return false;
    elfDescribePc(a, out, outsz);
    return true;
}

static void logFaultBacktrace(void) {
    uint64_t sp = g_recover_sp;
    if (!sp || (sp & 7)) { compatLog("ELF:   backtrace: no usable stack pointer"); return; }

    MemoryInfo mi = {}; u32 pi = 0;
    if (R_FAILED(svcQueryMemory(&mi, &pi, sp))) {
        compatLog("ELF:   backtrace: stack not mapped");
        return;
    }
    const uint64_t stack_end = mi.addr + mi.size;

    compatLog("ELF:   backtrace (code addresses on the stack, innermost first):");
    char line[256], where[192];
    int found = 0;
    // 128 words is deep enough to clear _free_r's frame and reach whoever
    // called it, without wading into unrelated history further up.
    for (uint64_t a = sp; a + 8 <= stack_end && a < sp + 384 * 8; a += 8) {
        uint64_t w = *(const uint64_t*)a;
        if (!addrIsCode(w, where, sizeof(where))) continue;
        snprintf(line, sizeof(line), "ELF:     %p  %s", (void*)w, where);
        compatLog(line);
        if (++found >= 14) break;
    }
    if (!found) compatLog("ELF:     (nothing on the stack resolved to code)");
}

static void ctor_crash_handler(int sig) {
    if (g_in_recover) { g_recover_sig = sig; longjmp(g_recover_jmp, 1); }
}

// External shim table from shim_table.cpp
void* shimResolve(const char* name);

// Accumulated unresolved symbol count across all elfLoad calls since elfResetCounts()
static int g_unresolved_count = 0;

// Relocation symbol lookup is extremely hot during startup. Keep successful
// name -> address bindings so repeated imports do not rescan every loaded SO's
// entire dynsym table. Unresolved names are deliberately not cached: a later
// dlopen may provide them, and the existing resolver's lookup semantics must
// remain unchanged.
static std::unordered_map<std::string, void*> g_symbol_cache;
static std::unordered_map<std::string, void*> g_exported_symbols;
int elfGetUnresolvedCount() { return g_unresolved_count; }

// Poison value written into any relocation slot whose symbol we couldn't
// resolve, instead of leaving it null. Same idea as max_nx's
// taint_missing_imports (a similar Android-.so-on-Switch loader project):
// a real bug that reaches an unresolved import would otherwise crash with a
// far/pc of plain 0 or a small addend, indistinguishable from an ordinary
// null-pointer bug elsewhere. This value's high bits put it far outside any
// real Switch VA range (still faults if ever actually branched to/dereferenced
// — same crash-and-recover behavior as before — just unmistakable in the log).
static constexpr uint64_t kUnresolvedSymbolPoison = 0xBAD0BAD0BAD00000ULL;

// First JIT failure code seen since elfResetCounts() (0 = all OK so far)
static uint32_t g_last_svc_perm_code = 0;
uint32_t elfGetLastSvcPermCode() { return g_last_svc_perm_code; }

void elfResetCounts() {
    g_unresolved_count = 0;
    g_last_svc_perm_code = 0;
    g_symbol_cache.clear();
    g_symbol_cache.reserve(4096);
    g_exported_symbols.clear();
    g_exported_symbols.reserve(32768);
}

// ─── elfNearestSym ───────────────────────────────────────────────────────────
const char* elfNearestSym(const LoadedSo* so, uint64_t vaddr, char* buf, size_t sz) {
    if (!so || !so->symtab_heap || !so->strtab_heap || so->sym_count == 0) {
        snprintf(buf, sz, "0x%llx", (unsigned long long)vaddr);
        return buf;
    }
    // No real function is bigger than this. Used as the cutoff for symbols that
    // don't carry an st_size, so "nearest preceding symbol" can't run away.
    static const uint64_t MAX_FUNC_SPAN = 1u << 20;   // 1MB

    uint64_t best_val  = 0;
    uint64_t best_size = 0;
    const char* best   = nullptr;
    for (uint32_t i = 0; i < so->sym_count; i++) {
        const Elf64_Sym& s = so->symtab_heap[i];
        if (s.st_value == 0 || !s.st_name || s.st_name >= so->strsz) continue;
        if (s.st_value > vaddr) continue;
        // When the symbol declares a size, an address past its end simply
        // isn't in it — don't let it claim one.
        if (s.st_size && vaddr >= s.st_value + s.st_size) continue;
        if (s.st_value >= best_val) {
            best_val  = s.st_value;
            best_size = s.st_size;
            best      = so->strtab_heap + s.st_name;
        }
    }

    // A hardware log once attributed 153 faults to "UnitySendMessage+0x55d608ea4"
    // — a 23GB offset, for a PC that wasn't even inside the module. Naming a
    // function that far away is worse than admitting we don't know, because it
    // sends whoever reads the log after the wrong code. Fall back to the raw
    // address whenever the offset stops being believable.
    uint64_t off = best ? (vaddr - best_val) : 0;
    bool plausible = best && best[0] != '\0' &&
                     (best_size ? off < best_size : off < MAX_FUNC_SPAN);

    if (!plausible)
        snprintf(buf, sz, "0x%llx", (unsigned long long)vaddr);
    else
        snprintf(buf, sz, "%.80s+0x%llx", best, (unsigned long long)off);
    return buf;
}

// ─── elfRunCtors ──────────────────────────────────────────────────────────────
// Run DT_INIT_ARRAY constructors stored by elfLoad.  Logs each entry before
// calling it (and flushes via compatLog) so the crash site is visible in the
// log when the Switch dies inside a constructor.
// Dry run: load everything, execute nothing.
//
// Constructors are where the game's own code first runs, and where the fault
// we are stuck on lives. Everything before them — extracting, mapping,
// relocating, resolving every imported symbol — is the part that a change to
// the loader can silently break, and it can be exercised in a couple of
// seconds without the game ever getting control.
static bool g_dry_run = false;
void elfSetDryRun(bool on) { g_dry_run = on; }
bool elfIsDryRun(void)      { return g_dry_run; }

void elfRunCtors(LoadedSo* so, ProgressCb cb) {
    if (g_dry_run) {
        compatLogFmt("ELF: %s: dry run — %zu constructors NOT executed",
                     so->path.c_str(), so->init_arr_count);
        return;
    }
    if (!so || !so->init_arr || so->init_arr_count == 0) return;
    size_t sl = so->path.rfind('/');
    const char* soname = (sl != std::string::npos)
                         ? so->path.c_str() + sl + 1 : so->path.c_str();

    // libapplovin-native-crash-reporter registers real SIGSEGV/SIGBUS handlers
    // and reads /proc/self/maps — both crash on Switch.  It's non-essential
    // (crash reporting only), so skip its constructors entirely.
    if (strstr(soname, "applovin") != nullptr) {
        compatLogFmt("ELF: %s: SKIP constructors (crash-reporter, not needed)", soname);
        compatUiLog("applovin: skip ctors (crash-reporter)");
        return;
    }

    signal(SIGSEGV, ctor_crash_handler);
    signal(SIGBUS,  ctor_crash_handler);
    signal(SIGILL,  ctor_crash_handler);

    int  failed = 0, skipped = 0, ok = 0;

    // DT_INIT runs before DT_INIT_ARRAY (same as Android linker order)
    if (so->init_fn) {
        compatLogFmt("ELF: %s: DT_INIT @%p", soname, (void*)so->init_fn);
        g_recover_owner = threadGetSelf(); g_in_recover = true; g_recover_sig = 0; g_recover_esr = 0;
        if (setjmp(g_recover_jmp) == 0) {
            so->init_fn();
            g_in_recover = false;
            compatLog("ELF: DT_INIT OK");
        } else {
            g_in_recover = false;
            compatLogFmt("ELF: DT_INIT FAULT sig=%d — skipped", g_recover_sig);
        }
    }

    compatLogFmt("ELF: %s: running %zu constructors", soname, so->init_arr_count);
    {
        char ub[80];
        snprintf(ub, sizeof(ub), "%s: running %zu ctors", soname, so->init_arr_count);
        compatUiLog(ub);
    }
    compatUiSetPct(60);

    const size_t n = so->init_arr_count;
    // Emit a UI update every ~50 ctors and at the end
    const size_t ui_interval = (n > 50) ? (n / 8) : n;

    // Anchor before the constructor pass so the final heap walk still has
    // the same baseline as the diagnostic build.
    shimHeapAnchor();

    for (size_t k = 0; k < n; k++) {
        LoadedSo::InitFn fn = so->init_arr[k];
        if (!fn || fn == (LoadedSo::InitFn)(uintptr_t)-1) { skipped++; continue; }

        g_cur_ctor   = (int)(k + 1);
        g_cur_module = soname;
        g_recover_owner = threadGetSelf(); g_in_recover = true; g_recover_sig = 0; g_recover_esr = 0; g_recover_far = 0;
        if (setjmp(g_recover_jmp) == 0) {
            fn();
            g_in_recover = false;
            ok++;
        } else {
            g_in_recover = false;

            // A constructor fault is already recovered and the constructor is
            // skipped. Detailed symbol/stack/heap forensics are extremely
            // expensive when a broken Android library produces dozens or
            // hundreds of identical faults during startup. Preserve the first
            // three complete reports; after that only count the failure.
            if (g_ctor_faults <= 3) {
                char sym_buf[160];
                uint64_t fault_vaddr = g_recover_pc - (uint64_t)so->base;
                elfNearestSym(so, fault_vaddr, sym_buf, sizeof(sym_buf));

                char lr_buf[192];
                elfDescribePc(g_recover_lr, lr_buf, sizeof(lr_buf));

                char blk[220] = "";
                uint64_t mem = g_recover_freearg;
                if (mem && (mem & 15) == 0 && strcmp(shimAddrRegion(mem), "heap") == 0) {
                    const uint64_t* h = (const uint64_t*)(mem - 16);
                    uint64_t psz = h[0], szf = h[1];
                    snprintf(blk, sizeof(blk),
                             " | freeing %p: prev_size=0x%llx size=0x%llx PREV_INUSE=%d",
                             (void*)mem, (unsigned long long)psz,
                             (unsigned long long)(szf & ~7ULL), (int)(szf & 1));
                } else if (mem) {
                    snprintf(blk, sizeof(blk), " | freeing %p (%s)",
                             (void*)mem, shimAddrRegion(mem));
                }

                compatLogFmt("ELF: ctor[%zu/%zu] FAULT sig=%d esr=0x%08x pc=%p far=%p sym=%s "
                             "lr=%p (%s) x0=%p x6=%p(%s) x8=%p — skipped",
                             k + 1, n, g_recover_sig, g_recover_esr,
                             (void*)g_recover_pc, (void*)g_recover_far, sym_buf,
                             (void*)g_recover_lr, lr_buf,
                             (void*)g_recover_x0,
                             (void*)g_recover_x6, shimAddrRegion(g_recover_x6),
                             (void*)g_recover_x8);
                if (blk[0]) compatLog(blk);

                static bool logged_extent = false;
                if (!logged_extent) {
                    logged_extent = true;
                    uint64_t lo = 0, hi = 0, brk = 0;
                    shimHeapExtent(&lo, &hi, &brk);
                    compatLogFmt("ELF:  | heap region %p..%p (%llu MB), break %p, "
                                 "%lld MB left; freed ptr %s the end",
                                 (void*)lo, (void*)hi,
                                 (unsigned long long)((hi - lo) / 1048576),
                                 (void*)brk,
                                 (long long)((int64_t)(hi - brk) / 1048576),
                                 (mem == hi) ? "IS" : "is not");
                    compatLogFlush();
                }

                if (g_ctor_faults <= 3)
                    logFaultBacktrace();

                const uint32_t* insn = (const uint32_t*)(uintptr_t)g_recover_pc;
                compatLogFmt("ELF: INSN: [pc-12]=%08x [pc-8]=%08x [pc-4]=%08x [pc]=%08x [pc+4]=%08x",
                             insn[-3], insn[-2], insn[-1], insn[0], insn[1]);
            }
            failed++;
        }

        if ((k + 1) % ui_interval == 0 || k + 1 == n) {
            char ub[80];
            snprintf(ub, sizeof(ub), "%s ctor[%zu/%zu] ok=%d fault=%d",
                     soname, k + 1, n, ok, failed);
            compatUiLog(ub);
            int pct = 60 + (int)(20 * (k + 1) / n);
            compatUiSetPct(pct);
            if (cb) cb("Running constructors", ub);
        }
    }
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS,  SIG_DFL);
    signal(SIGILL,  SIG_DFL);
    elfHeapCanaryCheck("constructors");
    {
        int steps = 0; const char* stop = "?";
        shimHeapWalkStats(&steps, &stop);
        compatLogFmt("ELF: %s: final heap walk covered %d chunks, stopped: %s",
                     soname, steps, stop);
    }
    compatLogFmt("ELF: %s: ctors done ok=%d failed=%d skipped=%d",
                 soname, ok, failed, skipped);
    {
        char ub[80];
        snprintf(ub, sizeof(ub), "%s: ctors done ok=%d failed=%d", soname, ok, failed);
        compatUiLog(ub);
    }
}

// All successfully loaded .so files (for cross-library symbol resolution)
static std::vector<LoadedSo*> g_loaded_sos;


// Describe an arbitrary code address as "<so> +0x<off> sym=<name>" (or mark it
// as host code). Used for abort()/exit() callers and unrecovered faults.
void elfDescribePc(uint64_t pc, char* buf, size_t sz) {
    for (LoadedSo* so : g_loaded_sos) {
        if (pc >= (uint64_t)so->alloc && pc < (uint64_t)so->alloc + so->alloc_size) {
            char sym_buf[160];
            elfNearestSym(so, pc - (uint64_t)so->base, sym_buf, sizeof(sym_buf));
            snprintf(buf, sz, "%s +0x%lx sym=%s",
                     so->path.c_str(), pc - (uint64_t)so->base, sym_buf);
            return;
        }
    }
    // Host code: report the offset within our own module. An NRO's runtime base
    // is its text base, so this is directly comparable to `nm` output on the
    // build's .elf — no subtraction by hand, and it makes clear at a glance
    // whether an address is even in the text range.
    extern void compatLog(const char*);
    static uint64_t host_base = 0;
    if (!host_base) {
        MemoryInfo mi = {}; u32 pi = 0;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (uint64_t)(uintptr_t)&compatLog)))
            host_base = mi.addr;
    }
    if (host_base && pc >= host_base)
        snprintf(buf, sz, "host+0x%llx", (unsigned long long)(pc - host_base));
    else
        snprintf(buf, sz, "%p (unknown)", (void*)pc);
}

// Log what a faulting address actually is: containing kernel memory region,
// its type and permissions. Distinguishes heap / JIT / host image / stack /
// unmapped at a glance.
void elfLogAddrInfo(const char* tag, uint64_t addr) {
    MemoryInfo mi = {};
    u32 pageinfo = 0;
    char buf[256];
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pageinfo, addr))) {
        snprintf(buf, sizeof(buf), "%s %p: region=%p size=0x%lx type=0x%x perm=%c%c%c", tag,
                 (void*)addr, (void*)mi.addr, (unsigned long)mi.size,
                 (unsigned)mi.type,
                 (mi.perm & Perm_R) ? 'r' : '-',
                 (mi.perm & Perm_W) ? 'w' : '-',
                 (mi.perm & Perm_X) ? 'x' : '-');
    } else {
        snprintf(buf, sizeof(buf), "%s %p: svcQueryMemory failed", tag, (void*)addr);
    }
    // Raw (lock-free) — this runs from crash-forensics call sites where the
    // crashing thread may already hold the normal logger's mutex.
    compatLogRaw(buf);
}

// Last chance to get the crash PC on disk before svcReturnFromException kills
// the process. Runs on the exception stack; must not fault again (guard flag).
// Uses compatLogRaw (lock-free) throughout: if the crashing thread died while
// holding g_log_lock inside an ordinary compatLogFmt call, the normal path
// would deadlock forever here, and the process would just hang with nothing
// on disk instead of recording the fault.
static void logUnrecoveredFault(ThreadExceptionDump* ctx) {
    static bool logged = false;
    if (logged) return;
    logged = true;

    char buf[320];
    snprintf(buf, sizeof(buf),
             "UNRECOVERED FAULT desc=0x%x esr=0x%08x pc=%p far=%p lr=%p sp=%p fp=%p x0=%p x1=%p x2=%p x3=%p",
             (unsigned)ctx->error_desc, ctx->esr,
             (void*)ctx->pc.x, (void*)ctx->far.x,
             (void*)ctx->lr.x, (void*)ctx->sp.x, (void*)ctx->fp.x,
             (void*)ctx->cpu_gprs[0].x, (void*)ctx->cpu_gprs[1].x,
             (void*)ctx->cpu_gprs[2].x, (void*)ctx->cpu_gprs[3].x);
    compatLogRaw(buf);

    snprintf(buf, sizeof(buf),
             "UNRECOVERED FAULT x4=%p x5=%p x6=%p x7=%p x8=%p x9=%p x10=%p x11=%p x12=%p x13=%p x14=%p x15=%p",
             (void*)ctx->cpu_gprs[4].x, (void*)ctx->cpu_gprs[5].x,
             (void*)ctx->cpu_gprs[6].x, (void*)ctx->cpu_gprs[7].x,
             (void*)ctx->cpu_gprs[8].x, (void*)ctx->cpu_gprs[9].x,
             (void*)ctx->cpu_gprs[10].x, (void*)ctx->cpu_gprs[11].x,
             (void*)ctx->cpu_gprs[12].x, (void*)ctx->cpu_gprs[13].x,
             (void*)ctx->cpu_gprs[14].x, (void*)ctx->cpu_gprs[15].x);
    compatLogRaw(buf);

    char where[256];
    elfDescribePc(ctx->pc.x, where, sizeof(where));
    snprintf(buf, sizeof(buf), "UNRECOVERED FAULT at %s", where);
    compatLogRaw(buf);

    elfDescribePc(ctx->lr.x, where, sizeof(where));
    snprintf(buf, sizeof(buf), "UNRECOVERED FAULT lr in %s", where);
    compatLogRaw(buf);

    // Always scan the faulting thread's stack. LR can legitimately be zero
    // when the crashing frame was entered through a thread/tail-call path, so
    // gating all caller diagnostics on LR loses the most useful evidence.
    {
        const uint64_t sp = ctx->sp.x;
        MemoryInfo stackMi = {};
        u32 stackPi = 0;
        if (sp && (sp & 7) == 0 &&
            R_SUCCEEDED(svcQueryMemory(&stackMi, &stackPi, sp))) {
            const uint64_t stackEnd = stackMi.addr + stackMi.size;
            int found = 0;
            for (uint64_t a = sp; a + 8 <= stackEnd && a < sp + 0x800; a += 8) {
                const uint64_t candidate =
                    *(const volatile uint64_t*)(uintptr_t)a;
                char candidateWhere[192];
                if (!addrIsCode(candidate, candidateWhere, sizeof(candidateWhere)))
                    continue;

                snprintf(buf, sizeof(buf),
                         "UNRECOVERED FAULT STACK +0x%llx %p %s",
                         (unsigned long long)(a - sp),
                         (void*)candidate, candidateWhere);
                compatLogRaw(buf);
                if (++found >= 20)
                    break;
            }
            if (!found)
                compatLogRaw("UNRECOVERED FAULT STACK: no executable return addresses found");
        } else {
            compatLogRaw("UNRECOVERED FAULT STACK: SP not mapped/aligned");
        }
    }

    // Decode the exact instruction when it is the NULL halfword store seen in
    // the current crash. Keeping this here turns the hex word into a directly
    // actionable register relationship in the next log.
    {
        const uint32_t faultInsn =
            *(const volatile uint32_t*)(uintptr_t)ctx->pc.x;
        if (faultInsn == 0x78245941u) {
            snprintf(buf, sizeof(buf),
                     "UNRECOVERED FAULT DECODE: STRH W1,[X10,W4,UXTW#1] x10=%p w4=0x%x",
                     (void*)ctx->cpu_gprs[10].x,
                     (unsigned)(uint32_t)ctx->cpu_gprs[4].x);
            compatLogRaw(buf);
        }
    }

    // Extra shader-crash forensics: the current FAR is not a valid Switch
    // pointer. Dump the actual faulting instruction and the nearby heap bytes
    // referenced by the live registers so we can distinguish parser corruption
    // from an ABI/layout problem inside libXRenderOGL.
    {
        MemoryInfo pcMi = {};
        u32 pcPi = 0;
        if (!R_FAILED(svcQueryMemory(&pcMi, &pcPi, ctx->pc.x)) &&
            (pcMi.perm & Perm_X)) {
            const uint64_t region_end = pcMi.addr + pcMi.size;
            uint64_t start = ctx->pc.x & ~0xFULL;
            if (start < pcMi.addr) start = pcMi.addr;
            if (start + 0x30 <= region_end) {
                for (uint64_t a = start; a < start + 0x30; a += 4) {
                    const uint32_t insn =
                        *(const volatile uint32_t*)(uintptr_t)a;
                    snprintf(buf, sizeof(buf),
                             "UNRECOVERED FAULT PC+0x%llx @%p insn=%08x",
                             (unsigned long long)(a - ctx->pc.x),
                             (void*)a, insn);
                    compatLogRaw(buf);
                }
            }
        }
    }

    auto dumpFaultMemory = [&](const char* label, uint64_t addr) {
        if (!addr) return;
        MemoryInfo mi = {};
        u32 pi = 0;
        if (R_FAILED(svcQueryMemory(&mi, &pi, addr)) ||
            !(mi.perm & Perm_R)) {
            return;
        }

        const uint64_t region_end = mi.addr + mi.size;
        uint64_t start = (addr >= 0x20) ? (addr - 0x20) : addr;
        start &= ~0x7ULL;
        if (start < mi.addr) start = mi.addr;
        const uint64_t end = start + 0x40;
        if (end > region_end) return;

        for (uint64_t a = start; a < end; a += 8) {
            const uint64_t v =
                *(const volatile uint64_t*)(uintptr_t)a;
            snprintf(buf, sizeof(buf),
                     "UNRECOVERED FAULT MEM %s @%p = %016llx",
                     label, (void*)a, (unsigned long long)v);
            compatLogRaw(buf);
        }
    };

    dumpFaultMemory("x0", ctx->cpu_gprs[0].x);
    dumpFaultMemory("x3", ctx->cpu_gprs[3].x);
    dumpFaultMemory("x10", ctx->cpu_gprs[10].x);
    dumpFaultMemory("x20", ctx->cpu_gprs[20].x);
    dumpFaultMemory("x23", ctx->cpu_gprs[23].x);

    uint32_t alloc_kind = 0, alloc_phase = 0;
    uint64_t alloc_caller = 0, alloc_ptr = 0, alloc_size = 0, alloc_size2 = 0;
    shimLastAllocatorEvent(&alloc_kind, &alloc_phase, &alloc_caller,
                           &alloc_ptr, &alloc_size, &alloc_size2);
    if (alloc_kind != 0) {
        const char* kind_name = "unknown";
        switch (alloc_kind) {
            case 1: kind_name = "malloc"; break;
            case 2: kind_name = "calloc"; break;
            case 3: kind_name = "free"; break;
            case 4: kind_name = "realloc"; break;
        }
        char alloc_caller_desc[256] = {};
        elfDescribePc(alloc_caller, alloc_caller_desc, sizeof(alloc_caller_desc));
        compatLogFmt("UNRECOVERED FAULT LAST ALLOC: kind=%s phase=%s caller=%p (%s) ptr=%p size=%llu size2=%llu",
                     kind_name,
                     alloc_phase == 1 ? "ENTER" : "RETURN",
                     (void*)alloc_caller,
                     alloc_caller_desc,
                     (void*)alloc_ptr,
                     (unsigned long long)alloc_size,
                     (unsigned long long)alloc_size2);
    }

    elfLogAddrInfo("UNRECOVERED FAULT pc", ctx->pc.x);
    elfLogAddrInfo("UNRECOVERED FAULT far", ctx->far.x);
}

// ─── LoadedSo::findSym ────────────────────────────────────────────────────────
void* LoadedSo::findSym(const char* name) const {
    if (!symtab || !strtab) return nullptr;
    for (uint32_t i = 1; i < sym_count; i++) {
        const Elf64_Sym& s = symtab[i];
        if (s.st_shndx == SHN_UNDEF || s.st_value == 0) continue;
        // sym_count is derived from the gap between .dynsym and .dynstr, which
        // often includes .gnu.version bytes interpreted as fake Elf64_Sym entries.
        // Those fake entries can have wild st_name values that walk off the end
        // of the string table — guard before dereferencing.
        if (strsz > 0 && (uint64_t)s.st_name >= strsz) continue;
        // Garbage entries past real dynsym (from .gnu.version etc.) often have
        // st_value >> alloc_size.  Reject them to prevent wrong cross-library resolution.
        if (alloc_size > 0 && s.st_value >= alloc_size) continue;
        const char* sname = strtab + s.st_name;
        if (strcmp(sname, name) == 0) {
            if (data_alloc && data_vaddr > 0 && s.st_value >= data_vaddr)
                return data_alloc + (s.st_value - data_vaddr);
            return base + s.st_value;
        }
    }
    return nullptr;
}

// Build the global export index once per loaded library. This replaces the
// expensive nested lookup (every relocation -> every SO -> every dynsym entry)
// with one dynsym scan at library load time and O(1) name lookup afterwards.
// emplace() deliberately preserves the first-loaded definition, matching the
// resolver's existing search order.
static void indexLoadedSoSymbols(LoadedSo* so) {
    if (!so || !so->symtab || !so->strtab || so->sym_count == 0)
        return;

    for (uint32_t i = 1; i < so->sym_count; ++i) {
        const Elf64_Sym& sym = so->symtab[i];
        if (sym.st_shndx == SHN_UNDEF || sym.st_value == 0)
            continue;
        if (so->strsz > 0 && (uint64_t)sym.st_name >= so->strsz)
            continue;
        if (so->alloc_size > 0 && sym.st_value >= so->alloc_size)
            continue;

        const char* name = so->strtab + sym.st_name;
        if (!name[0])
            continue;

        void* addr = nullptr;
        if (so->data_alloc && so->data_vaddr > 0 && sym.st_value >= so->data_vaddr)
            addr = so->data_alloc + (sym.st_value - so->data_vaddr);
        else
            addr = so->base + sym.st_value;

        g_exported_symbols.emplace(name, addr);
    }
}

extern "C" bool compatActivateFarCryProfile(const char* profile) {
    // Exact CryCommon vtable order for this build:
    // ISystem::GetIConsole() = 24, IConsole::GetCVar() = 20,
    // IConsole::ExecuteString() = 32, ICVar::GetString()/Set() = 3/4.
    // Nearby slots are different methods (ISystem[27] is GetISoundSystem,
    // IConsole[21] is GetFont), so using them here can jump directly into
    // unrelated guest code with the profile arguments and corrupt execution.
    if (!profile || !*profile)
        return false;

    LoadedSo* gameSo = nullptr;
    for (LoadedSo* so : g_loaded_sos) {
        if (!so)
            continue;
        const char* p = so->path.c_str();
        const char* base = std::strrchr(p, '/');
        base = base ? base + 1 : p;
        if (std::strcmp(base, "libCryGame.so") == 0) {
            gameSo = so;
            break;
        }
    }

    if (!gameSo) {
        compatLog("PROFILE CVAR: libCryGame.so not loaded yet");
        return false;
    }

    // rohit-n/NearChuckle's Far Cry game module defines a global GetISystem().
    // Use the guest function and then the exact virtual interface order from
    // CryCommon: ISystem::GetIConsole() is slot 24, IConsole::GetCVar() slot
    // 20, and ICVar::Set(const char*) slot 4.
    using GetISystemFn = void* (*)();
    GetISystemFn getISystem =
        reinterpret_cast<GetISystemFn>(gameSo->findSym("_Z10GetISystemv"));
    if (!getISystem) {
        compatLog("PROFILE CVAR: GetISystem symbol not found");
        return false;
    }

    void* system = getISystem();
    if (!system) {
        compatLog("PROFILE CVAR: GetISystem returned NULL");
        return false;
    }

    void*** systemVtable = reinterpret_cast<void***>(system);
    if (!systemVtable || !*systemVtable) {
        compatLog("PROFILE CVAR: invalid ISystem vtable");
        return false;
    }

    using GetIConsoleFn = void* (*)(void*);
    auto getIConsole =
        reinterpret_cast<GetIConsoleFn>((*systemVtable)[24]);
    void* console = getIConsole(system);
    if (!console) {
        compatLog("PROFILE CVAR: GetIConsole returned NULL");
        return false;
    }

    void*** consoleVtable = reinterpret_cast<void***>(console);
    if (!consoleVtable || !*consoleVtable) {
        compatLog("PROFILE CVAR: invalid IConsole vtable");
        return false;
    }

    using GetCVarFn = void* (*)(void*, const char*, bool);
    auto getCVar =
        reinterpret_cast<GetCVarFn>((*consoleVtable)[20]);
    void* cvar = getCVar(console, "g_playerprofile", true);
    if (!cvar) {
        compatLog("PROFILE CVAR: g_playerprofile not found");
        return false;
    }

    void*** cvarVtable = reinterpret_cast<void***>(cvar);
    if (!cvarVtable || !*cvarVtable) {
        compatLog("PROFILE CVAR: invalid ICVar vtable");
        return false;
    }

    using GetStringFn = char* (*)(void*);
    using SetStringFn = void (*)(void*, const char*);
    auto getString =
        reinterpret_cast<GetStringFn>((*cvarVtable)[3]);
    auto setString =
        reinterpret_cast<SetStringFn>((*cvarVtable)[4]);

    const char* before = getString(cvar);
    compatLogFmt("PROFILE CVAR: before=%s set=%s",
                 before ? before : "(null)", profile);

    setString(cvar, profile);

    const char* after = getString(cvar);
    if (after && std::strcmp(after, profile) == 0) {
        compatSetActiveFarCryProfile(profile);
        compatLogFmt("PROFILE CVAR: after=%s result=OK",
                     after);
        return true;
    }

    // Some Far Cry builds keep the console variable mirrored through the
    // script/console layer. Retry through the real console command path so
    // the active profile state is changed in the same way as an in-game CVar
    // command, rather than relying only on the ICVar object's setter.
    using ExecuteStringFn = void (*)(void*, const char*, bool, bool);
    auto executeString =
        reinterpret_cast<ExecuteStringFn>((*consoleVtable)[32]);

    char command[512];
    std::snprintf(command, sizeof(command),
                  "g_playerprofile %s", profile);
    compatLogFmt("PROFILE CVAR: direct Set failed (after=%s), executing: %s",
                 after ? after : "(null)", command);
    executeString(console, command, false, true);

    after = getString(cvar);
    const bool ok = after && std::strcmp(after, profile) == 0;
    compatLogFmt("PROFILE CVAR: after=%s result=%s",
                 after ? after : "(null)", ok ? "OK" : "FAIL");
    if (ok)
        compatSetActiveFarCryProfile(profile);
    return ok;
}

extern "C" bool compatLoadFarCryProfileConfiguration(const char* profile) {
    if (!profile || !*profile)
        return false;

    LoadedSo* gameSo = nullptr;
    for (LoadedSo* so : g_loaded_sos) {
        if (!so)
            continue;
        const char* p = so->path.c_str();
        const char* base = std::strrchr(p, '/');
        base = base ? base + 1 : p;
        if (std::strcmp(base, "libCryGame.so") == 0) {
            gameSo = so;
            break;
        }
    }
    if (!gameSo)
        return false;

    using GetISystemFn = void* (*)();
    auto getISystem =
        reinterpret_cast<GetISystemFn>(gameSo->findSym("_Z10GetISystemv"));
    if (!getISystem)
        return false;

    void* system = getISystem();
    if (!system)
        return false;

    void*** systemVtable = reinterpret_cast<void***>(system);
    if (!systemVtable || !*systemVtable)
        return false;

    // ISystem::GetIScriptSystem() is slot 25 in CryCommon's interface.
    using GetIScriptSystemFn = void* (*)(void*);
    auto getIScriptSystem =
        reinterpret_cast<GetIScriptSystemFn>((*systemVtable)[25]);
    if (!getIScriptSystem)
        return false;

    void* scriptSystem = getIScriptSystem(system);
    if (!scriptSystem) {
        compatLog("PROFILE CONFIG: GetIScriptSystem returned NULL");
        return false;
    }

    void*** scriptVtable = reinterpret_cast<void***>(scriptSystem);
    if (!scriptVtable || !*scriptVtable)
        return false;

    // IScriptSystem::ExecuteBuffer() is slot 3.
    using ExecuteBufferFn = bool (*)(void*, const char*, size_t);
    auto executeBuffer =
        reinterpret_cast<ExecuteBufferFn>((*scriptVtable)[3]);
    if (!executeBuffer)
        return false;

    char escaped[256];
    size_t out = 0;
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(profile);
         *p && out + 2 < sizeof(escaped); ++p) {
        if (*p == '\\' || *p == '"')
            escaped[out++] = '\\';
        escaped[out++] = static_cast<char>(*p);
    }
    escaped[out] = '\0';

    char script[384];
    const int n = std::snprintf(
        script, sizeof(script),
        "Game:LoadConfiguration(\"%s\")", escaped);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(script)) {
        compatLog("PROFILE CONFIG: script command buffer overflow");
        return false;
    }

    compatLogFmt("PROFILE CONFIG: loading selected profile=%s", profile);
    const bool ok = executeBuffer(scriptSystem, script, static_cast<size_t>(n));
    compatLogFmt("PROFILE CONFIG: Game:LoadConfiguration result=%s",
                 ok ? "OK" : "FAIL");
    return ok;
}

extern "C" void compatPollFarCryBackgroundVideoSave() {
    static bool initialized = false;
    static int lastValue = -1;

    LoadedSo* sysSo = nullptr;
    for (LoadedSo* so : g_loaded_sos) {
        if (!so)
            continue;
        const char* p = so->path.c_str();
        const char* base = std::strrchr(p, '/');
        base = base ? base + 1 : p;
        if (std::strcmp(base, "libCrySystem.so") == 0) {
            sysSo = so;
            break;
        }
    }
    if (!sysSo)
        return;

    using GetISystemFn = void* (*)();
    auto getISystem = reinterpret_cast<GetISystemFn>(sysSo->findSym("_Z10GetISystemv"));
    if (!getISystem)
        return;

    void* system = getISystem();
    if (!system)
        return;

    void*** systemVtable = reinterpret_cast<void***>(system);
    if (!systemVtable || !*systemVtable)
        return;

    using GetIConsoleFn = void* (*)(void*);
    auto getIConsole = reinterpret_cast<GetIConsoleFn>((*systemVtable)[24]);
    if (!getIConsole)
        return;

    void* console = getIConsole(system);
    if (!console)
        return;

    void*** consoleVtable = reinterpret_cast<void***>(console);
    if (!consoleVtable || !*consoleVtable)
        return;

    using GetCVarFn = void* (*)(void*, const char*, bool);
    auto getCVar = reinterpret_cast<GetCVarFn>((*consoleVtable)[20]);
    if (!getCVar)
        return;

    void* cvar = getCVar(console, "ui_BackGroundVideo", true);
    if (!cvar)
        return; // UI CVar has not been created yet.

    void*** cvarVtable = reinterpret_cast<void***>(cvar);
    if (!cvarVtable || !*cvarVtable)
        return;

    using GetIValFn = int (*)(void*);
    auto getIVal = reinterpret_cast<GetIValFn>((*cvarVtable)[1]);
    if (!getIVal)
        return;

    const int value = getIVal(cvar) != 0 ? 1 : 0;
    if (!initialized) {
        initialized = true;
        lastValue = value;
        return;
    }

    if (value == lastValue)
        return;

    compatLogFmt("PROFILE CVar: ui_BackGroundVideo changed %d -> %d; saving profile",
                 lastValue, value);
    lastValue = value;

    if (!compatSaveFarCryConfiguration()) {
        compatLog("PROFILE CVar: failed to save ui_BackGroundVideo");
    }
}

extern "C" bool compatSaveFarCryConfiguration() {
    // Call the actual non-virtual CSystem::SaveConfiguration() symbol. This
    // preserves the engine's own serialization rules:
    //   selected profile -> <profile>_system.cfg / <profile>_game.cfg
    //   root system.cfg/game.cfg -> bootstrap/current root state
    //
    // This is intentionally callable only from the deferred Switch poll,
    // never from fopen/open/Lua callbacks.
    LoadedSo* sysSo = nullptr;
    for (LoadedSo* so : g_loaded_sos) {
        if (!so)
            continue;
        const char* p = so->path.c_str();
        const char* base = std::strrchr(p, '/');
        base = base ? base + 1 : p;
        if (std::strcmp(base, "libCrySystem.so") == 0) {
            sysSo = so;
            break;
        }
    }
    if (!sysSo)
        return false;

    using GetISystemFn = void* (*)();
    auto getISystem =
        reinterpret_cast<GetISystemFn>(sysSo->findSym("_Z10GetISystemv"));
    if (!getISystem)
        return false;

    void* system = getISystem();
    if (!system)
        return false;

    using SaveConfigurationFn = void (*)(void*);
    auto saveConfiguration =
        reinterpret_cast<SaveConfigurationFn>(
            sysSo->findSym("_ZN7CSystem17SaveConfigurationEv"));
    if (!saveConfiguration)
        return false;

    compatLog("PROFILE SAVE: calling real CSystem::SaveConfiguration()");
    saveConfiguration(system);
    compatLog("PROFILE SAVE: real CSystem::SaveConfiguration() returned");
    return true;
}

// ─── Global symbol resolver ───────────────────────────────────────────────────
// Checks our shim table FIRST so Switch-compatible implementations always win
// over any Bionic copies embedded in libapplovin.so / libquack.so.
// Allocator entry points are worth saying out loud. sh_free counted 10 calls
// against 34 faults, so the game is reaching newlib's free without passing
// through the shim, and where each of these actually binds is the difference
// between "the table was bypassed" and "something else calls free directly".
static bool isAllocSym(const char* n) {
    return strcmp(n, "free") == 0 || strcmp(n, "malloc") == 0 ||
           strcmp(n, "realloc") == 0 || strcmp(n, "calloc") == 0 ||
           strcmp(n, "CryMalloc") == 0 || strcmp(n, "CryRealloc") == 0 ||
           strcmp(n, "CryReallocSize") == 0 || strcmp(n, "CryFree") == 0 ||
           strcmp(n, "CryFreeSize") == 0 ||
           strcmp(n, "CryModuleMalloc") == 0 ||
           strcmp(n, "CryModuleRealloc") == 0 ||
           strcmp(n, "CryModuleReallocSize") == 0 ||
           strcmp(n, "CryModuleFree") == 0 ||
           strcmp(n, "CryModuleFreeSize") == 0 ||
           strcmp(n, "_ZdlPv") == 0 || strcmp(n, "_ZdaPv") == 0 ||
           strcmp(n, "_ZdlPvm") == 0 || strcmp(n, "_Znwm") == 0 ||
           strcmp(n, "_Znam") == 0;
}

// Filesystem imports that are especially relevant to CryEngine/CryPak shader
// lookup. These are diagnostic-only: enabling the trace must not alter binding.
static bool isFsTraceSym(const char* n) {
    if (!n || !*n) return false;
    return strcmp(n, "fopen") == 0 || strcmp(n, "fopen64") == 0 ||
           strcmp(n, "open") == 0 || strcmp(n, "opendir") == 0 ||
           strcmp(n, "readdir") == 0 || strcmp(n, "closedir") == 0 ||
           strcmp(n, "mkdir") == 0 || strcmp(n, "_mkdir") == 0 ||
           strcmp(n, "__mkdir") == 0 || strcmp(n, "mkdirat") == 0 ||
           strcmp(n, "__mkdirat") == 0 ||
           strcmp(n, "strcasecmp") == 0 || strcmp(n, "__strcasecmp") == 0 ||
           strcmp(n, "_findfirst64") == 0 || strcmp(n, "_findnext64") == 0 ||
           strcmp(n, "_findclose") == 0 || strcmp(n, "stat") == 0 ||
           strcmp(n, "stat64") == 0 || strcmp(n, "access") == 0 ||
           strcmp(n, "fstat") == 0 || strcmp(n, "fstat64") == 0 ||
           strcmp(n, "read") == 0 || strcmp(n, "lseek") == 0;
}

static bool isBinkAudioTraceSym(const char* n) {
    if (!n || !*n) return false;
    return strcmp(n, "CS_Stream_Create") == 0 ||
           strcmp(n, "CS_Stream_Play") == 0 ||
           strcmp(n, "CS_Stream_Stop") == 0 ||
           strcmp(n, "CS_Stream_Close") == 0 ||
           strcmp(n, "CS_Update") == 0;
}

static bool isCtypeTraceSym(const char* n) {
    if (!n || !*n) return false;
    return strcmp(n, "_ctype_") == 0 ||
           strcmp(n, "_toupper_tab_") == 0 ||
           strcmp(n, "_tolower_tab_") == 0 ||
           strcmp(n, "__ctype_b_loc") == 0 ||
           strcmp(n, "__ctype_tolower_loc") == 0 ||
           strcmp(n, "__ctype_toupper_loc") == 0 ||
           strcmp(n, "__ctype_get_mb_cur_max") == 0 ||
           strcmp(n, "tolower") == 0 ||
           strcmp(n, "toupper") == 0 ||
           strcmp(n, "isspace") == 0 ||
           strcmp(n, "isprint") == 0 ||
           strcmp(n, "isalpha") == 0 ||
           strcmp(n, "isdigit") == 0 ||
           strcmp(n, "isalnum") == 0 ||
           strcmp(n, "ispunct") == 0;
}
static void* resolveSymbol(const char* name) {
    if (!name || !name[0]) return nullptr;
    const bool trace = isAllocSym(name) || isFsTraceSym(name) ||
                       isCtypeTraceSym(name) || isBinkAudioTraceSym(name);

    // Successful bindings are stable for the lifetime of this process under
    // the resolver's existing first-loaded-definition semantics. Check the
    // cache BEFORE the shim table: shimResolve() is itself a linear scan over
    // every compatibility entry, so calling it for every repeated relocation
    // would erase most of the benefit of caching.
    auto cached = g_symbol_cache.find(name);
    if (cached != g_symbol_cache.end()) {
        if (trace) compatLogFmt("bind: %s -> cached %p", name, cached->second);
        return cached->second;
    }

    // Shim table takes priority over guest libraries.
    void* shim = shimResolve(name);
    if (shim) {
        if (trace) compatLogFmt("bind: %s -> shim %p", name, shim);
        g_symbol_cache.emplace(name, shim);
        return shim;
    }

    // Then the game's own libraries. The export index was built once when
    // each library was registered, preserving the first-loaded definition while
    // avoiding a full dynsym scan for every relocation.
    auto exported = g_exported_symbols.find(name);
    if (exported != g_exported_symbols.end()) {
        if (trace) compatLogFmt("bind: %s -> indexed %p", name, exported->second);
        g_symbol_cache.emplace(name, exported->second);
        return exported->second;
    }

    // Keep the original linear search as a safety net for any unusual symbol
    // that was intentionally excluded from the index.
    for (LoadedSo* so : g_loaded_sos) {
        void* p = so->findSym(name);
        if (p) {
            if (trace) compatLogFmt("bind: %s -> %s %p", name, so->path.c_str(), p);
            g_symbol_cache.emplace(name, p);
            return p;
        }
    }

    // Last: Unity/IL2CPP libc gap fillers. Do not cache this result:
    // a later dlopen() can load a guest library that defines the same symbol,
    // and the existing resolver would then select that newly available guest
    // definition before falling back.
    void* fb = shimResolveFallback(name);
    if (fb) {
        if (trace) compatLogFmt("bind: %s -> fallback %p", name, fb);
        return fb;
    }

    if (trace) compatLogFmt("bind: %s -> UNRESOLVED", name);
    return nullptr;
}

// ─── Real dlopen/dlsym backing ─────────────────────────────────────────────────
// Games that load their own native deps at runtime (Unity's NativeLoader.load
// → dlopen("libunity.so"), GPG, etc.) go through libdl, which our shim table
// routes here instead of the fake 0xDEAD stub. We back it with the same ELF
// loader + loaded-so registry used for the initial batch.
static std::string g_dlopen_dir;

void elfSetDlopenDir(const char* lib_dir) {
    g_dlopen_dir = lib_dir ? lib_dir : "";
}

static const char* baseName(const char* path) {
    const char* s = strrchr(path, '/');
    return s ? s + 1 : path;
}

LoadedSo* elfFindLoaded(const char* basename) {
    if (!basename) return nullptr;
    for (LoadedSo* so : g_loaded_sos)
        if (strcmp(baseName(so->path.c_str()), basename) == 0) return so;
    return nullptr;
}

LoadedSo* elfDlopen(const char* name) {
    if (!name || !name[0]) return nullptr;
    const char* bn = baseName(name);

    // Already loaded (part of the initial batch, or an earlier dlopen)?
    if (LoadedSo* existing = elfFindLoaded(bn)) {
        compatLogFmt("dlopen: %s already loaded → %p", bn, (void*)existing);
        return existing;
    }

    if (g_dlopen_dir.empty()) {
        compatLogFmt("dlopen: %s not loaded and no dlopen dir set — failing", bn);
        return nullptr;
    }
    std::string path = g_dlopen_dir + "/" + bn;
    compatLogFmt("dlopen: loading %s on demand from %s", bn, path.c_str());
    LoadedSo* so = elfLoad(path.c_str(), nullptr);
    if (!so) {
        compatLogFmt("dlopen: elfLoad failed for %s", path.c_str());
        return nullptr;
    }
    // Real dlopen runs the library's constructors before returning.
    elfRunCtors(so, nullptr);
    compatLogFmt("dlopen: %s loaded + ctors run → %p", bn, (void*)so);
    return so;
}

extern "C" int compatVideoPanelIsPlaying(void* self);
extern "C" volatile int g_near_video_open_failed;
extern "C" volatile uint32_t g_near_video_panel_finished_offset;

static bool patchVideoPanelIsPlaying(LoadedSo* so, uint8_t* stage_base,
                                      uint64_t min_vaddr, size_t alloc_size) {
    (void)so;

    if (!stage_base || !alloc_size)
        return false;

    // CUIVideoPanel::IsPlaying() in the Android source is:
    //
    //   int CUIVideoPanel::IsPlaying() { return !m_bFinished; }
    //
    // With clang/AArch64 this compiles to:
    //
    //   ldrb  wN, [x0, #m_bFinished]
    //   eor   w0, wN, #1
    //   ret
    //
    // The method is not guaranteed to be present in .dynsym, so use this
    // exact three-instruction body as a fallback signature scan.
    int matches = 0;
    uint32_t* match = nullptr;
    uint32_t matchOffset = 0xffffffffu;
    uint32_t fieldOffset = 0;

    auto isLdrbFromX0 = [](uint32_t w, unsigned& rt, unsigned& imm12) -> bool {
        // LDRB Wt, [Xn, #imm12]
        if ((w & 0xffc00000u) != 0x39400000u)
            return false;
        if (((w >> 5) & 31u) != 0) // Rn must be X0
            return false;
        rt = w & 31u;
        imm12 = (w >> 10) & 0xfffu;
        return true;
    };

    auto isEorW0Imm1 = [](uint32_t w, unsigned& rn) -> bool {
        // EOR W0, Wn, #1. For the 32-bit logical-immediate encoding, #1
        // has N=0, immr=0, imms=0, leaving Rn in bits [9:5] and Rd=0.
        if ((w & 0xfffffc1fu) != 0x52000000u)
            return false;
        rn = (w >> 5) & 31u;
        return true;
    };

    // stage_base is the ELF load-bias pointer (stage - min_vaddr). The
    // actual staged image begins at stage_base + min_vaddr.
    const uint8_t* bytes = stage_base + min_vaddr;
    for (size_t off = 0; off + 12 <= alloc_size; off += 4) {
        uint32_t w0, w1, w2;
        std::memcpy(&w0, bytes + off + 0, sizeof(w0));
        std::memcpy(&w1, bytes + off + 4, sizeof(w1));
        std::memcpy(&w2, bytes + off + 8, sizeof(w2));

        unsigned rt = 0, imm12 = 0, rn = 0;
        if (!isLdrbFromX0(w0, rt, imm12))
            continue;
        if (rt != rn) {
            // rn is set by the EOR test below; keep this check after it.
        }
        if (!isEorW0Imm1(w1, rn))
            continue;
        if (rn != rt)
            continue;
        if (w2 != 0xd65f03c0u) // RET
            continue;

        ++matches;
        match = (uint32_t*)(bytes + off);
        matchOffset = (uint32_t)off;
        fieldOffset = imm12;

        compatLogFmt("VIDEO PANEL SIGNATURE CANDIDATE: vaddr=0x%llx field=0x%x words=%08x %08x %08x",
                     (unsigned long long)(min_vaddr + off), imm12, w0, w1, w2);
    }

    if (matches == 0) {
        compatLog("VIDEO PANEL PATCH: native IsPlaying signature not found");
        return false;
    }

    if (matches > 1) {
        compatLogFmt("VIDEO PANEL PATCH: native signature ambiguous (%d matches); no code patched",
                     matches);
        return false;
    }

    // We only replace the tiny native IsPlaying() body. Returning zero makes
    // failed/missing intro videos appear finished to the Lua video sequencer,
    // while leaving every other UI method untouched. This is deliberate for
    // the Switch bring-up where the AMD64 intro movies are not available.
    const uint32_t old0 = match[0];
    const uint32_t old1 = match[1];
    const uint32_t old2 = match[2];

    match[0] = 0x2a1f03e0u; // MOV W0, WZR
    match[1] = 0xd65f03c0u; // RET
    match[2] = 0xd503201fu; // NOP

    g_near_video_panel_finished_offset = fieldOffset;

    compatLogFmt("VIDEO PANEL PATCH: IsPlaying vaddr=0x%08x -> return 0 (field=0x%x)",
                 (unsigned)(min_vaddr + matchOffset), fieldOffset);
    compatLogFmt("VIDEO PANEL PATCH OLD: %08x %08x %08x",
                 old0, old1, old2);
    return true;
}

// The previous input A/B experiments replaced BindCommandToKey() and a
// suspected input thunk with MOV W0, WZR; RET to get past an early crash.
// Those patches also disabled the actual Android/SDL input delivery path, so
// they must stay out of the functional runtime. The crash investigation can
// continue independently without silently swallowing Input:* callbacks.

// A/B diagnostic: bypass CSystem::Update() entirely and return true.
static bool patchFarCryDisplayInfoDefault(LoadedSo* so, uint8_t* stage_base,
                                               uint64_t min_vaddr, size_t alloc_size) {
    if (!so || !stage_base || !alloc_size)
        return false;

    const char* path = so->path.c_str();
    const char* base = std::strrchr(path, '/');
    base = base ? base + 1 : path;
    if (std::strcmp(base, "libCrySystem.so") != 0)
        return false;

    // The release CryEngine creates:
    //   CreateVariable("r_DisplayInfo", "0", VF_DUMPTODISK, ...);
    //
    // Do not depend on the two strings being adjacent in .rodata, nor on
    // CreateRendererVars() being present in the dynamic symbol table. Instead
    // locate the ARM64 code xref that loads the r_DisplayInfo name into x0 and
    // then the nearby load of the default string into x1 (the second argument
    // of CreateVariable). Redirect that x1 load to the existing "1" literal.
    constexpr const char kName[] = "r_DisplayInfo";
    constexpr const char kZero[] = "0";
    constexpr const char kOne[] = "1";

    const uintptr_t image_base = reinterpret_cast<uintptr_t>(so->base);
    const uint64_t mapped_start = image_base + min_vaddr;
    const uint64_t mapped_end = mapped_start + alloc_size;
    uint8_t* image = stage_base + min_vaddr;

    auto is_adrp = [](uint32_t w) {
        return (w & 0x9f000000u) == 0x90000000u;
    };

    auto is_add_imm = [](uint32_t w) {
        return (w & 0xffc00000u) == 0x91000000u;
    };

    auto is_ldr_unsigned_64 = [](uint32_t w) {
        return (w & 0xffc00000u) == 0xf9400000u;
    };

    auto adrp_target = [](uint64_t pc, uint32_t insn) -> uint64_t {
        int64_t imm21 = ((int64_t)((insn >> 5) & 0x7ffffu) |
                         ((int64_t)((insn >> 29) & 0x3u) << 19));
        if (imm21 & (1LL << 20))
            imm21 |= ~((1LL << 21) - 1);
        return (pc & ~0xfffULL) + (imm21 << 12);
    };

    auto add_target = [](uint64_t page, uint32_t insn) -> uint64_t {
        const uint64_t imm12 = (insn >> 10) & 0xfff;
        return page + imm12;
    };

    auto ldr_target = [](uint64_t page, uint32_t insn) -> uint64_t {
        const uint64_t imm12 = (insn >> 10) & 0xfff;
        return page + (imm12 << 3);
    };

    struct Ref {
        uint64_t target = 0;
        uint32_t* first = nullptr;
        uint32_t* second = nullptr;
        unsigned reg = 0;
        bool add_pair = false;
        size_t byte_off = 0;
    };

    auto make_ref = [&](uint8_t* code, uint64_t code_vaddr,
                        size_t off, const char* wanted,
                        Ref& out) -> bool {
        if (off + 8 > alloc_size)
            return false;

        uint32_t* w = reinterpret_cast<uint32_t*>(code + off);
        if (!is_adrp(w[0]))
            return false;

        const unsigned rd = w[0] & 31u;
        const uint64_t pc = image_base + code_vaddr + off;
        const uint64_t page = adrp_target(pc, w[0]);

        if (is_add_imm(w[1])) {
            const unsigned rd1 = w[1] & 31u;
            const unsigned rn1 = (w[1] >> 5) & 31u;
            if (rd1 != rd || rn1 != rd)
                return false;

            const uint64_t target = add_target(page, w[1]);
            if (target < mapped_start || target + std::strlen(wanted) + 1 > mapped_end)
                return false;

            const uint64_t tv = target - image_base;
            if (tv < min_vaddr || tv - min_vaddr >= alloc_size)
                return false;

            const char* s = reinterpret_cast<const char*>(
                stage_base + (tv - min_vaddr));
            if (std::strcmp(s, wanted) != 0)
                return false;

            out.target = target;
            out.first = &w[0];
            out.second = &w[1];
            out.reg = rd;
            out.add_pair = true;
            out.byte_off = code_vaddr + off;
            return true;
        }

        if (is_ldr_unsigned_64(w[1])) {
            const unsigned rt = w[1] & 31u;
            const unsigned rn = (w[1] >> 5) & 31u;
            if (rn != rd)
                return false;

            const uint64_t target = ldr_target(page, w[1]);
            if (target < mapped_start || target + std::strlen(wanted) + 1 > mapped_end)
                return false;

            const uint64_t tv = target - image_base;
            if (tv < min_vaddr || tv - min_vaddr >= alloc_size)
                return false;

            const char* s = reinterpret_cast<const char*>(
                stage_base + (tv - min_vaddr));
            if (std::strcmp(s, wanted) != 0)
                return false;

            out.target = target;
            out.first = &w[0];
            out.second = &w[1];
            out.reg = rt;
            out.add_pair = false;
            out.byte_off = code_vaddr + off;
            return true;
        }

        return false;
    };

    // Search the whole mapped image for code xrefs. The dynamic symbol table
    // may not contain CreateRendererVars(), so the CVar name itself is the
    // reliable anchor.
    std::vector<Ref> names;
    for (size_t off = 0; off + 8 <= alloc_size; off += 4) {
        Ref ref;
        if (make_ref(image, min_vaddr, off, kName, ref))
            names.push_back(ref);
    }

    if (names.empty()) {
        compatLog("FARCRY DISPLAYINFO PATCH: no ARM64 xref to r_DisplayInfo found");
        return false;
    }

    Ref* selected_name = nullptr;
    Ref* selected_zero = nullptr;
    size_t candidate_count = 0;
    size_t best_distance = SIZE_MAX;
    static Ref zero_holder;

    for (Ref& name_ref : names) {
        const size_t name_off =
            name_ref.byte_off >= min_vaddr
                ? static_cast<size_t>(name_ref.byte_off - min_vaddr)
                : 0;

        // Inspect one small basic-block-sized window around the name reference.
        // CreateVariable(name, default, flags, help) receives name in x0 and
        // default in x1. The release binary can contain several literal "0"
        // references in the same function, so "exactly one" is not a safe
        // requirement. Select the closest x1 -> "0" reference to the
        // r_DisplayInfo name reference instead.
        const size_t begin = name_off > 0x80 ? name_off - 0x80 : 0;
        const size_t finish =
            name_off + 0x100 < alloc_size ? name_off + 0x100 : alloc_size - 8;

        for (size_t off = begin; off <= finish; off += 4) {
            Ref zero_ref;
            if (!make_ref(image, min_vaddr, off, kZero, zero_ref))
                continue;

            if (zero_ref.reg != 1)
                continue;

            const size_t distance = (off > name_off)
                ? off - name_off
                : name_off - off;
            if (distance > 0x80)
                continue;

            ++candidate_count;
            if (distance < best_distance) {
                best_distance = distance;
                zero_holder = zero_ref;
                selected_name = &name_ref;
                selected_zero = &zero_holder;
            }
        }
    }

    compatLogFmt(
        "FARCRY DISPLAYINFO PATCH: candidate xrefs names=%llu zero_x1=%llu best_distance=0x%llx",
        (unsigned long long)names.size(),
        (unsigned long long)candidate_count,
        (unsigned long long)(best_distance == SIZE_MAX ? 0 : best_distance));

    if (!selected_name || !selected_zero) {
        compatLog("FARCRY DISPLAYINFO PATCH: no usable x1 -> \"0\" reference found");
        return false;
    }

    // Find the nearest existing "1" literal code reference to reuse.
    std::vector<Ref> ones;
    for (size_t off = 0; off + 8 <= alloc_size; off += 4) {
        Ref ref;
        if (make_ref(image, min_vaddr, off, kOne, ref))
            ones.push_back(ref);
    }

    if (ones.empty()) {
        compatLog("FARCRY DISPLAYINFO PATCH: no ARM64 xref to string '1' found");
        return false;
    }

    Ref* one = &ones[0];
    size_t one_distance = SIZE_MAX;
    for (Ref& ref : ones) {
        const size_t a = ref.byte_off > selected_zero->byte_off
            ? ref.byte_off - selected_zero->byte_off
            : selected_zero->byte_off - ref.byte_off;
        if (a < one_distance) {
            one_distance = a;
            one = &ref;
        }
    }

    // The default argument is expected to be an ADRP+ADD pair. This can be
    // safely redirected without changing the number of instructions.
    if (!selected_zero->add_pair || !one->add_pair) {
        compatLog("FARCRY DISPLAYINFO PATCH: default/'1' reference is not ADRP+ADD");
        return false;
    }

    auto encode_adrp = [](uint64_t pc, uint64_t target, unsigned rd) -> uint32_t {
        const int64_t page_delta =
            (static_cast<int64_t>(target & ~0xfffULL) -
             static_cast<int64_t>(pc & ~0xfffULL)) >> 12;
        if (page_delta < -(1LL << 20) || page_delta >= (1LL << 20))
            return 0;
        const uint32_t imm = static_cast<uint32_t>(page_delta) & 0x1fffffu;
        return 0x90000000u |
               ((imm & 0x3u) << 29) |
               ((imm & 0x7ffffu) << 5) |
               (rd & 31u);
    };

    auto encode_add = [](uint64_t target, unsigned rd) -> uint32_t {
        const uint64_t imm12 = target & 0xfffULL;
        return 0x91000000u |
               (static_cast<uint32_t>(imm12) << 10) |
               (rd << 5) | rd;
    };

    const uint64_t patch_pc =
        image_base + selected_zero->byte_off;
    const uint32_t new_adrp =
        encode_adrp(patch_pc, one->target, selected_zero->reg);
    const uint32_t new_add =
        encode_add(one->target, selected_zero->reg);

    if (!new_adrp || !new_add) {
        compatLog("FARCRY DISPLAYINFO PATCH: failed to encode default string relocation");
        return false;
    }

    const uint32_t old0 = *selected_zero->first;
    const uint32_t old1 = *selected_zero->second;
    *selected_zero->first = new_adrp;
    *selected_zero->second = new_add;
    armICacheInvalidate(selected_zero->first, 8);

    compatLogFmt(
        "FARCRY DISPLAYINFO PATCH: CreateVariable(r_DisplayInfo,0) -> 1 "
        "at vaddr=0x%llx old=%08x %08x new=%08x %08x",
        (unsigned long long)selected_zero->byte_off,
        old0, old1, new_adrp, new_add);
    return true;
}

[[maybe_unused]] static bool patchFarCrySystemUpdate(LoadedSo* so, uint8_t* stage_base,
                                    uint64_t min_vaddr, size_t alloc_size) {
    if (!so || !stage_base || !alloc_size)
        return false;

    const char* path = so->path.c_str();
    const char* base = std::strrchr(path, '/');
    base = base ? base + 1 : path;
    if (std::strcmp(base, "libCrySystem.so") != 0)
        return false;

    constexpr const char* kSym = "_ZN7CSystem6UpdateEii";
    void* fn = so->findSym(kSym);
    if (!fn) {
        compatLogFmt("FARCRY SYSTEM UPDATE A/B: symbol not found: %s", kSym);
        return false;
    }

    const uintptr_t imageBase = reinterpret_cast<uintptr_t>(so->base);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(fn);
    if (addr < imageBase || addr - imageBase >= alloc_size) {
        compatLogFmt(
            "FARCRY SYSTEM UPDATE A/B: symbol outside image fn=%p base=%p size=0x%llx",
            fn, reinterpret_cast<void*>(imageBase),
            (unsigned long long)alloc_size);
        return false;
    }

    const uint64_t off = static_cast<uint64_t>(addr - imageBase);
    uint32_t* insn = reinterpret_cast<uint32_t*>(
        stage_base + min_vaddr + off);
    const uint32_t old0 = insn[0];
    const uint32_t old1 = insn[1];

    // mov w0, #1; ret — make ISystem::Update() return "continue".
    insn[0] = 0x52800020u;
    insn[1] = 0xd65f03c0u;
    armICacheInvalidate(insn, 8);

    compatLogFmt(
        "FARCRY SYSTEM UPDATE A/B: patched CSystem::Update +0x%llx old=%08x %08x new=%08x %08x",
        (unsigned long long)off, old0, old1, insn[0], insn[1]);
    return true;
}

// Far Cry's Linux/Android CXGame::GetPlayerProfilePath() uses __builtin_trap()
// for filesystem states that the original Android runtime considers impossible.
// Those traps are valid diagnostics on Android, but they are not valid recovery
// points on Switch. Do not NOP the BRK itself: the instructions after a BRK are
// unreachable in the original control flow and executing them corrupts state.
// Instead, redirect only the conditional branch that enters the trap block.
//
// We handle both compiler forms seen in AArch64 builds:
//   1) conditional branch -> BRK
//   2) conditional branch with BRK as its fall-through
// In both cases only the branch is changed; the rest of the function remains
// byte-for-byte intact.
[[maybe_unused]] static bool patchFarCryProfilePathTrapBranches(LoadedSo* so, uint8_t* stage_base,
                                                uint64_t min_vaddr, size_t alloc_size) {
    if (!so || !stage_base || !alloc_size)
        return false;

    const char* path = so->path.c_str();
    const char* base = std::strrchr(path, '/');
    base = base ? base + 1 : path;
    if (std::strcmp(base, "libCryGame.so") != 0)
        return false;

    constexpr const char* kSym = "_ZN6CXGame20GetPlayerProfilePathEv";
    void* fn = so->findSym(kSym);
    if (!fn) {
        compatLogFmt("FARCRY PROFILE PATH: symbol not found: %s", kSym);
        return false;
    }

    const uintptr_t image_base = reinterpret_cast<uintptr_t>(so->base);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(fn);
    if (addr < image_base || addr - image_base >= alloc_size) {
        compatLogFmt("FARCRY PROFILE PATH: symbol outside image fn=%p base=%p size=0x%llx",
                     fn, reinterpret_cast<void*>(image_base),
                     (unsigned long long)alloc_size);
        return false;
    }

    const uint64_t fn_off = static_cast<uint64_t>(addr - image_base);
    uint8_t* code = stage_base + min_vaddr + fn_off;
    constexpr size_t kScanBytes = 0x300;
    const size_t scan_bytes = std::min(kScanBytes, alloc_size - fn_off);

    auto signExtend = [](uint64_t value, unsigned bits) -> int64_t {
        const uint64_t m = 1ULL << (bits - 1);
        return static_cast<int64_t>((value ^ m) - m);
    };

    auto isCondBranch = [](uint32_t w) {
        return (w & 0xff000010u) == 0x54000000u; // B.cond
    };
    auto isCompareBranch = [](uint32_t w) {
        return (w & 0x7f000000u) == 0x34000000u; // CBZ/CBNZ
    };
    auto isTestBranch = [](uint32_t w) {
        return (w & 0x7f000000u) == 0x36000000u; // TBZ/TBNZ
    };
    auto isUnconditionalBranch = [](uint32_t w) {
        return (w & 0x7c000000u) == 0x14000000u; // B
    };

    auto isConditionalBranch = [&](uint32_t w) {
        return isCondBranch(w) || isCompareBranch(w) || isTestBranch(w);
    };

    auto isAnyBranch = [&](uint32_t w) {
        return isConditionalBranch(w) || isUnconditionalBranch(w);
    };

    auto branchTarget = [&](uint32_t w, uintptr_t pc) -> uintptr_t {
        if (isUnconditionalBranch(w)) {
            const int64_t imm = signExtend(
                (static_cast<uint64_t>(w & 0x03ffffffu)) << 2, 28);
            return static_cast<uintptr_t>(static_cast<int64_t>(pc) + imm);
        }
        if (isCondBranch(w) || isCompareBranch(w)) {
            const int64_t imm = signExtend(
                (static_cast<uint64_t>(w >> 5) & 0x7ffffULL) << 2, 21);
            return static_cast<uintptr_t>(static_cast<int64_t>(pc) + imm);
        }
        if (isTestBranch(w)) {
            const int64_t imm = signExtend(
                (static_cast<uint64_t>(w >> 5) & 0x3fffULL) << 2, 16);
            return static_cast<uintptr_t>(static_cast<int64_t>(pc) + imm);
        }
        return 0;
    };

    unsigned patched_to_trap = 0;
    unsigned patched_fallthrough = 0;

    for (size_t brk_off = 0; brk_off + 4 <= scan_bytes; brk_off += 4) {
        if (*reinterpret_cast<uint32_t*>(code + brk_off) != 0xd4200020u)
            continue;

        const uintptr_t brk_pc = reinterpret_cast<uintptr_t>(code + brk_off);

        // The trap block generated by clang may begin with a BL to a noreturn
        // helper and put BRK immediately after it:
        //
        //     <conditional branch>
        //     bl  ...
        //     brk #0
        //
        // In that layout the conditional branch is at BRK-8, not BRK-4.
        // Handle both possible CFG shapes without executing the dead code:
        //
        //   A) branch target is inside the trap block -> NOP the branch;
        //   B) trap is the branch fall-through -> turn the conditional branch
        //      into an unconditional B to its original non-trap target.
        const size_t lookback = std::min<size_t>(0x100, brk_off);
        compatLogFmt(
            "FARCRY PROFILE PATH: BRK candidate +0x%zx within GetPlayerProfilePath",
            brk_off);

        for (size_t dist = 4; dist <= lookback; dist += 4) {
            const size_t off = brk_off - dist;
            uint32_t* insn = reinterpret_cast<uint32_t*>(code + off);
            const uint32_t old = *insn;
            if (!isAnyBranch(old))
                continue;

            const uintptr_t pc = reinterpret_cast<uintptr_t>(code + off);
            const uintptr_t target = branchTarget(old, pc);
            if (!target)
                continue;

            // Case A: target lands in the small trap block immediately before
            // BRK. Removing that jump follows the normal fall-through path.
            if (target >= brk_pc - 0x30 && target <= brk_pc) {
                *insn = 0xd503201fu;
                armICacheInvalidate(insn, 4);
                ++patched_to_trap;
                compatLogFmt(
                    "FARCRY PROFILE PATH: bypass branch->trap at +0x%zx old=%08x new=%08x target=+0x%zx brk=+0x%zx",
                    off, old, *insn,
                    (size_t)(target - reinterpret_cast<uintptr_t>(code)), brk_off);
                break;
            }

            // Case B: the trap is the fall-through path and the branch target
            // lies beyond the trap block. Preserve that target unconditionally.
            if (isConditionalBranch(old) && target > brk_pc && dist <= 0x40) {
                const int64_t delta =
                    static_cast<int64_t>(target) - static_cast<int64_t>(pc);
                if ((delta & 3) == 0) {
                    const int64_t imm26 = delta >> 2;
                    if (imm26 >= -(1LL << 25) && imm26 < (1LL << 25)) {
                        *insn = 0x14000000u |
                                (static_cast<uint32_t>(imm26) & 0x03ffffffu);
                        armICacheInvalidate(insn, 4);
                        ++patched_fallthrough;
                        compatLogFmt(
                            "FARCRY PROFILE PATH: bypass trap fall-through at +0x%zx old=%08x new=%08x target=+0x%zx brk=+0x%zx",
                            off, old, *insn,
                            (size_t)(target - reinterpret_cast<uintptr_t>(code)), brk_off);
                        break;
                    }
                }
            }
        }
    }

    compatLogFmt(
        "FARCRY PROFILE PATH: patched %u trap-entry and %u trap-fallthrough branches at +0x%llx",
        patched_to_trap, patched_fallthrough, (unsigned long long)fn_off);
    return (patched_to_trap + patched_fallthrough) != 0;
}

// A/B: GetSaveGameList() is called by the Far Cry menu transition before
// loading the first level. The Switch build currently reaches the Android
// GetPlayerProfilePath() trap from this Lua callback. Return an empty result
// instead of entering the profile/save enumeration path so the level transition
// can continue. This does not touch the actual save-game path once a level is
// running; it only removes the failing menu-time enumeration call.
[[maybe_unused]] static bool patchFarCryGetSaveGameList(LoadedSo* so, uint8_t* stage_base,
                                       uint64_t min_vaddr, size_t alloc_size) {
    if (!so || !stage_base || !alloc_size)
        return false;

    const char* path = so->path.c_str();
    const char* base = std::strrchr(path, '/');
    base = base ? base + 1 : path;
    if (std::strcmp(base, "libCryGame.so") != 0)
        return false;

    constexpr const char* kSym =
        "_ZN17CScriptObjectGame15GetSaveGameListEP16IFunctionHandler";

    void* fn = so->findSym(kSym);
    if (!fn) {
        compatLogFmt("FARCRY SAVE LIST A/B: symbol not found: %s", kSym);
        return false;
    }

    const uintptr_t imageBase = reinterpret_cast<uintptr_t>(so->base);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(fn);
    if (addr < imageBase || addr - imageBase >= alloc_size) {
        compatLogFmt(
            "FARCRY SAVE LIST A/B: symbol outside image fn=%p base=%p size=0x%llx",
            fn, reinterpret_cast<void*>(imageBase),
            (unsigned long long)alloc_size);
        return false;
    }

    const uint64_t off = static_cast<uint64_t>(addr - imageBase);
    uint32_t* insn = reinterpret_cast<uint32_t*>(
        stage_base + min_vaddr + off);

    const uint32_t old0 = insn[0];
    const uint32_t old1 = insn[1];

    // return 0;
    insn[0] = 0x2a1f03e0u; // mov w0, wzr
    insn[1] = 0xd65f03c0u; // ret
    armICacheInvalidate(insn, 8);

    compatLogFmt(
        "FARCRY SAVE LIST A/B: patched GetSaveGameList +0x%llx old=%08x %08x new=%08x %08x",
        (unsigned long long)off, old0, old1, insn[0], insn[1]);
    return true;
}

// Far Cry's Android build reaches CScriptSystem::SetGlobalTagHandlerString()
// immediately before the current NULL-write fault. The public Far Cry source
// shows this callback copies the incoming Lua string into the pointer stored in
// the tagged userdata and then notifies the script sink. On Switch, the fault
// occurs inside this callback at libCryScriptSystem.so +0x39ab8 (+0x98 from the
// function start). Disable only this string-tag setter as an A/B experiment.
// Integer/float tagged globals and all other Lua callbacks remain untouched.
static bool patchFarCrySetGlobalTagHandlerString(LoadedSo* so, uint8_t* stage_base,
                                                uint64_t min_vaddr, size_t alloc_size) {
    if (!so || !stage_base || !alloc_size)
        return false;

    const char* path = so->path.c_str();
    const char* base = std::strrchr(path, '/');
    base = base ? base + 1 : path;
    if (std::strcmp(base, "libCryScriptSystem.so") != 0)
        return false;

    constexpr const char* kSym =
        "_ZN13CScriptSystem25SetGlobalTagHandlerStringEP9lua_State";

    void* fn = so->findSym(kSym);
    if (!fn) {
        compatLogFmt("FARCRY SCRIPT SETGLOBAL A/B: symbol not found: %s", kSym);
        return false;
    }

    const uintptr_t imageBase = (uintptr_t)so->base;
    const uintptr_t addr = (uintptr_t)fn;
    if (addr < imageBase || addr - imageBase >= alloc_size) {
        compatLogFmt(
            "FARCRY SCRIPT SETGLOBAL A/B: symbol outside image fn=%p base=%p size=0x%llx",
            fn, (void*)imageBase, (unsigned long long)alloc_size);
        return false;
    }

    const uint64_t off = (uint64_t)(addr - imageBase);
    uint32_t* insn = reinterpret_cast<uint32_t*>(
        stage_base + min_vaddr + off);

    const uint32_t old0 = insn[0];
    const uint32_t old1 = insn[1];

    // MOV W0, WZR; RET — valid zero-return Lua C callback.
    insn[0] = 0x2a1f03e0u;
    insn[1] = 0xd65f03c0u;
    armICacheInvalidate(insn, 8);

    compatLogFmt(
        "FARCRY SCRIPT SETGLOBAL A/B: patched SetGlobalTagHandlerString +0x%llx old=%08x %08x new=%08x %08x",
        (unsigned long long)off, old0, old1, insn[0], insn[1]);
    return true;
}

// A/B experiment for the next crash after disabling the string-tag setter.
// The crash is now reported in CScriptSink::OnSetGlobal +0xa4. The original
// Far Cry implementation only calls CXConsole::RefreshVariable(), so bypass
// this callback to determine whether RefreshVariable/console synchronization
// is the next incompatible host path. This is diagnostic-only.
static bool patchFarCryScriptSinkOnSetGlobal(LoadedSo* so, uint8_t* stage_base,
                                             uint64_t min_vaddr, size_t alloc_size) {
    if (!so || !stage_base || !alloc_size)
        return false;

    const char* path = so->path.c_str();
    const char* base = std::strrchr(path, '/');
    base = base ? base + 1 : path;
    if (std::strcmp(base, "libCrySystem.so") != 0)
        return false;

    constexpr const char* kSym = "_ZN11CScriptSink11OnSetGlobalEPKc";
    void* fn = so->findSym(kSym);
    if (!fn) {
        compatLogFmt("FARCRY SCRIPTSINK A/B: symbol not found: %s", kSym);
        return false;
    }

    const uintptr_t imageBase = (uintptr_t)so->base;
    const uintptr_t addr = (uintptr_t)fn;
    if (addr < imageBase || addr - imageBase >= alloc_size) {
        compatLogFmt(
            "FARCRY SCRIPTSINK A/B: symbol outside image fn=%p base=%p size=0x%llx",
            fn, (void*)imageBase, (unsigned long long)alloc_size);
        return false;
    }

    const uint64_t off = (uint64_t)(addr - imageBase);
    uint32_t* insn = reinterpret_cast<uint32_t*>(
        stage_base + min_vaddr + off);
    const uint32_t old0 = insn[0];
    const uint32_t old1 = insn[1];

    insn[0] = 0xd65f03c0u; // RET
    insn[1] = 0xd503201fu; // NOP (keeps the log/decode stable if inspected)
    armICacheInvalidate(insn, 8);

    compatLogFmt(
        "FARCRY SCRIPTSINK A/B: patched OnSetGlobal +0x%llx old=%08x %08x new=%08x %08x",
        (unsigned long long)off, old0, old1, insn[0], insn[1]);
    return true;
}

// A/B experiment for the next tagged-global crash.
// The string setter is already bypassed, and OnSetGlobal is bypassed. The next
// fault is now in SetGlobalTagHandlerFloat +0x6c, so bypass only the float
// tagged-global setter to identify the next incompatible callback/path.
static bool patchFarCrySetGlobalTagHandlerFloat(LoadedSo* so, uint8_t* stage_base,
                                               uint64_t min_vaddr, size_t alloc_size) {
    if (!so || !stage_base || !alloc_size)
        return false;

    const char* path = so->path.c_str();
    const char* base = std::strrchr(path, '/');
    base = base ? base + 1 : path;
    if (std::strcmp(base, "libCryScriptSystem.so") != 0)
        return false;

    constexpr const char* kSym = "_ZN13CScriptSystem24SetGlobalTagHandlerFloatEP9lua_State";
    void* fn = so->findSym(kSym);
    if (!fn) {
        compatLogFmt("FARCRY SCRIPT FLOAT A/B: symbol not found: %s", kSym);
        return false;
    }

    const uintptr_t imageBase = (uintptr_t)so->base;
    const uintptr_t addr = (uintptr_t)fn;
    if (addr < imageBase || addr - imageBase >= alloc_size) {
        compatLogFmt(
            "FARCRY SCRIPT FLOAT A/B: symbol outside image fn=%p base=%p size=0x%llx",
            fn, (void*)imageBase, (unsigned long long)alloc_size);
        return false;
    }

    const uint64_t off = (uint64_t)(addr - imageBase);
    uint32_t* insn = reinterpret_cast<uint32_t*>(
        stage_base + min_vaddr + off);
    const uint32_t old0 = insn[0];
    const uint32_t old1 = insn[1];
    insn[0] = 0x2a1f03e0u; // MOV W0, WZR
    insn[1] = 0xd65f03c0u; // RET
    armICacheInvalidate(insn, 8);

    compatLogFmt(
        "FARCRY SCRIPT FLOAT A/B: patched SetGlobalTagHandlerFloat +0x%llx old=%08x %08x new=%08x %08x",
        (unsigned long long)off, old0, old1, insn[0], insn[1]);
    return true;
}


// A/B experiment for the next tagged-global crash.
// String and float tagged-global setters are already bypassed. The next fault
// is now in SetGlobalTagHandlerInt +0x6c, so bypass only the integer tagged-global
// setter to identify the next incompatible callback/path.
static bool patchFarCrySetGlobalTagHandlerInt(LoadedSo* so, uint8_t* stage_base,
                                             uint64_t min_vaddr, size_t alloc_size) {
    if (!so || !stage_base || !alloc_size)
        return false;

    const char* path = so->path.c_str();
    const char* base = std::strrchr(path, '/');
    base = base ? base + 1 : path;
    if (std::strcmp(base, "libCryScriptSystem.so") != 0)
        return false;

    constexpr const char* kSym = "_ZN13CScriptSystem22SetGlobalTagHandlerIntEP9lua_State";
    void* fn = so->findSym(kSym);
    if (!fn) {
        compatLogFmt("FARCRY SCRIPT INT A/B: symbol not found: %s", kSym);
        return false;
    }

    const uintptr_t imageBase = (uintptr_t)so->base;
    const uintptr_t addr = (uintptr_t)fn;
    if (addr < imageBase || addr - imageBase >= alloc_size) {
        compatLogFmt(
            "FARCRY SCRIPT INT A/B: symbol outside image fn=%p base=%p size=0x%llx",
            fn, (void*)imageBase, (unsigned long long)alloc_size);
        return false;
    }

    const uint64_t off = (uint64_t)(addr - imageBase);
    uint32_t* insn = reinterpret_cast<uint32_t*>(
        stage_base + min_vaddr + off);
    const uint32_t old0 = insn[0];
    const uint32_t old1 = insn[1];

    insn[0] = 0x2a1f03e0u; // MOV W0, WZR
    insn[1] = 0xd65f03c0u; // RET
    armICacheInvalidate(insn, 8);

    compatLogFmt(
        "FARCRY SCRIPT INT A/B: patched SetGlobalTagHandlerInt +0x%llx old=%08x %08x new=%08x %08x",
        (unsigned long long)off, old0, old1, insn[0], insn[1]);
    return true;
}


// The Android CryEngine source leaves CRefStreamEngine::GetFileSize()'s Linux
// filesystem branch as an unfinished __builtin_trap()/return-0 stub. On Switch
// the PAK data is provided by our virtual archive layer, so patch this whole
// helper entry point to return the real size from that layer. This also removes
// the old BRK/CBNZ experiment that treated a valid virtual PAK fopen as a
// failed file.

/*
 * CRefStreamEngine::GetFileSize() is a C++ member function:
 *   x0 = this
 *   x1 = const char* path
 *   x2/w2 = unsigned flags
 *
 * The previous A/B hook used a helper with the wrong ABI and therefore treated
 * x0 (the CRefStreamEngine object) as the filename. Keep the member ABI intact
 * and forward x1 to the Switch virtual filesystem.
 */
static bool patchFarCryGetFileSize(LoadedSo* so, uint8_t* stage_base,
                                   uint64_t min_vaddr, size_t alloc_size) {
    if (!so || !stage_base || !alloc_size)
        return false;

    const char* base = std::strrchr(so->path.c_str(), '/');
    base = base ? base + 1 : so->path.c_str();
    if (std::strcmp(base, "libCrySystem.so") != 0)
        return false;

    constexpr const char* kSym =
        "_ZN16CRefStreamEngine11GetFileSizeEPKcj";

    void* fn = so->findSym(kSym);
    if (!fn) {
        compatLogFmt("FARCRY GETFILESIZE: symbol not found: %s", kSym);
        return false;
    }

    const uintptr_t imageBase = reinterpret_cast<uintptr_t>(so->base);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(fn);
    if (addr < imageBase || addr - imageBase >= alloc_size) {
        compatLogFmt(
            "FARCRY GETFILESIZE: symbol outside image fn=%p base=%p size=0x%llx",
            fn, reinterpret_cast<void*>(imageBase),
            (unsigned long long)alloc_size);
        return false;
    }

    const uint64_t helper = reinterpret_cast<uint64_t>(&compatGuestGetFileSize);

    // GetFileSize is virtual in IStreamEngine. Patch the CRefStreamEngine
    // vtable entry first so calls through g_GetStreamEngine() land here even
    // when the compiler uses a thunk for the concrete implementation.
    constexpr const char* kVtableSym = "_ZTV16CRefStreamEngine";
    void* vtable = so->findSym(kVtableSym);
    if (vtable) {
        const uintptr_t vtAddr = reinterpret_cast<uintptr_t>(vtable);
        if (vtAddr >= imageBase && vtAddr - imageBase + 0x38 <= alloc_size) {
            // Itanium ABI: symbol starts at offset-to-top/typeinfo;
            // object vptr points at symbol+0x10. In this class the slots are:
            // dtor, deleting dtor, GetStreamCompressionMask, StartRead,
            // GetFileSize => GetFileSize is symbol + 0x30.
            uint64_t* slot = reinterpret_cast<uint64_t*>(
                stage_base + min_vaddr + (vtAddr - imageBase) + 0x30);
            const uint64_t old = *slot;
            *slot = helper;
            armICacheInvalidate(slot, sizeof(*slot));

            compatLogFmt(
                "FARCRY GETFILESIZE VTBL: vtable=%p slot=%p old=%p new=%p",
                vtable, (void*)slot, (void*)old,
                reinterpret_cast<void*>(helper));
            // Do not stop here. The observed BRK is inside the concrete
            // implementation, which means this call path bypasses this vtable
            // slot (or uses a different vtable/thunk). Patch the implementation
            // entry as well so both virtual and direct calls are covered.
        }

        compatLogFmt(
            "FARCRY GETFILESIZE VTBL: vtable outside image vtable=%p base=%p size=0x%llx",
            vtable, reinterpret_cast<void*>(imageBase),
            (unsigned long long)alloc_size);
    } else {
        compatLog("FARCRY GETFILESIZE VTBL: vtable symbol not found; using entry hook");
    }

    // Fallback: patch the implementation entry itself. ABI remains:
    // x0=this, x1=path, w2=flags.
    uint32_t* insn = reinterpret_cast<uint32_t*>(
        stage_base + min_vaddr + (addr - imageBase));

    insn[0] = 0x58000050u; // ldr x16, #+8
    insn[1] = 0xd61f0200u; // br x16
    std::memcpy(&insn[2], &helper, sizeof(helper));
    armICacheInvalidate(insn, 16);

    compatLogFmt(
        "FARCRY GETFILESIZE: patched %s +0x%llx ABI(this,x1=path,w2=flags) -> %p",
        kSym,
        (unsigned long long)(addr - imageBase),
        reinterpret_cast<void*>(helper));
    return true;
}

static bool patchFarCryCallbackTimeQuota(LoadedSo* so, uint8_t* stage_base,
                                           uint64_t min_vaddr, size_t alloc_size) {
    if (!so || !stage_base || !alloc_size)
        return false;

    const char* base = std::strrchr(so->path.c_str(), '/');
    base = base ? base + 1 : so->path.c_str();
    if (std::strcmp(base, "libCrySystem.so") != 0)
        return false;

    constexpr const char* kSym =
        "_ZN16CRefStreamEngine20SetCallbackTimeQuotaEi";

    void* fn = so->findSym(kSym);
    if (!fn) {
        compatLogFmt("FARCRY STREAM QUOTA: symbol not found: %s", kSym);
        return false;
    }

    const uintptr_t imageBase = reinterpret_cast<uintptr_t>(so->base);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(fn);
    if (addr < imageBase || addr - imageBase + 16 > alloc_size) {
        compatLogFmt("FARCRY STREAM QUOTA: symbol outside image fn=%p base=%p size=0x%llx",
                     fn, reinterpret_cast<void*>(imageBase),
                     (unsigned long long)alloc_size);
        return false;
    }

    uint32_t* insn = reinterpret_cast<uint32_t*>(
        stage_base + min_vaddr + (addr - imageBase));
    const uint64_t helper = reinterpret_cast<uint64_t>(
        &compatGuestSetCallbackTimeQuota);

    insn[0] = 0x58000050u; // LDR X16, #+8
    insn[1] = 0xd61f0200u; // BR X16
    std::memcpy(&insn[2], &helper, sizeof(helper));
    armICacheInvalidate(insn, 16);

    compatLogFmt("FARCRY STREAM QUOTA: patched %s +0x%llx -> %p",
                 kSym,
                 (unsigned long long)(addr - imageBase),
                 reinterpret_cast<void*>(helper));
    return true;
}

static bool patchFarCryVirtualPakStreaming(LoadedSo* so, uint8_t* stage_base,
                                           uint64_t min_vaddr, size_t alloc_size) {
    if (!so || !stage_base || !alloc_size)
        return false;

    const char* base = std::strrchr(so->path.c_str(), '/');
    base = base ? base + 1 : so->path.c_str();
    if (std::strcmp(base, "libCrySystem.so") != 0)
        return false;

    void* activate = so->findSym("_ZN14CRefReadStream7ActivateEv");
    void* callRead = so->findSym("_ZN19CRefReadStreamProxy14CallReadFileExEv");
    void* onComplete = so->findSym("_ZN19CRefReadStreamProxy13OnIOCompleteEjj");

    if (!activate || !callRead || !onComplete) {
        compatLogFmt(
            "FARCRY PAK STREAM: symbols missing activate=%p callread=%p oncomplete=%p",
            activate, callRead, onComplete);
        return false;
    }

    const uintptr_t imageBase = reinterpret_cast<uintptr_t>(so->base);
    auto patchEntry = [&](void* fn, void* helper, const char* label,
                          void* originalSlot) -> bool {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(fn);
        if (addr < imageBase || addr - imageBase + 16 > alloc_size)
            return false;

        uint32_t* insn = reinterpret_cast<uint32_t*>(
            stage_base + min_vaddr + (addr - imageBase));

        *reinterpret_cast<void**>(originalSlot) = fn;

        const uint64_t helperAddr = reinterpret_cast<uint64_t>(helper);
        insn[0] = 0x58000050u;
        insn[1] = 0xd61f0200u;
        std::memcpy(&insn[2], &helperAddr, sizeof(helperAddr));
        armICacheInvalidate(insn, 16);

        compatLogFmt("FARCRY PAK STREAM: patched %s +0x%llx -> %p",
                     label,
                     (unsigned long long)(addr - imageBase),
                     helper);
        return true;
    };

    g_near_refstream_on_io_complete = onComplete;

    const bool a = patchEntry(
        activate,
        reinterpret_cast<void*>(&compatGuestActivateReadStream),
        "CRefReadStream::Activate",
        &g_near_original_refstream_activate);

    const bool b = patchEntry(
        callRead,
        reinterpret_cast<void*>(&compatGuestCallReadFileEx),
        "CRefReadStreamProxy::CallReadFileEx",
        &g_near_original_refstream_call_read);

    return a && b;
}


// A/B compatibility fix for the Far Cry multiplayer menu.
// CreateServer.lua calls Game:SetVariable("sv_punkbuster", 0/1), but the
// Android build does not register sv_punkbuster as a CVar.  The original
// CScriptObjectGame::SetVariable() reports that as a Lua error and returns
// through EndFunctionNull(), which aborts the CreateServer path before the
// server can load the selected level.
//
// Keep the engine's normal SetVariable implementation for every registered
// variable.  For the missing-variable error path only, suppress RaiseError
// and leave the existing EndFunctionNull() return intact.  This makes a
// missing optional CVar a no-op instead of a script error without bypassing
// any valid CVar writes.
static bool patchFarCrySetVariableMissingCVar(LoadedSo* so, uint8_t* stage_base,
                                              uint64_t min_vaddr, size_t alloc_size) {
    if (!so || !stage_base || !alloc_size)
        return false;

    const char* path = so->path.c_str();
    const char* base = std::strrchr(path, '/');
    base = base ? base + 1 : path;
    if (std::strcmp(base, "libCryGame.so") != 0)
        return false;

    constexpr const char* kSym =
        "_ZN17CScriptObjectGame11SetVariableEP16IFunctionHandler";

    void* fn = so->findSym(kSym);
    if (!fn) {
        compatLogFmt("FARCRY SETVARIABLE: symbol not found: %s", kSym);
        return false;
    }

    const uintptr_t image_base = reinterpret_cast<uintptr_t>(so->base);
    const uintptr_t fn_addr = reinterpret_cast<uintptr_t>(fn);
    if (fn_addr < image_base || fn_addr - image_base >= alloc_size) {
        compatLogFmt("FARCRY SETVARIABLE: symbol outside image fn=%p base=%p size=0x%llx",
                     fn, reinterpret_cast<void*>(image_base),
                     (unsigned long long)alloc_size);
        return false;
    }

    const uint64_t fn_off = static_cast<uint64_t>(fn_addr - image_base);

    // Do not assume the diagnostic string is close to the function. On this
    // build .rodata can be separated substantially from .text, and the old
    // 0x400-byte window made the patch silently miss the real xref.
    const size_t scan_bytes = std::min<size_t>(0x2000, alloc_size - fn_off);
    uint8_t* code = stage_base + min_vaddr + fn_off;
    uint8_t* image = stage_base + min_vaddr;

    // Exact diagnostic emitted by CScriptObjectGame::SetVariable():
    //   SetVariable invalid variable name "%s": no such variable found
    constexpr char kPrefix[] = "SetVariable invalid variable name";
    constexpr char kSuffix[] = "no such variable found";
    const size_t prefix_len = sizeof(kPrefix) - 1;
    size_t string_off = SIZE_MAX;

    for (size_t i = 0; i + prefix_len + 1 <= alloc_size; ++i) {
        if (std::memcmp(image + i, kPrefix, prefix_len) != 0)
            continue;
        const char* s = reinterpret_cast<const char*>(image + i);
        if (std::strstr(s, kSuffix) != nullptr) {
            string_off = i;
            break;
        }
    }

    if (string_off == SIZE_MAX) {
        compatLog("FARCRY SETVARIABLE: missing-CVar diagnostic string not found");
        return false;
    }

    auto sign_extend = [](uint64_t value, unsigned bits) -> int64_t {
        const uint64_t m = 1ULL << (bits - 1);
        return static_cast<int64_t>((value ^ m) - m);
    };

    auto is_adrp = [](uint32_t w) {
        return (w & 0x9f000000u) == 0x90000000u;
    };

    auto is_add_imm = [](uint32_t w) {
        return (w & 0xffc00000u) == 0x91000000u;
    };

    auto is_adr = [](uint32_t w) {
        return (w & 0x9f000000u) == 0x10000000u;
    };

    auto is_ldr_x_imm = [](uint32_t w) {
        return (w & 0xffc00000u) == 0xf9400000u;
    };

    auto is_bl = [](uint32_t w) {
        return (w & 0xfc000000u) == 0x94000000u;
    };

    const uintptr_t image_addr = reinterpret_cast<uintptr_t>(image);
    const uintptr_t string_addr = image_addr + string_off;

    auto patchNextRaiseError = [&](size_t ref_off, const char* mode) -> bool {
        // The diagnostic address setup is immediately before the error call.
        // Keep the window deliberately local so we cannot accidentally patch an
        // unrelated BL later in SetVariable().
        const size_t end_off = std::min(scan_bytes, ref_off + 0x80);
        for (size_t boff = ref_off + 4; boff + 4 <= end_off; boff += 4) {
            uint32_t* call = reinterpret_cast<uint32_t*>(code + boff);
            if (!is_bl(*call))
                continue;

            const uint32_t old = *call;
            *call = 0xd503201fu; // NOP; EndFunctionNull() remains unchanged.
            armICacheInvalidate(call, 4);

            compatLogFmt(
                "FARCRY SETVARIABLE: suppressed missing-CVar RaiseError at +0x%llx "
                "(SetVariable +0x%llx, ref=%s) old=%08x new=%08x literal=+0x%llx",
                (unsigned long long)(fn_off + boff),
                (unsigned long long)boff,
                mode,
                old, *call,
                (unsigned long long)string_off);
            return true;
        }
        return false;
    };

    // Resolve a nearby literal-address producer. Cover the forms emitted by
    // clang/gcc for PIC AArch64 code:
    //   ADRP + ADD   => address of local .rodata
    //   ADRP + LDR   => pointer through GOT
    //   ADR          => nearby literal
    for (size_t off = 0; off + 8 <= scan_bytes; off += 4) {
        const uint32_t a = *reinterpret_cast<const uint32_t*>(code + off);
        const uint32_t b = *reinterpret_cast<const uint32_t*>(code + off + 4);

        // ADRP; ADD Xn, Xn, #imm
        if (is_adrp(a) && is_add_imm(b)) {
            const unsigned rd = a & 31u;
            const unsigned rn = (b >> 5) & 31u;
            if (rd == rn) {
                const uint64_t imm21 =
                    (((uint64_t)((a >> 5) & 0x7ffffu)) << 2) |
                    ((uint64_t)((a >> 29) & 0x3u));
                const int64_t page_delta = sign_extend(imm21, 21) << 12;
                const uintptr_t pc = reinterpret_cast<uintptr_t>(code + off);
                const uintptr_t page = pc & ~static_cast<uintptr_t>(0xfff);
                const uintptr_t target_page =
                    static_cast<uintptr_t>(static_cast<int64_t>(page) + page_delta);
                const uint64_t imm12 = (b >> 10) & 0xfffu;
                const uintptr_t literal_addr = target_page + imm12;

                if (literal_addr == string_addr &&
                    patchNextRaiseError(off, "ADRP+ADD"))
                    return true;
            }
        }

        // ADRP; LDR Xn, [Xn, #imm] — load a pointer from a GOT slot.
        if (is_adrp(a) && is_ldr_x_imm(b)) {
            const unsigned rd = a & 31u;
            const unsigned rn = (b >> 5) & 31u;
            if (rd == rn) {
                const uint64_t imm21 =
                    (((uint64_t)((a >> 5) & 0x7ffffu)) << 2) |
                    ((uint64_t)((a >> 29) & 0x3u));
                const int64_t page_delta = sign_extend(imm21, 21) << 12;
                const uintptr_t pc = reinterpret_cast<uintptr_t>(code + off);
                const uintptr_t page = pc & ~static_cast<uintptr_t>(0xfff);
                const uintptr_t target_page =
                    static_cast<uintptr_t>(static_cast<int64_t>(page) + page_delta);
                const uint64_t imm12 = (b >> 10) & 0xfffu;
                const uintptr_t got_addr = target_page + (imm12 << 3);

                if (got_addr >= image_addr &&
                    got_addr + sizeof(uint64_t) <= image_addr + alloc_size) {
                    const uint64_t loaded = *reinterpret_cast<const uint64_t*>(got_addr);
                    if (loaded == string_addr &&
                        patchNextRaiseError(off, "ADRP+LDR"))
                        return true;
                }
            }
        }

        // ADR Xn, label — direct PC-relative address of a nearby literal.
        if (is_adr(a)) {
            const uint64_t imm21 =
                (((uint64_t)((a >> 5) & 0x7ffffu)) << 2) |
                ((uint64_t)((a >> 29) & 0x3u));
            const int64_t delta = sign_extend(imm21, 21);
            const uintptr_t pc = reinterpret_cast<uintptr_t>(code + off);
            const uintptr_t literal_addr =
                static_cast<uintptr_t>(static_cast<int64_t>(pc) + delta);

            if (literal_addr == string_addr &&
                patchNextRaiseError(off, "ADR"))
                return true;
        }
    }

    compatLogFmt(
        "FARCRY SETVARIABLE: diagnostic string found at +0x%llx, "
        "but no code reference/RaiseError was located in SetVariable",
        (unsigned long long)string_off);
    return false;
}

// ─── Per-game binary quirk patches ─────────────────────────────────────────────
// The actual fixups live in source/compat/games/ (one file per title), reached
// through compat/games.h, so game-specific patches stay isolated from the shared
// loader and from the Unity path. This helper just pulls the owning package id
// out of the .so path (…/games/<pkg>/lib/<soname>) and forwards.
static void patchKnownGameQuirks(LoadedSo* so, uint8_t* stage_base,
                                 uint64_t min_vaddr, size_t alloc_size,
                                 const char* path) {
    // Diagnostic/experimental handling for the current Far Cry bring-up crash.
    // libCrySystem.so + 0x7d0e0 contains BRK #1. The preceding CBNZ at
    // +0x7d01c jumps directly into it when the helper at +0xff4d0 returns
    // non-zero. Replacing the BRK itself with NOP previously caused execution
    // to fall through inside the error/cleanup block and produced an unrelated
    // heap/stack failure.
    //
    // This experiment keeps the BRK untouched and instead NOPs only that
    // conditional branch, so execution follows the normal fall-through path
    // of GetFileSize. All original instructions are still dumped first.
    if (!path || !stage_base)
        return;

    const char* base = std::strrchr(path, '/');
    base = base ? base + 1 : path;

    if (std::strcmp(base, "libCryScriptSystem.so") == 0) {
        if (!patchFarCrySetGlobalTagHandlerString(so, stage_base, min_vaddr, alloc_size))
            compatLog("FARCRY SCRIPT SETGLOBAL A/B: patch not applied");
        if (!patchFarCrySetGlobalTagHandlerFloat(so, stage_base, min_vaddr, alloc_size))
            compatLog("FARCRY SCRIPT FLOAT A/B: patch not applied");
        if (!patchFarCrySetGlobalTagHandlerInt(so, stage_base, min_vaddr, alloc_size))
            compatLog("FARCRY SCRIPT INT A/B: patch not applied");
        return;
    }

    if (std::strcmp(base, "libCrySystem.so") == 0) {
        if (!patchFarCryDisplayInfoDefault(so, stage_base, min_vaddr, alloc_size))
            compatLog("FARCRY DISPLAYINFO PATCH: not applied");
        if (!patchFarCryScriptSinkOnSetGlobal(so, stage_base, min_vaddr, alloc_size))
            compatLog("FARCRY SCRIPTSINK A/B: patch not applied");
        // Keep the original CSystem::Update() intact: it pumps SDL events
        // and calls CryInput::Update(), which is required for keyboard/mouse
        // input to reach the game.
        compatLog("FARCRY SYSTEM UPDATE A/B: disabled; using original CSystem::Update");

        if (!patchFarCryGetFileSize(so, stage_base, min_vaddr, alloc_size))
            compatLog("FARCRY GETFILESIZE: patch not applied");
        if (!patchFarCryVirtualPakStreaming(so, stage_base, min_vaddr, alloc_size))
            compatLog("FARCRY PAK STREAM: patch not applied");
        if (!patchFarCryCallbackTimeQuota(so, stage_base, min_vaddr, alloc_size))
            compatLog("FARCRY STREAM QUOTA: patch not applied");
        return;
    }

    if (std::strcmp(base, "libCryGame.so") == 0) {
        compatLogFmt("VIDEO PANEL PATCH: scanning libCryGame image min_vaddr=0x%llx size=0x%llx",
                     (unsigned long long)min_vaddr,
                     (unsigned long long)alloc_size);
        if (!patchVideoPanelIsPlaying(so, stage_base, min_vaddr, alloc_size))
            compatLog("VIDEO PANEL PATCH: not applied");

        // Do not rewrite GetPlayerProfilePath() trap/control-flow here.
        // The branch-level workaround was shown to redirect execution into the
        // stack-check failure path (+0xd5f44). Keep the original Android function
        // intact and fix its filesystem ABI at the readdir layer instead.

        // Do not patch the CryInput/CryGame input callbacks. The Android
        // SDL input bridge in runtime.cpp now delivers real Switch HID events
        // to SDL's registered onNativeKeyDown/Up/onNativeMouse callbacks.
        // GetSaveGameList() must return its normal Lua table now that
        // GetPlayerProfilePath() works with the corrected Android dirent ABI.
        // The previous return-0 A/B patch caused "for table must be a table"
        // in scripts/menuscreens/ingame/ingamesingle.lua.

        if (!patchFarCrySetVariableMissingCVar(so, stage_base, min_vaddr, alloc_size))
            compatLog("FARCRY SETVARIABLE: missing-CVar patch not applied");

        // Keep CXGame::LoadConfiguration() intact. The profile path now uses
        // the corrected Android dirent ABI, so bypassing LoadConfiguration()
        // would leave the profile system pinned to the default state.
        compatLog("FARCRY LOADCFG A/B: disabled; using original LoadConfiguration");
        return;
    }

    if (std::strcmp(base, "libCrySystem.so") != 0)
        return;

    constexpr uint64_t kBrkOffset = 0x7d0e0;
    if (kBrkOffset < 0x30 || kBrkOffset + 0x30 > alloc_size) {
        compatLogFmt("CrySystem BRK DIAG: offset 0x%llx outside image size 0x%llx",
                     (unsigned long long)kBrkOffset,
                     (unsigned long long)alloc_size);
        return;
    }

    uint32_t* insn = reinterpret_cast<uint32_t*>(
        stage_base + min_vaddr + kBrkOffset);

    for (int i = -12; i <= 12; ++i) {
        const uint64_t off = kBrkOffset + (int64_t)i * 4;
        const uint32_t word = insn[i];
        compatLogFmt("CrySystem BRK CTX: off=0x%llx word=%08x",
                     (unsigned long long)off, word);

        // AArch64 BL encoding: 0b100101xxxxxxxxxxxxxxxxxxxxxxxxxx.
        if ((word & 0xfc000000u) == 0x94000000u) {
            int32_t imm26 = (int32_t)(word & 0x03ffffffu);
            if (imm26 & 0x02000000)
                imm26 |= (int32_t)0xfc000000;
            const int64_t target = (int64_t)off + ((int64_t)imm26 << 2);
            compatLogFmt("CrySystem BRK BL TARGET: from=0x%llx to=0x%llx",
                         (unsigned long long)off,
                         (unsigned long long)target);
        }
    }

    const uint32_t old = *insn;
    bool patchCbnzToBrk = false;

    // Extra load-time diagnostics for the exact path that reaches BRK #1.
    // The crash LR showed that execution returns to 0x7d01c, where CBNZ X0
    // branches directly to 0x7d0e0. The preceding BL at 0x7d018 returns the
    // value that becomes X0, so identify that helper and its literal argument.
    {
        const uint64_t cbnzOff = 0x7d01c;
        const uint32_t cbnz = *reinterpret_cast<const uint32_t*>(
            stage_base + min_vaddr + cbnzOff);
        if ((cbnz & 0x7e000000u) == 0x34000000u) {
            const bool nonzero = ((cbnz >> 24) & 1u) != 0;
            int32_t imm19 = (int32_t)((cbnz >> 5) & 0x7ffffu);
            if (imm19 & 0x40000)
                imm19 |= (int32_t)0xfff80000;
            const int64_t cbTarget = (int64_t)cbnzOff + ((int64_t)imm19 << 2);
            const unsigned rt = cbnz & 31u;
            compatLogFmt("CrySystem BRK CHECK: off=0x%llx %s X%u -> 0x%llx",
                         (unsigned long long)cbnzOff,
                         nonzero ? "CBNZ" : "CBZ",
                         rt,
                         (unsigned long long)cbTarget);
            if (nonzero && rt == 0 && cbTarget == (int64_t)kBrkOffset)
                patchCbnzToBrk = true;
        }

        const uint64_t callOff = 0x7d018;
        const uint32_t callInsn = *reinterpret_cast<const uint32_t*>(
            stage_base + min_vaddr + callOff);
        if ((callInsn & 0xfc000000u) == 0x94000000u) {
            int32_t imm26 = (int32_t)(callInsn & 0x03ffffffu);
            if (imm26 & 0x02000000)
                imm26 |= (int32_t)0xfc000000;
            const int64_t callTarget = (int64_t)callOff + ((int64_t)imm26 << 2);
            char targetDesc[256] = {};
            if (so)
                elfDescribePc((uint64_t)so->base + (uint64_t)callTarget,
                              targetDesc, sizeof(targetDesc));
            compatLogFmt("CrySystem BRK CHECK: BL from=0x%llx to=0x%llx helper=%s",
                         (unsigned long long)callOff,
                         (unsigned long long)callTarget,
                         targetDesc[0] ? targetDesc : "unknown");
        }

        // Decode the ADRP+ADD pair at 0x7d00c/0x7d014 that forms X1 for the
        // helper call. This is a read-only diagnostic; it never touches code.
        const uint64_t adrpOff = 0x7d00c;
        const uint64_t addOff  = 0x7d014;
        const uint32_t adrp = *reinterpret_cast<const uint32_t*>(
            stage_base + min_vaddr + adrpOff);
        const uint32_t add  = *reinterpret_cast<const uint32_t*>(
            stage_base + min_vaddr + addOff);
        if ((adrp & 0x9f000000u) == 0x90000000u &&
            ((add & 0xffc00000u) == 0x91000000u)) {
            int64_t imm21 = (int64_t)(((adrp >> 5) & 0x7ffffu) << 2) |
                              (int64_t)((adrp >> 29) & 3u);
            if (imm21 & (1ll << 20))
                imm21 -= (1ll << 21);
            const int64_t page = ((int64_t)adrpOff & ~0xfffLL) + (imm21 << 12);
            const uint64_t imm12 = (add >> 10) & 0xfffu;
            const uint64_t litVaddr = (uint64_t)(page + (int64_t)imm12);
            if (litVaddr < alloc_size) {
                char literal[128] = {};
                size_t n = 0;
                const unsigned char* src = stage_base + min_vaddr + litVaddr;
                while (n + 1 < sizeof(literal) && litVaddr + n < alloc_size) {
                    const unsigned char c = src[n];
                    if (c == 0) break;
                    literal[n++] = (c >= 32 && c < 127) ? (char)c : '.';
                }
                literal[n] = '\0';
                compatLogFmt("CrySystem BRK ARG LITERAL: off=0x%llx text=\\\"%s\\\"",
                             (unsigned long long)litVaddr, literal);
            } else {
                compatLogFmt("CrySystem BRK ARG LITERAL: computed off=0x%llx outside image",
                             (unsigned long long)litVaddr);
            }
        }
    }

    // Find direct branches in the preceding 0x400 bytes that target the BRK.
    // This identifies the real error/abort path without executing or modifying
    // the guest instruction.
    const uint64_t scanStart = kBrkOffset > 0x400 ? kBrkOffset - 0x400 : 0;
    for (uint64_t off = scanStart; off < kBrkOffset; off += 4) {
        const uint32_t w = *reinterpret_cast<const uint32_t*>(
            stage_base + min_vaddr + off);
        bool hits = false;
        const char* kind = nullptr;
        int64_t target = 0;

        if ((w & 0x7c000000u) == 0x14000000u ||
            (w & 0xfc000000u) == 0x94000000u) {
            int32_t imm26 = (int32_t)(w & 0x03ffffffu);
            if (imm26 & 0x02000000)
                imm26 |= (int32_t)0xfc000000;
            target = (int64_t)off + ((int64_t)imm26 << 2);
            hits = (target == (int64_t)kBrkOffset);
            kind = ((w & 0x7c000000u) == 0x14000000u) ? "B" : "BL";
        } else if ((w & 0xff000010u) == 0x54000000u) {
            int32_t imm19 = (int32_t)((w >> 5) & 0x7ffffu);
            if (imm19 & 0x40000)
                imm19 |= (int32_t)0xfff80000;
            target = (int64_t)off + ((int64_t)imm19 << 2);
            hits = (target == (int64_t)kBrkOffset);
            kind = "B.cond";
        } else if ((w & 0x7e000000u) == 0x34000000u) {
            int32_t imm19 = (int32_t)((w >> 5) & 0x7ffffu);
            if (imm19 & 0x40000)
                imm19 |= (int32_t)0xfff80000;
            target = (int64_t)off + ((int64_t)imm19 << 2);
            hits = (target == (int64_t)kBrkOffset);
            kind = ((w >> 24) & 1) ? "CBNZ" : "CBZ";
        } else if ((w & 0x7f000000u) == 0x36000000u) {
            int32_t imm14 = (int32_t)((w >> 5) & 0x3fffu);
            if (imm14 & 0x2000)
                imm14 |= (int32_t)0xffffc000;
            target = (int64_t)off + ((int64_t)imm14 << 2);
            hits = (target == (int64_t)kBrkOffset);
            kind = ((w >> 24) & 1) ? "TBNZ" : "TBZ";
        }

        if (hits) {
            compatLogFmt("CrySystem BRK TARGETED BY: from=0x%llx word=%08x kind=%s",
                         (unsigned long long)off, w, kind);
        }
    }

    if (patchCbnzToBrk) {
        constexpr uint64_t kCbnzOffset = 0x7d01c;
        uint32_t* cbnzInsn = reinterpret_cast<uint32_t*>(
            stage_base + min_vaddr + kCbnzOffset);
        const uint32_t cbnzOld = *cbnzInsn;
        *cbnzInsn = 0xd503201fu; // NOP
        compatLogFmt("CrySystem BRK EXPERIMENT: NOP CBNZ at 0x%llx old=%08x new=%08x",
                     (unsigned long long)kCbnzOffset,
                     cbnzOld,
                     *cbnzInsn);
    }

    compatLogFmt("CrySystem BRK DIAG: off=0x%llx word=%08x (BRK left unchanged)",
                 (unsigned long long)kBrkOffset, old);
}

// ─── RELA relocation processing ───────────────────────────────────────────────
// write_base: where to write relocation results (RW mapping)
// exec_base:  address values to store in GOT entries (RX mapping)
// These differ when using JIT dual-mapping; they're equal in the heap fallback.
static void applyRela(LoadedSo* so, const Elf64_Rela* relas, size_t count,
                      uint8_t* write_base, uint8_t* exec_base,
                      uint8_t* write_alloc, size_t alloc_size,
                      uint64_t strsz, const char* tag, ProgressCb cb) {
    for (size_t i = 0; i < count; i++) {
        const Elf64_Rela& r = relas[i];
        uint32_t sym_idx = ELF64_R_SYM(r.r_info);
        uint32_t type    = ELF64_R_TYPE(r.r_info);

        // Push a throttled on-screen update AND trigger a render via cb so
        // the progress screen visibly scrolls instead of looking frozen.
        // Updating the overlay every 4096 relocations is enough to show
        // progress without turning the relocation loop into a UI workload.
        if ((i & 4095) == 0 || i + 1 == count) {
            char ub[64];
            snprintf(ub, sizeof(ub), "%s %zu/%zu", tag, i + 1, count);
            compatUiLog(ub);
            if (cb) cb("Loading ELF library", ub);
        }

        if (r.r_offset < so->min_vaddr) continue;
        uint8_t* target_ptr = write_base + r.r_offset;
        if (target_ptr < write_alloc || target_ptr + 8 > write_alloc + alloc_size) {
            compatLogFmt("%s[%zu/%zu] WARN target 0x%llx out of backing-image bounds — skipped",
                         tag, i + 1, count, (unsigned long long)r.r_offset);
            continue;
        }
        uint64_t* target = (uint64_t*)target_ptr;

        if (type == R_AARCH64_RELATIVE) {
            *target = (uint64_t)exec_base + (uint64_t)r.r_addend;
            continue;
        }

        if (!so->symtab || sym_idx == 0) continue;
        if (sym_idx >= so->sym_count) {
            compatLogFmt("%s[%zu/%zu] WARN sym_idx %u >= sym_count %u — skipped",
                         tag, i + 1, count, sym_idx, so->sym_count);
            continue;
        }

        const Elf64_Sym& sym = so->symtab[sym_idx];
        // .gnu.version can inflate sym_count; guard st_name against strsz.
        const char* sym_name = "";
        if (so->strtab) {
            if (strsz == 0 || sym.st_name < strsz) {
                sym_name = so->strtab + sym.st_name;
            } else {
                compatLogFmt("%s[%zu/%zu] WARN st_name %u >= strsz %llu — name lookup skipped",
                             tag, i + 1, count, sym.st_name, (unsigned long long)strsz);
            }
        }

        uint64_t sym_addr = 0;
        if (isCtypeTraceSym(sym_name)) {
            compatLogFmt("CTYPE RELA: %s type=%u r_offset=0x%llx addend=%lld shndx=%u st_value=0x%llx",
                         sym_name,
                         type,
                         (unsigned long long)r.r_offset,
                         (long long)r.r_addend,
                         (unsigned)sym.st_shndx,
                         (unsigned long long)sym.st_value);
        }
        if (sym.st_shndx != SHN_UNDEF && sym.st_value != 0) {
            // A defined weak symbol in a shared object is still preemptible.
            // NearChuckle's Android CryGame contains weak CS_Stream_* / CS_Update
            // fallback bodies in UIVideoBinkDec.cpp. The real CrySoundSystem
            // provides the strong implementation on Android; on Switch our
            // compatibility shim must be allowed to override that weak local
            // definition as well. Treat only weak-defined symbols as
            // dynamically preemptible here; keep strong local definitions
            // bound exactly as before.
            if (ELF64_ST_BIND(sym.st_info) == STB_WEAK && sym_name[0]) {
                void* resolved = resolveSymbol(sym_name);
                if (resolved) {
                    sym_addr = (uint64_t)resolved;
                    if (isBinkAudioTraceSym(sym_name)) {
                        compatLogFmt("ELF: weak-defined %s preempted -> %p",
                                     sym_name, resolved);
                    }
                } else {
                    sym_addr = (uint64_t)exec_base + sym.st_value;
                }
            } else {
                sym_addr = (uint64_t)exec_base + sym.st_value;
            }
        } else if (sym_name[0]) {
            // ELF weak undefined symbols are intentionally allowed to remain
            // unresolved: the dynamic linker resolves them to the null address.
            // C++ thread_local initialization thunks use exactly this pattern
            // (_ZTH...), and callers test the GOT entry before invoking it.
            // Treating a missing weak symbol as our poison address changes that
            // ABI contract and can turn a harmless NULL check into a crash.
            void* resolved = (void*)resolveSymbol(sym_name);
            if (resolved) {
                sym_addr = (uint64_t)resolved;
            } else if (ELF64_ST_BIND(sym.st_info) == STB_WEAK) {
                // Undefined weak symbols may legally resolve to NULL when no
                // provider exists. Preserve that ELF rule, but first give our
                // Switch compatibility shims a chance to provide symbols such
                // as OpenAL's TLS wrapper and __cxa_thread_atexit_impl.
                compatLogFmt("ELF: weak unresolved -> 0: %s", sym_name);
                sym_addr = 0;
            } else {
                compatLogFmt("ELF: unresolved: %s", sym_name);
                g_unresolved_count++;
                sym_addr = kUnresolvedSymbolPoison;
            }
        }

        switch (type) {
            case R_AARCH64_ABS64:
                *target = sym_addr + (uint64_t)r.r_addend;
                break;
            case R_AARCH64_GLOB_DAT:
                *target = sym_addr + (uint64_t)r.r_addend;
                break;
            case R_AARCH64_JUMP_SLOT:
                *target = sym_addr;
                break;
            case R_AARCH64_COPY:
                if (sym_addr && sym.st_size > 0) {
                    size_t csz = (size_t)sym.st_size;
                    if (csz > 0x10000) {
                        compatLogFmt("%s[%zu/%zu] WARN COPY size %zu capped to 0x10000",
                                     tag, i + 1, count, csz);
                        csz = 0x10000;
                    }
                    memcpy(target, (void*)sym_addr, csz);
                }
                break;
        }
    }
    compatLogFmt("%s: all %zu entries processed", tag, count);
}

// ─── elfLoad ──────────────────────────────────────────────────────────────────
// Does NOT reset g_unresolved_count or g_last_svc_perm_code — caller must call
// elfResetCounts() before loading a batch so counts accumulate correctly.
LoadedSo* elfLoad(const char* path, ProgressCb cb) {
    compatLogFmt("ELF: loading %s", path);

    // Read only the ELF header and program-header table up front. The loader
    // only needs PT_LOAD bytes for the runtime image, so do not malloc/read
    // the entire shared object just to copy a subset of it.
    FILE* f = fopen(path, "rb");
    if (!f) { compatLog("ELF: fopen failed"); return nullptr; }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        compatLog("ELF: fseek end failed");
        return nullptr;
    }
    const long file_end = ftell(f);
    if (file_end < 0) {
        fclose(f);
        compatLog("ELF: ftell failed");
        return nullptr;
    }
    const size_t fsize = (size_t)file_end;
    if (fsize < sizeof(Elf64_Ehdr)) {
        fclose(f);
        compatLog("ELF: file is smaller than ELF header");
        return nullptr;
    }

    rewind(f);
    Elf64_Ehdr ehdr_storage = {};
    if (fread(&ehdr_storage, 1, sizeof(ehdr_storage), f) != sizeof(ehdr_storage)) {
        fclose(f);
        compatLog("ELF: failed to read ELF header");
        return nullptr;
    }
    const Elf64_Ehdr* ehdr = &ehdr_storage;

    // Validate ELF header
    if (memcmp(ehdr->e_ident, "\x7f" "ELF", 4) ||
        ehdr->e_ident[4] != 2 ||
        ehdr->e_ident[5] != 1 ||
        ehdr->e_machine != EM_AARCH64 ||
        ehdr->e_type != ET_DYN) {
        fclose(f);
        compatLog("ELF: not an ARM64 shared lib");
        return nullptr;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0 ||
        ehdr->e_phentsize != sizeof(Elf64_Phdr)) {
        fclose(f);
        compatLog("ELF: invalid or unsupported program-header table");
        return nullptr;
    }

    const uint64_t phdr_bytes =
        (uint64_t)ehdr->e_phnum * (uint64_t)ehdr->e_phentsize;
    if (ehdr->e_phoff > fsize ||
        phdr_bytes > (uint64_t)fsize - (uint64_t)ehdr->e_phoff) {
        fclose(f);
        compatLog("ELF: program-header table is outside file bounds");
        return nullptr;
    }

    std::vector<Elf64_Phdr> phdrs(ehdr->e_phnum);
    if (fseek(f, (long)ehdr->e_phoff, SEEK_SET) != 0 ||
        fread(phdrs.data(), sizeof(Elf64_Phdr), ehdr->e_phnum, f) != ehdr->e_phnum) {
        fclose(f);
        compatLog("ELF: failed to read program headers");
        return nullptr;
    }

    // Walk PT_LOAD segments to find the virtual address span and first data segment
    // Program headers were copied into an owned vector above.
    uint64_t min_vaddr      = UINT64_MAX, max_vaddr = 0;
    uint64_t data_seg_vaddr = UINT64_MAX;  // vaddr of first writable (PF_W) segment
    int      load_count = 0, exec_seg_count = 0, writable_seg_count = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        load_count++;
        if (phdrs[i].p_flags & PF_X) exec_seg_count++;
        if (phdrs[i].p_flags & PF_W) writable_seg_count++;
        if (phdrs[i].p_vaddr < min_vaddr) min_vaddr = phdrs[i].p_vaddr;
        uint64_t end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (end > max_vaddr) max_vaddr = end;
        if ((phdrs[i].p_flags & PF_W) && phdrs[i].p_vaddr < data_seg_vaddr)
            data_seg_vaddr = phdrs[i].p_vaddr;
    }
    if (min_vaddr == UINT64_MAX) {
        if (f) { fclose(f); f = nullptr; }
        compatLog("ELF: no PT_LOAD segments");
        return nullptr;
    }
    // Report unusual ELF layouts because permissions are now assigned per
    // PT_LOAD. Multiple executable/writable segments are valid ELF, but they
    // deserve a visible log entry when encountered.
    if (exec_seg_count > 1 || writable_seg_count > 1) {
        compatLogFmt("ELF: WARN %d PT_LOAD segments (%d exec, %d writable) — "
                     "single-split-point assumption may mis-map permissions",
                     load_count, exec_seg_count, writable_seg_count);
    }

    size_t alloc_size = (size_t)ALIGN_UP(max_vaddr - min_vaddr, 0x1000);

    // Page-aligned location of the first writable segment, retained for
    // symbol lookup. The actual mapping remains one contiguous image.
    uint64_t data_off_pg = 0;
    if (data_seg_vaddr != UINT64_MAX && data_seg_vaddr > min_vaddr)
        data_off_pg = ALIGN_DOWN(data_seg_vaddr - min_vaddr, 0x1000);

    // ── Allocate unified process-code image ──────────────────────────────────
    // One persistent heap-backed ELF image is mapped into the process code
    // region. Unlike the old SplitMap path, this creates no KCodeMemory object
    // per guest library and keeps one contiguous ELF load bias for code/data.
    Handle process_handle = envGetOwnProcessHandle();
    const bool have_map_syscalls =
        envIsSyscallHinted(0x77) && envIsSyscallHinted(0x78) && envIsSyscallHinted(0x73);
    compatLogFmt("ProcessMap: own_handle=0x%08x hints map=%d unmap=%d perm=%d",
                 (unsigned)process_handle,
                 envIsSyscallHinted(0x77) ? 1 : 0,
                 envIsSyscallHinted(0x78) ? 1 : 0,
                 envIsSyscallHinted(0x73) ? 1 : 0);
    if (process_handle == INVALID_HANDLE || !have_map_syscalls) {
        compatLog("ProcessMap: required process-code syscalls are unavailable");
        if (f) { fclose(f); f = nullptr; }
        return nullptr;
    }

    uint8_t* backing = (uint8_t*)memalign(0x1000, alloc_size);
    if (!backing) {
        if (f) { fclose(f); f = nullptr; }
        compatLog("ProcessMap: heap backing allocation failed");
        return nullptr;
    }

    VirtmemReservation* process_code_rv = nullptr;
    virtmemLock();
    void* va = virtmemFindCodeMemory(alloc_size, 0x1000);
    if (va)
        process_code_rv = virtmemAddReservation(va, alloc_size);
    virtmemUnlock();
    if (!process_code_rv) {
        free(backing);
        if (f) { fclose(f); f = nullptr; }
        compatLog("ProcessMap: unable to reserve code-region VA");
        return nullptr;
    }

    uint8_t* code_exec = (uint8_t*)va;
    uint8_t* data_exec = data_off_pg ? code_exec + data_off_pg : nullptr;
    // ── Prepare the unified ELF image directly in the final backing buffer ────
    // The backing allocation remains CPU-writable even after its physical pages
    // are mapped into the process code region. Relocations and game fixups only
    // need a writable host pointer, so there is no reason to maintain a second
    // full-size staging buffer and memcpy the entire image a second time.
    elfHeapCanaryArm();
    uint8_t* stage = backing;
    memset(stage, 0, alloc_size);
    compatLog("ELF: backing image allocated and zeroed");

    // exec_base: used for GOT entries that reference CODE symbols.
    // Data symbols are at data_exec+offset within the same process-code mapping.
    uint8_t* stage_base = stage - min_vaddr;
    uint8_t* exec_base  = code_exec - min_vaddr;

    // ── Read PT_LOAD segments directly into backing image ────────────────────────────
    for (int i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr& ph = phdrs[i];
        if (ph.p_type != PT_LOAD || ph.p_filesz == 0) continue;
        if (ph.p_offset + ph.p_filesz > fsize) {
            compatLogFmt("ELF: seg[%d] WARN offset+filesz exceeds file size — skipped", i);
            continue;
        }
        uint8_t* seg_dst = stage_base + ph.p_vaddr;
        if (seg_dst < stage || seg_dst + ph.p_filesz > stage + alloc_size) {
            compatLogFmt("ELF: seg[%d] WARN dest out of stage bounds — skipped", i);
            continue;
        }
        compatLogFmt("ELF: seg[%d] vaddr=0x%llx filesz=0x%llx memsz=0x%llx flags=0x%x",
                     i, (unsigned long long)ph.p_vaddr, (unsigned long long)ph.p_filesz,
                     (unsigned long long)ph.p_memsz, ph.p_flags);
        if (fseek(f, (long)ph.p_offset, SEEK_SET) != 0) {
            compatLogFmt("ELF: seg[%d] WARN seek to file offset failed — skipped", i);
            continue;
        }
        if (fread(seg_dst, 1, ph.p_filesz, f) != ph.p_filesz) {
            compatLogFmt("ELF: seg[%d] WARN short segment read — skipped", i);
            continue;
        }
    }
    elfHeapCanaryCheck("segment copy");
    compatLog("ELF: PT_LOAD segments copied to backing image");
    if (f) { fclose(f); f = nullptr; }
    {
        uint32_t s0 = *(volatile uint32_t*)stage;
        compatLogFmt("ELF: backing[0]=0x%08x after segs copy", s0);
    }

    // ── Parse PT_DYNAMIC from staging buffer ─────────────────────────────────
    uint64_t strtab_vaddr = 0, symtab_vaddr = 0;
    uint64_t rela_vaddr = 0, rela_sz = 0;
    uint64_t jmprel_vaddr = 0, jmprel_sz = 0;
    uint64_t strsz = 0, syment = sizeof(Elf64_Sym);
    uint64_t init_fn_vaddr = 0;
    uint64_t init_arr_vaddr = 0, init_arr_sz = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_DYNAMIC) continue;
        uint8_t* dyn_ptr = stage_base + phdrs[i].p_vaddr;
        if (dyn_ptr < stage || dyn_ptr >= stage + alloc_size) {
            compatLogFmt("ELF: PT_DYNAMIC out of stage bounds — skipping dynamic parse");
            break;
        }
        const Elf64_Dyn* dyn = (const Elf64_Dyn*)dyn_ptr;
        for (int d = 0; d < 4096 && dyn->d_tag != DT_NULL; dyn++, d++) {
            switch (dyn->d_tag) {
                case DT_STRTAB:      strtab_vaddr   = dyn->d_un.d_ptr; break;
                case DT_SYMTAB:      symtab_vaddr   = dyn->d_un.d_ptr; break;
                case DT_RELA:        rela_vaddr     = dyn->d_un.d_ptr; break;
                case DT_RELASZ:      rela_sz        = dyn->d_un.d_val; break;
                case DT_JMPREL:      jmprel_vaddr   = dyn->d_un.d_ptr; break;
                case DT_PLTRELSZ:    jmprel_sz      = dyn->d_un.d_val; break;
                case DT_STRSZ:       strsz          = dyn->d_un.d_val; break;
                case DT_SYMENT:      syment         = dyn->d_un.d_val; break;
                case DT_INIT:        init_fn_vaddr  = dyn->d_un.d_ptr; break;
                case DT_INIT_ARRAY:  init_arr_vaddr = dyn->d_un.d_ptr; break;
                case DT_INIT_ARRAYSZ:init_arr_sz    = dyn->d_un.d_val; break;
            }
        }
        break;
    }
    compatLogFmt("ELF: dyn: strtab=0x%llx/%llu symtab=0x%llx syment=%llu "
                 "rela=0x%llx/%llu jmprel=0x%llx/%llu init_arr=0x%llx/%llu",
                 (unsigned long long)strtab_vaddr, (unsigned long long)strsz,
                 (unsigned long long)symtab_vaddr, (unsigned long long)syment,
                 (unsigned long long)rela_vaddr,   (unsigned long long)rela_sz,
                 (unsigned long long)jmprel_vaddr, (unsigned long long)jmprel_sz,
                 (unsigned long long)init_arr_vaddr,(unsigned long long)init_arr_sz);

    // ── Build LoadedSo — strtab/symtab point into staging buffer ─────────────
    LoadedSo* so = new LoadedSo();
    so->using_jit   = false;
    so->jit_mem     = {};
    so->alloc       = code_exec;
    so->write_alloc = backing;
    so->data_alloc  = data_exec;
    so->alloc_size  = alloc_size;
    so->min_vaddr   = min_vaddr;
    so->data_vaddr  = data_off_pg ? (min_vaddr + data_off_pg) : 0;
    so->base        = exec_base;
    so->path        = path;

    uint32_t sym_count = 0;
    if (strtab_vaddr) so->strtab = (const char*)(stage_base + strtab_vaddr);
    so->strsz = strsz;
    if (symtab_vaddr && strtab_vaddr && syment) {
        so->symtab = (Elf64_Sym*)(stage_base + symtab_vaddr);
        if (strtab_vaddr > symtab_vaddr)
            sym_count = (uint32_t)((strtab_vaddr - symtab_vaddr) / syment);
        if (sym_count > 200000) sym_count = 200000;
        so->sym_count = sym_count;
    }
    compatLogFmt("ELF: so built sym_count=%u strsz=%llu", sym_count, (unsigned long long)strsz);

    // Registration is deferred until the process-code mapping and permissions
    // are known to have succeeded. This prevents the global export index from
    // retaining pointers to a library whose load later failed.

    // ── Apply relocations to staging buffer ──────────────────────────────────
    // GOT entries store exec-side addresses; the writes go to the heap stage.
    if (rela_vaddr && rela_sz && so->symtab) {
        compatLogFmt("ELF: rela %llu entries", (unsigned long long)(rela_sz / sizeof(Elf64_Rela)));
        applyRela(so, (const Elf64_Rela*)(stage_base + rela_vaddr),
                  rela_sz / sizeof(Elf64_Rela),
                  stage_base, exec_base, stage, alloc_size, strsz, "RELA", cb);
    }
    compatLog("ELF: rela done");
    if (jmprel_vaddr && jmprel_sz && so->symtab) {
        compatLogFmt("ELF: jmprel %llu entries", (unsigned long long)(jmprel_sz / sizeof(Elf64_Rela)));
        applyRela(so, (const Elf64_Rela*)(stage_base + jmprel_vaddr),
                  jmprel_sz / sizeof(Elf64_Rela),
                  stage_base, exec_base, stage, alloc_size, strsz, "JMPREL", cb);
    }
    compatLog("ELF: jmprel done");

    // ── Copy strtab/symtab to heap before staging buffer is freed ────────────
    if (strtab_vaddr && strsz) {
        so->strtab_heap = (char*)malloc(strsz + 1);
        if (so->strtab_heap) {
            memcpy(so->strtab_heap, stage_base + strtab_vaddr, strsz);
            so->strtab_heap[strsz] = '\0';
            so->strtab = so->strtab_heap;
        }
    }
    if (symtab_vaddr && sym_count && syment) {
        size_t symtab_bytes = (size_t)sym_count * sizeof(Elf64_Sym);
        so->symtab_heap = (Elf64_Sym*)malloc(symtab_bytes);
        if (so->symtab_heap) {
            memcpy(so->symtab_heap, stage_base + symtab_vaddr, symtab_bytes);
            so->symtab = so->symtab_heap;
        }
    }
    elfHeapCanaryCheck("strtab/symtab copy");
    compatLog("ELF: strtab/symtab copied");

    // The complete ELF image is contiguous at one load bias, so local data
    // pointers and ADRP targets already refer to their final runtime addresses.
    // Per-game instruction fixups (see patchKnownGameQuirks) — applied to the
    // staged code while it's still writable, before either mapping path below
    // makes it executable. Signature-gated, so a no-op for anything unmatched.
    patchKnownGameQuirks(so, stage_base, min_vaddr, alloc_size, path);

    // ── Map the already-relocated backing image into process code ────────────
    armDCacheFlush(backing, alloc_size);

    virtmemLock();
    virtmemRemoveReservation(process_code_rv);
    process_code_rv = nullptr;
    virtmemUnlock();

    Result map_rc = svcMapProcessCodeMemory(process_handle,
                                             (uint64_t)code_exec,
                                             (uint64_t)backing,
                                             alloc_size);
    if (R_FAILED(map_rc)) {
        compatLogFmt("ProcessMap: svcMapProcessCodeMemory FAILED 0x%08x", (uint32_t)map_rc);
        free(backing);
        if (f) { fclose(f); f = nullptr; }
        g_loaded_sos.pop_back();
        delete so;
        return nullptr;
    }

    bool permissions_ok = true;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr& ph = phdrs[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0)
            continue;

        const uint64_t rel_start  = ph.p_vaddr - min_vaddr;
        const uint64_t rel_end    = rel_start + ph.p_memsz;
        const uint64_t page_start = ALIGN_DOWN(rel_start, 0x1000);
        const uint64_t page_end   = ALIGN_UP(rel_end, 0x1000);
        const uint64_t page_size  = page_end - page_start;
        uint32_t perm = Perm_None;

        if ((ph.p_flags & PF_X) && (ph.p_flags & PF_W)) {
            compatLogFmt("ProcessMap: refusing RWX PT_LOAD[%d] flags=0x%x", i, ph.p_flags);
            permissions_ok = false;
            break;
        }
        if (ph.p_flags & PF_X) perm = Perm_Rx;
        else if (ph.p_flags & PF_W) perm = Perm_Rw;
        else if (ph.p_flags & PF_R) perm = Perm_R;

        Result perm_rc = svcSetProcessMemoryPermission(process_handle,
                                                        (uint64_t)code_exec + page_start,
                                                        page_size,
                                                        perm);
        compatLogFmt("ProcessMap: PT_LOAD[%d] rel=0x%llx size=0x%llx flags=0x%x perm=%s rc=0x%08x",
                     i,
                     (unsigned long long)page_start,
                     (unsigned long long)page_size,
                     ph.p_flags,
                     (perm == Perm_Rx) ? "Rx" :
                     (perm == Perm_Rw) ? "Rw" :
                     (perm == Perm_R)  ? "R" : "None",
                     (uint32_t)perm_rc);
        if (R_FAILED(perm_rc)) {
            permissions_ok = false;
            break;
        }
    }

    if (!permissions_ok) {
        svcUnmapProcessCodeMemory(process_handle,
                                  (uint64_t)code_exec,
                                  (uint64_t)backing,
                                  alloc_size);
        free(backing);
        if (f) { fclose(f); f = nullptr; }
        g_loaded_sos.pop_back();
        delete so;
        return nullptr;
    }

    armICacheInvalidate(code_exec, alloc_size);
    compatLogFmt("ProcessMap: mapped image base=%p backing=%p size=0x%zx",
                 (void*)code_exec, (void*)backing, alloc_size);
    compatLog("ELF: process-code copy complete");

    // Now that the image is fully mapped and executable, publish it to the
    // cross-library resolver and build its export index for following loads.
    g_loaded_sos.push_back(so);
    indexLoadedSoSymbols(so);
    compatLog("ELF: registered");
    compatLogFlush();

    // PT_LOAD permissions above make PF_X pages executable.
    bool code_is_exec = true;

    // ── Store DT_INIT / DT_INIT_ARRAY for deferred constructor run ──────────
    // One contiguous process-code mapping: every ELF vaddr uses one load bias.
    auto vaddr_to_exec = [&](uint64_t vaddr) -> uint8_t* {
        return code_exec + (vaddr - min_vaddr);
    };
    if (init_fn_vaddr && code_is_exec) {
        so->init_fn = (LoadedSo::InitFn)vaddr_to_exec(init_fn_vaddr);
        compatLogFmt("ELF: DT_INIT fn deferred @%p", (void*)so->init_fn);
    }
    if (init_arr_vaddr && init_arr_sz && code_is_exec) {
        so->init_arr       = (LoadedSo::InitFn*)vaddr_to_exec(init_arr_vaddr);
        so->init_arr_count = init_arr_sz / sizeof(LoadedSo::InitFn);
        compatLogFmt("ELF: %zu constructors deferred (arr=%p)",
                     so->init_arr_count, (void*)so->init_arr);
    }

    if (f) { fclose(f); f = nullptr; }
    compatLogFmt("ELF: loaded OK code_exec=%p data_exec=%p sym_count=%u unresolved=%d",
                 (void*)code_exec, (void*)data_exec,
                 so->sym_count, g_unresolved_count);
    return so;
}

// Number of faults recovered from since load began. Read by the watchdog.
int elfGetCtorFaultCount(void) { return g_ctor_faults; }