/**
 * Gladius - Recompiled Game Entry Point
 *
 * This is the executable that hosts the recompiled game code, on Windows,
 * macOS and Linux. It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install the fault handler (a VEH on Windows, signals on POSIX)
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the crash report (title_crash) for game-specific diagnosis
 *
 * What is not portable C -- where the executable is, where the output goes,
 * message boxes, symbol names, the fault handler, and on POSIX which thread
 * the title runs on -- is in the runtime (src/platform/host_main.h,
 * src/platform/recomp_fault.h), not here.
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       Gladius
 *   Title ID:    0x00000000
 *   Base addr:   0x00010000
 *   Entry point: 0x00000000
 *   Code size:   ~??? KB (.text)
 *   Sections:    ?? (list them)
 *   Kernel imports: ??
 */

#include "platform/xbox_winnt.h"     /* <windows.h> on Windows, the NT types elsewhere */
#include "platform/host_main.h"
#include "platform/recomp_fault.h"
#ifndef _WIN32
#include "host.h"                     /* src/host: the main-thread loop */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef __APPLE__
#include <malloc/malloc.h>            /* malloc_zone_pressure_relief */
#endif

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>
#include "xbox_watchpoint.h"
#include "xbox_fault_route.h"

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* g_eax..g_edi, g_esp, g_seh_ebp and the x87 state (g_fp_stack, g_fp_top,
 * g_fp_control_word, g_fp_cmp) come from xbox_memory_layout.h, through
 * xboxrecomp.h above: fields of one thread-local struct, declared once in
 * templates/runtime/recomp_cpu.h. Do not redeclare them here -- the names are
 * macros, so it does not compile, and the hand-written declarations it
 * replaces were how a register once came to be read from the wrong storage
 * (a plain `extern` of a __declspec(thread) variable resolves to the image's
 * TLS template, and the guest started with every register at zero).
 *
 * SSE state stays separate thread-locals. RECOMP_TLS is not optional here: a
 * plain `extern` would resolve to the TLS template, as above. Global for the
 * same reason the GPRs are: one guest routine can lift to several C
 * functions, so a value written in one body is read in the next. */
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x0015B2A7  /* XBE entry point VA */
/* Relative to the executable's own directory, build/<Config>/: the game's
 * files go in game/ at the top of this repository. A game/ folder beside the
 * executable is looked at first, and RECOMP_GAME_DIR overrides both. */
#define YOUR_GAME_XBE_PATH      "..\\..\\game\\gladius_eng.xbe"
#define YOUR_GAME_DIR            "..\\..\\game"
/* The image the console boots. A disc whose default.xbe only picks a language
 * and launches another image names that one here. */
#define YOUR_GAME_XBE_NAME       "gladius_eng.xbe"

/* ── Forward declarations ──────────────────────────────────── */

/* Defined in recomp_trace.c. Declared here rather than pulled from
 * the generated headers, which this file does not include. */
void recomp_profile_dump(void);
void recomp_exit_trace_init(void);      /* src/kernel/exit_trace.c */

/* Defined in kernel_bridge.c: the APU is a separate library the kernel must
 * not need to link, so the kernel takes its interrupt line as a callback. */
void xbox_SetApuInterruptSource(int (*pending)(void));

/* Generated with the lifted code (recomp_dispatch.c); declared in
 * recomp_types.h, which this file does not include. */
int recomp_dispatch_init(void);

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* ============================================================
 * Finding the game, from wherever the executable was started
 *
 * A player double-clicks the executable, or runs a shortcut to it, and in
 * neither case is the working directory anything in particular. So the game
 * is looked for beside the executable rather than beside the caller:
 *
 *   0. macOS, when the executable is inside a .app: <folder of the .app>/game,
 *      then <the .app>/Contents/Resources/game
 *   1. <exe dir>\\game\\               -- what a distributed build looks like
 *   2. <exe dir>\\YOUR_GAME_DIR     -- the development tree, where the build
 *                                     sits several levels under the repo
 *                                     (titles/<title>/build/<Config>/)
 *   3. the same one level up        -- a single-configuration build
 *                                     (Ninja: titles/<title>/build-mac/)
 *
 * RECOMP_GAME_DIR overrides all of them, for running one build against another copy
 * of the game files.
 * ============================================================ */

static char g_game_dir[MAX_PATH];
static char g_xbe_path[MAX_PATH];

/* The paths below are written with backslashes, as Windows spells them;
 * xbox_path_normalize turns them into forward slashes everywhere else and
 * leaves them alone on Windows. */
static BOOL file_exists(const char *path)
{
    return host_file_exists(path);
}

/* Fills g_game_dir and g_xbe_path, or returns FALSE having left a message in
 * `tried` naming every place that was looked at -- which is the only useful
 * thing to tell somebody whose game files are somewhere else. */
