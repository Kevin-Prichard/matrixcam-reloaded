#include "slic_visualizer.h"
#include "slic_analyzer.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    pthread_t thread;
    int index;
} render_worker_t;

typedef struct {
    const slic_pixel_t *pixels;
    uint8_t *map;
    uint32_t pitch;
    int width, height;
} render_job_t;

static int source_width, output_width, output_height;
static int *xmap, *ymap;
static pthread_mutex_t render_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t render_job_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t render_job_done = PTHREAD_COND_INITIALIZER;
static render_worker_t *render_workers;
static int render_worker_count, render_completed, render_stopping;
static uint64_t render_generation;
static render_job_t active_render_job;

static uint32_t render_pixel(const slic_pixel_t *pixels, int x, int y) {
    const slic_pixel_t *pixel = &pixels[(size_t)y * source_width + x];
    int boundary = (x > 0 && pixel->label != pixel[-1].label) ||
                   (y > 0 && pixel->label != pixels[(size_t)(y - 1) * source_width + x].label);
    if (boundary) return 0x00101010u;
    return (uint32_t)pixel->red << 16 | (uint32_t)pixel->green << 8 | pixel->blue;
}

static void render_rows(const render_job_t *job, int first_row, int end_row) {
    for (int y = first_row; y < end_row; y++) {
        uint32_t *row = (uint32_t *)(job->map + (size_t)y * job->pitch);
        int source_y = ymap[y];
        for (int x = 0; x < job->width; x++)
            row[x] = render_pixel(job->pixels, xmap[x], source_y);
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
        render_rows(&job, first_row, end_row);

        pthread_mutex_lock(&render_mutex);
        render_completed++;
        if (render_completed == render_worker_count)
            pthread_cond_signal(&render_job_done);
    }
    pthread_mutex_unlock(&render_mutex);
    return NULL;
}

static void stop_render_workers(void) {
    if (!render_workers) return;
    pthread_mutex_lock(&render_mutex);
    render_stopping = 1;
    pthread_cond_broadcast(&render_job_ready);
    pthread_mutex_unlock(&render_mutex);
    for (int i = 0; i < render_worker_count; i++)
        pthread_join(render_workers[i].thread, NULL);
    free(render_workers);
    render_workers = NULL;
    render_worker_count = 0;
}

static int visualizer_init(int grid_width, int grid_height, int out_width, int out_height) {
    if (grid_width < 1 || grid_height < 1 || out_width < 1 || out_height < 1) {
        fprintf(stderr, "invalid SLIC visualizer dimensions\n");
        return -1;
    }
    source_width = grid_width;
    output_width = out_width;
    output_height = out_height;
    render_stopping = 0;
    render_generation = 0;
    render_completed = 0;

    xmap = malloc((size_t)output_width * sizeof(*xmap));
    ymap = malloc((size_t)output_height * sizeof(*ymap));
    if (!xmap || !ymap) {
        perror("SLIC visualizer maps");
        free(xmap);
        free(ymap);
        xmap = ymap = NULL;
        return -1;
    }
    for (int x = 0; x < output_width; x++)
        xmap[x] = (int)((uint64_t)x * source_width / output_width);
    for (int y = 0; y < output_height; y++)
        ymap[y] = (int)((uint64_t)y * grid_height / output_height);

    long online_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (online_cores < 1) online_cores = 1;
    size_t worker_count = (size_t)(online_cores < output_height ? online_cores : output_height);
    if (worker_count > SIZE_MAX / sizeof(*render_workers)) {
        fprintf(stderr, "too many SLIC render workers\n");
        free(xmap);
        free(ymap);
        xmap = ymap = NULL;
        return -1;
    }
    render_worker_count = (int)worker_count;
    render_workers = calloc(worker_count, sizeof(*render_workers));
    if (!render_workers) {
        perror("SLIC render workers");
        free(xmap);
        free(ymap);
        xmap = ymap = NULL;
        return -1;
    }

    int started_workers = 0;
    for (int i = 0; i < render_worker_count; i++) {
        render_workers[i].index = i;
        int error = pthread_create(&render_workers[i].thread, NULL, render_worker_main,
                                   &render_workers[i]);
        if (error) {
            fprintf(stderr, "could not create SLIC render worker: %s\n", strerror(error));
            break;
        }
        started_workers++;
    }
    if (started_workers != render_worker_count) {
        render_worker_count = started_workers;
        stop_render_workers();
        free(xmap);
        free(ymap);
        xmap = ymap = NULL;
        return -1;
    }
    printf("SLIC render workers %d\n", render_worker_count);
    return 0;
}

static void visualizer_render(const void *cells, uint8_t *fb_map, uint32_t fb_pitch) {
    pthread_mutex_lock(&render_mutex);
    active_render_job = (render_job_t){
        .pixels = cells,
        .map = fb_map,
        .pitch = fb_pitch,
        .width = output_width,
        .height = output_height
    };
    render_completed = 0;
    render_generation++;
    pthread_cond_broadcast(&render_job_ready);
    while (render_completed < render_worker_count)
        pthread_cond_wait(&render_job_done, &render_mutex);
    pthread_mutex_unlock(&render_mutex);
}

static void visualizer_shutdown(void) {
    stop_render_workers();
    free(xmap);
    free(ymap);
    xmap = ymap = NULL;
    render_stopping = 0;
}

const visualizer_t slic_visualizer = {
    .name = "slic",
    .init = visualizer_init,
    .render = visualizer_render,
    .shutdown = visualizer_shutdown,
};
