#include "delaunay_visualizer.h"
#include "cell_analyzer.h"
#include "config.h"
#include "delaunay.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    double priority;
    size_t cell_index;
    delaunay_point_t point;
    uint32_t color;
} candidate_t;

typedef struct {
    const delaunay_point_t *points;
    const uint32_t *colors;
    const delaunay_triangle_t *triangles;
    size_t triangle_count;
    uint8_t *map;
    uint32_t pitch;
    int height;
} render_job_t;

typedef struct {
    pthread_t thread;
    int index;
} render_worker_t;

static int grid_width, grid_height, output_width, output_height;
static size_t candidate_capacity, point_capacity, triangle_capacity;
static candidate_t *candidates;
static delaunay_context_t *triangulator;
static delaunay_point_t *work_points, *mesh_points;
static uint32_t *work_colors, *mesh_colors;
static delaunay_triangle_t *work_triangles, *mesh_triangles;
static size_t mesh_point_count, mesh_triangle_count;
static int reported_triangulation_error;
static pthread_mutex_t render_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t render_job_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t render_job_done = PTHREAD_COND_INITIALIZER;
static render_worker_t *render_workers;
static int render_worker_count, render_completed, render_stopping;
static uint64_t render_generation;
static render_job_t active_render_job;

static uint32_t hash32(uint32_t value) {
    value ^= value >> 16;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15;
    value *= UINT32_C(0x846ca68b);
    value ^= value >> 16;
    return value;
}

static uint32_t random_for_cell(size_t cell_index, uint32_t salt) {
    return hash32((uint32_t)cell_index ^ G.delaunay_seed ^ salt);
}

static uint32_t cell_color(const cell_t *cell) {
    uint8_t red, green, blue;
    if (cell->luminosity >= 127) {
        red = cell->foreground_r;
        green = cell->foreground_g;
        blue = cell->foreground_b;
    } else {
        red = cell->background_r;
        green = cell->background_g;
        blue = cell->background_b;
    }
    return (uint32_t)red << 16 | (uint32_t)green << 8 | blue;
}

static int compare_candidates(const void *left, const void *right) {
    const candidate_t *a = left, *b = right;
    if (a->priority < b->priority) return -1;
    if (a->priority > b->priority) return 1;
    return (a->cell_index > b->cell_index) - (a->cell_index < b->cell_index);
}

static size_t prepare_points(const cell_t *cells) {
    size_t count = candidate_capacity;
    double cell_width = output_width / (double)grid_width;
    double cell_height = output_height / (double)grid_height;
    for (int y = 0; y < grid_height; y++) {
        for (int x = 0; x < grid_width; x++) {
            size_t index = (size_t)y * grid_width + x;
            const cell_t *cell = &cells[index];
            unsigned horizontal = 0, vertical = 0;
            if (x > 0) {
                int value = (int)cell->luminosity - cells[index - 1].luminosity;
                horizontal += (unsigned)(value < 0 ? -value : value);
            }
            if (x + 1 < grid_width) {
                int value = (int)cell->luminosity - cells[index + 1].luminosity;
                horizontal += (unsigned)(value < 0 ? -value : value);
            }
            if (y > 0) {
                int value = (int)cell->luminosity - cells[index - grid_width].luminosity;
                vertical += (unsigned)(value < 0 ? -value : value);
            }
            if (y + 1 < grid_height) {
                int value = (int)cell->luminosity - cells[index + grid_width].luminosity;
                vertical += (unsigned)(value < 0 ? -value : value);
            }
            double feature = (horizontal + vertical) / 1020.0;
            double random_value =
                (random_for_cell(index, UINT32_C(0x9e3779b9)) + 1.0) / 4294967296.0;
            double edge_bias = 1.0 + G.delaunay_edge_weight * feature * 8.0;
            uint32_t jitter_x = random_for_cell(index, UINT32_C(0x243f6a88));
            uint32_t jitter_y = random_for_cell(index, UINT32_C(0xb7e15162));
            double jx = ((jitter_x / 4294967295.0) - 0.5) * 0.5;
            double jy = ((jitter_y / 4294967295.0) - 0.5) * 0.5;
            double px = (x + 0.5 + jx) * cell_width;
            double py = (y + 0.5 + jy) * cell_height;
            if (px > output_width) px = output_width;
            if (py > output_height) py = output_height;
            candidates[index] = (candidate_t){
                .priority = random_value / edge_bias,
                .cell_index = index,
                .point = {px, py},
                .color = cell_color(cell)
            };
        }
    }
    qsort(candidates, count, sizeof(*candidates), compare_candidates);

    size_t selected = (size_t)G.delaunay_point_count;
    if (selected > count) selected = count;
    size_t total = selected + 4;
    work_points[0] = (delaunay_point_t){0, 0};
    work_points[1] = (delaunay_point_t){output_width, 0};
    work_points[2] = (delaunay_point_t){output_width, output_height};
    work_points[3] = (delaunay_point_t){0, output_height};
    size_t corner_cells[4] = {
        0,
        (size_t)grid_width - 1,
        (size_t)(grid_height - 1) * grid_width + grid_width - 1,
        (size_t)(grid_height - 1) * grid_width
    };
    for (size_t i = 0; i < 4; i++)
        work_colors[i] = cell_color(&cells[corner_cells[i]]);
    for (size_t i = 0; i < selected; i++) {
        work_points[i + 4] = candidates[i].point;
        work_colors[i + 4] = candidates[i].color;
    }
    return total;
}

