/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t: not in stdlib.h off MSVC */
#include <stdlib.h>   /* getenv, exit: the spin verdict below */
#include <string.h>   /* strcmp: RECOMP_ICALL_FATAL */

/* One diagnostic report, one piece.
 *
 * Each report below is a dozen fprintf calls and the CRT takes its lock per
 * call, so with more than one guest thread running the lines interleave in
 * the middle of a report. Dino Crisis 3 runs six workers once it is past its
 * menus, and its log came out like this:
 *
 *   callers: 0x004DB424 <- 0x002705B4 <- 0x002705B4[ICALL] unresolved jump
 *   target 0x41B0B870 -- 1 time(s) (total calls: 6930990)
 *
 * -- two threads' reports spliced together, with a caller list that stops
 * mid-sentence and an address that belongs to neither line as read. These
 * diagnostics exist to be read during exactly the multi-threaded bring-up
 * that breaks them.
 *
 * The CRT exposes the lock these calls were already taking one at a time, so
 * hold it across the whole report instead. */
#if defined(_MSC_VER)
#  define RECOMP_DIAG_LOCK()   _lock_file(stderr)
#  define RECOMP_DIAG_UNLOCK() _unlock_file(stderr)
#elif defined(__unix__) || defined(__APPLE__)
#  define RECOMP_DIAG_LOCK()   flockfile(stderr)
#  define RECOMP_DIAG_UNLOCK() funlockfile(stderr)
#else
#  define RECOMP_DIAG_LOCK()   ((void)0)
#  define RECOMP_DIAG_UNLOCK() ((void)0)
#endif

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

/* The guest registers (g_eax, g_esp, g_icall_saved_esp, ...) are fields of
 * one thread-local struct, declared once in recomp_cpu.h, which the lifter
 * copies into gen/ beside the code it describes. Take them from there; never
 * redeclare one here. They used to be redeclared by hand, and a declaration
 * without the thread-local storage class linked and silently read other
 * storage, so every read got 0 -- which is why the log below once reported
 * no call site: not because the guest esp was stale, but because this file
 * was not reading the guest esp at all. The names are macros now, so a
 * redeclaration no longer compiles. */
#include "recomp/gen/recomp_cpu.h"

/* g_eax: recomp_cpu.h. */
/* The guest stack pointer. At the moment an indirect call is refused, the
 * caller has already pushed its guest return address, so the top of the
 * guest stack is the call site -- the one thing the old log did not say. */
/* g_esp: recomp_cpu.h. */
extern ptrdiff_t g_xbox_mem_offset;
/* The lifted code sections, so a value on the guest stack can be told
 * apart from data when naming the callers of a refused call. */
extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;
/* The esp the dispatch macro captured. Not g_esp, which is stale by the time
 * a refused call is reported. Zero when the title's generated header predates
 * this, and the log then says it has no callers rather than inventing them. */
/* g_icall_saved_esp: recomp_cpu.h. */
/* Which dispatch form was refused: 0 unknown, 1 call, 2 jump. Unknown
 * means this title was lifted before the macros published it. */
