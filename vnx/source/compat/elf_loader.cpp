#include "compat/loader.h"
#include "compat/games.h"
#include <switch.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <vector>
#include <setjmp.h>
#include <signal.h>
#include <switch/arm/thread_context.h>

// Log helpers — declared before the exception handler so it can use them.
extern void compatLog(const char* msg);
extern void compatLogFmt(const char* fmt, ...);
extern void compatLogFlush();
extern void compatUiLog(const char* msg);
extern void compatUiSetPct(int pct);

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
    // Diagnostic: log first few init_arr values to see if they're populated
    {
        size_t nlog = n < 4 ? n : 4;
        for (size_t di = 0; di < nlog; di++)
            compatLogFmt("ELF: init_arr[%zu]=%p", di, (void*)(uintptr_t)so->init_arr[di]);
        compatLogFlush();
    }
    // Emit a UI update every ~50 ctors and at the end
    const size_t ui_interval = (n > 50) ? (n / 8) : n;

    // Anchor before the first constructor so the walk below covers everything
    // the game allocates.
    shimHeapAnchor();
    bool heap_ok = true;

    for (size_t k = 0; k < n; k++) {
        LoadedSo::InitFn fn = so->init_arr[k];
        if (!fn || fn == (LoadedSo::InitFn)(uintptr_t)-1) { skipped++; continue; }

        compatLogFmt("ELF: ctor[%zu/%zu] @%p", k+1, n, (void*)fn);
        compatLogFlush();
        g_cur_ctor   = (int)(k + 1);
        g_cur_module = soname;
        g_recover_owner = threadGetSelf(); g_in_recover = true; g_recover_sig = 0; g_recover_esr = 0; g_recover_far = 0;
        if (setjmp(g_recover_jmp) == 0) {
            fn();
            g_in_recover = false;
            compatLogFmt("ELF: ctor[%zu/%zu] OK", k + 1, n);
            ok++;
            // Report only the transition. Once the heap is broken it stays
            // broken, and 300 identical complaints would bury the one line
            // that matters — which constructor broke it.
            if (heap_ok) {
                char why[400];
                if (!shimHeapCheck(why, sizeof(why))) {
                    heap_ok = false;
                    compatLogFmt("ELF: *** HEAP CORRUPTED BY ctor[%zu/%zu] @%p — %s",
                                 k + 1, n, (void*)fn, why);
                    compatLogFlush();
                }
            }
        } else {
            g_in_recover = false;
            char sym_buf[160];
            uint64_t fault_vaddr = g_recover_pc - (uint64_t)so->base;
            elfNearestSym(so, fault_vaddr, sym_buf, sizeof(sym_buf));
            // The PC is inside our own newlib (_free_r), so the question that
            // matters is who called it — x30 at fault time answers that, and
            // elfDescribePc resolves it against whichever module it lands in.
            // Without this the caller is unknowable: the shim's free() logged
            // nothing, so either it was bypassed or its checks passed, and the
            // return address is what tells the two apart.
            char lr_buf[192];
            elfDescribePc(g_recover_lr, lr_buf, sizeof(lr_buf));

            // The block being freed. Between-constructor walks come back clean
            // with full coverage, so the damage is made and consumed inside one
            // constructor and only exists at this instant — this is the one
            // moment it can be looked at.
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
            // Once only: if the freed address is the end of the heap, this is
            // exhaustion rather than damage, and every fix aimed at corruption
            // has been aimed at the wrong thing.
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
            // Only the first few: 155 identical faults would bury the log, and
            // they all come from the same place.
            if (g_ctor_faults <= 3) logFaultBacktrace();

            // Dump surrounding instructions to diagnose root cause
            const uint32_t* insn = (const uint32_t*)(uintptr_t)g_recover_pc;
            compatLogFmt("ELF: INSN: [pc-12]=%08x [pc-8]=%08x [pc-4]=%08x [pc]=%08x [pc+4]=%08x",
                         insn[-3], insn[-2], insn[-1], insn[0], insn[1]);
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
        compatLogFmt("ELF: %s: heap walk covered %d chunks, stopped: %s (%s)",
                     soname, steps, stop, heap_ok ? "no corruption seen" : "corruption reported");
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
    char buf[256];
    snprintf(buf, sizeof(buf), "UNRECOVERED FAULT desc=0x%x esr=0x%08x pc=%p far=%p lr=%p sp=%p",
             (unsigned)ctx->error_desc, ctx->esr, (void*)ctx->pc.x, (void*)ctx->far.x,
             (void*)ctx->lr.x, (void*)ctx->sp.x);
    compatLogRaw(buf);
    char where[256];
    elfDescribePc(ctx->pc.x, where, sizeof(where));
    snprintf(buf, sizeof(buf), "UNRECOVERED FAULT at %s", where);
    compatLogRaw(buf);
    elfDescribePc(ctx->lr.x, where, sizeof(where));
    snprintf(buf, sizeof(buf), "UNRECOVERED FAULT lr in %s", where);
    compatLogRaw(buf);
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
           strcmp(n, "_ZdlPv") == 0 || strcmp(n, "_ZdaPv") == 0 ||
           strcmp(n, "_ZdlPvm") == 0 || strcmp(n, "_Znwm") == 0 ||
           strcmp(n, "_Znam") == 0;
}

static void* resolveSymbol(const char* name) {
    if (!name || !name[0]) return nullptr;
    const bool trace = isAllocSym(name);

    // Shim table takes priority — our implementations override any game-library
    // copies of pthread_*, libc functions, GLES, EGL, libandroid, etc.
    void* shim = shimResolve(name);
    if (shim) {
        if (trace) compatLogFmt("bind: %s -> shim %p", name, shim);
        return shim;
    }

    // Then the game's own libraries — a game that ships its own libc (cocos2d-x
    // titles like Hill Climb Racing statically link newlib) MUST keep using it.
    for (LoadedSo* so : g_loaded_sos) {
        void* p = so->findSym(name);
        if (p) {
            if (trace) compatLogFmt("bind: %s -> %s %p", name, so->path.c_str(), p);
            return p;
        }
    }

    // Last: Unity/IL2CPP libc gap fillers. Only reached when neither the shim
    // overrides nor any game library provides the symbol — i.e. exactly the
    // undefined imports of libunity/libil2cpp, never a game's own copies.
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

// ─── ADRP data-segment redirect ───────────────────────────────────────────────
// Scans code_buf (the writable copy of the code segment, size code_size words),
// finds ADRP instructions whose natural runtime target falls in the phantom data
// range [phantom_data_start, phantom_data_end), and rewrites them to land in the
// separate data_jit allocation at data_rx instead.  Both allocations are in the
// same svcMapCodeMemory code-region VA band, so the delta fits in ADRP ±4 GB.
static void patchAdrpToDataJit(uint8_t* code_buf, size_t code_size,
                                uint64_t code_rx,
                                uint64_t phantom_data_start,
                                uint64_t phantom_data_end,
                                uint64_t data_rx) {
    uint32_t* words  = (uint32_t*)code_buf;
    size_t    nwords = code_size / 4;
    int       patched = 0, skipped = 0;
    for (size_t i = 0; i < nwords; i++) {
        uint32_t insn = words[i];
        if ((insn & 0x9F000000u) != 0x90000000u) continue;  // not ADRP

        uint64_t pc      = code_rx + (uint64_t)i * 4;
        uint64_t pc_page = pc & ~0xfffULL;

        int64_t immhi = (int64_t)((insn >> 5) & 0x7ffff);
        int64_t immlo = (int64_t)((insn >> 29) & 3);
        int64_t imm21 = (immhi << 2) | immlo;
        if (imm21 & (1LL << 20)) imm21 -= (1LL << 21);

        uint64_t tgt_page = (uint64_t)((int64_t)pc_page + imm21 * 4096LL);
        if (tgt_page < phantom_data_start || tgt_page >= phantom_data_end) continue;

        uint64_t off_in_data = tgt_page - phantom_data_start;
        uint64_t new_tgt     = data_rx + off_in_data;
        int64_t  new_imm21   = ((int64_t)new_tgt - (int64_t)pc_page) / 4096LL;

        if (new_imm21 < -(1 << 20) || new_imm21 >= (1 << 20)) { ++skipped; continue; }

        uint32_t nlo = (uint32_t)(new_imm21 & 3);
        uint32_t nhi = (uint32_t)((new_imm21 >> 2) & 0x7ffff);
        words[i] = (insn & 0x9F00001Fu) | (nlo << 29) | (nhi << 5);
        ++patched;
    }
    compatLogFmt("ADRP→data_jit: %d patched %d skipped (code_rx=%p data_rx=0x%llx phantom=0x%llx..0x%llx)",
                 patched, skipped, (void*)code_rx,
                 (unsigned long long)data_rx,
                 (unsigned long long)phantom_data_start,
                 (unsigned long long)phantom_data_end);
}

// ─── Per-game binary quirk patches ─────────────────────────────────────────────
// The actual fixups live in source/compat/games/ (one file per title), reached
// through compat/games.h, so game-specific patches stay isolated from the shared
// loader and from the Unity path. This helper just pulls the owning package id
// out of the .so path (…/games/<pkg>/lib/<soname>) and forwards.
static void patchKnownGameQuirks(uint8_t* stage_base, uint64_t min_vaddr,
                                 size_t alloc_size, const char* path) {
    const char* soname = strrchr(path, '/');
    soname = soname ? soname + 1 : path;

    char pkg[128] = {0};
    const char* g = strstr(path, "/games/");
    if (g) {
        g += 7;                              // past "/games/"
        const char* slash = strchr(g, '/');
        size_t n = slash ? (size_t)(slash - g) : strlen(g);
        if (n >= sizeof(pkg)) n = sizeof(pkg) - 1;
        memcpy(pkg, g, n);
    }
    gameApplyQuirks(pkg, soname, stage_base, min_vaddr, alloc_size);
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
        if ((i & 511) == 0 || i + 1 == count) {
            char ub[64];
            snprintf(ub, sizeof(ub), "%s %zu/%zu", tag, i + 1, count);
            compatUiLog(ub);
            if (cb) cb("Loading ELF library", ub);
        }

        if (r.r_offset < so->min_vaddr) continue;
        uint8_t* target_ptr = write_base + r.r_offset;
        if (target_ptr < write_alloc || target_ptr + 8 > write_alloc + alloc_size) {
            compatLogFmt("%s[%zu/%zu] WARN target 0x%llx out of stage bounds — skipped",
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
        if (sym.st_shndx != SHN_UNDEF && sym.st_value != 0) {
            sym_addr = (uint64_t)exec_base + sym.st_value;
        } else if (sym_name[0]) {
            sym_addr = (uint64_t)resolveSymbol(sym_name);
            if (!sym_addr) {
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

    // Read the entire file
    FILE* f = fopen(path, "rb");
    if (!f) { compatLog("ELF: fopen failed"); return nullptr; }

    fseek(f, 0, SEEK_END);
    size_t fsize = (size_t)ftell(f);
    rewind(f);

    uint8_t* file_data = (uint8_t*)malloc(fsize);
    if (!file_data) { fclose(f); compatLog("ELF: OOM"); return nullptr; }
    fread(file_data, 1, fsize, f);
    fclose(f);

    // Validate ELF header
    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)file_data;
    if (fsize < sizeof(Elf64_Ehdr) ||
        memcmp(ehdr->e_ident, "\x7f" "ELF", 4) ||
        ehdr->e_ident[4] != 2 ||           // ELFCLASS64
        ehdr->e_ident[5] != 1 ||           // ELFDATA2LSB
        ehdr->e_machine  != EM_AARCH64 ||
        ehdr->e_type     != ET_DYN) {
        free(file_data);
        compatLog("ELF: not an ARM64 shared lib");
        return nullptr;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        free(file_data);
        compatLog("ELF: no program headers");
        return nullptr;
    }

    // Walk PT_LOAD segments to find the virtual address span and first data segment
    const Elf64_Phdr* phdrs = (const Elf64_Phdr*)(file_data + ehdr->e_phoff);
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
        free(file_data);
        compatLog("ELF: no PT_LOAD segments");
        return nullptr;
    }
    // The SplitMap step below assumes exactly one contiguous executable region
    // followed by exactly one contiguous writable region (a single split point
    // at the first PF_W segment) — true for every .so this project has loaded
    // so far, but not guaranteed by the ELF spec in general. Warn rather than
    // silently mis-map permissions if a future game's linker output ever has
    // more than one segment of either kind, so a weird crash on a new game
    // points straight here instead of being a mystery.
    if (exec_seg_count > 1 || writable_seg_count > 1) {
        compatLogFmt("ELF: WARN %d PT_LOAD segments (%d exec, %d writable) — "
                     "single-split-point assumption may mis-map permissions",
                     load_count, exec_seg_count, writable_seg_count);
    }

    size_t alloc_size = (size_t)ALIGN_UP(max_vaddr - min_vaddr, 0x1000);

    // Page-aligned split between code and data segments.
    // data_off_pg == 0 means no separate data segment (single JIT allocation).
    uint64_t data_off_pg = 0;
    if (data_seg_vaddr != UINT64_MAX && data_seg_vaddr > min_vaddr)
        data_off_pg = ALIGN_DOWN(data_seg_vaddr - min_vaddr, 0x1000);
    size_t code_jit_size = (data_off_pg > 0) ? (size_t)data_off_pg : alloc_size;
    size_t data_jit_size = alloc_size - code_jit_size;

    // ── Allocate memory regions ───────────────────────────────────────────────
    // Primary strategy: split-VA CodeMemory map.
    // svcCreateCodeMemory wraps a heap allocation into a kernel handle whose
    // pages can be placed at any code-region VA via svcControlCodeMemory.
    // Two separate handles (code + data) are placed at adjacent VAs so ADRP
    // instructions in the code segment naturally reach data with no patching.
    // Code VA is promoted to Rx; data VA stays Rw permanently.
    bool     using_split_map = false;
    uint8_t* code_heap_buf   = nullptr;        // heap placeholder for svcCreateCodeMemory
    uint8_t* data_heap_buf   = nullptr;        // heap placeholder for svcCreateCodeMemory
    uint8_t* code_va_base    = nullptr;        // code VA (MapSlave → Rx exec alias)
    uint8_t* data_va_base    = nullptr;        // data VA (MapOwner → Rw, permanent)
    uint8_t* code_write_va   = nullptr;        // temporary MapOwner write alias for code
    Handle   split_h_code    = INVALID_HANDLE;
    Handle   split_h_data    = INVALID_HANDLE;
    VirtmemReservation* split_va_rv         = nullptr;
    VirtmemReservation* split_code_write_rv = nullptr;

    // Reserve adjacent exec+data VAs and a separate write VA for code.
    // Writes go through MapOwner aliases AFTER svcCreateCodeMemory so they
    // reach the physical pages the kernel maps — the heap backing buffers are
    // only placeholders and their content does not matter.
    if (data_off_pg > 0 && data_jit_size > 0) {
        code_heap_buf = (uint8_t*)memalign(0x1000, code_jit_size);
        data_heap_buf = (uint8_t*)memalign(0x1000, data_jit_size);
        if (code_heap_buf && data_heap_buf) {
            virtmemLock();
            void* va = virtmemFindCodeMemory(alloc_size, 0x1000);
            if (va) {
                split_va_rv = virtmemAddReservation(va, alloc_size);
                // Find write VA while exec reservation is held (prevents overlap)
                void* wva = virtmemFindCodeMemory(code_jit_size, 0x1000);
                if (wva) {
                    split_code_write_rv = virtmemAddReservation(wva, code_jit_size);
                    using_split_map = true;
                    code_va_base  = (uint8_t*)va;
                    data_va_base  = code_va_base + code_jit_size;
                    code_write_va = (uint8_t*)wva;
                    compatLogFmt("SplitMap: reserved code_va=%p data_va=%p write_va=%p csz=0x%zx dsz=0x%zx",
                                 code_va_base, data_va_base, code_write_va, code_jit_size, data_jit_size);
                } else {
                    compatLog("SplitMap: no write VA found");
                    virtmemRemoveReservation(split_va_rv);
                    split_va_rv = nullptr;
                }
            } else {
                compatLog("SplitMap: virtmemFindCodeMemory failed");
            }
            virtmemUnlock();
        }
        if (!using_split_map) {
            free(code_heap_buf); code_heap_buf = nullptr;
            free(data_heap_buf); data_heap_buf = nullptr;
        }
    }

    Jit      code_jit = {}, data_jit = {};
    bool     using_jit      = false;
    bool     using_data_jit = false;
    uint8_t* code_write = nullptr;
    uint8_t* code_exec  = nullptr;
    uint8_t* data_write = nullptr;
    uint8_t* data_exec  = nullptr;

    if (using_split_map) {
        code_write = code_heap_buf;
        code_exec  = code_va_base;
        data_write = data_heap_buf;
        data_exec  = data_va_base;
    } else {
        Result jit_rc = jitCreate(&code_jit, code_jit_size);
        if (R_SUCCEEDED(jit_rc)) {
            Result w_rc = jitTransitionToWritable(&code_jit);
            if (R_SUCCEEDED(w_rc)) {
                using_jit  = true;
                code_write = (uint8_t*)code_jit.rw_addr;
                code_exec  = (uint8_t*)code_jit.rx_addr;
                compatLogFmt("JIT: code write=%p exec=%p size=0x%zx",
                             (void*)code_write, (void*)code_exec, code_jit_size);
            } else {
                compatLogFmt("JIT: code jitTransitionToWritable 0x%08X", w_rc);
                jitClose(&code_jit);
            }
        } else {
            compatLogFmt("JIT: code jitCreate 0x%08X — heap fallback", (uint32_t)jit_rc);
        }

        if (!using_jit) {
            code_write = code_exec = (uint8_t*)memalign(0x1000, alloc_size);
            if (!code_write) { free(file_data); compatLog("ELF: memalign failed"); return nullptr; }
        } else if (data_jit_size > 0) {
            Result d_rc = jitCreate(&data_jit, data_jit_size);
            if (R_SUCCEEDED(d_rc)) {
                d_rc = jitTransitionToWritable(&data_jit);
                if (R_SUCCEEDED(d_rc)) {
                    data_write = (uint8_t*)data_jit.rw_addr;
                    data_exec  = (uint8_t*)data_jit.rx_addr;
                    int64_t delta = (int64_t)data_exec - (int64_t)code_exec;
                    if (delta < 0) delta = -delta;
                    if ((uint64_t)delta < (4ULL * 1024 * 1024 * 1024)) {
                        using_data_jit = true;
                        compatLogFmt("JIT: data write=%p exec=%p size=0x%zx (stays Rw, delta=0x%llx)",
                                     (void*)data_write, (void*)data_exec, data_jit_size,
                                     (unsigned long long)delta);
                    } else {
                        compatLogFmt("JIT: data_jit too far (delta=0x%llx) — single-JIT fallback",
                                     (unsigned long long)delta);
                        jitClose(&data_jit);
                        data_write = data_exec = nullptr;
                        jitClose(&code_jit);
                        using_jit = false;
                        Result jit_rc2 = jitCreate(&code_jit, alloc_size);
                        if (R_SUCCEEDED(jit_rc2) && R_SUCCEEDED(jitTransitionToWritable(&code_jit))) {
                            using_jit  = true;
                            code_write = (uint8_t*)code_jit.rw_addr;
                            code_exec  = (uint8_t*)code_jit.rx_addr;
                            code_jit_size = alloc_size;
                            data_jit_size = 0;
                            data_off_pg   = 0;
                            compatLogFmt("JIT: single-JIT write=%p exec=%p size=0x%zx",
                                         (void*)code_write, (void*)code_exec, alloc_size);
                        } else {
                            code_write = code_exec = (uint8_t*)memalign(0x1000, alloc_size);
                            if (!code_write) { free(file_data); compatLog("ELF: memalign failed"); return nullptr; }
                        }
                    }
                } else {
                    compatLogFmt("JIT: data jitTransitionToWritable 0x%08X", (uint32_t)d_rc);
                    jitClose(&data_jit);
                }
            } else {
                compatLogFmt("JIT: data jitCreate 0x%08X — data writes will fault", (uint32_t)d_rc);
            }
        }
    }

    // ── Heap staging buffer ───────────────────────────────────────────────────
    elfHeapCanaryArm();                 // bracket the biggest allocation we make
    // One staging buffer for the whole process, grown as needed and never
    // returned to the allocator.
    //
    // Each module used to malloc its own — 18.9MB for libunity, ~58MB for
    // libil2cpp — zero it, and free it again. Three cycles of tens of
    // megabytes is heavy churn right where the allocator is most fragile, and
    // a freed block of that size coalesces into the top chunk, which is
    // precisely the structure that ends up pointing at zeroed memory. Keeping
    // one buffer removes the churn entirely; the peak footprint is unchanged
    // because it is only ever as large as the biggest module.
    static uint8_t* s_stage      = nullptr;
    static size_t   s_stage_size = 0;
    if (alloc_size > s_stage_size) {
        uint8_t* grown = (uint8_t*)realloc(s_stage, alloc_size);
        if (grown) { s_stage = grown; s_stage_size = alloc_size; }
        else       { s_stage_size = 0; free(s_stage); s_stage = nullptr; }
    }
    uint8_t* stage = s_stage;
    if (!stage) {
        compatLog("ELF: malloc staging buffer OOM");
        if (using_split_map) {
            // svcCreateCodeMemory not yet called — just release VA reservation and free buffers
            virtmemLock();
            if (split_va_rv) { virtmemRemoveReservation(split_va_rv); split_va_rv = nullptr; }
            virtmemUnlock();
            free(code_heap_buf); free(data_heap_buf);
        } else if (using_jit) {
            jitClose(&code_jit); if (using_data_jit) jitClose(&data_jit);
        } else {
            free(code_write);
        }
        free(file_data);
        return nullptr;
    }
    compatLog("ELF: stage alloc OK");
    memset(stage, 0, alloc_size);
    compatLog("ELF: stage zeroed");

    // exec_base: used for GOT entries that reference CODE symbols.
    // Data symbols are at data_exec+offset (handled after relocations via remapping).
    uint8_t* stage_base = stage - min_vaddr;
    uint8_t* exec_base  = code_exec - min_vaddr;

    // ── Copy PT_LOAD segments into staging buffer ────────────────────────────
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
        memcpy(seg_dst, file_data + ph.p_offset, ph.p_filesz);
    }
    elfHeapCanaryCheck("segment copy");
    compatLog("ELF: segs copied to stage");
    {
        uint32_t s0 = *(volatile uint32_t*)stage;
        compatLogFmt("ELF: stage[0]=0x%08x after segs copy", s0);
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
    so->using_jit   = using_jit;
    so->jit_mem     = code_jit;
    so->alloc       = code_exec;
    so->write_alloc = code_write;
    so->data_alloc  = data_exec;   // nullptr if single-jit
    so->alloc_size  = alloc_size;
    so->min_vaddr   = min_vaddr;
    // data_vaddr stores the page-aligned vaddr base of the data_jit allocation,
    // used by findSym to route data-segment symbols to data_exec.
    so->data_vaddr  = using_data_jit ? (min_vaddr + data_off_pg) : 0;
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

    // Register now so cross-library resolution works during relocation
    g_loaded_sos.push_back(so);
    compatLog("ELF: registered");

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
    compatLogFlush();

    // ── Post-relocation: remap phantom data pointers → data_exec ─────────────
    // Relocations above used exec_base = code_exec − min_vaddr, so any
    // R_AARCH64_RELATIVE / local-symbol address that falls in the data segment
    // will have been written as (code_exec + data_vaddr_offset), which is a
    // "phantom" address (code pages, not the real data_jit allocation).
    // Scan the data portion of stage and fix up those 64-bit pointers.
    if (using_data_jit) {
        uint64_t ph_start = (uint64_t)code_exec + data_off_pg;
        uint64_t ph_end   = (uint64_t)code_exec + alloc_size;
        uint64_t* scan    = (uint64_t*)(stage + data_off_pg);
        size_t    nscan   = data_jit_size / 8;
        int       remapped = 0;
        for (size_t i = 0; i < nscan; i++) {
            uint64_t v = scan[i];
            if (v >= ph_start && v < ph_end) {
                scan[i] = (uint64_t)data_exec + (v - ph_start);
                ++remapped;
            }
        }
        compatLogFmt("data-ptr remap: %d entries (phantom=0x%llx..0x%llx → data_rx=%p)",
                     remapped, (unsigned long long)ph_start,
                     (unsigned long long)ph_end, (void*)data_exec);
        compatLogFlush();

        // ── ADRP patch: redirect code→data ADRP instructions ─────────────
        // ADRP instructions in the code segment will compute addresses in the
        // phantom range (code_exec + data_off_pg ...). Patch them to land in
        // data_exec instead, which is the actual Rw data allocation.
        patchAdrpToDataJit(stage, code_jit_size,
                           (uint64_t)code_exec,
                           ph_start, ph_end,
                           (uint64_t)data_exec);
    }

    // Per-game instruction fixups (see patchKnownGameQuirks) — applied to the
    // staged code while it's still writable, before either mapping path below
    // makes it executable. Signature-gated, so a no-op for anything unmatched.
    patchKnownGameQuirks(stage_base, min_vaddr, alloc_size, path);

    // ── Copy stage → final regions then map executable ───────────────────────
    if (using_split_map) {
        // Create handles from the placeholder heap buffers.  Their content is
        // irrelevant — we write the real code/data through MapOwner aliases
        // AFTER the SVCs so the writes actually reach the physical pages the
        // kernel maps.  Writing to the original heap buffer before
        // svcCreateCodeMemory only reaches D-cache; the physical RAM (which
        // the kernel captures) still holds stale bytes.
        Result rc_hc = svcCreateCodeMemory(&split_h_code, code_heap_buf, code_jit_size);
        Result rc_hd = R_SUCCEEDED(rc_hc)
                     ? svcCreateCodeMemory(&split_h_data, data_heap_buf, data_jit_size)
                     : rc_hc;

        // Release all VA reservations — kernel needs these VAs free in its page table
        virtmemLock();
        if (split_va_rv)         { virtmemRemoveReservation(split_va_rv);         split_va_rv         = nullptr; }
        if (split_code_write_rv) { virtmemRemoveReservation(split_code_write_rv); split_code_write_rv = nullptr; }
        virtmemUnlock();

        if (R_SUCCEEDED(rc_hc) && R_SUCCEEDED(rc_hd)) {
            // Map code handle as temporary MapOwner (Rw write alias) at code_write_va
            Result rc_wc = svcControlCodeMemory(split_h_code,
                               CodeMapOperation_MapOwner, code_write_va, code_jit_size, Perm_Rw);
            // Map data handle as MapOwner (Rw) at data_va_base — stays Rw permanently
            Result rc_wd = R_SUCCEEDED(rc_wc)
                         ? svcControlCodeMemory(split_h_data,
                               CodeMapOperation_MapOwner, data_va_base, data_jit_size, Perm_Rw)
                         : rc_wc;

            if (R_SUCCEEDED(rc_wc) && R_SUCCEEDED(rc_wd)) {
                // Write code and data through MapOwner aliases → reaches physical pages
                compatUiLog("Copying code segment...");
                if (cb) cb("Loading ELF library", "Copying code segment");
                memcpy(code_write_va, stage, code_jit_size);
                compatUiLog("Copying data segment...");
                memcpy(data_va_base, stage + data_off_pg, data_jit_size);
                armDCacheFlush(code_write_va, code_jit_size);
                armDCacheFlush(data_va_base, data_jit_size);

                // Probe first 5 JMPREL GOT entries to confirm values reached data_va_base
                if (jmprel_vaddr && jmprel_sz) {
                    const Elf64_Rela* jr = (const Elf64_Rela*)(stage_base + jmprel_vaddr);
                    size_t njr = jmprel_sz / sizeof(Elf64_Rela);
                    for (size_t ji = 0; ji < 5 && ji < njr; ji++) {
                        uint64_t off = jr[ji].r_offset;
                        if (off >= data_off_pg && off + 8 <= data_off_pg + data_jit_size) {
                            uint64_t val = *(const uint64_t*)(data_va_base + (off - data_off_pg));
                            compatLogFmt("GOT[%zu] @va+0x%llx = %p",
                                         ji, (unsigned long long)(off - data_off_pg), (void*)val);
                        }
                    }
                    compatLogFlush();
                }

                // Promote code: remove write alias, add MapSlave (Rx exec alias)
                svcControlCodeMemory(split_h_code,
                    CodeMapOperation_UnmapOwner, code_write_va, code_jit_size, Perm_None);
                Result rc_mc = svcControlCodeMemory(split_h_code,
                                   CodeMapOperation_MapSlave, code_va_base, code_jit_size, Perm_Rx);

                if (R_SUCCEEDED(rc_mc)) {
                    armICacheInvalidate(code_va_base, code_jit_size);
                    compatLogFmt("SplitMap: code_va=%p Rx data_va=%p Rw OK",
                                 (void*)code_va_base, (void*)data_va_base);
                    compatLogFlush();
                } else {
                    compatLogFmt("SplitMap: MapSlave FAILED 0x%08x", (uint32_t)rc_mc);
                    compatLogFlush();
                    svcControlCodeMemory(split_h_data, CodeMapOperation_UnmapOwner,
                                         data_va_base, data_jit_size, Perm_None);
                    svcCloseHandle(split_h_code);
                    svcCloseHandle(split_h_data);
                    free(file_data);
                    g_loaded_sos.pop_back(); delete so;
                    return nullptr;
                }
            } else {
                compatLogFmt("SplitMap: MapOwner FAILED rc_wc=0x%08x rc_wd=0x%08x",
                             (uint32_t)rc_wc, (uint32_t)rc_wd);
                compatLogFlush();
                if (R_SUCCEEDED(rc_wc))
                    svcControlCodeMemory(split_h_code, CodeMapOperation_UnmapOwner,
                                         code_write_va, code_jit_size, Perm_None);
                svcCloseHandle(split_h_code);
                svcCloseHandle(split_h_data);
                free(file_data);
                g_loaded_sos.pop_back(); delete so;
                return nullptr;
            }
        } else {
            compatLogFmt("SplitMap: svcCreateCodeMemory FAILED rc_hc=0x%08x rc_hd=0x%08x",
                         (uint32_t)rc_hc, (uint32_t)rc_hd);
            compatLogFlush();
            if (R_SUCCEEDED(rc_hc)) svcCloseHandle(split_h_code);
            free(file_data);
            g_loaded_sos.pop_back(); delete so;
            return nullptr;
        }
    } else if (using_jit) {
        compatLogFmt("ELF: copy code→JIT write=%p size=0x%zx", (void*)code_write, code_jit_size);
        memcpy(code_write, stage, code_jit_size);
        if (using_data_jit) {
            compatLogFmt("ELF: copy data→JIT write=%p size=0x%zx", (void*)data_write, data_jit_size);
            memcpy(data_write, stage + data_off_pg, data_jit_size);
        }
    }
    compatLog("ELF: JIT copy done (staging buffer retained for the next module)");
    compatLogFlush();

    // ── Make code executable ─────────────────────────────────────────────────
    uint32_t this_svc_perm_code = 0;
    if (using_split_map) {
        // MapSlave(Rx) established above after memcpy — cache flush makes writes visible.
        this_svc_perm_code = 0u;
    } else if (using_jit) {
        Result exec_rc = jitTransitionToExecutable(&code_jit);
        so->jit_mem = code_jit;
        this_svc_perm_code = (uint32_t)exec_rc;
        if (R_FAILED(exec_rc))
            compatLogFmt("JIT: code jitTransitionToExecutable failed 0x%08X", exec_rc);
        else
            compatLogFmt("JIT: code Rx OK%s",
                         using_data_jit ? "; data_jit stays Rw" : "");
    } else {
        this_svc_perm_code = 0xD801;
    }
    if (g_last_svc_perm_code == 0 && this_svc_perm_code != 0)
        g_last_svc_perm_code = this_svc_perm_code;

    // I-cache invalidate for JIT path (split_map does this inside its own block after MapSlave)
    if (using_jit)
        armICacheInvalidate(code_exec, code_jit_size);

    // ── Store DT_INIT / DT_INIT_ARRAY for deferred constructor run ──────────
    // Helper: convert a vaddr to its runtime exec address, accounting for the
    // split between code_exec and data_exec.
    auto vaddr_to_exec = [&](uint64_t vaddr) -> uint8_t* {
        uint64_t rel = vaddr - min_vaddr;
        if (using_data_jit && rel >= data_off_pg)
            return data_exec + (rel - data_off_pg);
        return code_exec + rel;
    };

    bool code_is_exec = (using_split_map || using_jit) && this_svc_perm_code == 0;
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

    free(file_data);
    compatLogFmt("ELF: loaded OK code_exec=%p data_exec=%p sym_count=%u unresolved=%d",
                 (void*)code_exec, (void*)data_exec,
                 so->sym_count, g_unresolved_count);
    return so;
}

// Number of faults recovered from since load began. Read by the watchdog.
int elfGetCtorFaultCount(void) { return g_ctor_faults; }
