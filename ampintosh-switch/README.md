# Ampintosh for Nintendo Switch (Homebrew)

A from-scratch C++ reimagining of **KiwiSingh/Ampintosh** for the Nintendo
Switch homebrew scene, built with **devkitPro / libnx + SDL2**.

This build scans configured music folders, decodes **MP3 / FLAC / WAV /
AIFF-AIFC PCM** with bundled/small decoders, streams float PCM through SDL2,
runs a live FFT, and draws macOS-Ampintosh-inspired visualizers and glassy deck
chrome.

## What's new in this patch

- SMB MP3/FLAC/WAV now use async libsmb2 streaming: Ampintosh prebuffers a bounded 10% into a decoded PCM ring, starts playback, then continues filling the ring in the background.
- Added SMB seek support for MP3/FLAC/WAV streams, including touchscreen seek and left-stick scrubbing.
- Last.fm now-playing/scrobble integration remains configured from `[lastfm]` in `ampintosh.ini`.

- Replaced the flat library list with a real multi-folder file browser.
  It starts at library roots, enters folders with **B**, and includes `..` to go up.
- Fixed directory playback so the visible browser page never limits the actual
  queue. A folder with more than eight tracks now queues every supported file.
- Added directory-aware queueing: playback finishes the current scanned folder,
  then rolls into the next scanned folder instead of looping the same visible set.
- Added view toggling with **-**: browser → player if a track is already loaded,
  and player → browser without stopping playback.
- Startup now opens into a parent source menu with **Local files**, **Network**, and **USB** before the file browser.
- Fixed browser/queue discovery so WAV/WAVE files and MP3s in later folders are shown even if the startup recursive scan missed them. Selecting a local track refreshes that folder into the playback queue before loading.
- Added in-app skin hotswapping with **ZL/ZR**.

- Fixed Unicode/special-character filesystem discovery regression. The browser/scanner now
  accepts positive `dirent` directory/file hints first, then falls back to
  `stat()`/`opendir()` probes. This keeps folders such as
  `PSYCHIC LOVER 〜PSYCHIC SELECTION vol.1〜` visible while still recognizing
  Japanese WAV filenames like `タギルチカラ.wav`.
- Extension detection now works on full UTF-8 paths and also recognizes
  fullwidth/ideographic Japanese dot separators before extensions.
  The player UI appears only after selecting a track/stream inside one of those sources.
- Controls are labelled with the real Nintendo Switch face buttons:
  **B = Play**, **A = mode**, **Y = visualizer**, **X = rescan**, **ZL/ZR = skin**. The D-pad and left stick both navigate lists; holding the left stick up/down accelerates through long folders.
- Playback has a dedicated now-playing layout with:
  - a fixed album-art well
  - a fixed bottom controls strip
  - a separate progress/time strip
  - ellipsized long titles so controls no longer collide with metadata
- Album art support was added through `SDL2_image`:
  - embedded MP3 ID3v2 APIC cover art
  - embedded FLAC `METADATA_BLOCK_PICTURE` cover art
  - embedded MP3 ID3, FLAC Vorbis-comment, and WAV INFO/ID3 text metadata
  - sidecar `cover/folder/front/album` `.jpg/.jpeg/.png` images
- The Switch UI now borrows more directly from the macOS app:
  - gradient/scanline background
  - translucent glass-style panels
  - monospaced deck header
  - status/format chips
  - skin palettes from the desktop version
  - macOS-style visualizer names and flow
- Default visualizer is now **Fractal**, matching the desktop app's default.
- Added the desktop skin names to `ampintosh.ini`:
  - `ampintosh`
  - `fruit_studio`
  - `live_session`
  - `orange_black`
  - `digital_tamer`
  - `octohub`
  - `space_cowboy`
  - `music_glass`
- Added **AIFF / AIFC uncompressed PCM** playback in addition to MP3, FLAC, and WAV.
- Replaced the small five-visualizer set with the desktop-inspired set:
  - Spectrum
  - Mirror Bars
  - Oscilloscope
  - Fractal
  - Orbit
  - Rings
  - Tunnel
  - Radial Burst
  - Lissajous
  - Bloom
  - Particles
  - Spectro Fall