/* g_icall_dispatch_form: recomp_cpu.h. */

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    /*
     * Rate-limited per target, reporting at 1, 10, 100 ...
     *
     * An unresolved target inside a loop is the normal case, not the rare
     * one: Mortal Kombat: Deadly Alliance produced 46843 of these for a
     * single address in one 30-second run, seventeen lines each. That buries
     * every other diagnostic in the log and says nothing the first report did
     * not. The progression is the useful part -- one line says a target was
     * never identified, a run of them says the title is stuck on it.
     */
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }

    RECOMP_DIAG_LOCK();
    fprintf(stderr, "[ICALL] unresolved %starget 0x%08X -- %llu time(s) "
                    "(total calls: %llu)\n",
            g_icall_dispatch_form == 1 ? "call " :
            g_icall_dispatch_form == 2 ? "jump " : "",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    if (g_icall_dispatch_form == 2 && hits[i] == 1)
        fprintf(stderr, "  a jump, not a call: if this address is inside a "
                        "function rather than at its start, the guest is doing "
                        "its own control flow (a coroutine, a longjmp, or a "
                        "switch arm) and seeding it as a function will not "
                        "help\n");

    /*
     * Who called it. The lifted caller pushed its return address on the guest
     * stack, so code-range values above esp name the chain -- the same scan
     * the memory watchpoints use. It is a heuristic: stale return addresses
     * from earlier frames show up too, and the first entry is the reliable
     * one. It is still the difference between an address with no context and
     * a function to open, which is what an unresolved target needs.
     */
    {
        const uint8_t *stack = (const uint8_t *)g_xbox_mem_offset
                             + g_icall_saved_esp;
        int shown = 0;
        fprintf(stderr, "  callers:");
        for (i = 0; g_xbox_mem_offset && g_icall_saved_esp
                    && i < 160 && shown < 6; i++) {
            uint32_t v;
            memcpy(&v, stack + (size_t)i * 4, sizeof v);
            if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "%s 0x%08X", shown ? " <-" : "", v);
                shown++;
            }
        }
        if (!shown)
            fprintf(stderr, " (none on the stack)");
        fprintf(stderr, "\n");
    }

    /* The recent-target ring buffer, once per address. What ran just before
     * an unresolved call is context for the first report and noise after. */
    if (hits[i] == 1) {
        fprintf(stderr, "  Recent ICALL targets:\n");
        for (i = 0; i < 16; i++) {
            int idx = (g_icall_trace_idx - 16 + i) & 15;
            if (g_icall_trace[idx])
                fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
        }
    }
    RECOMP_DIAG_UNLOCK();
    fflush(stderr);
}
/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va, uint32_t saved_esp)
{
    /* A power of ten, because the rate limiter above only reaches this
     * function body at 1, 10, 100 ... and the verdict has to land on one of
     * those. A hundred thousand skips of one target is unambiguous: no title
     * makes progress through that. */
    enum { SLOTS = 16, SPIN_VERDICT = 100000 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    static int said_it;   /* the verdict below is said once, not once per target */
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    {
        /* The call site, read off the guest stack. Without it the log
         * says a wild pointer was skipped but not by whom, and the
         * caller is the only thing that leads anywhere.
         *
         * It comes from the esp the dispatch macro captured, not from
         * g_esp. The lifted call site pushes its return address onto the
         * *local* esp and only syncs g_esp at certain points, so g_esp is
         * stale here -- it read 0 exactly when a null target most needed
         * explaining, which sent three rounds of Jet Set Radio Future
         * chasing inferences instead of a call site. saved_esp is the value
         * before that push, so the return address is the dword below it.
         *
         * Only for the call form. A tail jump pushes no return address, so
         * that dword is whatever the frame happened to leave there: Tony
         * Hawk's Pro Skater 2X reported "from 0x00000008" and then "from
         * 0x00000004" for the same million skipped calls, and both are
         * plainly not code. Printing a number that cannot be an address as
         * though it were the caller is worse than printing nothing, because
         * it is the one field a reader goes to next.
         *
         * So: name the form, and where the top of the stack cannot answer,
         * scan for code-range values the way the unresolved-target logger
         * above does. Stale return addresses from earlier frames come back
         * too and the first is the reliable one, but a list of real code
         * addresses is a place to start and 0x00000008 is not. */
        uint32_t caller = 0;
        if (g_icall_dispatch_form != 2 && saved_esp >= 4 && g_xbox_mem_offset)
            caller = *(const uint32_t *)((const uint8_t *)g_xbox_mem_offset
                                         + (saved_esp - 4));
        if (caller < g_xbox_code_lo || caller >= g_xbox_code_hi)
            caller = 0;
        RECOMP_DIAG_LOCK();
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped "
                        "%llu time(s) via a %s",
                va, (unsigned long long)hits[i],
                g_icall_dispatch_form == 2 ? "jump" : "call");
        if (caller)
            fprintf(stderr, " from 0x%08X", caller);
        fprintf(stderr, " (null or wild function pointer, at call #%llu)\n",
                (unsigned long long)g_icall_count);

        /* The stack scan, once per address: the same heuristic the
         * unresolved-target logger uses, and the only thing that names a
         * caller when the return address is not where it should be. */
        if (hits[i] == 1) {
            const uint8_t *stack = (const uint8_t *)g_xbox_mem_offset
                                 + saved_esp;
            int shown = 0, k;
            fprintf(stderr, "  callers:");
            for (k = 0; g_xbox_mem_offset && saved_esp
                        && k < 160 && shown < 6; k++) {
                uint32_t v;
                memcpy(&v, stack + (size_t)k * 4, sizeof v);
                if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                    fprintf(stderr, "%s 0x%08X", shown ? " <-" : "", v);
                    shown++;
                }
            }
            if (!shown)
                fprintf(stderr, " (none on the stack)");
            fprintf(stderr, "\n");
        }

        /* RECOMP_ICALL_FATAL=1: fault here instead of skipping, so the crash
         * handler prints the guest call stack that reached this call.
         *
         * The caller printed above is read from the top of the guest stack,
         * which is right only when the lifted call site pushed a return
         * address there and nothing has since moved esp. When it reads 0 --
         * exactly the case where a null target most needs explaining -- the
         * line says a wild pointer was skipped and nothing about by whom. A
         * deliberate fault costs the run and buys the backtrace. */
        {
            static int fatal = -1;
            if (fatal < 0) {
                const char *v = getenv("RECOMP_ICALL_FATAL");
                fatal = v && *v && strcmp(v, "0") != 0;
            }
            if (fatal) {
                fprintf(stderr, "[ICALL] RECOMP_ICALL_FATAL: faulting here for "
                                "a backtrace\n");
                fflush(stderr);
                *(volatile int *)0 = 1;
            }
        }
    }

    /* Past a certain count this stops being a warning and becomes a verdict.
     *
     * A skipped call sets eax to 0 and returns. Zero is a fine answer for a
     * null function pointer in most code -- it reads as NULL, false, or
     * nothing -- but it is also S_OK, so in COM-shaped code a loop that
     * repeats while the result is non-negative can never leave. Panzer
     * Dragoon Orta hung exactly there, three billion skips in forty seconds,
     * with no frame ever presented.
     *
     * The value is deliberately left alone: there is no return value that is
     * right for both conventions, and guessing failure would break the
     * callers for which zero is correct. What can be fixed is the silence.
     * Say plainly that the title is hung, once, and say what to do about it.
     */
    if (hits[i] == SPIN_VERDICT && !said_it) {
        said_it = 1;
        fprintf(stderr,
            "[ICALL] ^^ this is a hang, not slow progress. One target has been\n"
            "        skipped %d times. The skip returns eax = 0, which is also\n"
            "        S_OK, so a loop testing for a non-negative result will\n"
            "        never exit. Try `py -3 -m tools.seed_from_log <log>` to\n"
            "        recover the target as a function, and read\n"
            "        docs/technical/memory-watchpoints.md for who wrote the\n"
            "        pointer. Set RECOMP_ICALL_SPIN_FATAL=1 to stop here\n"
            "        instead of spinning.\n", SPIN_VERDICT);
        fflush(stderr);
        if (getenv("RECOMP_ICALL_SPIN_FATAL")) {
            fprintf(stderr, "[ICALL] RECOMP_ICALL_SPIN_FATAL is set; exiting.\n");
            fflush(stderr);
            exit(3);
        }
    }
    RECOMP_DIAG_UNLOCK();
    fflush(stderr);
}


