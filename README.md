# Gladius (Xbox) — static recompilation

LucasArts' *Gladius* (Xbox, 2003) recompiled into a native program for
Windows, macOS and Linux (including SteamOS and the Steam Deck). You bring your
own copy of the game, and it is built from that copy on your computer. This
repository and its releases contain no game code or data.

- [Play](#play): download, point it at your disc, play.
- [Build it yourself](#build-it-yourself): for developers working on the port.

# Play

## What you need

- Gladius, the English PAL release: a disc image (`.iso` or `.xiso`), a folder
  with the disc's files in it, or a copy made to run from a console's hard
  disk. Other releases are refused (see [Releases of the
  game](#releases-of-the-game)).
- 64-bit Windows 10 or 11, a Mac with Apple Silicon (macOS 13 or later), or
  x86-64 Linux, SteamOS included, with Vulkan drivers.
- About 500 MB of space, plus room for the game's files if you give it a disc
  image (an image is unpacked once; a folder is used where it is).

Nothing else needs installing: the download carries everything it builds the
game with.

## Download and start

Download the file for your system from the
[latest release](https://github.com/phobos665/gladius-recomp/releases/latest):

| System | File | Start |
| --- | --- | --- |
| Windows | `gladius-<version>-windows-x86_64.zip` | Unzip it and run `gladius.exe` |
| macOS | `gladius-<version>-macos-arm64.tar.xz` | Unpack it, then see [macOS](#macos) below |
| Linux, SteamOS | `gladius-<version>-linux-x86_64.tar.xz` | Unpack it and run `./gladius` |

The first time it starts, it asks for your copy of the game: choose **Disc
image...** or **Folder...**, or paste a path. It then builds the game, which
takes a few minutes (longer on a Steam Deck), and opens the settings. Press
**Play**. After that it opens straight to the settings.

**Windows** may say it protected your PC, because the release is not signed:
choose More info, then Run anyway.

### macOS

Move `Gladius.app` to Applications. It is not signed with an Apple Developer
ID, so before opening it the first time, open Terminal and run

```sh
xattr -dr com.apple.quarantine "/Applications/Gladius.app"
```

That clears the flag macOS puts on everything downloaded, from the app and the
tools it builds the game with. Then open it.

### Steam Deck and SteamOS

Unpack the release in desktop mode and run `gladius` once to build the game.
To play from Game Mode, add `gladius` to Steam (Add a Non-Steam Game) and give
the shortcut `icon.png` from the release folder as its icon.

## What works

Played on macOS (Apple Silicon) from boot through the menus, the intro movie,
the in-engine intro and the first tutorial battle, at 60 fps with sound.

- **60 fps, natively.** It is a PAL disc. A PAL Xbox runs it at 60 Hz when the
  console's PAL60 setting is on, and the port reports that setting (the
  default).
- **Native widescreen.** The engine already handles a 16:9 screen; this port
  tells it the screen is 16:9. The 3D fills the picture unstretched; the
  menus, HUD and tips, which are drawn for 4:3, stay 4:3 in the middle.

Known issues:

- After the intro, the "Ask Usus ... Press A to continue" tip sometimes does
  not take A. Its voice line's stream is paused right after it is created
  and never fed.

## Controls

Controllers work as an Xbox pad, and the keyboard plays as controller 1.
Change the bindings in the launcher's Input tab. In the launcher itself,
Enter or A plays, Esc or B quits, and Tab or the shoulders change tab.

While playing:

| Key | |
| --- | --- |
| F9 | Show or hide the frame rate |
| F10 | Change the frame cap |
| F11 | Save a screenshot and a capture of the frame, for bug reports |
| Alt+Enter | Switch between fullscreen and a window |

## Settings

The launcher sets resolution, widescreen (native 16:9, off by default), frame
rate, fullscreen and the controls.

The settings file (see [Where things are kept](#where-things-are-kept)) also
takes `pal60 = 0`, which plays as a 50 Hz-only PAL console would. The default,
1, is 60 Hz.

## Where things are kept

The built game, its saves and the logs are in one folder per user:

| | |
| --- | --- |
| Windows | `%LOCALAPPDATA%\xboxrecomp\builds\4C410008\` |
| macOS | `~/Library/Application Support/xboxrecomp/builds/4C410008/` |
| Linux | `~/.local/share/xboxrecomp/builds/4C410008/` |

In it, `game/UDATA/` holds your saves, `build.log` is the build's log and
`gladius_recomp.log` the game's. Your own disc or folder is never written to.

The settings and controls are kept apart from the build:

| | Settings | Controls |
| --- | --- | --- |
| Windows | `%APPDATA%\xboxrecomp\titles\4C410008.conf` | `%APPDATA%\xboxrecomp\input_bindings.json` |
| macOS | `~/Library/Application Support/xboxrecomp/titles/4C410008.conf` | `~/Library/Application Support/xboxrecomp/input_bindings.json` |
| Linux | `~/.config/xboxrecomp/titles/4C410008.conf` | `~/.config/xboxrecomp/input_bindings.json` |

## Updating

Download the new release and start it. It sees that the game was built by an
older release and offers **Build again**, with your disc remembered. Saves and
settings carry over.

## Problems

- The disc is refused: it is not one of the releases below. Please open an
  issue with the title, region and SHA-1 the message shows.
- The build fails: open an issue and attach `build.log` (see [Where things
  are kept](#where-things-are-kept)).
- A black window and nothing else: attach `gladius_recomp.log` from the same
  folder.
- A rendering bug in one place: press F11 there, and attach the `.bmp` and
  `.d3dcap` files it writes beside the game.

## Releases of the game

The disc's `default.xbe` is only a language picker; the port boots the
English game, `gladius_eng.xbe`, and recognises it by its SHA-1:

| Copy | `gladius_eng.xbe` SHA-1 |
| --- | --- |
| Disc (English) | `0a751bd1378e27f680c38c7bd8e562d1a4a034fe` |
| Hard-drive copy (English) | `212ca3df63da2582c59791198702de0d7f96360a` |

Any other is refused.

# Build it yourself

For working on the port. This builds the same game by hand, from a clone,
against the [DirectXbox-Recomp](https://github.com/phobos665/DirectXbox-Recomp)
toolkit: the game's own code is lifted to C from the XBE (PAL disc, title ID
`0x4C410008`, XDK 5659) and runs natively, and Direct3D 8 and DirectSound are
replaced at the API boundary.

## Layout

```
gladius-recomp/
  CMakeLists.txt          the project; pulls in ../xboxrecomp
  src/main.c              entry point, game folder lookup (from the toolkit's template)
  src/recomp_manual.c     the title's own overrides, each with its reason
  src/recomp/gen/         lifted code (generated, not committed)
  config/seeds/           functions the disassembler cannot find on its own
  config/sections/        sections the XBE marks executable that are data
  release/gladius.json    the recipe the release workflow builds from
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

## Running a build of your own

```bash
build/Release/gladius_launcher
```

The launcher starts `gladius_recomp` beside it; `build/Release/gladius_recomp`
starts the game directly with the settings as they are. The settings file is
as in [Where things are kept](#where-things-are-kept), and every setting can
also be an environment variable, which wins over the file:

| Setting | Variable | Default | |
|---|---|---|---|
| `widescreen` | `RECOMP_WIDESCREEN` | 0 | native 16:9 |
| `pal60` | `RECOMP_PAL60` | 1 | 0 is a 50 Hz-only PAL console |
| — | `RECOMP_GLADIUS_WIDE` | 1 | 0 keeps the engine at 4:3 under widescreen |
| — | `RECOMP_GLADIUS_CLOCK` | 0 | 1 reports game time against wall time |
| — | `RECOMP_GLADIUS_PORTRAITS` | 1 | 0 leaves the menus' 3D portraits unadjusted under widescreen |

**Saves.** The game keeps its saves on the Xbox's E: drive, which the runtime
maps to the game folder: in a build of your own they are written into
`game/UDATA`. Test runs belong on a copy of `game/` without `UDATA`
(`RECOMP_GAME_DIR`) and with `RECOMP_SAVE_DIR` set to a fresh folder.

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

## Making a release

Pushing a version tag (`git tag v1.0 && git push origin v1.0`) runs
`.github/workflows/release.yml`, which builds the Windows, macOS and Linux
downloads from `release/gladius.json` and attaches them to the tag's GitHub
release. No game files are involved. How the downloads work is in the
toolkit's
[self-build-release.md](https://github.com/phobos665/DirectXbox-Recomp/blob/main/docs/technical/self-build-release.md).
