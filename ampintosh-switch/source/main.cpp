// Ampintosh for Nintendo Switch (homebrew / NRO)
// ------------------------------------------------
// A from-scratch C++ reimagining of the macOS Ampintosh player for the
// devkitPro / libnx + SDL2 toolchain. It keeps the spirit of the original:
// decode audio, run a real FFT over the playing signal, and draw a
// frequency-reactive spectrum.
//
// Audio:   dr_mp3 decodes the whole track to interleaved float PCM up front;
//          an SDL audio callback streams it out and advances a shared cursor.
// Analyse: the render thread reads a window of PCM around the cursor, applies
//          a Hann window + radix-2 FFT, and buckets bins into log-spaced bands.
// Draw:    SDL2 renderer paints smoothed spectrum bars, a play/pause glyph,
//          a track strip, and a progress bar. No font dependency.
//
// Drop .mp3 files in  sdmc:/ampintosh/  (falls back to  sdmc:/music/ ).
// Controls:  (+) quit   (A) play/pause   (L/R) previous/next track

#include <switch.h>
#define SDL_MAIN_HANDLED          // we provide main() ourselves; don't let SDL rename it
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <utility>
#include <vector>

#ifdef AMPINTOSH_USB
#include <usbhsfs.h>          // USB mass storage (opt-in: make USB=1)
#endif
#ifdef AMPINTOSH_NET
#include <curl/curl.h>        // network streams   (opt-in: make NET=1)
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

// Ampintosh palette (0xRRGGBB).
struct RGB { Uint8 r, g, b; };
static constexpr RGB COL_BG       = { 0x0c, 0x11, 0x18 };
static constexpr RGB COL_PANEL    = { 0x10, 0x19, 0x23 };
static constexpr RGB COL_PRIMARY  = { 0xa8, 0xff, 0x5f }; // green
static constexpr RGB COL_SECONDARY= { 0xff, 0xcf, 0x5f }; // amber
static constexpr RGB COL_TERTIARY = { 0x73, 0xff, 0xe1 }; // teal
static constexpr RGB COL_TEXT     = { 0xea, 0xf6, 0xff };

// ---------------------------------------------------------------------------
// Shared player state (audio thread <-> main thread)
// ---------------------------------------------------------------------------
// Supported source formats. Stored on the player so we free with the matching
// decoder's allocator.
enum class AudioFmt { None, Mp3, Flac, Wav };

struct Player {
    float*                    pcm        = nullptr; // interleaved float, owned
    drmp3_uint64              totalFrames = 0;
    int                       channels    = 2;
    int                       sampleRate  = 44100;
    AudioFmt                  fmt         = AudioFmt::None;
    SDL_AudioDeviceID         dev         = 0;       // reopened to match each track
    std::atomic<drmp3_uint64> cursor{0};            // current playback frame
    std::atomic<bool>         playing{true};
    std::atomic<bool>         loop{true};
};

