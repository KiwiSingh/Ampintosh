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
  local pkgs="switch-dev switch-sdl2 switch-sdl2_ttf switch-sdl2_image"
  [ "$WANT_NET" = 1 ] && pkgs="$pkgs switch-curl"
  say "Installing: $pkgs"
  sudo "$dkp" -Sy --needed --noconfirm $pkgs
  ok "Libraries installed"
  if [ "$WANT_NET" = 1 ]; then
    install_optional_remote_packages || true
  fi
  [ "$WANT_USB" = 1 ] && build_libusbhsfs
}

portlibs_prefix() {
  load_devkit_env
  echo "$DEVKITPRO/portlibs/switch"
}

pkgconf_switch() {
  load_devkit_env
  echo "$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config"
}

switch_pkg_exists() {
  local pc; pc="$(pkgconf_switch)"
  [ -x "$pc" ] && "$pc" --exists "$@" 2>/dev/null
}

switch_cmake_toolchain() {
  load_devkit_env
  local c
  for c in "$DEVKITPRO/cmake/Switch.cmake" \
           "$DEVKITPRO/cmake/NintendoSwitch.cmake" \
           "$DEVKITPRO/cmake/Platform/NintendoSwitch.cmake"; do
    [ -f "$c" ] && { echo "$c"; return 0; }
  done
  return 1
}

host_jobs() {
  if command -v nproc >/dev/null 2>&1; then nproc;
  elif command -v sysctl >/dev/null 2>&1; then sysctl -n hw.ncpu 2>/dev/null || echo 4;
  else echo 4; fi
}

brew_no_auto_update() {
  HOMEBREW_NO_AUTO_UPDATE=1 brew "$@"
}

find_xcrun_tool() {
  [ "$(uname -s)" = "Darwin" ] || return 1
  command -v xcrun >/dev/null 2>&1 || return 1
  local c
  c="$(xcrun --find "$1" 2>/dev/null || true)"
  [ -n "$c" ] && [ -x "$c" ] && { echo "$c"; return 0; }
  return 1
}

find_host_git() {
  local c
  if [ -n "${AMPINTOSH_GIT:-}" ] && [ -x "$AMPINTOSH_GIT" ]; then echo "$AMPINTOSH_GIT"; return 0; fi
  c="$(command -v git 2>/dev/null || true)"; [ -n "$c" ] && [ -x "$c" ] && { echo "$c"; return 0; }
  for c in /usr/bin/git /opt/homebrew/bin/git /usr/local/bin/git /opt/local/bin/git; do
    [ -x "$c" ] && { echo "$c"; return 0; }
  done
  find_xcrun_tool git && return 0
  return 1
}

find_host_make() {
  local c
  if [ -n "${AMPINTOSH_MAKE:-}" ] && [ -x "$AMPINTOSH_MAKE" ]; then echo "$AMPINTOSH_MAKE"; return 0; fi
  c="$(command -v make 2>/dev/null || true)"; [ -n "$c" ] && [ -x "$c" ] && { echo "$c"; return 0; }
  for c in /usr/bin/make /opt/homebrew/bin/gmake /usr/local/bin/gmake /opt/local/bin/gmake /opt/homebrew/bin/make /usr/local/bin/make /opt/local/bin/make; do
    [ -x "$c" ] && { echo "$c"; return 0; }
  done
  find_xcrun_tool make && return 0
  return 1
}

