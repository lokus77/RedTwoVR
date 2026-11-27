// Host audio output: the AI DMA blocks the game plays (16-bit stereo, 32 or 48 kHz) go to an
// SDL3 audio stream. R2_NO_AUDIO=1 opens no device (automated runs); R2_AUDIO_DUMP=<file>
// also writes everything to a WAV file; R2_AUDIO_LOG=1 logs the queue once a second.
//
// Buffering: the game hands over a 5 ms block whenever the guest's clock says one is due (onAi,
// at the guest thread's safepoints), the device plays on its own clock. Started with nothing in
// hand, the queue sat at about one device period (5-20 ms, now and then 0.2 ms), so a block a
// few ms late left the device empty: crackling. Playback now starts (and starts again after the
// device ran dry: a load, a skipped cinematic, a state load) only once kTargetMs is queued, and
// the two clocks' drift is taken up by the stream's playback rate (at most 0.5% off, inaudible)
// rather than by dropping blocks, which clicked. R2_AUDIO_MS=<ms> sets the cushion.
//
// The output can be lost behind the game's back: the user's sound stopped in a session (the game
// still mixed: a state saved then played loud in a new process). The stream follows Windows'
// default device itself (SDL migrates it), but is opened again when its device is removed
// (deviceEvent) or stops taking sound - full for a second while playing; device changes are
// logged, and every perf line says what the game played (sound: blocks, peak, gaps, drops).
//
// Speakers (setting speakers): stereo plays the game's own mix - in its Pro Logic II mode
// matrix-encoded for a receiver's decoder, as the GameCube's output was. 5.1 and 7.1 play the
// surround discretely: the DSP keeps the rear and the surround channel it folds into the stereo
// (musyx_dsp.cpp, musyxDiscreteBlock) and the output takes them back out onto speakers of their
// own (front L/R; center and LFE silent: the game pans between L and R; 7.1 splits the rear into
// side and back pairs). Automatic picks from the default device's channels (the system's
// speaker setup). Blocks the DSP did not write play on the front pair.
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "hw/musyx_dsp.h"
#include "runtime.h"
#include "settings.h"

namespace rt::audio {

namespace {
SDL_AudioStream* s_stream = nullptr;
u32 s_rate = 0;
float s_gain = 1.f;
int s_channels = 2;  // the stream's: 2, 6 (5.1) or 8 (7.1)
int s_speakers = 0;  // settings::Speakers
bool s_sdlAudio = false;
FILE* s_dump = nullptr;
u32 s_dumpBytes = 0, s_dumpRate = 0;
int s_dumpChannels = 0;

double s_targetMs = 50.0;     // the cushion playback keeps
double kMaxMs = 150.0;  // past this blocks are dropped (only after a burst): the cushion + 100 ms
constexpr double kMaxRatioOff = 0.005;
bool s_playing = false;
double s_avgMs = 0.0;  // queue depth, smoothed: the device takes it in chunks of its period
float s_ratio = 1.f;
u64 s_underruns = 0, s_dropped = 0;
u64 s_fullSinceTb = 0;  // the queue has been past kMaxMs since then (0: it is not)
// for the perf lines (perfText): since the last one
std::atomic<u64> s_perfBlocks{0}, s_perfGaps{0}, s_perfDropped{0}, s_perfReopened{0};
std::atomic<int> s_perfPeak{0};

void writeWavHeader() {
    auto u32le = [](u8* p, u32 v) { p[0] = u8(v); p[1] = u8(v >> 8); p[2] = u8(v >> 16); p[3] = u8(v >> 24); };
    u8 h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    u32le(h + 4, 36 + s_dumpBytes);
    h[22] = u8(s_dumpChannels);
    u32le(h + 24, s_dumpRate);
    u32le(h + 28, s_dumpRate * 2 * u32(s_dumpChannels));
    h[32] = u8(2 * s_dumpChannels);
    u32le(h + 40, s_dumpBytes);
    fseek(s_dump, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, s_dump);
    fseek(s_dump, 0, SEEK_END);
}

void setRatio(float r) {
    if (std::fabs(r - s_ratio) < 0.0002f && r != 1.f) return;
    if (r == s_ratio) return;
    s_ratio = r;
    SDL_SetAudioStreamFrequencyRatio(s_stream, r);
}

void stopPlaying() {
    if (!s_playing) return;
    SDL_PauseAudioStreamDevice(s_stream);
    s_playing = false;
    setRatio(1.f);
}

std::string deviceName(SDL_AudioDeviceID id) {
    const char* n = id ? SDL_GetAudioDeviceName(id) : nullptr;
    return n ? n : "?";
}

// the default playback device's channels (the system's speaker setup), 0 when unknown
int deviceChannels() {
    if (!s_sdlAudio) return 0;
    SDL_AudioSpec spec{};
    if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr)) return 0;
    return spec.channels;
}

