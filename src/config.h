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

typedef enum {
    SLIC_PARAMS_NONE,
    SLIC_PARAMS_LIST,
    SLIC_PARAMS_GET,
    SLIC_PARAMS_RANGE
} slic_params_action_t;

typedef struct {
    const char *vdev, *ddev, *font_path;
    const char *visualizer;
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
    int delaunay_point_count;
    double delaunay_edge_weight;
    uint32_t delaunay_seed;
    int slic_cluster_count;
    int slic_iterations;
    int slic_sample_stride;
    int slic_compactness;
    int slic_params_set;
    slic_params_action_t slic_params_action;
    const char *slic_param_query;
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

/* Prints SLIC parameter values/ranges for a completed -a query action. */
void config_print_slic_params(void);

#endif