find_host_cmake() {
  local c
  if [ -n "${AMPINTOSH_CMAKE:-}" ] && [ -x "$AMPINTOSH_CMAKE" ]; then
    echo "$AMPINTOSH_CMAKE"; return 0
  fi
  c="$(command -v cmake 2>/dev/null || true)"
  if [ -n "$c" ] && [ -x "$c" ]; then echo "$c"; return 0; fi
  for c in \
    /Applications/CMake.app/Contents/bin/cmake \
    "$HOME/Applications/CMake.app/Contents/bin/cmake" \
    /opt/homebrew/bin/cmake \
    /usr/local/bin/cmake \
    /opt/local/bin/cmake \
    "$PROJECT_DIR/.ampintosh-deps/cmake-host/CMake.app/Contents/bin/cmake" \
    "$PROJECT_DIR/.ampintosh-deps/cmake-host/bin/cmake"; do
    [ -x "$c" ] && { echo "$c"; return 0; }
  done
  find_xcrun_tool cmake && return 0
  if [ -d "$PROJECT_DIR/.ampintosh-deps/cmake-host" ]; then
    c="$(find "$PROJECT_DIR/.ampintosh-deps/cmake-host" \( -path '*/CMake.app/Contents/bin/cmake' -o -path '*/bin/cmake' \) -type f -perm -111 2>/dev/null | head -1 || true)"
    [ -n "$c" ] && { echo "$c"; return 0; }
  fi
  return 1
}

remember_host_git() {
  local c
  c="$(find_host_git || true)"
  [ -n "$c" ] || return 1
  export AMPINTOSH_GIT="$c"
  export PATH="$(dirname "$c"):$PATH"
  return 0
}

remember_host_make() {
  local c
  c="$(find_host_make || true)"
  [ -n "$c" ] || return 1
  export AMPINTOSH_MAKE="$c"
  export PATH="$(dirname "$c"):$PATH"
  return 0
}

remember_host_cmake() {
  local c
  c="$(find_host_cmake || true)"
  [ -n "$c" ] || return 1
  export AMPINTOSH_CMAKE="$c"
  export PATH="$(dirname "$c"):$PATH"
  return 0
}

install_kitware_cmake_binary() {
  [ "$(uname -s)" = "Darwin" ] || return 1
  command -v curl >/dev/null 2>&1 || return 1
  command -v tar >/dev/null 2>&1 || return 1

  local deps tmp api url cm arch_pat
  deps="${AMPINTOSH_DEPS_DIR:-$PROJECT_DIR/.ampintosh-deps}/cmake-host"
  mkdir -p "$deps"

  if remember_host_cmake; then
    return 0
  fi

  api="https://api.github.com/repos/Kitware/CMake/releases/latest"
  case "$(uname -m)" in
    arm64|aarch64) arch_pat='(universal|arm64)' ;;
    *)             arch_pat='(universal|x86_64|x86)' ;;
  esac

  url="$(curl -fsSL "$api" \
    | grep -Eo 'https://[^\"]+cmake-[^\"]+macos[^\"]+\.tar\.gz' \
    | grep -Ei "$arch_pat" \
    | head -1 || true)"
  if [ -z "$url" ]; then
    url="$(curl -fsSL "$api" \
      | grep -Eo 'https://[^\"]+cmake-[^\"]+Darwin[^\"]+\.tar\.gz' \
      | head -1 || true)"
  fi
  [ -n "$url" ] || { warn "Could not find a downloadable Kitware CMake macOS tarball"; return 1; }

  tmp="$(mktemp -d)"
  say "Downloading portable CMake from Kitware"
  curl -fL "$url" -o "$tmp/cmake.tar.gz" || return 1
  rm -rf "$deps"
  mkdir -p "$deps"
  tar -xzf "$tmp/cmake.tar.gz" -C "$deps" --strip-components=1 || return 1

  cm="$(find "$deps" \( -path '*/CMake.app/Contents/bin/cmake' -o -path '*/bin/cmake' \) -type f -perm -111 2>/dev/null | head -1 || true)"
  if [ -z "$cm" ]; then
    warn "Portable CMake downloaded but no cmake binary was found under $deps"
    return 1
  fi
  export AMPINTOSH_CMAKE="$cm"
  export PATH="$(dirname "$cm"):$PATH"
  ok "Portable CMake ready ($cm)"
}