// the output's channels for a speakers setting
int channelsFor(int speakers) {
    switch (settings::Speakers(speakers)) {
    case settings::Speakers::Stereo: return 2;
    case settings::Speakers::Surround51: return 6;
    case settings::Speakers::Surround71: return 8;
    default: {
        const int n = deviceChannels();
        return n >= 8 ? 8 : n >= 6 ? 6 : 2;
    }
    }
}

const char* layoutName(int channels) { return channels == 8 ? "7.1" : channels == 6 ? "5.1" : "stereo"; }

// opened paused: playback starts once the cushion is queued (push)
bool openStream() {
    s_channels = channelsFor(s_speakers);
    const SDL_AudioSpec spec{SDL_AUDIO_S16, s_channels, int(s_rate)};
    s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s_stream) {
        logf("audio", "no audio device: %s", SDL_GetError());
        return false;
    }
    SDL_SetAudioStreamGain(s_stream, s_gain);
    s_playing = false;
    s_ratio = 1.f;
    s_fullSinceTb = 0;
    logf("audio", "output: %u Hz %s, %.0f ms buffered, on %s (%d channels)", s_rate, layoutName(s_channels),
         s_targetMs, deviceName(SDL_GetAudioStreamDevice(s_stream)).c_str(), deviceChannels());
    return true;
}

void reopen(const char* why, bool trouble = true) {
    logf("audio", "output %s: opening the default device again", why);
    if (trouble) s_perfReopened++;
    if (s_stream) SDL_DestroyAudioStream(s_stream);
    s_stream = nullptr;
    openStream();
}

// the speakers setting or the device's channels changed what the output should be
void checkLayout(const char* why) {
    if (s_stream && channelsFor(s_speakers) != s_channels) reopen(why, false);
}

// R2_AUDIO_LOG=1: once a second
void logQueue(double queuedMs) {
    static const bool log = envFlag("R2_AUDIO_LOG");
    if (!log) return;
    static u64 lastTb = 0, pushes = 0, lastUnder = 0, lastDrop = 0;
    static double minMs = 1e9, maxMs = 0;
    pushes++;
    minMs = std::min(minMs, queuedMs);
    maxMs = std::max(maxMs, queuedMs);
    const u64 now = tbNow();
    if (now - lastTb < kTbClock) return;
    logf("audio", "%llu blocks/s, queue %.1f-%.1f ms (avg %.1f), ratio %.4f, underruns %llu, dropped %llu",
         (unsigned long long)pushes, minMs, maxMs, s_avgMs, s_ratio, (unsigned long long)(s_underruns - lastUnder),
         (unsigned long long)(s_dropped - lastDrop));
    lastTb = now;
    pushes = 0;
    lastUnder = s_underruns;
    lastDrop = s_dropped;
    minMs = 1e9;
    maxMs = 0;
}
}  // namespace

void init() {
    if (const char* d = getenv("R2_AUDIO_DUMP")) {
        s_dump = fopen(d, "wb");
        if (s_dump) {
            logf("audio", "writing %s", d);
            const u8 room[44] = {};  // the header's place (written with each block; it used to land on the first samples)
            fwrite(room, 1, sizeof room, s_dump);
        }
    }
    setBuffer(settings::get().audioBuffer);
    if (const char* ms = getenv("R2_AUDIO_MS")) {
        s_targetMs = std::clamp(atof(ms), 10.0, 250.0);
        kMaxMs = s_targetMs + 100.0;
    }
    s_speakers = int(settings::get().speakers);
    if (envFlag("R2_NO_AUDIO")) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        logf("audio", "no audio: %s", SDL_GetError());
        return;
    }
    s_sdlAudio = true;
    s_rate = 32000;
    openStream();
}

