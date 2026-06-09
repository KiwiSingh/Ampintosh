// Ampintosh for Nintendo Switch (homebrew / NRO)
// ------------------------------------------------
// A from-scratch C++ reimagining of the macOS Ampintosh player for the
// devkitPro / libnx + SDL2 toolchain. It keeps the spirit of the original:
// decode audio, run a real FFT over the playing signal, and draw several
// frequency-reactive visualizers.
//
// Configure library roots, USB roots, NXMP-style network bookmarks, stream
// lists, autoplay, playback mode, and the default visualizer from ampintosh.ini.

#include <switch.h>
#define SDL_MAIN_HANDLED          // we provide main() ourselves; don't let SDL rename it
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <map>
#include <deque>
#include <random>
#include <sstream>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <utility>
#include <vector>

#ifdef AMPINTOSH_USB
#if __has_include(<usbhsfs.h>)
#include <usbhsfs.h>          // USB mass storage (opt-in: make USB=1)
#else
#error "AMPINTOSH_USB requires libusbhsfs. Run ./install.sh --usb or install DarkMatterCore/libusbhsfs with: make BUILD_TYPE=ISC install"
#endif
#endif
#ifdef AMPINTOSH_NET
#include <curl/curl.h>        // network streams + remote file fetch (opt-in: make NET=1)
#if defined(AMPINTOSH_SFTP)
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif
#if defined(AMPINTOSH_SMB)
// Some libsmb2 installs expose dependent SMB2 constants/types (for example
// SMB2_GUID_SIZE and smb2_lease_key) from smb2.h rather than libsmb2.h.
// Include the low-level protocol header first so libsmb2.h is complete when
// compiled as C++ by the Ampintosh frontend.
#if __has_include(<smb2/smb2.h>)
#include <smb2/smb2.h>
#endif
#include <smb2/libsmb2.h>
#ifndef SMB2_NEGOTIATE_SIGNING_ENABLED
#define SMB2_NEGOTIATE_SIGNING_ENABLED 0x0001   // standard SMB2 flag; fallback if smb2.h absent
#endif
#endif
#endif

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr int   SCREEN_W   = 1280;
static constexpr int   SCREEN_H   = 720;
static constexpr int   FFT_SIZE   = 1024;          // power of two
static constexpr int   HALF_SIZE  = FFT_SIZE / 2;
static constexpr int   NUM_BANDS  = 32;
static constexpr float PI_F       = 3.14159265358979323846f;

// Ampintosh palette (0xRRGGBB). The macOS app has several skins; this
// Switch port keeps the same named palettes and draws them with lightweight
// SDL primitives.
struct RGB { Uint8 r, g, b; };

struct SkinPalette {
    const char* name;
    RGB bg0, bg1, primary, secondary, tertiary, text, muted, panel, display;
};

enum class SkinMode {
    Ampintosh, FruitStudio, LiveSession, OrangeBlack, DigitalTamer,
    OctoHub, SpaceCowboy, MusicGlass
};

static constexpr SkinPalette SKINS[] = {
    { "Ampintosh",     {0x0c,0x11,0x18}, {0x1f,0x2a,0x36}, {0xa8,0xff,0x5f}, {0xff,0xcf,0x5f}, {0x73,0xff,0xe1}, {0xea,0xf6,0xff}, {0x9f,0xb8,0xd6}, {0x10,0x19,0x23}, {0x04,0x11,0x09} },
    { "Fruit Studio",  {0x15,0x1a,0x20}, {0x27,0x30,0x3a}, {0x9d,0xff,0x63}, {0xff,0x9b,0x45}, {0x5f,0xc7,0xff}, {0xf3,0xf7,0xec}, {0xb8,0xc3,0xb4}, {0x20,0x28,0x32}, {0x0f,0x1c,0x15} },
    { "Live Session",  {0x22,0x22,0x22}, {0x11,0x11,0x11}, {0xf8,0xf8,0xf2}, {0xff,0x6a,0x00}, {0x00,0x7a,0xff}, {0xf8,0xf8,0xf2}, {0xc8,0xc8,0xc2}, {0x2a,0x2a,0x2a}, {0x15,0x15,0x15} },
    { "Orange Black",  {0x05,0x05,0x05}, {0x25,0x15,0x05}, {0xff,0x9f,0x1c}, {0xff,0xff,0xff}, {0xff,0x5a,0x00}, {0xff,0xff,0xff}, {0xb7,0xb7,0xb7}, {0x15,0x15,0x15}, {0x09,0x09,0x09} },
    { "Digital Tamer", {0x06,0x19,0x22}, {0x32,0x11,0x32}, {0x00,0xe5,0xff}, {0xff,0x34,0x5f}, {0xff,0xd4,0x47}, {0xe8,0xfb,0xff}, {0x9a,0xcd,0xd7}, {0x10,0x24,0x36}, {0x06,0x13,0x1d} },
    { "OctoHub",       {0x0d,0x11,0x17}, {0x16,0x1b,0x22}, {0x7e,0xe7,0x87}, {0x58,0xa6,0xff}, {0xd2,0xa8,0xff}, {0xf0,0xf6,0xfc}, {0x8b,0x94,0x9e}, {0x16,0x1b,0x22}, {0x0d,0x11,0x17} },
    { "Space Cowboy",  {0x11,0x14,0x1d}, {0x3d,0x1f,0x18}, {0xff,0xd1,0x66}, {0xef,0x47,0x6f}, {0x06,0xd6,0xa0}, {0xf7,0xf7,0xff}, {0xa8,0xad,0xc0}, {0x1b,0x20,0x31}, {0x08,0x09,0x0f} },
    { "Music Glass",   {0x0b,0x10,0x1e}, {0x24,0x2d,0x44}, {0x99,0xe2,0xff}, {0xff,0xcc,0x70}, {0xc3,0x8d,0xff}, {0xf5,0xfb,0xff}, {0xb6,0xc3,0xd5}, {0x18,0x22,0x32}, {0x07,0x0d,0x18} }
};

static SkinPalette gSkin = SKINS[0];

#define COL_BG        (gSkin.bg0)
#define COL_BG2       (gSkin.bg1)
#define COL_PANEL     (gSkin.panel)
#define COL_DISPLAY   (gSkin.display)
#define COL_PRIMARY   (gSkin.primary)
#define COL_SECONDARY (gSkin.secondary)
#define COL_TERTIARY  (gSkin.tertiary)
#define COL_TEXT      (gSkin.text)
#define COL_MUTED     (gSkin.muted)

// ---------------------------------------------------------------------------
// Shared player state (audio thread <-> main thread)
// ---------------------------------------------------------------------------
// Supported source formats. Stored on the player so we free with the matching
// decoder's allocator.
enum class AudioFmt { None, Mp3, Flac, Wav, Aiff };

enum class PlaybackMode { Shuffle, RepeatOne, RepeatAll };
enum class VisualizerMode {
    Spectrum, Mirror, Scope, Fractal, Orbit, Rings, Tunnel, Radial,
    Lissajous, Bloom, Particles, Matrix
};

struct StreamDecoder;
static drmp3_uint64 streamReadFrames(StreamDecoder* s, float* out, drmp3_uint64 framesToRead);
static bool streamIsFinished(StreamDecoder* s);
static bool streamRequestSeekToFrame(StreamDecoder* s, drmp3_uint64 frame);
static void destroyStreamDecoder(StreamDecoder*& s);

struct Player {
    float*                    pcm         = nullptr; // interleaved float, owned
    drmp3_uint64              totalFrames = 0;
    int                       channels    = 2;
    int                       sampleRate  = 44100;
    AudioFmt                  fmt         = AudioFmt::None;
    SDL_AudioDeviceID         dev         = 0;       // reopened to match each track
    StreamDecoder*            stream      = nullptr; // network streaming decoder (SMB, no full-file cache)
    bool                      streaming   = false;
    float                     recentMono[FFT_SIZE] = {0}; // latest output for streaming visualizers
    int                       recentWrite = 0;
    std::atomic<drmp3_uint64> cursor{0};             // current playback frame
    std::atomic<bool>         playing{false};        // do not autoplay by default
    std::atomic<bool>         repeatOne{false};
    std::atomic<bool>         finished{false};       // edge from audio thread
};

// Pull PCM into SDL's buffer and advance the shared cursor. Runs on the audio
// thread; pcm is immutable for the lifetime of a loaded track (we close the
// device around track changes), and cursor is atomic, so no extra mutex.
static void audioCallback(void* userdata, Uint8* stream, int len) {
    Player* p = static_cast<Player*>(userdata);
    float* out = reinterpret_cast<float*>(stream);
    const int ch = p->channels;
    const int outFrames = len / (int)(sizeof(float) * ch);

    if (!p->playing.load()) {
        std::memset(stream, 0, len);
        return;
    }

    if (p->streaming && p->stream) {
        const drmp3_uint64 got = streamReadFrames(p->stream, out, (drmp3_uint64)outFrames);
        const int gotFrames = (int)got;
        for (int i = 0; i < gotFrames; ++i) {
            float mono = 0.0f;
            for (int c = 0; c < ch; ++c) mono += out[i * ch + c];
            mono /= (float)std::max(1, ch);
            p->recentMono[p->recentWrite % FFT_SIZE] = mono;
            p->recentWrite = (p->recentWrite + 1) % FFT_SIZE;
        }
        if (gotFrames < outFrames) {
            // Async SMB streaming can briefly underrun when the NAS/Wi‑Fi stalls.
            // Never do network I/O on this real-time callback; fill the gap with
            // silence and only end the track once the pump has reached EOF and
            // drained its PCM ring.
            std::memset(out + gotFrames * ch, 0, (outFrames - gotFrames) * ch * sizeof(float));
            if (streamIsFinished(p->stream)) {
                p->playing.store(false);
                p->finished.store(true);
            }
        }
        p->cursor.store(p->cursor.load() + got);
        return;
    }

    if (!p->pcm || p->totalFrames == 0) {
        std::memset(stream, 0, len);
        return;
    }

    drmp3_uint64 cur = p->cursor.load();
    for (int i = 0; i < outFrames; ++i) {
        if (cur >= p->totalFrames) {
            if (p->repeatOne.load()) {
                cur = 0;
            } else {
                for (int c = 0; c < ch; ++c) out[i * ch + c] = 0.0f;
                p->playing.store(false);
                p->finished.store(true);
                continue;
            }
        }
        float mono = 0.0f;
        for (int c = 0; c < ch; ++c) {
            out[i * ch + c] = p->pcm[cur * ch + c];
            mono += out[i * ch + c];
        }
        mono /= (float)std::max(1, ch);
        p->recentMono[p->recentWrite % FFT_SIZE] = mono;
        p->recentWrite = (p->recentWrite + 1) % FFT_SIZE;
        ++cur;
    }
    p->cursor.store(cur);
}

