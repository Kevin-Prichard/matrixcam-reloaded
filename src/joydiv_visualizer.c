#include "joydiv_visualizer.h"
#include "joydiv_analyzer.h"
#include "config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    float x, y;
} plot_point_t;

static int output_width, output_height;
static plot_point_t *points;

static uint32_t blend_color(uint32_t background, uint32_t foreground, int alpha) {
    if (alpha >= 255) return foreground;
    if (alpha <= 0) return background;
    uint32_t inverse = (uint32_t)(255 - alpha);
    uint32_t red = (((background >> 16) & 255) * inverse +
                    ((foreground >> 16) & 255) * (uint32_t)alpha + 127) / 255;
    uint32_t green = (((background >> 8) & 255) * inverse +
                      ((foreground >> 8) & 255) * (uint32_t)alpha + 127) / 255;
    uint32_t blue = ((background & 255) * inverse +
                     (foreground & 255) * (uint32_t)alpha + 127) / 255;
    return red << 16 | green << 8 | blue;
}

static void put_pixel(uint8_t *map, uint32_t pitch, int x, int y,
                      uint32_t color, int alpha) {
    if (x < 0 || x >= output_width || y < 0 || y >= output_height) return;
    uint32_t *row = (uint32_t *)(map + (size_t)y * pitch);
    row[x] = blend_color(row[x], color, alpha);
}

enum {
    CLIP_LEFT = 1,
    CLIP_RIGHT = 2,
    CLIP_BOTTOM = 4,
    CLIP_TOP = 8
};

static int clip_code(double x, double y, double xmin, double ymin,
                     double xmax, double ymax) {
    int code = 0;
    if (x < xmin) code |= CLIP_LEFT;
    else if (x > xmax) code |= CLIP_RIGHT;
    if (y < ymin) code |= CLIP_TOP;
    else if (y > ymax) code |= CLIP_BOTTOM;
    return code;
}

static int clip_line(double *x0, double *y0, double *x1, double *y1,
                     double xmin, double ymin, double xmax, double ymax) {
    int c0 = clip_code(*x0, *y0, xmin, ymin, xmax, ymax);
    int c1 = clip_code(*x1, *y1, xmin, ymin, xmax, ymax);
    for (int i = 0; i < 8; i++) {
        if (!(c0 | c1)) return 1;
        if (c0 & c1) return 0;
        int code = c0 ? c0 : c1;
        double x = 0, y = 0;
        if (code & CLIP_TOP) {
            x = *x0 + (*x1 - *x0) * (ymin - *y0) / (*y1 - *y0);
            y = ymin;
        } else if (code & CLIP_BOTTOM) {
            x = *x0 + (*x1 - *x0) * (ymax - *y0) / (*y1 - *y0);
            y = ymax;
        } else if (code & CLIP_RIGHT) {
            y = *y0 + (*y1 - *y0) * (xmax - *x0) / (*x1 - *x0);
            x = xmax;
        } else {
            y = *y0 + (*y1 - *y0) * (xmin - *x0) / (*x1 - *x0);
            x = xmin;
        }
        if (code == c0) {
            *x0 = x;
            *y0 = y;
            c0 = clip_code(*x0, *y0, xmin, ymin, xmax, ymax);
        } else {
            *x1 = x;
            *y1 = y;
            c1 = clip_code(*x1, *y1, xmin, ymin, xmax, ymax);
        }
    }
    return 0;
}

static void draw_line(uint8_t *map, uint32_t pitch,
                      float x0f, float y0f, float x1f, float y1f,
                      int xmin, int ymin, int xmax, int ymax,
                      uint32_t color, int alpha) {
    double x0 = x0f, y0 = y0f, x1 = x1f, y1 = y1f;
    if (!clip_line(&x0, &y0, &x1, &y1, xmin, ymin, xmax, ymax)) return;
    int ix0 = (int)lround(x0), iy0 = (int)lround(y0);
    int ix1 = (int)lround(x1), iy1 = (int)lround(y1);
    int dx = abs(ix1 - ix0), sx = ix0 < ix1 ? 1 : -1;
    int dy = -abs(iy1 - iy0), sy = iy0 < iy1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        put_pixel(map, pitch, ix0, iy0, color, alpha);
        if (ix0 == ix1 && iy0 == iy1) break;
        int twice_error = 2 * error;
        if (twice_error >= dy) {
            error += dy;
            ix0 += sx;
        }
        if (twice_error <= dx) {
            error += dx;
            iy0 += sy;
        }
    }
}