static BOOL find_game(char *tried, size_t tried_bytes)
{
    char exe[MAX_PATH], dir[MAX_PATH], candidate[MAX_PATH];
    const char *env = getenv("RECOMP_GAME_DIR");
    int i;

    tried[0] = '\0';
    if (env && *env) {
        snprintf(g_game_dir, sizeof g_game_dir, "%s", env);
        snprintf(g_xbe_path, sizeof g_xbe_path, "%s\\%s", env, YOUR_GAME_XBE_NAME);
        xbox_path_normalize(g_game_dir);
        xbox_path_normalize(g_xbe_path);
        if (file_exists(g_xbe_path))
            return TRUE;
        snprintf(tried, tried_bytes, "RECOMP_GAME_DIR: %s", g_xbe_path);
        return FALSE;
    }

#ifdef __APPLE__
    /* A macOS application bundle (scripts/make_macos_app.py): the game
     * folder beside the .app first, then one inside it. */
    {
        char app[MAX_PATH];

        if (host_app_bundle_dir(app, sizeof app)) {
            for (i = 0; i < 2; i++) {
                snprintf(candidate, sizeof candidate, "%s", app);
                if (i == 0) {
                    host_path_dirname(candidate);
                    strncat(candidate, "/game", sizeof candidate - strlen(candidate) - 1);
                } else {
                    strncat(candidate, "/Contents/Resources/game",
                            sizeof candidate - strlen(candidate) - 1);
                }
                snprintf(g_xbe_path, sizeof g_xbe_path, "%s/%s", candidate, YOUR_GAME_XBE_NAME);
                if (file_exists(g_xbe_path)) {
                    snprintf(g_game_dir, sizeof g_game_dir, "%s", candidate);
                    return TRUE;
                }
                {
                    size_t n = strlen(tried);
                    snprintf(tried + n, tried_bytes - n, "%s%s", n ? "\n" : "", g_xbe_path);
                }
            }
        }
    }
#endif

    if (!host_exe_path(exe, sizeof exe))
        return FALSE;
    snprintf(dir, sizeof dir, "%s", exe);
    host_path_dirname(dir);

    for (i = 0; i < 3; i++) {
        if (i == 0)
            snprintf(candidate, sizeof candidate, "%s\\game", dir);
        else if (i == 1)
            snprintf(candidate, sizeof candidate, "%s\\%s", dir, YOUR_GAME_DIR);
        else if (strncmp(YOUR_GAME_DIR, "..\\", 3) == 0)
            snprintf(candidate, sizeof candidate, "%s\\%s", dir, YOUR_GAME_DIR + 3);
        else
            break;
        snprintf(g_xbe_path, sizeof g_xbe_path, "%s\\%s", candidate, YOUR_GAME_XBE_NAME);
        xbox_path_normalize(candidate);
        xbox_path_normalize(g_xbe_path);
        if (file_exists(g_xbe_path)) {
            snprintf(g_game_dir, sizeof g_game_dir, "%s", candidate);
            return TRUE;
        }
        {
            size_t n = strlen(tried);
            snprintf(tried + n, tried_bytes - n, "%s%s", n ? "\n" : "", g_xbe_path);
        }
    }
    return FALSE;
}



/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* ── Fault handling ────────────────────────────────────────── */

/*
 * The runtime catches every host fault (src/platform/recomp_fault.h) and
 * hands it first to xbox_fault_route (src/kernel/xbox_fault_route.c), which
 * services the faults that are meant to happen -- a watchpoint, a trapped
 * device register, the rest of a 16 KB host page a 4 KB trap covered -- and
 * then to title_crash below, the report for everything else.
 *
 * When the recompiled game hits an access violation, the report prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the fault carries a
 * host program counter and nothing else -- there is no guest program counter
 * to report, and the host address changes every build. Two things recover
 * the guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the symbols already map host address back to guest
 *     function: host_symbol_name turns an anonymous pc into that name
 *     (dbghelp and the .pdb beside the .exe on Windows, dladdr elsewhere).
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 */
static void print_guest_context(uintptr_t pc)
{
    char name[256];
    uintptr_t disp = 0;

    if (host_symbol_name(pc, name, sizeof name, &disp))
        fprintf(stderr, "  in %s+0x%llX\n",
                name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* And the raw words, because the useful thing is often not a
         * return address. A fault through a garbage pointer is diagnosed
         * by finding where the pointer came from, and the object it was
         * loaded out of is usually sitting in the frame -- invisible in
         * the filtered list above, which keeps only code addresses. */
        fprintf(stderr, "  guest stack (raw):\n");
        for (i = 0; i < 48; i += 4)
            fprintf(stderr, "    [esp+%-3d] %08X %08X %08X %08X\n",
                    i * 4, sp[i], sp[i + 1], sp[i + 2], sp[i + 3]);
    }
}