install_missing_host_tools_macos() {
  local missing=("$@")
  [ "${#missing[@]}" -eq 0 ] && return 0

  local normal_missing=()
  local need_cmake=0
  local t
  for t in "${missing[@]}"; do
    case "$t" in
      git)   remember_host_git >/dev/null 2>&1 || normal_missing+=(git) ;;
      make)  remember_host_make >/dev/null 2>&1 || normal_missing+=(make) ;;
      cmake) remember_host_cmake >/dev/null 2>&1 || need_cmake=1 ;;
      *)     normal_missing+=("$t") ;;
    esac
  done

  # CMake is the tool Homebrew most often fails to provide on prerelease macOS.
  # Try an official portable Kitware build before asking Homebrew for a bottle.
  if [ "$need_cmake" = 1 ] && ! remember_host_cmake >/dev/null 2>&1; then
    install_kitware_cmake_binary || true
  fi

  if [ "${#normal_missing[@]}" -gt 0 ]; then
    if command -v brew >/dev/null 2>&1; then
      say "Installing host build tools with Homebrew: ${normal_missing[*]}"
      brew_no_auto_update install "${normal_missing[@]}" || warn "Homebrew failed to install: ${normal_missing[*]}"
    else
      warn "Missing host tools: ${normal_missing[*]}; Homebrew is not installed"
    fi
  fi

  if [ "$need_cmake" = 1 ] && ! remember_host_cmake >/dev/null 2>&1; then
    if command -v brew >/dev/null 2>&1; then
      say "Installing host CMake with Homebrew"
      if ! brew_no_auto_update install cmake; then
        warn "Homebrew could not install a CMake bottle; trying --build-from-source"
        brew_no_auto_update install --build-from-source cmake || warn "Homebrew CMake source build failed"
      fi
    fi
    if ! remember_host_cmake >/dev/null 2>&1; then
      warn "Homebrew did not provide CMake; trying portable Kitware CMake instead"
      install_kitware_cmake_binary || true
    fi
  fi

  remember_host_git >/dev/null 2>&1 || return 1
  remember_host_make >/dev/null 2>&1 || return 1
  remember_host_cmake >/dev/null 2>&1 || return 1
  return 0
}
ensure_cmake_build_tools() {
  local missing=()
  remember_host_git >/dev/null 2>&1 || missing+=(git)
  remember_host_cmake >/dev/null 2>&1 || missing+=(cmake)
  remember_host_make >/dev/null 2>&1 || missing+=(make)
  [ "${#missing[@]}" -eq 0 ] && { ok "Host build tools present"; return 0; }

  case "$(uname -s)" in
    Darwin)
      install_missing_host_tools_macos "${missing[@]}" || {
        warn "Missing host build tools after PATH/xcrun/Homebrew/portable-CMake search: ${missing[*]}."
        return 1
      }
      ;;
    Linux)
      if command -v apt-get >/dev/null 2>&1; then
        say "Installing host build tools with apt: ${missing[*]}"
        sudo apt-get update && sudo apt-get install -y "${missing[@]}" || {
          warn "apt failed to install required host build tools: ${missing[*]}"
          return 1
        }
      else
        warn "Missing host build tools: ${missing[*]}. Install git, cmake, and make for your distro."
        return 1
      fi
      ;;
    *) warn "Missing host build tools: ${missing[*]}"; return 1 ;;
  esac

  remember_host_cmake >/dev/null 2>&1 || { warn "CMake still was not found after host-tool setup"; return 1; }
  remember_host_git >/dev/null 2>&1 || { warn "git still was not found after host-tool setup"; return 1; }
  remember_host_make >/dev/null 2>&1 || { warn "make still was not found after host-tool setup"; return 1; }
  ok "Host build tools present: git=$AMPINTOSH_GIT cmake=$AMPINTOSH_CMAKE make=$AMPINTOSH_MAKE"
}
clone_or_update_source() {
  local url="$1" dir="$2" git_bin
  git_bin="$(find_host_git || true)"
  [ -n "$git_bin" ] || { warn "git not found; cannot clone $url"; return 1; }
  mkdir -p "$(dirname "$dir")"
  if [ -d "$dir/.git" ]; then
    say "Updating $(basename "$dir")"
    "$git_bin" -C "$dir" fetch --depth 1 origin || true
    "$git_bin" -C "$dir" pull --ff-only || true
  else
    rm -rf "$dir"
    say "Cloning $url"
    "$git_bin" clone --depth 1 "$url" "$dir"
  fi
}