// Pull PCM into SDL's buffer and advance the shared cursor. Runs on the audio
// thread; pcm is immutable for the lifetime of a loaded track (we lock the
// device around track changes), and cursor is atomic, so no extra mutex.
static void audioCallback(void* userdata, Uint8* stream, int len) {
    Player* p = static_cast<Player*>(userdata);
    float* out = reinterpret_cast<float*>(stream);
    const int ch = p->channels;
    const int outFrames = len / (int)(sizeof(float) * ch);

    if (!p->pcm || p->totalFrames == 0 || !p->playing.load()) {
        std::memset(stream, 0, len);
        return;
    }

    drmp3_uint64 cur = p->cursor.load();
    for (int i = 0; i < outFrames; ++i) {
        if (cur >= p->totalFrames) {
            if (p->loop.load()) {
                cur = 0;
            } else {
                for (int c = 0; c < ch; ++c) out[i * ch + c] = 0.0f;
                continue;
            }
        }
        for (int c = 0; c < ch; ++c)
            out[i * ch + c] = p->pcm[cur * ch + c];
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
            drmp3_uint64 f = start + i;
            float s = 0.0f;
            if (p.pcm && f < p.totalFrames) {
                if (ch >= 2) s = 0.5f * (p.pcm[f * ch] + p.pcm[f * ch + 1]);
                else          s = p.pcm[f * ch];
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
// Track discovery + loading
// ---------------------------------------------------------------------------
// Map a filename's extension to a decoder. Returns None for unsupported types.
static AudioFmt fmtForName(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return AudioFmt::None;
    std::string ext = name.substr(dot + 1);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    if (ext == "mp3")  return AudioFmt::Mp3;
    if (ext == "flac") return AudioFmt::Flac;
    if (ext == "wav")  return AudioFmt::Wav;
    return AudioFmt::None;
}

// Filename/URL tail without directory or extension, for the now-playing label.
static std::string displayName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t q = base.find('?');                 // drop URL query strings
    if (q != std::string::npos) base = base.substr(0, q);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    return base;
}

// A playable item: either a local file path or a network URL.
struct Entry {
    std::string ref;     // file path ("sdmc:/.../x.mp3", "ums0:/.../y.flac") or URL
    std::string label;   // display name
    bool        isURL = false;
};

// Recursively collect audio files under `dir` into `out`. Depth- and count-
// capped so a huge library or a pathological tree can't stall startup.
static void scanDir(const std::string& dir, std::vector<std::string>& out, int depth) {
    if (depth > 8 || out.size() >= 4096) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        const std::string name = e->d_name;
        if (name == "." || name == ".." || (!name.empty() && name[0] == '.')) continue;
        const std::string full = dir + "/" + name;

        bool isDir;
        if (e->d_type == DT_DIR)       isDir = true;
        else if (e->d_type == DT_REG)  isDir = false;
        else {                          // DT_UNKNOWN — ask the filesystem
            struct stat st;
            if (stat(full.c_str(), &st) != 0) continue;
            isDir = S_ISDIR(st.st_mode);
        }

        if (isDir)                                 scanDir(full, out, depth + 1);
        else if (fmtForName(name) != AudioFmt::None) out.push_back(full);
        if (out.size() >= 4096) break;
    }
    closedir(d);
}

// All directories to search. The SD card always; USB mass-storage mounts too
// when built with USB=1 and a drive is attached.
static std::vector<std::string> audioRoots() {
    std::vector<std::string> roots = { "sdmc:/ampintosh", "sdmc:/music" };
#ifdef AMPINTOSH_USB
    const u32 n = usbHsFsGetMountedDeviceCount();
    if (n > 0) {
        std::vector<UsbHsFsDevice> devs(n);
        const u32 got = usbHsFsListMountedDevices(devs.data(), n);
        for (u32 i = 0; i < got; ++i) {
            const std::string m = devs[i].name;           // e.g. "ums0:"
            roots.push_back(m + "/ampintosh");
            roots.push_back(m + "/music");
            roots.push_back(m + "/Music");
        }
    }
#endif
    return roots;
}

static std::vector<std::string> scanFiles(const std::vector<std::string>& roots) {
    std::vector<std::string> out;
    for (const auto& r : roots) scanDir(r, out, 0);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

#ifdef AMPINTOSH_NET
// Read stream URLs (one per line, '#' comments) from a playlist on the SD card.
static std::vector<std::string> readStreamURLs() {
    std::vector<std::string> urls;
    for (const char* p : { "sdmc:/ampintosh/streams.txt", "sdmc:/music/streams.txt" }) {
        FILE* f = fopen(p, "r");
        if (!f) continue;
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
                s.pop_back();
            const size_t a = s.find_first_not_of(" \t");
            if (a == std::string::npos) continue;
            s = s.substr(a);
            if (s.empty() || s[0] == '#') continue;
            urls.push_back(s);
        }
        fclose(f);
    }
    return urls;
}

static size_t curlToFile(void* ptr, size_t sz, size_t nmemb, void* f) {
    return fwrite(ptr, sz, nmemb, static_cast<FILE*>(f));
}

// Download a URL to a local file. Cert verification is off — this is a homebrew
// toy, not a security boundary, and it avoids shipping a CA bundle.
static bool downloadURL(const std::string& url, const std::string& dest) {
    FILE* f = fopen(dest.c_str(), "wb");
    if (!f) return false;
    CURL* c = curl_easy_init();
    if (!c) { fclose(f); return false; }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlToFile);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Ampintosh/1.0");
    const CURLcode res = curl_easy_perform(c);
    curl_easy_cleanup(c);
    fclose(f);
    return res == CURLE_OK;
}
#endif // AMPINTOSH_NET

// Build the full library: local files (SD + USB), then any network streams.
static std::vector<Entry> buildLibrary() {
    std::vector<Entry> out;
    for (const auto& f : scanFiles(audioRoots()))
        out.push_back({ f, displayName(f), false });
#ifdef AMPINTOSH_NET
    for (const auto& u : readStreamURLs()) {
        std::string label = displayName(u);
        if (label.empty()) label = u;
        out.push_back({ u, label, true });
    }
#endif
    return out;
}

// Decode any supported file to interleaved float PCM. Channels/rate/frames are
// returned via out-params. Caller frees with freeDecoded() using the same fmt.
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
        default: return nullptr;
    }
}