// ---------------------------------------------------------------------------
// Iterative radix-2 Cooley-Tukey FFT (in-place, complex).
// ---------------------------------------------------------------------------
static void fft(float* re, float* im, int n) {
    // Bit-reversal permutation.
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * PI_F / (float)len;
        const float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cwr = 1.0f, cwi = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k, b = i + k + len / 2;
                const float tr = re[b] * cwr - im[b] * cwi;
                const float ti = re[b] * cwi + im[b] * cwr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                const float ncwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr;
                cwr = ncwr;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Analyzer: window of PCM around the cursor -> log-spaced normalized bands.
// ---------------------------------------------------------------------------
struct Analyzer {
    float window[FFT_SIZE];
    int   bandEdges[NUM_BANDS + 1];

    void init(int sampleRate) {
        for (int i = 0; i < FFT_SIZE; ++i)
            window[i] = 0.5f * (1.0f - cosf(2.0f * PI_F * i / (FFT_SIZE - 1)));

        const double minHz = 32.0;
        const double maxHz = std::min(16000.0, sampleRate / 2.0);
        const double binHz = (double)sampleRate / FFT_SIZE;
        for (int b = 0; b <= NUM_BANDS; ++b) {
            const double frac = (double)b / NUM_BANDS;
            const double hz   = minHz * pow(maxHz / minHz, frac);
            int bin = (int)lround(hz / binHz);
            if (bin < 1) bin = 1;
            if (bin > HALF_SIZE - 1) bin = HALF_SIZE - 1;
            bandEdges[b] = bin;
        }
    }

    // Fills bands[NUM_BANDS] in 0..1. Returns RMS loudness in 0..1.
    float analyze(const Player& p, float* bands) {
        float re[FFT_SIZE], im[FFT_SIZE];
        const drmp3_uint64 start = p.cursor.load();
        const int ch = p.channels;

        float sumSq = 0.0f;
        for (int i = 0; i < FFT_SIZE; ++i) {
            float s = 0.0f;
            if (p.streaming) {
                // Network streams are decoded progressively, so there is no full
                // PCM buffer to look ahead into. Analyse the latest audio emitted
                // by the callback instead. This keeps visualizers alive without
                // forcing a whole-file cache.
                const int idx = (p.recentWrite + i) % FFT_SIZE;
                s = p.recentMono[idx];
            } else {
                drmp3_uint64 f = start + i;
                if (p.pcm && f < p.totalFrames) {
                    if (ch >= 2) s = 0.5f * (p.pcm[f * ch] + p.pcm[f * ch + 1]);
                    else          s = p.pcm[f * ch];
                }
            }
            sumSq += s * s;
            re[i] = s * window[i];
            im[i] = 0.0f;
        }

        fft(re, im, FFT_SIZE);

        for (int b = 0; b < NUM_BANDS; ++b) {
            const int lo = bandEdges[b];
            const int hi = std::max(bandEdges[b + 1], lo + 1);
            float sum = 0.0f; int count = 0;
            for (int bin = lo; bin < hi && bin < HALF_SIZE; ++bin) {
                const float mag = re[bin] * re[bin] + im[bin] * im[bin];
                sum += mag; ++count;
            }
            float power = (count > 0 ? sum / count : 0.0f) / (float)FFT_SIZE;
            float db = 10.0f * log10f(power + 1e-9f);
            float v  = (db + 66.0f) / 56.0f;          // ~ -66..-10 dB -> 0..1
            v = std::min(std::max(v, 0.0f), 1.0f);
            v = powf(v, 1.35f);                         // gamma for punchier peaks
            v *= 0.82f + 0.5f * (float)b / NUM_BANDS;   // gentle high-end tilt
            bands[b] = std::min(v, 1.0f);
        }

        float rms = sqrtf(sumSq / FFT_SIZE);
        return std::min(1.0f, std::max(0.0f, (20.0f * log10f(rms + 1e-7f) + 52.0f) / 46.0f));
    }
};

// ---------------------------------------------------------------------------
// Track discovery + config
// ---------------------------------------------------------------------------
struct Entry {
    std::string ref;     // file path ("sdmc:/.../x.mp3", "ums0:/.../y.flac") or URL
    std::string label;   // display name
    std::string dir;     // parent folder, used for directory-scoped playback
    bool        isURL = false;
};

struct DirectoryGroup {
    std::string path;
    int         first = 0;
    int         count = 0;
};

enum class BrowserItemKind { Up, Directory, Track, StreamFolder, Stream };
enum class SourceKind { LocalFiles, Network, Usb };


struct BrowserItem {
    BrowserItemKind kind = BrowserItemKind::Track;
    std::string     label;
    std::string     path;       // directory path, file path, URL, or special browser path
    int             entryIndex = -1;
    int             trackCount = 0;
};

static const char* BROWSER_ROOT        = "";
static const char* BROWSER_NET         = "net:/";
static const char* BROWSER_SOURCE_MENU = "source:/";
static const char* BROWSER_NETBOOKMARK = "nxmpnet:/";

enum class AppScreen { SourceMenu, Browser, Player };

struct SourceMenuItem {
    SourceKind  kind = SourceKind::LocalFiles;
    std::string label;
    std::string detail;
    int         count = 0;
    bool        available = true;
};

struct CoverArt {
    SDL_Texture* texture = nullptr;
    int          w = 0;
    int          h = 0;
    std::string  source;
};

struct NetworkBookmark {
    std::string sectionName;
    std::string type;
    std::string server;
    std::string username;
    std::string password;
    std::string path;
    int         port = 0;
};

struct AppConfig {
    std::vector<std::string> musicDirs;
    std::vector<std::string> streamLists;
    std::vector<std::string> streamURLs;
    std::vector<std::string> enabledExtensions;
    std::vector<NetworkBookmark> networkBookmarks;
    bool recursive       = true;
    bool useDefaultRoots = true;
    int  maxDepth        = 8;
    int  maxTracks       = 4096;
    bool usbEnabled      = true;
    int  usbWaitMs       = 1400;
    bool netEnabled      = true;
    int  netTimeoutSec   = 25;
    bool smbDirectStreaming = true;  // async libsmb2 streaming for MP3/FLAC/WAV; false = full cache first
    int  smbStreamBufferMs = 45000;  // decoded PCM ring size
    int  smbStreamPrebufferPercent = 10;
    int  smbStreamPrebufferMaxMs = 15000;
    std::string cacheDir = "sdmc:/ampintosh";
    bool autoplay        = false;
    PlaybackMode playbackMode = PlaybackMode::Shuffle;
    VisualizerMode visualizer = VisualizerMode::Fractal;
    SkinMode skin = SkinMode::Ampintosh;

    bool lastfmEnabled      = false;
    bool lastfmNowPlaying   = true;
    bool lastfmScrobble     = true;
    std::string lastfmApiKey;
    std::string lastfmApiSecret;
    std::string lastfmSessionKey;
    std::string lastfmDefaultArtist;
    int  lastfmScrobblePercent = 50;
    int  lastfmMinSeconds      = 30;
};

#ifdef AMPINTOSH_USB
static bool g_usbReady = false;
#endif
#ifdef AMPINTOSH_NET
static bool g_socketReady = false;
static bool g_netReady = false;
#endif

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

static std::string lowerCopy(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool startsWith(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return s.size() >= n && std::memcmp(s.data(), prefix, n) == 0;
}

static bool isHttpURL(const std::string& s) {
    const std::string l = lowerCopy(s);
    return startsWith(l, "http://") || startsWith(l, "https://");
}

static std::string redactURLCredentials(const std::string& input) {
    const std::string l = lowerCopy(input);
    size_t scheme = std::string::npos;
    for (const char* pfx : {"http://", "https://", "sftp://", "smb://"}) {
        if (startsWith(l, pfx)) { scheme = std::strlen(pfx); break; }
    }
    if (scheme == std::string::npos) return input;
    const size_t slash = input.find('/', scheme);
    const size_t at = input.find('@', scheme);
    if (at == std::string::npos || (slash != std::string::npos && at > slash)) return input;
    return input.substr(0, scheme) + "***:***@" + input.substr(at + 1);
}

static std::vector<std::string> splitList(const std::string& value) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : value) {
        if (c == ',' || c == '|') {
            cur = trim(cur);
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    cur = trim(cur);
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static void appendUnique(std::vector<std::string>& out, const std::string& v) {
    if (v.empty()) return;
    if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
}

static bool hasExplicitSchemeOrDevice(const std::string& path) {
    if (path.find("://") != std::string::npos) return true;
    const size_t dev = path.find(":/");
    return dev != std::string::npos && dev > 0;
}

static std::string canonicalLocalConfigPath(std::string path) {
    path = trim(path);
    if (path.empty()) return path;
    const std::string l = lowerCopy(path);
    if (isHttpURL(path) || startsWith(l, "sftp://") || startsWith(l, "smb://") || startsWith(l, "usb:/"))
        return path;

    // NXMP uses POSIX-ish SD-root paths such as / or /Music. Ampintosh/libnx
    // file I/O is much more reliable when those are made explicit as sdmc:/...
    // Without this, scanning can accidentally walk a process-relative root and
    // miss sibling folders such as PSYCHIC LOVER 〜PSYCHIC SELECTION vol.1〜.
    if (!hasExplicitSchemeOrDevice(path) && !path.empty() && path.front() == '/') {
        if (path == "/") return "sdmc:/";
        return std::string("sdmc:") + path;
    }
    return path;
}

static std::string normalizeExtensionToken(std::string ext) {
    ext = trim(ext);
    while (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    for (char& c : ext) {
        if ((unsigned char)c < 0x80) c = (char)std::tolower((unsigned char)c);
    }
    return ext;
}

static std::string urlEncodeComponent(const std::string& s, bool pathMode = false) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        const bool unreserved = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved || (pathMode && c == '/')) {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::string urlDecodeComponent(const std::string& s, bool plusAsSpace = false) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hexNibble(s[i + 1]);
            const int lo = hexNibble(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (plusAsSpace && s[i] == '+') out.push_back(' ');
        else out.push_back(s[i]);
    }
    return out;
}

static bool parseBool(const std::string& v, bool fallback) {
    const std::string s = lowerCopy(trim(v));
    if (s == "1" || s == "true" || s == "yes" || s == "on" || s == "enabled" || s == "auto") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off" || s == "disabled") return false;
    return fallback;
}

static PlaybackMode parsePlaybackMode(const std::string& v, PlaybackMode fallback) {
    std::string s = lowerCopy(trim(v));
    s.erase(std::remove_if(s.begin(), s.end(), [](char c){ return c == '_' || c == '-' || std::isspace((unsigned char)c); }), s.end());
    if (s == "shuffle" || s == "random") return PlaybackMode::Shuffle;
    if (s == "repeatone" || s == "one" || s == "single") return PlaybackMode::RepeatOne;
    if (s == "repeatall" || s == "all" || s == "loopall") return PlaybackMode::RepeatAll;
    return fallback;
}

static VisualizerMode parseVisualizerMode(const std::string& v, VisualizerMode fallback) {
    std::string s = lowerCopy(trim(v));
    s.erase(std::remove_if(s.begin(), s.end(), [](char c){ return c == '_' || c == '-' || std::isspace((unsigned char)c); }), s.end());
    if (s == "spectrum" || s == "bars") return VisualizerMode::Spectrum;
    if (s == "mirror" || s == "mirrorbars" || s == "pulse") return VisualizerMode::Mirror;
    if (s == "scope" || s == "oscilloscope" || s == "waveform" || s == "wave") return VisualizerMode::Scope;
    if (s == "fractal" || s == "tree") return VisualizerMode::Fractal;
    if (s == "orbit" || s == "particlesorbit") return VisualizerMode::Orbit;
    if (s == "rings" || s == "circle") return VisualizerMode::Rings;
    if (s == "tunnel" || s == "recttunnel") return VisualizerMode::Tunnel;
    if (s == "radial" || s == "radialburst") return VisualizerMode::Radial;
    if (s == "lissajous" || s == "xy") return VisualizerMode::Lissajous;
    if (s == "bloom" || s == "flower") return VisualizerMode::Bloom;
    if (s == "particles" || s == "fountain") return VisualizerMode::Particles;
    if (s == "matrix" || s == "spectrofall" || s == "waterfall") return VisualizerMode::Matrix;
    return fallback;
}

static SkinMode parseSkinMode(const std::string& v, SkinMode fallback) {
    std::string s = lowerCopy(trim(v));
    s.erase(std::remove_if(s.begin(), s.end(), [](char c){ return c == '_' || c == '-' || std::isspace((unsigned char)c); }), s.end());
    if (s == "ampintosh" || s == "classic") return SkinMode::Ampintosh;
    if (s == "fruitstudio" || s == "fruitstudio12" || s == "flstudio") return SkinMode::FruitStudio;
    if (s == "livesession" || s == "live") return SkinMode::LiveSession;
    if (s == "orangeblack" || s == "orange") return SkinMode::OrangeBlack;
    if (s == "digitaltamer" || s == "digimon") return SkinMode::DigitalTamer;
    if (s == "octohub" || s == "github") return SkinMode::OctoHub;
    if (s == "spacecowboy" || s == "cowboy") return SkinMode::SpaceCowboy;
    if (s == "musicglass" || s == "glass") return SkinMode::MusicGlass;
    return fallback;
}

static const SkinPalette& paletteForSkin(SkinMode mode) {
    switch (mode) {
        case SkinMode::FruitStudio:  return SKINS[1];
        case SkinMode::LiveSession:  return SKINS[2];
        case SkinMode::OrangeBlack:  return SKINS[3];
        case SkinMode::DigitalTamer: return SKINS[4];
        case SkinMode::OctoHub:      return SKINS[5];
        case SkinMode::SpaceCowboy:  return SKINS[6];
        case SkinMode::MusicGlass:   return SKINS[7];
        case SkinMode::Ampintosh:
        default:                     return SKINS[0];
    }
}

static SkinMode nextSkinMode(SkinMode mode, int direction = 1) {
    const int n = (int)(sizeof(SKINS) / sizeof(SKINS[0]));
    int idx = 0;
    switch (mode) {
        case SkinMode::FruitStudio:  idx = 1; break;
        case SkinMode::LiveSession:  idx = 2; break;
        case SkinMode::OrangeBlack:  idx = 3; break;
        case SkinMode::DigitalTamer: idx = 4; break;
        case SkinMode::OctoHub:      idx = 5; break;
        case SkinMode::SpaceCowboy:  idx = 6; break;
        case SkinMode::MusicGlass:   idx = 7; break;
        case SkinMode::Ampintosh:
        default:                     idx = 0; break;
    }
    idx = (idx + direction) % n;
    if (idx < 0) idx += n;
    switch (idx) {
        case 1: return SkinMode::FruitStudio;
        case 2: return SkinMode::LiveSession;
        case 3: return SkinMode::OrangeBlack;
        case 4: return SkinMode::DigitalTamer;
        case 5: return SkinMode::OctoHub;
        case 6: return SkinMode::SpaceCowboy;
        case 7: return SkinMode::MusicGlass;
        case 0:
        default: return SkinMode::Ampintosh;
    }
}

static int parseInt(const std::string& v, int fallback, int lo, int hi) {
    char* end = nullptr;
    long n = std::strtol(trim(v).c_str(), &end, 10);
    if (!end || *end != '\0') return fallback;
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    return (int)n;
}

static std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

static void addDefaultConfigLists(AppConfig& cfg) {
    // Keep the normal Ampintosh library roots as fallbacks even when an INI has
    // NXMP-style startpath/music_dir entries. This prevents an old app-folder
    // ampintosh.ini that points at one album folder from hiding sibling albums
    // in sdmc:/ampintosh. Users who really want a strict root list can set
    // use_default_roots = false.
    if (cfg.useDefaultRoots) {
        appendUnique(cfg.musicDirs, "sdmc:/ampintosh");
        appendUnique(cfg.musicDirs, "sdmc:/music");
        appendUnique(cfg.musicDirs, "usb:/ampintosh");
        appendUnique(cfg.musicDirs, "usb:/music");
        appendUnique(cfg.musicDirs, "usb:/Music");
    }
    if (cfg.streamLists.empty() || cfg.useDefaultRoots) {
        appendUnique(cfg.streamLists, "sdmc:/ampintosh/streams.txt");
        appendUnique(cfg.streamLists, "sdmc:/music/streams.txt");
        appendUnique(cfg.streamLists, "usb:/ampintosh/streams.txt");
        appendUnique(cfg.streamLists, "usb:/music/streams.txt");
        appendUnique(cfg.streamLists, "usb:/Music/streams.txt");
    }
}

static bool isCoreConfigSection(const std::string& section) {
    const std::string s = lowerCopy(trim(section));
    return s.empty() || s == "main" || s == "library" || s == "usb" ||
           s == "network" || s == "net" || s == "playback" || s == "ui" ||
           s == "last.fm" || s == "lastfm" || s == "audioscrobbler" ||
           s == "enigma2";
}

static bool networkBookmarkEquals(const NetworkBookmark& a, const NetworkBookmark& b) {
    return lowerCopy(a.sectionName) == lowerCopy(b.sectionName) &&
           lowerCopy(a.type) == lowerCopy(b.type) &&
           a.server == b.server && a.path == b.path && a.username == b.username &&
           a.port == b.port;
}

static void appendUniqueNetworkBookmark(std::vector<NetworkBookmark>& out, const NetworkBookmark& b) {
    if (b.server.empty() || b.type.empty()) return;
    for (const NetworkBookmark& existing : out) {
        if (networkBookmarkEquals(existing, b)) return;
    }
    out.push_back(b);
}

static void maybeAddNXMPNetworkBookmark(AppConfig& cfg, const std::string& rawSection,
                                        const std::map<std::string, std::string>& kv) {
    if (isCoreConfigSection(rawSection)) return;
    auto get = [&](const char* key) -> std::string {
        auto it = kv.find(key);
        return it == kv.end() ? std::string() : trim(it->second);
    };

    NetworkBookmark b;
    b.sectionName = trim(rawSection);
    b.type = lowerCopy(get("type"));
    b.server = get("server");
    b.username = get("username");
    b.password = get("password");
    b.path = get("path");
    b.port = parseInt(get("port"), 0, 0, 65535);

    // NXMP-style network sections are arbitrary section names with at least
    // `type` and `server`, for example [NAS] type=smb server=192.168.1.14.
    // Ignore empty template sections such as [Enigma2].
    if (b.type.empty() || b.server.empty()) return;
    appendUniqueNetworkBookmark(cfg.networkBookmarks, b);
}

static std::string networkBookmarkURLWithPath(const NetworkBookmark& b, const std::string& pathOverride) {
    std::string type = lowerCopy(trim(b.type));
    if (type == "ssh") type = "sftp";
    if (type == "smb2") type = "smb";
    if (type.empty()) return std::string();

    const std::string effectivePath = pathOverride.empty() ? b.path : pathOverride;

    // A user may put a complete URL in `server`; keep it and append path when useful.
    if (isHttpURL(b.server) || startsWith(lowerCopy(b.server), "sftp://") || startsWith(lowerCopy(b.server), "smb://")) {
        std::string url = b.server;
        if (!effectivePath.empty()) {
            if (!url.empty() && url.back() != '/' && effectivePath.front() != '/') url += '/';
            url += urlEncodeComponent(effectivePath, true);
        }
        return url;
    }

    if (type != "http" && type != "https" && type != "sftp" && type != "smb")
        return std::string();

    std::string url = type + "://";
    if (!b.username.empty()) {
        url += urlEncodeComponent(b.username);
        if (!b.password.empty()) url += ":" + urlEncodeComponent(b.password);
        url += "@";
    }
    url += b.server;
    if (b.port > 0) url += ":" + std::to_string(b.port);
    if (!effectivePath.empty()) {
        if (effectivePath.front() != '/') url += '/';
        url += urlEncodeComponent(effectivePath, true);
    }
    return url;
}

static std::string networkBookmarkURL(const NetworkBookmark& b) {
    return networkBookmarkURLWithPath(b, std::string());
}

static bool applyConfigKV(AppConfig& cfg, const std::string& section, const std::string& key, const std::string& value) {
    const std::string k = section.empty() ? key : (section + "." + key);
    const std::string lk = lowerCopy(k);
    const std::string v = trim(value);

    auto addValues = [&](std::vector<std::string>& target) {
        for (const std::string& item : splitList(v)) appendUnique(target, item);
    };
    auto addMusicValues = [&]() {
        for (const std::string& item : splitList(v)) appendUnique(cfg.musicDirs, canonicalLocalConfigPath(item));
    };

    if (lk == "music_dir" || lk == "music.dir" || lk == "library.dir" || lk == "library.music_dir" ||
        lk == "main.startpath" || lk == "startpath" ||
        lk == "scan_dir" || lk == "library.scan_dir" || lk == "root" || lk == "library.root") {
        addMusicValues(); return true;
    }
    if (lk == "stream_list" || lk == "network.stream_list" || lk == "streams_file" || lk == "network.streams_file") {
        addValues(cfg.streamLists); return true;
    }
    if (lk == "stream" || lk == "stream_url" || lk == "network.stream" || lk == "network.stream_url" || lk == "url" || lk == "network.url") {
        addValues(cfg.streamURLs); return true;
    }
    if (lk == "recursive" || lk == "library.recursive") {
        cfg.recursive = parseBool(v, cfg.recursive); return true;
    }
    if (lk == "use_default_roots" || lk == "default_roots" ||
        lk == "library.use_default_roots" || lk == "library.default_roots") {
        cfg.useDefaultRoots = parseBool(v, cfg.useDefaultRoots); return true;
    }
    if (lk == "max_depth" || lk == "library.max_depth") {
        cfg.maxDepth = parseInt(v, cfg.maxDepth, 0, 32); return true;
    }
    if (lk == "max_tracks" || lk == "library.max_tracks") {
        cfg.maxTracks = parseInt(v, cfg.maxTracks, 1, 20000); return true;
    }
    if (lk == "enabled_extensions" || lk == "main.enabled_extensions" ||
        lk == "library.enabled_extensions") {
        for (const std::string& item : splitList(v)) {
            const std::string ext = normalizeExtensionToken(item);
            if (!ext.empty()) appendUnique(cfg.enabledExtensions, ext);
        }
        return true;
    }
    if (lk == "usb" || lk == "usb.enabled" || lk == "usb_enabled") {
        cfg.usbEnabled = parseBool(v, cfg.usbEnabled); return true;
    }
    if (lk == "usb.wait_ms" || lk == "usb_wait_ms") {
        cfg.usbWaitMs = parseInt(v, cfg.usbWaitMs, 0, 8000); return true;
    }
    if (lk == "network" || lk == "network.enabled" || lk == "net" || lk == "net.enabled" || lk == "network_enabled") {
        cfg.netEnabled = parseBool(v, cfg.netEnabled); return true;
    }
    if (lk == "network.timeout" || lk == "network.timeout_sec" || lk == "net.timeout_sec") {
        cfg.netTimeoutSec = parseInt(v, cfg.netTimeoutSec, 3, 180); return true;
    }
    if (lk == "cache_dir" || lk == "network.cache_dir") {
        cfg.cacheDir = v; return true;
    }
    if (lk == "smb_streaming" || lk == "network.smb_streaming" || lk == "smb.async_streaming" || lk == "network.smb_async_streaming" ||
        lk == "smb.direct_streaming" || lk == "network.smb_direct_streaming") {
        cfg.smbDirectStreaming = parseBool(v, cfg.smbDirectStreaming); return true;
    }
    if (lk == "smb_stream_buffer_ms" || lk == "network.smb_stream_buffer_ms") {
        cfg.smbStreamBufferMs = parseInt(v, cfg.smbStreamBufferMs, 5000, 120000); return true;
    }
    if (lk == "smb_stream_prebuffer_percent" || lk == "network.smb_stream_prebuffer_percent") {
        cfg.smbStreamPrebufferPercent = parseInt(v, cfg.smbStreamPrebufferPercent, 1, 50); return true;
    }
    if (lk == "smb_stream_prebuffer_max_ms" || lk == "network.smb_stream_prebuffer_max_ms") {
        cfg.smbStreamPrebufferMaxMs = parseInt(v, cfg.smbStreamPrebufferMaxMs, 1000, 60000); return true;
    }
    if (lk == "autoplay" || lk == "playback.autoplay") {
        cfg.autoplay = parseBool(v, cfg.autoplay); return true;
    }
    if (lk == "playback_mode" || lk == "playback.mode" || lk == "mode") {
        cfg.playbackMode = parsePlaybackMode(v, cfg.playbackMode); return true;
    }
    if (lk == "visualizer" || lk == "ui.visualizer") {
        cfg.visualizer = parseVisualizerMode(v, cfg.visualizer); return true;
    }
    if (lk == "skin" || lk == "ui.skin" || lk == "theme" || lk == "ui.theme") {
        cfg.skin = parseSkinMode(v, cfg.skin); return true;
    }
    if (lk == "lastfm.enabled" || lk == "last.fm.enabled" || lk == "audioscrobbler.enabled") {
        cfg.lastfmEnabled = parseBool(v, cfg.lastfmEnabled); return true;
    }
    if (lk == "lastfm.api_key" || lk == "lastfm.apikey" || lk == "last.fm.api_key") {
        cfg.lastfmApiKey = v; return true;
    }
    if (lk == "lastfm.api_secret" || lk == "lastfm.secret" || lk == "last.fm.api_secret") {
        cfg.lastfmApiSecret = v; return true;
    }
    if (lk == "lastfm.session_key" || lk == "lastfm.sk" || lk == "last.fm.session_key") {
        cfg.lastfmSessionKey = v; return true;
    }
    if (lk == "lastfm.now_playing" || lk == "lastfm.update_now_playing") {
        cfg.lastfmNowPlaying = parseBool(v, cfg.lastfmNowPlaying); return true;
    }
    if (lk == "lastfm.scrobble") {
        cfg.lastfmScrobble = parseBool(v, cfg.lastfmScrobble); return true;
    }
    if (lk == "lastfm.scrobble_percent" || lk == "lastfm.percent") {
        cfg.lastfmScrobblePercent = parseInt(v, cfg.lastfmScrobblePercent, 10, 95); return true;
    }
    if (lk == "lastfm.min_seconds" || lk == "lastfm.minimum_seconds") {
        cfg.lastfmMinSeconds = parseInt(v, cfg.lastfmMinSeconds, 10, 240); return true;
    }
    if (lk == "lastfm.default_artist" || lk == "lastfm.fallback_artist") {
        cfg.lastfmDefaultArtist = v; return true;
    }
    return false;
}

static bool loadConfigFromPath(const char* path, AppConfig& cfg) {
    FILE* f = std::fopen(path, "r");
    if (!f) return false;

    char buf[2048];
    std::string section;
    std::string sectionDisplay;
    std::map<std::string, std::string> sectionKV;

    auto flushSection = [&]() {
        maybeAddNXMPNetworkBookmark(cfg, sectionDisplay.empty() ? section : sectionDisplay, sectionKV);
        sectionKV.clear();
    };

    while (std::fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            flushSection();
            sectionDisplay = trim(line.substr(1, line.size() - 2));
            section = lowerCopy(sectionDisplay);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lowerCopy(trim(line.substr(0, eq)));
        const std::string val = trim(line.substr(eq + 1));
        sectionKV[key] = val;
        applyConfigKV(cfg, section, key, val);
    }
    flushSection();
    std::fclose(f);
    return true;
}

static AppConfig loadAppConfig(std::string& loadedPath) {
    AppConfig cfg;
    const char* paths[] = {
        // Prefer the library-adjacent config. This avoids a stale copy inside
        // /switch/ampintosh from silently overriding /ampintosh/ampintosh.ini.
        "sdmc:/ampintosh/ampintosh.ini",
        "sdmc:/ampintosh.ini",
        "sdmc:/switch/Ampintosh/ampintosh.ini",
        "sdmc:/switch/ampintosh/ampintosh.ini"
    };
    for (const char* p : paths) {
        if (loadConfigFromPath(p, cfg)) {
            loadedPath = p;
            break;
        }
    }
    addDefaultConfigLists(cfg);
    return cfg;
}

static std::string trimAsciiWhitespace(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    return s;
}

static size_t findLastUtf8ExtSeparator(const std::string& s, size_t* sepLen = nullptr) {
    size_t best = s.find_last_of('.');
    size_t bestLen = (best == std::string::npos) ? 0 : 1;

    auto consider = [&](const char* token, size_t len) {
        size_t pos = s.find(token);
        while (pos != std::string::npos) {
            if (best == std::string::npos || pos > best) { best = pos; bestLen = len; }
            pos = s.find(token, pos + len);
        }
    };

    // Some Japanese/Asian filenames use fullwidth/ideographic dots before the
    // extension. Treat those as extension separators without trying to normalize
    // the rest of the UTF-8 path.
    consider("\xEF\xBC\x8E", 3); // U+FF0E FULLWIDTH FULL STOP: ．
    consider("\xE3\x80\x82", 3); // U+3002 IDEOGRAPHIC FULL STOP: 。
    consider("\xEF\xBD\xA1", 3); // U+FF61 HALFWIDTH IDEOGRAPHIC FULL STOP: ｡

    if (sepLen) *sepLen = bestLen;
    return best;
}

// Map a filename/path/URL extension to a decoder. Paths are handled as opaque
// UTF-8 bytes; only the ASCII extension is normalized. This keeps names such as
// "PSYCHIC LOVER 〜...〜" or "タギルチカラ.wav" from being mangled.
static AudioFmt fmtForName(const std::string& name) {
    std::string s = trimAsciiWhitespace(name);
    const size_t q = s.find_first_of("?#");
    if (q != std::string::npos) s = s.substr(0, q);
    s = trimAsciiWhitespace(s);

    size_t sepLen = 0;
    const size_t dot = findLastUtf8ExtSeparator(s, &sepLen);
    if (dot == std::string::npos) return AudioFmt::None;
    std::string ext = trimAsciiWhitespace(s.substr(dot + sepLen));
    for (char& c : ext) {
        if ((unsigned char)c < 0x80) c = (char)std::tolower((unsigned char)c);
    }

    if (ext == "mp3")  return AudioFmt::Mp3;
    if (ext == "flac") return AudioFmt::Flac;
    if (ext == "wav" || ext == "wave") return AudioFmt::Wav;
    if (ext == "aif" || ext == "aiff" || ext == "aifc") return AudioFmt::Aiff;
    return AudioFmt::None;
}

static bool isPlayableLocalFile(const std::string& path);

static bool isPlayableAudioFile(const std::string& name) {
    return isPlayableLocalFile(name);
}

static bool isPlayableAudioLeafName(const std::string& name) {
    // Check the raw readdir leaf as well as the full path. This is deliberately
    // redundant: it protects folders with dots/fullwidth chars in their names
    // from confusing path-level heuristics, while still accepting decomposed
    // Japanese filenames such as タギルチカラ.wav.
    return fmtForName(name) != AudioFmt::None;
}

[[maybe_unused]] static bool containsNonAscii(const std::string& s) {
    for (unsigned char c : s) if (c >= 0x80) return true;
    return false;
}

// Last error from a remote-browser listing attempt, surfaced in the UI so a
// failure is actionable instead of a generic "unavailable".
static std::string gNetLastError;

static std::string asciiFallbackLabel(const std::string& s) {
    // Previously this replaced non-ASCII names with "[unicode]" because the
    // bundled font lacked CJK glyphs. The UI now loads the Switch system font
    // (full Japanese coverage), so return the real UTF-8 label unchanged.
    return s;
}

static AudioFmt sniffAudioFmtFromFile(const std::string& path) {
    if (path.empty() || isHttpURL(path)) return AudioFmt::None;
    const std::string l = lowerCopy(path);
    if (startsWith(l, "sftp://") || startsWith(l, "smb://")) return AudioFmt::None;

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return AudioFmt::None;
    unsigned char h[16] = {0};
    const size_t n = std::fread(h, 1, sizeof(h), f);
    std::fclose(f);
    if (n >= 12 && std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WAVE", 4) == 0) return AudioFmt::Wav;
    if (n >= 4  && std::memcmp(h, "fLaC", 4) == 0) return AudioFmt::Flac;
    if (n >= 12 && std::memcmp(h, "FORM", 4) == 0 && (std::memcmp(h + 8, "AIFF", 4) == 0 || std::memcmp(h + 8, "AIFC", 4) == 0)) return AudioFmt::Aiff;
    if (n >= 3  && std::memcmp(h, "ID3", 3) == 0) return AudioFmt::Mp3;
    if (n >= 2  && h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) return AudioFmt::Mp3;
    return AudioFmt::None;
}

static AudioFmt fmtForLocalFile(const std::string& path) {
    AudioFmt f = fmtForName(path);
    if (f != AudioFmt::None) return f;
    return sniffAudioFmtFromFile(path);
}

static bool isPlayableLocalFile(const std::string& path) {
    return fmtForLocalFile(path) != AudioFmt::None;
}

static bool isPlaylistLeafName(const std::string& name) {
    std::string s = trimAsciiWhitespace(name);
    size_t sepLen = 0;
    const size_t dot = findLastUtf8ExtSeparator(s, &sepLen);
    if (dot == std::string::npos) return false;
    std::string ext = normalizeExtensionToken(s.substr(dot + sepLen));
    return ext == "m3u" || ext == "m3u8";
}

static bool isPlaylistFile(const std::string& name) {
    std::string s = trimAsciiWhitespace(name);
    const size_t q = s.find_first_of("?#");
    if (q != std::string::npos) s = s.substr(0, q);
    size_t sepLen = 0;
    const size_t dot = findLastUtf8ExtSeparator(s, &sepLen);
    if (dot == std::string::npos) return false;
    std::string ext = normalizeExtensionToken(s.substr(dot + sepLen));
    return ext == "m3u" || ext == "m3u8";
}

// Filename/URL tail without directory or extension, for the now-playing label.
static std::string displayName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t q = base.find_first_of("?#");
    if (q != std::string::npos) base = base.substr(0, q);
    const size_t dot = findLastUtf8ExtSeparator(base);
    if (dot != std::string::npos) base = base.substr(0, dot);
    return base;
}

static std::string directoryName(const std::string& path) {
    if (isHttpURL(path)) return "Network Streams";
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

static bool isBrowserRoot(const std::string& path) {
    return path.empty();
}

static bool isBrowserNet(const std::string& path) {
    return path == BROWSER_NET;
}

static bool isNetworkBookmarkPath(const std::string& path) {
    return startsWith(path, BROWSER_NETBOOKMARK);
}

static int networkBookmarkIndexFromPath(const std::string& path) {
    if (!isNetworkBookmarkPath(path)) return -1;
    const std::string tail = path.substr(std::strlen(BROWSER_NETBOOKMARK));
    char* end = nullptr;
    long n = std::strtol(tail.c_str(), &end, 10);
    if (!end || (*end != '\0' && *end != '/')) return -1;
    return n < 0 ? -1 : (int)n;
}

static bool splitNetworkBookmarkBrowserPath(const std::string& path, int& index, std::string& remotePath) {
    index = networkBookmarkIndexFromPath(path);
    remotePath.clear();
    if (index < 0) return false;

    const std::string tail = path.substr(std::strlen(BROWSER_NETBOOKMARK));
    char* end = nullptr;
    (void)std::strtol(tail.c_str(), &end, 10);
    if (end && *end == '/') {
        // Preserve absolute SFTP paths. Example: nxmpnet:/0//home/me/music
        // becomes /home/me/music. Relative SMB paths remain share/folder.
        remotePath = std::string(end + 1);
    }
    return true;
}

static std::string networkBookmarkBrowserPath(int index, const std::string& remotePath = std::string()) {
    std::string out = std::string(BROWSER_NETBOOKMARK) + std::to_string(index);
    if (!remotePath.empty()) out += "/" + remotePath;
    return out;
}

static bool isBrowserSourceMenu(const std::string& path) {
    return path == BROWSER_SOURCE_MENU;
}

static const char* sourceKindName(SourceKind kind) {
    switch (kind) {
        case SourceKind::LocalFiles: return "Local files";
        case SourceKind::Network:    return "Network";
        case SourceKind::Usb:        return "USB";
    }
    return "Local files";
}

static bool looksLikeUsbMountPath(const std::string& path) {
    const std::string l = lowerCopy(path);
    if (startsWith(l, "usb:/")) return true;       // ampintosh.ini alias
    if (startsWith(l, "ums")) {
        const size_t dev = l.find(":/");
        if (dev != std::string::npos && dev <= 5) return true;
    }
    if (startsWith(l, "usb")) {
        const size_t dev = l.find(":/");
        if (dev != std::string::npos && dev <= 5) return true;
    }
    return false;
}

static std::string stripTrailingSlashes(std::string path) {
    const size_t dev = path.find(":/");
    const size_t minLen = (dev == std::string::npos) ? 1 : dev + 2;
    while (path.size() > minLen && path.back() == '/') path.pop_back();
    return path;
}

static bool isDeviceRoot(const std::string& rawPath) {
    const std::string path = stripTrailingSlashes(rawPath);
    const size_t dev = path.find(":/");
    return dev != std::string::npos && path.size() == dev + 2;
}

static std::string parentDirectory(const std::string& rawPath) {
    if (rawPath.empty() || isBrowserNet(rawPath) || isBrowserSourceMenu(rawPath) || isNetworkBookmarkPath(rawPath)) return BROWSER_ROOT;
    std::string path = stripTrailingSlashes(rawPath);
    if (isDeviceRoot(path)) return BROWSER_ROOT;

    const size_t dev = path.find(":/");
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return BROWSER_ROOT;
    if (dev != std::string::npos && slash == dev + 1) return path.substr(0, dev + 2);
    return path.substr(0, slash);
}

static std::string pathLeaf(const std::string& rawPath) {
    std::string path = stripTrailingSlashes(rawPath);
    if (path.empty()) return "Library roots";
    if (isBrowserSourceMenu(path)) return "Source Menu";
    if (isBrowserNet(path)) return "Network Streams";
    if (isNetworkBookmarkPath(path)) return "Network Location";
    if (isDeviceRoot(path)) return path;
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static DIR* openDirectoryRobust(const std::string& path) {
    if (path.empty()) return nullptr;
    DIR* d = opendir(path.c_str());
    if (d) return d;

    // Fallback for fsdev/libnx edge cases with long UTF-8 child paths. Opening
    // by full path can fail for some FAT/exFAT entries even though the parent
    // directory returned the exact child name. Try parent + relative leaf too.
    const std::string parent = parentDirectory(path);
    const std::string leaf = pathLeaf(path);
    if (parent.empty() || leaf.empty() || parent == path || leaf == path) return nullptr;

    char cwd[1024];
    const bool haveCwd = (getcwd(cwd, sizeof(cwd)) != nullptr);
    if (chdir(parent.c_str()) != 0) return nullptr;
    d = opendir(leaf.c_str());
    if (haveCwd) chdir(cwd);
    else chdir("sdmc:/");
    return d;
}

enum class FsPathKind { Missing, File, Directory, Other };

struct ListedDirEntry {
    std::string name;
    FsPathKind  kind = FsPathKind::Other;
    bool        kindKnown = false;
};

static FsPathKind classifyFilesystemPathNative(const std::string& path) {
#ifdef __SWITCH__
    if (path.empty()) return FsPathKind::Other;
    FsFileSystem* fs = nullptr;
    char fsPath[FS_MAX_PATH] = {0};
    if (fsdevTranslatePath(path.c_str(), &fs, fsPath) == 0 && fs) {
        FsDirEntryType type;
        if (R_SUCCEEDED(fsFsGetEntryType(fs, fsPath, &type))) {
            if (type == FsDirEntryType_Dir) return FsPathKind::Directory;
            if (type == FsDirEntryType_File) return FsPathKind::File;
            return FsPathKind::Other;
        }
    }
#else
    (void)path;
#endif
    return FsPathKind::Missing;
}

static void addListedDirEntry(std::vector<ListedDirEntry>& out, const std::string& name, FsPathKind kind, bool kindKnown) {
    if (name.empty() || name == "." || name == "..") return;
    for (ListedDirEntry& existing : out) {
        if (existing.name == name) {
            if (kindKnown && !existing.kindKnown) {
                existing.kind = kind;
                existing.kindKnown = true;
            } else if (kindKnown && existing.kindKnown && existing.kind != FsPathKind::File && kind == FsPathKind::File) {
                existing.kind = kind;
            }
            return;
        }
    }
    out.push_back({ name, kind, kindKnown });
}

static bool listDirectoryStdio(const std::string& path, std::vector<ListedDirEntry>& out) {
    DIR* d = openDirectoryRobust(path);
    if (!d) return false;

    struct dirent* e;
    bool opened = true;
    while ((e = readdir(d)) != nullptr) {
        const std::string name = e->d_name;
        FsPathKind kind = FsPathKind::Other;
        bool known = false;
#ifdef DT_DIR
        if (e->d_type == DT_DIR) { kind = FsPathKind::Directory; known = true; }
#endif
#ifdef DT_REG
        // Only trust DT_REG as a hint for leaf names that already look like
        // files/playlists. Some Switch FAT/exFAT paths have reported dotted
        // Unicode directories as regular files, which hid album folders.
        if (!known && e->d_type == DT_REG && (isPlayableAudioLeafName(name) || isPlaylistLeafName(name))) {
            kind = FsPathKind::File;
            known = true;
        }
#endif
        addListedDirEntry(out, name, kind, known);
    }
    closedir(d);
    return opened;
}

static bool listDirectoryNative(const std::string& path, std::vector<ListedDirEntry>& out) {
#ifdef __SWITCH__
    if (path.empty()) return false;
    FsFileSystem* fs = nullptr;
    char fsPath[FS_MAX_PATH] = {0};
    if (fsdevTranslatePath(path.c_str(), &fs, fsPath) != 0 || !fs) return false;

    FsDir dir;
    const u32 mode = FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles;
    Result rc = fsFsOpenDirectory(fs, fsPath, mode, &dir);
    if (R_FAILED(rc)) return false;

    FsDirectoryEntry buf[48];
    bool opened = true;
    while (true) {
        s64 total = 0;
        rc = fsDirRead(&dir, &total, sizeof(buf) / sizeof(buf[0]), buf);
        if (R_FAILED(rc) || total <= 0) break;
        for (s64 i = 0; i < total; ++i) {
            FsPathKind kind = FsPathKind::Other;
            bool known = true;
            if (buf[i].type == FsDirEntryType_Dir) kind = FsPathKind::Directory;
            else if (buf[i].type == FsDirEntryType_File) kind = FsPathKind::File;
            else known = false;
            addListedDirEntry(out, std::string(buf[i].name), kind, known);
        }
    }
    fsDirClose(&dir);
    return opened;
#else
    (void)path; (void)out;
    return false;
#endif
}

static bool listDirectoryEntries(const std::string& path, std::vector<ListedDirEntry>& out) {
    out.clear();
    const bool stdioOpened = listDirectoryStdio(path, out);
    const size_t beforeNative = out.size();
    const bool nativeOpened = listDirectoryNative(path, out);

    // On Switch, fsdev/opendir can enumerate a Unicode folder at the parent
    // level but fail to enumerate the folder's own children. Native fsFs* reads
    // are the fallback that fixes folders such as:
    //   PSYCHIC LOVER 〜PSYCHIC SELECTION vol.1〜
    return stdioOpened || nativeOpened || beforeNative != out.size() || !out.empty();
}

static FsPathKind classifyFilesystemPath(const std::string& path) {
    if (path.empty() || isHttpURL(path) || startsWith(lowerCopy(path), "sftp://") || startsWith(lowerCopy(path), "smb://") || isBrowserRoot(path) || isBrowserNet(path) || isBrowserSourceMenu(path) || isNetworkBookmarkPath(path))
        return FsPathKind::Other;

    const FsPathKind nativeKind = classifyFilesystemPathNative(path);
    if (nativeKind == FsPathKind::Directory || nativeKind == FsPathKind::File)
        return nativeKind;

    // Prefer opendir() over dirent::d_type/stat for stdio-backed paths.
    DIR* d = openDirectoryRobust(path);
    if (d) { closedir(d); return FsPathKind::Directory; }

    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) return FsPathKind::Directory;
        if (S_ISREG(st.st_mode)) return FsPathKind::File;
        return FsPathKind::Other;
    }

    // Last resort: some fsdev-backed paths can be opened even if stat() is
    // flaky. Do not require this for browsing, but it helps classify odd files.
    FILE* f = fopen(path.c_str(), "rb");
    if (f) { fclose(f); return FsPathKind::File; }

    return FsPathKind::Missing;
}

static bool pathExistsAsDir(const std::string& path) {
    return classifyFilesystemPath(path) == FsPathKind::Directory;
}

static bool pathIsSameOrUnder(const std::string& rawPath, const std::string& rawRoot) {
    const std::string path = stripTrailingSlashes(rawPath);
    const std::string root = stripTrailingSlashes(rawRoot);
    if (path == root) return true;
    if (root.empty()) return true;
    std::string prefix = root;
    if (prefix.back() != '/') prefix += '/';
    return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0;
}

static bool isUsbAlias(const std::string& path) {
    const std::string l = lowerCopy(path);
    return startsWith(l, "usb:/");
}

#ifdef AMPINTOSH_USB
static std::vector<UsbHsFsDevice> listUsbDevices() {
    std::vector<UsbHsFsDevice> devs;
    if (!g_usbReady) return devs;

    const u32 n = usbHsFsGetMountedDeviceCount();
    if (n == 0) return devs;

    devs.resize(n);
    const u32 got = usbHsFsListMountedDevices(devs.data(), n);
    devs.resize(got);
    return devs;
}

static void waitForUsbMounts(int waitMs) {
    if (!g_usbReady || waitMs <= 0) return;
    const int stepMs = 100;
    for (int waited = 0; waited < waitMs; waited += stepMs) {
        if (usbHsFsGetMountedDeviceCount() > 0) return;
        svcSleepThread((s64)stepMs * 1000000LL);
    }
}
#endif

static std::vector<std::string> expandDevicePath(const std::string& path, const AppConfig& cfg) {
    std::vector<std::string> out;
    if (!isUsbAlias(path)) {
        out.push_back(path);
        return out;
    }

#ifdef AMPINTOSH_USB
    if (!cfg.usbEnabled || !g_usbReady) return out;
    const std::string suffix = path.substr(4);       // "usb:/Music" -> "/Music"
    for (const UsbHsFsDevice& d : listUsbDevices()) {
        if (d.name[0] == '\0') continue;
        out.push_back(std::string(d.name) + suffix);
    }
#else
    (void)cfg;
#endif
    return out;
}

// Recursively collect audio files under `dir` into `out`. Depth- and count-
// capped so a huge library or a pathological tree can't stall startup.
static void scanDir(const std::string& dir, std::vector<std::string>& out, int depth, const AppConfig& cfg) {
    if (depth > cfg.maxDepth || (int)out.size() >= cfg.maxTracks) return;
    std::vector<ListedDirEntry> listed;
    if (!listDirectoryEntries(dir, listed)) return;
    for (const ListedDirEntry& ent : listed) {
        const std::string& name = ent.name;
        if (name == "." || name == ".." || (!name.empty() && name[0] == '.')) continue;
        const std::string full = joinPath(dir, name);

        if (isPlayableAudioLeafName(name) || isPlayableAudioFile(full) || isPlaylistLeafName(name) || isPlaylistFile(full)) {
            out.push_back(full);
        } else {
            FsPathKind kind = ent.kindKnown ? ent.kind : classifyFilesystemPath(full);
            if (kind == FsPathKind::Directory && cfg.recursive)
                scanDir(full, out, depth + 1, cfg);
        }
        if ((int)out.size() >= cfg.maxTracks) break;
    }
}

static std::vector<std::string> scanFiles(const AppConfig& cfg) {
    std::vector<std::string> out;
    for (const auto& root : cfg.musicDirs) {
        for (const auto& expanded : expandDevicePath(root, cfg))
            scanDir(expanded, out, 0, cfg);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static void ensureDirRecursive(const std::string& rawPath) {
    if (rawPath.empty()) return;
    std::string path = rawPath;
    while (!path.empty() && path.back() == '/') path.pop_back();
    const size_t dev = path.find(":/");
    size_t start = (dev == std::string::npos) ? 0 : dev + 2;
    for (size_t pos = path.find('/', start + 1); pos != std::string::npos; pos = path.find('/', pos + 1)) {
        std::string partial = path.substr(0, pos);
        if (!partial.empty()) mkdir(partial.c_str(), 0777);
    }
    mkdir(path.c_str(), 0777);
}

#ifdef AMPINTOSH_NET
// Read stream URLs (one per line, '#' comments) from configured stream lists.
static std::vector<std::string> readStreamURLs(const AppConfig& cfg) {
    std::vector<std::string> urls;
    for (const auto& configuredPath : cfg.streamLists) {
        for (const auto& p : expandDevicePath(configuredPath, cfg)) {
            FILE* f = fopen(p.c_str(), "r");
            if (!f) continue;
            char line[2048];
            while (fgets(line, sizeof(line), f)) {
                std::string s(line);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
                    s.pop_back();
                const size_t a = s.find_first_not_of(" \t");
                if (a == std::string::npos) continue;
                s = s.substr(a);
                if (s.empty() || s[0] == '#') continue;
                const std::string ls = lowerCopy(s);
                if (isHttpURL(s) || startsWith(ls, "sftp://") || startsWith(ls, "smb://")) appendUnique(urls, s);
            }
            fclose(f);
        }
    }
    for (const auto& u : cfg.streamURLs) {
        const std::string lu = lowerCopy(u);
        if (isHttpURL(u) || startsWith(lu, "sftp://") || startsWith(lu, "smb://")) appendUnique(urls, u);
    }
    return urls;
}

static size_t curlToFile(void* ptr, size_t sz, size_t nmemb, void* f) {
    return fwrite(ptr, sz, nmemb, static_cast<FILE*>(f));
}

// Download a URL to a local file. Cert verification is off — this is a homebrew
// toy, not a security boundary, and it avoids shipping a CA bundle.
static bool downloadURL(const std::string& url, const std::string& dest, const AppConfig& cfg) {
    ensureDirRecursive(cfg.cacheDir);
    FILE* f = fopen(dest.c_str(), "wb");
    if (!f) return false;
    CURL* c = curl_easy_init();
    if (!c) { fclose(f); return false; }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlToFile);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, (long)cfg.netTimeoutSec);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)cfg.netTimeoutSec);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Ampintosh-Switch/1.5.2");
    const CURLcode res = curl_easy_perform(c);
    curl_easy_cleanup(c);
    fclose(f);
    return res == CURLE_OK;
}
#endif // AMPINTOSH_NET

static void normalizeLibraryEntries(std::vector<Entry>& entries);

static std::vector<std::string> readPlaylistTargets(const std::string& playlistPath, const AppConfig& cfg) {
    std::vector<std::string> targets;
    FILE* f = fopen(playlistPath.c_str(), "r");
    if (!f) return targets;

    const std::string base = directoryName(playlistPath);
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        s = trim(s);
        if (s.empty() || s[0] == '#') continue;
        if (isPlayableAudioFile(s) || isHttpURL(s) || startsWith(lowerCopy(s), "sftp://") || startsWith(lowerCopy(s), "smb://")) {
            if (s.find(":/") == std::string::npos && !base.empty()) s = joinPath(base, s);
            appendUnique(targets, s);
        }
        if ((int)targets.size() >= cfg.maxTracks) break;
    }
    fclose(f);
    return targets;
}


static void undoId3Unsync(std::vector<uint8_t>& data);
static bool extractId3CoverFromBlock(const uint8_t* block, size_t blockSize, std::vector<uint8_t>& image);

// ---------------------------------------------------------------------------
// Embedded text metadata (ID3 / FLAC Vorbis comments / WAV INFO+ID3)
// ---------------------------------------------------------------------------
struct TrackMetadata {
    std::string title;
    std::string artist;
    std::string album;
    bool any() const { return !title.empty() || !artist.empty() || !album.empty(); }
};

static std::map<std::string, TrackMetadata> gTrackMetadata;

static uint32_t rd32beMeta(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t rd32leMeta(const uint8_t* p) { return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t rdSyncSafeMeta(const uint8_t* p) {
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) << 7)  |  (uint32_t)(p[3] & 0x7f);
}