/* ── Gladius: the streaming threads wait for their creator ─── */

/*
 * sub_001F0510 and sub_00203090 each start a worker (sub_001F0470,
 * sub_00202FF0) and only then create its event, open D:\ and set the byte at
 * +0x0C that says the object is ready. The worker's first act is to return -1
 * if that byte is still 0. On the console the creator is not preempted in
 * between: the new thread has the creator's priority and none of those calls
 * waits (D:\ is the volume itself, no disc read). Here the open of D:\ is a
 * blocking call that hands the guest lock to the new thread, and on Windows
 * the two run in parallel, so both workers exited at once and the title sat
 * on its first loading frame for good, polling them.
 *
 * Each worker now waits, at most two seconds, for the byte before running the
 * title's own body. Both are stdcall(object), so the object is at [esp+4].
 */
void xbox_GuestSleep(uint32_t ms);       /* DWORD: unsigned long on Windows, the same 32 bits */
extern void sub_001F0470_gen(void);
extern void sub_00202FF0_gen(void);

#define GLAD_MEM8(va) (*(volatile uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define GLAD_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

static void gladius_wait_ready(const char *who)
{
    uint32_t obj = GLAD_MEM32(g_esp + 4);
    int ms = 0;

    if (!obj)
        return;
    while (GLAD_MEM8(obj + 0x0C) == 0 && ms < 2000) {
        xbox_GuestSleep(1);
        ms++;
    }
    if (ms) {
        fprintf(stderr, "[GLADIUS] %s: waited %d ms for its creator to mark object "
                        "0x%08X ready%s\n", who, ms, obj,
                GLAD_MEM8(obj + 0x0C) ? "" : " (gave up: the title's own exit follows)");
        fflush(stderr);
    }
}

