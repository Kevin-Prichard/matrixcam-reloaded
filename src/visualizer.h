#ifndef CELLAVG_VISUALIZER_H
#define CELLAVG_VISUALIZER_H
#include <stdint.h>

/* Implemented by one frame-visualization strategy (e.g. glyph rendering).
 * Converts an analysis-result grid produced by a matching frame_analyzer_t
 * into packed 0xRRGGBB pixels written into a display framebuffer. Multiple
 * visualizer implementations can coexist; the main loop only depends on
 * this interface, never on any concrete visualizer or analyzer. */
typedef struct visualizer {
    const char *name;
    /* One-time setup given the analysis grid dimensions and the output
     * framebuffer's pixel dimensions; may spin up worker threads. Returns 0
     * on success, -1 on failure. */
    int (*init)(int grid_w, int grid_h, int out_width, int out_height);
    /* Renders one fresh grid_w*grid_h analysis-result grid into the given
     * framebuffer (row-major 32bpp, `fb_pitch` bytes per row). */
    void (*render)(const void *cells, uint8_t *fb_map, uint32_t fb_pitch);
    void (*shutdown)(void);
} visualizer_t;

#endif
