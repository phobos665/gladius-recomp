# Gladius (Xbox) — static recompilation

A native build of LucasArts' *Gladius* (Xbox, 2003; PAL disc, title ID
`0x4C410008`, XDK 5659) made with the [xboxrecomp](https://github.com/phobos665/xboxrecomp)
toolkit. The game's own code is lifted to C from the XBE and runs natively;
Direct3D 8 and DirectSound are replaced at the API boundary.

**No game code or data is in this repository.** You need the files from a
copy of the disc you own.

## Current State

Runs on macOS (Apple Silicon) from boot through the menus, the intro movie,
the in-engine intro and the first tutorial battle, at 60 fps with sound.

- **60 fps, natively.** It is a PAL disc. A PAL Xbox runs it at 60 Hz when the
  console's PAL60 setting is on, and the runtime reports that setting
  (`pal60 = 1`, the default).
- **Native widescreen** (`widescreen = 1`). The engine already handles a 16:9
  screen; this build tells it the screen is 16:9. The 3D fills the picture
  unstretched; the menus, HUD and tips, which are drawn for 4:3, stay 4:3 in
  the middle.

Known issues:

- After the intro, the "Ask Usus ... Press A to continue" tip sometimes does
  not take A. Its voice line's stream is paused right after it is created
  and never fed.

## Layout

```
gladius-recomp/
  CMakeLists.txt          the project; pulls in ../xboxrecomp
  src/main.c              entry point, game folder lookup (from the toolkit's template)
  src/recomp_manual.c     the title's own overrides, each with its reason
  src/recomp/gen/         lifted code (generated, not committed)
  config/seeds/           functions the disassembler cannot find on its own
  config/sections/        sections the XBE marks executable that are data
  scripts/recompile.sh    runs the toolkit's lift pipeline for this game
  game/                   your game files (not committed)
```

## Building

You need the toolkit checked out beside this repository, at
`../xboxrecomp`, at a commit that has the fixes this game found. Until those
are on the toolkit's `main`, that is its `title/gladius` branch. Point
`XBOXRECOMP_DIR` elsewhere to use another checkout.

The toolkit's own requirements apply: see its `docs/GETTING_STARTED.md`
(on macOS: Homebrew CMake and Ninja, the LunarG Vulkan SDK, a Python with
`capstone` in the toolkit's `.venv`).

1. **Game files.** Extract the disc into `game/`, so that
   `game/gladius_eng.xbe` exists. The disc's `default.xbe` is only a language
   picker; this build boots the English image.
2. **XDK symbols.** Build the toolkit's `third_party/XbSymbolDatabase` CLI
   (`cmake --build ... --target XbSymbolDatabaseCLI`), then from the toolkit:

   ```bash
   .venv/bin/python -m tools.xdk_symbols ../gladius-recomp/game/gladius_eng.xbe \
       --cli <path to XbSymbolDatabaseCLI>
   ```

   That writes `game/gladius_eng_xdk_symbols.json`. Without it nothing is
   replaced by name, and the game runs with no picture and no sound.
3. **Lift.** `scripts/recompile.sh` (about a minute).
4. **Build.**

   ```bash
   cmake -S . -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build/Release --target gladius_recomp gladius_launcher --parallel
   ```

## Running

```bash
build/Release/gladius_launcher
```

The launcher (the toolkit's, on Windows and macOS) sets resolution,
widescreen, frame rate, fullscreen and the controls, writes them to the
settings file below, and starts `gladius_recomp` beside it. Keyboard, mouse
or pad: Enter or A plays, Esc or B quits, Tab or the shoulders change tab.
`build/Release/gladius_recomp` starts the game directly with the settings
as they are.

Settings live in the per-user settings file the toolkit writes
(`~/Library/Application Support/xboxrecomp/titles/4C410008.conf` on macOS), or
as environment variables:

| Setting | Variable | Default | |
|---|---|---|---|
| `widescreen` | `RECOMP_WIDESCREEN` | 0 | native 16:9 |
| `pal60` | `RECOMP_PAL60` | 1 | 0 is a 50 Hz-only PAL console |
| — | `RECOMP_GLADIUS_WIDE` | 1 | 0 keeps the engine at 4:3 under widescreen |
| — | `RECOMP_GLADIUS_CLOCK` | 0 | 1 reports game time against wall time |

F9 shows the frame rate, F10 steps the frame cap, F11 saves a capture of the
frame on screen.

**Saves.** The game keeps its saves on the Xbox's E: drive, which the runtime
maps to the game folder: they are written into `game/UDATA`. Test runs belong
on a copy of `game/` without `UDATA` (`RECOMP_GAME_DIR`) and with
`RECOMP_SAVE_DIR` set to a fresh folder.

## What is specific to this game

All in `src/recomp_manual.c`, `config/` and `CMakeLists.txt`:

- **Streaming threads wait for their creator.** The two loader workers check a
  ready flag their creator sets only after creating them; on the console the
  creator is never preempted in between.
- **Widescreen through the engine.** `sub_001BBD60` sets the 2D layer and the
  3D projection from the screen's width and height. Under widescreen it is
  given 640x360, and the stored screen size goes back to 640x480 after.
- **3D in the menus' layout.** A camera draws into a rectangle of its own
  (sub_0019E8E0: viewport sub_001AF350, scissor sub_001AF990); Choose Your
  Destiny's portraits are such cameras. Under widescreen their middle is
  drawn in towards the screen's as the 4:3 2D round them is, and any
  scissor set to a camera's rectangle (the menu widget, sub_00119B40, sets
  it last) is narrowed to the portrait's width on screen
  (`RECOMP_GLADIUS_PORTRAITS=0` leaves them be).
- **Widescreen declaration.** `recomp_title_widescreen(NATIVE REGISTER 176
  CENTRE_2D)`: the engine has its own 16:9 handling, its 3D projects through
  c176, and its 2D stays 4:3 in the middle.
- **Seeds:** the Sofdec `PSGSFD_I`/`_P` bases, controller init in XPP, thread
  routines, a function merged into its neighbour after a no-return call, and a
  Sofdec callback.
- **Data sections:** `DOLBY` (the DSP's Dolby encoder image).