void sub_001F0470(void)
{
    gladius_wait_ready("sub_001F0470");
    sub_001F0470_gen();
}

void sub_00202FF0(void)
{
    gladius_wait_ready("sub_00202FF0");
    sub_00202FF0_gen();
}

/* ── Gladius: widescreen through the engine's own aspect ───── */

/*
 * sub_001BBD60(width, height) runs once at start-up with the device's
 * 640x480. From height / width it sets the 2D layer's orthographic matrix
 * (0x004AA988, through sub_001AEFB0) and the x and y factors the 3D
 * projection takes (0x004C0AC4 / 0x004C0AC0, read by sub_0019E690): for a
 * screen wider than 4:3 it widens the view horizontally and keeps the 2D at
 * 4:3 proportions -- Hor+, in the engine. The Xbox build never hands it
 * anything but 640x480, and asks D3D for a 4:3 mode whatever the console
 * says (sub_001E0E40 pushes 0 for the widescreen argument of its mode pick).
 *
 * With widescreen on, it is called with the height a 16:9 screen of this
 * width would have, so the engine renders the anamorphic frame a 16:9 Xbox
 * title renders; the screen size it keeps (height 0x004C0AC8, width 0x004C0ACC,
 * read in 58 places)
 * and the half-pixel offset it derives from it go back to the real ones.
 *
 * With that the 3D fills the 16:9 picture. The 2D is the renderer's to
 * place: the menus are drawn for 4:3 and stay at 4:3, centred, with their
 * backdrops built from several textured pieces -- so only whole-screen
 * flat-colour passes (the fades and the cutscenes' letterbox bars) may span
 * the picture, which is the renderer's centre policy (widescreen_2d =
 * centre, which the project declares: CENTRE_2D in CMakeLists.txt). The 3D is told apart from the 2D by the
 * projection register, c176 (recomp_title_widescreen in CMakeLists.txt).
 * RECOMP_GLADIUS_WIDE=0 leaves the engine at 4:3.
 */
extern void sub_001BBD60_gen(void);
int xbox_EnvSwitch(const char *name, int default_on);
int recomp_config_bool(const char *env, const char *key, int fallback);

void sub_001BBD60(void)
{
    uint32_t w = GLAD_MEM32(g_esp + 4), h = GLAD_MEM32(g_esp + 8), h_eff;
    static int on = -1;

    if (on < 0)
        on = xbox_EnvSwitch("RECOMP_GLADIUS_WIDE", 1) &&
             recomp_config_bool("RECOMP_WIDESCREEN", "widescreen", 0);
    if (!on || !w || !h) {
        sub_001BBD60_gen();
        return;
    }
    h_eff = (w * 9u + 8u) / 16u;
    GLAD_MEM32(g_esp + 8) = h_eff;
    sub_001BBD60_gen();
    GLAD_MEM32(g_esp + 8) = h;
    GLAD_MEM32(0x004C0AC8u) = h;      /* height; 0x004C0ACC is the width */
    {
        /* It took 0.5 / height off the ortho's y translation; take the real one. */
        float *ty = (float *)((uintptr_t)0x004AA9BCu + g_xbox_mem_offset);
        *ty += 0.5f / (float)h_eff - 0.5f / (float)h;
    }
    fprintf(stderr, "[GLADIUS] widescreen: the engine's aspect from %ux%u, as a 16:9 "
                    "screen of %u pixels' width; frame stays %ux%u\n", w, h_eff, w, w, h);
    fflush(stderr);
}

