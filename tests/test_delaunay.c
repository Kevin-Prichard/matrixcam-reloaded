#include "delaunay.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static size_t triangulate(delaunay_context_t *context, const delaunay_point_t *points,
                          size_t point_count, delaunay_triangle_t *triangles,
                          size_t capacity) {
    size_t triangle_count = 0;
    assert(delaunay_triangulate(context, points, point_count, triangles, capacity,
                                &triangle_count) == 0);
    return triangle_count;
}

static long double orient(delaunay_point_t a, delaunay_point_t b, delaunay_point_t c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static long double incircle(delaunay_point_t a, delaunay_point_t b, delaunay_point_t c,
                            delaunay_point_t p) {
    long double ax = a.x - p.x, ay = a.y - p.y;
    long double bx = b.x - p.x, by = b.y - p.y;
    long double cx = c.x - p.x, cy = c.y - p.y;
    return (ax * ax + ay * ay) * (bx * cy - by * cx)
         - (bx * bx + by * by) * (ax * cy - ay * cx)
         + (cx * cx + cy * cy) * (ax * by - ay * bx);
}

static void check_mesh(const delaunay_point_t *points, size_t point_count,
                       const delaunay_triangle_t *triangles, size_t triangle_count) {
    for (size_t i = 0; i < triangle_count; i++) {
        delaunay_triangle_t triangle = triangles[i];
        assert(triangle.a < point_count && triangle.b < point_count && triangle.c < point_count);
        assert(fabsl(orient(points[triangle.a], points[triangle.b], points[triangle.c])) > 1e-12L);
    }
}

int main(void) {
    delaunay_context_t *context = delaunay_create(128);
    assert(context);
    delaunay_triangle_t triangles[256];

    const delaunay_point_t triangle_points[] = {{0, 0}, {4, 0}, {1, 3}};
    size_t count = triangulate(context, triangle_points, 3, triangles, 256);
    assert(count == 1);
    check_mesh(triangle_points, 3, triangles, count);

    const delaunay_point_t square[] = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
    count = triangulate(context, square, 4, triangles, 256);
    assert(count == 2);
    check_mesh(square, 4, triangles, count);

    const delaunay_point_t duplicate[] = {
        {0, 0}, {4, 0}, {4, 4}, {0, 4}, {4, 0}
    };
    count = triangulate(context, duplicate, 5, triangles, 256);
    assert(count == 2);
    check_mesh(duplicate, 5, triangles, count);

    const delaunay_point_t collinear[] = {{0, 0}, {1, 1}, {2, 2}, {3, 3}};
    assert(triangulate(context, collinear, 4, triangles, 256) == 0);

    const delaunay_point_t near_collinear[] = {
        {0, 0}, {1, 1e-10}, {2, 0}, {1, 2e-10}, {1.25, 0.4}
    };
    count = triangulate(context, near_collinear, 5, triangles, 256);
    check_mesh(near_collinear, 5, triangles, count);

    delaunay_point_t random_points[64];
    uint32_t state = 0x12345678u;
    for (size_t i = 0; i < 64; i++) {
        state = state * 1664525u + 1013904223u;
        random_points[i].x = (state >> 8) / 16777216.0;
        state = state * 1664525u + 1013904223u;
        random_points[i].y = (state >> 8) / 16777216.0;
    }
    count = triangulate(context, random_points, 64, triangles, 256);
    assert(count > 80);
    check_mesh(random_points, 64, triangles, count);
    for (size_t i = 0; i < count; i++) {
        delaunay_triangle_t triangle = triangles[i];
        delaunay_point_t a = random_points[triangle.a];
        delaunay_point_t b = random_points[triangle.b];
        delaunay_point_t c = random_points[triangle.c];
        if (orient(a, b, c) < 0) {
            delaunay_point_t swap = a;
            a = b;
            b = swap;
        }
        for (size_t j = 0; j < 64; j++) {
            if (j == triangle.a || j == triangle.b || j == triangle.c) continue;
            assert(incircle(a, b, c, random_points[j]) < 1e-10L);
        }
    }

    delaunay_point_t invalid[] = {{0, 0}, {1, 0}, {NAN, 1}};
    size_t ignored;
    assert(delaunay_triangulate(context, invalid, 3, triangles, 256, &ignored) == -1);
    assert(delaunay_triangulate(context, triangle_points, 3, triangles, 0, &ignored) == -1);

    delaunay_destroy(context);
    puts("Delaunay geometry tests passed");
    return 0;
}