/* Every fault the route declined, except breakpoints. Say something about
 * every one, not only access violations: a fault this stays silent on reads
 * as the process simply vanishing -- an exit code and nothing in the log. The
 * GPU register range used to be skipped outright, and Burnout 2's first run
 * on a fresh machine died that way, in a write to the vblank interrupt-enable
 * register. */
static void title_crash(const recomp_fault *f)
{
    if (f->kind != RECOMP_FAULT_ACCESS) {
        fprintf(stderr, "[EXCEPTION] code 0x%08lX at RIP=0x%llX (first chance)\n",
                (unsigned long)f->code,
                (unsigned long long)f->pc);
        print_guest_context(f->pc);
        fflush(stderr);
        return;
    }

    {
        uintptr_t fault_addr = f->host_addr;
        int is_write = f->is_write == 1;

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)f->pc,
            (unsigned long long)fault_addr,
            f->is_write < 0 ? "access" : is_write ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        /* The faulting instruction's bytes. When the fault is in a trapped
         * device page, this is the instruction form the runtime's decoder
         * did not know, which is exactly what has to be added to it. */
        {
            const uint8_t *ip = (const uint8_t *)f->pc;
            int i;
            fprintf(stderr, "  host instruction:");
            for (i = 0; i < 12; i++)
                fprintf(stderr, " %02X", ip[i]);
            fprintf(stderr, "\n");
        }
        print_guest_context(f->pc);

        /* Force the profile table out here, not at the next scheduled report.
         * The report interval means the file on disk lags the run, and the
         * fault lands inside that lag -- so without this, the functions that
         * ran immediately before the crash are missing from it, and their
         * absence reads as "never ran". */
        recomp_profile_dump();

        /* RECOMP_FIND_VALUE: when the fault is through a garbage pointer,
         * the next question is who is holding it. Scanning guest RAM for
         * the value names the field, and that field is what to point
         * RECOMP_WATCH_WRITE at on the next run. */
        xbox_watch_scan_on_crash();

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging, named.
         *
         * The window is this module's own range (host_module_range), not a
         * preferred base, which ASLR moves. The header and the leading
         * "[i] 0x..." are kept as they were for anything that parses them. */
        {
            const uintptr_t *sp = (const uintptr_t *)f->sp;
            uintptr_t lo = 0, hi = 0;
            int shown = 0;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            if (sp && host_module_range(&lo, &hi)) {
                for (int i = 0; i < 256 && shown < 12; i++) {
                    char name[256];
                    uintptr_t disp = 0;

                    if (sp[i] < lo || sp[i] >= hi)
                        continue;
                    if (host_symbol_name(sp[i], name, sizeof name, &disp))
                        fprintf(stderr, "    [%d] 0x%llX %s+0x%llX\n", i,
                                (unsigned long long)sp[i], name,
                                (unsigned long long)disp);
                    else
                        fprintf(stderr, "    [%d] 0x%llX (module+0x%llX)\n", i,
                                (unsigned long long)sp[i],
                                (unsigned long long)(sp[i] - lo));
                    shown++;
                }
            }
        }
        fflush(stderr);
    }
}

/* ── Start-up ──────────────────────────────────────────────── */

/* Where the diagnostics go: host_setup_output (src/platform/host_main.h).
 * A windowed program started by double-click has no console, so its output
 * goes to <executable>.log unless it was redirected or started from a
 * terminal. The log's path is remembered so a failure can name it in its
 * message box: "it did not start" is not a bug report, and the file is. */
static char g_log_path[MAX_PATH];

/* Appended to a message box, when there is a file worth reading. */
static void log_hint(char *buf, size_t bytes)
{
    size_t n = strlen(buf);

    if (g_log_path[0] && bytes > n)
        snprintf(buf + n, bytes - n, "\n\nThere is more in:\n%s", g_log_path);
}

/* The title, start to finish: the same on every host. On POSIX it runs on a
 * thread of its own (host_main_posix), so the process's main thread is free
 * for the window. */
