cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Gladius - Static Recompilation
#
# Built against the xboxrecomp toolkit, a checkout beside this one
# (../xboxrecomp) unless XBOXRECOMP_DIR says otherwise. See README.md.
# ============================================================================

# C and C++: the runtime's renderer has C++ in it (the DXC loader, VMA),
# so the executable links with the C++ driver and its standard library.
# The game's own sources are all C.
project(gladius_recomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# ── Bring-up diagnostics ────────────────────────────────────────
#
# ebx/esi/edi are callee-saved on x86 and the recompiler keeps them in globals,
# so a function whose epilogue did not lift correctly corrupts its *caller*.
# There is no crash and no message where the failure happens: the caller
# carries on with a wrong loop cursor or a wrong `this` and faults somewhere
# unrelated, often several frames away.
#
# With this on, every call -- direct and indirect -- checks that ebx, esi and
# edi came back unchanged and that esp came back at least 4 higher, and names
# the offending function. Off by default because it costs a few compares per
# call; leave it on until a title boots.
#
# Directory scope, and set before add_subdirectory, deliberately: the check is
# emitted into generated game code but recomp_abi_violation_log() is compiled
# into xbox_kernel. Defining it on the game target alone builds the calls
# without the callee and fails to link.
option(RECOMP_ABI_CHECK "Verify callee-saved registers across every call" OFF)
if(RECOMP_ABI_CHECK)
    add_compile_definitions(RECOMP_ABI_CHECK)
    message(STATUS "ABI check enabled: callee-saved registers verified per call")
endif()

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 250 (scripts/recompile.py's default), you get recomp_0000.c, recomp_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME}
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)
# A windowed program on Windows (WinMain, no console); a plain executable
# elsewhere, whose main() hands the title its own thread (host_main_posix).
if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE ON)
endif()

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

# Host libraries needed at link time. Off Windows the runtime libraries
# bring their own (pthreads, dl, SDL3 and its frameworks, OpenSSL).
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11
        dxgi
        dxguid
        xinput
        winmm
        # Lets the fault handler resolve the host RIP back to the generated
        # function's symbol, i.e. to the guest function that faulted.
        dbghelp
    )
else()
    # The same for the POSIX fault report, which names a host pc with
    # dladdr: that sees only exported symbols, so export the generated
    # functions (-rdynamic on Linux; a Mach-O executable exports them as is).
    set_target_properties(${PROJECT_NAME} PROPERTIES ENABLE_EXPORTS ON)
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        # A PDB in every configuration, so the crash handler's SymFromAddr can
        # name the generated function a host RIP is in. Linker-only: without
        # /Zi it holds public symbols -- every sub_XXXXXXXX -- and costs no
        # recompile of the large generated files.
        /DEBUG
    )
    # The generated code files can be very large. Increase object file section
    # limit, and compile them in parallel: MSBuild only parallelises across
    # projects, so without /MP the 36 generated files of a 36,000-function
    # title compile one after another on a single core.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /MP)

    # Drop MSVC's Debug runtime checks. /RTC1 traps a local read before its
    # first write, and lifted code does that deliberately: an fpo_leaf function
    # inherits its caller's frame, so it reads ebp before assigning it and
    # picks the value up from g_seh_ebp. That is correct by design here, but
    # /RTC1 cannot know it and aborts through a message box -- which never
    # reaches stderr, so the run looks like a silent hang at the first lifted
    # frame. Debug is the configuration bring-up uses, so this cannot stay on.
    string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
#
# # What the player's one Widescreen switch means for this game. Say it
# # once you know: a game with a 16:9 mode of its own needs only the console
# # flag (NATIVE); one without needs its camera widened (HOR_PLUS). REGISTER
# # is the vertex constant register holding its projection, 60 if left out.
# # A game nobody has described is treated as NATIVE, and looks stretched
# # if it is not. See src/config/CMakeLists.txt.
# Gladius's engine widens its own view for a 16:9 screen (recomp_manual.c,
# sub_001BBD60); its 3D programs project through c176..c179, so that is the
# register that tells the renderer a draw is 3D and not to be kept at 4:3.
recomp_title_widescreen(NATIVE REGISTER 176 CENTRE_2D)
