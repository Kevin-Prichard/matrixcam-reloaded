#ifndef CELLAVG_DELAUNAY_H
#define CELLAVG_DELAUNAY_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    double x, y;
} delaunay_point_t;

typedef struct {
    uint32_t a, b, c;
} delaunay_triangle_t;

typedef struct delaunay_context delaunay_context_t;

/* Allocates reusable scratch space for triangulations of up to max_points. */
delaunay_context_t *delaunay_create(size_t max_points);
void delaunay_destroy(delaunay_context_t *context);

/* Returns 0 on success, -1 for invalid input, insufficient output capacity,
 * or allocation failure. Exact duplicate points are ignored; triangle
 * indices always refer to the original input array. Collinear sets succeed
 * with zero triangles. */
int delaunay_triangulate(delaunay_context_t *context,
                         const delaunay_point_t *points, size_t point_count,
                         delaunay_triangle_t *triangles, size_t triangle_capacity,
                         size_t *triangle_count);

#endif