/* ── Gladius: widescreen for the 3D drawn into the 2D layout ── */

/*
 * A camera draws into a rectangle of the 640x480 screen of its own
 * (+0x25C x, +0x260 y, +0x264 width, +0x268 height): sub_0019E8E0 hands it
 * to the engine's viewport, sub_001AF350, which turns it into the pixel
 * scale and offset its vertex programs apply -- D3D's own SetViewport is
 * never called with it -- and to the scissor, sub_001AF990. The game's
 * camera fills the screen. The menus' cameras do not: Choose Your Destiny
 * draws each gladiator in a camera the size of its portrait, at the
 * portrait's place.
 *
 * In widescreen those draws are 3D (they project through c176), so the
 * renderer leaves them across the whole 16:9 picture while the portraits
 * round them, being 2D, are kept at 4:3 in the middle (CENTRE_2D): each
 * model came out 4/3 as far from the middle as its portrait, and spilled
 * out of it, as its scissor was 4/3 as wide as the frame drawn round it.
 *
 * So, in widescreen, a camera's viewport is moved as the 2D round it is:
 * its middle drawn in by 0.75 towards the screen's, its width kept, since
 * the model is already the right size through the widened projection; and
 * its scissor goes to the same middle at 0.75 of the width, the portrait's
 * width on screen. A camera that fills the screen is left where it is.
 * Only the cameras' rectangles are changed: the engine's other viewports
 * and scissors are not cameras'. A camera's own methods set them
 * (sub_0019E8E0, and the scissor alone in sub_0019D900 and sub_0019DA70:
 * g_camera_rect), and so does the menu widget drawing the model,
 * sub_00119B40, which works the same rectangle out for itself and sets the
 * scissor last, just before the model is queued (the engine's queue keeps
 * each draw's scissor, sub_0020CF40) -- so a scissor set to a rectangle a
 * camera has just drawn into is a camera's too, whoever sets it
 * (g_camera_rects). Left at the full width, the model was painted over its
 * portrait's border. RECOMP_GLADIUS_PORTRAITS=0 leaves them as the engine
 * set them.
 */
extern void sub_0019E8E0_gen(void);
extern void sub_0019D900_gen(void);
extern void sub_0019DA70_gen(void);
extern void sub_001AF350_gen(void);
extern void sub_001AF990_gen(void);

static int g_camera_rect;               /* inside a camera's own methods */

/* The rectangles cameras have drawn into lately, as their viewport calls
 * gave them: a scissor set to one of them is a camera's, whoever sets it. */
static struct { int32_t x, y, w, h; } g_camera_rects[8];
static unsigned g_camera_rects_next;

static void camera_rect_note(int32_t x, int32_t y, int32_t w, int32_t h)
{
    unsigned i;

    for (i = 0; i < 8; i++)
        if (g_camera_rects[i].x == x && g_camera_rects[i].y == y &&
            g_camera_rects[i].w == w && g_camera_rects[i].h == h)
            return;
    i = g_camera_rects_next++ % 8;
    g_camera_rects[i].x = x;
    g_camera_rects[i].y = y;
    g_camera_rects[i].w = w;
    g_camera_rects[i].h = h;
}

static int camera_rect_known(int32_t x, int32_t y, int32_t w, int32_t h)
{
    unsigned i;

    for (i = 0; i < 8; i++)
        if (g_camera_rects[i].w && g_camera_rects[i].x == x && g_camera_rects[i].y == y &&
            g_camera_rects[i].w == w && g_camera_rects[i].h == h)
            return 1;
    return 0;
}

static int gladius_portraits_on(void)
{
    static int on = -1;

    if (on < 0)
        on = xbox_EnvSwitch("RECOMP_GLADIUS_WIDE", 1) &&
             xbox_EnvSwitch("RECOMP_GLADIUS_PORTRAITS", 1) &&
             recomp_config_bool("RECOMP_WIDESCREEN", "widescreen", 0);
    return on;
}

void sub_0019E8E0(void)
{
    g_camera_rect++;
    sub_0019E8E0_gen();
    g_camera_rect--;
}

void sub_0019D900(void)
{
    g_camera_rect++;
    sub_0019D900_gen();
    g_camera_rect--;
}