static uint32_t average_color(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t red = (((a >> 16) & 255) + ((b >> 16) & 255) + ((c >> 16) & 255) + 1) / 3;
    uint32_t green = (((a >> 8) & 255) + ((b >> 8) & 255) + ((c >> 8) & 255) + 1) / 3;
    uint32_t blue = ((a & 255) + (b & 255) + (c & 255) + 1) / 3;
    return red << 16 | green << 8 | blue;
}

static void put_pixel(uint8_t *map, uint32_t pitch, int x, int y, uint32_t color) {
    uint32_t *row = (uint32_t *)(map + (size_t)y * pitch);
    row[x] = color;
}

static void fill_triangle(uint8_t *map, uint32_t pitch, const delaunay_point_t *a,
                          const delaunay_point_t *b, const delaunay_point_t *c,
                          uint32_t color, int first_row, int end_row) {
    double area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (fabs(area) < 1e-9) return;
    int min_x = (int)floor(fmin(a->x, fmin(b->x, c->x)));
    int max_x = (int)ceil(fmax(a->x, fmax(b->x, c->x)));
    int min_y = (int)floor(fmin(a->y, fmin(b->y, c->y)));
    int max_y = (int)ceil(fmax(a->y, fmax(b->y, c->y)));
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= output_width) max_x = output_width - 1;
    if (max_y >= output_height) max_y = output_height - 1;
    if (min_y < first_row) min_y = first_row;
    if (max_y >= end_row) max_y = end_row - 1;
    for (int y = min_y; y <= max_y; y++) {
        double py = y + 0.5;
        for (int x = min_x; x <= max_x; x++) {
            double px = x + 0.5;
            double e0 = (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
            double e1 = (c->x - b->x) * (py - b->y) - (c->y - b->y) * (px - b->x);
            double e2 = (a->x - c->x) * (py - c->y) - (a->y - c->y) * (px - c->x);
            if ((area > 0 && e0 >= 0 && e1 >= 0 && e2 >= 0) ||
                (area < 0 && e0 <= 0 && e1 <= 0 && e2 <= 0))
                put_pixel(map, pitch, x, y, color);
        }
    }
}

static void draw_line(uint8_t *map, uint32_t pitch, const delaunay_point_t *a,
                      const delaunay_point_t *b) {
    int x0 = (int)(a->x + 0.5), y0 = (int)(a->y + 0.5);
    int x1 = (int)(b->x + 0.5), y1 = (int)(b->y + 0.5);
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < output_width && y0 >= 0 && y0 < output_height)
            put_pixel(map, pitch, x0, y0, 0x00181818u);
        if (x0 == x1 && y0 == y1) break;
        int twice_error = 2 * error;
        if (twice_error >= dy) { error += dy; x0 += sx; }
        if (twice_error <= dx) { error += dx; y0 += sy; }
    }
}

static void render_fill_rows(const render_job_t *job, int first_row, int end_row) {
    for (int y = first_row; y < end_row; y++)
        memset(job->map + (size_t)y * job->pitch, 0,
               (size_t)output_width * sizeof(uint32_t));
    for (size_t i = 0; i < job->triangle_count; i++) {
        const delaunay_triangle_t *triangle = &job->triangles[i];
        fill_triangle(job->map, job->pitch, &job->points[triangle->a],
                      &job->points[triangle->b], &job->points[triangle->c],
                      average_color(job->colors[triangle->a], job->colors[triangle->b],
                                    job->colors[triangle->c]), first_row, end_row);
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
        render_fill_rows(&job, first_row, end_row);

        pthread_mutex_lock(&render_mutex);
        render_completed++;
        if (render_completed == render_worker_count)
            pthread_cond_signal(&render_job_done);
    }
    pthread_mutex_unlock(&render_mutex);
    return NULL;
}

static int render_workers_start(int height) {
    long online_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (online_cores < 1) online_cores = 1;
    render_worker_count = online_cores < height ? (int)online_cores : height;
    render_workers = calloc((size_t)render_worker_count, sizeof(*render_workers));
    if (!render_workers) { perror("Delaunay render workers"); return -1; }

    int started = 0;
    for (int i = 0; i < render_worker_count; i++) {
        render_workers[i].index = i;
        int error = pthread_create(&render_workers[i].thread, NULL, render_worker_main,
                                   &render_workers[i]);
        if (error) {
            fprintf(stderr, "could not create Delaunay render worker: %s\n", strerror(error));
            break;
        }
        started++;
    }
    if (started != render_worker_count) {
        pthread_mutex_lock(&render_mutex);
        render_stopping = 1;
        pthread_cond_broadcast(&render_job_ready);
        pthread_mutex_unlock(&render_mutex);
        for (int i = 0; i < started; i++) pthread_join(render_workers[i].thread, NULL);
        free(render_workers);
        render_workers = NULL;
        render_worker_count = 0;
        return -1;
    }
    printf("Delaunay render workers %d\n", render_worker_count);
    return 0;
}

