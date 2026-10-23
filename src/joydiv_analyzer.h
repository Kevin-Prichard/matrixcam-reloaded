#ifndef CELLAVG_JOYDIV_ANALYZER_H
#define CELLAVG_JOYDIV_ANALYZER_H

#include "pipeline.h"
#include <stddef.h>
#include <stdint.h>

#define JOYDIV_RESULT_MAGIC 0x4a445652u

typedef struct {
    uint32_t magic;
    int frame_width, frame_height;
    int zone_count, grouprows;
    size_t zone_offset, frame_offset, data_offset, total_size;
} joydiv_result_header_t;

typedef struct {
    int x, y, w, h;
    int yvalue;
    int lines;
    int channel_count, group_count, b;
    double bez_tension, bez_smooth;
    size_t data_offset;
} joydiv_zone_result_t;

extern frame_analyzer_t joydiv_analyzer;

int joydiv_analyzer_configure(int out_width, int out_height);

#endif