static int title_main(int argc, char **argv)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)argc;
    (void)argv;

    /* Before anything prints: a windowed program has nowhere to print
     * unless this says where. */
    host_setup_output(g_log_path, sizeof g_log_path);

    /* Unbuffered output for immediate visibility during debugging, and so a
     * crash keeps the tail of the log rather than losing it in a buffer. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=== Gladius - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* The fault handler, first in line: a VEH on Windows, signals on POSIX.
     * It loads the symbols the report names functions with up front, since
     * at fault time the process is already in a bad way. */
    recomp_fault_install(xbox_fault_route, title_crash);
    /* And the other way a run ends: an exit nobody logged. Prints [EXIT]
     * with the code and both stacks before the process goes (exit_trace.c). */
    recomp_exit_trace_init();

    /* Step 1: Find and load the XBE */
    {
        char tried[1024], message[1400];

        if (!find_game(tried, sizeof tried)) {
            snprintf(message, sizeof message,
                     "This needs the game's own files, which are not included.\n\n"
                     "Put them in a folder called \"game\" next to this program, so that\n"
                     "\"game\\" YOUR_GAME_XBE_NAME "\" exists, or set RECOMP_GAME_DIR to where they are.\n\n"
                     "Looked for:\n%s", tried);
            fprintf(stderr, "%s\n", message);
            log_hint(message, sizeof message);
            host_error_box("Gladius", message);
            return 1;
        }
        printf("Game files: %s\n", g_game_dir);
        if (!load_xbe(g_xbe_path, &xbe_data, &xbe_size)) {
            snprintf(message, sizeof message,
                     "Found the game at\n%s\nbut could not read " YOUR_GAME_XBE_NAME ".",
                     g_game_dir);
            fprintf(stderr, "%s\n", message);
            log_hint(message, sizeof message);
            host_error_box("Gladius", message);
            return 1;
        }
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        static const char layout_failed[] =
            "Failed to initialize Xbox memory layout.\n"
            "The required virtual address range may be unavailable.";
        fprintf(stderr, "%s\n", layout_failed);
        host_error_box("Recomp", layout_failed);
        free(xbe_data);
        return 1;
    }

    /* The layout has copied out everything it needs (headers, sections,
     * certificate, TLS directory) into guest memory and its own statics,
     * and nothing keeps a pointer into the file buffer, so let it go now
     * rather than at exit: it is the whole XBE (45 MB on BLiNX). */
    free(xbe_data);
    xbe_data = NULL;
#ifdef __APPLE__
    /* macOS's allocator keeps a freed large block cached, still resident,
     * for reuse: BLiNX's 42.7 MB stayed in the footprint as "Malloc Large
     * (empty)" after the free. Ask it to hand cached memory back now. */
    malloc_zone_pressure_relief(NULL, 0);
#endif

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* The device registers the layout traps (the vblank interrupt page, the
     * APU, AC'97), registered with the fault route that services them. Only
     * a table: a range whose page is never trapped never faults. */
    apu_fault_register();

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(g_game_dir, NULL);
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 5a: memory watchpoints, if any were asked for. After the
     * bridge, so the pages a watch names are mapped and committed. */
    xbox_watch_init();

    /* Step 5b: the emulated APU, when its registers are trapped.
     *
     * RECOMP_AC97_READY makes the memory layout report the audio codec as
     * present and turn the APU's register pages into faults, so the title's
     * DirectSound can be answered by the emulated APU instead of by plain
     * memory. The fault handler above routes those faults to
     * apu_hook_handle_mmio(), which declines every access -- silently --
     * until the device exists. Create it under the same switch, and hand the
     * kernel its interrupt line so the timer thread can deliver it. */
    /* On unless turned off: the emulated audio hardware is not an
     * experiment any more, and without it the first DirectSound register
     * the title touches faults. xbox_EnvSwitch, so RECOMP_AC97_READY=0
     * still takes it away. */
    if (xbox_EnvSwitch("RECOMP_AC97_READY", 1)) {
        /* The fault handler serves from g_apu_state, and init returns the
         * device rather than storing it: left unassigned, every APU register
         * access was declined and reported as a crash reading 0xFE801100
         * (NV_PAPU_FECTL), the first register DirectSound touches. */
        /* The APU addresses guest memory physically, and in this runtime
         * physical page P lives in the contiguous window at
         * XBOX_CONTIG_BASE + P, not at guest VA P: the low 64 MB of VA is
         * the image and the heap, a different mapping. Handing it VA 0 made
         * every DirectSound buffer and DSP page table read the wrong bytes;
         * TimeSplitters 2's scatter-gather tables all read as empty. */
        g_apu_state = mcpx_apu_init_standalone(
            (uint8_t *)g_xbox_mem_offset + XBOX_CONTIG_BASE);
        if (!g_apu_state)
            fprintf(stderr, "[BOOT] APU init failed; APU register accesses "
                            "will fault\n");
        else
            xbox_SetApuInterruptSource(mcpx_apu_irq_pending);
    }

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* ── Entry points ──────────────────────────────────────────── */

#ifdef _WIN32
/* The windowed program's entry point. */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;
    return title_main(__argc, __argv);
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    return title_main(argc, argv);
}
#else
/* The title runs on its own thread; this, the process's main thread, runs
 * the host loop that every window needs on macOS (src/host). */
int main(int argc, char **argv)
{
    static const host_loop loop = { recomp_host_loop_run, recomp_host_loop_quit };
    return host_main_posix(argc, argv, title_main, &loop);
}
#endif
