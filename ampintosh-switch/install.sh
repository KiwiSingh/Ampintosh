#!/usr/bin/env bash
#
# install.sh — set up the devkitPro toolchain, pull the libraries Ampintosh
# needs, make sure the bundled font is present, and build Ampintosh.nro.
#
# Supports macOS (incl. Apple Silicon via Rosetta) and Debian/Ubuntu Linux.
# Re-running is safe: every step checks whether it's already done.
#
# Usage:
#   ./install.sh            # install deps (if needed) and build
#   ./install.sh --build    # skip dependency setup, just run make
#   ./install.sh --clean    # make clean, then build

set -euo pipefail

# --- pretty logging --------------------------------------------------------
c_blue=$'\033[1;34m'; c_green=$'\033[1;32m'; c_yellow=$'\033[1;33m'; c_red=$'\033[1;31m'; c_off=$'\033[0m'
say()  { printf "%s==>%s %s\n" "$c_blue"  "$c_off" "$*"; }
ok()   { printf "%s ok %s %s\n" "$c_green" "$c_off" "$*"; }
warn() { printf "%swarn%s %s\n" "$c_yellow" "$c_off" "$*"; }
die()  { printf "%serr %s %s\n" "$c_red" "$c_off" "$*" >&2; exit 1; }

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

MODE="full"
WANT_USB=0
WANT_NET=0
for arg in "$@"; do
  case "$arg" in
    --build) MODE="build" ;;
    --clean) MODE="clean" ;;
    --usb)   WANT_USB=1 ;;
    --net)   WANT_NET=1 ;;
    * )      die "unknown option: $arg (use --build, --clean, --usb, --net)" ;;
  esac
done

# Font fetched if the bundled one is somehow missing (it ships in romfs/).
FONT_URL="https://raw.githubusercontent.com/google/fonts/main/ofl/jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf"

# ---------------------------------------------------------------------------
# Locate dkp-pacman (it may not be on PATH yet in this shell session).
# ---------------------------------------------------------------------------
find_dkp_pacman() {
  local c
  c="$(command -v dkp-pacman 2>/dev/null || true)"
  if [ -n "$c" ]; then echo "$c"; return; fi
  for c in /opt/devkitpro/tools/bin/dkp-pacman \
           /opt/devkitpro/pacman/bin/dkp-pacman \
           /opt/devkitpro/pacman/bin/pacman; do
    [ -x "$c" ] && { echo "$c"; return; }
  done
  echo ""
}

# Pull DEVKITPRO into this shell if a previous install already set it up.
load_devkit_env() {
  if [ -f /etc/profile.d/devkit-env.sh ]; then
    # shellcheck disable=SC1091
    source /etc/profile.d/devkit-env.sh || true
  fi
  export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
  export DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
  export DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"
  export PATH="$DEVKITPRO/tools/bin:$PATH"
}

# ---------------------------------------------------------------------------
# macOS toolchain setup
# ---------------------------------------------------------------------------
setup_macos() {
  # Rosetta 2 — the devkitA64 binaries are x86_64 only.
  if [ "$(uname -m)" = "arm64" ]; then
    if /usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null; then
      ok "Rosetta 2 present"
    else
      say "Installing Rosetta 2 (devkitA64 is x86_64)"
      softwareupdate --install-rosetta --agree-to-license || warn "Rosetta install reported an issue; continuing"
    fi
  fi

  # Xcode command line tools.
  if xcode-select -p >/dev/null 2>&1; then
    ok "Xcode command line tools present"
  else
    say "Requesting Xcode command line tools (a GUI dialog will appear)"
    xcode-select --install || true
    die "Re-run this script once the Command Line Tools finish installing."
  fi

  # devkitPro pacman.
  if [ -n "$(find_dkp_pacman)" ]; then
    ok "devkitPro pacman already installed"
  else
    say "Fetching the latest devkitPro pacman installer"
    local api url tmp
    api="https://api.github.com/repos/devkitPro/pacman/releases/latest"
    url="$(curl -fsSL "$api" | grep -o '"browser_download_url"[^,]*\.pkg"' | head -1 | sed -E 's/.*"(https[^"]+)".*/\1/')"
    [ -n "$url" ] || die "Could not find the installer URL. Install manually from https://github.com/devkitPro/pacman/releases/latest"
    tmp="$(mktemp -d)/devkitpro-pacman-installer.pkg"
    curl -fsSL "$url" -o "$tmp"
    say "Installing devkitPro pacman (sudo required)"
    sudo installer -pkg "$tmp" -target / || die "pkg install failed"
    ok "devkitPro pacman installed"
  fi
}

# ---------------------------------------------------------------------------
# Debian/Ubuntu toolchain setup
# ---------------------------------------------------------------------------
setup_debian() {
  if [ -n "$(find_dkp_pacman)" ]; then
    ok "devkitPro pacman already installed"
    return
  fi
  if ! command -v wget >/dev/null 2>&1; then
    sudo apt-get update && sudo apt-get install -y wget
  fi
  say "Installing devkitPro pacman via the official apt bootstrap (sudo required)"
  local s; s="$(mktemp)"
  wget -qO "$s" https://apt.devkitpro.org/install-devkitpro-pacman
  chmod +x "$s"
  sudo "$s" || die "devkitPro pacman bootstrap failed"
  ok "devkitPro pacman installed"
}