void sub_0019DA70(void)
{
    g_camera_rect++;
    sub_0019DA70_gen();
    g_camera_rect--;
}

/* cdecl (x, y, width, height) for both; the caller's own pushed copies, so
 * changed for the call and put back after it. */
static int camera_rect_args(int32_t *x, int32_t *w)
{
    int32_t screen_w = (int32_t)GLAD_MEM32(0x004C0ACCu);

    *x = (int32_t)GLAD_MEM32(g_esp + 4);
    *w = (int32_t)GLAD_MEM32(g_esp + 12);
    return g_camera_rect && gladius_portraits_on() && *w > 0 &&
           (*x != 0 || *w != screen_w);
}

/* Where the 2D squeeze puts a point at x: drawn in by 0.75 towards the
 * screen's middle. */
static int32_t squeeze_x(int32_t x)
{
    int32_t mid = (int32_t)GLAD_MEM32(0x004C0ACCu) / 2;

    return mid + (int32_t)((x - mid) * 3 / 4);
}

void sub_001AF350(void)
{
    int32_t x, w;

    if (!camera_rect_args(&x, &w)) {
        sub_001AF350_gen();
        return;
    }
    camera_rect_note(x, (int32_t)GLAD_MEM32(g_esp + 8), w, (int32_t)GLAD_MEM32(g_esp + 16));
    {
        static int told;
        int32_t to = squeeze_x(x + w / 2) - w / 2;

        if (told < 4 && GLAD_MEM32(0x003775BCu) != (uint32_t)to) {
            told++;
            fprintf(stderr, "[GLADIUS] widescreen: a camera's %dx%d view at x=%d is "
                            "drawn at x=%d, with the 2D round it\n",
                    w, (int32_t)GLAD_MEM32(g_esp + 16), x, to);
            fflush(stderr);
        }
        GLAD_MEM32(g_esp + 4) = (uint32_t)to;
    }
    sub_001AF350_gen();
    GLAD_MEM32(g_esp + 4) = (uint32_t)x;
}

void sub_001AF990(void)
{
    int32_t x, w, w2;

    if (!camera_rect_args(&x, &w)) {
        int32_t y = (int32_t)GLAD_MEM32(g_esp + 8), h = (int32_t)GLAD_MEM32(g_esp + 16);

        if (!gladius_portraits_on() || !camera_rect_known(x, y, w, h)) {
            sub_001AF990_gen();
            return;
        }
    }
    w2 = w * 3 / 4;
    GLAD_MEM32(g_esp + 4) = (uint32_t)(squeeze_x(x + w / 2) - w2 / 2);
    GLAD_MEM32(g_esp + 12) = (uint32_t)w2;
    sub_001AF990_gen();
    GLAD_MEM32(g_esp + 4) = (uint32_t)x;
    GLAD_MEM32(g_esp + 12) = (uint32_t)w;
}

/* RECOMP_GLADIUS_CLOCK=1: every five seconds, how far the game's own clock
 * (the float sub_001FC860 adds each frame's time to, 0x00502468) moved
 * against the wall clock and the frames presented. A clock-driven game moves
 * it one second a second at any frame rate. */
extern void sub_001FC860_gen(void);
uint64_t xbox_ReadTimeStampCounter(void);

void sub_001FC860(void)
{
    static int on = -1;
    static uint64_t t0;
    static float g0;
    static unsigned long frames;

    sub_001FC860_gen();
    if (on < 0)
        on = xbox_EnvSwitch("RECOMP_GLADIUS_CLOCK", 0);
    if (!on)
        return;
    {
        uint64_t now = xbox_ReadTimeStampCounter();
        float g = *(volatile float *)((uintptr_t)0x00502468u + g_xbox_mem_offset);
        frames++;
        if (!t0) {
            t0 = now;
            g0 = g;
            frames = 0;
        } else if (now - t0 >= 5ull * 733333333ull) {
            double wall = (double)(now - t0) / 733333333.0;
            fprintf(stderr, "[GLADIUS] clock: %.2f s of game time in %.2f s, %lu frames "
                            "(%.1f fps)\n", (double)(g - g0), wall, frames, frames / wall);
            fflush(stderr);
            t0 = now;
            g0 = g;
            frames = 0;
        }
    }
}

