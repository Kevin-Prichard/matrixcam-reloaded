#ifndef CELLAVG_CONFIG_H
#define CELLAVG_CONFIG_H
#include <stdint.h>

/* One inclusive [low, high] Unicode codepoint range requested via -r. */
typedef struct { uint32_t low, high; } codepoint_range_t;
typedef struct { const char *name; int value; } camera_param_t;

typedef enum {
    CAMERA_PARAMS_NONE,
    CAMERA_PARAMS_LIST,
    CAMERA_PARAMS_GET,
    CAMERA_PARAMS_RANGE
} camera_params_action_t;

typedef struct {
    const char *vdev, *ddev, *font_path;
    int capw, caph;              /* requested/actual capture resolution */
    int cw, ch;                  /* cell size in pixels */
    int outw, outh;              /* requested display resolution */
    int nworkers;
    int gray, hflip, bichrome;
    int focus_value;             /* -1 = autofocus, else manual focus_absolute */
    uint32_t bichrome_fg, bichrome_bg;
    int denom;                   /* JPEG decode scale denominator (1/denom) */
    int scw, sch;                /* source pixels sampled per cell after scaling */
    int gw, gh;                  /* grid dimensions in cells, derived after capture starts */
    codepoint_range_t *codepoint_ranges;
    int codepoint_range_count;
    camera_param_t *camera_params;
    int camera_param_count;
    camera_params_action_t camera_params_action;
    const char *camera_param_query;
} config_t;

extern config_t G;

/* Parses argv into G, applying defaults for anything not specified. Exits
 * the process with a usage message on invalid/unknown arguments. */
void config_parse_args(int argc, char **argv);

/* Derives G.denom/scw/sch/gw/gh from G.cw/ch and the actual capture
 * resolution (G.capw/caph, which may have been adjusted by the capture
 * source to what the driver granted). Exits the process if the cell size
 * does not fit the frame. */
void config_derive_grid(void);

#endif
