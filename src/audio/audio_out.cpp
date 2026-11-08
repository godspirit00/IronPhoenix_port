// WASAPI audio output implementing the small SDL3 stream subset used by the APU emulation.
// 48 kHz, 16-bit stereo PCM is pushed by the APU frame thread; a render thread drains it.
#include "../common.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <SDL3/SDL.h>
#include <vector>
#include <stdio.h>

struct SDL_AudioStream {
    IAudioClient *client = nullptr;
    IAudioRenderClient *render = nullptr;
    HANDLE event = nullptr, thread = nullptr;
    UINT32 buffer_frames = 0;
    CRITICAL_SECTION lock;
    std::vector<uint8_t> ring;
    size_t head = 0, tail = 0, fill = 0; // bytes
    float gain = 1.0f;
    volatile bool running = false;
};

static SDL_AudioStream *g_stream;
static FILE *g_dump;
static uint32_t g_dump_bytes;

// Debug: record everything the APU outputs to a 48 kHz stereo WAV
void audio_set_dump(const char *path) {
    g_dump = fopen(path, "wb");
    if (!g_dump) return;
    uint8_t hdr[44] = {0};
    fwrite(hdr, 1, 44, g_dump); // patched on each write
}
static void dump_write(const void *buf, int len) {
    if (!g_dump) return;
    fwrite(buf, 1, len, g_dump);
    g_dump_bytes += len;
    if ((g_dump_bytes & 0xFFFF) < (uint32_t)len) { // refresh header every ~64 KB
        long pos = ftell(g_dump);
        uint32_t riff = 36 + g_dump_bytes, fmt_len = 16, rate = 48000, brate = 48000 * 4, data = g_dump_bytes;
        uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
        fseek(g_dump, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, g_dump); fwrite(&riff, 4, 1, g_dump); fwrite("WAVEfmt ", 1, 8, g_dump);
        fwrite(&fmt_len, 4, 1, g_dump); fwrite(&pcm, 2, 1, g_dump); fwrite(&ch, 2, 1, g_dump);
        fwrite(&rate, 4, 1, g_dump); fwrite(&brate, 4, 1, g_dump); fwrite(&align, 2, 1, g_dump);
        fwrite(&bits, 2, 1, g_dump); fwrite("data", 1, 4, g_dump); fwrite(&data, 4, 1, g_dump);
        fseek(g_dump, pos, SEEK_SET);
        fflush(g_dump);
    }
}
static const char *g_err = "";
static const int kRingBytes = 48000 * 4 / 2; // 500 ms

extern "C" bool SDL_Init(uint32_t flags) { return true; }
extern "C" const char *SDL_GetError(void) { return g_err; }
extern "C" uint64_t SDL_GetTicks(void) { return GetTickCount64(); }
extern "C" int SDL_GetNumLogicalCPUCores(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

static DWORD WINAPI render_thread(void *arg) {
    SDL_AudioStream *s = (SDL_AudioStream *)arg;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (s->running) {
        WaitForSingleObject(s->event, 100);
        UINT32 padding = 0;
        if (FAILED(s->client->GetCurrentPadding(&padding))) continue;
        UINT32 avail = s->buffer_frames - padding;
        if (!avail) continue;
        BYTE *dst;
        if (FAILED(s->render->GetBuffer(avail, &dst))) continue;
        size_t want = (size_t)avail * 4;
        EnterCriticalSection(&s->lock);
        size_t got = want < s->fill ? want : s->fill;
        for (size_t i = 0; i < got; i++) {
            dst[i] = s->ring[s->tail];
            s->tail = (s->tail + 1) % s->ring.size();
        }
        s->fill -= got;
        float g = s->gain;
        LeaveCriticalSection(&s->lock);
        if (g != 1.0f) {
            int16_t *pcm = (int16_t *)dst;
            for (size_t i = 0; i < got / 2; i++) {
                float v = pcm[i] * g;
                pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        }
        if (got < want) memset(dst + got, 0, want - got); // underrun: silence
        s->render->ReleaseBuffer(avail, 0);
    }
    return 0;
}

extern "C" SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec, void *cb, void *ud) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = nullptr;
    IMMDevice *dev = nullptr;
    SDL_AudioStream *s = new SDL_AudioStream;
    InitializeCriticalSection(&s->lock);
    s->ring.resize(kRingBytes);
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&en)) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&s->client))) {
        g_err = "no audio device";
        logf("audio: no output device; running silent");
        return s; // silent stream: data is accepted and discarded by the throttle
    }
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = 48000 * 4;
    REFERENCE_TIME dur = 200000; // 20 ms device buffer
    HRESULT hr = s->client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                           AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                       dur, 0, &wf, nullptr);
    if (FAILED(hr)) {
        logf("audio: IAudioClient::Initialize failed %08lx", hr);
        s->client->Release();
        s->client = nullptr;
        return s;
    }
    s->event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    s->client->SetEventHandle(s->event);
    s->client->GetBufferSize(&s->buffer_frames);
    s->client->GetService(__uuidof(IAudioRenderClient), (void **)&s->render);
    logf("audio: WASAPI output 48kHz stereo, device buffer %u frames", s->buffer_frames);
    g_stream = s;
    return s;
}

extern "C" SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *stream) { return 1; }

extern "C" bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID devid, SDL_AudioSpec *spec, int *frames) {
    spec->format = SDL_AUDIO_S16LE;
    spec->channels = 2;
    spec->freq = 48000;
    *frames = g_stream && g_stream->buffer_frames ? (int)g_stream->buffer_frames : 960;
    return true;
}

extern "C" bool SDL_ResumeAudioDevice(SDL_AudioDeviceID dev) {
    SDL_AudioStream *s = g_stream;
    if (!s || !s->client || s->running) return true;
    s->running = true;
    s->thread = CreateThread(nullptr, 0, render_thread, s, 0, nullptr);
    s->client->Start();
    return true;
}

extern "C" void SDL_DestroyAudioStream(SDL_AudioStream *s) {
    if (!s) return;
    s->running = false;
    if (s->thread) WaitForSingleObject(s->thread, 1000);
    if (s->client) s->client->Stop();
}

extern "C" bool SDL_SetAudioStreamGain(SDL_AudioStream *s, float gain) {
    s->gain = gain;
    return true;
}

extern "C" bool SDL_PutAudioStreamData(SDL_AudioStream *s, const void *buf, int len) {
    dump_write(buf, len);
    if (!s->client) return true;
    const uint8_t *p = (const uint8_t *)buf;
    EnterCriticalSection(&s->lock);
    for (int i = 0; i < len; i++) {
        if (s->fill == s->ring.size()) break; // overflow: drop
        s->ring[s->head] = p[i];
        s->head = (s->head + 1) % s->ring.size();
        s->fill++;
    }
    LeaveCriticalSection(&s->lock);
    return true;
}

extern "C" int SDL_GetAudioStreamQueued(SDL_AudioStream *s) {
    if (!s->client) return -1; // no device: let the APU pace itself by the clock
    EnterCriticalSection(&s->lock);
    int n = (int)s->fill;
    LeaveCriticalSection(&s->lock);
    return n;
}