static void appendUtf8Codepoint(std::string& out, uint32_t cp) {
    if (cp == 0) return;
    if (cp <= 0x7f) out.push_back((char)cp);
    else if (cp <= 0x7ff) {
        out.push_back((char)(0xc0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back((char)(0xe0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back((char)(0x80 | (cp & 0x3f)));
    } else if (cp <= 0x10ffff) {
        out.push_back((char)(0xf0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back((char)(0x80 | (cp & 0x3f)));
    }
}

static std::string latin1ToUtf8(const uint8_t* data, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = data[i];
        if (c == 0) break;
        appendUtf8Codepoint(out, c);
    }
    return out;
}

static std::string utf16BytesToUtf8(const uint8_t* data, size_t n, bool littleEndian, size_t pos = 0) {
    std::string out;
    while (pos + 1 < n) {
        uint16_t w = littleEndian ? (uint16_t)(data[pos] | (data[pos + 1] << 8))
                                  : (uint16_t)((data[pos] << 8) | data[pos + 1]);
        pos += 2;
        if (w == 0) break;
        uint32_t cp = w;
        if (w >= 0xd800 && w <= 0xdbff && pos + 1 < n) {
            uint16_t w2 = littleEndian ? (uint16_t)(data[pos] | (data[pos + 1] << 8))
                                       : (uint16_t)((data[pos] << 8) | data[pos + 1]);
            if (w2 >= 0xdc00 && w2 <= 0xdfff) {
                pos += 2;
                cp = 0x10000u + (((uint32_t)w - 0xd800u) << 10) + ((uint32_t)w2 - 0xdc00u);
            }
        }
        appendUtf8Codepoint(out, cp);
    }
    return out;
}

static std::string cleanMetadataString(std::string s) {
    // Decode common WAV/RIFF UTF-16 payloads before trimming at NULL. ID3 text
    // frames are decoded before they get here, but RIFF INFO chunks in the wild
    // may be UTF-16LE/BE even though the old INFO convention was ANSI.
    if (s.size() >= 2) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(s.data());
        if (b[0] == 0xff && b[1] == 0xfe) return trim(utf16BytesToUtf8(b, s.size(), true, 2));
        if (b[0] == 0xfe && b[1] == 0xff) return trim(utf16BytesToUtf8(b, s.size(), false, 2));
    }

    // Heuristic for BOM-less UTF-16LE ASCII-ish INFO fields (e.g. T\0i\0t\0l\0e\0).
    if (s.size() >= 6) {
        size_t oddNuls = 0, oddChecked = 0;
        for (size_t i = 1; i < s.size() && oddChecked < 16; i += 2, ++oddChecked)
            if (s[i] == '\0') ++oddNuls;
        if (oddChecked >= 3 && oddNuls * 2 >= oddChecked) {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(s.data());
            return trim(utf16BytesToUtf8(b, s.size(), true, 0));
        }
    }

    // Keep only the first NULL-separated ID3/INFO value for the UI; Vorbis
    // comments arrive as plain UTF-8 and normally have no NULLs.
    const size_t nul = s.find('\0');
    if (nul != std::string::npos) s = s.substr(0, nul);
    if (s.size() >= 3 && (uint8_t)s[0] == 0xef && (uint8_t)s[1] == 0xbb && (uint8_t)s[2] == 0xbf)
        s.erase(0, 3);
    return trim(s);
}

static std::string decodeId3TextValue(const uint8_t* data, size_t n) {
    if (!data || n == 0) return std::string();
    const uint8_t enc = data[0];
    const uint8_t* p = data + 1;
    size_t len = n - 1;
    std::string out;
    if (enc == 0) {
        out = latin1ToUtf8(p, len);
    } else if (enc == 3) {
        out.assign((const char*)p, len);
    } else if (enc == 1) { // UTF-16 with BOM.
        bool le = false;
        size_t pos = 0;
        if (len >= 2 && p[0] == 0xff && p[1] == 0xfe) { le = true; pos = 2; }
        else if (len >= 2 && p[0] == 0xfe && p[1] == 0xff) { le = false; pos = 2; }
        // Some encoders omit the BOM. ID3 says it should be there; assume BE if absent.
        out = utf16BytesToUtf8(p, len, le, pos);
    } else if (enc == 2) { // UTF-16BE without BOM.
        out = utf16BytesToUtf8(p, len, false, 0);
    }
    return cleanMetadataString(out);
}

static void mergeMetadataField(TrackMetadata& meta, const std::string& rawKey, const std::string& rawValue) {
    const std::string key = lowerCopy(trim(rawKey));
    const std::string value = cleanMetadataString(rawValue);
    if (value.empty()) return;
    if ((key == "title" || key == "tit2" || key == "inam") && meta.title.empty()) meta.title = value;
    else if ((key == "artist" || key == "albumartist" || key == "album_artist" || key == "tpe1" || key == "tpe2" || key == "iart") && meta.artist.empty()) meta.artist = value;
    else if ((key == "album" || key == "talb" || key == "iprd" || key == "ialb") && meta.album.empty()) meta.album = value;
}

static bool parseId3v2MetadataBlock(const uint8_t* data, size_t n, TrackMetadata& meta) {
    if (!data || n < 10 || std::memcmp(data, "ID3", 3) != 0) return false;
    const int version = data[3];
    if (version < 3 || version > 4) return false;
    const uint8_t flags = data[5];
    const uint32_t tagSize = rdSyncSafeMeta(data + 6);
    if (tagSize == 0 || tagSize > 16u * 1024u * 1024u) return false;
    const size_t avail = std::min<size_t>(tagSize, n - 10);
    std::vector<uint8_t> tag(data + 10, data + 10 + avail);
    if (flags & 0x80) undoId3Unsync(tag);

    size_t pos = 0;
    if (flags & 0x40) { // extended header.
        if (tag.size() < 4) return false;
        uint32_t ext = (version == 4) ? rdSyncSafeMeta(tag.data()) : rd32beMeta(tag.data());
        if (version == 3) ext += 4; // v2.3 size excludes the size field itself.
        if (ext < tag.size()) pos = ext;
        else return false;
    }

    bool found = false;
    for (; pos + 10 <= tag.size();) {
        if (tag[pos] == 0) break;
        char idBuf[5] = {0};
        std::memcpy(idBuf, tag.data() + pos, 4);
        const std::string id(idBuf);
        const uint32_t frameSize = (version == 4) ? rdSyncSafeMeta(tag.data() + pos + 4) : rd32beMeta(tag.data() + pos + 4);
        const size_t dataPos = pos + 10;
        if (frameSize == 0 || dataPos + frameSize > tag.size()) break;

        if (id == "TIT2" || id == "TPE1" || id == "TPE2" || id == "TALB") {
            mergeMetadataField(meta, id, decodeId3TextValue(tag.data() + dataPos, frameSize));
            found = true;
        }
        pos = dataPos + frameSize;
    }
    return found && meta.any();
}

static bool extractMp3TextMetadata(const std::string& path, TrackMetadata& meta) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t hdr[10];
    if (std::fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { std::fclose(f); return false; }
    if (std::memcmp(hdr, "ID3", 3) != 0) { std::fclose(f); return false; }
    const uint32_t tagSize = rdSyncSafeMeta(hdr + 6);
    if (tagSize == 0 || tagSize > 16u * 1024u * 1024u) { std::fclose(f); return false; }
    std::vector<uint8_t> block(10 + tagSize);
    std::memcpy(block.data(), hdr, 10);
    const bool ok = std::fread(block.data() + 10, 1, tagSize, f) == tagSize;
    std::fclose(f);
    return ok && parseId3v2MetadataBlock(block.data(), block.size(), meta);
}

static bool seekPastOptionalId3v2Prefix(FILE* f, uint64_t* outOffset = nullptr) {
    if (outOffset) *outOffset = 0;
    if (!f) return false;
    uint8_t hdr[10];
    if (std::fseek(f, 0, SEEK_SET) != 0) return false;
    if (std::fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;
    if (std::memcmp(hdr, "ID3", 3) == 0) {
        const uint32_t tagSize = rdSyncSafeMeta(hdr + 6);
        if (tagSize == 0 || tagSize > 16u * 1024u * 1024u) return false;
        const uint64_t off = 10ull + (uint64_t)tagSize;
        if (std::fseek(f, (long)off, SEEK_SET) != 0) return false;
        if (outOffset) *outOffset = off;
        return true;
    }
    if (std::fseek(f, 0, SEEK_SET) != 0) return false;
    return true;
}

static bool extractFlacTextMetadata(const std::string& path, TrackMetadata& meta) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    if (!seekPastOptionalId3v2Prefix(f)) { std::fclose(f); return false; }
    uint8_t magic[4];
    if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "fLaC", 4) != 0) {
        std::fclose(f); return false;
    }
    bool found = false;
    for (;;) {
        uint8_t h[4];
        if (std::fread(h, 1, 4, f) != 4) break;
        const int type = h[0] & 0x7f;
        const bool last = (h[0] & 0x80) != 0;
        const uint32_t len = ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
        if (len > 16u * 1024u * 1024u) break;
        if (type != 4) { // VORBIS_COMMENT.
            std::fseek(f, (long)len, SEEK_CUR);
            if (last) break;
            continue;
        }
        std::vector<uint8_t> block(len);
        if (std::fread(block.data(), 1, block.size(), f) != block.size()) break;
        size_t pos = 0;
        if (pos + 4 > block.size()) break;
        uint32_t vendorLen = rd32leMeta(block.data() + pos); pos += 4;
        if (pos + vendorLen + 4 > block.size()) break;
        pos += vendorLen;
        uint32_t count = rd32leMeta(block.data() + pos); pos += 4;
        for (uint32_t i = 0; i < count && pos + 4 <= block.size(); ++i) {
            uint32_t clen = rd32leMeta(block.data() + pos); pos += 4;
            if (pos + clen > block.size()) break;
            std::string comment((const char*)block.data() + pos, clen);
            pos += clen;
            const size_t eq = comment.find('=');
            if (eq == std::string::npos) continue;
            mergeMetadataField(meta, comment.substr(0, eq), comment.substr(eq + 1));
        }
        found = meta.any();
        break;
    }
    std::fclose(f);
    return found;
}

static void parseWavInfoList(const std::vector<uint8_t>& block, TrackMetadata& meta) {
    if (block.size() < 4 || std::memcmp(block.data(), "INFO", 4) != 0) return;
    size_t pos = 4;
    while (pos + 8 <= block.size()) {
        char idBuf[5] = {0};
        std::memcpy(idBuf, block.data() + pos, 4);
        const uint32_t len = rd32leMeta(block.data() + pos + 4);
        pos += 8;
        if (pos + len > block.size()) break;
        std::string value((const char*)block.data() + pos, len);
        mergeMetadataField(meta, idBuf, value);
        pos += len + (len & 1u);
    }
}

static bool extractWavTextMetadata(const std::string& path, TrackMetadata& meta) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t riff[12];
    if (std::fread(riff, 1, sizeof(riff), f) != sizeof(riff) ||
        std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        std::fclose(f); return false;
    }
    bool found = false;
    for (;;) {
        uint8_t h[8];
        if (std::fread(h, 1, 8, f) != 8) break;
        char idBuf[5] = {0};
        std::memcpy(idBuf, h, 4);
        const uint32_t len = rd32leMeta(h + 4);
        const std::string id(idBuf);
        const std::string idLower = lowerCopy(id);
        const bool interesting = (id == "LIST" || idLower == "id3 " || idLower == "id3");
        if (interesting && len <= 16u * 1024u * 1024u) {
            std::vector<uint8_t> block(len);
            if (std::fread(block.data(), 1, block.size(), f) != block.size()) break;
            if (id == "LIST") parseWavInfoList(block, meta);
            else parseId3v2MetadataBlock(block.data(), block.size(), meta);
            found = meta.any();
        } else {
            if (std::fseek(f, (long)len, SEEK_CUR) != 0) break;
        }
        if (len & 1u) std::fseek(f, 1, SEEK_CUR);
    }
    std::fclose(f);
    return found;
}

static TrackMetadata readTrackMetadataFromLocalFile(const std::string& path) {
    TrackMetadata meta;
    const AudioFmt fmt = fmtForLocalFile(path);
    switch (fmt) {
        case AudioFmt::Mp3:  extractMp3TextMetadata(path, meta); break;
        case AudioFmt::Flac: extractFlacTextMetadata(path, meta); break;
        case AudioFmt::Wav:  extractWavTextMetadata(path, meta); break;
        default: break;
    }
    return meta;
}

static std::string metadataDisplayLabel(const TrackMetadata& meta, const std::string& fallback) {
    if (!meta.title.empty() && !meta.artist.empty()) return meta.artist + " - " + meta.title;
    if (!meta.title.empty()) return meta.title;
    return fallback.empty() ? std::string("Unknown Track") : fallback;
}

static TrackMetadata metadataForRef(const std::string& ref) {
    auto it = gTrackMetadata.find(ref);
    if (it != gTrackMetadata.end()) return it->second;
    const std::string l = lowerCopy(ref);
    if (startsWith(l, "smb://") || startsWith(l, "sftp://") || isHttpURL(ref)) return TrackMetadata{};
    TrackMetadata meta = readTrackMetadataFromLocalFile(ref);
    if (meta.any()) gTrackMetadata[ref] = meta;
    return meta;
}

static std::string labelForAudioRef(const std::string& ref) {
    return metadataDisplayLabel(metadataForRef(ref), displayName(ref));
}

static void cacheMetadataForRefFromLocalFile(const std::string& ref, const std::string& localPath) {
    TrackMetadata meta = readTrackMetadataFromLocalFile(localPath);
    if (meta.any()) gTrackMetadata[ref] = meta;
}

// Build the full library: local files (SD + configured USB placeholders), then streams.
static std::vector<Entry> buildLibrary(const AppConfig& cfg) {
    std::vector<Entry> out;
    for (const auto& f : scanFiles(cfg)) {
        if (isPlaylistFile(f)) {
            for (const std::string& target : readPlaylistTargets(f, cfg)) {
                if (isPlayableAudioFile(target) || isHttpURL(target) || startsWith(lowerCopy(target), "sftp://") || startsWith(lowerCopy(target), "smb://"))
                    out.push_back({ target, labelForAudioRef(target), directoryName(target), isHttpURL(target) || startsWith(lowerCopy(target), "sftp://") || startsWith(lowerCopy(target), "smb://") });
            }
        } else {
            out.push_back({ f, labelForAudioRef(f), directoryName(f), false });
        }
    }
#ifdef AMPINTOSH_NET
    if (cfg.netEnabled && g_netReady) {
        for (const auto& u : readStreamURLs(cfg)) {
            std::string label = displayName(u);
            if (label.empty()) label = u;
            out.push_back({ u, label, directoryName(u), true });
        }
        for (const NetworkBookmark& b : cfg.networkBookmarks) {
            const std::string u = networkBookmarkURL(b);
            if (u.empty()) continue;
            // Treat NXMP-style bookmarks as playable only when the configured path
            // points at a decoder-backed file or playlist. Directory browsing for
            // SMB/SFTP needs a real remote filesystem backend, so directory roots
            // stay visible in the Network menu but are not forced into the queue.
            if (isPlayableAudioFile(u)) {
                std::string label = b.sectionName.empty() ? displayName(u) : b.sectionName;
                out.push_back({ u, label, directoryName(u), true });
            }
        }
    }
#else
    (void)cfg;
#endif
    normalizeLibraryEntries(out);
    return out;
}

static std::vector<DirectoryGroup> buildDirectoryGroups(const std::vector<Entry>& entries) {
    std::vector<DirectoryGroup> dirs;
    for (int i = 0; i < (int)entries.size(); ++i) {
        const std::string path = entries[i].dir.empty() ? "<root>" : entries[i].dir;
        if (dirs.empty() || dirs.back().path != path) {
            dirs.push_back({ path, i, 1 });
        } else {
            dirs.back().count++;
        }
    }
    return dirs;
}

static int directoryIndexForEntry(const std::vector<DirectoryGroup>& dirs, int entryIndex) {
    for (int i = 0; i < (int)dirs.size(); ++i) {
        if (entryIndex >= dirs[i].first && entryIndex < dirs[i].first + dirs[i].count) return i;
    }
    return -1;
}

static int clampEntryIndex(int idx, int count) {
    if (count <= 0) return 0;
    return std::max(0, std::min(idx, count - 1));
}

static int stepDirectoryOrderedIndex(int current, int direction, const std::vector<DirectoryGroup>& dirs, int entryCount) {
    if (entryCount <= 1) return 0;
    if (dirs.empty()) return (current + direction + entryCount) % entryCount;
    current = clampEntryIndex(current, entryCount);
    const int dirIdx = directoryIndexForEntry(dirs, current);
    if (dirIdx < 0) return (current + direction + entryCount) % entryCount;

    const DirectoryGroup& d = dirs[dirIdx];
    const int local = current - d.first;
    if (direction >= 0) {
        if (local + 1 < d.count) return current + 1;
        const int nextDir = (dirIdx + 1) % (int)dirs.size();
        return dirs[nextDir].first;
    }

    if (local > 0) return current - 1;
    const int prevDir = (dirIdx + (int)dirs.size() - 1) % (int)dirs.size();
    return dirs[prevDir].first + dirs[prevDir].count - 1;
}

static int findEntryIndexByRef(const std::vector<Entry>& entries, const std::string& ref) {
    for (int i = 0; i < (int)entries.size(); ++i)
        if (entries[i].ref == ref) return i;
    return -1;
}

static int countEntriesInOrUnder(const std::vector<Entry>& entries, const std::string& dir) {
    int count = 0;
    for (const Entry& e : entries) {
        if (e.isURL) continue;
        if (pathIsSameOrUnder(e.dir, dir)) ++count;
    }
    return count;
}

static int countPlayableFilesUnderDir(const std::string& dir, const AppConfig& cfg, int depth = 0, int cap = 9999) {
    if (depth > cfg.maxDepth || cap <= 0) return 0;
    int count = 0;
    std::vector<ListedDirEntry> listed;
    if (!listDirectoryEntries(dir, listed)) return 0;
    for (const ListedDirEntry& ent : listed) {
        if (count >= cap) break;
        const std::string& name = ent.name;
        if (name == "." || name == ".." || (!name.empty() && name[0] == '.')) continue;
        const std::string full = joinPath(dir, name);

        if (isPlayableAudioLeafName(name) || isPlayableAudioFile(full)) {
            ++count;
        } else {
            FsPathKind kind = ent.kindKnown ? ent.kind : classifyFilesystemPath(full);
            if (kind == FsPathKind::Directory && cfg.recursive)
                count += countPlayableFilesUnderDir(full, cfg, depth + 1, cap - count);
        }
    }
    return count;
}

static int browserTrackCountForDir(const std::vector<Entry>& entries, const std::string& dir, const AppConfig& cfg) {
    const int scanned = countEntriesInOrUnder(entries, dir);
    if (scanned > 0) return scanned;
    return countPlayableFilesUnderDir(dir, cfg);
}

static void normalizeLibraryEntries(std::vector<Entry>& entries) {
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return lowerCopy(a.ref) < lowerCopy(b.ref);
    });
    entries.erase(std::unique(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.ref == b.ref;
    }), entries.end());
}

static bool addDirectPlayableFilesFromDir(std::vector<Entry>& entries, const std::string& dir) {
    bool changed = false;
    std::vector<ListedDirEntry> listed;
    if (!listDirectoryEntries(dir, listed)) return false;
    for (const ListedDirEntry& ent : listed) {
        const std::string& name = ent.name;
        if (name == "." || name == ".." || (!name.empty() && name[0] == '.')) continue;
        const std::string full = joinPath(dir, name);

        if (!isPlayableAudioLeafName(name) && !isPlayableAudioFile(full)) continue;
        if (findEntryIndexByRef(entries, full) >= 0) continue;
        entries.push_back({ full, labelForAudioRef(full), directoryName(full), false });
        changed = true;
    }
    if (changed) normalizeLibraryEntries(entries);
    return changed;
}

static int countURLStreams(const std::vector<Entry>& entries) {
    int count = 0;
    for (const Entry& e : entries) if (e.isURL) ++count;
    return count;
}

static bool configuredRootMatchesSource(const std::string& root, SourceKind source) {
    if (source == SourceKind::Usb) return looksLikeUsbMountPath(root);
    if (source == SourceKind::LocalFiles) return !looksLikeUsbMountPath(root) && !isHttpURL(root);
    return false;
}

static std::vector<std::string> expandedMusicRootsForSource(const AppConfig& cfg, SourceKind source) {
    std::vector<std::string> roots;
    if (source == SourceKind::Network) return roots;
    for (const std::string& configured : cfg.musicDirs) {
        if (!configuredRootMatchesSource(configured, source)) continue;
        for (const std::string& expanded : expandDevicePath(configured, cfg))
            appendUnique(roots, stripTrailingSlashes(expanded));
    }
    return roots;
}

static int countPlayableFilesInRoots(const std::vector<Entry>& entries, const std::vector<std::string>& roots, const AppConfig& cfg) {
    int count = 0;
    for (const std::string& root : roots) {
        const int scanned = countEntriesInOrUnder(entries, root);
        const int live = countPlayableFilesUnderDir(root, cfg);
        count += std::max(scanned, live);
    }
    return count;
}

static bool entryBelongsToSource(const Entry& e, const AppConfig& cfg, SourceKind source) {
    if (source == SourceKind::Network) return e.isURL;
    if (e.isURL) return false;
    const std::vector<std::string> roots = expandedMusicRootsForSource(cfg, source);
    for (const std::string& root : roots) {
        if (pathIsSameOrUnder(e.dir, root)) return true;
    }
    // Fallback for manually mounted USB paths that may not be represented by
    // an alias after a rescan. This keeps direct ums0:/ or usb0:/ roots in USB.
    if (source == SourceKind::Usb) return looksLikeUsbMountPath(e.ref) || looksLikeUsbMountPath(e.dir);
    if (source == SourceKind::LocalFiles) return !looksLikeUsbMountPath(e.ref) && !looksLikeUsbMountPath(e.dir);
    return false;
}

static std::vector<SourceMenuItem> buildSourceMenuItems(const std::vector<Entry>& entries, const AppConfig& cfg) {
    std::vector<SourceMenuItem> items;
    const std::vector<std::string> localRoots = expandedMusicRootsForSource(cfg, SourceKind::LocalFiles);
    const std::vector<std::string> usbRoots   = expandedMusicRootsForSource(cfg, SourceKind::Usb);

    const int localTracks = countPlayableFilesInRoots(entries, localRoots, cfg);
    const int streams     = countURLStreams(entries) + (int)cfg.networkBookmarks.size();
    const int usbTracks   = countPlayableFilesInRoots(entries, usbRoots, cfg);

    SourceMenuItem local;
    local.kind = SourceKind::LocalFiles;
    local.label = "Local files";
    local.count = localTracks;
    local.available = !localRoots.empty() || localTracks > 0;
    { char b[96]; snprintf(b, sizeof(b), "%d track%s · SD/card configured roots", localTracks, localTracks == 1 ? "" : "s"); local.detail = b; }
    items.push_back(local);

    SourceMenuItem network;
    network.kind = SourceKind::Network;
    network.label = "Network";
    network.count = streams;
#ifdef AMPINTOSH_NET
    network.available = cfg.netEnabled && g_netReady;
#else
    network.available = false;
#endif
    { char b[96]; snprintf(b, sizeof(b), "%d item%s · streams + NXMP-style locations", streams, streams == 1 ? "" : "s"); network.detail = b; }
    items.push_back(network);

    SourceMenuItem usb;
    usb.kind = SourceKind::Usb;
    usb.label = "USB";
    usb.count = usbTracks;
#ifdef AMPINTOSH_USB
    usb.available = cfg.usbEnabled && g_usbReady && (!usbRoots.empty() || usbTracks > 0);
#else
    usb.available = false;
#endif
    { char b[96]; snprintf(b, sizeof(b), "%d track%s · usb:/ paths / mounted devices", usbTracks, usbTracks == 1 ? "" : "s"); usb.detail = b; }
    items.push_back(usb);

    return items;
}

static void sortBrowserItems(std::vector<BrowserItem>& items) {
    std::stable_sort(items.begin(), items.end(), [](const BrowserItem& a, const BrowserItem& b) {
        auto rank = [](BrowserItemKind k) {
            switch (k) {
                case BrowserItemKind::Up:           return 0;
                case BrowserItemKind::Directory:    return 1;
                case BrowserItemKind::StreamFolder: return 1;
                case BrowserItemKind::Track:        return 2;
                case BrowserItemKind::Stream:       return 2;
            }
            return 3;
        };
        const int ra = rank(a.kind), rb = rank(b.kind);
        if (ra != rb) return ra < rb;
        return lowerCopy(a.label) < lowerCopy(b.label);
    });
}


static std::string cleanRemoteServerHost(std::string server) {
    server = trim(server);
    const std::string lower = lowerCopy(server);
    const size_t scheme = lower.find("://");
    if (scheme != std::string::npos) server = server.substr(scheme + 3);
    const size_t at = server.find('@');
    if (at != std::string::npos) server = server.substr(at + 1);
    const size_t slash = server.find('/');
    if (slash != std::string::npos) server = server.substr(0, slash);
    const size_t colon = server.find(':');
    if (colon != std::string::npos && server.find(']') == std::string::npos) server = server.substr(0, colon);
    return server;
}

static std::string remoteStartPathForBookmark(const NetworkBookmark& b) {
    const std::string type = lowerCopy(trim(b.type));
    if (!b.path.empty()) return b.path;
    if (type == "sftp" || type == "ssh") return "/";
    return std::string();
}

static std::string joinRemotePath(const std::string& base, const std::string& leaf) {
    if (base.empty() || base == "/") return base == "/" ? ("/" + leaf) : leaf;
    if (base.back() == '/') return base + leaf;
    return base + "/" + leaf;
}

static std::string remoteParentPath(const NetworkBookmark& b, const std::string& remotePath) {
    const std::string start = stripTrailingSlashes(remoteStartPathForBookmark(b));
    std::string p = stripTrailingSlashes(remotePath.empty() ? remoteStartPathForBookmark(b) : remotePath);
    if (p.empty() || p == "/" || p == start) return std::string();
    const size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) return std::string();
    if (slash == 0) return "/";
    return p.substr(0, slash);
}

static bool splitSmbSharePath(const NetworkBookmark& b, const std::string& remotePath, std::string& share, std::string& pathInShare) {
    std::string p = remotePath.empty() ? remoteStartPathForBookmark(b) : remotePath;
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    if (p.empty()) return false;
    const size_t slash = p.find('/');
    share = slash == std::string::npos ? p : p.substr(0, slash);
    pathInShare = slash == std::string::npos ? std::string("/") : p.substr(slash + 1);
    if (pathInShare.empty()) pathInShare = "/";
    else if (pathInShare.front() != '/') pathInShare = "/" + pathInShare;
    return !share.empty();
}

#ifdef AMPINTOSH_NET
#ifdef AMPINTOSH_SFTP
static bool g_libssh2Ready = false;

static int connectTcpBlocking(const std::string& host, int port, int timeoutSec) {
    (void)timeoutSec; // libnx sockets are blocking here; keep the caller timeout for future poll support.
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const std::string service = std::to_string(port);
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || !res) return -1;
    int sock = -1;
    for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, rp->ai_addr, (socklen_t)rp->ai_addrlen) == 0) break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    return sock;
}

struct SftpSessionHandle {
    int sock = -1;
    LIBSSH2_SESSION* session = nullptr;
    LIBSSH2_SFTP* sftp = nullptr;
    ~SftpSessionHandle() {
        if (sftp) libssh2_sftp_shutdown(sftp);
        if (session) { libssh2_session_disconnect(session, "Ampintosh done"); libssh2_session_free(session); }
        if (sock >= 0) close(sock);
    }
};

static bool openSftpSession(const NetworkBookmark& b, SftpSessionHandle& h, const AppConfig& cfg) {
    if (b.username.empty()) return false;
    if (!g_libssh2Ready) {
        if (libssh2_init(0) != 0) return false;
        g_libssh2Ready = true;
    }
    const std::string host = cleanRemoteServerHost(b.server);
    const int port = b.port > 0 ? b.port : 22;
    h.sock = connectTcpBlocking(host, port, cfg.netTimeoutSec);
    if (h.sock < 0) return false;
    h.session = libssh2_session_init();
    if (!h.session) return false;
    libssh2_session_set_blocking(h.session, 1);
    if (libssh2_session_handshake(h.session, h.sock) != 0) return false;
    if (libssh2_userauth_password(h.session, b.username.c_str(), b.password.c_str()) != 0) return false;
    h.sftp = libssh2_sftp_init(h.session);
    return h.sftp != nullptr;
}

static bool listSftpDirectory(const NetworkBookmark& b, const std::string& remotePath, std::vector<ListedDirEntry>& out, const AppConfig& cfg) {
    SftpSessionHandle h;
    if (!openSftpSession(b, h, cfg)) return false;
    const std::string path = remotePath.empty() ? "/" : remotePath;
    LIBSSH2_SFTP_HANDLE* dir = libssh2_sftp_opendir(h.sftp, path.c_str());
    if (!dir) return false;
    char nameBuf[1024];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    bool opened = true;
    while (true) {
        std::memset(&attrs, 0, sizeof(attrs));
        const int rc = libssh2_sftp_readdir(dir, nameBuf, sizeof(nameBuf) - 1, &attrs);
        if (rc <= 0) break;
        nameBuf[rc] = '\0';
        std::string name(nameBuf, rc);
        FsPathKind kind = FsPathKind::Other;
        bool known = false;
        if (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
            known = true;
            if (LIBSSH2_SFTP_S_ISDIR(attrs.permissions)) kind = FsPathKind::Directory;
            else if (LIBSSH2_SFTP_S_ISREG(attrs.permissions)) kind = FsPathKind::File;
            else kind = FsPathKind::Other;
        }
        addListedDirEntry(out, name, kind, known);
    }
    libssh2_sftp_closedir(dir);
    return opened;
}
#endif // AMPINTOSH_SFTP


#ifdef AMPINTOSH_SMB
struct ParsedSmbUrl {
    std::string host;
    std::string user;
    std::string password;
    std::string share;
    std::string pathInShare;
};

static bool parseSmbUrl(const std::string& url, ParsedSmbUrl& out) {
    const std::string lower = lowerCopy(url);
    if (!startsWith(lower, "smb://")) return false;
    std::string rest = url.substr(6);
    const size_t slash = rest.find('/');
    if (slash == std::string::npos) return false;
    std::string authHost = rest.substr(0, slash);
    std::string path = rest.substr(slash + 1);
    const size_t at = authHost.rfind('@');
    if (at != std::string::npos) {
        std::string auth = authHost.substr(0, at);
        authHost = authHost.substr(at + 1);
        const size_t colon = auth.find(':');
        if (colon == std::string::npos) out.user = urlDecodeComponent(auth);
        else {
            out.user = urlDecodeComponent(auth.substr(0, colon));
            out.password = urlDecodeComponent(auth.substr(colon + 1));
        }
    }
    if (authHost.empty() || path.empty()) return false;
    const size_t shareSlash = path.find('/');
    out.host = authHost; // libsmb2 accepts host[:port]
    out.share = urlDecodeComponent(shareSlash == std::string::npos ? path : path.substr(0, shareSlash));
    std::string rel = shareSlash == std::string::npos ? std::string() : path.substr(shareSlash + 1);
    rel = urlDecodeComponent(rel);
    out.pathInShare = rel.empty() ? std::string("/") : (rel.front() == '/' ? rel : "/" + rel);
    return !out.host.empty() && !out.share.empty();
}

struct SmbStreamHandle {
    struct smb2_context* smb = nullptr;
    struct smb2fh* fh = nullptr;
    uint64_t pos = 0;
    uint64_t size = 0;
    uint32_t maxRead = 64 * 1024;

    void close() {
        if (smb && fh) { smb2_close(smb, fh); fh = nullptr; }
        if (smb) { smb2_disconnect_share(smb); smb2_destroy_context(smb); smb = nullptr; }
        pos = 0; size = 0;
    }
};

static bool openSmbStreamFromUrl(const std::string& url, SmbStreamHandle& h, const AppConfig& cfg) {
    ParsedSmbUrl u;
    if (!parseSmbUrl(url, u)) return false;
    h.smb = smb2_init_context();
    if (!h.smb) return false;
    smb2_set_timeout(h.smb, cfg.netTimeoutSec > 0 ? cfg.netTimeoutSec : 25);
    smb2_set_security_mode(h.smb, SMB2_NEGOTIATE_SIGNING_ENABLED);
    if (!u.user.empty()) smb2_set_user(h.smb, u.user.c_str());
    if (!u.password.empty()) smb2_set_password(h.smb, u.password.c_str());
    const char* user = u.user.empty() ? nullptr : u.user.c_str();
    if (smb2_connect_share(h.smb, u.host.c_str(), u.share.c_str(), user) != 0) { h.close(); return false; }
    h.fh = smb2_open(h.smb, u.pathInShare.c_str(), O_RDONLY);
    if (!h.fh) { h.close(); return false; }
    h.maxRead = smb2_get_max_read_size(h.smb);
    if (h.maxRead == 0 || h.maxRead > 1024 * 1024) h.maxRead = 64 * 1024;
    uint64_t end = 0;
    if (smb2_lseek(h.smb, h.fh, 0, SEEK_END, &end) >= 0) {
        h.size = end;
        uint64_t cur = 0;
        smb2_lseek(h.smb, h.fh, 0, SEEK_SET, &cur);
        h.pos = 0;
    }
    return true;
}

