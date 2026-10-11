#include "audio.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#elif defined(__ANDROID__)
#include <aaudio/AAudio.h>
#include <GLFW/glfw3.h>  // glfwAndroidSystemClick
#endif

namespace {

constexpr float kPi = 3.14159265f;
int gRate = 44100;
std::vector<float> gClips[(int)Sfx::Count];
struct Voice {
    const std::vector<float>* clip;
    size_t pos;
    float gain;
};
std::mutex gMutex;
std::vector<Voice> gVoices;
std::atomic<float> gVolume{0.6f};
std::atomic<bool> gRunning{false};
std::atomic<bool> gToolSounds{false};
bool gInited = false;  // init() was called (automated captures never call it: silent)

// ---- synthesis: every clip is a few lines of maths (a sine with a falling pitch is a "plop", filtered noise is water)
uint32_t gNoise = 0x9E3779B9u;
float noise() {
    gNoise = gNoise * 1664525u + 1013904223u;
    return (float)(gNoise >> 8) / 8388608.0f - 1.0f;
}

template <class F>
void make(Sfx s, float seconds, F&& f) {
    std::vector<float>& c = gClips[(int)s];
    c.resize((size_t)(seconds * gRate));
    float phase = 0;
    for (size_t i = 0; i < c.size(); ++i) {
        const float t = (float)i / gRate, p = t / seconds;
        c[i] = f(t, p, phase);
    }
    // A few milliseconds of fade at both ends: no clicks from a clip starting or stopping mid-wave.
    const size_t edge = std::min(c.size() / 4, (size_t)(0.003f * gRate));
    for (size_t i = 0; i < edge; ++i) {
        const float k = (float)i / edge;
        c[i] *= k;
        c[c.size() - 1 - i] *= k;
    }
}

void synthesise() {
    auto tone = [](float& phase, float hz) {
        phase += 2.0f * kPi * hz / gRate;
        if (phase > 2.0f * kPi) phase -= 2.0f * kPi;
        return std::sin(phase);
    };
    // The key click: a soft, slightly muffled "tk" like a phone's touch sound — a short burst of low-passed noise on
    // a quickly damped body, not a beep.
    {
        float lp = 0, lp2 = 0;
        make(Sfx::Click, 0.028f, [&](float t, float, float& ph) {
            lp += (noise() - lp) * 0.35f;
            lp2 += (lp - lp2) * 0.35f;
            return lp2 * std::exp(-t * 260) * 1.1f + tone(ph, 1150) * std::exp(-t * 320) * 0.22f;
        });
    }
    // The tools: quiet and round, nothing sharp.
    make(Sfx::Insert, 0.09f, [&](float t, float p, float& ph) {  // a bubble: the pitch rises as it closes
        return tone(ph, 380 + 520 * p) * std::sin(kPi * std::min(1.0f, t * 60)) * std::exp(-t * 32) * 0.32f;
    });
    {
        float lp = 0;
        make(Sfx::Boost, 0.2f, [&](float, float p, float& ph) {  // a soft swell: breathy noise and a rising hum
            lp += (noise() - lp) * (0.03f + 0.08f * p);
            return (lp * 0.9f + tone(ph, 220 + 330 * p) * 0.12f) * std::sin(kPi * p) * 0.55f;
        });
    }
    make(Sfx::Remove, 0.07f, [&](float t, float p, float& ph) {  // a low pop
        return tone(ph, 240 - 130 * p) * std::exp(-t * 55) * 0.4f;
    });
    {
        float gate = 1, lp = 0;
        make(Sfx::Shock, 0.11f, [&](float t, float, float&) {  // a short crackle
            if ((int)(t * 1500) % 9 == 0) gate = noise() > 0.3f ? 1.0f : 0.2f;
            lp += (noise() - lp) * 0.5f;
            return lp * gate * std::exp(-t * 30) * 0.3f;
        });
    }
    make(Sfx::Needle, 0.035f, [&](float t, float, float& ph) {  // a tiny tick
        return tone(ph, 1700) * std::exp(-t * 160) * 0.15f + noise() * std::exp(-t * 500) * 0.05f;
    });
    {
        float ph2 = 0;
        make(Sfx::Wall, 0.05f, [&](float t, float, float& ph) {  // a light knock on wood
            return (tone(ph, 420) + 0.4f * tone(ph2, 1100)) * std::exp(-t * 80) * 0.28f;
        });
    }
    {
        float lp = 0, lp2 = 0;
        make(Sfx::Water, 0.28f, [&](float t, float p, float&) {  // a gurgle
            lp += (noise() - lp) * 0.05f;
            lp2 += (lp - lp2) * 0.2f;
            return lp2 * (0.7f + 0.3f * std::sin(t * 2 * kPi * 9)) * std::sin(kPi * p) * 1.4f;
        });
    }
}

// Mixes the playing voices into `out` (mono floats).
void mix(float* out, int frames) {
    std::fill(out, out + frames, 0.0f);
    const float vol = gVolume.load();
    std::lock_guard<std::mutex> lk(gMutex);
    for (Voice& v : gVoices) {
        const size_t n = std::min((size_t)frames, v.clip->size() - v.pos);
        for (size_t i = 0; i < n; ++i) out[i] += (*v.clip)[v.pos + i] * v.gain;
        v.pos += n;
    }
    gVoices.erase(std::remove_if(gVoices.begin(), gVoices.end(), [](const Voice& v) { return v.pos >= v.clip->size(); }), gVoices.end());
    for (int i = 0; i < frames; ++i) out[i] = std::tanh(out[i] * vol);  // soft limit: many voices never clip harshly
}

#if defined(_WIN32)
HWAVEOUT gOut = nullptr;
HANDLE gEvent = nullptr;
std::thread gThread;
constexpr int kBuffers = 3, kFrames = 1024;
int16_t gData[kBuffers][kFrames];
WAVEHDR gHdr[kBuffers];

void pump() {
    float tmp[kFrames];
    while (gRunning) {
        WaitForSingleObject(gEvent, 50);
        for (int b = 0; b < kBuffers; ++b) {
            if (!(gHdr[b].dwFlags & WHDR_DONE)) continue;
            mix(tmp, kFrames);
            for (int i = 0; i < kFrames; ++i) gData[b][i] = (int16_t)std::lround(std::clamp(tmp[i], -1.0f, 1.0f) * 32000.0f);
            gHdr[b].dwFlags &= ~WHDR_DONE;
            waveOutWrite(gOut, &gHdr[b], sizeof(WAVEHDR));
        }
    }
}

bool openDevice() {
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = (DWORD)gRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = (DWORD)gRate * 2;
    gEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (waveOutOpen(&gOut, WAVE_MAPPER, &fmt, (DWORD_PTR)gEvent, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        CloseHandle(gEvent);
        gEvent = nullptr;
        gOut = nullptr;
        return false;
    }
    for (int b = 0; b < kBuffers; ++b) {
        gHdr[b] = {};
        gHdr[b].lpData = (LPSTR)gData[b];
        gHdr[b].dwBufferLength = sizeof(gData[b]);
        waveOutPrepareHeader(gOut, &gHdr[b], sizeof(WAVEHDR));
        gHdr[b].dwFlags |= WHDR_DONE;  // empty: the pump fills it first
    }
    gRunning = true;
    gThread = std::thread(pump);
    SetEvent(gEvent);
    return true;
}

void closeDevice() {
    if (!gOut) return;
    gRunning = false;
    SetEvent(gEvent);
    if (gThread.joinable()) gThread.join();
    waveOutReset(gOut);
    for (int b = 0; b < kBuffers; ++b) waveOutUnprepareHeader(gOut, &gHdr[b], sizeof(WAVEHDR));
    waveOutClose(gOut);
    CloseHandle(gEvent);
    gOut = nullptr;
    gEvent = nullptr;
}
#elif defined(__ANDROID__)
AAudioStream* gStream = nullptr;

aaudio_data_callback_result_t onAudio(AAudioStream*, void*, void* data, int32_t frames) {
    mix(static_cast<float*>(data), frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

bool openDevice() {
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, onAudio, nullptr);
    const bool ok = AAudioStreamBuilder_openStream(b, &gStream) == AAUDIO_OK;
    AAudioStreamBuilder_delete(b);
    if (!ok) return false;
    gRate = AAudioStream_getSampleRate(gStream);  // the device's own rate: the clips are made for it
    gRunning = true;
    return true;
}

bool startDevice() { return gStream && AAudioStream_requestStart(gStream) == AAUDIO_OK; }

void closeDevice() {
    if (!gStream) return;
    gRunning = false;
    AAudioStream_requestStop(gStream);
    AAudioStream_close(gStream);
    gStream = nullptr;
}
#else
bool openDevice() { return false; }
void closeDevice() {}
#endif

}  // namespace

namespace audio {

bool init() {
    gInited = true;
#if defined(__ANDROID__)
    if (!openDevice()) return false;
    synthesise();
    return startDevice();
#else
    synthesise();
    return openDevice();
#endif
}

void shutdown() {
    closeDevice();
    std::lock_guard<std::mutex> lk(gMutex);
    gVoices.clear();
}

void play(Sfx s, float gain) {
    if (!gInited || gVolume.load() <= 0.0f) return;
    if (s != Sfx::Click && !gToolSounds) return;
#if defined(__ANDROID__)
    if (s == Sfx::Click) {  // the phone's own touch sound, as in the original
        glfwAndroidSystemClick(std::clamp(gain * gVolume.load(), 0.0f, 1.0f));
        return;
    }
#endif
    if (!gRunning) return;
    const std::vector<float>& c = gClips[(int)s];
    if (c.empty()) return;
    std::lock_guard<std::mutex> lk(gMutex);
    // The same sound already starting this instant (a stroke repeats it every frame): no pile-up.
    for (const Voice& v : gVoices)
        if (v.clip == &c && v.pos < c.size() / 3) return;
    if (gVoices.size() < 16) gVoices.push_back({&c, 0, gain});
}

void setVolume(float v) { gVolume = std::clamp(v, 0.0f, 1.0f); }
void setToolSounds(bool on) { gToolSounds = on; }

}  // namespace audio
