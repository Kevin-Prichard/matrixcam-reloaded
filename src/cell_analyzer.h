#ifndef CELLAVG_CELL_ANALYZER_H
#define CELLAVG_CELL_ANALYZER_H
#include <stdint.h>
#include "pipeline.h"

/* Per-cell dual-color (foreground/background) average plus overall
 * luminosity, produced by cell_analyzer from one decoded video frame. This
 * is the concrete grid-cell format a matching glyph_visualizer expects. */
typedef struct {
    uint8_t foreground_r, foreground_g, foreground_b;
    uint8_t background_r, background_g, background_b;
    uint8_t luminosity;
} cell_t;

/* Decodes an MJPEG frame and reduces it to a G.gw x G.gh grid of cell_t
 * averages, splitting each cell's pixels into foreground/background by a
 * luminosity threshold and (per G.hflip) mirroring horizontally. Implements
 * the frame_analyzer_t interface. */
extern const frame_analyzer_t cell_analyzer;

#endif