cmake_install_build() {
  local build_dir="$1" prefix="$2" cmake_bin
  cmake_bin="$(find_host_cmake || true)"
  [ -n "$cmake_bin" ] || { warn "cmake not found; cannot build $build_dir"; return 1; }
  "$cmake_bin" --build "$build_dir" --parallel "$(host_jobs)"
  if [ -w "$prefix" ] || [ -w "$(dirname "$prefix")" ]; then
    "$cmake_bin" --install "$build_dir"
  else
    say "Installing into $prefix (sudo required)"
    sudo env DEVKITPRO="$DEVKITPRO" DEVKITARM="$DEVKITARM" DEVKITPPC="$DEVKITPPC" PATH="$PATH" "$cmake_bin" --install "$build_dir"
  fi
}

try_dkp_install_one() {
  local pkg="$1"
  local dkp; dkp="$(find_dkp_pacman)"
  [ -n "$dkp" ] || return 1
  say "Trying devkitPro package: $pkg"
  sudo "$dkp" -Sy --needed --noconfirm "$pkg"
}

build_libsmb2_from_source() {
  load_devkit_env
  switch_pkg_exists libsmb2 && { ok "libsmb2 already present"; return 0; }
  ensure_cmake_build_tools || return 1

  local toolchain prefix deps src build pc cmake_bin make_bin
  toolchain="$(switch_cmake_toolchain)" || { warn "Could not find devkitPro Switch CMake toolchain under $DEVKITPRO/cmake"; return 1; }
  prefix="$(portlibs_prefix)"
  deps="${AMPINTOSH_DEPS_DIR:-$PROJECT_DIR/.ampintosh-deps}"
  src="$deps/libsmb2"
  build="$deps/build-libsmb2-switch"
  pc="$(pkgconf_switch)"
  cmake_bin="$(find_host_cmake || true)"
  make_bin="$(find_host_make || true)"
  [ -n "$cmake_bin" ] || { warn "cmake not found after host-tool setup"; return 1; }
  [ -n "$make_bin" ] || { warn "make not found after host-tool setup"; return 1; }

  clone_or_update_source "https://github.com/sahlberg/libsmb2.git" "$src"
  rm -rf "$build"
  say "Configuring libsmb2 for Nintendo Switch"
  "$cmake_bin" -S "$src" -B "$build" \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$prefix;$DEVKITPRO/libnx" \
    -DPKG_CONFIG_EXECUTABLE="$pc" \
    -G "Unix Makefiles" \
    -DCMAKE_MAKE_PROGRAM="$make_bin" \
    -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_EXAMPLES=OFF \
    -DENABLE_LIBKRB5=OFF \
    -DENABLE_GSSAPI=OFF
  say "Building libsmb2"
  cmake_install_build "$build" "$prefix" || return 1
  switch_pkg_exists libsmb2 || { warn "libsmb2 source install finished, but libsmb2.pc was not found by $pc"; return 1; }
  ok "libsmb2 installed from source"
}

