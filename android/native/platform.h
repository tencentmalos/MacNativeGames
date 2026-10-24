// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Called on a producer worker, never on Oboe's realtime callback.
typedef void (*MngAudioPull)(void *context, float *stereo, unsigned frames);
void *mng_audio_open(int rate, MngAudioPull pull, void *context);
void mng_audio_close(void *output); // joins worker before releasing context
void mng_audio_reopen(void *output);
uint64_t mng_audio_consumed(void *output);
#ifdef __cplusplus
}
#endif
