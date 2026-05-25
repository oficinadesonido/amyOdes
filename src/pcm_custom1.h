#ifndef __PCM_H
#define __PCM_H

#include "pcm_samples_custom1.h"

const uint16_t pcm_samples = 6;
#define PCM_LENGTH 118739
#define PCM_AMY_SAMPLE_RATE 22050

const pcm_map_t pcm_map[6] PROGMEM = {
    {0, 4752, 0, 4752, 36},
    {4752, 12615, 0, 12615, 36},
    {17367, 6497, 0, 6497, 36},
    {23864, 25575, 0, 25575, 36},
    {49439, 44100, 0, 44100, 36},
    {93539, 25200, 0, 25200, 36},
};

#endif