static size_t smbStreamRead(void* user, void* outBuf, size_t bytesToRead) {
    SmbStreamHandle* h = static_cast<SmbStreamHandle*>(user);
    if (!h || !h->smb || !h->fh || bytesToRead == 0) return 0;
    uint8_t* dst = static_cast<uint8_t*>(outBuf);
    size_t total = 0;
    while (total < bytesToRead) {
        size_t want = bytesToRead - total;
        if (want > h->maxRead) want = h->maxRead;
        if (want > 0x7fffffffU) want = 0x7fffffffU;
        const int got = smb2_pread(h->smb, h->fh, dst + total, (uint32_t)want, h->pos);
        if (got <= 0) break;
        h->pos += (uint64_t)got;
        total += (size_t)got;
        if ((size_t)got < want) break;
    }
    return total;
}

static bool smbStreamSeek64(SmbStreamHandle* h, int64_t offset, int origin) {
    if (!h || !h->smb || !h->fh) return false;
    int whence = SEEK_SET;
    if (origin == DRMP3_SEEK_CUR || origin == DRWAV_SEEK_CUR || origin == DRFLAC_SEEK_CUR) whence = SEEK_CUR;
    else if (origin == DRMP3_SEEK_END || origin == DRWAV_SEEK_END || origin == DRFLAC_SEEK_END) whence = SEEK_END;
    uint64_t current = 0;
    const int64_t r = smb2_lseek(h->smb, h->fh, offset, whence, &current);
    if (r < 0) return false;
    h->pos = current;
    return true;
}

static drmp3_bool32 smbMp3Seek(void* user, int offset, drmp3_seek_origin origin) { return smbStreamSeek64(static_cast<SmbStreamHandle*>(user), offset, origin) ? DRMP3_TRUE : DRMP3_FALSE; }
static drmp3_bool32 smbMp3Tell(void* user, drmp3_int64* cursor) { if (!user || !cursor) return DRMP3_FALSE; *cursor = (drmp3_int64)static_cast<SmbStreamHandle*>(user)->pos; return DRMP3_TRUE; }
static drwav_bool32 smbWavSeek(void* user, int offset, drwav_seek_origin origin) { return smbStreamSeek64(static_cast<SmbStreamHandle*>(user), offset, origin) ? DRWAV_TRUE : DRWAV_FALSE; }
static drwav_bool32 smbWavTell(void* user, drwav_int64* cursor) { if (!user || !cursor) return DRWAV_FALSE; *cursor = (drwav_int64)static_cast<SmbStreamHandle*>(user)->pos; return DRWAV_TRUE; }
static drflac_bool32 smbFlacSeek(void* user, int offset, drflac_seek_origin origin) { return smbStreamSeek64(static_cast<SmbStreamHandle*>(user), offset, origin) ? DRFLAC_TRUE : DRFLAC_FALSE; }
static drflac_bool32 smbFlacTell(void* user, drflac_int64* cursor) { if (!user || !cursor) return DRFLAC_FALSE; *cursor = (drflac_int64)static_cast<SmbStreamHandle*>(user)->pos; return DRFLAC_TRUE; }

struct StreamDecoder {
    AudioFmt fmt = AudioFmt::None;
    SmbStreamHandle smb;
    drmp3 mp3;
    drflac* flac = nullptr;
    drwav wav;
    bool mp3Ready = false;
    bool wavReady = false;

    // Async streaming pump. The SMB handle and dr_* decoder live on the pump
    // thread; the SDL callback only drains decoded PCM from this ring.
    SDL_mutex* pumpMutex = nullptr;
    SDL_cond*  pumpCond  = nullptr;
    SDL_Thread* pumpThread = nullptr;
    bool pumpStop = false;
    bool pumpEof = false;
    bool pumpFailed = false;
    bool seekPending = false;
    bool pumpPrimed = false;       // true once enough decoded PCM is buffered to play smoothly
    bool pumpEverPrimed = false;   // after the first start, use the smaller rebuffer threshold
    bool pumpBuffering = true;     // exposed to the UI for the buffering indicator
    drmp3_uint64 seekFrame = 0;
    int pumpChannels = 2;
    int pumpChunkFrames = 2048;
    std::vector<float> ring;
    size_t ringFrames = 0;
    size_t ringRead = 0;
    size_t ringWrite = 0;
    size_t ringFill = 0;
    size_t pumpPrebufferTargetFrames = 0;
    size_t pumpRebufferTargetFrames = 0;

    void stopPump() {
        if (pumpMutex) {
            SDL_LockMutex(pumpMutex);
            pumpStop = true;
            if (pumpCond) SDL_CondBroadcast(pumpCond);
            SDL_UnlockMutex(pumpMutex);
        }
        if (pumpThread) {
            SDL_WaitThread(pumpThread, nullptr);
            pumpThread = nullptr;
        }
        if (pumpCond) { SDL_DestroyCond(pumpCond); pumpCond = nullptr; }
        if (pumpMutex) { SDL_DestroyMutex(pumpMutex); pumpMutex = nullptr; }
        ring.clear();
        ringFrames = ringRead = ringWrite = ringFill = 0;
        pumpStop = pumpEof = pumpFailed = seekPending = false;
        pumpPrimed = pumpEverPrimed = false;
        pumpBuffering = true;
        seekFrame = 0;
        pumpPrebufferTargetFrames = pumpRebufferTargetFrames = 0;
    }

    void close() {
        stopPump();
        if (mp3Ready) { drmp3_uninit(&mp3); mp3Ready = false; }
        if (flac) { drflac_close(flac); flac = nullptr; }
        if (wavReady) { drwav_uninit(&wav); wavReady = false; }
        smb.close();
        fmt = AudioFmt::None;
    }
};


static bool downloadSmbURL(const std::string& url, const std::string& dest, const AppConfig& cfg) {
    ensureDirRecursive(cfg.cacheDir);
    SmbStreamHandle h;
    if (!openSmbStreamFromUrl(url, h, cfg)) return false;
    FILE* f = std::fopen(dest.c_str(), "wb");
    if (!f) { h.close(); return false; }
    std::vector<uint8_t> buf(128 * 1024);
    bool ok = true;
    for (;;) {
        const size_t got = smbStreamRead(&h, buf.data(), buf.size());
        if (got == 0) break;
        if (std::fwrite(buf.data(), 1, got, f) != got) { ok = false; break; }
    }
    std::fclose(f);
    h.close();
    return ok;
}

static bool smbReadAtFully(SmbStreamHandle& h, uint64_t offset, void* outBuf, size_t bytesToRead) {
    if (!h.smb || !h.fh || !outBuf) return false;
    uint8_t* dst = static_cast<uint8_t*>(outBuf);
    size_t total = 0;
    while (total < bytesToRead) {
        size_t want = bytesToRead - total;
        if (want > h.maxRead) want = h.maxRead;
        if (want > 0x7fffffffU) want = 0x7fffffffU;
        const int got = smb2_pread(h.smb, h.fh, dst + total, (uint32_t)want, offset + (uint64_t)total);
        if (got <= 0) return false;
        total += (size_t)got;
    }
    return true;
}

static bool smbReadBlock(SmbStreamHandle& h, uint64_t offset, size_t len, std::vector<uint8_t>& out) {
    if (len > 16u * 1024u * 1024u) return false;
    out.assign(len, 0);
    return len == 0 || smbReadAtFully(h, offset, out.data(), out.size());
}

static bool smbFlacPayloadOffset(SmbStreamHandle& h, uint64_t& off) {
    off = 0;
    uint8_t hdr[10];
    if (smbReadAtFully(h, 0, hdr, sizeof(hdr)) && std::memcmp(hdr, "ID3", 3) == 0) {
        const uint32_t tagSize = rdSyncSafeMeta(hdr + 6);
        if (tagSize == 0 || tagSize > 16u * 1024u * 1024u) return false;
        off = 10ull + (uint64_t)tagSize;
    }
    uint8_t magic[4];
    return smbReadAtFully(h, off, magic, sizeof(magic)) && std::memcmp(magic, "fLaC", 4) == 0;
}

static bool extractMp3TextMetadataFromSmb(SmbStreamHandle& h, TrackMetadata& meta) {
    uint8_t hdr[10];
    if (!smbReadAtFully(h, 0, hdr, sizeof(hdr)) || std::memcmp(hdr, "ID3", 3) != 0) return false;
    const uint32_t tagSize = rdSyncSafeMeta(hdr + 6);
    if (tagSize == 0 || tagSize > 16u * 1024u * 1024u) return false;
    std::vector<uint8_t> block;
    if (!smbReadBlock(h, 0, 10ull + tagSize, block)) return false;
    return parseId3v2MetadataBlock(block.data(), block.size(), meta);
}

static bool extractFlacTextMetadataFromSmb(SmbStreamHandle& h, TrackMetadata& meta) {
    uint64_t off = 0;
    if (!smbFlacPayloadOffset(h, off)) return false;
    off += 4;
    bool found = false;
    for (;;) {
        uint8_t mh[4];
        if (!smbReadAtFully(h, off, mh, sizeof(mh))) break;
        const int type = mh[0] & 0x7f;
        const bool last = (mh[0] & 0x80) != 0;
        const uint32_t len = ((uint32_t)mh[1] << 16) | ((uint32_t)mh[2] << 8) | mh[3];
        off += 4;
        if (len > 16u * 1024u * 1024u) break;
        if (type == 4) { // VORBIS_COMMENT
            std::vector<uint8_t> block;
            if (!smbReadBlock(h, off, len, block)) break;
            size_t pos = 0;
            if (pos + 4 > block.size()) break;
            uint32_t vendorLen = rd32leMeta(block.data() + pos); pos += 4;
            if (pos + vendorLen + 4 > block.size()) break;
            pos += vendorLen;
            uint32_t count = rd32leMeta(block.data() + pos); pos += 4;
            for (uint32_t i = 0; i < count && pos + 4 <= block.size(); ++i) {
                uint32_t clen = rd32leMeta(block.data() + pos); pos += 4;
                if (pos + clen > block.size()) break;
                std::string comment((const char*)block.data() + pos, clen);
                pos += clen;
                const size_t eq = comment.find('=');
                if (eq != std::string::npos) mergeMetadataField(meta, comment.substr(0, eq), comment.substr(eq + 1));
            }
            found = meta.any();
            break;
        }
        off += len;
        if (last) break;
    }
    return found;
}

static bool extractWavTextMetadataFromSmb(SmbStreamHandle& h, TrackMetadata& meta) {
    uint8_t riff[12];
    if (!smbReadAtFully(h, 0, riff, sizeof(riff)) ||
        std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) return false;
    bool found = false;
    uint64_t pos = 12;
    while (pos + 8 <= h.size) {
        uint8_t ch[8];
        if (!smbReadAtFully(h, pos, ch, sizeof(ch))) break;
        char idBuf[5] = {0};
        std::memcpy(idBuf, ch, 4);
        const std::string id(idBuf);
        const std::string idLower = lowerCopy(id);
        const uint32_t len = rd32leMeta(ch + 4);
        pos += 8;
        const bool interesting = (id == "LIST" || idLower == "id3 " || idLower == "id3");
        if (interesting && len <= 16u * 1024u * 1024u && pos + len <= h.size) {
            std::vector<uint8_t> block;
            if (!smbReadBlock(h, pos, len, block)) break;
            if (id == "LIST") parseWavInfoList(block, meta);
            else parseId3v2MetadataBlock(block.data(), block.size(), meta);
            found = meta.any();
        }
        pos += (uint64_t)len + (len & 1u);
    }
    return found;
}

static TrackMetadata readTrackMetadataFromSmbURL(const std::string& url, const AppConfig& cfg) {
    TrackMetadata meta;
    SmbStreamHandle h;
    if (!openSmbStreamFromUrl(url, h, cfg)) return meta;
    AudioFmt fmt = fmtForName(url);
    if (fmt == AudioFmt::None) {
        uint8_t magic[12] = {0};
        if (smbReadAtFully(h, 0, magic, sizeof(magic))) {
            if (std::memcmp(magic, "ID3", 3) == 0) fmt = AudioFmt::Mp3;
            else if (std::memcmp(magic, "fLaC", 4) == 0) fmt = AudioFmt::Flac;
            else if (std::memcmp(magic, "RIFF", 4) == 0 && std::memcmp(magic + 8, "WAVE", 4) == 0) fmt = AudioFmt::Wav;
        }
    }
    switch (fmt) {
        case AudioFmt::Mp3:  extractMp3TextMetadataFromSmb(h, meta); break;
        case AudioFmt::Flac: extractFlacTextMetadataFromSmb(h, meta); break;
        case AudioFmt::Wav:  extractWavTextMetadataFromSmb(h, meta); break;
        default: break;
    }
    h.close();
    return meta;
}

static void cacheMetadataForRefFromSmbURL(const std::string& ref, const AppConfig& cfg) {
    if (gTrackMetadata.find(ref) != gTrackMetadata.end()) return;
    TrackMetadata meta = readTrackMetadataFromSmbURL(ref, cfg);
    if (meta.any()) gTrackMetadata[ref] = meta;
}

static bool extractSmbId3Cover(SmbStreamHandle& h, std::vector<uint8_t>& image) {
    uint8_t hdr[10];
    if (!smbReadAtFully(h, 0, hdr, sizeof(hdr)) || std::memcmp(hdr, "ID3", 3) != 0) return false;
    const uint32_t tagSize = rdSyncSafeMeta(hdr + 6);
    if (tagSize == 0 || tagSize > 16u * 1024u * 1024u) return false;
    std::vector<uint8_t> block;
    if (!smbReadBlock(h, 0, 10ull + tagSize, block)) return false;
    return extractId3CoverFromBlock(block.data(), block.size(), image);
}

static bool extractSmbFlacCover(SmbStreamHandle& h, std::vector<uint8_t>& image) {
    uint64_t off = 0;
    if (!smbFlacPayloadOffset(h, off)) return false;
    off += 4;
    for (;;) {
        uint8_t mh[4];
        if (!smbReadAtFully(h, off, mh, sizeof(mh))) break;
        const int type = mh[0] & 0x7f;
        const bool last = (mh[0] & 0x80) != 0;
        const uint32_t len = ((uint32_t)mh[1] << 16) | ((uint32_t)mh[2] << 8) | mh[3];
        off += 4;
        if (len > 16u * 1024u * 1024u) break;
        if (type == 6 && len > 32) {
            std::vector<uint8_t> block;
            if (!smbReadBlock(h, off, len, block)) break;
            const uint8_t* p = block.data();
            const uint8_t* end = p + block.size();
            if (p + 8 > end) break;
            p += 4;
            const uint32_t mimeLen = rd32beMeta(p); p += 4;
            if (p + mimeLen + 4 > end) break;
            p += mimeLen;
            const uint32_t descLen = rd32beMeta(p); p += 4;
            if (p + descLen + 20 > end) break;
            p += descLen + 16;
            const uint32_t dataLen = rd32beMeta(p); p += 4;
            if (p + dataLen <= end && dataLen > 16) {
                image.assign(p, p + dataLen);
                return true;
            }
        }
        off += len;
        if (last) break;
    }
    return false;
}

static bool extractSmbWavCover(SmbStreamHandle& h, std::vector<uint8_t>& image) {
    uint8_t riff[12];
    if (!smbReadAtFully(h, 0, riff, sizeof(riff)) ||
        std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) return false;
    uint64_t pos = 12;
    while (pos + 8 <= h.size) {
        uint8_t ch[8];
        if (!smbReadAtFully(h, pos, ch, sizeof(ch))) break;
        char idBuf[5] = {0};
        std::memcpy(idBuf, ch, 4);
        const std::string idLower = lowerCopy(std::string(idBuf));
        const uint32_t len = rd32leMeta(ch + 4);
        pos += 8;
        if ((idLower == "id3 " || idLower == "id3") && len <= 16u * 1024u * 1024u && pos + len <= h.size) {
            std::vector<uint8_t> block;
            if (!smbReadBlock(h, pos, len, block)) break;
            if (extractId3CoverFromBlock(block.data(), block.size(), image)) return true;
        }
        pos += (uint64_t)len + (len & 1u);
    }
    return false;
}

static bool extractSmbCover(const std::string& url, const AppConfig& cfg, AudioFmt fmt, std::vector<uint8_t>& image) {
    SmbStreamHandle h;
    if (!openSmbStreamFromUrl(url, h, cfg)) return false;
    if (fmt == AudioFmt::None) fmt = fmtForName(url);
    bool ok = false;
    switch (fmt) {
        case AudioFmt::Mp3:  ok = extractSmbId3Cover(h, image); break;
        case AudioFmt::Flac: ok = extractSmbFlacCover(h, image); break;
        case AudioFmt::Wav:  ok = extractSmbWavCover(h, image); break;
        default: break;
    }
    h.close();
    return ok;
}

static bool initSmbStreamDecoder(const std::string& url, const AppConfig& cfg, StreamDecoder*& out,
                                 AudioFmt* fmtOut, int* chOut, int* srOut, drmp3_uint64* framesOut) {
    out = nullptr;
    StreamDecoder* d = new StreamDecoder();
    if (!openSmbStreamFromUrl(url, d->smb, cfg)) { delete d; return false; }
    AudioFmt fmt = fmtForName(url);
    if (fmt == AudioFmt::None) fmt = AudioFmt::Mp3;
    d->fmt = fmt;
    bool ok = false;
    int ch = 0, sr = 0;
    drmp3_uint64 frames = 0;
    switch (fmt) {
        case AudioFmt::Mp3:
            ok = drmp3_init(&d->mp3, smbStreamRead, smbMp3Seek, smbMp3Tell, nullptr, &d->smb, nullptr) != 0;
            if (ok) {
                d->mp3Ready = true;
                ch = (int)d->mp3.channels; sr = (int)d->mp3.sampleRate;
                frames = d->mp3.totalPCMFrameCount == DRMP3_UINT64_MAX ? 0 : d->mp3.totalPCMFrameCount;
            }
            break;
        case AudioFmt::Flac:
            d->flac = drflac_open(smbStreamRead, smbFlacSeek, smbFlacTell, &d->smb, nullptr);
            ok = d->flac != nullptr;
            if (ok) { ch = (int)d->flac->channels; sr = (int)d->flac->sampleRate; frames = (drmp3_uint64)d->flac->totalPCMFrameCount; }
            break;
        case AudioFmt::Wav:
            ok = drwav_init(&d->wav, smbStreamRead, smbWavSeek, smbWavTell, &d->smb, nullptr) != 0;
            if (ok) { d->wavReady = true; ch = (int)d->wav.channels; sr = (int)d->wav.sampleRate; frames = (drmp3_uint64)d->wav.totalPCMFrameCount; }
            break;
        default:
            ok = false;
            break;
    }
    if (!ok || ch <= 0 || sr <= 0) { d->close(); delete d; return false; }
    *fmtOut = fmt; *chOut = ch; *srOut = sr; *framesOut = frames;
    out = d;
    return true;
}
#endif // AMPINTOSH_SMB

#ifdef AMPINTOSH_SMB
static bool listSmbDirectory(const NetworkBookmark& b, const std::string& remotePath, std::vector<ListedDirEntry>& out, const AppConfig& cfg) {
    std::string share, pathInShare;
    if (!splitSmbSharePath(b, remotePath, share, pathInShare)) {
        gNetLastError = "SMB: no share in path (use server/share/folder)";
        return false;
    }
    struct smb2_context* smb = smb2_init_context();
    if (!smb) { gNetLastError = "SMB: smb2_init_context failed"; return false; }
    smb2_set_timeout(smb, cfg.netTimeoutSec > 0 ? cfg.netTimeoutSec : 25);
    smb2_set_security_mode(smb, SMB2_NEGOTIATE_SIGNING_ENABLED); // many servers require signing
    if (!b.username.empty()) smb2_set_user(smb, b.username.c_str());
    if (!b.password.empty()) smb2_set_password(smb, b.password.c_str());
    const std::string host = cleanRemoteServerHost(b.server);
    const char* user = b.username.empty() ? nullptr : b.username.c_str();
    if (smb2_connect_share(smb, host.c_str(), share.c_str(), user) != 0) {
        gNetLastError = std::string("SMB connect: ") + smb2_get_error(smb);
        smb2_destroy_context(smb);
        return false;
    }
    struct smb2dir* dir = smb2_opendir(smb, pathInShare.c_str());
    if (!dir && pathInShare == "/") dir = smb2_opendir(smb, "");
    if (!dir) {
        gNetLastError = std::string("SMB opendir: ") + smb2_get_error(smb);
        smb2_disconnect_share(smb);
        smb2_destroy_context(smb);
        return false;
    }
    bool opened = true;
    while (struct smb2dirent* de = smb2_readdir(smb, dir)) {
        if (!de->name) continue;
        FsPathKind kind = FsPathKind::Other;
        bool known = true;
        if (de->st.smb2_type == SMB2_TYPE_DIRECTORY) kind = FsPathKind::Directory;
        else if (de->st.smb2_type == SMB2_TYPE_FILE) kind = FsPathKind::File;
        else known = false;
        addListedDirEntry(out, de->name, kind, known);
    }
    smb2_closedir(smb, dir);
    smb2_disconnect_share(smb);
    smb2_destroy_context(smb);
    gNetLastError.clear();
    return opened;
}
#endif // AMPINTOSH_SMB

static bool listNetworkBookmarkDirectory(const NetworkBookmark& b, const std::string& remotePath, std::vector<ListedDirEntry>& out, const AppConfig& cfg) {
    out.clear();
    std::string type = lowerCopy(trim(b.type));
    if (type == "ssh") type = "sftp";
    if (type == "smb2") type = "smb";
#ifdef AMPINTOSH_SFTP
    if (type == "sftp") return listSftpDirectory(b, remotePath, out, cfg);
#endif
#ifdef AMPINTOSH_SMB
    if (type == "smb") return listSmbDirectory(b, remotePath, out, cfg);
#endif
    (void)b; (void)remotePath; (void)cfg;
#if defined(AMPINTOSH_SMB) || defined(AMPINTOSH_SFTP)
    gNetLastError = "Unknown bookmark type '" + type + "' (use smb or sftp)";
#else
    gNetLastError = type + ": support not built into this binary";
#endif
    return false;
}
#endif // AMPINTOSH_NET

static std::vector<BrowserItem> buildBrowserItems(const std::string& currentDir,
                                                  SourceKind source,
                                                  const std::vector<Entry>& entries,
                                                  const AppConfig& cfg) {
    std::vector<BrowserItem> out;

    if (isBrowserSourceMenu(currentDir)) return out;

    if (isBrowserRoot(currentDir)) {
        BrowserItem up;
        up.kind = BrowserItemKind::Up;
        up.label = ".. Source Menu";
        up.path = BROWSER_SOURCE_MENU;
        out.push_back(up);

        const std::vector<std::string> roots = expandedMusicRootsForSource(cfg, source);
        for (const std::string& root : roots) {
            const int n = browserTrackCountForDir(entries, root, cfg);
            if (!pathExistsAsDir(root) && n <= 0) continue;
            BrowserItem item;
            item.kind = BrowserItemKind::Directory;
            item.label = pathLeaf(root);
            item.path = root;
            item.trackCount = n;
            out.push_back(item);
        }

        // If the configured roots were missing or hidden behind an alias, still
        // expose scanned folders for this source so the user is never trapped in
        // the first music folder that happened to have files.
        if (out.size() == 1) {
            std::vector<std::string> seen;
            for (const Entry& e : entries) {
                if (!entryBelongsToSource(e, cfg, source) || e.dir.empty()) continue;
                const std::string d = stripTrailingSlashes(e.dir);
                if (std::find(seen.begin(), seen.end(), d) != seen.end()) continue;
                seen.push_back(d);
                BrowserItem item;
                item.kind = BrowserItemKind::Directory;
                item.label = pathLeaf(d);
                item.path = d;
                item.trackCount = browserTrackCountForDir(entries, d, cfg);
                out.push_back(item);
            }
        }

        sortBrowserItems(out);
        return out;
    }

    BrowserItem up;
    up.kind = BrowserItemKind::Up;
    up.label = "..";
    up.path = isBrowserNet(currentDir) ? std::string(BROWSER_SOURCE_MENU) : parentDirectory(currentDir);
    out.push_back(up);

    if (isBrowserNet(currentDir)) {
        for (int i = 0; i < (int)cfg.networkBookmarks.size(); ++i) {
            const NetworkBookmark& b = cfg.networkBookmarks[i];
            BrowserItem item;
            item.kind = BrowserItemKind::StreamFolder;
            item.label = b.sectionName.empty() ? (b.type + " " + b.server) : b.sectionName;
            item.path = std::string(BROWSER_NETBOOKMARK) + std::to_string(i);
            const std::string url = networkBookmarkURL(b);
            item.trackCount = isPlayableAudioFile(url) ? 1 : 0;
            out.push_back(item);
        }
        for (int i = 0; i < (int)entries.size(); ++i) {
            if (!entries[i].isURL) continue;
            BrowserItem item;
            item.kind = BrowserItemKind::Stream;
            item.label = entries[i].label;
            item.path = entries[i].ref;
            item.entryIndex = i;
            out.push_back(item);
        }
        sortBrowserItems(out);
        return out;
    }

    if (isNetworkBookmarkPath(currentDir)) {
        int nb = -1;
        std::string currentRemote;
        splitNetworkBookmarkBrowserPath(currentDir, nb, currentRemote);
        if (nb >= 0 && nb < (int)cfg.networkBookmarks.size()) {
            const NetworkBookmark& b = cfg.networkBookmarks[nb];
            if (currentRemote.empty()) currentRemote = remoteStartPathForBookmark(b);

            BrowserItem info;
            info.kind = BrowserItemKind::Up;
            const std::string parentRemote = remoteParentPath(b, currentRemote);
            info.label = parentRemote.empty() ? ".. Network" : "..";
            info.path = parentRemote.empty() ? std::string(BROWSER_NET) : networkBookmarkBrowserPath(nb, parentRemote);
            out[0] = info;

            const std::string directUrl = networkBookmarkURLWithPath(b, currentRemote);
            if (!directUrl.empty() && isPlayableAudioFile(directUrl)) {
                BrowserItem item;
                item.kind = BrowserItemKind::Stream;
                item.label = b.sectionName.empty() ? displayName(directUrl) : b.sectionName;
                item.path = directUrl;
                item.entryIndex = findEntryIndexByRef(entries, directUrl);
                out.push_back(item);
                sortBrowserItems(out);
                return out;
            }

#ifdef AMPINTOSH_NET
            std::vector<ListedDirEntry> listed;
            if (listNetworkBookmarkDirectory(b, currentRemote, listed, cfg)) {
                for (const ListedDirEntry& ent : listed) {
                    const std::string& name = ent.name;
                    if (name == "." || name == ".." || (!name.empty() && name[0] == '.')) continue;
                    const std::string remoteChild = joinRemotePath(currentRemote, name);
                    FsPathKind kind = ent.kindKnown ? ent.kind : FsPathKind::Other;
                    const std::string childUrl = networkBookmarkURLWithPath(b, remoteChild);
                    if (kind == FsPathKind::Directory) {
                        BrowserItem item;
                        item.kind = BrowserItemKind::StreamFolder;
                        item.label = name;
                        item.path = networkBookmarkBrowserPath(nb, remoteChild);
                        item.trackCount = 0;
                        out.push_back(item);
                    } else if (isPlayableAudioLeafName(name) || isPlayableAudioFile(childUrl)) {
                        BrowserItem item;
                        item.kind = BrowserItemKind::Stream;
                        item.label = asciiFallbackLabel(displayName(name));
                        item.path = childUrl;
                        item.entryIndex = findEntryIndexByRef(entries, childUrl);
                        out.push_back(item);
                    }
                }
            } else
#endif
            {
                BrowserItem item;
                item.kind = BrowserItemKind::StreamFolder;
#if defined(AMPINTOSH_SMB) || defined(AMPINTOSH_SFTP)
                item.label = gNetLastError.empty() ? "Remote browser unavailable" : gNetLastError;
#elif defined(AMPINTOSH_NET)
                item.label = "SMB/SFTP not built in - install libsmb2/libssh2 and rebuild";
#else
                item.label = "Network disabled at build time";
#endif
                item.path = currentDir;
                item.trackCount = 0;
                out.push_back(item);
            }
        }
        sortBrowserItems(out);
        return out;
    }

    std::vector<ListedDirEntry> listed;
    if (!listDirectoryEntries(currentDir, listed)) {
        sortBrowserItems(out);
        return out;
    }

    for (const ListedDirEntry& ent : listed) {
        const std::string& name = ent.name;
        if (name == "." || name == ".." || (!name.empty() && name[0] == '.')) continue;
        const std::string full = joinPath(currentDir, name);

        if (isPlayableAudioLeafName(name) || isPlayableAudioFile(full)) {
            const int idx = findEntryIndexByRef(entries, full);
            BrowserItem item;
            item.kind = BrowserItemKind::Track;
            item.label = asciiFallbackLabel((idx >= 0 && idx < (int)entries.size()) ? entries[idx].label : displayName(full));
            item.path = full;
            item.entryIndex = idx;
            out.push_back(item);
            continue;
        }

        FsPathKind kind = ent.kindKnown ? ent.kind : classifyFilesystemPath(full);
        if (kind == FsPathKind::Directory) {
            BrowserItem item;
            item.kind = BrowserItemKind::Directory;
            item.label = name;
            item.path = full;
            item.trackCount = browserTrackCountForDir(entries, full, cfg);
            out.push_back(item);
        }
    }
    sortBrowserItems(out);
    return out;
}



static uint32_t rd32beRaw(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t rd24beRaw(const uint8_t* p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static uint32_t rdSyncSafe28(const uint8_t* p) {
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) << 7)  |  (uint32_t)(p[3] & 0x7f);
}

static void undoId3Unsync(std::vector<uint8_t>& data) {
    size_t w = 0;
    for (size_t r = 0; r < data.size(); ++r) {
        data[w++] = data[r];
        if (data[r] == 0xff && r + 1 < data.size() && data[r + 1] == 0x00) ++r;
    }
    data.resize(w);
}

static bool extractId3CoverFromBlock(const uint8_t* block, size_t blockSize, std::vector<uint8_t>& image) {
    if (!block || blockSize < 10 || std::memcmp(block, "ID3", 3) != 0) return false;
    const int version = block[3];
    if (version < 3 || version > 4) return false; // APIC is ID3v2.3/2.4 here.
    const bool unsync = (block[5] & 0x80) != 0;
    const uint32_t tagSize = rdSyncSafe28(block + 6);
    if (tagSize == 0 || tagSize > 16u * 1024u * 1024u || 10ull + tagSize > blockSize) return false;
    std::vector<uint8_t> tag(block + 10, block + 10 + tagSize);
    if (unsync) undoId3Unsync(tag);

    for (size_t pos = 0; pos + 10 <= tag.size();) {
        if (tag[pos] == 0) break;
        const char* id = (const char*)tag.data() + pos;
        const uint32_t frameSize = (version == 4) ? rdSyncSafe28(tag.data() + pos + 4) : rd32beRaw(tag.data() + pos + 4);
        const size_t dataPos = pos + 10;
        if (frameSize == 0 || dataPos + frameSize > tag.size()) break;
        if (std::memcmp(id, "APIC", 4) == 0 && frameSize > 8) {
            const uint8_t* fr = tag.data() + dataPos;
            const size_t n = frameSize;
            size_t i = 1;                                  // encoding byte.
            while (i < n && fr[i] != 0) ++i;               // MIME string.
            if (i >= n) return false;
            ++i;                                           // after MIME terminator.
            if (i >= n) return false;
            ++i;                                           // picture type.
            const uint8_t enc = fr[0];
            if (enc == 0 || enc == 3) {                    // ISO-8859-1 / UTF-8 desc.
                while (i < n && fr[i] != 0) ++i;
                if (i < n) ++i;
            } else {                                       // UTF-16 desc, two-byte terminator.
                while (i + 1 < n && !(fr[i] == 0 && fr[i + 1] == 0)) i += 2;
                if (i + 1 < n) i += 2;
            }
            if (i < n && n - i > 16) {
                image.assign(fr + i, fr + n);
                return true;
            }
        }
        pos = dataPos + frameSize;
    }
    return false;
}

static bool extractMp3Cover(const std::string& path, std::vector<uint8_t>& image) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t hdr[10];
    if (std::fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { std::fclose(f); return false; }
    if (std::memcmp(hdr, "ID3", 3) != 0) { std::fclose(f); return false; }
    const uint32_t tagSize = rdSyncSafe28(hdr + 6);
    if (tagSize == 0 || tagSize > 16u * 1024u * 1024u) { std::fclose(f); return false; }
    std::vector<uint8_t> block(10 + tagSize);
    std::memcpy(block.data(), hdr, 10);
    const bool ok = std::fread(block.data() + 10, 1, tagSize, f) == tagSize;
    std::fclose(f);
    return ok && extractId3CoverFromBlock(block.data(), block.size(), image);
}