static void freeDecoded(float* p, AudioFmt fmt) {
    if (!p) return;
    switch (fmt) {
        case AudioFmt::Mp3:  drmp3_free(p, nullptr);  break;
        case AudioFmt::Flac: drflac_free(p, nullptr); break;
        case AudioFmt::Wav:  drwav_free(p, nullptr);  break;
        default: break;
    }
}

// Decode a file fully, then (re)open the audio device to match the track's own
// sample rate and channel count, so mono / 48 kHz / etc. all play correctly.
// Closing the device before swapping the buffer guarantees the callback isn't
// running while we free the old PCM. Returns false on failure.
static bool loadTrack(Player& p, Analyzer& analyzer, const std::string& path) {
    const AudioFmt fmt = fmtForName(path);
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

    p.pcm         = data;
    p.totalFrames = frames;
    p.channels    = ch;
    p.sampleRate  = sr;
    p.fmt         = fmt;
    p.cursor.store(0);
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

    SDL_PauseAudioDevice(p.dev, 0); // start playback
    p.playing.store(true);
    return true;
}

// Jump playback to a fraction (0..1) of the track. Safe to call from the main
// thread: cursor is atomic and the audio callback re-reads it each buffer.
static void seekToFraction(Player& p, double frac) {
    if (p.totalFrames == 0) return;
    frac = std::min(1.0, std::max(0.0, frac));
    p.cursor.store((drmp3_uint64)(frac * (double)(p.totalFrames - 1)));
}