- Retains the previous patch's `ampintosh.ini`, USB, network, Shuffle, Repeat One,
  Repeat All, and non-autoplay behavior.

## Format support

Core audio formats supported by this Switch build:

| Format | Extensions | Notes |
| ------ | ---------- | ----- |
| MP3 | `.mp3` | decoded with `dr_mp3` |
| FLAC | `.flac` | decoded with `dr_flac` |
| WAV | `.wav`, `.wave` | decoded with `dr_wav` |
| AIFF / AIFC PCM | `.aif`, `.aiff`, `.aifc` | uncompressed `NONE` and `sowt` PCM |

The macOS app can lean on AVFoundation and the desktop OS for broader codec
coverage. The Switch build intentionally avoids pretending to be full foobar2000:
AAC/ALAC/Opus/Vorbis/APE/WavPack/etc. need additional codec libraries before they
can be supported safely. The scanner only includes formats this build can really
decode.

## Quick start

The included script installs the toolchain dependencies, including
`switch-sdl2_image` for cover art, and builds the NRO:

```bash
./install.sh
```

Optional features are still opt-in:

```bash
./install.sh --usb        # USB drives via libusbhsfs
./install.sh --net        # network file streams via switch-curl
./install.sh --usb --net  # both
```

`--build` no longer blindly assumes optional dependencies already exist. If you
run `./install.sh --build --usb --net`, the script now verifies libusbhsfs and
switch-curl first, and installs/builds any missing requested optional dependency
before invoking `make`.

Manual build equivalents:

```bash
make
make USB=1
make NET=1
make USB=1 NET=1
```

If you built or installed the **GPL** libusbhsfs variant instead of the ISC
variant, use:

```bash
make USB=1 USB_BUILD_TYPE=GPL
```

## Install & run

1. Copy `Ampintosh.nro` to `sdmc:/switch/Ampintosh/Ampintosh.nro`.
2. Copy the included `ampintosh.ini` to `sdmc:/switch/Ampintosh/ampintosh.ini`.
3. Put music in any folder listed in the INI, for example `sdmc:/ampintosh/`.
4. Launch Ampintosh from hbmenu.

If no INI exists, Ampintosh falls back to sensible defaults:

- `sdmc:/ampintosh`
- `sdmc:/music`
- `usb:/ampintosh`
- `usb:/music`
- `usb:/Music`

The default startup behavior is: scan the configured folders/streams, open the
**Source Menu**, and wait for you to choose **Local files**, **Network**, or
**USB**. No song is decoded or played until you enter a source and press **B** on
a selected song/stream. Local files shows non-USB configured roots, USB shows
`usb:/...` / mounted USB roots, and Network opens stream entries. Inside Local or
USB, the browser lets you enter folders and shows `..` to go up one directory;
at the source root, `.. Source Menu` returns to the parent menu. The browser may
show only one page of rows at a time, but the playback queue is built from the
full directory, not from the visible rows.

## ampintosh.ini

Ampintosh checks these locations in order and uses the first one it finds:

```text
sdmc:/ampintosh/ampintosh.ini
sdmc:/ampintosh.ini
sdmc:/switch/Ampintosh/ampintosh.ini
sdmc:/switch/ampintosh/ampintosh.ini
```

Example:

```ini
[Main]
startpath = sdmc:/music
enabled_extensions = .mp3,.flac,.wav,.wave,.aif,.aiff,.aifc,.m3u,.m3u8
showhidden = false

[library]
recursive = true
max_depth = 8
max_tracks = 4096
music_dir = sdmc:/ampintosh
music_dir = sdmc:/music
music_dir = usb:/ampintosh
music_dir = usb:/music
music_dir = usb:/Music

[usb]
enabled = true
wait_ms = 1400

[network]
enabled = true
timeout_sec = 25
# HTTP/SFTP and unsupported remote formats still use this cache. SMB MP3/FLAC/WAV streams progressively.
cache_dir = sdmc:/ampintosh
stream_list = sdmc:/ampintosh/streams.txt
stream_list = usb:/ampintosh/streams.txt
stream_url = https://example.com/song.mp3

[NAS Music]
server = 192.168.1.14
type = smb
username = parthasarathi
password = your_password_here
port = 445
path = Music/song.flac

[SFTP Music]
server = example.com
type = sftp
username = user
password = your_password_here
port = 22
path = /home/user/Music/song.mp3

[lastfm]
enabled = false
api_key =
api_secret =
session_key =
now_playing = true
scrobble = true
scrobble_percent = 50
min_seconds = 30
default_artist =

[playback]
autoplay = false
mode = shuffle

[ui]
visualizer = fractal
skin = ampintosh
```