static bool extractFlacCover(const std::string& path, std::vector<uint8_t>& image) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    if (!seekPastOptionalId3v2Prefix(f)) { std::fclose(f); return false; }
    uint8_t magic[4];
    if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "fLaC", 4) != 0) {
        std::fclose(f); return false;
    }

    for (;;) {
        uint8_t h[4];
        if (std::fread(h, 1, 4, f) != 4) break;
        const int type = h[0] & 0x7f;
        const bool last = (h[0] & 0x80) != 0;
        const uint32_t len = rd24beRaw(h + 1);
        if (len > 16u * 1024u * 1024u) break;
        std::vector<uint8_t> block(len);
        if (std::fread(block.data(), 1, block.size(), f) != block.size()) break;
        if (type == 6 && len > 32) {                       // METADATA_BLOCK_PICTURE.
            const uint8_t* p = block.data();
            const uint8_t* end = p + block.size();
            if (p + 8 > end) break;
            p += 4;                                       // picture type.
            const uint32_t mimeLen = rd32beRaw(p); p += 4;
            if (p + mimeLen + 4 > end) break;
            p += mimeLen;
            const uint32_t descLen = rd32beRaw(p); p += 4;
            if (p + descLen + 20 > end) break;
            p += descLen + 16;                            // width/height/depth/colors.
            const uint32_t dataLen = rd32beRaw(p); p += 4;
            if (p + dataLen <= end && dataLen > 16) {
                image.assign(p, p + dataLen);
                std::fclose(f);
                return true;
            }
        }
        if (last) break;
    }
    std::fclose(f);
    return false;
}

static bool extractWavCover(const std::string& path, std::vector<uint8_t>& image) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t riff[12];
    if (std::fread(riff, 1, sizeof(riff), f) != sizeof(riff) ||
        std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        std::fclose(f); return false;
    }
    bool found = false;
    for (;;) {
        uint8_t h[8];
        if (std::fread(h, 1, 8, f) != 8) break;
        char idBuf[5] = {0};
        std::memcpy(idBuf, h, 4);
        const std::string id(idBuf);
        const std::string idLower = lowerCopy(id);
        const uint32_t len = rd32leMeta(h + 4);
        if ((idLower == "id3 " || idLower == "id3") && len <= 16u * 1024u * 1024u) {
            std::vector<uint8_t> block(len);
            if (std::fread(block.data(), 1, block.size(), f) != block.size()) break;
            found = extractId3CoverFromBlock(block.data(), block.size(), image);
            if (found) break;
        } else {
            if (std::fseek(f, (long)len, SEEK_CUR) != 0) break;
        }
        if (len & 1u) std::fseek(f, 1, SEEK_CUR);
    }
    std::fclose(f);
    return found;
}

static std::string parentDirOf(const std::string& path) {
    return directoryName(path);
}

static std::string stemOf(const std::string& path) {
    std::string base = displayName(path);
    return base;
}

static bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static SDL_Texture* textureFromImageBytes(SDL_Renderer* r, const std::vector<uint8_t>& bytes, int* w, int* h) {
    if (bytes.empty()) return nullptr;
    SDL_RWops* rw = SDL_RWFromConstMem(bytes.data(), (int)bytes.size());
    if (!rw) return nullptr;
    SDL_Surface* surf = IMG_Load_RW(rw, 1);
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
    if (tex) { *w = surf->w; *h = surf->h; }
    SDL_FreeSurface(surf);
    return tex;
}

static SDL_Texture* textureFromImagePath(SDL_Renderer* r, const std::string& path, int* w, int* h) {
    SDL_Surface* surf = IMG_Load(path.c_str());
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
    if (tex) { *w = surf->w; *h = surf->h; }
    SDL_FreeSurface(surf);
    return tex;
}

static void clearCoverArt(CoverArt& art) {
    if (art.texture) SDL_DestroyTexture(art.texture);
    art.texture = nullptr;
    art.w = art.h = 0;
    art.source.clear();
}

static void loadCoverArtForEntry(SDL_Renderer* r, CoverArt& art, const Entry& e, AudioFmt fmt, const AppConfig& cfg) {
    clearCoverArt(art);
    if (!r) return;

#ifdef AMPINTOSH_NET
#if defined(AMPINTOSH_SMB)
    if (e.isURL && startsWith(lowerCopy(e.ref), "smb://")) {
        std::vector<uint8_t> image;
        if (extractSmbCover(e.ref, cfg, fmt, image)) {
            art.texture = textureFromImageBytes(r, image, &art.w, &art.h);
            if (art.texture) art.source = "embedded";
        }
        return;
    }
#endif
#endif
    if (e.isURL) return;

    const std::string dir = parentDirOf(e.ref);
    const std::string stem = stemOf(e.ref);
    std::vector<std::string> candidates;
    for (const char* name : { "cover", "folder", "front", "album", "AlbumArt", stem.c_str() }) {
        candidates.push_back(joinPath(dir, std::string(name) + ".jpg"));
        candidates.push_back(joinPath(dir, std::string(name) + ".jpeg"));
        candidates.push_back(joinPath(dir, std::string(name) + ".png"));
    }
    for (const std::string& c : candidates) {
        if (!fileExists(c)) continue;
        art.texture = textureFromImagePath(r, c, &art.w, &art.h);
        if (art.texture) { art.source = "sidecar"; return; }
    }

    std::vector<uint8_t> image;
    if ((fmt == AudioFmt::Mp3 && extractMp3Cover(e.ref, image)) ||
        (fmt == AudioFmt::Flac && extractFlacCover(e.ref, image)) ||
        (fmt == AudioFmt::Wav && extractWavCover(e.ref, image))) {
        art.texture = textureFromImageBytes(r, image, &art.w, &art.h);
        if (art.texture) art.source = "embedded";
    }
}

// Minimal AIFF/AIFC PCM decoder. This intentionally handles the common, safe
// uncompressed cases used by DAWs and converters: AIFF "NONE" big-endian PCM
// and AIFC "sowt" little-endian PCM. Anything compressed is skipped instead of
// being mis-decoded.
static uint16_t rd16be(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32be(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t rd16le(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32le(const uint8_t* p) { return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }

static double rdExtended80(const uint8_t* p) {
    const int sign = (p[0] & 0x80) ? -1 : 1;
    const int exp  = ((p[0] & 0x7f) << 8) | p[1];
    uint64_t mant = 0;
    for (int i = 0; i < 8; ++i) mant = (mant << 8) | p[2 + i];
    if (exp == 0 && mant == 0) return 0.0;
    const double frac = (double)mant / 9223372036854775808.0; // 2^63, includes int bit
    return sign * std::ldexp(frac, exp - 16383);
}

static int32_t signExtend(uint32_t v, int bits) {
    if (bits >= 32) return (int32_t)v;
    const uint32_t m = 1u << (bits - 1);
    return (int32_t)((v ^ m) - m);
}

static float* decodeAiffToF32(const std::string& path, int* channels, int* sampleRate, drmp3_uint64* frames) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    std::fseek(f, 0, SEEK_END);
    const long szLong = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (szLong < 54) { std::fclose(f); return nullptr; }
    std::vector<uint8_t> bytes((size_t)szLong);
    if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) { std::fclose(f); return nullptr; }
    std::fclose(f);

    if (std::memcmp(bytes.data(), "FORM", 4) != 0) return nullptr;
    const bool isAIFF = std::memcmp(bytes.data() + 8, "AIFF", 4) == 0;
    const bool isAIFC = std::memcmp(bytes.data() + 8, "AIFC", 4) == 0;
    if (!isAIFF && !isAIFC) return nullptr;

    int ch = 0, bits = 0, sr = 0;
    uint32_t frameCount = 0;
    bool little = false;
    const uint8_t* ssnd = nullptr;
    size_t ssndBytes = 0;

    for (size_t pos = 12; pos + 8 <= bytes.size();) {
        const uint8_t* id = bytes.data() + pos;
        const uint32_t chunkSize = rd32be(bytes.data() + pos + 4);
        const size_t dataPos = pos + 8;
        if (dataPos + chunkSize > bytes.size()) break;

        if (std::memcmp(id, "COMM", 4) == 0 && chunkSize >= 18) {
            const uint8_t* c = bytes.data() + dataPos;
            ch = (int)rd16be(c);
            frameCount = rd32be(c + 2);
            bits = (int)rd16be(c + 6);
            sr = (int)std::lround(rdExtended80(c + 8));
            if (isAIFC && chunkSize >= 22) {
                const char* comp = (const char*)(c + 18);
                if (std::memcmp(comp, "sowt", 4) == 0) little = true;
                else if (std::memcmp(comp, "NONE", 4) != 0) return nullptr;
            }
        } else if (std::memcmp(id, "SSND", 4) == 0 && chunkSize >= 8) {
            const uint32_t offset = rd32be(bytes.data() + dataPos);
            const size_t audioPos = dataPos + 8u + offset;
            if (audioPos <= dataPos + chunkSize && audioPos <= bytes.size()) {
                ssnd = bytes.data() + audioPos;
                ssndBytes = dataPos + chunkSize - audioPos;
            }
        }
        pos = dataPos + chunkSize + (chunkSize & 1u);
    }

    if (!ssnd || ch <= 0 || ch > 8 || sr <= 0 || frameCount == 0) return nullptr;
    if (!(bits == 8 || bits == 16 || bits == 24 || bits == 32)) return nullptr;
    const int bytesPerSample = bits / 8;
    const uint64_t needed = (uint64_t)frameCount * (uint64_t)ch * (uint64_t)bytesPerSample;
    if (needed == 0 || needed > ssndBytes) {
        frameCount = (uint32_t)(ssndBytes / ((uint64_t)ch * (uint64_t)bytesPerSample));
        if (frameCount == 0) return nullptr;
    }

    const uint64_t sampleCount = (uint64_t)frameCount * (uint64_t)ch;
    float* out = (float*)std::malloc((size_t)(sampleCount * sizeof(float)));
    if (!out) return nullptr;

    for (uint64_t i = 0; i < sampleCount; ++i) {
        const uint8_t* s = ssnd + i * bytesPerSample;
        uint32_t raw = 0;
        if (bits == 8) raw = s[0];
        else if (bits == 16) raw = little ? rd16le(s) : rd16be(s);
        else if (bits == 24) {
            raw = little ? ((uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16))
                         : (((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2]);
        } else raw = little ? rd32le(s) : rd32be(s);
        const int32_t signedSample = signExtend(raw, bits);
        const double denom = bits == 32 ? 2147483648.0 : (double)(1u << (bits - 1));
        double v = (double)signedSample / denom;
        if (v < -1.0) v = -1.0;
        if (v >  1.0) v =  1.0;
        out[i] = (float)v;
    }

    *channels = ch;
    *sampleRate = sr;
    *frames = (drmp3_uint64)frameCount;
    return out;
}

// Decode any supported file to interleaved float PCM. Channels/rate/frames are
// returned via out-params. Caller frees with freeDecoded() using the same fmt.

static drmp3_uint64 streamDecodeFrames(StreamDecoder* s, float* out, drmp3_uint64 framesToRead) {
#if defined(AMPINTOSH_NET) && defined(AMPINTOSH_SMB)
    if (!s || !out || framesToRead == 0) return 0;
    switch (s->fmt) {
        case AudioFmt::Mp3:  return s->mp3Ready ? drmp3_read_pcm_frames_f32(&s->mp3, framesToRead, out) : 0;
        case AudioFmt::Flac: return s->flac ? (drmp3_uint64)drflac_read_pcm_frames_f32(s->flac, framesToRead, out) : 0;
        case AudioFmt::Wav:  return s->wavReady ? (drmp3_uint64)drwav_read_pcm_frames_f32(&s->wav, framesToRead, out) : 0;
        default: return 0;
    }
#else
    (void)s; (void)out; (void)framesToRead;
    return 0;
#endif
}

#if defined(AMPINTOSH_NET) && defined(AMPINTOSH_SMB)
static bool streamDecodeSeekToFrame(StreamDecoder* s, drmp3_uint64 frame) {
    if (!s) return false;
    switch (s->fmt) {
        case AudioFmt::Mp3:  return s->mp3Ready && drmp3_seek_to_pcm_frame(&s->mp3, frame) != 0;
        case AudioFmt::Flac: return s->flac && drflac_seek_to_pcm_frame(s->flac, frame) != 0;
        case AudioFmt::Wav:  return s->wavReady && drwav_seek_to_pcm_frame(&s->wav, frame) != 0;
        default: return false;
    }
}

static size_t streamRingFreeFramesLocked(const StreamDecoder* s) {
    return s && s->ringFrames >= s->ringFill ? (s->ringFrames - s->ringFill) : 0;
}

static void streamRingResetLocked(StreamDecoder* s) {
    if (!s) return;
    s->ringRead = s->ringWrite = s->ringFill = 0;
}

static void streamRingWriteLocked(StreamDecoder* s, const float* src, size_t frames) {
    const size_t ch = (size_t)std::max(1, s->pumpChannels);
    while (frames > 0 && s->ringFrames > 0) {
        const size_t contiguous = std::min(frames, s->ringFrames - s->ringWrite);
        std::memcpy(&s->ring[s->ringWrite * ch], src, contiguous * ch * sizeof(float));
        s->ringWrite = (s->ringWrite + contiguous) % s->ringFrames;
        s->ringFill += contiguous;
        src += contiguous * ch;
        frames -= contiguous;
    }
}

static int streamPumpThreadMain(void* userdata) {
    StreamDecoder* s = static_cast<StreamDecoder*>(userdata);
    if (!s || !s->pumpMutex || s->ringFrames == 0) return 1;
    const int ch = std::max(1, s->pumpChannels);
    const int chunkFrames = std::max(512, s->pumpChunkFrames);
    std::vector<float> scratch((size_t)chunkFrames * (size_t)ch);

    for (;;) {
        bool doSeek = false;
        drmp3_uint64 targetFrame = 0;

        SDL_LockMutex(s->pumpMutex);
        if (s->seekPending) {
            doSeek = true;
            targetFrame = s->seekFrame;
            s->seekPending = false;
            s->pumpEof = false;
            s->pumpPrimed = false;
            s->pumpEverPrimed = true;
            s->pumpBuffering = true;
            streamRingResetLocked(s);
            SDL_CondBroadcast(s->pumpCond);
        }
        while (!s->pumpStop && !doSeek && streamRingFreeFramesLocked(s) < (size_t)std::min(512, chunkFrames)) {
            SDL_CondWait(s->pumpCond, s->pumpMutex);
            if (s->seekPending) {
                doSeek = true;
                targetFrame = s->seekFrame;
                s->seekPending = false;
                s->pumpEof = false;
                s->pumpPrimed = false;
                s->pumpEverPrimed = true;
                s->pumpBuffering = true;
                streamRingResetLocked(s);
                SDL_CondBroadcast(s->pumpCond);
                break;
            }
        }
        if (s->pumpStop) { SDL_UnlockMutex(s->pumpMutex); break; }
        const size_t want = doSeek ? 0 : std::min((size_t)chunkFrames, streamRingFreeFramesLocked(s));
        SDL_UnlockMutex(s->pumpMutex);

        if (doSeek) {
            const bool ok = streamDecodeSeekToFrame(s, targetFrame);
            SDL_LockMutex(s->pumpMutex);
            if (!ok) s->pumpFailed = true;
            SDL_CondBroadcast(s->pumpCond);
            SDL_UnlockMutex(s->pumpMutex);
            if (!ok) break;
            continue;
        }

        const drmp3_uint64 got64 = streamDecodeFrames(s, scratch.data(), (drmp3_uint64)want);
        const size_t got = (size_t)got64;

        SDL_LockMutex(s->pumpMutex);
        if (got == 0) {
            s->pumpEof = true;
            SDL_CondBroadcast(s->pumpCond);
            SDL_UnlockMutex(s->pumpMutex);
            break;
        }
        streamRingWriteLocked(s, scratch.data(), got);
        SDL_CondBroadcast(s->pumpCond);
        SDL_UnlockMutex(s->pumpMutex);
    }
    return 0;
}

static bool startStreamDecoderPump(StreamDecoder* s, int channels, int sampleRate,
                                   drmp3_uint64 totalFrames, const AppConfig& cfg) {
    if (!s || channels <= 0 || sampleRate <= 0) return false;
    s->pumpChannels = channels;
    s->pumpChunkFrames = std::max(1024, sampleRate / 20); // about 50 ms per decode pull.

    const int boundedBufferMs = std::max(5000, std::min(cfg.smbStreamBufferMs, 120000));
    const int pct = std::max(1, std::min(cfg.smbStreamPrebufferPercent, 50));
    const int maxPreMs = std::max(1000, std::min(cfg.smbStreamPrebufferMaxMs, 60000));

    size_t prebufferFrames = (size_t)std::max(sampleRate / 2, sampleRate * 2); // fallback ~2s.
    if (totalFrames > 0) {
        prebufferFrames = (size_t)std::max<drmp3_uint64>(sampleRate / 2, (totalFrames * (drmp3_uint64)pct) / 100);
        prebufferFrames = std::min(prebufferFrames, (size_t)((int64_t)sampleRate * maxPreMs / 1000));
    }

    size_t bufferFrames = (size_t)std::max((int64_t)sampleRate * boundedBufferMs / 1000,
                                           (int64_t)prebufferFrames + (int64_t)sampleRate * 5);
    bufferFrames = std::max(bufferFrames, (size_t)sampleRate * 5);
    // Switch memory is finite; cap decoded ring memory to a sane maximum.
    const size_t maxFramesByMemory = (32u * 1024u * 1024u) / (sizeof(float) * (size_t)channels);
    bufferFrames = std::min(bufferFrames, maxFramesByMemory);
    prebufferFrames = std::min(prebufferFrames, bufferFrames / 2);

    s->ringFrames = bufferFrames;
    s->ring.assign(s->ringFrames * (size_t)channels, 0.0f);
    s->ringRead = s->ringWrite = s->ringFill = 0;
    s->pumpStop = s->pumpEof = s->pumpFailed = s->seekPending = false;
    s->pumpPrimed = false;
    s->pumpEverPrimed = false;
    s->pumpBuffering = true;
    s->seekFrame = 0;
    const size_t target = std::max<size_t>(512, prebufferFrames);
    s->pumpPrebufferTargetFrames = target;
    size_t rebufferFrames = (size_t)std::max(sampleRate / 2, sampleRate * 2);
    rebufferFrames = std::min(rebufferFrames, std::max<size_t>(512, bufferFrames / 3));
    s->pumpRebufferTargetFrames = std::max<size_t>(512, rebufferFrames);
    s->pumpMutex = SDL_CreateMutex();
    s->pumpCond = SDL_CreateCond();
    if (!s->pumpMutex || !s->pumpCond) return false;
    s->pumpThread = SDL_CreateThread(streamPumpThreadMain, "smb-audio-pump", s);
    if (!s->pumpThread) return false;

    // Do not block the UI while the initial 10% prebuffer fills. The audio
    // callback outputs silence until this target is reached, while the render
    // loop shows a buffering indicator and progress.
    (void)maxPreMs;
    return true;
}
#endif

static drmp3_uint64 streamReadFrames(StreamDecoder* s, float* out, drmp3_uint64 framesToRead) {
#if defined(AMPINTOSH_NET) && defined(AMPINTOSH_SMB)
    if (!s || !out || framesToRead == 0) return 0;
    if (!s->pumpMutex || s->ringFrames == 0) return streamDecodeFrames(s, out, framesToRead);
    const size_t ch = (size_t)std::max(1, s->pumpChannels);
    SDL_LockMutex(s->pumpMutex);

    if (!s->pumpPrimed && !s->pumpEof && !s->pumpFailed) {
        const size_t targetFrames = std::max<size_t>(512, s->pumpEverPrimed ? s->pumpRebufferTargetFrames
                                                                            : s->pumpPrebufferTargetFrames);
        if (s->ringFill < targetFrames) {
            s->pumpBuffering = true;
            SDL_CondBroadcast(s->pumpCond);
            SDL_UnlockMutex(s->pumpMutex);
            return 0;
        }
        s->pumpPrimed = true;
        s->pumpEverPrimed = true;
        s->pumpBuffering = false;
    }

    size_t frames = std::min((size_t)framesToRead, s->ringFill);
    if (frames == 0 && !s->pumpEof && !s->pumpFailed) {
        s->pumpPrimed = false;
        s->pumpBuffering = true;
        SDL_CondBroadcast(s->pumpCond);
        SDL_UnlockMutex(s->pumpMutex);
        return 0;
    }

    size_t remaining = frames;
    float* dst = out;
    while (remaining > 0) {
        const size_t contiguous = std::min(remaining, s->ringFrames - s->ringRead);
        std::memcpy(dst, &s->ring[s->ringRead * ch], contiguous * ch * sizeof(float));
        s->ringRead = (s->ringRead + contiguous) % s->ringFrames;
        s->ringFill -= contiguous;
        dst += contiguous * ch;
        remaining -= contiguous;
    }
    if (s->ringFill == 0 && !s->pumpEof && !s->pumpFailed) {
        s->pumpPrimed = false;
        s->pumpBuffering = true;
    }
    SDL_CondBroadcast(s->pumpCond);
    SDL_UnlockMutex(s->pumpMutex);
    return (drmp3_uint64)frames;
#else
    (void)s; (void)out; (void)framesToRead;
    return 0;
#endif
}

static bool streamIsFinished(StreamDecoder* s) {
#if defined(AMPINTOSH_NET) && defined(AMPINTOSH_SMB)
    if (!s) return true;
    if (!s->pumpMutex) return false;
    SDL_LockMutex(s->pumpMutex);
    const bool done = (s->pumpEof || s->pumpFailed) && s->ringFill == 0;
    SDL_UnlockMutex(s->pumpMutex);
    return done;
#else
    (void)s;
    return true;
#endif
}

struct StreamBufferInfo {
    size_t fillFrames = 0;
    size_t capacityFrames = 0;
    size_t targetFrames = 0;
    bool buffering = false;
    bool eof = false;
    bool failed = false;
};

static StreamBufferInfo streamGetBufferInfo(StreamDecoder* s) {
    StreamBufferInfo info;
#if defined(AMPINTOSH_NET) && defined(AMPINTOSH_SMB)
    if (!s || !s->pumpMutex) return info;
    SDL_LockMutex(s->pumpMutex);
    info.fillFrames = s->ringFill;
    info.capacityFrames = s->ringFrames;
    info.eof = s->pumpEof;
    info.failed = s->pumpFailed;
    const size_t targetFrames = std::max<size_t>(512, s->pumpEverPrimed ? s->pumpRebufferTargetFrames
                                                                        : s->pumpPrebufferTargetFrames);
    info.targetFrames = std::min(targetFrames, std::max<size_t>(1, s->ringFrames));
    const bool ready = s->pumpPrimed || info.fillFrames >= info.targetFrames || info.eof || info.failed;
    info.buffering = !ready && !info.failed;
    SDL_UnlockMutex(s->pumpMutex);
#else
    (void)s;
#endif
    return info;
}

static bool streamRequestSeekToFrame(StreamDecoder* s, drmp3_uint64 frame) {
#if defined(AMPINTOSH_NET) && defined(AMPINTOSH_SMB)
    if (!s || !s->pumpMutex) return false;
    SDL_LockMutex(s->pumpMutex);
    s->seekFrame = frame;
    s->seekPending = true;
    s->pumpEof = false;
    s->pumpPrimed = false;
    s->pumpEverPrimed = true;
    s->pumpBuffering = true;
    streamRingResetLocked(s);
    SDL_CondBroadcast(s->pumpCond);
    SDL_UnlockMutex(s->pumpMutex);
    return true;
#else
    (void)s; (void)frame;
    return false;
#endif
}

static void destroyStreamDecoder(StreamDecoder*& s) {
#if defined(AMPINTOSH_NET) && defined(AMPINTOSH_SMB)
    if (s) { s->close(); delete s; s = nullptr; }
#else
    s = nullptr;
#endif
}

static float* decodeToF32(const std::string& path, AudioFmt fmt,
                          int* channels, int* sampleRate, drmp3_uint64* frames) {
    unsigned int ch = 0, sr = 0;
    switch (fmt) {
        case AudioFmt::Mp3: {
            drmp3_config cfg; drmp3_uint64 f = 0;
            float* d = drmp3_open_file_and_read_pcm_frames_f32(path.c_str(), &cfg, &f, nullptr);
            if (d) { *channels = (int)cfg.channels; *sampleRate = (int)cfg.sampleRate; *frames = f; }
            return d;
        }
        case AudioFmt::Flac: {
            drflac_uint64 f = 0;
            float* d = drflac_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &sr, &f, nullptr);
            if (d) { *channels = (int)ch; *sampleRate = (int)sr; *frames = (drmp3_uint64)f; }
            return d;
        }
        case AudioFmt::Wav: {
            drwav_uint64 f = 0;
            float* d = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &sr, &f, nullptr);
            if (d) { *channels = (int)ch; *sampleRate = (int)sr; *frames = (drmp3_uint64)f; }
            return d;
        }
        case AudioFmt::Aiff:
            return decodeAiffToF32(path, channels, sampleRate, frames);
        default: return nullptr;
    }
}

static void freeDecoded(float* p, AudioFmt fmt) {
    if (!p) return;
    switch (fmt) {
        case AudioFmt::Mp3:  drmp3_free(p, nullptr);  break;
        case AudioFmt::Flac: drflac_free(p, nullptr); break;
        case AudioFmt::Wav:  drwav_free(p, nullptr);  break;
        case AudioFmt::Aiff: std::free(p);             break;
        default: break;
    }
}

static void unloadPlayer(Player& p) {
    if (p.dev) { SDL_PauseAudioDevice(p.dev, 1); SDL_CloseAudioDevice(p.dev); p.dev = 0; }
    freeDecoded(p.pcm, p.fmt);
    p.pcm = nullptr;
    destroyStreamDecoder(p.stream);
    p.streaming = false;
    p.totalFrames = 0;
    p.channels = 2;
    p.sampleRate = 44100;
    p.fmt = AudioFmt::None;
    p.cursor.store(0);
    p.recentWrite = 0;
    std::memset(p.recentMono, 0, sizeof(p.recentMono));
    p.playing.store(false);
    p.repeatOne.store(false);
    p.finished.store(false);
}

// Decode a file fully, then (re)open the audio device to match the track's own
// sample rate and channel count, so mono / 48 kHz / etc. all play correctly.
// Closing the device before swapping the buffer guarantees the callback isn't
// running while we free the old PCM. Returns false on failure.
static bool loadTrack(Player& p, Analyzer& analyzer, const std::string& path, bool startPlaying) {
    const AudioFmt fmt = fmtForLocalFile(path);
    if (fmt == AudioFmt::None) return false;

    int ch = 0, sr = 0;
    drmp3_uint64 frames = 0;
    float* data = decodeToF32(path, fmt, &ch, &sr, &frames);
    if (!data || frames == 0 || ch <= 0 || sr <= 0) {
        freeDecoded(data, fmt);
        return false;
    }

    // Tear down the previous device (stops the callback) before replacing PCM.
    if (p.dev) { SDL_PauseAudioDevice(p.dev, 1); SDL_CloseAudioDevice(p.dev); p.dev = 0; }
    freeDecoded(p.pcm, p.fmt);
    destroyStreamDecoder(p.stream);
    p.streaming = false;

    p.pcm         = data;
    p.totalFrames = frames;
    p.channels    = ch;
    p.sampleRate  = sr;
    p.fmt         = fmt;
    p.cursor.store(0);
    p.recentWrite = 0;
    std::memset(p.recentMono, 0, sizeof(p.recentMono));
    p.finished.store(false);
    analyzer.init(p.sampleRate);

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq     = p.sampleRate;
    want.format   = AUDIO_F32SYS;
    want.channels = (Uint8)p.channels;
    want.samples  = 1024;
    want.callback = audioCallback;
    want.userdata = &p;
    p.dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!p.dev) return false;

    p.playing.store(startPlaying);
    SDL_PauseAudioDevice(p.dev, 0); // callback runs; emits silence while paused
    return true;
}


#if defined(AMPINTOSH_NET) && defined(AMPINTOSH_SMB)
static bool loadSmbStreamTrack(Player& p, Analyzer& analyzer, const std::string& url, const AppConfig& cfg, bool startPlaying) {
    StreamDecoder* stream = nullptr;
    AudioFmt fmt = AudioFmt::None;
    int ch = 0, sr = 0;
    drmp3_uint64 frames = 0;
    if (!initSmbStreamDecoder(url, cfg, stream, &fmt, &ch, &sr, &frames)) return false;

    if (p.dev) { SDL_PauseAudioDevice(p.dev, 1); SDL_CloseAudioDevice(p.dev); p.dev = 0; }
    freeDecoded(p.pcm, p.fmt);
    p.pcm = nullptr;
    destroyStreamDecoder(p.stream);

    if (!startStreamDecoderPump(stream, ch, sr, frames, cfg)) {
        destroyStreamDecoder(stream);
        return false;
    }

    p.stream = stream;
    p.streaming = true;
    p.totalFrames = frames; // may be 0 for some MP3 streams; playback still works.
    p.channels = ch;
    p.sampleRate = sr;
    p.fmt = fmt;
    p.cursor.store(0);
    p.recentWrite = 0;
    std::memset(p.recentMono, 0, sizeof(p.recentMono));
    p.finished.store(false);
    analyzer.init(p.sampleRate);

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq     = p.sampleRate;
    want.format   = AUDIO_F32SYS;
    want.channels = (Uint8)p.channels;
    want.samples  = 4096; // async SMB streaming drains a decoded PCM ring.
    want.callback = audioCallback;
    want.userdata = &p;
    p.dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!p.dev) { unloadPlayer(p); return false; }
    p.playing.store(startPlaying);
    SDL_PauseAudioDevice(p.dev, 0);
    return true;
}
#endif

// Jump playback to a fraction (0..1) of the track. Safe to call from the main
// thread: cursor is atomic and the audio callback re-reads it each buffer.
static void seekToFraction(Player& p, double frac) {
    if (p.totalFrames == 0) return;
    frac = std::min(1.0, std::max(0.0, frac));
    const drmp3_uint64 target = (drmp3_uint64)(frac * (double)(p.totalFrames - 1));
    if (p.streaming) {
        if (!streamRequestSeekToFrame(p.stream, target)) return;
    }
    p.cursor.store(target);
    p.finished.store(false);
}

// Load a library entry. Local files go straight to loadTrack(). SMB entries
// stream through libsmb2 when that backend is present, so playback starts after
// open/header decode instead of after a whole-track cache. Other network URLs
// still use the existing cache fallback.
static bool loadEntry(Player& p, Analyzer& analyzer, const Entry& e, const AppConfig& cfg, bool startPlaying) {
#ifdef AMPINTOSH_NET
    if (e.isURL) {
        if (!cfg.netEnabled || !g_netReady) return false;
        const std::string l = lowerCopy(e.ref);
#if defined(AMPINTOSH_SMB)
        if (startsWith(l, "smb://")) {
            cacheMetadataForRefFromSmbURL(e.ref, cfg);
            AudioFmt f = fmtForName(e.ref);
            if (cfg.smbDirectStreaming &&
                (f == AudioFmt::Mp3 || f == AudioFmt::Flac || f == AudioFmt::Wav || f == AudioFmt::None)) {
                // Async path: a background thread reads/decodes from SMB and fills
                // a decoded PCM ring. Playback starts after a bounded 10% prebuffer,
                // and the SDL callback never touches the network.
                if (loadSmbStreamTrack(p, analyzer, e.ref, cfg, startPlaying)) return true;
            }
            // Fallback path: fetch SMB through libsmb2, then decode locally.
            if (f == AudioFmt::None) f = AudioFmt::Mp3;
            const char* ext = (f == AudioFmt::Flac) ? "flac" : (f == AudioFmt::Wav) ? "wav" : (f == AudioFmt::Aiff) ? "aiff" : "mp3";
            const std::string tmp = joinPath(cfg.cacheDir, std::string(".netcache.") + ext);
            if (!downloadSmbURL(e.ref, tmp, cfg)) return false;
            cacheMetadataForRefFromLocalFile(e.ref, tmp);
            return loadTrack(p, analyzer, tmp, startPlaying);
        }
#endif
        AudioFmt f = fmtForName(e.ref);
        if (f == AudioFmt::None) f = AudioFmt::Mp3;     // assume MP3 if URL has no extension
        const char* ext = (f == AudioFmt::Flac) ? "flac" : (f == AudioFmt::Wav) ? "wav" : (f == AudioFmt::Aiff) ? "aiff" : "mp3";
        const std::string tmp = joinPath(cfg.cacheDir, std::string(".netcache.") + ext);
        if (!downloadURL(e.ref, tmp, cfg)) return false;
        cacheMetadataForRefFromLocalFile(e.ref, tmp);
        return loadTrack(p, analyzer, tmp, startPlaying);
    }
#else
    (void)cfg;
#endif
    return loadTrack(p, analyzer, e.ref, startPlaying);
}


