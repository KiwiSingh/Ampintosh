# Ampintosh for Nintendo Switch (Homebrew)

A from-scratch C++ reimagining of the macOS Ampintosh player for the Nintendo
Switch homebrew scene, built with **devkitPro / libnx + SDL2**. It keeps the
spirit of the desktop app: decode audio, run a real FFT over the live signal,
and render a frequency-reactive spectrum.

This is original homebrew that produces a `.nro`, the format loaded by the
Homebrew Menu (hbmenu). It requires a homebrew-capable console to run.

## What it does

It scans your SD card for audio files, decodes the selected track to PCM with
the [dr_libs](https://github.com/mackron/dr_libs) family (bundled, public
domain) — **MP3, FLAC, and WAV** — streams it through SDL2's audio device, and
on every frame runs a 1024-point Hann-windowed radix-2 FFT over the playing
audio. The magnitudes are bucketed into 32 log-spaced bands and drawn as
smoothed spectrum bars in the Ampintosh palette, with a play/pause glyph, a
track strip, a scrubable progress bar, and a now-playing readout (track title,
format, and elapsed/total time) drawn with SDL2_ttf using a bundled font.

## Quick start

The included script installs the whole toolchain (devkitPro + libraries) and
builds the NRO in one go. On macOS or Debian/Ubuntu:

```bash
./install.sh
```

Re-running is safe — each step is skipped if already done. Use `./install.sh
--build` to skip dependency setup and just rebuild, or `--clean` for a clean
build. Optional features are opt-in flags (they pull extra dependencies):

```bash
./install.sh --net        # network streams (installs switch-curl)
./install.sh --usb        # USB drives (builds libusbhsfs from source)
./install.sh --usb --net  # both
```

If you'd rather set things up by hand, read on.

## Prerequisites (manual setup)

Install devkitPro and the packages this project links against (see the
[devkitPro getting started guide](https://devkitpro.org/wiki/Getting_Started)):

```bash
sudo dkp-pacman -S switch-dev switch-sdl2 switch-sdl2_ttf
```

`switch-dev` provides devkitA64 + libnx; `switch-sdl2` pulls in SDL2 and its
Mesa/EGL dependencies; `switch-sdl2_ttf` adds the font renderer (and freetype).
On Apple Silicon you also need Rosetta 2 (`softwareupdate --install-rosetta`),
since the devkitA64 binaries are x86_64. Make sure `DEVKITPRO` is exported in
your shell (the installer adds this to your profile; you may need to reboot or
open a new terminal).

## Build

From the project root:

```bash
make
```

That produces `Ampintosh.nro` with the title/author/version baked into its NACP
metadata, and the font packed into its RomFS. `make clean` removes the build
artifacts. The Makefile resolves SDL2_ttf's link chain (freetype, libpng, etc.)
via the toolchain's `pkg-config`, so library ordering is handled automatically.

Optional features are off by default; enable them on the make line (after
installing their dependencies): `make USB=1`, `make NET=1`, or `make USB=1 NET=1`.

To give it a custom launcher icon, the project ships `Ampintosh.jpg` (256×256)
and the Makefile points at it. To swap it, either replace that file with your own
256×256 JPEG, or drop a square `AmpintoshIcon.png` in the project root and run
`./install.sh` — the script converts it to `Ampintosh.jpg` for you (via `sips`
on macOS, ImageMagick on Linux).

## Install & run

1. Copy `Ampintosh.nro` to your SD card, e.g. `sdmc:/switch/Ampintosh/Ampintosh.nro`.
2. Put your music in `sdmc:/ampintosh/` — **subfolders are scanned recursively**,
   so whole album/artist folders are fine. (If that folder is empty/missing it
   also looks in `sdmc:/music/`.)
3. Launch the Homebrew Menu on your console and start Ampintosh.

If no tracks are found you'll get an idle bar pattern and a hint instead of a
crash. Press **Y** any time to rescan (handy after plugging in a USB drive).

### USB drives (built with `--usb` / `USB=1`)

Attach a FAT32/exFAT-formatted USB drive and put music in `<drive>/ampintosh/`,
`<drive>/music/`, or `<drive>/Music/`. Press **Y** to rescan if you attach it
after launch. (The ISC build of libusbhsfs covers FAT/exFAT; NTFS/ext need the
GPL build.)

### Network streams (built with `--net` / `NET=1`)

Create `sdmc:/ampintosh/streams.txt` with one URL per line (`#` for comments)
pointing at audio **files** (`.mp3`/`.flac`/`.wav`) on the web. They appear in
the playlist tagged `STREAM`; selecting one downloads it, then plays it through
the normal pipeline. This handles hosted files, not continuous live radio
(which is usually an endless AAC stream — out of scope here).

## Controls

| Input | Action |
| ----- | ------ |
| `+`            | Quit back to hbmenu |
| `A`            | Play / pause |
| `L` / `R`      | Previous / next track |
| `Y`            | Rescan library (SD + USB + streams) |
| Left stick ←/→ | Scrub through the track (speed scales with how far you push) |
| Touchscreen    | Tap or drag along the lower part of the screen to seek (handheld mode) |

## Notes & limitations

- **MP3, FLAC, and WAV** are supported, dispatched by file extension. Adding
  more formats is a matter of dropping in another single-header decoder and
  extending `fmtForName` / `decodeToF32`.
- The whole track is decoded into RAM up front (simple and glitch-free for a
  player; a few minutes of stereo float is tens of MB, which is fine on the
  Switch's memory). Streaming decode would be the move for very long files.
- The audio device is re-opened per track so mono / 48 kHz / odd-rate files play
  at the correct pitch and channel layout.
- Touch seeking only works in handheld mode (docked has no touchscreen); the
  analog stick works in both.
- This is a clean-room reimplementation, not a recompile of the SwiftUI app —
  none of AVFoundation / Accelerate / AppKit exists on libnx, so the audio
  engine, FFT, and UI were rewritten in portable C++.

## Project layout

```
ampintosh-switch/
├── install.sh        # one-shot toolchain install + build
├── Makefile          # adapted from the switchbrew SDL2 template
├── Ampintosh.jpg     # 256x256 launcher icon (baked into the NACP)
├── README.md
├── romfs/            # packed into the .nro
│   ├── font.ttf      # JetBrains Mono (OFL) — bundled now-playing font
│   └── FONT-LICENSE.txt
└── source/
    ├── main.cpp      # player + FFT analyzer + SDL2/TTF renderer
    ├── dr_mp3.h      # bundled single-header decoders (public domain)
    ├── dr_flac.h
    └── dr_wav.h
```

## Font license

The bundled `romfs/font.ttf` is **JetBrains Mono**, licensed under the SIL Open
Font License 1.1 (see `romfs/FONT-LICENSE.txt`). It's redistributable; if you
swap in your own font, make sure it's one you're allowed to ship.
