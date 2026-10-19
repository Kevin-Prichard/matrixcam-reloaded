/* ---------- Glyph-rendering visualizer ---------- */
#include "glyph_visualizer.h"
#include "cell_analyzer.h"
#include "config.h"
#include "glyph_atlas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

typedef struct {
    uint32_t foreground, background;
    const uint8_t *glyph;
} render_cell_t;

typedef struct {
    uint8_t *map;
    uint32_t pitch;
    const render_cell_t *cells;
    const int *xmap, *xsample, *ymap, *ysample;
    int width, height;
} render_job_t;

typedef struct {
    pthread_t thread;
    int index;
    uint32_t *line;
} render_worker_t;

static int grid_w, grid_h, out_w, out_h;
static int *xmap, *xsample, *ymap, *ysample;
static render_cell_t *render_cells;

static pthread_mutex_t render_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t render_job_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t render_job_done = PTHREAD_COND_INITIALIZER;
static render_worker_t *render_workers;
static int render_worker_count, render_completed, render_stopping;
static uint64_t render_generation;
static render_job_t active_render_job;

static uint32_t pack_color(uint8_t red, uint8_t green, uint8_t blue) {
    if (G.gray) {
        uint32_t luminosity = (77 * red + 150 * green + 29 * blue) >> 8;
        return luminosity * 0x010101u;
    }
    return (uint32_t)red << 16 | (uint32_t)green << 8 | blue;
}

static void prepare_render_cells(const cell_t *cells) {
    size_t cell_count = (size_t)grid_w * grid_h;
    for (size_t i = 0; i < cell_count; i++) {
        const cell_t *cell = &cells[i];
        render_cells[i].foreground = G.bichrome ? G.bichrome_fg
            : pack_color(cell->foreground_r, cell->foreground_g, cell->foreground_b);
        render_cells[i].background = G.bichrome ? G.bichrome_bg
            : pack_color(cell->background_r, cell->background_g, cell->background_b);
        render_cells[i].glyph = glyph_atlas_bitmap_for_luminosity(cell->luminosity);
    }
}

static uint32_t blend_pixel(uint32_t foreground, uint32_t background, uint8_t coverage) {
    uint32_t inverse_coverage = 255 - coverage;
    uint32_t red = (((foreground >> 16) & 255) * coverage +
                    ((background >> 16) & 255) * inverse_coverage + 127) / 255;
    uint32_t green = (((foreground >> 8) & 255) * coverage +
                      ((background >> 8) & 255) * inverse_coverage + 127) / 255;
    uint32_t blue = ((foreground & 255) * coverage +
                     (background & 255) * inverse_coverage + 127) / 255;
    return red << 16 | green << 8 | blue;
}

static void render_rows(const render_job_t *job, uint32_t *line, int first_row, int end_row) {
    for (int y = first_row; y < end_row; y++) {
        int cell_y = job->ymap[y];
        int glyph_y = job->ysample[y];
        for (int x = 0; x < job->width; x++) {
            int cell_x = job->xmap[x];
            const render_cell_t *cell = &job->cells[cell_y * grid_w + cell_x];
            uint8_t coverage = cell->glyph[(size_t)glyph_y * G.cw + job->xsample[x]];
            line[x] = coverage == 0 ? cell->background
                    : coverage == 255 ? cell->foreground
                    : blend_pixel(cell->foreground, cell->background, coverage);
        }
        memcpy(job->map + (size_t)y * job->pitch, line, (size_t)job->width * 4);
    }
}

static void *render_worker_main(void *argument) {
    render_worker_t *worker = argument;
    uint64_t completed_generation = 0;
    pthread_mutex_lock(&render_mutex);
    for (;;) {
        while (!render_stopping && render_generation == completed_generation)
            pthread_cond_wait(&render_job_ready, &render_mutex);
        if (render_stopping) break;
        completed_generation = render_generation;
        render_job_t job = active_render_job;
        pthread_mutex_unlock(&render_mutex);

        int first_row = (int)((int64_t)worker->index * job.height / render_worker_count);
        int end_row = (int)((int64_t)(worker->index + 1) * job.height / render_worker_count);
        render_rows(&job, worker->line, first_row, end_row);

        pthread_mutex_lock(&render_mutex);
        render_completed++;
        if (render_completed == render_worker_count)
            pthread_cond_signal(&render_job_done);
    }
    pthread_mutex_unlock(&render_mutex);
    return NULL;
}