static void draw_point(uint8_t *map, uint32_t pitch, const plot_point_t *point,
                       uint32_t color, int alpha) {
    put_pixel(map, pitch, (int)lround(point->x), (int)lround(point->y), color, alpha);
}

static void draw_straight(uint8_t *map, uint32_t pitch, int count,
                          int xmin, int ymin, int xmax, int ymax,
                          uint32_t color, int alpha) {
    if (count < 2) {
        if (count == 1) draw_point(map, pitch, &points[0], color, alpha);
        return;
    }
    for (int i = 1; i < count; i++)
        draw_line(map, pitch, points[i - 1].x, points[i - 1].y,
                  points[i].x, points[i].y, xmin, ymin, xmax, ymax, color, alpha);
}

static void draw_bezier(uint8_t *map, uint32_t pitch, int count,
                        double tension, double smooth,
                        int xmin, int ymin, int xmax, int ymax,
                        uint32_t color, int alpha) {
    if (count < 2) {
        if (count == 1) draw_point(map, pitch, &points[0], color, alpha);
        return;
    }
    int steps = (int)ceil(smooth);
    if (steps < 1) steps = 1;
    if (steps > 64) steps = 64;
    for (int i = 0; i < count - 1; i++) {
        plot_point_t p0 = i > 0 ? points[i - 1] : points[i];
        plot_point_t p1 = points[i];
        plot_point_t p2 = points[i + 1];
        plot_point_t p3 = i + 2 < count ? points[i + 2] : points[i + 1];
        float c1x = p1.x + (float)(tension * (p2.x - p0.x) / 6.0);
        float c1y = p1.y + (float)(tension * (p2.y - p0.y) / 6.0);
        float c2x = p2.x - (float)(tension * (p3.x - p1.x) / 6.0);
        float c2y = p2.y - (float)(tension * (p3.y - p1.y) / 6.0);
        plot_point_t previous = p1;
        for (int step = 1; step <= steps; step++) {
            float t = (float)step / (float)steps;
            float u = 1.0f - t;
            float x = u * u * u * p1.x + 3.0f * u * u * t * c1x +
                      3.0f * u * t * t * c2x + t * t * t * p2.x;
            float y = u * u * u * p1.y + 3.0f * u * u * t * c1y +
                      3.0f * u * t * t * c2y + t * t * t * p2.y;
            draw_line(map, pitch, previous.x, previous.y, x, y,
                      xmin, ymin, xmax, ymax, color, alpha);
            previous = (plot_point_t){x, y};
        }
    }
}

static float attenuation(float x, float width, int border) {
    if (border <= 0) return 1.0f;
    float distance = x < width - x ? x : width - x;
    float u = (distance - (float)border) / (float)border;
    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;
    return u * u * (3.0f - 2.0f * u);
}