// Load a library entry. Local files go straight to loadTrack(); network entries
// are downloaded to a temp file first, then loaded through the same path (so
// the existing decode-to-RAM pipeline is reused unchanged).
static bool loadEntry(Player& p, Analyzer& analyzer, const Entry& e) {
#ifdef AMPINTOSH_NET
    if (e.isURL) {
        AudioFmt f = fmtForName(e.ref);
        if (f == AudioFmt::None) f = AudioFmt::Mp3;     // assume MP3 if URL has no extension
        const char* ext = (f == AudioFmt::Flac) ? "flac" : (f == AudioFmt::Wav) ? "wav" : "mp3";
        const std::string tmp = std::string("sdmc:/ampintosh/.netcache.") + ext;
        if (!downloadURL(e.ref, tmp)) return false;
        return loadTrack(p, analyzer, tmp);
    }
#endif
    return loadTrack(p, analyzer, e.ref);
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
static const char* fmtName(AudioFmt f) {
    switch (f) {
        case AudioFmt::Mp3:  return "MP3";
        case AudioFmt::Flac: return "FLAC";
        case AudioFmt::Wav:  return "WAV";
        default: return "";
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
    SDL_Rect dst{ align == 1 ? x - w : x, y, w, h };
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

static void drawSpectrum(SDL_Renderer* r, const float* bands, float amp) {
    const int marginX = 40;
    const int baseY   = SCREEN_H - 90;
    const int top     = 120;
    const int usableH = baseY - top;
    const float gap   = 6.0f;
    const float bw    = (SCREEN_W - 2 * marginX - gap * (NUM_BANDS - 1)) / NUM_BANDS;
    const float glow  = 0.5f + amp * 0.5f;

    for (int i = 0; i < NUM_BANDS; ++i) {
        const float level = bands[i];
        const int   h = (int)std::max(4.0f, usableH * level);
        const int   x = (int)(marginX + i * (bw + gap));
        // Lerp green -> amber across the band index, brighten with level.
        const float t = (float)i / (NUM_BANDS - 1);
        RGB c;
        c.r = (Uint8)std::min(255.0f, (COL_PRIMARY.r + (COL_SECONDARY.r - COL_PRIMARY.r) * t) * (0.6f + 0.4f * level));
        c.g = (Uint8)std::min(255.0f, (COL_PRIMARY.g + (COL_SECONDARY.g - COL_PRIMARY.g) * t) * (0.6f + 0.4f * level));
        c.b = (Uint8)std::min(255.0f, (COL_PRIMARY.b + (COL_SECONDARY.b - COL_PRIMARY.b) * t) * (0.6f + 0.4f * level));
        setColor(r, c, (Uint8)(std::min(1.0f, (0.45f + level * 0.5f) * glow) * 255));
        fillRect(r, x, baseY - h, (int)bw, h);
        // Peak cap.
        setColor(r, COL_TEXT, (Uint8)(std::min(1.0f, 0.35f + amp * 0.4f) * 255));
        fillRect(r, x, baseY - h - 4, (int)bw, 2);
    }
}

// Play/pause glyph drawn from rectangles (no font needed).
static void drawTransport(SDL_Renderer* r, bool playing) {
    const int x = 40, y = 40, s = 34;
    setColor(r, COL_TERTIARY);
    if (playing) {
        fillRect(r, x, y, s / 3, s);
        fillRect(r, x + (2 * s) / 3, y, s / 3, s);
    } else {
        // Simple right-pointing triangle approximated by shrinking bars.
        for (int i = 0; i < s; ++i) {
            const int barH = s - 2 * std::abs(i - s / 2);
            if (barH > 0) fillRect(r, x + i, y + (s - barH) / 2, 1, barH);
        }
    }
}

static void drawTrackStrip(SDL_Renderer* r, int count, int current) {
    if (count <= 0) return;
    const int y = 44, x0 = 110, dotW = 16, dotH = 24, gap = 8;
    for (int i = 0; i < count && i < 24; ++i) {
        RGB c = (i == current) ? COL_PRIMARY : COL_PANEL;
        setColor(r, c, (i == current) ? 255 : 180);
        fillRect(r, x0 + i * (dotW + gap), y, dotW, dotH);
    }
}

static void drawProgress(SDL_Renderer* r, double frac, bool active) {
    const int x = 40, y = SCREEN_H - 50, w = SCREEN_W - 80, h = 8;
    frac = std::min(1.0, std::max(0.0, frac));
    setColor(r, COL_PANEL, 200);
    fillRect(r, x, y, w, h);
    setColor(r, active ? COL_SECONDARY : COL_PRIMARY);
    fillRect(r, x, y, (int)(w * frac), h);
    // Playhead handle (brighter while scrubbing).
    const int px = x + (int)(w * frac);
    setColor(r, COL_TEXT);
    fillRect(r, px - 2, y - 5, 4, h + 10);
}

// Dim idle bars + a hint marker when no tracks were found.
static void drawEmptyState(SDL_Renderer* r, float phase) {
    const int marginX = 40, baseY = SCREEN_H - 90;
    const float gap = 6.0f;
    const float bw = (SCREEN_W - 2 * marginX - gap * (NUM_BANDS - 1)) / NUM_BANDS;
    for (int i = 0; i < NUM_BANDS; ++i) {
        const float wob = 0.5f + 0.5f * sinf(phase + i * 0.3f);
        const int h = (int)(20 + 40 * wob);
        const int x = (int)(marginX + i * (bw + gap));
        setColor(r, COL_PANEL, 200);
        fillRect(r, x, baseY - h, (int)bw, h);
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    SDL_SetMainReady();   // required when SDL_MAIN_HANDLED is defined

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
        return 1;

#ifdef AMPINTOSH_NET
    socketInitializeDefault();
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
#ifdef AMPINTOSH_USB
    usbHsFsInitialize(0);   // mounts attached USB mass-storage in the background
#endif

    SDL_Window* win = SDL_CreateWindow("Ampintosh", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, SCREEN_W, SCREEN_H, 0);
    if (!win) { SDL_Quit(); return 1; }

    SDL_Renderer* ren = SDL_CreateRenderer(win, -1,
                            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) { SDL_DestroyWindow(win); SDL_Quit(); return 1; }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    // RomFS is packed into the .nro; open the bundled font from it. Both are
    // optional — if anything here fails the app still runs, just without text.
    romfsInit();
    TTF_Font* fontBig = nullptr;
    TTF_Font* fontSmall = nullptr;
    if (TTF_Init() == 0) {
        fontBig   = TTF_OpenFont("romfs:/font.ttf", 26);
        fontSmall = TTF_OpenFont("romfs:/font.ttf", 20);
    }

    // The combined "Player 1" handheld/controller is index 0.
    SDL_GameController* pad = nullptr;
    if (SDL_NumJoysticks() > 0 && SDL_IsGameController(0))
        pad = SDL_GameControllerOpen(0);

    Player   player;
    Analyzer analyzer;
    analyzer.init(player.sampleRate);

    // The audio device is opened per track inside loadTrack() to match each
    // file's sample rate / channel count.
    std::vector<Entry> entries = buildLibrary();   // SD (recursive) + USB + streams
    int trackIndex = 0;
    bool haveTrack = false;
    if (!entries.empty())
        haveTrack = loadEntry(player, analyzer, entries[trackIndex]);

    float smoothBands[NUM_BANDS] = {0};
    float smoothAmp = 0.0f;
    float idlePhase = 0.0f;
    bool  running = true;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;

            if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (ev.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_START:        // (+)
                        running = false;
                        break;
                    case SDL_CONTROLLER_BUTTON_A:            // play / pause
                        if (haveTrack)
                            player.playing.store(!player.playing.load());
                        break;
                    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: // previous (L)
                        if (haveTrack && entries.size() > 1) {
                            trackIndex = (trackIndex - 1 + (int)entries.size()) % (int)entries.size();
                            loadEntry(player, analyzer, entries[trackIndex]);
                        }
                        break;
                    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:// next (R)
                        if (haveTrack && entries.size() > 1) {
                            trackIndex = (trackIndex + 1) % (int)entries.size();
                            loadEntry(player, analyzer, entries[trackIndex]);
                        }
                        break;
                    case SDL_CONTROLLER_BUTTON_Y:            // rescan library
                        entries = buildLibrary();
                        trackIndex = 0;
                        haveTrack = !entries.empty() && loadEntry(player, analyzer, entries[0]);
                        break;
                    default: break;
                }
            }

            // Touchscreen: tap or drag along the lower part of the screen to
            // seek. tfinger coords are normalized 0..1.
            if (ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERMOTION) {
                if (haveTrack && ev.tfinger.y > 0.80f)
                    seekToFraction(player, ev.tfinger.x);
            }
        }

        // Analog-stick scrubbing: hold the left stick left/right to scrub
        // through the track, speed scaled by how far it's pushed.
        bool scrubbing = false;
        if (pad && haveTrack) {
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
                player.cursor.store((drmp3_uint64)cur);
            }
        }

        // ---- Analyse ----
        float bands[NUM_BANDS] = {0};
        float amp = 0.0f;
        if (haveTrack && player.playing.load())
            amp = analyzer.analyze(player, bands);

        // Fast attack / slow decay smoothing, mirroring the desktop build.
        for (int i = 0; i < NUM_BANDS; ++i) {
            const float target = bands[i];
            const float coeff  = target > smoothBands[i] ? 0.55f : 0.16f;
            smoothBands[i] += (target - smoothBands[i]) * coeff;
        }
        smoothAmp += (amp - smoothAmp) * (amp > smoothAmp ? 0.45f : 0.12f);
        idlePhase += 0.05f;

        // ---- Draw ----
        setColor(ren, COL_BG);
        SDL_RenderClear(ren);

        if (haveTrack) {
            drawSpectrum(ren, smoothBands, smoothAmp);
            drawTransport(ren, player.playing.load());
            drawTrackStrip(ren, (int)entries.size(), trackIndex);
            double frac = player.totalFrames ? (double)player.cursor.load() / (double)player.totalFrames : 0.0;
            drawProgress(ren, frac, scrubbing);

            // Now-playing label: "title   ·   FORMAT" (or STREAM for network).
            std::string title = entries[trackIndex].label;
            const char* fn = entries[trackIndex].isURL ? "STREAM" : fmtName(player.fmt);
            if (*fn) title += "   \xC2\xB7   " + std::string(fn); // UTF-8 middle dot
            drawText(ren, fontBig, title, 40, 80, COL_TEXT);

            // Elapsed / total, right-aligned above the progress bar.
            const double elapsed = (double)player.cursor.load() / (double)player.sampleRate;
            const double total   = (double)player.totalFrames  / (double)player.sampleRate;
            drawText(ren, fontSmall, fmtTime(elapsed) + " / " + fmtTime(total),
                     SCREEN_W - 40, SCREEN_H - 84, COL_TERTIARY, 1);
        } else {
            drawEmptyState(ren, idlePhase);
            drawText(ren, fontBig, "Ampintosh", 40, 60, COL_TEXT);
            drawText(ren, fontSmall, "No tracks found - put music (incl. subfolders) in sdmc:/ampintosh/",
                     40, 96, COL_TERTIARY);
            drawText(ren, fontSmall, "Press Y to rescan.", 40, 122, COL_TERTIARY);
        }

        SDL_RenderPresent(ren);
    }

    // ---- Teardown ----
    if (player.dev) {
        SDL_PauseAudioDevice(player.dev, 1);
        SDL_CloseAudioDevice(player.dev);
    }
    if (player.pcm) freeDecoded(player.pcm, player.fmt);
    if (pad) SDL_GameControllerClose(pad);
    if (fontBig) TTF_CloseFont(fontBig);
    if (fontSmall) TTF_CloseFont(fontSmall);
    TTF_Quit();
    romfsExit();
#ifdef AMPINTOSH_USB
    usbHsFsExit();
#endif
#ifdef AMPINTOSH_NET
    curl_global_cleanup();
    socketExitDefault();
#endif
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