// ---------------------------------------------------------------------------
// Last.fm integration (optional, configured in ampintosh.ini)
// ---------------------------------------------------------------------------
#ifdef AMPINTOSH_NET
struct LastFmJob { std::string body; int timeoutSec = 10; };
static SDL_mutex* gLastFmMutex = nullptr;
static SDL_cond*  gLastFmCond  = nullptr;
static SDL_Thread* gLastFmThread = nullptr;
static bool gLastFmStop = false;
static std::deque<LastFmJob> gLastFmQueue;

struct LastFmPlaybackState {
    std::string ref;
    std::string artist;
    std::string album;
    std::string track;
    int durationSec = 0;
    int startedAt = 0;
    bool scrobbled = false;
};
static LastFmPlaybackState gLastFmCurrent;

struct Md5Ctx { uint32_t a, b, c, d; uint64_t bytes; uint8_t buf[64]; size_t used; };
static uint32_t md5Rol(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }
static void md5Init(Md5Ctx& c) { c.a=0x67452301u; c.b=0xefcdab89u; c.c=0x98badcfeu; c.d=0x10325476u; c.bytes=0; c.used=0; }
static void md5Block(Md5Ctx& c, const uint8_t block[64]) {
    uint32_t x[16];
    for (int i=0;i<16;++i) x[i]=(uint32_t)block[i*4]|((uint32_t)block[i*4+1]<<8)|((uint32_t)block[i*4+2]<<16)|((uint32_t)block[i*4+3]<<24);
    uint32_t a=c.a,b=c.b,cc=c.c,d=c.d;
    auto step=[&](uint32_t f,uint32_t& aa,uint32_t bb,uint32_t xx,uint32_t s,uint32_t ac){ aa += f + xx + ac; aa = md5Rol(aa,s) + bb; };
#define F(x,y,z) (((x)&(y)) | (~(x)&(z)))
#define G(x,y,z) (((x)&(z)) | ((y)&~(z)))
#define H(x,y,z) ((x)^(y)^(z))
#define I(x,y,z) ((y)^((x)|~(z)))
#define S(f,a,b,c,d,k,s,t) step(f(b,c,d),a,b,x[k],s,t)
    S(F,a,b,cc,d,0,7,0xd76aa478); S(F,d,a,b,cc,1,12,0xe8c7b756); S(F,cc,d,a,b,2,17,0x242070db); S(F,b,cc,d,a,3,22,0xc1bdceee);
    S(F,a,b,cc,d,4,7,0xf57c0faf); S(F,d,a,b,cc,5,12,0x4787c62a); S(F,cc,d,a,b,6,17,0xa8304613); S(F,b,cc,d,a,7,22,0xfd469501);
    S(F,a,b,cc,d,8,7,0x698098d8); S(F,d,a,b,cc,9,12,0x8b44f7af); S(F,cc,d,a,b,10,17,0xffff5bb1); S(F,b,cc,d,a,11,22,0x895cd7be);
    S(F,a,b,cc,d,12,7,0x6b901122); S(F,d,a,b,cc,13,12,0xfd987193); S(F,cc,d,a,b,14,17,0xa679438e); S(F,b,cc,d,a,15,22,0x49b40821);
    S(G,a,b,cc,d,1,5,0xf61e2562); S(G,d,a,b,cc,6,9,0xc040b340); S(G,cc,d,a,b,11,14,0x265e5a51); S(G,b,cc,d,a,0,20,0xe9b6c7aa);
    S(G,a,b,cc,d,5,5,0xd62f105d); S(G,d,a,b,cc,10,9,0x02441453); S(G,cc,d,a,b,15,14,0xd8a1e681); S(G,b,cc,d,a,4,20,0xe7d3fbc8);
    S(G,a,b,cc,d,9,5,0x21e1cde6); S(G,d,a,b,cc,14,9,0xc33707d6); S(G,cc,d,a,b,3,14,0xf4d50d87); S(G,b,cc,d,a,8,20,0x455a14ed);
    S(G,a,b,cc,d,13,5,0xa9e3e905); S(G,d,a,b,cc,2,9,0xfcefa3f8); S(G,cc,d,a,b,7,14,0x676f02d9); S(G,b,cc,d,a,12,20,0x8d2a4c8a);
    S(H,a,b,cc,d,5,4,0xfffa3942); S(H,d,a,b,cc,8,11,0x8771f681); S(H,cc,d,a,b,11,16,0x6d9d6122); S(H,b,cc,d,a,14,23,0xfde5380c);
    S(H,a,b,cc,d,1,4,0xa4beea44); S(H,d,a,b,cc,4,11,0x4bdecfa9); S(H,cc,d,a,b,7,16,0xf6bb4b60); S(H,b,cc,d,a,10,23,0xbebfbc70);
    S(H,a,b,cc,d,13,4,0x289b7ec6); S(H,d,a,b,cc,0,11,0xeaa127fa); S(H,cc,d,a,b,3,16,0xd4ef3085); S(H,b,cc,d,a,6,23,0x04881d05);
    S(H,a,b,cc,d,9,4,0xd9d4d039); S(H,d,a,b,cc,12,11,0xe6db99e5); S(H,cc,d,a,b,15,16,0x1fa27cf8); S(H,b,cc,d,a,2,23,0xc4ac5665);
    S(I,a,b,cc,d,0,6,0xf4292244); S(I,d,a,b,cc,7,10,0x432aff97); S(I,cc,d,a,b,14,15,0xab9423a7); S(I,b,cc,d,a,5,21,0xfc93a039);
    S(I,a,b,cc,d,12,6,0x655b59c3); S(I,d,a,b,cc,3,10,0x8f0ccc92); S(I,cc,d,a,b,10,15,0xffeff47d); S(I,b,cc,d,a,1,21,0x85845dd1);
    S(I,a,b,cc,d,8,6,0x6fa87e4f); S(I,d,a,b,cc,15,10,0xfe2ce6e0); S(I,cc,d,a,b,6,15,0xa3014314); S(I,b,cc,d,a,13,21,0x4e0811a1);
    S(I,a,b,cc,d,4,6,0xf7537e82); S(I,d,a,b,cc,11,10,0xbd3af235); S(I,cc,d,a,b,2,15,0x2ad7d2bb); S(I,b,cc,d,a,9,21,0xeb86d391);
#undef S
#undef F
#undef G
#undef H
#undef I
    c.a += a; c.b += b; c.c += cc; c.d += d;
}
static void md5Update(Md5Ctx& c, const uint8_t* data, size_t len) {
    c.bytes += len;
    while (len) {
        const size_t n = std::min(len, 64 - c.used);
        std::memcpy(c.buf + c.used, data, n); c.used += n; data += n; len -= n;
        if (c.used == 64) { md5Block(c, c.buf); c.used = 0; }
    }
}
static std::string md5Hex(const std::string& s) {
    Md5Ctx c; md5Init(c); md5Update(c, (const uint8_t*)s.data(), s.size());
    const uint64_t bits = c.bytes * 8;
    uint8_t one = 0x80; md5Update(c, &one, 1);
    uint8_t zero = 0;
    while (c.used != 56) md5Update(c, &zero, 1);
    uint8_t len[8]; for (int i=0;i<8;++i) len[i]=(uint8_t)(bits >> (8*i));
    md5Update(c, len, 8);
    uint32_t vals[4] = {c.a,c.b,c.c,c.d};
    char out[33]; int pos=0;
    for (uint32_t v: vals) for (int i=0;i<4;++i) { std::snprintf(out+pos, 3, "%02x", (unsigned)((v>>(8*i))&0xff)); pos+=2; }
    out[32]='\0'; return out;
}

static std::string lastFmApiSig(const std::map<std::string,std::string>& params, const std::string& secret) {
    std::string s;
    for (const auto& kv : params) {
        if (kv.first == "format" || kv.first == "callback" || kv.first == "api_sig") continue;
        s += kv.first + kv.second;
    }
    s += secret;
    return md5Hex(s);
}

static std::string formEncode(const std::map<std::string,std::string>& params) {
    std::string body;
    for (const auto& kv : params) {
        if (!body.empty()) body += '&';
        body += urlEncodeComponent(kv.first) + "=" + urlEncodeComponent(kv.second);
    }
    return body;
}

static size_t curlDiscard(void* ptr, size_t size, size_t nmemb, void*) { (void)ptr; return size * nmemb; }

static int lastFmThreadMain(void*) {
    for (;;) {
        LastFmJob job;
        SDL_LockMutex(gLastFmMutex);
        while (!gLastFmStop && gLastFmQueue.empty()) SDL_CondWait(gLastFmCond, gLastFmMutex);
        if (gLastFmStop && gLastFmQueue.empty()) { SDL_UnlockMutex(gLastFmMutex); break; }
        job = gLastFmQueue.front();
        gLastFmQueue.pop_front();
        SDL_UnlockMutex(gLastFmMutex);

        CURL* c = curl_easy_init();
        if (!c) continue;
        curl_easy_setopt(c, CURLOPT_URL, "https://ws.audioscrobbler.com/2.0/");
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, job.body.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlDiscard);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, (long)job.timeoutSec);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)job.timeoutSec);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "Ampintosh-Switch/1.5.2 Last.fm");
        curl_easy_perform(c);
        curl_easy_cleanup(c);
    }
    return 0;
}

static bool lastFmConfigured(const AppConfig& cfg) {
    return cfg.netEnabled && g_netReady && cfg.lastfmEnabled && !cfg.lastfmApiKey.empty() && !cfg.lastfmApiSecret.empty() && !cfg.lastfmSessionKey.empty();
}

static void lastFmStart(const AppConfig& cfg) {
    if (!lastFmConfigured(cfg) || gLastFmThread) return;
    gLastFmMutex = SDL_CreateMutex();
    gLastFmCond = SDL_CreateCond();
    if (!gLastFmMutex || !gLastFmCond) return;
    gLastFmStop = false;
    gLastFmThread = SDL_CreateThread(lastFmThreadMain, "lastfm", nullptr);
}

static void lastFmStop() {
    if (!gLastFmThread) return;
    SDL_LockMutex(gLastFmMutex);
    gLastFmStop = true;
    SDL_CondSignal(gLastFmCond);
    SDL_UnlockMutex(gLastFmMutex);
    SDL_WaitThread(gLastFmThread, nullptr);
    gLastFmThread = nullptr;
    if (gLastFmCond) { SDL_DestroyCond(gLastFmCond); gLastFmCond = nullptr; }
    if (gLastFmMutex) { SDL_DestroyMutex(gLastFmMutex); gLastFmMutex = nullptr; }
    gLastFmQueue.clear();
}

static void lastFmEnqueue(const AppConfig& cfg, std::map<std::string,std::string> params) {
    if (!lastFmConfigured(cfg) || !gLastFmMutex || !gLastFmCond) return;
    params["api_key"] = cfg.lastfmApiKey;
    params["sk"] = cfg.lastfmSessionKey;
    params["api_sig"] = lastFmApiSig(params, cfg.lastfmApiSecret);
    params["format"] = "json";
    LastFmJob job;
    job.body = formEncode(params);
    job.timeoutSec = std::max(3, std::min(cfg.netTimeoutSec, 20));
    SDL_LockMutex(gLastFmMutex);
    if (gLastFmQueue.size() < 16) gLastFmQueue.push_back(job);
    SDL_CondSignal(gLastFmCond);
    SDL_UnlockMutex(gLastFmMutex);
}

static std::string stripExtensionDisplay(std::string name) {
    const size_t sep = findLastUtf8ExtSeparator(name);
    if (sep != std::string::npos) name = name.substr(0, sep);
    return name.empty() ? std::string("Unknown Track") : name;
}

static std::string metadataPathFromRef(const std::string& ref) {
    const std::string l = lowerCopy(ref);
    if (startsWith(l, "smb://") || startsWith(l, "sftp://") || startsWith(l, "http://") || startsWith(l, "https://")) {
        const size_t scheme = ref.find("://");
        const size_t slash = scheme == std::string::npos ? std::string::npos : ref.find('/', scheme + 3);
        return slash == std::string::npos ? ref : urlDecodeComponent(ref.substr(slash));
    }
    return urlDecodeComponent(ref);
}

static LastFmPlaybackState makeLastFmState(const Entry& e, const Player& p, const AppConfig& cfg) {
    LastFmPlaybackState st;
    st.ref = e.ref;
    const TrackMetadata embedded = metadataForRef(e.ref);
    const std::string metaPath = metadataPathFromRef(e.ref);
    st.track = embedded.title.empty() ? stripExtensionDisplay(displayName(metaPath)) : embedded.title;
    st.album = embedded.album.empty() ? pathLeaf(directoryName(metaPath)) : embedded.album;
    st.artist = embedded.artist.empty() ? pathLeaf(parentDirectory(directoryName(metaPath))) : embedded.artist;
    if (st.artist.empty() || st.artist == "/" || st.artist.find("://") != std::string::npos) st.artist = cfg.lastfmDefaultArtist;
    if (st.artist.empty()) st.artist = st.album.empty() ? std::string("Unknown Artist") : st.album;
    st.durationSec = (p.sampleRate > 0 && p.totalFrames > 0) ? (int)((double)p.totalFrames / (double)p.sampleRate + 0.5) : 0;
    st.startedAt = (int)std::time(nullptr);
    return st;
}

static void lastFmBeginTrack(const Entry& e, const Player& p, const AppConfig& cfg) {
    if (!lastFmConfigured(cfg)) return;
    gLastFmCurrent = makeLastFmState(e, p, cfg);
    if (cfg.lastfmNowPlaying && !gLastFmCurrent.artist.empty() && !gLastFmCurrent.track.empty()) {
        std::map<std::string,std::string> params;
        params["method"] = "track.updateNowPlaying";
        params["artist"] = gLastFmCurrent.artist;
        params["track"] = gLastFmCurrent.track;
        if (!gLastFmCurrent.album.empty()) params["album"] = gLastFmCurrent.album;
        if (gLastFmCurrent.durationSec > 0) params["duration"] = std::to_string(gLastFmCurrent.durationSec);
        lastFmEnqueue(cfg, params);
    }
}

static void lastFmMaybeScrobble(const Entry& e, const Player& p, const AppConfig& cfg) {
    if (!lastFmConfigured(cfg) || !cfg.lastfmScrobble || gLastFmCurrent.scrobbled) return;
    if (gLastFmCurrent.ref != e.ref) return;
    if (!p.playing.load()) return;
    const int elapsed = p.sampleRate > 0 ? (int)(p.cursor.load() / (drmp3_uint64)p.sampleRate) : 0;
    int threshold = cfg.lastfmMinSeconds;
    if (gLastFmCurrent.durationSec > 0) {
        threshold = std::max(threshold, (gLastFmCurrent.durationSec * cfg.lastfmScrobblePercent) / 100);
        threshold = std::min(threshold, 240); // Last.fm's common scrobble rule cap.
    }
    if (elapsed < threshold) return;
    std::map<std::string,std::string> params;
    params["method"] = "track.scrobble";
    params["artist"] = gLastFmCurrent.artist;
    params["track"] = gLastFmCurrent.track;
    params["timestamp"] = std::to_string(gLastFmCurrent.startedAt);
    if (!gLastFmCurrent.album.empty()) params["album"] = gLastFmCurrent.album;
    if (gLastFmCurrent.durationSec > 0) params["duration"] = std::to_string(gLastFmCurrent.durationSec);
    lastFmEnqueue(cfg, params);
    gLastFmCurrent.scrobbled = true;
}
#else
static void lastFmStart(const AppConfig&) {}
static void lastFmStop() {}
static void lastFmBeginTrack(const Entry&, const Player&, const AppConfig&) {}
static void lastFmMaybeScrobble(const Entry&, const Player&, const AppConfig&) {}
#endif

static bool tryLoadCurrent(std::vector<Entry>& entries, int& trackIndex, Player& player,
                           Analyzer& analyzer, const AppConfig& cfg, bool startPlaying) {
    if (entries.empty()) { unloadPlayer(player); return false; }
    trackIndex = std::max(0, std::min(trackIndex, (int)entries.size() - 1));
    for (size_t attempt = 0; attempt < entries.size(); ++attempt) {
        if (loadEntry(player, analyzer, entries[trackIndex], cfg, startPlaying)) return true;
        trackIndex = (trackIndex + 1) % (int)entries.size();
    }
    unloadPlayer(player);
    return false;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
static const char* fmtName(AudioFmt f) {
    switch (f) {
        case AudioFmt::Mp3:  return "MP3";
        case AudioFmt::Flac: return "FLAC";
        case AudioFmt::Wav:  return "WAV";
        case AudioFmt::Aiff: return "AIFF";
        default: return "";
    }
}

static const char* playbackModeName(PlaybackMode m) {
    switch (m) {
        case PlaybackMode::Shuffle:   return "Shuffle";
        case PlaybackMode::RepeatOne: return "Repeat One";
        case PlaybackMode::RepeatAll: return "Repeat All";
        default: return "";
    }
}

static const char* visualizerName(VisualizerMode v) {
    switch (v) {
        case VisualizerMode::Spectrum: return "Spectrum";
        case VisualizerMode::Mirror:   return "Mirror Bars";
        case VisualizerMode::Scope:    return "Oscilloscope";
        case VisualizerMode::Fractal:  return "Fractal";
        case VisualizerMode::Orbit:    return "Orbit";
        case VisualizerMode::Rings:    return "Rings";
        case VisualizerMode::Tunnel:   return "Tunnel";
        case VisualizerMode::Radial:   return "Radial Burst";
        case VisualizerMode::Lissajous:return "Lissajous";
        case VisualizerMode::Bloom:    return "Bloom";
        case VisualizerMode::Particles:return "Particles";
        case VisualizerMode::Matrix:   return "Spectro Fall";
        default: return "";
    }
}

static PlaybackMode nextPlaybackMode(PlaybackMode m) {
    switch (m) {
        case PlaybackMode::Shuffle:   return PlaybackMode::RepeatOne;
        case PlaybackMode::RepeatOne: return PlaybackMode::RepeatAll;
        case PlaybackMode::RepeatAll: return PlaybackMode::Shuffle;
        default: return PlaybackMode::Shuffle;
    }
}

static VisualizerMode nextVisualizerMode(VisualizerMode v) {
    switch (v) {
        case VisualizerMode::Spectrum: return VisualizerMode::Mirror;
        case VisualizerMode::Mirror:   return VisualizerMode::Scope;
        case VisualizerMode::Scope:    return VisualizerMode::Fractal;
        case VisualizerMode::Fractal:  return VisualizerMode::Orbit;
        case VisualizerMode::Orbit:    return VisualizerMode::Rings;
        case VisualizerMode::Rings:    return VisualizerMode::Tunnel;
        case VisualizerMode::Tunnel:   return VisualizerMode::Radial;
        case VisualizerMode::Radial:   return VisualizerMode::Lissajous;
        case VisualizerMode::Lissajous:return VisualizerMode::Bloom;
        case VisualizerMode::Bloom:    return VisualizerMode::Particles;
        case VisualizerMode::Particles:return VisualizerMode::Matrix;
        case VisualizerMode::Matrix:   return VisualizerMode::Spectrum;
        default: return VisualizerMode::Spectrum;
    }
}

static std::string fmtTime(double seconds) {
    if (seconds < 0) seconds = 0;
    const int total = (int)seconds;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
    return std::string(buf);
}

// Immediate-mode UTF-8 text at (x,y); align=1 right-aligns x to the text's right
// edge. Cheap for the handful of short strings drawn here. No-ops without a font
// so the app still runs if the bundled font is missing.
static int drawText(SDL_Renderer* r, TTF_Font* font, const std::string& text,
                    int x, int y, RGB c, int align = 0) {
    if (!font || text.empty()) return 0;
    SDL_Color col{ c.r, c.g, c.b, 255 };
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text.c_str(), col);
    if (!surf) return 0;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
    const int w = surf->w, h = surf->h;
    SDL_FreeSurface(surf);
    if (!tex) return 0;
    SDL_Rect dst{ align == 1 ? x - w : (align == 2 ? x - w / 2 : x), y, w, h };
    SDL_RenderCopy(r, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
    return w;
}

static inline void setColor(SDL_Renderer* r, RGB c, Uint8 a = 255) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, a);
}
static inline void fillRect(SDL_Renderer* r, int x, int y, int w, int h) {
    SDL_Rect rect{ x, y, w, h };
    SDL_RenderFillRect(r, &rect);
}

static RGB lerpRGB(RGB a, RGB b, float t) {
    t = std::min(1.0f, std::max(0.0f, t));
    return {
        (Uint8)(a.r + (b.r - a.r) * t),
        (Uint8)(a.g + (b.g - a.g) * t),
        (Uint8)(a.b + (b.b - a.b) * t)
    };
}

static RGB scaleRGB(RGB c, float k) {
    k = std::max(0.0f, k);
    return {
        (Uint8)std::min(255.0f, c.r * k),
        (Uint8)std::min(255.0f, c.g * k),
        (Uint8)std::min(255.0f, c.b * k)
    };
}

static void drawLineThick(SDL_Renderer* r, int x0, int y0, int x1, int y1, int thickness) {
    thickness = std::max(1, thickness);
    const int radius = thickness / 2;
    for (int o = -radius; o <= radius; ++o) {
        SDL_RenderDrawLine(r, x0 + o, y0, x1 + o, y1);
        SDL_RenderDrawLine(r, x0, y0 + o, x1, y1 + o);
    }
}

static void drawEllipse(SDL_Renderer* r, int cx, int cy, int rx, int ry, int segments = 96) {
    if (rx <= 0 || ry <= 0) return;
    int px = cx + rx, py = cy;
    for (int i = 1; i <= segments; ++i) {
        float a = 2.0f * PI_F * (float)i / (float)segments;
        int x = cx + (int)(cosf(a) * rx);
        int y = cy + (int)(sinf(a) * ry);
        SDL_RenderDrawLine(r, px, py, x, y);
        px = x; py = y;
    }
}

static float bandEnergy(const float* bands, float lo, float hi) {
    int a = std::max(0, std::min(NUM_BANDS - 1, (int)floorf(lo * NUM_BANDS)));
    int b = std::max(a + 1, std::min(NUM_BANDS, (int)ceilf(hi * NUM_BANDS)));
    float sum = 0.0f;
    for (int i = a; i < b; ++i) sum += bands[i];
    return sum / (float)(b - a);
}

static float monoSampleAt(const Player& p, drmp3_uint64 frame) {
    if (p.streaming) {
        (void)frame;
        const int idx = (p.recentWrite + (int)(frame % FFT_SIZE)) % FFT_SIZE;
        return p.recentMono[idx];
    }
    if (!p.pcm || frame >= p.totalFrames) return 0.0f;
    if (p.channels >= 2) return 0.5f * (p.pcm[frame * p.channels] + p.pcm[frame * p.channels + 1]);
    return p.pcm[frame * p.channels];
}

static void drawLiquidRetroBackground(SDL_Renderer* r, float phase) {
    // Vertical gradient approximated with scanline-height bands.
    for (int y = 0; y < SCREEN_H; y += 3) {
        float t = (float)y / (float)SCREEN_H;
        RGB c = lerpRGB(COL_BG, COL_BG2, t);
        setColor(r, c);
        fillRect(r, 0, y, SCREEN_W, 3);
    }

    // Soft coloured corners, lifted from the macOS LiquidRetroBackground vibe.
    for (int i = 0; i < 18; ++i) {
        Uint8 a = (Uint8)std::max(0, 28 - i);
        setColor(r, COL_PRIMARY, a);
        fillRect(r, 0, i * 10, 280 - i * 8, 10);
        setColor(r, COL_SECONDARY, (Uint8)(a * 0.75f));
        fillRect(r, SCREEN_W - (300 - i * 8), SCREEN_H - (i + 1) * 10, 300 - i * 8, 10);
    }

    // CRT-ish scanlines and subtle animated grid.
    setColor(r, COL_TEXT, 13);
    for (int y = 0; y < SCREEN_H; y += 6) fillRect(r, 0, y, SCREEN_W, 1);
    setColor(r, COL_MUTED, 22);
    const int drift = (int)(sinf(phase * 0.33f) * 6.0f);
    for (int x = -28 + drift; x < SCREEN_W; x += 28) SDL_RenderDrawLine(r, x, 0, x, SCREEN_H);
    for (int y = -24 - drift; y < SCREEN_H; y += 24) SDL_RenderDrawLine(r, 0, y, SCREEN_W, y);
}

static void drawGlassPanel(SDL_Renderer* r, int x, int y, int w, int h, RGB tint, Uint8 fillAlpha = 150) {
    setColor(r, COL_PANEL, fillAlpha);
    fillRect(r, x, y, w, h);
    setColor(r, tint, 34);
    fillRect(r, x + 1, y + 1, w - 2, h / 2);
    setColor(r, COL_TEXT, 66);
    SDL_RenderDrawLine(r, x, y, x + w, y);
    SDL_RenderDrawLine(r, x, y, x, y + h);
    setColor(r, tint, 78);
    SDL_RenderDrawLine(r, x, y + h - 1, x + w, y + h - 1);
    SDL_RenderDrawLine(r, x + w - 1, y, x + w - 1, y + h);
    setColor(r, COL_BG, 110);
    fillRect(r, x + 5, y + h - 4, w - 10, 2);
}

static int drawFormatChip(SDL_Renderer* r, TTF_Font* font, const std::string& text, int x, int y, RGB tint, bool active = false) {
    const int padX = 9, h = 26;
    const int tw = drawText(r, font, text, -4000, -4000, COL_TEXT);
    const int w = std::max(54, tw + padX * 2);
    setColor(r, active ? tint : COL_PANEL, active ? 190 : 145);
    fillRect(r, x, y, w, h);
    setColor(r, tint, active ? 220 : 96);
    SDL_RenderDrawLine(r, x, y, x + w, y);
    SDL_RenderDrawLine(r, x, y + h - 1, x + w, y + h - 1);
    drawText(r, font, text, x + padX, y + 4, active ? COL_TEXT : tint);
    return w;
}

static void drawVisualizerBackground(SDL_Renderer* r) {
    drawGlassPanel(r, 30, 132, SCREEN_W - 60, SCREEN_H - 252, COL_TERTIARY, 82);
    setColor(r, COL_DISPLAY, 122);
    fillRect(r, 42, 144, SCREEN_W - 84, SCREEN_H - 276);
    setColor(r, COL_MUTED, 24);
    for (int x = 42; x <= SCREEN_W - 42; x += 28) SDL_RenderDrawLine(r, x, 144, x, SCREEN_H - 132);
    for (int y = 144; y <= SCREEN_H - 132; y += 24) SDL_RenderDrawLine(r, 42, y, SCREEN_W - 42, y);
}

static void drawSpectrum(SDL_Renderer* r, const float* bands, float amp) {
    const int marginX = 60;
    const int baseY   = SCREEN_H - 142;
    const int top     = 164;
    const int usableH = baseY - top;
    const float gap   = 5.0f;
    const float bw    = (SCREEN_W - 2 * marginX - gap * (NUM_BANDS - 1)) / NUM_BANDS;
    const float glow  = 0.5f + amp * 0.5f;

    for (int i = 0; i < NUM_BANDS; ++i) {
        const float level = bands[i];
        const int   h = (int)std::max(4.0f, usableH * level);
        const int   x = (int)(marginX + i * (bw + gap));
        const float t = (float)i / (NUM_BANDS - 1);
        RGB c = lerpRGB(COL_PRIMARY, COL_SECONDARY, t);
        c = scaleRGB(c, 0.62f + 0.46f * level);
        setColor(r, c, (Uint8)(std::min(1.0f, (0.45f + level * 0.5f) * glow) * 255));
        fillRect(r, x, baseY - h, (int)bw, h);
        setColor(r, COL_TEXT, (Uint8)(std::min(1.0f, 0.35f + amp * 0.4f) * 255));
        fillRect(r, x, baseY - h - 4, (int)bw, 2);
    }
}

static void drawMirror(SDL_Renderer* r, const float* bands, float amp) {
    const int midY = SCREEN_H / 2 + 28;
    const int marginX = 62;
    const float gap = 5.0f;
    const float bw = (SCREEN_W - 2 * marginX - gap * (NUM_BANDS - 1)) / NUM_BANDS;
    const int maxH = 180;
    for (int i = 0; i < NUM_BANDS; ++i) {
        const float level = bands[i];
        const int h = (int)(8 + maxH * level * (0.78f + amp * 0.42f));
        const int x = (int)(marginX + i * (bw + gap));
        RGB c = lerpRGB(COL_TERTIARY, COL_PRIMARY, (float)i / (NUM_BANDS - 1));
        setColor(r, c, (Uint8)(110 + 120 * std::min(1.0f, level + amp * 0.25f)));
        fillRect(r, x, midY - h, (int)bw, h);
        setColor(r, c, (Uint8)(70 + 90 * level));
        fillRect(r, x, midY + 2, (int)bw, h);
    }
    setColor(r, COL_SECONDARY, (Uint8)(90 + 120 * amp));
    fillRect(r, marginX, midY - 2, SCREEN_W - 2 * marginX, 4);
}

static void drawScope(SDL_Renderer* r, const Player& p, float amp, float phase) {
    const int x0 = 60, x1 = SCREEN_W - 60;
    const int mid = SCREEN_H / 2 + 26;
    const int height = 205;
    const int w = x1 - x0;
    const int windowFrames = std::max(512, p.sampleRate / 24);

    setColor(r, COL_TERTIARY, (Uint8)(62 + 80 * amp));
    int lastX = x0;
    int lastY = mid;
    for (int x = 0; x < w; ++x) {
        drmp3_uint64 f = p.cursor.load() + (drmp3_uint64)((double)x * windowFrames / std::max(1, w - 1));
        float s = monoSampleAt(p, f);
        if (!p.playing.load()) s += 0.04f * sinf((float)x * 0.16f + phase * 0.8f);
        s = std::min(1.0f, std::max(-1.0f, s));
        int y = mid - (int)(s * height * (0.34f + amp * 0.45f));
        if (x > 0) drawLineThick(r, lastX, lastY, x0 + x, y, 3);
        lastX = x0 + x; lastY = y;
    }
    setColor(r, COL_PRIMARY, (Uint8)(135 + 95 * amp));
    SDL_RenderDrawLine(r, x0, mid, x1, mid);
}

static void drawFractal(SDL_Renderer* r, const float* bands, float amp, float phase) {
    (void)amp;
    struct Branch { float x, y, len, angle; int depth; };
    std::vector<Branch> stack;
    const float bass = bandEnergy(bands, 0.00f, 0.16f);
    const float mid  = bandEnergy(bands, 0.34f, 0.58f);
    const float treble = bandEnergy(bands, 0.58f, 1.00f);
    stack.push_back({ SCREEN_W * 0.5f, SCREEN_H - 144.0f, 100.0f + bass * 92.0f, -PI_F / 2.0f, 0 });
    const int maxDepth = 6 + (int)(treble * 3.0f);
    while (!stack.empty()) {
        Branch b = stack.back(); stack.pop_back();
        float pulse = bands[b.depth % NUM_BANDS];
        float jitter = sinf(phase * 1.7f + b.depth * 1.3f) * pulse * 0.16f;
        float x2 = b.x + cosf(b.angle + jitter) * b.len;
        float y2 = b.y + sinf(b.angle + jitter) * b.len;
        RGB c = lerpRGB(COL_PRIMARY, COL_TERTIARY, (float)b.depth / (float)maxDepth);
        setColor(r, c, (Uint8)std::max(42, 190 - b.depth * 18));
        drawLineThick(r, (int)b.x, (int)b.y, (int)x2, (int)y2, std::max(1, (int)(5 + bass * 7 - b.depth)));
        if (b.depth < maxDepth && b.len > 8.0f) {
            float nextLen = b.len * (0.58f + bass * 0.13f + pulse * 0.06f);
            float split = 0.36f + mid * 0.38f + sinf(phase * 0.7f) * (0.08f + mid * 0.12f);
            stack.push_back({ x2, y2, nextLen, b.angle - split, b.depth + 1 });
            stack.push_back({ x2, y2, nextLen, b.angle + split, b.depth + 1 });
            if (treble > 0.52f && (b.depth % 3) == 0) stack.push_back({ x2, y2, nextLen * 0.72f, b.angle + sinf(phase + b.depth) * 0.72f, b.depth + 1 });
        }
    }
}