void setBuffer(int level) {
    static constexpr double kMs[] = {50.0, 100.0, 160.0};
    const double ms = kMs[std::clamp(level, 0, 2)];
    if (ms == s_targetMs && kMaxMs == ms + 100.0) return;
    s_targetMs = ms;
    kMaxMs = ms + 100.0;
    if (s_stream) logf("audio", "buffer %.0f ms", ms);  // (taken up as the queue drifts to it)
}

void setSpeakers(int speakers) {
    if (speakers == s_speakers) return;
    s_speakers = speakers;
    checkLayout("speakers changed");
}

int outputChannels() { return s_stream ? s_channels : channelsFor(s_speakers); }

// every SDL event (gx_bridge pollEvents): playback devices coming and going
void deviceEvent(const void* p) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(p);
    if (e.type != SDL_EVENT_AUDIO_DEVICE_ADDED && e.type != SDL_EVENT_AUDIO_DEVICE_REMOVED &&
        e.type != SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED)
        return;
    if (e.adevice.recording) return;
    const SDL_AudioDeviceID ours = s_stream ? SDL_GetAudioStreamDevice(s_stream) : 0;
    const char* what = e.type == SDL_EVENT_AUDIO_DEVICE_ADDED ? "added" : e.type == SDL_EVENT_AUDIO_DEVICE_REMOVED ? "removed" : "format changed";
    logf("audio", "playback device %u %s: %s%s", unsigned(e.adevice.which), what,
         e.type == SDL_EVENT_AUDIO_DEVICE_REMOVED ? "" : deviceName(e.adevice.which).c_str(),
         ours && e.adevice.which == ours ? " (the game's output)" : "");
    if (s_stream && e.type == SDL_EVENT_AUDIO_DEVICE_REMOVED && e.adevice.which == ours) reopen("device removed");
    else if (settings::Speakers(s_speakers) == settings::Speakers::Auto) checkLayout("speaker setup changed");
    // no output so far (there was no device), and now there is one
    static const bool none = envFlag("R2_NO_AUDIO");
    if (!s_stream && !none && s_rate && e.type == SDL_EVENT_AUDIO_DEVICE_ADDED) openStream();
}

std::string perfText() {
    const u64 blocks = s_perfBlocks.exchange(0), gaps = s_perfGaps.exchange(0), dropped = s_perfDropped.exchange(0),
              reopened = s_perfReopened.exchange(0);
    const int peak = s_perfPeak.exchange(0);
    char t[160];
    snprintf(t, sizeof t, "sound %llu blocks, peak %d%%, %llu gaps, %llu dropped%s", (unsigned long long)blocks,
             peak * 100 / 32768, (unsigned long long)gaps, (unsigned long long)dropped,
             reopened ? ", output reopened" : "");
    return t;
}

void setGain(float gain) {
    if (gain == s_gain) return;
    s_gain = gain;
    if (s_stream) SDL_SetAudioStreamGain(s_stream, gain);
}

// one block as `channels` interleaved channels (SDL's order: FL FR FC LFE BL BR SL SR); the
// discrete channels from the DSP when it wrote the block, else the stereo on the front pair
void compose(const u8* data, u32 frames, int channels, const s16* discrete, std::vector<s16>& out) {
    out.assign(size_t(frames) * size_t(channels), 0);
    for (u32 i = 0; i < frames; i++) {
        s16* o = &out[size_t(i) * size_t(channels)];
        const s16* d = discrete ? discrete + i * 8 : nullptr;
        o[0] = d ? d[0] : s16(data[4 * i] << 8 | data[4 * i + 1]);
        o[1] = d ? d[1] : s16(data[4 * i + 2] << 8 | data[4 * i + 3]);
        if (!d || channels == 2) continue;
        if (channels == 6) {
            o[4] = d[2];
            o[5] = d[3];
        } else {
            o[4] = d[4];
            o[5] = d[5];
            o[6] = d[6];
            o[7] = d[7];
        }
    }
}

