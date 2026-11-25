# The Apprentice: CD-i static recompilation

A native Windows and Linux / SteamOS (Steam Deck) static recompilation toolchain
and HLE runtime for *The Apprentice* (The Vision Factory / Philips Interactive
Media, 1994, CD-i, USA release).

> **No derived code or game assets are included in this repository or its
> release packages.** You must provide your own copy of the CD-i disc image
> (`.chd` or `.bin`/`.cue`). The game's 68000 program modules are extracted and
> translated into C **locally on the user's machine** from their disc image (at
> build time via `./build.sh` / `build.bat`, or automatically on first launch of
> the `.AppImage` into `saves/derived/`).

A clean-room runtime provides what the game's programs expect from a CD-i
player:
* the CD-RTOS (OS-9) kernel;
* the CD file manager;
* the video decoder, presented with Direct3D 11 (Windows) or OpenGL 3 (Linux /
  SteamOS) at any resolution;
* the ADPCM audio processor and CD-DA playback;
* the ROM trap libraries.

## Credits & Acknowledgments

This project is based on the original Windows static recompilation of *The
Apprentice* by **[tools81](https://github.com/tools81)**
([`tools81/the-apprentice-static-recomp`](https://github.com/tools81/the-apprentice-static-recomp)),
which in turn was built from the CD-i static recompilation template and HLE
runtime components developed for the *Link: The Faces of Evil*, *Zelda's
Adventure* (multi-process kernel and streaming soundmap player), and *Laser
Lords* (Cartoon HD / HD+ upscaler and motion interpolation) ports.

Because this repository removes all pre-generated 68000-to-C files (`gen/`) so
that no code derived from the game's binaries is stored in the repository or
distributed in releases (all derived code is generated on the user's machine),
it is maintained as a standalone repository rather than a direct GitHub fork.
Full credit for the 68000 analyzer/recompiler (`tools/`) and CD-RTOS HLE
runtime design goes to **tools81**. See
[What this game needed](#what-this-game-needed).

## Installing a release

Both platforms distribute as a **single self-contained executable** containing
**zero derived game code or game assets**, with the recompiler and CHD
conversion tools embedded directly inside the binary:

* **`Apprentice-<version>-x86_64.AppImage`** (Linux / SteamOS / Steam Deck):
  make it executable (`chmod +x Apprentice-*.AppImage`) and run it. It opens an
  interactive graphical launcher and file browser (navigable with Steam Deck
  controls, keyboard, or mouse/touch) that locates your `.cue`/`.bin` (or
  converts a `.chd` disc image into `./saves/`), derives the 68000-to-C code
  locally on your machine into `./saves/derived/`, and compiles it to a native
  module `./saves/derived/recomp_user.so` (if no system C compiler is present,
  an interactive selection screen lets you either download a portable Zig/Clang
  C compiler into `./saves/derived/compiler/` on the fly or run immediately with
  the built-in 68000 engine). It provides an in-game Quick Menu overlay
  (`Start` / `Burger` `≡` button or `F1`) while `Select` / `View` (`⧉`) or `P`
  opens the original CD-i in-game pause menu. To register a native Linux Steam
  shortcut (clearing any accidental Proton override), click
  **Steam Shortcut (Native Linux)** in the launcher or pass `--add-to-steam`.
* **`Apprentice-<version>-x64.exe`** (Windows): a single standalone `.exe` that
  behaves identically to the Linux `.AppImage`. Place it in any folder and run
  it: it opens the same interactive graphical launcher and in-window file
  browser (or Windows file dialog), converts `.chd` images into `./saves/`,
  derives the 68000-to-C code locally into `./saves/derived/` (offering the same
  interactive selection screen to download a portable Zig/Clang C compiler into
  `./saves/derived/compiler/` or use the built-in 68000 engine if no system C
  compiler is found), renders Cartoon HD / Cartoon HD+ directly on the GPU via
  Direct3D 11, and provides the in-game Quick Menu overlay (`Start` / `Menu` `≡`
  button or `F1`) while `Select` / `View` (`⧉`) or `P` opens the original CD-i
  in-game pause menu.

Windows 10 or later (64-bit), or 64-bit Linux (SteamOS 3+, Ubuntu, Fedora,
Arch, Debian).

**100% Self-Contained (CWD & tmp only).** Everything the build or the game
writes stays strictly inside the current working directory (`./saves/` and
`./apprentice.ini`) or `/tmp` (`%TEMP%` on Windows) for temporary tool
extraction. Nothing is ever written to AppData, `%USERPROFILE%`, or external
folders.

**Updates.** When `update_repo` is configured in `game.json`, the game can check
GitHub for a newer release in the background. `apprentice.ini` controls this:
* `update_check=1` asks first (the default);
* `update_check=auto` installs without asking;
* `update_check=0` turns the check off.

`--no-update` skips the check for one run.

`apprentice.ini` keys:

| Key | Meaning |
|-----|---------|
| `rom=` | The disc image (`.cue`/`.bin`) or the folder that holds it |
| `update_check=` | `1`, `auto` or `0` (see above) |
| `show_launcher=` | `1` to always open the GUI launcher on start, `0` (default) to open it only when no disc image is found or `--gui` is passed |

### Making a release

1. Set the version in `VERSION`.
2. On Linux, run `./package.sh` (or `./build.sh --stub`). On Windows, run
   `package.bat` (or `build.bat --stub`). They write the single self-contained
   release binary to `dist/`:
   * Linux / SteamOS (`./package.sh`):
     * `dist/Apprentice-<version>-x86_64.AppImage`
   * Windows (`package.bat`):
     * `dist/Apprentice-<version>-x64.exe`
   * `dist/SHA256SUMS.txt`

   Neither the disc image nor any derived `gen/` code is packaged:
   both scripts build with `tools/gen.py --stub` so release binaries contain
   zero derived game code.
3. Publish them as a GitHub release tagged `v<version>`.

## Building and running

1. Optionally put your disc image in `rom/` (`.chd` or `.bin` + `.cue`), or
   simply select it in the graphical launcher at first run.
2. Run `./build.sh` (Linux / SteamOS) or `build.bat` (Windows). It:
   1. generates the 68000-to-C code locally in `gen/` if a disc image is in
      `rom/` (or generates a zero-derived-code stub if `--stub` is passed or no
      disc image is in `rom/` yet);
   2. embeds the Python recompiler and CHD tools into `build/cmake/embedded_tools.h`;
   3. compiles and outputs the single self-contained executable:
      * Linux / SteamOS: `dist/Apprentice-<version>-x86_64.AppImage`
      * Windows: `dist/Apprentice-<version>-x64.exe`
3. Run the resulting `.AppImage` or `.exe`. It looks for the disc image in
   `rom/` or `saves/` in the current working directory, accepts a path on the
   command line, or lets you browse for `.cue`, `.bin`, or `.chd` files in the
   built-in graphical launcher (`--gui`).

Requirements:
* **Linux / SteamOS:** GCC or Clang, CMake 3.19+, Python 3.8+, X11 and OpenGL
  development headers (`libx11-dev`, `libgl-dev`), and `mksquashfs`
  (`squashfs-tools`) for `.AppImage` packaging. Audio (PipeWire / PulseAudio /
  ALSA) and gamepads (`/dev/input/js*`) are used directly without extra
  build-time dependencies.
* **Windows:** Windows 10+, Visual Studio 2026 (the C compiler), CMake 3.19+
  and Python 3.8+. No third-party libraries are needed.

Saves and other files go to `saves/` next to the program (or `.AppImage`):
* the game's NVRAM file `nvr_apprentice` (saved games and the high-score
  table);
* the quicksave slots `quick_01.state` … `quick_10.state` (shared by both
  programs);
* `derived/` (when running a zero-derived-code release build: holds the C code
  and native module derived from your disc image on first run);
* `settings.ini` (window and filter) and `enhancements.ini` (run-ahead,
  interpolation, upscaler, current slot);
* `recomp.log`.

### Controls

| CD-i | Keyboard | Gamepad (XInput / Steam Deck) |
|------|----------|-------------------------------|
| Thumbpad | Arrow keys (Shift = faster) | Left stick / D-pad |
| Button 1 | Z, Space or A | A |
| Button 2 | X, Ctrl or S | B or Y |
| Both buttons | C or Enter | X |
| Original CD-i In-Game Pause Menu | P or Pause | Select / View (`⧉`) |

| Key / Button | Function |
|--------------|----------|
| F1 or Start / Burger (`≡`) / Guide | In-game Quick Menu overlay (Windows, Linux, Steam Deck) |
| P, Pause or Select / View (`⧉`) | Original CD-i in-game pause menu (`CONTINUE` / `ABORT`) |
| F2 or L1 / LB | Quicksave to the current slot |
| F3 | Next quicksave slot (1-10) |
| F4 or R1 / RB | Load the current slot |
| F5 | Invincibility on/off (the player only) |
| F6 | Run-ahead: off, 1, 2 or 3 fields |
| F7 | Sprite interpolation on/off |
| F8 | Upscaler: Cartoon HD, Cartoon HD+, off |
| F9 | Aspect mode |
| F10 | Filter (sharp, smooth, CRT, nearest) |
| F11 or Alt+Enter | Full screen |
| F12 | Crop overscan borders on/off |
| Tab or R2 / RT (hold) | Run fast |
| Esc | Quit |

Messages for these keys appear at the bottom of the picture.

## Enhancements

The front end (`runtime/frontend.c`) runs between fields, when no 68k
process is running. At that point the whole machine can be snapshotted
(`runtime/state.c`): the RAM in use, the CPU, the kernel, the CD file
manager, video and audio.

* **Quicksaves (F2/F3/F4).** A snapshot of the machine in one of ten slots.
  Loading one continues exactly where it was saved; the pictures after a load
  are identical to the pictures after the save. The disc's own save game and
  high scores (NVRAM) are separate and unaffected.
* **Invincibility (F5).** The game keeps Marvin's life in one byte of his
  object, which is always the first object slot (`0x108C6D`, game.json
  `"invincibility"`). Getting hit, running out of time and the other ways of
  dying all bring it from 1 to 0. The level code that does that is loaded from
  disc and runs in the interpreter; with invincibility on, the interpreter
  skips any byte write that would take that life from alive to dead. Enemies
  are not affected: they still die when Marvin jumps on them. Touching an
  enemy does nothing to Marvin, and lives never go down.
* **Run-ahead (F6, default 1 field).** After each field the machine is
  snapshotted and run one more field with the current input. That field is
  shown, then the snapshot is restored. Input shows on screen one field
  (17 ms) sooner, at the cost of emulating every field twice. With 1 or 2
  fields the picture shown at field N is exactly field N+1 or N+2 of a plain
  run.
* **Sprite interpolation (F7, default on).** When the game holds a picture
  for several fields, the emulation looks ahead from the same snapshot for
  the next one. Parts that moved as a whole are shown along their path
  (`runtime/motion.c`, from the Laser Lords port), so motion is shown at 60
  frames a second. Hand-drawn changes switch on time and are never blended.
  In practice *The Apprentice* already moves everything at 60 updates a
  second on NTSC: gameplay, the scrolling and the stage map redraw every
  field. The cut scenes that change more slowly are hand-drawn cels. So the
  60 FPS target is met by the game itself, and interpolation seldom has
  anything to do. It looks ahead only once a picture has been held, so it
  costs almost nothing in play.
* **Cartoon HD / Cartoon HD+ (F8).** The 384x240 picture is enlarged by an
  edge-directed xBR shader directly on the GPU (`runtime/render_d3d11.c` on
  Windows, `runtime/render_gl.c` on Linux / SteamOS; with a CPU fallback in
  `runtime/cartoon.c` for `--dump` / software mode). Cartoon HD turns pixel
  staircases into smooth outlines and keeps flat fills flat; Cartoon HD+ adds
  bolder ink lines and an adaptive sharpen. The presenter's own filters (F10)
  apply on top.

On a 16-core machine, a field with everything on takes about 12 ms (HD+) of
the 16.7 ms available. Headless runs keep run-ahead and interpolation off so
that test runs compare plain fields; `--runahead N` and
`--interpolation on|off` turn them on.

## The disc

*The Apprentice* is a "CD-i Ready" disc:
* **Track 1 pregap:** the CD-i track (programs, graphics, ADPCM music) sits
  in the pregap of track 1.
* **CD-DA tracks:** the remaining 22 tracks are Red Book audio. They are the
  game's soundtrack, played with SS_CDDA and also playable on an audio CD
  player.

The CHD stores the pregap as audio, byte-swapped. `tools/chd.py` swaps it back,
so the `.bin` has the CD-i label at sector 16, as the tools expect.
`python tools/chd.py image.chd --verify` checks the decoder against the raw
SHA-1 in the CHD header.

The program files are in `/CMDS`:

| File | Module(s) | What it is |
|------|-----------|------------|
| `cdi_loader` | `cdi_loader` | Boot program (hand-written assembler). Reads the CSD and the disc's table of contents, sets up the display, publishes it in the data module `cdi_config`, then loads and forks `cdi_applNN` in turn. Each program's exit code picks the next one. |
| `cdi_appl01` | `cdi_philips` | Philips logo |
| `cdi_appl02` | `cdi_factory`, `cdi_vbltrap` | "Philips Interactive Media" logo |
| `cdi_appl03` | `cdi_app`, `cdi_vbltrap`, `cdi_sysproc` | The game |
| `cdi_appl04` | `cdi_invaders` | A hidden Space Invaders game |
| `cdi_start` | `cdi_start` (subroutine module) | Start-up and memory set-up code shared with `cdi_app`. It copies itself into RAM and runs from there, so it is interpreted. |
| `cdi_app.stb` | data module | Microware symbol table for `cdi_app`. The analyzer uses it for entry points and function names. |

## game.json

```json
{
  "title": "The Apprentice",
  "exe_name": "apprentice",
  "tv": "ntsc",
  "modules": [
    {"file": "CMDS/cdi_loader", "base": "0x13F000", "interpret": true},
    {"file": "CMDS/cdi_appl01", "base": "0x100000"},
    {"file": "CMDS/cdi_appl02", "base": "0x104000", "extra_entries": ["0x105506", "..."], "skip": ["cdi_vbltrap"]},
    {"file": "CMDS/cdi_appl03", "base": "0x108000", "skip": ["cdi_vbltrap"], "stb_skip": ["routine_list", "..."]},
    {"file": "CMDS/cdi_appl04", "base": "0x10E000", "extra_entries": ["..."]},
    {"file": "CMDS/cdi_start", "base": "0x114000"}
  ]
}
```

* Each entry is a **file**. Every code module in it is recompiled at
  `base + offset in the file`, and the runtime loads the whole file at `base`
  when the game loads it.
* The first entry is the program the runtime starts.
* **`interpret`**: load the file at `base` without recompiling it. The
  loader mixes data into its code; it runs once and is tiny.
* **`skip`**: modules in a file that are not recompiled. The two copies of
  `cdi_vbltrap` are emulated in C.
* **`stb_skip`**: names in the `.stb` symbol table that label data inside
  the code, so they are not entry points.
* **`invincibility`**: `{"life_byte": "0x108c6d"}`, the address of the
  player's life byte that F5 protects (see [Enhancements](#enhancements)).
* **`extra_entries`**: code addresses the analyzer misses. These are
  function pointers to routines without a prologue. The runtime logs any it
  finds (`note: code at ...` in `saves/recomp.log`) and runs them in the
  interpreter meanwhile.

`python tools/analyze.py N` writes a disassembly of the modules of entry `N`
to `build/listing_<module>.asm`, with the names from the `.stb` symbol table.

## What this game needed

These are the changes compared with the template. The details are in
[docs/CDI_NOTES.md](docs/CDI_NOTES.md).

**Disc and modules**
* **CHD images** (`tools/chd.py`), including the CD-i Ready layout.
* **The extractor:** every file on the disc has file number 1. The extractor
  therefore tells real-time files apart by their sector subheaders, and skips
  CD-DA entries.
* **Files with several modules,** program-start subroutine modules, Microware
  `.stb` symbol tables and an interpreted main program.

**Kernel**
* **Concurrent processes** (from the Zelda port): the loader waits for each
  application. The game runs a system-state "thread": `cdi_sysproc` returns
  into the game's own code with the game's static storage. It runs at
  priority 65535 and is woken every field through an event.
* **The kernel calls the game makes:** F$Fork with priority and system state,
  F$Wait ended by an A$Wake alarm, F$Event, F$DatMod (the loader's
  `cdi_config`), F$SetSys (a system-globals block), F$GPrDsc, I$ChgDir.
  F$SRtMem rejects bad blocks, as the real kernel does; the game frees a
  directory buffer with size and address swapped.

**Trap libraries**
* **`cio` (trap #13)** is the Microware C I/O library from the player's ROM,
  emulated in `runtime/cio.c`: `sprintf`, `open`, `read`, `create`, `lseek`
  and so on.
* **`cdi_vbltrap` (trap #9)** patches the kernel and the video driver so that
  a routine runs on every vertical blank. The runtime calls that routine at
  each field instead.

**Video and CD**
* **Display tables in RAM.** The game finds its line control table's address
  through a link instruction and writes MCD212 display instructions straight
  into memory, in 64-byte segments per line. The renderer now runs the
  display control program from emulated RAM.
* **CD-DA:** the TOC (raw `/cd@` reads and SetStat $3A), directory paths,
  SS_CDDA playback mixed into the audio output, and SS_Abort completing the
  status block.

**Audio:** the streaming soundmap player from the Zelda port, including
SD_Loop. The game mixes its music and sound effects into one looping
soundmap.

## Checking the translation

Headless runs are deterministic. Run the same session twice, once with
`--interp` (everything interpreted), and compare the frame dumps:

```
apprentice.exe --headless --frames 20000 --dump 500 --fuzz 7
apprentice.exe --headless --frames 20000 --dump 500 --fuzz 7 --interp
```

These all produced identical frames:
* the intro, title and attract mode;
* scripted and random play through game over and the high-score table;
* "Exit Game";
* the hidden invaders game.

### Debug options

| Option | Effect |
|--------|--------|
| `--headless --frames N --dump K` | Run without a window and write every Kth field to `saves/frame_*.bmp` (`tools/bmp2png.py` converts them) |
| `--trace` / `--trace3` | Log system calls (with callers), signals and disc activity |
| `--script "field:key:dur,..."` | Scripted input (keys `1 2 L R U D`) |
| `--fuzz SEED` | Random input |
| `--wav out.wav` | Record the audio |
| `--interp` | Interpret everything (reference for comparisons) |
| `--poke ADDR:BYTE` | Write a byte after the loader is loaded. For example, `--poke 13f05f:4` starts the hidden invaders game. |
| `--runahead N`, `--interpolation on\|off`, `--upscale hd\|hdplus\|off`, `--invincible` | The enhancements, as on the keys |
| `--dump-from N` | No frame dumps before field N |
| `--watch ADDR` | Log every change of a longword made by interpreted code, with the pc |
| `--ramdump-at F1,F2,...` | Write the RAM at those fields to `saves/ram_NNNNNN.bin` |
| `--callprof FIELD` | Profile interpreted calls from that field on; the top targets go to the log |
| `--script` keys `S N O I` | Press F2, F3, F4 or F5 at that field |
| `--no-update` | No update check this run (headless runs never check) |

## Layout

| Path | Contents |
|------|----------|
| `tools/chd.py` | CHD v5 reader and `.bin`/`.cue` converter (LZMA, deflate, FLAC, CD ECC) |
| `tools/cdi_extract.py` | Green Book file system reader |
| `tools/m68k.py`, `tools/os9mod.py` | 68000 decoder, OS-9 module headers |
| `tools/analyze.py` | Code discovery: recursive descent plus switch tables, routine tables, unrolled loops, `.stb` symbols |
| `tools/gen.py` | 68000 to C generator |
| `runtime/os9.c` | OS-9 kernel: processes, signals, events, alarms, modules, paths, `cdi_vbltrap` |
| `runtime/cio.c` | The `cio` trap library |
| `runtime/cdfm.c` | CD file manager: files, directories, the raw device and TOC, real-time play, CD-DA |
| `runtime/video.c` | UCM display control and the MCD212 renderer |
| `runtime/audio.c` | ADPCM soundmaps, real-time audio, CD-DA mixing |
| `runtime/frontend.c` | Between fields: hotkeys, quicksaves, invincibility, run-ahead, interpolation, presentation |
| `runtime/state.c` | Machine snapshots and quicksave files |
| `runtime/cartoon.c`, `runtime/motion.c` | Cartoon HD / HD+ upscaler fallback, motion interpolation (from the Laser Lords port) |
| `runtime/host_win32.c`, `runtime/render_d3d11.c` | Windows windowing, GUI launcher & Quick Menu, XInput, audio, Direct3D 11 GPU xBR presenter |
| `runtime/host_linux.c`, `runtime/render_gl.c` | Linux / SteamOS windowing, GUI launcher & Quick Menu, gamepad, audio, OpenGL 3 GPU xBR presenter |
| `runtime/update.c` | Update check and installer (GitHub releases) |
| `tools/embed_tools.py` | Embeds `tools/*.py`, `runtime/recomp.h`, and `game.json` into the binary |
| `package.sh`, `package.bat`, `tools/package.ps1` | Release packagers producing the single `.AppImage` (Linux) and single `.exe` (Windows) |
| `VERSION` | The version, compiled into the programs |
| `docs/CDI_NOTES.md` | CD-i and CD-RTOS facts worked out during the ports |