static void drawOrbit(SDL_Renderer* r, const float* bands, float amp, float phase) {
    const int cx = SCREEN_W / 2, cy = SCREEN_H / 2 + 18;
    const float radius = 116.0f + amp * 76.0f;
    for (int i = 0; i < 56; ++i) {
        const float energy = bands[i % NUM_BANDS];
        const float t = phase * (0.55f + amp * 1.1f) * (0.8f + energy * 1.7f) + i * 0.38f;
        const float x = cx + cosf(t * 1.7f) * radius * cosf(i * 0.09f);
        const float y = cy + sinf(t * 2.1f) * radius * sinf(i * 0.13f);
        const int dot = (int)(3 + energy * 10 + amp * 3);
        setColor(r, (i % 3 == 0) ? COL_SECONDARY : COL_PRIMARY, (Uint8)(88 + energy * 140));
        fillRect(r, (int)x - dot / 2, (int)y - dot / 2, dot, dot);
    }
}

static void drawRings(SDL_Renderer* r, const float* bands, float amp, float phase) {
    const int cx = SCREEN_W / 2, cy = SCREEN_H / 2 + 18;
    const int count = 12;
    for (int i = 0; i < count; ++i) {
        float level = bands[i * NUM_BANDS / count];
        float pulse = 0.5f + 0.5f * sinf(phase * (1.2f + amp * 2.6f) + i * 0.72f);
        int rx = (int)((40 + i * 24) * (0.84f + amp * 0.18f + pulse * level * 0.18f));
        int ry = (int)(rx * (0.58f + level * 0.16f));
        RGB c = lerpRGB(COL_PRIMARY, COL_SECONDARY, (float)i / (count - 1));
        setColor(r, c, (Uint8)(50 + level * 150));
        drawEllipse(r, cx, cy, rx, ry, 110);
    }
    setColor(r, COL_TERTIARY, (Uint8)(80 + amp * 130));
    fillRect(r, cx - 5, cy - 5, 10, 10);
}

static void drawTunnel(SDL_Renderer* r, const float* bands, float amp, float phase) {
    const int cx = SCREEN_W / 2;
    const int cy = SCREEN_H / 2 + 18;
    for (int depth = 0; depth < 14; ++depth) {
        const float norm = (depth + fmodf(phase * (0.08f + amp * 0.22f), 1.0f)) / 14.0f;
        const float scale = powf(norm, 1.7f);
        const float level = bands[depth * NUM_BANDS / 14];
        const int w = (int)(SCREEN_W * (0.16f + scale * 1.16f + level * 0.10f));
        const int h = (int)(SCREEN_H * (0.12f + scale * 0.86f + level * 0.10f));
        setColor(r, COL_PRIMARY, (Uint8)std::max(22.0f, (1.0f - scale) * 170.0f + level * 70.0f));
        SDL_Rect rect{ cx - w / 2, cy - h / 2, w, h };
        SDL_RenderDrawLine(r, rect.x, rect.y, rect.x + rect.w, rect.y);
        SDL_RenderDrawLine(r, rect.x, rect.y + rect.h, rect.x + rect.w, rect.y + rect.h);
        SDL_RenderDrawLine(r, rect.x, rect.y, rect.x, rect.y + rect.h);
        SDL_RenderDrawLine(r, rect.x + rect.w, rect.y, rect.x + rect.w, rect.y + rect.h);
    }
    setColor(r, COL_TERTIARY, 55);
    for (int spoke = 0; spoke < 12; ++spoke) {
        float a = spoke * 2.0f * PI_F / 12.0f + phase * 0.05f;
        SDL_RenderDrawLine(r, cx, cy, cx + (int)(cosf(a) * SCREEN_W * 0.62f), cy + (int)(sinf(a) * SCREEN_H * 0.62f));
    }
}

static void drawRadial(SDL_Renderer* r, const float* bands, float amp, float phase) {
    const int cx = SCREEN_W / 2;
    const int cy = SCREEN_H / 2 + 18;
    const float inner = 44.0f + amp * 20.0f;
    const float reach = 250.0f;
    const float rot = phase * (0.03f + amp * 0.08f);
    for (int i = 0; i < NUM_BANDS; ++i) {
        const float a = rot + (2.0f * PI_F * i) / NUM_BANDS - PI_F / 2.0f;
        const float level = bands[i];
        const float outer = inner + reach * level;
        RGB c = lerpRGB(COL_TERTIARY, COL_SECONDARY, (float)i / (NUM_BANDS - 1));
        setColor(r, c, (Uint8)(115 + 120 * level));
        drawLineThick(r,
            cx + (int)(cosf(a) * inner), cy + (int)(sinf(a) * inner),
            cx + (int)(cosf(a) * outer), cy + (int)(sinf(a) * outer),
            std::max(1, (int)(1 + level * 4)));
    }
    setColor(r, COL_PRIMARY, (Uint8)(75 + 120 * amp));
    drawEllipse(r, cx, cy, (int)(inner * (0.7f + amp * 0.6f)), (int)(inner * (0.7f + amp * 0.6f)), 72);
}

static void drawLissajous(SDL_Renderer* r, const float* bands, float amp, float phase) {
    const int cx = SCREEN_W / 2, cy = SCREEN_H / 2 + 18;
    const float bass = bandEnergy(bands, 0.00f, 0.16f);
    const float mid  = bandEnergy(bands, 0.34f, 0.58f);
    const float treble = bandEnergy(bands, 0.58f, 1.0f);
    const float rx = 320.0f * (0.36f + amp * 0.78f);
    const float ry = 210.0f * (0.36f + amp * 0.78f);
    const float a = 2.0f + bass * 4.0f;
    const float b = 3.0f + treble * 5.0f;
    const float delta = phase * (0.08f + mid * 0.2f);
    int px = cx, py = cy;
    setColor(r, COL_TERTIARY, (Uint8)(65 + amp * 90));
    for (int step = 0; step <= 256; ++step) {
        float t = (float)step / 256.0f * 2.0f * PI_F;
        int x = cx + (int)(sinf(a * t + delta) * rx);
        int y = cy + (int)(sinf(b * t) * ry);
        if (step > 0) drawLineThick(r, px, py, x, y, 3);
        px = x; py = y;
    }
    setColor(r, COL_PRIMARY, (Uint8)(120 + amp * 100));
    drawEllipse(r, cx, cy, 5 + (int)(amp * 12), 5 + (int)(amp * 12), 32);
}

static void drawBloom(SDL_Renderer* r, const float* bands, float amp, float phase) {
    const int cx = SCREEN_W / 2, cy = SCREEN_H / 2 + 18;
    const float base = 220.0f;
    const float spin = phase * (0.02f + bandEnergy(bands, 0.0f, 0.16f) * 0.08f);
    for (int i = 0; i < NUM_BANDS; ++i) {
        float level = bands[i];
        float a = spin + (float)i / NUM_BANDS * 2.0f * PI_F;
        float reach = base * (0.18f + level * (0.35f + amp * 0.65f));
        float wing = 0.12f + level * 0.18f;
        int tx = cx + (int)(cosf(a) * reach);
        int ty = cy + (int)(sinf(a) * reach * 0.78f);
        RGB c = lerpRGB(COL_PRIMARY, COL_SECONDARY, (float)i / NUM_BANDS);
        setColor(r, c, (Uint8)(30 + level * 130));
        SDL_RenderDrawLine(r, cx, cy, tx, ty);
        SDL_RenderDrawLine(r, cx, cy, cx + (int)(cosf(a - wing) * reach * 0.72f), cy + (int)(sinf(a - wing) * reach * 0.56f));
        SDL_RenderDrawLine(r, cx, cy, cx + (int)(cosf(a + wing) * reach * 0.72f), cy + (int)(sinf(a + wing) * reach * 0.56f));
    }
    setColor(r, COL_SECONDARY, (Uint8)(80 + amp * 155));
    fillRect(r, cx - 8 - (int)(amp * 8), cy - 8 - (int)(amp * 8), 16 + (int)(amp * 16), 16 + (int)(amp * 16));
}

static void drawParticles(SDL_Renderer* r, const float* bands, float amp, float phase) {
    const int perBand = 4;
    for (int i = 0; i < NUM_BANDS; ++i) {
        float level = bands[i];
        float baseX = 54.0f + (SCREEN_W - 108.0f) * (i + 0.5f) / NUM_BANDS;
        for (int j = 0; j < perBand; ++j) {
            float seed = (float)(i * perBand + j);
            float ph = fmodf(phase * (0.08f + level * 0.32f + amp * 0.16f) + seed * 0.37f, 1.0f);
            float lift = (level * 0.85f + 0.15f) * 360.0f * (1.0f - ph);
            float wobble = sinf(phase * 1.7f + seed) * (4.0f + level * 12.0f);
            int dot = (int)(2.0f + level * 6.0f + amp * 3.0f);
            int x = (int)(baseX + wobble);
            int y = SCREEN_H - 142 - (int)lift;
            setColor(r, (j & 1) ? COL_TERTIARY : COL_PRIMARY, (Uint8)((1.0f - ph) * (70 + level * 160)));
            fillRect(r, x - dot / 2, y - dot / 2, dot, dot);
        }
    }
}

static void drawMatrix(SDL_Renderer* r, const float* bands, float amp, float phase) {
    const int x0 = 48, y0 = 152, w = SCREEN_W - 96, h = SCREEN_H - 292;
    const int rows = 16;
    const float cellW = (float)w / NUM_BANDS;
    const float cellH = (float)h / rows;
    const float scroll = fmodf(phase * (0.05f + amp * 0.18f), 1.0f);
    for (int col = 0; col < NUM_BANDS; ++col) {
        float level = bands[col];
        for (int row = 0; row < rows; ++row) {
            float depth = ((float)row + scroll) / rows;
            float lit = level * (1.0f - depth) * (0.6f + amp * 0.5f);
            if (lit <= 0.035f) continue;
            RGB c = lerpRGB(COL_PRIMARY, COL_SECONDARY, (float)col / (NUM_BANDS - 1));
            setColor(r, c, (Uint8)std::min(235.0f, lit * 310.0f));
            fillRect(r, x0 + (int)(col * cellW + 1), y0 + h - (int)((row + 1) * cellH) + 1,
                     std::max(1, (int)cellW - 2), std::max(1, (int)cellH - 2));
        }
    }
}

static void drawVisualizer(SDL_Renderer* r, VisualizerMode mode, const Player& p,
                           const float* bands, float amp, float phase) {
    drawVisualizerBackground(r);
    switch (mode) {
        case VisualizerMode::Spectrum: drawSpectrum(r, bands, amp); break;
        case VisualizerMode::Mirror:   drawMirror(r, bands, amp); break;
        case VisualizerMode::Scope:    drawScope(r, p, amp, phase); break;
        case VisualizerMode::Fractal:  drawFractal(r, bands, amp, phase); break;
        case VisualizerMode::Orbit:    drawOrbit(r, bands, amp, phase); break;
        case VisualizerMode::Rings:    drawRings(r, bands, amp, phase); break;
        case VisualizerMode::Tunnel:   drawTunnel(r, bands, amp, phase); break;
        case VisualizerMode::Radial:   drawRadial(r, bands, amp, phase); break;
        case VisualizerMode::Lissajous:drawLissajous(r, bands, amp, phase); break;
        case VisualizerMode::Bloom:    drawBloom(r, bands, amp, phase); break;
        case VisualizerMode::Particles:drawParticles(r, bands, amp, phase); break;
        case VisualizerMode::Matrix:   drawMatrix(r, bands, amp, phase); break;
    }
}

// Play/pause glyph drawn from rectangles (no font needed).
static void drawTransport(SDL_Renderer* r, bool playing) {
    const int x = 46, y = 44, s = 34;
    drawGlassPanel(r, x - 12, y - 12, 62, 58, playing ? COL_PRIMARY : COL_SECONDARY, 120);
    setColor(r, playing ? COL_PRIMARY : COL_SECONDARY);
    if (playing) {
        fillRect(r, x, y, s / 3, s);
        fillRect(r, x + (2 * s) / 3, y, s / 3, s);
    } else {
        for (int i = 0; i < s; ++i) {
            const int barH = s - 2 * std::abs(i - s / 2);
            if (barH > 0) fillRect(r, x + i, y + (s - barH) / 2, 1, barH);
        }
    }
}

static void drawTrackStrip(SDL_Renderer* r, int count, int current) {
    if (count <= 0) return;
    const int y = 102, x0 = 122, dotW = 13, dotH = 14, gap = 7;
    for (int i = 0; i < count && i < 32; ++i) {
        RGB c = (i == current) ? COL_PRIMARY : COL_PANEL;
        setColor(r, c, (i == current) ? 245 : 150);
        fillRect(r, x0 + i * (dotW + gap), y, dotW, dotH);
    }
}

static void drawProgress(SDL_Renderer* r, double frac, bool active) {
    const int x = 54, y = SCREEN_H - 116, w = SCREEN_W - 108, h = 9;
    frac = std::min(1.0, std::max(0.0, frac));
    drawGlassPanel(r, 30, SCREEN_H - 142, SCREEN_W - 60, 44, active ? COL_SECONDARY : COL_PRIMARY, 116);
    setColor(r, COL_DISPLAY, 215);
    fillRect(r, x, y, w, h);
    setColor(r, active ? COL_SECONDARY : COL_PRIMARY);
    fillRect(r, x, y, (int)(w * frac), h);
    const int px = x + (int)(w * frac);
    setColor(r, COL_TEXT);
    fillRect(r, px - 2, y - 6, 4, h + 12);
}


static std::string ellipsizeText(TTF_Font* font, const std::string& text, int maxW) {
    if (!font || maxW <= 0) return text;
    int w = 0, h = 0;
    if (TTF_SizeUTF8(font, text.c_str(), &w, &h) != 0 || w <= maxW) return text;
    const std::string suffix = "...";
    std::string out = text;
    while (!out.empty()) {
        out.pop_back();
        std::string candidate = out + suffix;
        if (TTF_SizeUTF8(font, candidate.c_str(), &w, &h) == 0 && w <= maxW) return candidate;
    }
    return suffix;
}

static void drawBufferingIndicator(SDL_Renderer* r, TTF_Font* fontSmall, const StreamBufferInfo& info, float phase) {
    if (!info.buffering && !info.failed) return;
    const int w = 470;
    const int h = 56;
    const int x = (SCREEN_W - w) / 2;
    const int y = SCREEN_H - 214;
    drawGlassPanel(r, x, y, w, h, info.failed ? COL_SECONDARY : COL_TERTIARY, 176);

    const char* label = info.failed ? "STREAM ERROR" : "BUFFERING";
    int chipX = x + 18;
    chipX += drawFormatChip(r, fontSmall, label, chipX, y + 14, info.failed ? COL_SECONDARY : COL_PRIMARY, true) + 14;

    const int barX = chipX;
    const int barY = y + 24;
    const int barW = x + w - 86 - barX;
    const int barH = 8;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, COL_DISPLAY.r, COL_DISPLAY.g, COL_DISPLAY.b, 168);
    SDL_Rect bg{barX, barY, std::max(24, barW), barH};
    SDL_RenderFillRect(r, &bg);

    double pct = 0.0;
    if (info.targetFrames > 0) pct = std::min(1.0, (double)info.fillFrames / (double)info.targetFrames);
    if (info.eof && info.fillFrames > 0) pct = 1.0;
    SDL_SetRenderDrawColor(r, COL_PRIMARY.r, COL_PRIMARY.g, COL_PRIMARY.b, 220);
    SDL_Rect fg{barX, barY, (int)(std::max(24, barW) * pct), barH};
    SDL_RenderFillRect(r, &fg);

    char pctBuf[16];
    std::snprintf(pctBuf, sizeof(pctBuf), "%d%%", (int)std::round(pct * 100.0));
    drawText(r, fontSmall, pctBuf, x + w - 54, y + 14, COL_TEXT, 1);

    const int dotBase = x + w - 34;
    for (int i = 0; i < 3; ++i) {
        const float wave = 0.5f + 0.5f * std::sin(phase * 1.8f + (float)i * 1.7f);
        const int rad = 2 + (int)std::round(wave * 3.0f);
        SDL_SetRenderDrawColor(r, COL_SECONDARY.r, COL_SECONDARY.g, COL_SECONDARY.b, (Uint8)(90 + wave * 150));
        SDL_Rect d{dotBase + i * 10, y + 39 - rad, rad * 2, rad * 2};
        SDL_RenderFillRect(r, &d);
    }
}

static void drawAlbumArtWell(SDL_Renderer* r, TTF_Font* fontBig, TTF_Font* fontSmall,
                             const CoverArt& art, const Entry* e, AudioFmt fmt) {
    const int x = 46, y = 148, sz = 248;
    drawGlassPanel(r, x, y, sz, sz, COL_PRIMARY, 144);
    setColor(r, COL_DISPLAY, 232);
    fillRect(r, x + 12, y + 12, sz - 24, sz - 24);

    if (art.texture) {
        const float srcAspect = art.h ? (float)art.w / (float)art.h : 1.0f;
        const int box = sz - 24;
        int dw = box, dh = box;
        if (srcAspect > 1.0f) dh = (int)(box / srcAspect);
        else                  dw = (int)(box * srcAspect);
        SDL_Rect dst{ x + 12 + (box - dw) / 2, y + 12 + (box - dh) / 2, dw, dh };
        SDL_RenderCopy(r, art.texture, nullptr, &dst);
        drawFormatChip(r, fontSmall, art.source == "embedded" ? "Embedded art" : "Cover art", x + 20, y + sz - 42, COL_PRIMARY, true);
        return;
    }

    setColor(r, COL_PRIMARY, 54);
    for (int i = 0; i < 7; ++i) {
        const int inset = 26 + i * 12;
        SDL_Rect rr{ x + inset, y + inset, sz - 2 * inset, sz - 2 * inset };
        SDL_RenderDrawRect(r, &rr);
    }
    const std::string label = e ? ellipsizeText(fontBig, e->label, sz - 50) : "Ampintosh";
    drawText(r, fontBig, "ART", x + sz / 2, y + 76, COL_PRIMARY, 2);
    drawText(r, fontSmall, label, x + sz / 2, y + 126, COL_TEXT, 2);
    drawText(r, fontSmall, fmtName(fmt), x + sz / 2, y + 158, COL_MUTED, 2);
    drawText(r, fontSmall, "Sidecar or embedded", x + sz / 2, y + sz - 52, COL_TERTIARY, 2);
}

static void drawButtonHelp(SDL_Renderer* r, TTF_Font* fontSmall, const std::vector<std::pair<std::string, std::string>>& items) {
    drawGlassPanel(r, 30, SCREEN_H - 94, SCREEN_W - 60, 68, COL_TERTIARY, 128);
    int x = 54;
    const int y = SCREEN_H - 74;
    for (const auto& it : items) {
        const int chipW = drawFormatChip(r, fontSmall, it.first, x, y - 2, COL_PRIMARY, true);
        x += chipW + 8;
        x += drawText(r, fontSmall, it.second, x, y + 2, COL_TEXT) + 18;
        if (x > SCREEN_W - 180) break;
    }
}

static void drawSourceMenu(SDL_Renderer* r, TTF_Font* fontBig, TTF_Font* fontSmall,
                           const std::vector<SourceMenuItem>& items, int selected,
                           const std::string& configPath, float phase, bool canReturnToPlayer) {
    drawLiquidRetroBackground(r, phase);
    drawGlassPanel(r, 30, 26, SCREEN_W - 60, 104, COL_TERTIARY, 126);
    drawText(r, fontSmall, std::string("AMPINTOSH SWITCH  ·  ") + gSkin.name, 54, 38, COL_MUTED);
    drawText(r, fontBig, "Choose Music Source", 54, 68, COL_TEXT);
    drawText(r, fontSmall, canReturnToPlayer ? "- toggles player / source menu" : "B opens a source", SCREEN_W - 54, 78, COL_TERTIARY, 1);

    const int cardX = 120, cardY = 174, cardW = SCREEN_W - 240, cardH = 372;
    drawGlassPanel(r, cardX, cardY, cardW, cardH, COL_PRIMARY, 132);
    drawText(r, fontSmall, "Parent menu", cardX + 24, cardY + 18, COL_TERTIARY);
    drawText(r, fontSmall, configPath.empty() ? "Config: defaults" : ellipsizeText(fontSmall, std::string("Config: ") + configPath, 620),
             cardX + cardW - 24, cardY + 18, COL_MUTED, 1);

    const int rowY0 = cardY + 64;
    const int rowH = 86;
    for (int i = 0; i < (int)items.size(); ++i) {
        const SourceMenuItem& item = items[i];
        const bool cur = i == selected;
        const int y = rowY0 + i * rowH;
        if (cur) {
            setColor(r, COL_PRIMARY, 90);
            fillRect(r, cardX + 18, y - 8, cardW - 36, rowH - 12);
            setColor(r, COL_PRIMARY, 230);
            fillRect(r, cardX + 18, y - 8, 5, rowH - 12);
        }

        RGB chipColor = item.kind == SourceKind::LocalFiles ? COL_PRIMARY : (item.kind == SourceKind::Network ? COL_SECONDARY : COL_TERTIARY);
        const char* chip = item.kind == SourceKind::LocalFiles ? "SD" : (item.kind == SourceKind::Network ? "NET" : "USB");
        drawFormatChip(r, fontSmall, chip, cardX + 44, y + 12, cur ? COL_SECONDARY : chipColor, cur);
        drawText(r, fontBig, item.label, cardX + 132, y + 4, item.available ? COL_TEXT : COL_MUTED);
        drawText(r, fontSmall, item.detail, cardX + 132, y + 42, item.available ? COL_TERTIARY : COL_MUTED);
        drawText(r, fontSmall, item.available ? "B open" : "Unavailable", cardX + cardW - 44, y + 24, item.available ? COL_PRIMARY : COL_MUTED, 1);
    }

    drawGlassPanel(r, 120, 574, SCREEN_W - 240, 46, COL_SECONDARY, 110);
    drawText(r, fontSmall, "Local files browses SD/card roots. USB browses only mounted usb:/ roots. Network opens stream entries.", 144, 588, COL_TEXT);

    drawButtonHelp(r, fontSmall, {
        {"B", "Open source"}, {"D-Pad/L-Stick", "Select"}, {"X", "Rescan"},
        {"ZL/ZR", "Skin"}, {"-", canReturnToPlayer ? "Player" : "Menu"}, {"+", "Quit"}
    });
}

static void drawBrowser(SDL_Renderer* r, TTF_Font* fontBig, TTF_Font* fontSmall,
                        const std::vector<BrowserItem>& items,
                        const std::vector<Entry>& entries,
                        const std::vector<DirectoryGroup>& dirs,
                        const std::string& currentDir, SourceKind source,
                        int selected, int offset, const std::string& configPath,
                        PlaybackMode playbackMode, VisualizerMode visualizer, float phase,
                        bool canReturnToPlayer) {
    drawLiquidRetroBackground(r, phase);
    drawGlassPanel(r, 30, 26, SCREEN_W - 60, 90, COL_TERTIARY, 126);
    drawText(r, fontSmall, std::string("AMPINTOSH SWITCH  ·  ") + gSkin.name, 54, 38, COL_MUTED);
    drawText(r, fontBig, isBrowserRoot(currentDir) ? sourceKindName(source) : pathLeaf(currentDir), 54, 66, COL_TEXT);
    drawText(r, fontSmall, canReturnToPlayer ? "- toggles player / browser" : "B opens folders or starts a selected song", SCREEN_W - 54, 76, COL_TERTIARY, 1);

    const int listX = 46, listY = 136, listW = 828, listH = 458;
    drawGlassPanel(r, listX, listY, listW, listH, COL_PRIMARY, 132);
    drawText(r, fontSmall, isBrowserRoot(currentDir) ? (std::string(sourceKindName(source)) + " roots") : ellipsizeText(fontSmall, currentDir, 560), listX + 18, listY + 14, COL_TERTIARY);
    char countBuf[96];
    snprintf(countBuf, sizeof(countBuf), "%d item%s · %d track%s · %d folder%s",
             (int)items.size(), items.size() == 1 ? "" : "s",
             (int)entries.size(), entries.size() == 1 ? "" : "s",
             (int)dirs.size(), dirs.size() == 1 ? "" : "s");
    drawText(r, fontSmall, countBuf, listX + listW - 18, listY + 14, COL_MUTED, 1);

    const int rowY0 = listY + 54;
    const int rowH = 34;
    const int rows = (listH - 70) / rowH;
    if (items.empty()) {
        drawText(r, fontBig, "No items here", listX + 30, rowY0 + 52, COL_TEXT);
        drawText(r, fontSmall, "Use .. to return to the source menu, edit ampintosh.ini, attach USB, or press X to rescan.", listX + 30, rowY0 + 92, COL_TERTIARY);
    } else {
        for (int row = 0; row < rows; ++row) {
            const int idx = offset + row;
            if (idx < 0 || idx >= (int)items.size()) break;
            const BrowserItem& item = items[idx];
            const bool cur = idx == selected;
            const int y = rowY0 + row * rowH;
            if (cur) {
                setColor(r, COL_PRIMARY, 90);
                fillRect(r, listX + 12, y - 3, listW - 24, rowH - 2);
                setColor(r, COL_PRIMARY, 230);
                fillRect(r, listX + 12, y - 3, 4, rowH - 2);
            }

            const char* chip = "FILE";
            RGB chipColor = COL_MUTED;
            switch (item.kind) {
                case BrowserItemKind::Up:           chip = "UP";  chipColor = COL_TERTIARY; break;
                case BrowserItemKind::Directory:    chip = "DIR"; chipColor = COL_PRIMARY;  break;
                case BrowserItemKind::StreamFolder: chip = "NET"; chipColor = COL_SECONDARY; break;
                case BrowserItemKind::Stream:       chip = "NET"; chipColor = COL_SECONDARY; break;
                case BrowserItemKind::Track: {
                    const AudioFmt f = fmtForLocalFile(item.path);
                    chip = *fmtName(f) ? fmtName(f) : "FILE";
                    chipColor = COL_MUTED;
                    break;
                }
            }
            drawFormatChip(r, fontSmall, chip, listX + 24, y, cur ? COL_SECONDARY : chipColor, cur);

            std::string title = item.label;
            if ((item.kind == BrowserItemKind::Directory || item.kind == BrowserItemKind::StreamFolder) && item.trackCount > 0) {
                char nbuf[40];
                snprintf(nbuf, sizeof(nbuf), "  ·  %d track%s", item.trackCount, item.trackCount == 1 ? "" : "s");
                title += nbuf;
            }
            drawText(r, fontSmall, ellipsizeText(fontSmall, title, listW - 220), listX + 104, y + 4, cur ? COL_TEXT : COL_MUTED);

            char idxBuf[16]; snprintf(idxBuf, sizeof(idxBuf), "%02d", idx + 1);
            drawText(r, fontSmall, idxBuf, listX + listW - 28, y + 4, cur ? COL_PRIMARY : COL_MUTED, 1);
        }
    }

    const int infoX = 902, infoY = 136, infoW = 332, infoH = 458;
    drawGlassPanel(r, infoX, infoY, infoW, infoH, COL_SECONDARY, 132);
    drawText(r, fontSmall, "Selection", infoX + 18, infoY + 14, COL_TERTIARY);
    if (!items.empty()) {
        const BrowserItem& item = items[std::max(0, std::min(selected, (int)items.size() - 1))];
        drawText(r, fontBig, ellipsizeText(fontBig, item.label, infoW - 36), infoX + 18, infoY + 50, COL_TEXT);
        const std::string safeItemPath = item.path.empty() ? std::string("Library roots") : redactURLCredentials(item.path);
        drawText(r, fontSmall, ellipsizeText(fontSmall, safeItemPath, infoW - 36), infoX + 18, infoY + 94, COL_MUTED);

        int chipX = infoX + 18;
        if (item.kind == BrowserItemKind::Directory || item.kind == BrowserItemKind::StreamFolder) {
            chipX += drawFormatChip(r, fontSmall, item.kind == BrowserItemKind::StreamFolder ? "STREAMS" : "FOLDER", chipX, infoY + 132, COL_PRIMARY, true) + 8;
            if (item.trackCount > 0) {
                char nbuf[32]; snprintf(nbuf, sizeof(nbuf), "%d SONGS", item.trackCount);
                drawFormatChip(r, fontSmall, nbuf, chipX, infoY + 132, COL_SECONDARY, true);
            }
            drawText(r, fontSmall, item.kind == BrowserItemKind::StreamFolder ? "B opens this network item." : "B enters this folder.", infoX + 18, infoY + 190, COL_TEXT);
            drawText(r, fontSmall, item.kind == BrowserItemKind::StreamFolder ? "Direct file targets can play." : "Select a song inside to open", infoX + 18, infoY + 222, COL_MUTED);
            drawText(r, fontSmall, item.kind == BrowserItemKind::StreamFolder ? "SMB/SFTP folders can be browsed." : "the playback UI.", infoX + 18, infoY + 248, COL_MUTED);
        } else if (item.kind == BrowserItemKind::Up) {
            drawFormatChip(r, fontSmall, "UP", chipX, infoY + 132, COL_TERTIARY, true);
            drawText(r, fontSmall, "B goes up one directory.", infoX + 18, infoY + 190, COL_TEXT);
            drawText(r, fontSmall, "At the top, this returns to", infoX + 18, infoY + 222, COL_MUTED);
            drawText(r, fontSmall, item.path == BROWSER_SOURCE_MENU ? "the source menu." : "the source roots.", infoX + 18, infoY + 248, COL_MUTED);
        } else {
            const bool isStream = item.kind == BrowserItemKind::Stream;
            const AudioFmt f = isStream ? AudioFmt::None : fmtForLocalFile(item.path);
            chipX += drawFormatChip(r, fontSmall, isStream ? "STREAM" : (*fmtName(f) ? fmtName(f) : "FILE"), chipX, infoY + 132, COL_PRIMARY, true) + 8;
            chipX += drawFormatChip(r, fontSmall, playbackModeName(playbackMode), chipX, infoY + 132, COL_SECONDARY, true) + 8;
            drawFormatChip(r, fontSmall, visualizerName(visualizer), infoX + 18, infoY + 172, COL_TERTIARY, true);
            if (item.entryIndex >= 0 && item.entryIndex < (int)entries.size()) {
                const int dirIdx = directoryIndexForEntry(dirs, item.entryIndex);
                if (dirIdx >= 0) {
                    char dirBuf[64];
                    snprintf(dirBuf, sizeof(dirBuf), "Folder %d/%d · %d file%s", dirIdx + 1, (int)dirs.size(),
                             dirs[dirIdx].count, dirs[dirIdx].count == 1 ? "" : "s");
                    drawText(r, fontSmall, dirBuf, infoX + 18, infoY + 222, COL_TERTIARY);
                    drawText(r, fontSmall, ellipsizeText(fontSmall, dirs[dirIdx].path, infoW - 36), infoX + 18, infoY + 248, COL_MUTED);
                }
            }
            drawText(r, fontSmall, "B opens the player and starts", infoX + 18, infoY + 302, COL_TEXT);
            drawText(r, fontSmall, "this selected item only now.", infoX + 18, infoY + 328, COL_TEXT);
            drawText(r, fontSmall, "After this folder ends, playback", infoX + 18, infoY + 368, COL_MUTED);
            drawText(r, fontSmall, "rolls into the next folder.", infoX + 18, infoY + 394, COL_MUTED);
        }
        drawText(r, fontSmall, "- toggles browser / player views.", infoX + 18, infoY + 428, COL_TERTIARY);
    } else {
        drawText(r, fontSmall, "Supported: MP3, FLAC, WAV, AIFF/AIFC PCM, M3U", infoX + 18, infoY + 70, COL_TEXT);
        drawText(r, fontSmall, "USB paths use usb:/... in ampintosh.ini", infoX + 18, infoY + 108, COL_MUTED);
        drawText(r, fontSmall, "Network uses stream_url, SMB, and SFTP sections", infoX + 18, infoY + 142, COL_MUTED);
    }

    if (!configPath.empty())
        drawText(r, fontSmall, "INI: " + ellipsizeText(fontSmall, configPath, 620), 54, SCREEN_H - 120, COL_MUTED);

    drawButtonHelp(r, fontSmall, {
        {"↑↓/L-Stick", "Browse"}, {"B", "Open/Play"}, {"A", "Mode"},
        {"Y", "Viz"}, {"X", "Rescan"}, {"ZL/ZR", "Skin"}, {"-", canReturnToPlayer ? "Player" : "Files"}, {"+", "Quit"}
    });
}