build_libssh2_from_source() {
  load_devkit_env
  switch_pkg_exists libssh2 && { ok "libssh2 already present"; return 0; }
  ensure_cmake_build_tools || return 1

  local dkp; dkp="$(find_dkp_pacman)"
  if [ -n "$dkp" ] && ! switch_pkg_exists libcrypto; then
    say "Installing switch-openssl for libssh2 crypto"
    sudo "$dkp" -Sy --needed --noconfirm switch-openssl || warn "Could not install switch-openssl automatically; libssh2 source build may fail"
  fi

  local toolchain prefix deps src build pc cmake_bin make_bin
  toolchain="$(switch_cmake_toolchain)" || { warn "Could not find devkitPro Switch CMake toolchain under $DEVKITPRO/cmake"; return 1; }
  prefix="$(portlibs_prefix)"
  deps="${AMPINTOSH_DEPS_DIR:-$PROJECT_DIR/.ampintosh-deps}"
  src="$deps/libssh2"
  build="$deps/build-libssh2-switch"
  pc="$(pkgconf_switch)"
  cmake_bin="$(find_host_cmake || true)"
  make_bin="$(find_host_make || true)"
  [ -n "$cmake_bin" ] || { warn "cmake not found after host-tool setup"; return 1; }
  [ -n "$make_bin" ] || { warn "make not found after host-tool setup"; return 1; }

  clone_or_update_source "https://github.com/libssh2/libssh2.git" "$src"
  rm -rf "$build"
  say "Configuring libssh2 for Nintendo Switch"
  "$cmake_bin" -S "$src" -B "$build" \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$prefix;$DEVKITPRO/libnx" \
    -DPKG_CONFIG_EXECUTABLE="$pc" \
    -G "Unix Makefiles" \
    -DCMAKE_MAKE_PROGRAM="$make_bin" \
    -DOPENSSL_ROOT_DIR="$prefix" \
    -DCRYPTO_BACKEND=OpenSSL \
    -DBUILD_STATIC_LIBS=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTING=OFF \
    -DLIBSSH2_BUILD_DOCS=OFF \
    -DENABLE_ZLIB_COMPRESSION=OFF
  say "Building libssh2"
  cmake_install_build "$build" "$prefix" || return 1
  switch_pkg_exists libssh2 || { warn "libssh2 source install finished, but libssh2.pc was not found by $pc"; return 1; }
  ok "libssh2 installed from source"
}

install_optional_remote_packages() {
  load_devkit_env
  local pc; pc="$(pkgconf_switch)"
  [ -x "$pc" ] || return 0

  if switch_pkg_exists libssh2 libsmb2; then
    ok "Remote browser dependencies present (libssh2 + libsmb2)"
    return 0
  fi

  # Some devkitPro feeds have libssh2, but libsmb2 is often missing. Install
  # package candidates individually so one missing target does not abort all.
  if ! switch_pkg_exists libssh2; then
    try_dkp_install_one switch-libssh2 || warn "switch-libssh2 package unavailable; falling back to source build"
  fi
  if ! switch_pkg_exists libsmb2; then
    try_dkp_install_one switch-libsmb2 || \
    try_dkp_install_one switch-smb2 || \
    warn "No devkitPro libsmb2 package found; falling back to source build"
  fi

  # Source fallback. These are optional; normal HTTP/direct URL streaming still
  # works through libcurl if either source build fails.
  if ! switch_pkg_exists libsmb2; then
    build_libsmb2_from_source || warn "libsmb2 source build failed; SMB browser will stay disabled"
  fi
  if ! switch_pkg_exists libssh2; then
    build_libssh2_from_source || warn "libssh2 source build failed; SFTP browser will stay disabled"
  fi

  if switch_pkg_exists libssh2 libsmb2; then
    ok "Remote browser dependencies present (libssh2 + libsmb2)"
  else
    warn "Remote browser deps are incomplete: SMB=$(switch_pkg_exists libsmb2 && echo on || echo off), SFTP=$(switch_pkg_exists libssh2 && echo on || echo off). Ampintosh will still build with the available network features."
  fi
}

# libusbhsfs isn't a devkitPro package — build the ISC variant
# (FAT/exFAT, no GPL deps) from source straight into the portlibs dir.
have_libusbhsfs() {
  load_devkit_env
  [ -f "$DEVKITPRO/portlibs/switch/include/usbhsfs.h" ] && \
  [ -f "$DEVKITPRO/portlibs/switch/lib/libusbhsfs.a" ]
}