# ---------------------------------------------------------------------------
# Install the library packages Ampintosh links against.
# ---------------------------------------------------------------------------
install_packages() {
  load_devkit_env
  local dkp; dkp="$(find_dkp_pacman)"
  [ -n "$dkp" ] || die "dkp-pacman not found even after install; open a new terminal (or reboot) and re-run."
  local pkgs="switch-dev switch-sdl2 switch-sdl2_ttf"
  [ "$WANT_NET" = 1 ] && pkgs="$pkgs switch-curl"
  say "Installing: $pkgs"
  sudo "$dkp" -Sy --needed --noconfirm $pkgs
  ok "Libraries installed"
  [ "$WANT_USB" = 1 ] && build_libusbhsfs
}

# libusbhsfs isn't a prebuilt package — build the ISC variant (FAT/exFAT, no
# GPL deps) from source straight into the portlibs dir.
build_libusbhsfs() {
  load_devkit_env
  if [ -f "$DEVKITPRO/portlibs/switch/lib/libusbhsfs.a" ]; then
    ok "libusbhsfs already installed"; return
  fi
  command -v git >/dev/null 2>&1 || die "git is required to build libusbhsfs"
  local d; d="$(mktemp -d)/libusbhsfs"
  say "Building libusbhsfs (ISC build) from source"
  git clone --depth 1 https://github.com/DarkMatterCore/libusbhsfs "$d"
  ( cd "$d" && make BUILD_TYPE=ISC install ) || die "libusbhsfs build failed"
  ok "libusbhsfs installed"
}

# ---------------------------------------------------------------------------
# Make sure the bundled font is present.
# ---------------------------------------------------------------------------
ensure_font() {
  if [ -s romfs/font.ttf ]; then
    ok "Bundled font present"
  else
    say "Fetching bundled font (JetBrains Mono, OFL)"
    mkdir -p romfs
    curl -fsSL "$FONT_URL" -o romfs/font.ttf || die "font download failed"
    ok "Font downloaded to romfs/font.ttf"
  fi
}

# ---------------------------------------------------------------------------
# Make sure the 256x256 launcher icon (Ampintosh.jpg) is present. If it's
# missing but a source image is, convert it with sips (macOS, built in) or
# ImageMagick (Linux). The Makefile references Ampintosh.jpg, so it must exist.
# ---------------------------------------------------------------------------
ensure_icon() {
  if [ -s Ampintosh.jpg ]; then
    ok "Launcher icon present (Ampintosh.jpg)"
    return
  fi
  local src=""
  for cand in AmpintoshIcon.png AmpintoshIcon.jpg icon.png icon.jpg; do
    [ -f "$cand" ] && { src="$cand"; break; }
  done
  [ -n "$src" ] || die "No launcher icon. Add a 256x256 Ampintosh.jpg (or a square source named AmpintoshIcon.png) to the project root."

  say "Converting $src -> Ampintosh.jpg (256x256)"
  if command -v sips >/dev/null 2>&1; then
    sips -s format jpeg -z 256 256 "$src" --out Ampintosh.jpg >/dev/null
  elif command -v magick >/dev/null 2>&1; then
    magick "$src" -resize 256x256\! -quality 92 Ampintosh.jpg
  elif command -v convert >/dev/null 2>&1; then
    convert "$src" -resize 256x256\! -quality 92 Ampintosh.jpg
  else
    die "Need 'sips' (macOS) or ImageMagick to convert $src. Install one, or supply a 256x256 Ampintosh.jpg directly."
  fi
  [ -s Ampintosh.jpg ] || die "Icon conversion produced no output."
  ok "Icon ready (Ampintosh.jpg)"
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
build() {
  load_devkit_env
  [ -n "${DEVKITPRO:-}" ] && [ -d "$DEVKITPRO" ] || die "DEVKITPRO is not set/usable. Open a new terminal (or reboot) and re-run."
  command -v make >/dev/null 2>&1 || die "make not found on PATH"

  if [ "$MODE" = "clean" ]; then
    say "make clean"; make clean || true
  fi
  local extra=""
  [ "$WANT_USB" = 1 ] && extra="$extra USB=1"
  [ "$WANT_NET" = 1 ] && extra="$extra NET=1"
  say "Building Ampintosh.nro$extra"
  make $extra
  if [ -f Ampintosh.nro ]; then
    ok "Built $(pwd)/Ampintosh.nro ($(du -h Ampintosh.nro | cut -f1 | tr -d ' '))"
    echo
    echo "Next: copy Ampintosh.nro to sdmc:/switch/Ampintosh/Ampintosh.nro,"
    echo "drop .mp3/.flac/.wav files in sdmc:/ampintosh/, and launch via hbmenu."
  else
    die "Build finished but Ampintosh.nro was not produced — check the output above."
  fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
if [ "$MODE" != "build" ]; then
  case "$(uname -s)" in
    Darwin) setup_macos ;;
    Linux)
      if command -v apt-get >/dev/null 2>&1; then setup_debian
      else die "Unsupported Linux distro for auto-install. See https://devkitpro.org/wiki/devkitPro_pacman, then re-run with --build."
      fi ;;
    *) die "Unsupported OS: $(uname -s). Install devkitPro manually, then re-run with --build." ;;
  esac
  install_packages
fi

ensure_font
ensure_icon
build