### Last.fm

Last.fm support is optional and only active in `NET=1` builds. Add a `[lastfm]`
section with `enabled = true`, your `api_key`, `api_secret`, and a pre-generated
Last.fm `session_key`. Ampintosh signs `track.updateNowPlaying` and
`track.scrobble` requests locally and sends them on a background SDL thread so
playback/UI are not blocked.

Scrobbling occurs after `min_seconds` and the configured `scrobble_percent` of
the track have elapsed. For files without embedded tags, Ampintosh derives the
track name from the filename and artist/album from the parent folders; use
`default_artist` as a fallback for flat folders.

### INI notes

- `music_dir` can be repeated.
- `music_dir` accepts `sdmc:/...`, explicit USB paths like `ums0:/Music`, or the
  wildcard `usb:/Music`.
- `usb:/...` expands to every mounted libusbhsfs volume returned by
  `usbHsFsListMountedDevices()`.
- `stream_list` points at a text file containing one `http://`, `https://`,
  `sftp://`, or `smb://` URL per line. Lines beginning with `#` are ignored.
- `stream_url` adds direct one-off network files without a stream list.
- NXMP-style network sections are accepted: any non-core section with
  `server`, `type`, optional `username`, optional `password`, optional `port`,
  and `path` appears under the Network source menu.
- `[Main] startpath` is accepted as another local root.
- `[Main] enabled_extensions` is parsed for NXMP config compatibility, but the
  player only queues formats with real built-in decoders.
- `mode` accepts `shuffle`, `repeat_one`, or `repeat_all`.
- `visualizer` accepts `spectrum`, `mirror`, `scope`, `fractal`, `orbit`,
  `rings`, `tunnel`, `radial`, `lissajous`, `bloom`, `particles`, or `matrix`.
- `skin` accepts the desktop-inspired skins listed above.

## Directory playback behavior

Ampintosh now separates the source menu, the visible file browser, and the
playback queue. The parent menu chooses Local files, Network, or USB. The browser
inside Local/USB behaves like a small NXMilk-style file manager: configured
source roots first, folders next, playable files inside, and `..` for parent
navigation.

For playback, Ampintosh tracks the parent folder for every scanned entry. When
you start a file, Shuffle mode builds a shuffled bag from **all playable files in
that same folder**. Once that bag is exhausted, playback automatically moves to
the next scanned folder and shuffles that folder. Repeat All uses directory order
too: it walks through the current folder first, then moves into the next folder.

This means the browser can stay compact and paged without limiting playback to
the rows currently on screen or to the first music directory that was found.


### Unicode and Japanese filenames

Ampintosh treats local paths as UTF-8 byte strings and does not try to normalize
or transliterate them. Directory scanning now uses a robust path classifier
instead of trusting `dirent::d_type`, which is unreliable on some Switch
fsdev/libusbhsfs/FAT/exFAT paths. This specifically helps folders and files with
characters such as `〜`, kana/kanji, and fullwidth punctuation.

The scanner recognizes ordinary ASCII extensions like `.mp3` and `.wav`, plus
Japanese-style fullwidth/ideographic dot separators before the extension, such as
`曲名．wav` or `曲名。WAV`.

## USB drives

Build with `--usb` or `make USB=1`.

USB support uses libusbhsfs. The app initializes libusbhsfs only if `[usb]`
`enabled = true`, waits `wait_ms`, then expands all `usb:/...` paths from the INI
against the currently mounted USB volumes.