static void render_workers_stop(void) {
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

static void render_mesh(const delaunay_point_t *points, const uint32_t *colors,
                        const delaunay_triangle_t *triangles, size_t triangle_count,
                        uint8_t *map, uint32_t pitch) {
    render_job_t job = {
        .points = points, .colors = colors, .triangles = triangles,
        .triangle_count = triangle_count, .map = map, .pitch = pitch,
        .height = output_height
    };
    dispatch_render_job(&job);
    for (size_t i = 0; i < triangle_count; i++) {
        const delaunay_triangle_t *triangle = &triangles[i];
        draw_line(map, pitch, &points[triangle->a], &points[triangle->b]);
        draw_line(map, pitch, &points[triangle->b], &points[triangle->c]);
        draw_line(map, pitch, &points[triangle->c], &points[triangle->a]);
    }
}

static int visualizer_init(int gw, int gh, int outw, int outh) {
    if (gw < 1 || gh < 1 || outw < 1 || outh < 1 ||
        (size_t)gw > SIZE_MAX / (size_t)gh) {
        fprintf(stderr, "invalid Delaunay visualizer dimensions\n");
        return -1;
    }
    grid_width = gw;
    grid_height = gh;
    output_width = outw;
    output_height = outh;
    candidate_capacity = (size_t)gw * gh;
    if (candidate_capacity > SIZE_MAX / sizeof(*candidates)) {
        fprintf(stderr, "Delaunay candidate buffer is too large\n");
        return -1;
    }
    point_capacity = (size_t)G.delaunay_point_count + 4;
    triangle_capacity = point_capacity * 3 + 32;

    candidates = malloc(candidate_capacity * sizeof(*candidates));
    work_points = malloc(point_capacity * sizeof(*work_points));
    mesh_points = malloc(point_capacity * sizeof(*mesh_points));
    work_colors = malloc(point_capacity * sizeof(*work_colors));
    mesh_colors = malloc(point_capacity * sizeof(*mesh_colors));
    work_triangles = malloc(triangle_capacity * sizeof(*work_triangles));
    mesh_triangles = malloc(triangle_capacity * sizeof(*mesh_triangles));
    triangulator = delaunay_create(point_capacity);
    if (!candidates || !work_points || !mesh_points || !work_colors || !mesh_colors ||
        !work_triangles || !mesh_triangles || !triangulator) {
        perror("Delaunay visualizer buffers");
        return -1;
    }
    if (render_workers_start(output_height) < 0) return -1;
    printf("Delaunay visualizer: up to %d sampled points, edge weight %.2f, seed %u\n",
           G.delaunay_point_count, G.delaunay_edge_weight, G.delaunay_seed);
    return 0;
}

static void visualizer_render(const void *data, uint8_t *fb_map, uint32_t fb_pitch) {
    const cell_t *cells = data;
    size_t point_count = prepare_points(cells);
    size_t triangle_count = 0;
    if (delaunay_triangulate(triangulator, work_points, point_count, work_triangles,
                             triangle_capacity, &triangle_count) == 0) {
        delaunay_point_t *old_points = mesh_points;
        mesh_points = work_points;
        work_points = old_points;
        uint32_t *old_colors = mesh_colors;
        mesh_colors = work_colors;
        work_colors = old_colors;
        delaunay_triangle_t *old_triangles = mesh_triangles;
        mesh_triangles = work_triangles;
        work_triangles = old_triangles;
        mesh_point_count = point_count;
        mesh_triangle_count = triangle_count;
    } else if (!reported_triangulation_error) {
        fprintf(stderr, "Delaunay triangulation failed; retaining the last valid mesh\n");
        reported_triangulation_error = 1;
    }
    if (mesh_point_count)
        render_mesh(mesh_points, mesh_colors, mesh_triangles, mesh_triangle_count,
                    fb_map, fb_pitch);
    else
        for (int y = 0; y < output_height; y++)
            memset(fb_map + (size_t)y * fb_pitch, 0,
                   (size_t)output_width * sizeof(uint32_t));
}

static void visualizer_shutdown(void) {
    render_workers_stop();
    delaunay_destroy(triangulator);
    free(candidates);
    free(work_points);
    free(mesh_points);
    free(work_colors);
    free(mesh_colors);
    free(work_triangles);
    free(mesh_triangles);
    candidates = NULL;
    triangulator = NULL;
    work_points = mesh_points = NULL;
    work_colors = mesh_colors = NULL;
    work_triangles = mesh_triangles = NULL;
    mesh_point_count = mesh_triangle_count = 0;
}

const visualizer_t delaunay_visualizer = {
    .name = "delaunay",
    .init = visualizer_init,
    .render = visualizer_render,
    .shutdown = visualizer_shutdown,
};
