#ifndef CELLAVG_PIPELINE_H
#define CELLAVG_PIPELINE_H
#include <stddef.h>
#include <stdint.h>

/* Implemented by one frame-analysis strategy (e.g. per-cell color/luminosity
 * averaging). Converts a raw encoded video frame into a grid of
 * analysis-result records that a matching visualizer_t can render. Multiple
 * analyzer implementations can coexist; the pipeline below only depends on
 * this interface, never on any concrete analyzer. */
typedef struct frame_analyzer {
    const char *name;
    size_t result_cell_size;           /* bytes per output grid cell */
    /* Allocates per-worker-thread scratch state (e.g. a JPEG decompressor);
     * may be NULL if no scratch state is needed. */
    void *(*create_context)(void);
    void (*destroy_context)(void *context);
    /* Decodes/analyzes one raw frame into grid_w*grid_h cells at `out`
     * (sized result_cell_size * grid_w * grid_h). Returns 0 on success, -1
     * to drop the frame (e.g. decode failure). */
    int (*analyze)(void *context, const uint8_t *data, size_t len, void *out);
} frame_analyzer_t;

typedef struct { uint64_t cap, drop, bytes, bad, proc, proc_us, shown; } pipeline_stats_t;

/* Starts `worker_count` analysis worker threads consuming frames submitted
 * via pipeline_submit_frame() and producing grid_w*grid_h result cells with
 * `analyzer`. `queue_depth` sets how many in-flight raw frames may be
 * buffered before new captures are dropped. Returns 0 on success. */
int pipeline_start(const frame_analyzer_t *analyzer, int worker_count, int queue_depth,
                    int grid_w, int grid_h);

/* Signals workers to exit and joins them; call after `running` is cleared. */
void pipeline_stop(void);

/* Called by a capture source for every newly captured raw frame. Copies the
 * frame into a free slot for background analysis, or counts a drop if none
 * is free. */
void pipeline_submit_frame(const uint8_t *data, size_t len);

/* Blocks up to ~100ms for a fresher analyzed grid than `last_seen`. If one
 * is available, copies it into `out` (sized result_cell_size * grid_w *
 * grid_h) and returns its sequence number; otherwise returns `last_seen`. */
uint64_t pipeline_wait_latest(void *out, uint64_t last_seen);

/* Returns accumulated stats since the last call and resets the counters;
 * used for the periodic status line. */
pipeline_stats_t pipeline_take_stats(void);

/* Counts one more frame successfully presented to the display, for the
 * periodic status line's "display fps" figure. */
void pipeline_note_shown(void);

#endif