Press **X** after plugging in or removing a drive to rescan.

The bundled install script builds the ISC libusbhsfs variant by default. That is
safest for licensing and supports FAT/exFAT. If you need NTFS/ext support, install
the GPL libusbhsfs build yourself and build Ampintosh with `USB_BUILD_TYPE=GPL`.

If `make USB=1` says `usbhsfs.h` or `libusbhsfs.a` is missing, run:

```bash
./install.sh --build --usb
```

The script will clone/build DarkMatterCore/libusbhsfs and install it into
`$DEVKITPRO/portlibs/switch`.

## Network files

Build with `--net` or `make NET=1`.

Network entries are treated as downloadable audio files, not infinite live radio
streams. Ampintosh downloads the selected URL into `[network] cache_dir`, then
plays it through the same decoder path as local music.

Direct URL streams support `http://`, `https://`, `sftp://`, and `smb://` URLs
when the Switch libcurl build has the matching protocol enabled. NXMP-style
network bookmarks are parsed from arbitrary sections such as `[NAS]` or
`[SSH Test]` with `server`, `type`, `username`, `password`, `port`, and `path`.

Supported network file URL formats are **MP3 / FLAC / WAV / AIFF**. M3U/M3U8
playlists are parsed when they point at those decoder-backed formats. If a URL
has no clear extension, Ampintosh assumes MP3.

SMB/SFTP directory browsing is implemented when the optional `libssh2` and
`libsmb2` Switch portlibs are available. NXMP-style bookmark sections appear in
the Network menu; pressing **B** opens the remote folder, shows subfolders and
decoder-backed tracks, and `..` navigates upward. Direct remote file playback is
still cached locally before decode, just like URL streams.

## Controls

### Source menu

| Input | Action |
| ----- | ------ |
| `↑` / `↓` | Move between Local files / Network / USB |
| `B` | Open highlighted source |
| `X` | Rescan library, USB devices, and stream lists |
| `-` | Toggle back to the player if a track is loaded; otherwise return to source menu |
| `+` | Quit back to hbmenu |

### Browser screen

| Input | Action |
| ----- | ------ |
| `↑` / `↓` | Move through folders/files |
| `L` / `R` | Page up / page down |
| `B` | Open highlighted folder / go up on `..` / play highlighted song |
| `A` | Cycle Shuffle / Repeat One / Repeat All |
| `Y` | Cycle visualizers |
| `X` | Rescan library, USB devices, and stream lists |
| `-` | Toggle back to the player if a track is loaded; otherwise return to source menu |
| `+` | Quit back to hbmenu |

### Player screen

| Input | Action |
| ----- | ------ |
| `B` | Play / pause |
| `A` | Cycle Shuffle / Repeat One / Repeat All |
| `Y` | Cycle visualizers |
| `X` | Rescan library, USB devices, and stream lists |
| `L` / `R` | Previous / next track |
| `-` | Toggle to the browser/source context without stopping playback |
| `+` | Quit back to hbmenu |
| Left stick ←/→ | Scrub through the track |
| Touchscreen | Tap or drag along the lower progress area to seek |

## Project layout

```text
ampintosh-switch/
├── ampintosh.ini      # sample SD-card config
├── install.sh         # one-shot toolchain install + build
├── Makefile           # adapted from the switchbrew SDL2 template
├── Ampintosh.jpg      # launcher icon, converted from the macOS source art
├── README.md
├── romfs/
│   ├── font.ttf
│   └── FONT-LICENSE.txt
└── source/
    ├── main.cpp
    ├── dr_mp3.h
    ├── dr_flac.h
    └── dr_wav.h
```

## Notes & limitations

- The whole selected track is decoded into RAM up front. This keeps playback
  simple and glitch-resistant, but extremely long files will use more memory.
- Network playback downloads the full file first.
- Embedded cover art is currently parsed for MP3 and FLAC. Text metadata is parsed for MP3, FLAC, and WAV. WAV/AIFF still get the
  reserved art area and can use sidecar images such as `cover.jpg` or
  `folder.png`.