build_libusbhsfs() {
  load_devkit_env
  if have_libusbhsfs; then
    ok "libusbhsfs already installed"; return
  fi
  remember_host_git >/dev/null 2>&1 || die "git is required to build libusbhsfs"
  remember_host_make >/dev/null 2>&1 || die "make is required to build libusbhsfs"
  local d make_bin; d="$(mktemp -d)/libusbhsfs"; make_bin="$(find_host_make)"
  say "Building libusbhsfs (ISC build) from source"
  "$AMPINTOSH_GIT" clone --depth 1 https://github.com/DarkMatterCore/libusbhsfs "$d"
  if [ -w "$DEVKITPRO" ] || [ -w "$DEVKITPRO/portlibs" ] || [ -w "$DEVKITPRO/portlibs/switch" ]; then
    ( cd "$d" && "$make_bin" BUILD_TYPE=ISC install ) || die "libusbhsfs build failed"
  else
    say "Installing libusbhsfs into $DEVKITPRO/portlibs/switch (sudo required)"
    ( cd "$d" && sudo env DEVKITPRO="$DEVKITPRO" DEVKITARM="$DEVKITARM" DEVKITPPC="$DEVKITPPC" PATH="$PATH" "$make_bin" BUILD_TYPE=ISC install ) || die "libusbhsfs build failed"
  fi
  have_libusbhsfs || die "libusbhsfs install completed but usbhsfs.h/libusbhsfs.a were not found under $DEVKITPRO/portlibs/switch"
  ok "libusbhsfs installed"
}

ensure_requested_features() {
  load_devkit_env

  # --build still needs the core SDL packages this project now links against.
  if "$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config" --exists SDL2 SDL2_ttf SDL2_image 2>/dev/null; then
    ok "Core SDL dependencies present"
  else
    local dkp; dkp="$(find_dkp_pacman)"
    [ -n "$dkp" ] || die "Missing SDL2/TTF/Image portlibs and dkp-pacman was not found. Re-run without --build or install switch-sdl2 switch-sdl2_ttf switch-sdl2_image manually."
    say "Core SDL dependencies missing; installing switch-sdl2 switch-sdl2_ttf switch-sdl2_image"
    sudo "$dkp" -Sy --needed --noconfirm switch-sdl2 switch-sdl2_ttf switch-sdl2_image || die "SDL dependency install failed"
    "$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config" --exists SDL2 SDL2_ttf SDL2_image 2>/dev/null || die "SDL dependencies installed but pkg-config still cannot find SDL2/TTF/Image"
    ok "Core SDL dependencies installed"
  fi

  if [ "$WANT_USB" = 1 ]; then
    if have_libusbhsfs; then
      ok "USB dependency present (libusbhsfs)"
    else
      say "USB requested but libusbhsfs is missing; installing it now"
      build_libusbhsfs
    fi
  fi

  if [ "$WANT_NET" = 1 ]; then
    if "$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config" --exists libcurl 2>/dev/null; then
      ok "Network dependency present (switch-curl)"
    else
      dkp="$(find_dkp_pacman)"
      [ -n "$dkp" ] || die "NET=1 requires switch-curl, but dkp-pacman was not found. Re-run ./install.sh --net without --build, or install switch-curl manually."
      say "Network requested but switch-curl is missing; installing it now"
      sudo "$dkp" -Sy --needed --noconfirm switch-curl || die "switch-curl install failed"
      "$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config" --exists libcurl 2>/dev/null || die "switch-curl installed but libcurl.pc was not found"
      ok "switch-curl installed"
    fi
    if "$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config" --exists libssh2 libsmb2 2>/dev/null; then
      ok "Remote browser dependencies present (libssh2 + libsmb2)"
    else
      install_optional_remote_packages || true
    fi
  fi
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
  remember_host_make >/dev/null 2>&1 || die "make not found on PATH or via xcrun"

  if [ "$MODE" = "clean" ]; then
    say "make clean"; "$AMPINTOSH_MAKE" clean || true
  fi
  local extra=""
  [ "$WANT_USB" = 1 ] && extra="$extra USB=1"
  [ "$WANT_NET" = 1 ] && extra="$extra NET=1"
  say "Building Ampintosh.nro$extra"
  "$AMPINTOSH_MAKE" $extra
  if [ -f Ampintosh.nro ]; then
    ok "Built $(pwd)/Ampintosh.nro ($(du -h Ampintosh.nro | cut -f1 | tr -d ' '))"
    echo
    echo "Next: copy Ampintosh.nro to sdmc:/switch/Ampintosh/Ampintosh.nro,"
    echo "copy ampintosh.ini to sdmc:/switch/Ampintosh/ampintosh.ini,"
    echo "drop .mp3/.flac/.wav files in a configured folder, and launch via hbmenu."
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

ensure_requested_features
ensure_font
ensure_icon
build
