#include "delaunay.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    long double x, y;
    uint32_t original;
} vertex_t;

typedef struct {
    uint32_t a, b, c;
} work_triangle_t;

typedef struct {
    uint32_t generation;
    uint32_t low, high;
    uint32_t from, to;
    unsigned count;
} edge_slot_t;

struct delaunay_context {
    size_t max_points;
    size_t triangle_capacity;
    size_t edge_capacity;
    vertex_t *vertices;
    work_triangle_t *work_triangles;
    edge_slot_t *edges;
    size_t *edge_touched;
    size_t edge_touched_count;
    uint32_t generation;
};

static int multiply_size(size_t a, size_t b, size_t *result) {
    if (a && b > SIZE_MAX / a) return -1;
    *result = a * b;
    return 0;
}

static size_t next_power_of_two(size_t value) {
    size_t result = 1;
    while (result < value) {
        if (result > SIZE_MAX / 2) return 0;
        result *= 2;
    }
    return result;
}

delaunay_context_t *delaunay_create(size_t max_points) {
    if (max_points > UINT32_MAX - 3 || max_points > (SIZE_MAX - 32) / 3)
        return NULL;
    size_t edge_target;
    if (multiply_size(max_points ? max_points : 1, 16, &edge_target) < 0)
        return NULL;
    size_t edge_capacity = next_power_of_two(edge_target);
    size_t vertex_bytes, triangle_bytes, edge_bytes, touched_bytes;
    size_t triangle_capacity = max_points * 3 + 32;
    if (!edge_capacity ||
        multiply_size(max_points + 3, sizeof(vertex_t), &vertex_bytes) < 0 ||
        multiply_size(triangle_capacity, sizeof(work_triangle_t), &triangle_bytes) < 0 ||
        multiply_size(edge_capacity, sizeof(edge_slot_t), &edge_bytes) < 0 ||
        multiply_size(edge_capacity, sizeof(size_t), &touched_bytes) < 0)
        return NULL;

    delaunay_context_t *context = calloc(1, sizeof(*context));
    if (!context) return NULL;
    context->max_points = max_points;
    context->triangle_capacity = triangle_capacity;
    context->edge_capacity = edge_capacity;
    context->vertices = calloc(1, vertex_bytes);
    context->work_triangles = malloc(triangle_bytes);
    context->edges = calloc(1, edge_bytes);
    context->edge_touched = malloc(touched_bytes);
    if (!context->vertices || !context->work_triangles || !context->edges ||
        !context->edge_touched) {
        delaunay_destroy(context);
        return NULL;
    }
    return context;
}

void delaunay_destroy(delaunay_context_t *context) {
    if (!context) return;
    free(context->vertices);
    free(context->work_triangles);
    free(context->edges);
    free(context->edge_touched);
    free(context);
}

static int compare_vertices(const void *left, const void *right) {
    const vertex_t *a = left, *b = right;
    if (a->x < b->x) return -1;
    if (a->x > b->x) return 1;
    if (a->y < b->y) return -1;
    if (a->y > b->y) return 1;
    return (a->original > b->original) - (a->original < b->original);
}

static long double orientation(const vertex_t *a, const vertex_t *b, const vertex_t *c) {
    return (b->x - a->x) * (c->y - a->y) -
           (b->y - a->y) * (c->x - a->x);
}

static int circle_contains(const vertex_t *a, const vertex_t *b, const vertex_t *c,
                           const vertex_t *point) {
    long double ax = a->x - point->x, ay = a->y - point->y;
    long double bx = b->x - point->x, by = b->y - point->y;
    long double cx = c->x - point->x, cy = c->y - point->y;
    long double aa = ax * ax + ay * ay;
    long double bb = bx * bx + by * by;
    long double cc = cx * cx + cy * cy;
    long double determinant = aa * (bx * cy - by * cx)
                            - bb * (ax * cy - ay * cx)
                            + cc * (ax * by - ay * bx);
    long double scale = fabsl(ax);
    if (fabsl(ay) > scale) scale = fabsl(ay);
    if (fabsl(bx) > scale) scale = fabsl(bx);
    if (fabsl(by) > scale) scale = fabsl(by);
    if (fabsl(cx) > scale) scale = fabsl(cx);
    if (fabsl(cy) > scale) scale = fabsl(cy);
    long double tolerance = LDBL_EPSILON * 128 * (scale * scale * scale * scale + 1);
    return determinant > tolerance;
}