- USB support is scoped to this homebrew app; it is not system-wide USB storage.
- If a configured root cannot be opened, it is skipped instead of aborting the
  whole library scan.
- AIFF/AIFC support is PCM-only. Compressed AIFC files are skipped safely.
- Broader foobar-style codec coverage will require adding actual codec libraries;
  do not just whitelist extensions unless there is a real decoder behind them.

## Font license

The bundled `romfs/font.ttf` is **JetBrains Mono**, licensed under the SIL Open
Font License 1.1. See `romfs/FONT-LICENSE.txt`.

### Filesystem note

On Switch/libnx, `dirent.d_type`, `stat()`, and `opendir()` can disagree on
FAT/exFAT or USB paths, especially under folders with Japanese/fullwidth
characters. Ampintosh therefore treats decoder-backed audio extensions as files
before consulting filesystem type hints. This keeps paths like
`PSYCHIC LOVER 〜PSYCHIC SELECTION vol.1〜/タギルチカラ.wav` visible in the browser.



### 1.3.10 INI precedence / fallback roots fix

- Prefer `sdmc:/ampintosh/ampintosh.ini` over stale copies in `sdmc:/switch/ampintosh/`.
- Keep default local/USB library roots visible even when an INI defines `startpath` or `music_dir`.
- Add `use_default_roots = false` for users who want the INI roots to be strict.

### 1.3.9 Unicode directory enumeration fix

Ampintosh now uses libnx native filesystem directory reads as a fallback to stdio `opendir()`/`readdir()`. This fixes folders that are visible in the browser but appear empty when their names or child filenames contain Japanese text, combining marks, wave-dash characters, or other UTF-8 edge cases.

### 1.4.5 host CMake fallback

- Wrapped Homebrew installs as `HOMEBREW_NO_AUTO_UPDATE=1 brew ...` so dependency setup does not trigger a brew auto-update on macOS.
- If Homebrew has no CMake bottle, the installer now tries `brew install --build-from-source cmake`.
- If Homebrew still cannot provide CMake, the installer downloads a portable Kitware CMake binary into `.ampintosh-deps/cmake-host/` and uses that for the optional libsmb2/libssh2 source builds.
- Missing optional remote-browser build tools now skip SMB/SFTP source builds with a warning instead of killing the whole Ampintosh build.
- Bumped NRO metadata version to 1.4.5.

### 1.4.2 SMB/SFTP browser
- Added optional libssh2-backed SFTP directory browsing for NXMP-style network bookmarks.
- Added optional libsmb2-backed SMB directory browsing for NXMP-style network bookmarks.
- Network bookmarks now open into folders instead of placeholder entries when the portlibs are present.
- Bumped NRO metadata version to 1.4.2.

### 1.4.0 Japanese filename / magic-sniff fix

- Added local file magic sniffing for MP3, FLAC, WAV/WAVE, and AIFF/AIFC so a track can still be recognized even if libnx/fsdev returns a weird or partially normalized UTF-8 name.
- Browser and playback now use the same local format detector instead of relying only on the filename extension.
- Added an ASCII-safe visible label fallback for Unicode filenames. The original UTF-8 path is still used for playback; the fallback only prevents CJK/NFD names from becoming visually invisible when the bundled font lacks Japanese glyphs.

If a Japanese filename still does not appear at all on Switch, normalize the SD-card filenames from macOS:

```bash
python3 tools/normalize_unicode_filenames.py /Volumes/SWITCH/ampintosh --dry-run
python3 tools/normalize_unicode_filenames.py /Volumes/SWITCH/ampintosh
```

This converts decomposed names such as `タギルチカラ` to NFC `タギルチカラ`, which is easier for libnx/fsdev to enumerate reliably.

### SMB/SFTP source fallback

If `./install.sh --net` cannot find `switch-libsmb2` or `switch-libssh2` in devkitPro pacman, it now falls back to building the missing portlibs from source:

- `libsmb2` from `https://github.com/sahlberg/libsmb2.git`
- `libssh2` from `https://github.com/libssh2/libssh2.git`