void push(const u8* data, u32 frames, u32 rate) {
    int peak = 0;
    for (u32 i = 0; i < frames * 2; i++) peak = std::max(peak, std::abs(int(s16(data[2 * i] << 8 | data[2 * i + 1]))));
    s_perfBlocks++;
    if (peak > s_perfPeak.load(std::memory_order_relaxed)) s_perfPeak.store(peak, std::memory_order_relaxed);
    if (s_dump && !s_dumpChannels) s_dumpChannels = outputChannels();
    const s16* discrete =
        (s_stream && s_channels > 2) || s_dumpChannels > 2 ? musyxDiscreteBlock(data, frames) : nullptr;
    static std::vector<s16> buf;
    if (s_dump) {
        if (!s_dumpRate) s_dumpRate = rate;
        compose(data, frames, s_dumpChannels, discrete, buf);
        fwrite(buf.data(), 2, buf.size(), s_dump);
        s_dumpBytes += u32(buf.size() * 2);
        writeWavHeader();
    }
    if (!s_stream) return;
    compose(data, frames, s_channels, discrete, buf);
    const int bytes = int(buf.size() * 2);
    if (rate != s_rate) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, s_channels, int(rate)};
        SDL_SetAudioStreamFormat(s_stream, &spec, nullptr);
        s_rate = rate;
    }
    // R2_AUDIO_STALL_TEST=<seconds>: the device stops taking sound then (paused behind push's
    // back), as a lost output does; the output must be opened again a second later
    static const double stallAt = getenv("R2_AUDIO_STALL_TEST") ? atof(getenv("R2_AUDIO_STALL_TEST")) : -1.0;
    if (stallAt >= 0.0 && s_playing) {
        static const u64 t0 = tbNow();
        static bool stalled = false;
        if (!stalled && double(tbNow() - t0) / double(kTbClock) >= stallAt) {
            stalled = true;
            logf("audio", "stall test: output paused behind the game's back");
            SDL_PauseAudioStreamDevice(s_stream);
        }
    }
    const double bytesPerMs = s_rate * 2.0 * s_channels / 1000.0;
    const double queuedMs = SDL_GetAudioStreamQueued(s_stream) / bytesPerMs;
    logQueue(queuedMs);
    // Ran dry (a dry stream still reports ~0.2 ms: the resampler's leftovers): one clean gap
    // while the cushion refills, instead of playing on with nothing in hand.
    if (s_playing && queuedMs < 2.0) {
        s_underruns++;
        s_perfGaps++;
        stopPlaying();
    }
    if (queuedMs > kMaxMs) {
        s_dropped++;
        s_perfDropped++;
        // the device takes nothing (a burst after a stall drains in a few blocks)
        const u64 now = tbNow();
        if (!s_fullSinceTb) s_fullSinceTb = now;
        else if (now - s_fullSinceTb > kTbClock) reopen("stopped taking sound for a second");
        return;
    }
    s_fullSinceTb = 0;
    SDL_PutAudioStreamData(s_stream, buf.data(), bytes);
    const double nowMs = queuedMs + bytes / bytesPerMs;
    if (!s_playing) {
        if (nowMs >= s_targetMs) {
            s_avgMs = s_targetMs;
            if (!SDL_ResumeAudioStreamDevice(s_stream)) {
                static int logs = 0;
                if (logs++ < 5) logf("audio", "cannot start the output: %s", SDL_GetError());
            }
            s_playing = true;
        }
        return;
    }
    // drift: play a little faster while above the cushion, a little slower below it
    s_avgMs += (nowMs - s_avgMs) * 0.01;
    const double off = std::clamp((s_avgMs - s_targetMs) / s_targetMs, -1.0, 1.0) * kMaxRatioOff;
    setRatio(float(1.0 + off));
}

// a state was loaded: what is queued belongs to the other timeline
void flush() {
    if (!s_stream) return;
    SDL_ClearAudioStream(s_stream);
    stopPlaying();
}

}  // namespace rt::audio