static void render_zone(const uint8_t *result, const joydiv_result_header_t *header,
                        const joydiv_zone_result_t *zone,
                        uint8_t *fb_map, uint32_t fb_pitch) {
    int64_t rx0 = (int64_t)zone->x * output_width / G.capw;
    int64_t rx1 = (int64_t)(zone->x + zone->w) * output_width / G.capw;
    int64_t ry0 = (int64_t)zone->y * output_height / G.caph;
    int64_t ry1 = (int64_t)(zone->y + zone->h) * output_height / G.caph;
    if (rx1 <= rx0) rx1 = rx0 + 1;
    if (ry1 <= ry0) ry1 = ry0 + 1;
    if (rx0 < 0) rx0 = 0;
    if (ry0 < 0) ry0 = 0;
    if (rx1 > output_width) rx1 = output_width;
    if (ry1 > output_height) ry1 = output_height;
    int rect_w = (int)(rx1 - rx0);
    int rect_h = (int)(ry1 - ry0);
    if (rect_w < 1 || rect_h < 1) return;

    int clip_x0 = G.hflip ? output_width - (int)(rx0 + rect_w) : (int)rx0;
    int clip_x1 = clip_x0 + rect_w - 1;
    int clip_y0 = (int)ry0;
    int clip_y1 = clip_y0 + rect_h - 1;

    double spacing = (double)header->grouprows * rect_h / (double)zone->h;
    if (spacing > rect_h) spacing = rect_h;
    const uint8_t *data = result + zone->data_offset;
    int channels = zone->channel_count;

    for (int group = 0; group < zone->group_count; group++) {
        int first_row = group * header->grouprows;
        int rows = zone->h - first_row;
        if (rows <= 0) break;
        if (rows > header->grouprows) rows = header->grouprows;
        double baseline = (double)ry0 +
            ((double)first_row + rows * 0.5) * rect_h / (double)zone->h;

        for (int channel = 0; channel < channels; channel++) {
            int count = zone->w;
            for (int column = 0; column < count; column++) {
                size_t index = ((size_t)group * (size_t)zone->w + (size_t)column) *
                    (size_t)channels + (size_t)channel;
                float value = (float)data[index] / 255.0f;
                float window = attenuation((float)column + 0.5f,
                                           (float)zone->w, zone->b);
                float displacement = value * window * (float)spacing * 2.0f;
                float plot_y = (float)(baseline - displacement);
                if (plot_y < (float)clip_y0) plot_y = (float)clip_y0;
                if (plot_y > (float)clip_y1) plot_y = (float)clip_y1;
                int position = (int)(((double)column + 0.5) * rect_w /
                                     (double)zone->w);
                if (position < 0) position = 0;
                if (position >= rect_w) position = rect_w - 1;
                float plot_x = (float)(rx0 + position);
                if (G.hflip) plot_x = (float)(output_width - 1) - plot_x;
                points[column] = (plot_point_t){plot_x, plot_y};
            }

            uint32_t color = channels == 1 ? 0x00ffffffu
                : channel == 0 ? 0x00ff0000u
                : channel == 1 ? 0x0000ff00u : 0x000000ffu;
            if (zone->lines == JOYDIV_LINES_BEZIER) {
                draw_bezier(fb_map, fb_pitch, count, zone->bez_tension,
                            zone->bez_smooth, clip_x0, clip_y0, clip_x1, clip_y1,
                            color, G.joydiv_alpha);
            } else {
                draw_straight(fb_map, fb_pitch, count, clip_x0, clip_y0,
                              clip_x1, clip_y1, color, G.joydiv_alpha);
            }
        }
    }
}

static int visualizer_init(int grid_width, int grid_height,
                           int out_width, int out_height) {
    (void)grid_width;
    (void)grid_height;
    if (out_width < 1 || out_height < 1) {
        fprintf(stderr, "invalid joydiv visualizer dimensions\n");
        return -1;
    }
    output_width = out_width;
    output_height = out_height;
    int max_width = 1;
    for (int i = 0; i < G.joydiv_zone_count; i++)
        if (G.joydiv_zones[i].w > max_width) max_width = G.joydiv_zones[i].w;
    points = malloc((size_t)max_width * sizeof(*points));
    if (!points) {
        perror("joydiv plot points");
        return -1;
    }
    return 0;
}

static void visualizer_render(const void *cells, uint8_t *fb_map, uint32_t fb_pitch) {
    const joydiv_result_header_t *header = cells;
    if (header->magic != JOYDIV_RESULT_MAGIC ||
        header->frame_width != output_width ||
        header->frame_height != output_height ||
        header->zone_count < 1 ||
        header->grouprows < 1) return;

    if (G.joydiv_background == JOYDIV_BACKGROUND_VIDEO) {
        const uint8_t *frame = (const uint8_t *)cells + header->frame_offset;
        for (int y = 0; y < output_height; y++) {
            uint32_t *row = (uint32_t *)(fb_map + (size_t)y * fb_pitch);
            for (int x = 0; x < output_width; x++) {
                int source_x = G.hflip ? output_width - 1 - x : x;
                const uint8_t *pixel =
                    frame + ((size_t)y * (size_t)output_width + (size_t)source_x) * 3;
                row[x] = (uint32_t)pixel[0] << 16 |
                         (uint32_t)pixel[1] << 8 | pixel[2];
            }
        }
    } else {
        for (int y = 0; y < output_height; y++) {
            uint32_t *row = (uint32_t *)(fb_map + (size_t)y * fb_pitch);
            for (int x = 0; x < output_width; x++)
                row[x] = G.joydiv_background_color;
        }
    }

    const joydiv_zone_result_t *zones =
        (const joydiv_zone_result_t *)((const uint8_t *)cells + header->zone_offset);
    for (int i = 0; i < header->zone_count; i++)
        render_zone(cells, header, &zones[i], fb_map, fb_pitch);
}

static void visualizer_shutdown(void) {
    free(points);
    points = NULL;
}

const visualizer_t joydiv_visualizer = {
    .name = "joydiv",
    .init = visualizer_init,
    .render = visualizer_render,
    .shutdown = visualizer_shutdown,
};
