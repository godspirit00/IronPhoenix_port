/* Tiny subset of the SDL3 API used by xemu's APU monitor, backed by WASAPI (audio/audio_out.cpp). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t SDL_AudioDeviceID;
typedef enum { SDL_AUDIO_S16LE = 0x8010 } SDL_AudioFormat;
typedef struct SDL_AudioSpec {
    SDL_AudioFormat format;
    int channels;
    int freq;
} SDL_AudioSpec;
typedef struct SDL_AudioStream SDL_AudioStream;

#define SDL_INIT_AUDIO 0x10u
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ((SDL_AudioDeviceID)0xFFFFFFFFu)
#define SDL_AUDIO_BYTESIZE(x) (((x) & 0xFF) / 8)

bool SDL_Init(uint32_t flags);
const char *SDL_GetError(void);
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec, void *callback, void *userdata);
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *stream);
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID devid, SDL_AudioSpec *spec, int *sample_frames);
bool SDL_ResumeAudioDevice(SDL_AudioDeviceID dev);
void SDL_DestroyAudioStream(SDL_AudioStream *stream);
bool SDL_SetAudioStreamGain(SDL_AudioStream *stream, float gain);
bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *buf, int len);
int SDL_GetAudioStreamQueued(SDL_AudioStream *stream);
uint64_t SDL_GetTicks(void);
int SDL_GetNumLogicalCPUCores(void);

#ifdef __cplusplus
}
#endif