// Dim idle bars + a hint marker when no tracks were found.
static void drawEmptyState(SDL_Renderer* r, float phase) {
    drawVisualizerBackground(r);
    const int marginX = 60, baseY = SCREEN_H - 150;
    const float gap = 5.0f;
    const float bw = (SCREEN_W - 2 * marginX - gap * (NUM_BANDS - 1)) / NUM_BANDS;
    for (int i = 0; i < NUM_BANDS; ++i) {
        const float wob = 0.5f + 0.5f * sinf(phase + i * 0.3f);
        const int h = (int)(20 + 64 * wob);
        const int x = (int)(marginX + i * (bw + gap));
        setColor(r, COL_PANEL, 210);
        fillRect(r, x, baseY - h, (int)bw, h);
        setColor(r, COL_MUTED, 52);
        fillRect(r, x, baseY - h - 3, (int)bw, 2);
    }
}

static void syncRepeatFlag(Player& p, PlaybackMode mode, size_t entryCount) {
    p.repeatOne.store(mode == PlaybackMode::RepeatOne || entryCount <= 1);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
// Load the UI font. Prefer the Switch system shared font, which includes full
// Japanese (and Latin) glyphs so CJK filenames render properly; fall back to the
// bundled romfs font if the pl service is unavailable. g_sysFontData must stay
// alive for the whole program because SDL_ttf reads glyphs from it on demand.
static PlFontData g_sysFontData;
static bool       g_sysFontReady = false;

static TTF_Font* openUiFont(int pt) {
    if (g_sysFontReady) {
        SDL_RWops* rw = SDL_RWFromConstMem(g_sysFontData.address, (int)g_sysFontData.size);
        if (rw) {
            TTF_Font* f = TTF_OpenFontRW(rw, 1, pt);   // freesrc=1: SDL frees rw with the font
            if (f) return f;
        }
    }
    return TTF_OpenFont("romfs:/font.ttf", pt);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    SDL_SetMainReady();   // required when SDL_MAIN_HANDLED is defined

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
        return 1;

    std::string configPath;
    AppConfig config = loadAppConfig(configPath);
    PlaybackMode playbackMode = config.playbackMode;
    VisualizerMode visualizer = config.visualizer;
    gSkin = paletteForSkin(config.skin);

#ifdef AMPINTOSH_NET
    if (config.netEnabled) {
        Result sockRc = socketInitializeDefault();
        if (R_SUCCEEDED(sockRc)) {
            g_socketReady = true;
            if (curl_global_init(CURL_GLOBAL_DEFAULT) == 0) {
                g_netReady = true;
            } else {
                socketExit();
                g_socketReady = false;
            }
        }
    }
#endif
    lastFmStart(config);
#ifdef AMPINTOSH_USB
    if (config.usbEnabled) {
        Result usbRc = usbHsFsInitialize(0);
        if (R_SUCCEEDED(usbRc)) {
            g_usbReady = true;
            waitForUsbMounts(config.usbWaitMs);
        }
    }
#endif

    SDL_Window* win = SDL_CreateWindow("Ampintosh", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, SCREEN_W, SCREEN_H, 0);
    if (!win) { SDL_Quit(); return 1; }

    SDL_Renderer* ren = SDL_CreateRenderer(win, -1,
                            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) { SDL_DestroyWindow(win); SDL_Quit(); return 1; }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);

    // RomFS is packed into the .nro; open the bundled font from it. Both are
    // optional — if anything here fails the app still runs, just without text.
    romfsInit();
    plInitialize(PlServiceType_User);
    g_sysFontReady = R_SUCCEEDED(plGetSharedFontByType(&g_sysFontData, PlSharedFontType_Standard));

    TTF_Font* fontBig = nullptr;
    TTF_Font* fontSmall = nullptr;
    if (TTF_Init() == 0) {
        fontBig   = openUiFont(26);
        fontSmall = openUiFont(20);
    }

    // The combined "Player 1" handheld/controller is index 0.
    SDL_GameController* pad = nullptr;
    if (SDL_NumJoysticks() > 0 && SDL_IsGameController(0))
        pad = SDL_GameControllerOpen(0);

    Player   player;
    Analyzer analyzer;
    analyzer.init(player.sampleRate);

    std::mt19937 rng((uint32_t)std::time(nullptr) ^ (uint32_t)SDL_GetTicks());

    // The app now opens into a parent source menu. No track is decoded and no
    // playback UI is shown until the user explicitly selects a source, then a track.
    std::vector<Entry> entries = buildLibrary(config);
    std::vector<DirectoryGroup> directories = buildDirectoryGroups(entries);
    std::vector<SourceMenuItem> sourceItems = buildSourceMenuItems(entries, config);
    SourceKind activeSource = SourceKind::LocalFiles;
    std::vector<BrowserItem> browserItems;
    std::string browserPath = BROWSER_ROOT;
    std::vector<int> shuffleBag;
    int trackIndex = 0;
    int sourceIndex = 0;
    int browserIndex = 0;
    int browserOffset = 0;
    bool haveTrack = false;
    AppScreen screen = AppScreen::SourceMenu;
    CoverArt coverArt;
    syncRepeatFlag(player, playbackMode, entries.size());

    auto refreshSourceItems = [&]() {
        sourceItems = buildSourceMenuItems(entries, config);
    };
    auto refreshLibraryDerived = [&]() {
        directories = buildDirectoryGroups(entries);
        refreshSourceItems();
        syncRepeatFlag(player, playbackMode, entries.size());
    };
    auto cycleSkin = [&](int direction = 1) {
        config.skin = nextSkinMode(config.skin, direction);
        gSkin = paletteForSkin(config.skin);
    };
    auto clampSource = [&]() {
        if (sourceItems.empty()) { sourceIndex = 0; return; }
        sourceIndex = std::max(0, std::min(sourceIndex, (int)sourceItems.size() - 1));
    };
    auto refreshBrowserItems = [&](const std::string& preferredPath = std::string()) {
        browserItems = buildBrowserItems(browserPath, activeSource, entries, config);
        if (!preferredPath.empty()) {
            for (int i = 0; i < (int)browserItems.size(); ++i) {
                if (browserItems[i].path == preferredPath) { browserIndex = i; break; }
            }
        }
    };
    auto clampBrowser = [&]() {
        if (browserItems.empty()) { browserIndex = 0; browserOffset = 0; return; }
        browserIndex = std::max(0, std::min(browserIndex, (int)browserItems.size() - 1));
        const int visibleRows = 11;
        if (browserIndex < browserOffset) browserOffset = browserIndex;
        if (browserIndex >= browserOffset + visibleRows) browserOffset = browserIndex - visibleRows + 1;
        browserOffset = std::max(0, std::min(browserOffset, std::max(0, (int)browserItems.size() - visibleRows)));
    };
    auto moveMenuSelection = [&](int delta) {
        if (delta == 0) return;
        if (screen == AppScreen::SourceMenu) {
            sourceIndex += delta;
            clampSource();
        } else if (screen == AppScreen::Browser && !browserItems.empty()) {
            browserIndex += delta;
            clampBrowser();
        }
    };
    auto navigateBrowserTo = [&](const std::string& path, const std::string& preferredPath = std::string()) {
        browserPath = path;
        browserIndex = 0;
        browserOffset = 0;
        refreshBrowserItems(preferredPath);
        clampBrowser();
    };
    auto openSourceSelection = [&]() {
        if (sourceItems.empty()) return;
        const SourceMenuItem& item = sourceItems[std::max(0, std::min(sourceIndex, (int)sourceItems.size() - 1))];
        if (!item.available) return;
        activeSource = item.kind;
        navigateBrowserTo(activeSource == SourceKind::Network ? std::string(BROWSER_NET) : std::string(BROWSER_ROOT));
        screen = AppScreen::Browser;
    };
    refreshSourceItems();
    clampSource();
    auto rebuildShuffleBag = [&](int dirIdx, int excludeIndex) {
        shuffleBag.clear();
        if (dirIdx < 0 || dirIdx >= (int)directories.size()) return;
        const DirectoryGroup& d = directories[dirIdx];
        for (int i = d.first; i < d.first + d.count; ++i) {
            if (i != excludeIndex) shuffleBag.push_back(i);
        }
        std::shuffle(shuffleBag.begin(), shuffleBag.end(), rng);
    };
    auto removeFromShuffleBag = [&](int idx) {
        shuffleBag.erase(std::remove(shuffleBag.begin(), shuffleBag.end(), idx), shuffleBag.end());
    };
    auto loadSelected = [&](int idx, bool startPlaying, bool resetQueue) {
        if (entries.empty()) {
            haveTrack = false; unloadPlayer(player); clearCoverArt(coverArt);
            shuffleBag.clear(); screen = AppScreen::Browser; return;
        }
        trackIndex = std::max(0, std::min(idx, (int)entries.size() - 1));
        haveTrack = tryLoadCurrent(entries, trackIndex, player, analyzer, config, startPlaying);
        if (haveTrack) {
            const TrackMetadata loadedMeta = metadataForRef(entries[trackIndex].ref);
            const std::string loadedLabel = metadataDisplayLabel(loadedMeta, entries[trackIndex].label.empty() ? displayName(entries[trackIndex].ref) : entries[trackIndex].label);
            if (!loadedLabel.empty()) entries[trackIndex].label = loadedLabel;
            loadCoverArtForEntry(ren, coverArt, entries[trackIndex], player.fmt, config);
            if (player.playing.load()) lastFmBeginTrack(entries[trackIndex], player, config);
            if (playbackMode == PlaybackMode::Shuffle) {
                if (resetQueue) rebuildShuffleBag(directoryIndexForEntry(directories, trackIndex), trackIndex);
                else removeFromShuffleBag(trackIndex);
            } else {
                shuffleBag.clear();
            }
            screen = AppScreen::Player;
        } else {
            clearCoverArt(coverArt);
            shuffleBag.clear();
            screen = AppScreen::Browser;
        }
        syncRepeatFlag(player, playbackMode, entries.size());
    };
    auto showCurrentTrackInBrowser = [&]() {
        if (haveTrack && trackIndex >= 0 && trackIndex < (int)entries.size()) {
            const Entry& e = entries[trackIndex];
            if (e.isURL) activeSource = SourceKind::Network;
            else if (looksLikeUsbMountPath(e.ref) || looksLikeUsbMountPath(e.dir)) activeSource = SourceKind::Usb;
            else activeSource = SourceKind::LocalFiles;
            navigateBrowserTo(e.isURL ? std::string(BROWSER_NET) : e.dir, e.ref);
        } else {
            screen = AppScreen::SourceMenu;
            return;
        }
        screen = AppScreen::Browser;
    };
    auto openBrowserSelection = [&]() {
        if (browserItems.empty()) return;
        const BrowserItem& item = browserItems[std::max(0, std::min(browserIndex, (int)browserItems.size() - 1))];
        switch (item.kind) {
            case BrowserItemKind::Up:
                if (isBrowserSourceMenu(item.path)) screen = AppScreen::SourceMenu;
                else navigateBrowserTo(item.path);
                break;
            case BrowserItemKind::Directory:
                navigateBrowserTo(item.path);
                break;
            case BrowserItemKind::StreamFolder:
                if (isNetworkBookmarkPath(item.path)) {
                    const int nb = networkBookmarkIndexFromPath(item.path);
                    if (nb >= 0 && nb < (int)config.networkBookmarks.size()) {
                        const NetworkBookmark& b = config.networkBookmarks[nb];
                        const std::string url = networkBookmarkURL(b);
                        if (!url.empty() && isPlayableAudioFile(url)) {
                            int idx = findEntryIndexByRef(entries, url);
                            if (idx < 0) {
                                entries.push_back({ url, b.sectionName.empty() ? displayName(url) : b.sectionName, directoryName(url), true });
                                normalizeLibraryEntries(entries);
                                refreshLibraryDerived();
                                idx = findEntryIndexByRef(entries, url);
                            }
                            if (idx >= 0) loadSelected(idx, true, true);
                        } else {
                            navigateBrowserTo(item.path);
                        }
                    }
                } else {
                    navigateBrowserTo(item.path);
                }
                break;
            case BrowserItemKind::Track:
            case BrowserItemKind::Stream: {
                int idx = item.entryIndex;
                if (item.kind == BrowserItemKind::Track) {
                    if (idx < 0 || idx >= (int)entries.size()) {
                        if (addDirectPlayableFilesFromDir(entries, directoryName(item.path))) {
                            refreshLibraryDerived();
                            refreshBrowserItems(item.path);
                            clampBrowser();
                        }
                        idx = findEntryIndexByRef(entries, item.path);
                    }
                }
                if (item.kind == BrowserItemKind::Stream && !item.path.empty()) {
                    bool addedStreams = false;
                    for (const BrowserItem& bi : browserItems) {
                        if (bi.kind != BrowserItemKind::Stream || bi.path.empty()) continue;
                        if (findEntryIndexByRef(entries, bi.path) >= 0) continue;
                        entries.push_back({ bi.path, bi.label.empty() ? displayName(bi.path) : bi.label, directoryName(bi.path), true });
                        addedStreams = true;
                    }
                    if (addedStreams) {
                        normalizeLibraryEntries(entries);
                        refreshLibraryDerived();
                    }
                    idx = findEntryIndexByRef(entries, item.path);
                }
                if (idx >= 0 && idx < (int)entries.size())
                    loadSelected(idx, true, true);
                break;
            }
        }
    };
    auto nextQueueIndex = [&](int direction) -> int {
        if (entries.empty()) return 0;
        if (playbackMode == PlaybackMode::RepeatOne) return trackIndex;

        if (playbackMode == PlaybackMode::Shuffle && direction >= 0) {
            const int curDir = directoryIndexForEntry(directories, trackIndex);
            if (shuffleBag.empty()) {
                // Current directory has been exhausted. Roll into the next
                // scanned directory and shuffle that folder before any wrap.
                const int nextDir = directories.empty() ? -1 : ((curDir + 1 + (int)directories.size()) % (int)directories.size());
                rebuildShuffleBag(nextDir, nextDir == curDir ? trackIndex : -1);
            }
            if (!shuffleBag.empty()) {
                const int next = shuffleBag.back();
                shuffleBag.pop_back();
                return next;
            }
        }

        // Repeat All and backwards shuffle movement are directory-ordered: finish
        // a folder, then roll into the next/previous folder instead of looping a
        // visible browser page.
        return stepDirectoryOrderedIndex(trackIndex, direction, directories, (int)entries.size());
    };
    auto rescanLibrary = [&]() {
#ifdef AMPINTOSH_USB
        if (config.usbEnabled && g_usbReady) waitForUsbMounts(config.usbWaitMs);
#endif
        const std::string currentRef = haveTrack && trackIndex >= 0 && trackIndex < (int)entries.size() ? entries[trackIndex].ref : std::string();
        const std::string selectedPath = (!browserItems.empty() && browserIndex >= 0 && browserIndex < (int)browserItems.size())
            ? browserItems[browserIndex].path : std::string();

        entries = buildLibrary(config);
        refreshLibraryDerived();
        clampSource();
        const int found = currentRef.empty() ? -1 : findEntryIndexByRef(entries, currentRef);
        if (found >= 0) {
            trackIndex = found;
            if (playbackMode == PlaybackMode::Shuffle) rebuildShuffleBag(directoryIndexForEntry(directories, trackIndex), trackIndex);
        } else if (haveTrack) {
            haveTrack = false;
            unloadPlayer(player);
            clearCoverArt(coverArt);
            shuffleBag.clear();
            screen = AppScreen::SourceMenu;
        }

        if (isBrowserSourceMenu(browserPath)) {
            screen = AppScreen::SourceMenu;
        } else if (!isBrowserRoot(browserPath) && !isBrowserNet(browserPath) && !pathExistsAsDir(browserPath)) {
            browserPath = BROWSER_ROOT;
        }
        if (screen == AppScreen::Browser || screen == AppScreen::Player) {
            refreshBrowserItems(selectedPath);
            clampBrowser();
        }
        syncRepeatFlag(player, playbackMode, entries.size());
    };

    float smoothBands[NUM_BANDS] = {0};
    float smoothAmp = 0.0f;
    float idlePhase = 0.0f;
    bool  running = true;
    bool  zlHeld = false;
    bool  zrHeld = false;
    int   listStickDir = 0;
    Uint32 nextListStickTick = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;

            if (ev.type == SDL_CONTROLLERAXISMOTION) {
                if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                    const bool down = ev.caxis.value > 18000;
                    bool& held = (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) ? zlHeld : zrHeld;
                    if (down && !held) cycleSkin(ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? -1 : 1);
                    held = down;
                }
            }

            if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                const int btn = ev.cbutton.button;
                if (btn == SDL_CONTROLLER_BUTTON_START) {      // physical +
                    running = false;
                    continue;
                }

                if (screen == AppScreen::SourceMenu) {
                    switch (btn) {
                        case SDL_CONTROLLER_BUTTON_DPAD_UP:
                            moveMenuSelection(-1);
                            break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                            moveMenuSelection(1);
                            break;
                        case SDL_CONTROLLER_BUTTON_A:            // physical B: open source
                            openSourceSelection();
                            break;
                        case SDL_CONTROLLER_BUTTON_Y:            // physical X: rescan library
                            rescanLibrary();
                            break;
                        case SDL_CONTROLLER_BUTTON_BACK:         // physical -: player if one is loaded
                            if (haveTrack) screen = AppScreen::Player;
                            break;
                        default: break;
                    }
                } else if (screen == AppScreen::Browser) {
                    switch (btn) {
                        case SDL_CONTROLLER_BUTTON_DPAD_UP:
                            moveMenuSelection(-1);
                            break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                            moveMenuSelection(1);
                            break;
                        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                            moveMenuSelection(-11);
                            break;
                        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                            moveMenuSelection(11);
                            break;
                        case SDL_CONTROLLER_BUTTON_A:            // physical B: play selected
                            openBrowserSelection();
                            break;
                        case SDL_CONTROLLER_BUTTON_B:            // physical A: Shuffle / Repeat One / Repeat All
                            playbackMode = nextPlaybackMode(playbackMode);
                            if (playbackMode == PlaybackMode::Shuffle && haveTrack)
                                rebuildShuffleBag(directoryIndexForEntry(directories, trackIndex), trackIndex);
                            else
                                shuffleBag.clear();
                            syncRepeatFlag(player, playbackMode, entries.size());
                            break;
                        case SDL_CONTROLLER_BUTTON_X:            // physical Y: visualizer cycle
                            visualizer = nextVisualizerMode(visualizer);
                            break;
                        case SDL_CONTROLLER_BUTTON_Y:            // physical X: rescan library
                            rescanLibrary();
                            break;
                        case SDL_CONTROLLER_BUTTON_BACK:         // physical -: toggle back to player/source menu
                            if (haveTrack) screen = AppScreen::Player;
                            else screen = AppScreen::SourceMenu;
                            break;
                        default: break;
                    }
                } else {
                    switch (btn) {
                        case SDL_CONTROLLER_BUTTON_BACK:         // physical -: back to file browser
                            showCurrentTrackInBrowser();
                            break;
                        case SDL_CONTROLLER_BUTTON_A:            // physical B: play / pause
                            if (haveTrack) {
                                player.finished.store(false);
                                player.playing.store(!player.playing.load());
                            }
                            break;
                        case SDL_CONTROLLER_BUTTON_B:            // physical A: Shuffle / Repeat One / Repeat All
                            playbackMode = nextPlaybackMode(playbackMode);
                            if (playbackMode == PlaybackMode::Shuffle && haveTrack)
                                rebuildShuffleBag(directoryIndexForEntry(directories, trackIndex), trackIndex);
                            else
                                shuffleBag.clear();
                            syncRepeatFlag(player, playbackMode, entries.size());
                            break;
                        case SDL_CONTROLLER_BUTTON_X:            // physical Y: visualizer cycle
                            visualizer = nextVisualizerMode(visualizer);
                            break;
                        case SDL_CONTROLLER_BUTTON_Y:            // physical X: rescan library
                            rescanLibrary();
                            break;
                        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: // previous (L)
                            if (haveTrack && entries.size() > 1) {
                                const bool keepPlaying = player.playing.load();
                                const int next = nextQueueIndex(-1);
                                loadSelected(next, keepPlaying, false);
                            }
                            break;
                        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:// next (R)
                            if (haveTrack && entries.size() > 1) {
                                const bool keepPlaying = player.playing.load();
                                const int next = nextQueueIndex(1);
                                loadSelected(next, keepPlaying, false);
                            }
                            break;
                        default: break;
                    }
                }
            }

            if (ev.type == SDL_KEYDOWN) {
                const SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_ESCAPE) running = false;
                else if (screen == AppScreen::SourceMenu) {
                    if (k == SDLK_UP) { moveMenuSelection(-1); }
                    else if (k == SDLK_DOWN) { moveMenuSelection(1); }
                    else if (k == SDLK_RETURN || k == SDLK_SPACE) openSourceSelection();
                    else if (k == 's' || k == ']') cycleSkin(1);
                    else if (k == '[') cycleSkin(-1);
                    else if (k == 'r') rescanLibrary();
                    else if (k == SDLK_BACKSPACE && haveTrack) screen = AppScreen::Player;
                }
                else if (screen == AppScreen::Browser) {
                    if (k == SDLK_UP) { moveMenuSelection(-1); }
                    else if (k == SDLK_DOWN) { moveMenuSelection(1); }
                    else if (k == SDLK_PAGEUP) { moveMenuSelection(-11); }
                    else if (k == SDLK_PAGEDOWN) { moveMenuSelection(11); }
                    else if (k == SDLK_RETURN || k == SDLK_SPACE) openBrowserSelection();
                    else if (k == 's' || k == ']') cycleSkin(1);
                    else if (k == '[') cycleSkin(-1);
                    else if (k == SDLK_BACKSPACE) {
                        if (!isBrowserRoot(browserPath)) navigateBrowserTo(parentDirectory(browserPath));
                        else screen = AppScreen::SourceMenu;
                    }
                } else if (k == 's' || k == ']') {
                    cycleSkin(1);
                } else if (k == '[') {
                    cycleSkin(-1);
                } else if (k == SDLK_BACKSPACE) {
                    showCurrentTrackInBrowser();
                }
            }

            // Touchscreen: tap or drag along the lower part of the screen to
            // seek. tfinger coords are normalized 0..1.
            if (ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERMOTION) {
                if (screen == AppScreen::Player && haveTrack && ev.tfinger.y > 0.80f)
                    seekToFraction(player, ev.tfinger.x);
            }
        }

        // Analog-stick list navigation: in the source menu and browser, hold
        // the left stick up/down to move the highlighted row.  A light push
        // behaves like D-pad taps; a full push accelerates through long
        // folders so huge SMB/USB/SD libraries are not D-pad torture.
        if (pad && (screen == AppScreen::SourceMenu || screen == AppScreen::Browser)) {
            const Sint16 ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
            const int dead = 9000;
            const int magRaw = std::abs((int)ay);
            int dir = 0;
            if (ay > dead) dir = 1;
            else if (ay < -dead) dir = -1;

            if (dir == 0) {
                listStickDir = 0;
                nextListStickTick = 0;
            } else {
                const float mag = std::min(1.0f, (float)(magRaw - dead) / (32767.0f - (float)dead));
                int step = 1;
                Uint32 intervalMs = 135;
                if (screen == AppScreen::Browser) {
                    if (mag >= 0.86f) { step = 6; intervalMs = 45; }
                    else if (mag >= 0.62f) { step = 3; intervalMs = 75; }
                    else if (mag >= 0.34f) { step = 1; intervalMs = 105; }
                } else {
                    intervalMs = mag >= 0.62f ? 95 : 150;
                }

                const Uint32 nowTicks = SDL_GetTicks();
                if (dir != listStickDir) {
                    listStickDir = dir;
                    nextListStickTick = 0;
                }
                if (nextListStickTick == 0 || nowTicks >= nextListStickTick) {
                    moveMenuSelection(dir * step);
                    nextListStickTick = nowTicks + intervalMs;
                }
            }
        } else {
            listStickDir = 0;
            nextListStickTick = 0;
        }

        // Analog-stick scrubbing: hold the left stick left/right to scrub
        // through the track, speed scaled by how far it's pushed.
        bool scrubbing = false;
        if (pad && haveTrack && screen == AppScreen::Player) {
            const Sint16 ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
            const float deadFrac = 8000.0f / 32767.0f;
            float norm = (float)ax / 32767.0f;            // -1..1
            if (norm > deadFrac || norm < -deadFrac) {
                scrubbing = true;
                const float sign = norm < 0 ? -1.0f : 1.0f;
                float mag = (fabsf(norm) - deadFrac) / (1.0f - deadFrac);
                mag = std::min(1.0f, std::max(0.0f, mag));
                // Up to ~8 seconds of audio per real second at ~60 fps.
                const double deltaFrames = sign * mag * 8.0 * player.sampleRate / 60.0;
                long long cur = (long long)player.cursor.load() + (long long)deltaFrames;
                cur = std::max(0LL, std::min(cur, (long long)player.totalFrames - 1));
                if (player.streaming) seekToFraction(player, (double)cur / (double)std::max(1LL, (long long)player.totalFrames - 1));
                else {
                    player.cursor.store((drmp3_uint64)cur);
                    player.finished.store(false);
                }
            }
        }

        if (haveTrack && player.finished.exchange(false)) {
            if (!entries.empty()) {
                const int next = nextQueueIndex(1);
                loadSelected(next, true, false);
            }
        }

        syncRepeatFlag(player, playbackMode, entries.size());
        if (haveTrack && trackIndex >= 0 && trackIndex < (int)entries.size())
            lastFmMaybeScrobble(entries[trackIndex], player, config);

        // ---- Analyse ----
        float bands[NUM_BANDS] = {0};
        float amp = 0.0f;
        if (haveTrack && (player.pcm || player.streaming))
            amp = analyzer.analyze(player, bands);

        // Fast attack / slow decay smoothing, mirroring the desktop build.
        for (int i = 0; i < NUM_BANDS; ++i) {
            const float target = player.playing.load() ? bands[i] : 0.0f;
            const float coeff  = target > smoothBands[i] ? 0.55f : 0.16f;
            smoothBands[i] += (target - smoothBands[i]) * coeff;
        }
        const float targetAmp = player.playing.load() ? amp : 0.0f;
        smoothAmp += (targetAmp - smoothAmp) * (targetAmp > smoothAmp ? 0.45f : 0.12f);
        idlePhase += 0.05f;

        // ---- Draw ----
        if (screen == AppScreen::SourceMenu) {
            clampSource();
            drawSourceMenu(ren, fontBig, fontSmall, sourceItems, sourceIndex, configPath, idlePhase, haveTrack);
        } else if (screen == AppScreen::Browser) {
            clampBrowser();
            drawBrowser(ren, fontBig, fontSmall, browserItems, entries, directories, browserPath, activeSource,
                        browserIndex, browserOffset, configPath, playbackMode, visualizer, idlePhase, haveTrack);
        } else if (haveTrack) {
            drawLiquidRetroBackground(ren, idlePhase);
            drawVisualizer(ren, visualizer, player, smoothBands, smoothAmp, idlePhase);

            drawGlassPanel(ren, 30, 26, SCREEN_W - 60, 98, COL_TERTIARY, 124);
            drawTransport(ren, player.playing.load());
            drawTrackStrip(ren, (int)entries.size(), trackIndex);

            std::string title = entries[trackIndex].label;
            const char* fn = entries[trackIndex].isURL ? "STREAM" : fmtName(player.fmt);
            drawText(ren, fontSmall, std::string("AMPINTOSH SWITCH  ·  ") + gSkin.name, 122, 38, COL_MUTED);
            drawText(ren, fontBig, ellipsizeText(fontBig, title, 650), 122, 66, COL_TEXT);

            int chipX = SCREEN_W - 558;
            chipX += drawFormatChip(ren, fontSmall, *fn ? fn : "FILE", chipX, 76, COL_PRIMARY, true) + 8;
            chipX += drawFormatChip(ren, fontSmall, playbackModeName(playbackMode), chipX, 76, COL_SECONDARY, playbackMode != PlaybackMode::RepeatAll) + 8;
            drawFormatChip(ren, fontSmall, visualizerName(visualizer), chipX, 76, COL_TERTIARY, true);

            drawAlbumArtWell(ren, fontBig, fontSmall, coverArt, &entries[trackIndex], player.fmt);

            StreamBufferInfo bufferInfo;
            if (player.streaming) bufferInfo = streamGetBufferInfo(player.stream);
            const bool buffering = player.streaming && bufferInfo.buffering && player.playing.load();

            double frac = player.totalFrames ? (double)player.cursor.load() / (double)player.totalFrames : 0.0;
            drawProgress(ren, frac, scrubbing);
            drawBufferingIndicator(ren, fontSmall, bufferInfo, idlePhase);

            const double elapsed = (double)player.cursor.load() / (double)player.sampleRate;
            const double total   = (double)player.totalFrames  / (double)player.sampleRate;
            drawText(ren, fontSmall, fmtTime(elapsed) + " / " + fmtTime(total),
                     SCREEN_W - 54, SCREEN_H - 136, COL_PRIMARY, 1);
            const char* playState = buffering ? "BUFFERING" : (player.playing.load() ? "PLAYING" : "PAUSED");
            drawText(ren, fontSmall, playState,
                     54, SCREEN_H - 136, buffering ? COL_SECONDARY : (player.playing.load() ? COL_PRIMARY : COL_SECONDARY));

            drawButtonHelp(ren, fontSmall, {
                {"B", "Play/Pause"}, {"A", "Mode"}, {"Y", "Viz"},
                {"X", "Rescan"}, {"ZL/ZR", "Skin"}, {"L/R", "Track"}, {"-", "Library"}, {"+", "Quit"}
            });
        } else {
            drawLiquidRetroBackground(ren, idlePhase);
            drawEmptyState(ren, idlePhase);
            drawGlassPanel(ren, 30, 26, SCREEN_W - 60, 116, COL_TERTIARY, 118);
            drawText(ren, fontBig, "Ampintosh", 54, 52, COL_TEXT);
            drawText(ren, fontSmall, "No tracks found. Edit sdmc:/switch/Ampintosh/ampintosh.ini or put music in sdmc:/ampintosh/.",
                     54, 88, COL_TERTIARY);
            drawText(ren, fontSmall, "Supported core formats: MP3, FLAC, WAV/WAVE, AIFF/AIFC. Use usb:/... paths and press X to rescan.",
                     54, 114, COL_MUTED);
        }

        SDL_RenderPresent(ren);
    }

    // ---- Teardown ----
    lastFmStop();
    unloadPlayer(player);
    clearCoverArt(coverArt);
    if (pad) SDL_GameControllerClose(pad);
    if (fontBig) TTF_CloseFont(fontBig);
    if (fontSmall) TTF_CloseFont(fontSmall);
    IMG_Quit();
    TTF_Quit();
    plExit();
    romfsExit();
#ifdef AMPINTOSH_USB
    if (g_usbReady) usbHsFsExit();
#endif
#ifdef AMPINTOSH_NET
    if (g_netReady) {
        curl_global_cleanup();
        g_netReady = false;
    }
    if (g_socketReady) {
        socketExit();
        g_socketReady = false;
    }
#endif
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