The source builds install into `$DEVKITPRO/portlibs/switch` and are cached under `.ampintosh-deps/` in the project folder. `switch-openssl` is installed first when libssh2 has to be built from source.

Override the cache folder with:

```bash
AMPINTOSH_DEPS_DIR=/tmp/ampintosh-portlibs ./install.sh --build --net
```



### 1.5.0 SMB streaming + Last.fm

- Added the first experimental SMB direct-streaming path. Newer builds keep it opt-in for stability.
- Added optional Last.fm now-playing and scrobbling from `[lastfm]` config.
- Streaming visualizers now analyse recent callback audio when there is no full PCM buffer.
- Bumped NRO metadata version to 1.5.0.

### 1.4.6 libsmb2 header compatibility

- Included `smb2/smb2.h` before `smb2/libsmb2.h` when SMB support is enabled so source-built libsmb2 headers expose `SMB2_GUID_SIZE` and `smb2_lease_key` correctly under C++.
- Removed an unused remote path helper that caused a warning during SMB/SFTP builds.
- Bumped NRO metadata version to 1.4.6.

### 1.4.5 remote-build tool discovery

- Host tool discovery now checks `$AMPINTOSH_GIT`, `$AMPINTOSH_MAKE`, `$AMPINTOSH_CMAKE`, PATH, common macOS install paths, and `xcrun --find`.
- On macOS, CMake falls back to an official portable Kitware build before relying on Homebrew, which helps on prerelease macOS versions where Homebrew has no bottle.
- CMake source builds explicitly pass the discovered `make` binary via `CMAKE_MAKE_PROGRAM`.
- libusbhsfs and remote dependency source builds now use the same discovered host tool paths.
- Bumped NRO metadata version to 1.4.5.


### 1.5.1 SMB playback stability

SMB streaming is now asynchronous: a background thread reads and decodes SMB MP3/FLAC/WAV files into a PCM ring buffer, and the SDL audio callback only drains already-decoded audio.


### 1.5.2 left-stick browser navigation

- Added left-stick up/down navigation for the source menu and file browser.
- Holding the left stick farther up/down now accelerates through long SD/USB/SMB/SFTP lists, while the D-pad still works for precise one-row movement.
- Bumped NRO metadata version to 1.5.2.



### 1.5.6 SMB buffering indicator

- SMB async streaming no longer blocks the UI while the initial prebuffer fills.
- Added an on-player buffering overlay with percent progress for initial SMB buffering, rebuffering, and SMB seeks.
- Bumped NRO metadata version to 1.5.6.

### 1.5.4 Async SMB streaming + seek

- SMB MP3/FLAC/WAV playback now uses a background decoder/prefetch thread with a decoded PCM ring buffer.
- Playback starts after a bounded 10% prebuffer instead of waiting for the whole SMB file to cache.
- Added SMB stream seeking for MP3/FLAC/WAV, including touch-bar seek and left-stick scrubbing.
- Added tuning keys: `smb_streaming`, `smb_stream_prebuffer_percent`, `smb_stream_prebuffer_max_ms`, and `smb_stream_buffer_ms`.
- AIFF and unsupported remote formats still use the stable cache-first path.
- Bumped NRO metadata version to 1.5.4.

### 1.5.3 FLAC/WAV metadata + safer SMB streaming

- Ampintosh now reads embedded text metadata from MP3 ID3v2, FLAC Vorbis comments, and WAV INFO/ID3 chunks. Library labels and Last.fm scrobbles use the embedded title/artist/album when available.
- `smb_streaming = true` now uses the async PCM-ring path for MP3/FLAC/WAV. Set it to `false` to force full-cache-first SMB playback.
- Bumped NRO metadata version to 1.5.3.

### 1.5.6 - FLAC/WAV metadata hardening

- Reads FLAC Vorbis comments even when a stray ID3v2 prefix exists before `fLaC`.
- Reads WAV `LIST/INFO` and embedded ID3 chunks more robustly, including UTF-16 INFO strings.
- Reads FLAC/WAV text metadata from SMB streams without full-track caching.
- Supports embedded WAV cover art via ID3/APIC and SMB FLAC/WAV cover extraction.
