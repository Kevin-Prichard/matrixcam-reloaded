#ifndef CELLAVG_SLIC_ANALYZER_H
#define CELLAVG_SLIC_ANALYZER_H

#include "pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t label;
    uint8_t red, green, blue;
} slic_pixel_t;

extern const frame_analyzer_t slic_analyzer;

#ifdef __cplusplus
}
#endif

#endif