static uint64_t edge_hash(uint32_t low, uint32_t high) {
    uint64_t key = (uint64_t)low << 32 | high;
    key ^= key >> 30;
    key *= UINT64_C(0xbf58476d1ce4e5b9);
    key ^= key >> 27;
    key *= UINT64_C(0x94d049bb133111eb);
    key ^= key >> 31;
    return key;
}

static int add_edge(delaunay_context_t *context, uint32_t from, uint32_t to) {
    uint32_t low = from < to ? from : to;
    uint32_t high = from < to ? to : from;
    size_t index = (size_t)edge_hash(low, high) & (context->edge_capacity - 1);
    while (context->edges[index].generation == context->generation &&
           (context->edges[index].low != low || context->edges[index].high != high))
        index = (index + 1) & (context->edge_capacity - 1);
    edge_slot_t *slot = &context->edges[index];
    if (slot->generation != context->generation) {
        slot->generation = context->generation;
        slot->low = low;
        slot->high = high;
        slot->from = from;
        slot->to = to;
        slot->count = 1;
        context->edge_touched[context->edge_touched_count++] = index;
    } else {
        slot->count++;
        if (slot->count > 2) return -1;
    }
    return 0;
}

static int add_triangle(delaunay_context_t *context, size_t *triangle_count,
                        uint32_t a, uint32_t b, uint32_t c) {
    long double area = orientation(&context->vertices[a], &context->vertices[b],
                                   &context->vertices[c]);
    if (fabsl(area) <= LDBL_EPSILON * 128) return 0;
    if (area < 0) {
        uint32_t swap = a;
        a = b;
        b = swap;
    }
    if (*triangle_count >= context->triangle_capacity) return -1;
    context->work_triangles[(*triangle_count)++] = (work_triangle_t){a, b, c};
    return 0;
}

static int point_in_triangle(const vertex_t *a, const vertex_t *b, const vertex_t *c,
                             const vertex_t *point) {
    long double scale = 1;
    const vertex_t *vertices[] = {a, b, c, point};
    for (size_t i = 0; i < sizeof(vertices) / sizeof(vertices[0]); i++) {
        if (fabsl(vertices[i]->x) > scale) scale = fabsl(vertices[i]->x);
        if (fabsl(vertices[i]->y) > scale) scale = fabsl(vertices[i]->y);
    }
    long double tolerance = LDBL_EPSILON * 128 * scale * scale;
    return orientation(a, b, point) >= -tolerance &&
           orientation(b, c, point) >= -tolerance &&
           orientation(c, a, point) >= -tolerance;
}