static int render_workers_start(int width, int height) {
    long available_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (available_cores < 1) available_cores = 1;
    render_worker_count = available_cores < height ? (int)available_cores : height;
    render_workers = calloc(render_worker_count, sizeof(*render_workers));
    if (!render_workers) { perror("render workers"); return -1; }

    int started_workers = 0;
    for (int i = 0; i < render_worker_count; i++) {
        render_workers[i].index = i;
        render_workers[i].line = malloc((size_t)width * sizeof(uint32_t));
        if (!render_workers[i].line) {
            perror("render scanline");
            break;
        }
        int error = pthread_create(&render_workers[i].thread, NULL, render_worker_main,
                                   &render_workers[i]);
        if (error) {
            fprintf(stderr, "could not create render worker: %s\n", strerror(error));
            free(render_workers[i].line);
            render_workers[i].line = NULL;
            break;
        }
        started_workers++;
    }
    if (started_workers != render_worker_count) {
        pthread_mutex_lock(&render_mutex);
        render_stopping = 1;
        pthread_cond_broadcast(&render_job_ready);
        pthread_mutex_unlock(&render_mutex);
        for (int i = 0; i < started_workers; i++)
            pthread_join(render_workers[i].thread, NULL);
        for (int i = 0; i < render_worker_count; i++) free(render_workers[i].line);
        free(render_workers);
        render_workers = NULL;
        render_worker_count = 0;
        return -1;
    }
    printf("render workers %d\n", render_worker_count);
    return 0;
}

static void render_workers_stop(void) {
    pthread_mutex_lock(&render_mutex);
    render_stopping = 1;
    pthread_cond_broadcast(&render_job_ready);
    pthread_mutex_unlock(&render_mutex);
    for (int i = 0; i < render_worker_count; i++)
        pthread_join(render_workers[i].thread, NULL);
    for (int i = 0; i < render_worker_count; i++) free(render_workers[i].line);
    free(render_workers);
}

static void dispatch_render_job(const render_job_t *job) {
    pthread_mutex_lock(&render_mutex);
    active_render_job = *job;
    render_completed = 0;
    render_generation++;
    pthread_cond_broadcast(&render_job_ready);
    while (render_completed < render_worker_count)
        pthread_cond_wait(&render_job_done, &render_mutex);
    pthread_mutex_unlock(&render_mutex);
}

static int visualizer_init(int grid_width, int grid_height, int out_width, int out_height) {
    grid_w = grid_width; grid_h = grid_height; out_w = out_width; out_h = out_height;
    if (render_workers_start(out_w, out_h) < 0) return -1;

    xmap = malloc(out_w * sizeof(int));
    xsample = malloc(out_w * sizeof(int));
    ymap = malloc(out_h * sizeof(int));
    ysample = malloc(out_h * sizeof(int));
    render_cells = malloc((size_t)grid_w * grid_h * sizeof(render_cell_t));
    if (!xmap || !xsample || !ymap || !ysample || !render_cells) {
        perror("glyph visualizer buffers");
        render_workers_stop();
        return -1;
    }
    for (int x = 0; x < out_w; x++) {
        uint64_t position = (uint64_t)x * grid_w;
        xmap[x] = position / out_w;
        xsample[x] = (position % out_w) * G.cw / out_w;
    }
    for (int y = 0; y < out_h; y++) {
        uint64_t position = (uint64_t)y * grid_h;
        ymap[y] = position / out_h;
        ysample[y] = (position % out_h) * G.ch / out_h;
    }
    return 0;
}

static void visualizer_render(const void *cells, uint8_t *fb_map, uint32_t fb_pitch) {
    prepare_render_cells(cells);
    render_job_t job = {
        .map = fb_map, .pitch = fb_pitch,
        .cells = render_cells,
        .xmap = xmap, .xsample = xsample, .ymap = ymap, .ysample = ysample,
        .width = out_w, .height = out_h
    };
    dispatch_render_job(&job);
}

static void visualizer_shutdown(void) {
    render_workers_stop();
    free(xmap); free(xsample); free(ymap); free(ysample); free(render_cells);
}

const visualizer_t glyph_visualizer = {
    .name = "glyph",
    .init = visualizer_init,
    .render = visualizer_render,
    .shutdown = visualizer_shutdown,
};