int delaunay_triangulate(delaunay_context_t *context,
                         const delaunay_point_t *points, size_t point_count,
                         delaunay_triangle_t *triangles, size_t triangle_capacity,
                         size_t *triangle_count) {
    if (triangle_count) *triangle_count = 0;
    if (!context || !triangle_count || (point_count && !points) ||
        (triangle_capacity && !triangles) || point_count > context->max_points ||
        point_count > UINT32_MAX - 3)
        return -1;
    for (size_t i = 0; i < point_count; i++) {
        if (!isfinite(points[i].x) || !isfinite(points[i].y)) return -1;
        context->vertices[i] = (vertex_t){points[i].x, points[i].y, (uint32_t)i};
    }
    if (point_count < 3) return 0;

    qsort(context->vertices, point_count, sizeof(*context->vertices), compare_vertices);
    long double min_x = context->vertices[0].x, max_x = min_x;
    long double min_y = context->vertices[0].y, max_y = min_y;
    for (size_t i = 1; i < point_count; i++) {
        if (context->vertices[i].x < min_x) min_x = context->vertices[i].x;
        if (context->vertices[i].x > max_x) max_x = context->vertices[i].x;
        if (context->vertices[i].y < min_y) min_y = context->vertices[i].y;
        if (context->vertices[i].y > max_y) max_y = context->vertices[i].y;
    }
    long double span_x = max_x - min_x, span_y = max_y - min_y;
    long double span = span_x > span_y ? span_x : span_y;
    if (span == 0) return 0;

    size_t unique_count = 0;
    long double previous_x = 0, previous_y = 0;
    for (size_t i = 0; i < point_count; i++) {
        if (unique_count &&
            context->vertices[i].x == previous_x &&
            context->vertices[i].y == previous_y)
            continue;
        vertex_t vertex = context->vertices[i];
        previous_x = vertex.x;
        previous_y = vertex.y;
        vertex.x = (vertex.x - min_x) / span;
        vertex.y = (vertex.y - min_y) / span;
        context->vertices[unique_count++] = vertex;
    }
    if (unique_count < 3) return 0;

    uint32_t super_a = (uint32_t)unique_count;
    uint32_t super_b = super_a + 1;
    uint32_t super_c = super_a + 2;
    context->vertices[super_a] = (vertex_t){-32, -16, UINT32_MAX};
    context->vertices[super_b] = (vertex_t){33, -16, UINT32_MAX};
    context->vertices[super_c] = (vertex_t){0.5L, 33, UINT32_MAX};
    size_t active_count = 0;
    if (add_triangle(context, &active_count, super_a, super_b, super_c) < 0)
        return -1;

    for (size_t point_index = 0; point_index < unique_count; point_index++) {
        if (++context->generation == 0) {
            memset(context->edges, 0, context->edge_capacity * sizeof(*context->edges));
            context->generation = 1;
        }
        context->edge_touched_count = 0;
        vertex_t *point = &context->vertices[point_index];
        size_t removed = 0;
        for (size_t i = 0; i < active_count; i++) {
            work_triangle_t triangle = context->work_triangles[i];
            vertex_t *a = &context->vertices[triangle.a];
            vertex_t *b = &context->vertices[triangle.b];
            vertex_t *c = &context->vertices[triangle.c];
            if (!circle_contains(a, b, c, point) &&
                !point_in_triangle(a, b, c, point))
                continue;
            if (add_edge(context, triangle.a, triangle.b) < 0 ||
                add_edge(context, triangle.b, triangle.c) < 0 ||
                add_edge(context, triangle.c, triangle.a) < 0)
                return -1;
            context->work_triangles[i].a = UINT32_MAX;
            removed++;
        }
        if (!removed) return -1;

        size_t kept_count = 0;
        for (size_t i = 0; i < active_count; i++) {
            if (context->work_triangles[i].a != UINT32_MAX)
                context->work_triangles[kept_count++] = context->work_triangles[i];
        }
        active_count = kept_count;
        for (size_t i = 0; i < context->edge_touched_count; i++) {
            edge_slot_t *edge = &context->edges[context->edge_touched[i]];
            if (edge->generation == context->generation && edge->count == 1 &&
                add_triangle(context, &active_count, edge->from, edge->to,
                             (uint32_t)point_index) < 0)
                return -1;
        }
    }

    for (size_t i = 0; i < active_count; i++) {
        work_triangle_t triangle = context->work_triangles[i];
        if (triangle.a >= unique_count || triangle.b >= unique_count ||
            triangle.c >= unique_count)
            continue;
        if (*triangle_count >= triangle_capacity) {
            *triangle_count = 0;
            return -1;
        }
        triangles[*triangle_count] = (delaunay_triangle_t){
            context->vertices[triangle.a].original,
            context->vertices[triangle.b].original,
            context->vertices[triangle.c].original
        };
        (*triangle_count)++;
    }
    return 0;
}
