#include "joydiv_analyzer.h"
#include "config.h"
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>

struct err_mgr { struct jpeg_error_mgr pub; jmp_buf jb; };

typedef struct {
    struct jpeg_decompress_struct ci;
    struct err_mgr je;
    uint8_t *rowbuf;
    uint8_t *disprow;
    int *xmap;
    uint64_t **zone_sums;
    int *zone_group;
    int *zone_rows;
} joydiv_ctx_t;

static int display_width, display_height;
static joydiv_result_header_t layout;
static joydiv_zone_result_t *layout_zones;

static void on_error(j_common_ptr c) {
    longjmp(((struct err_mgr *)c->err)->jb, 1);
}

static void on_msg(j_common_ptr c, int level) {
    (void)c;
    (void)level;
}

static size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

int joydiv_analyzer_configure(int out_width, int out_height) {
    if (out_width < 1 || out_height < 1 ||
        (size_t)out_width > SIZE_MAX / (size_t)out_height) {
        fprintf(stderr, "invalid joydiv display dimensions\n");
        return -1;
    }
    size_t pixel_count = (size_t)out_width * (size_t)out_height;
    if (pixel_count > SIZE_MAX / 3) {
        fprintf(stderr, "joydiv display frame is too large\n");
        return -1;
    }
    size_t frame_bytes = pixel_count * 3;
    int zone_count = G.joydiv_zone_count;
    if (zone_count < 1 || (size_t)zone_count > SIZE_MAX / sizeof(*layout_zones)) {
        fprintf(stderr, "invalid joydiv zone count\n");
        return -1;
    }

    free(layout_zones);
    layout_zones = calloc((size_t)zone_count, sizeof(*layout_zones));
    if (!layout_zones) {
        perror("joydiv result layout");
        return -1;
    }

    size_t zone_offset = sizeof(layout);
    size_t zone_bytes = (size_t)zone_count * sizeof(*layout_zones);
    if (zone_offset > SIZE_MAX - zone_bytes) {
        fprintf(stderr, "joydiv result layout is too large\n");
        return -1;
    }
    size_t frame_offset = align_up(zone_offset + zone_bytes, 8);
    if (frame_offset > SIZE_MAX - frame_bytes) {
        fprintf(stderr, "joydiv result layout is too large\n");
        return -1;
    }
    size_t cursor = align_up(frame_offset + frame_bytes, 8);

    for (int i = 0; i < zone_count; i++) {
        const joydiv_zone_t *zone = &G.joydiv_zones[i];
        joydiv_zone_result_t *result = &layout_zones[i];
        int channels = zone->yvalue == JOYDIV_YVALUE_COLOR ? 3 : 1;
        int groups = (zone->h + G.joydiv_grouprows - 1) / G.joydiv_grouprows;
        if ((size_t)zone->w > SIZE_MAX / (size_t)channels) {
            fprintf(stderr, "joydiv result data is too large\n");
            return -1;
        }
        size_t per_group = (size_t)zone->w * (size_t)channels;
        if ((size_t)groups > SIZE_MAX / per_group) {
            fprintf(stderr, "joydiv result data is too large\n");
            return -1;
        }
        size_t line_bytes = (size_t)groups * per_group;
        if (cursor > SIZE_MAX - line_bytes) {
            fprintf(stderr, "joydiv result data is too large\n");
            return -1;
        }
        result->x = zone->x;
        result->y = zone->y;
        result->w = zone->w;
        result->h = zone->h;
        result->yvalue = zone->yvalue;
        result->lines = zone->lines;
        result->channel_count = channels;
        result->group_count = groups;
        result->b = zone->b;
        result->bez_tension = zone->bez_tension;
        result->bez_smooth = zone->bez_smooth;
        result->data_offset = cursor;
        cursor += line_bytes;
    }

    layout = (joydiv_result_header_t){
        .magic = JOYDIV_RESULT_MAGIC,
        .frame_width = out_width,
        .frame_height = out_height,
        .zone_count = zone_count,
        .grouprows = G.joydiv_grouprows,
        .zone_offset = zone_offset,
        .frame_offset = frame_offset,
        .data_offset = align_up(frame_offset + frame_bytes, 8),
        .total_size = cursor
    };
    display_width = out_width;
    display_height = out_height;
    joydiv_analyzer.result_cell_size = cursor;
    return 0;
}

static void *create_context(void) {
    joydiv_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        perror("joydiv context");
        return NULL;
    }
    ctx->ci.err = jpeg_std_error(&ctx->je.pub);
    ctx->je.pub.error_exit = on_error;
    ctx->je.pub.emit_message = on_msg;
    jpeg_create_decompress(&ctx->ci);

    int zone_count = layout.zone_count;
    ctx->rowbuf = malloc((size_t)G.capw * 3 + 64);
    ctx->disprow = malloc((size_t)display_width * 3);
    ctx->xmap = malloc((size_t)display_width * sizeof(*ctx->xmap));
    ctx->zone_sums = calloc((size_t)zone_count, sizeof(*ctx->zone_sums));
    ctx->zone_group = malloc((size_t)zone_count * sizeof(*ctx->zone_group));
    ctx->zone_rows = calloc((size_t)zone_count, sizeof(*ctx->zone_rows));
    if (!ctx->rowbuf || !ctx->disprow || !ctx->xmap || !ctx->zone_sums ||
        !ctx->zone_group || !ctx->zone_rows) {
        perror("joydiv context buffers");
        goto fail;
    }
    for (int i = 0; i < zone_count; i++) {
        size_t count = (size_t)layout_zones[i].w * (size_t)layout_zones[i].channel_count;
        ctx->zone_sums[i] = malloc(count * sizeof(*ctx->zone_sums[i]));
        if (!ctx->zone_sums[i]) {
            perror("joydiv zone sums");
            goto fail;
        }
        ctx->zone_group[i] = -1;
    }
    for (int x = 0; x < display_width; x++)
        ctx->xmap[x] = (int)((int64_t)x * G.capw / display_width);
    return ctx;

fail:
    if (ctx->zone_sums) {
        for (int i = 0; i < zone_count; i++) free(ctx->zone_sums[i]);
    }
    free(ctx->zone_sums);
    free(ctx->zone_group);
    free(ctx->zone_rows);
    free(ctx->xmap);
    free(ctx->disprow);
    free(ctx->rowbuf);
    jpeg_destroy_decompress(&ctx->ci);
    free(ctx);
    return NULL;
}

static void destroy_context(void *context) {
    joydiv_ctx_t *ctx = context;
    if (ctx->zone_sums) {
        for (int i = 0; i < layout.zone_count; i++) free(ctx->zone_sums[i]);
    }
    free(ctx->zone_sums);
    free(ctx->zone_group);
    free(ctx->zone_rows);
    free(ctx->xmap);
    free(ctx->disprow);
    free(ctx->rowbuf);
    jpeg_destroy_decompress(&ctx->ci);
    free(ctx);
}

static void finalize_group(joydiv_ctx_t *ctx, int zone_index, uint8_t *result) {
    int group = ctx->zone_group[zone_index];
    int rows = ctx->zone_rows[zone_index];
    if (group < 0 || rows < 1) return;

    const joydiv_zone_result_t *zone = &layout_zones[zone_index];
    size_t stride = (size_t)zone->w * (size_t)zone->channel_count;
    uint8_t *destination = result + zone->data_offset +
        (size_t)group * stride;
    uint64_t *sums = ctx->zone_sums[zone_index];

    for (size_t i = 0; i < stride; i++)
        destination[i] = (uint8_t)((sums[i] + (uint64_t)rows / 2) / (uint64_t)rows);

    memset(sums, 0, stride * sizeof(*sums));
    ctx->zone_group[zone_index] = -1;
    ctx->zone_rows[zone_index] = 0;
}

static int process(joydiv_ctx_t *ctx, const uint8_t *data, size_t len, uint8_t *out) {
    if (setjmp(ctx->je.jb)) {
        jpeg_abort_decompress(&ctx->ci);
        return -1;
    }
    jpeg_mem_src(&ctx->ci, data, len);
    jpeg_read_header(&ctx->ci, TRUE);
    ctx->ci.scale_num = 1;
    ctx->ci.scale_denom = 1;
    ctx->ci.out_color_space = JCS_RGB;
    ctx->ci.dct_method = JDCT_IFAST;
    ctx->ci.do_fancy_upsampling = FALSE;
    jpeg_start_decompress(&ctx->ci);
    if ((int)ctx->ci.output_width != G.capw ||
        (int)ctx->ci.output_height != G.caph) {
        fprintf(stderr, "joydiv_analyzer: size mismatch %dx%d vs %dx%d\n",
                ctx->ci.output_width, ctx->ci.output_height, G.capw, G.caph);
        jpeg_abort_decompress(&ctx->ci);
        return -1;
    }

        memcpy(out, &layout, sizeof(layout));
        memcpy(out + layout.zone_offset, layout_zones,
               (size_t)layout.zone_count * sizeof(*layout_zones));
        uint8_t *frame = out + layout.frame_offset;
        for (int i = 0; i < layout.zone_count; i++) {
            size_t stride = (size_t)layout_zones[i].w * (size_t)layout_zones[i].channel_count;
            memset(ctx->zone_sums[i], 0, stride * sizeof(*ctx->zone_sums[i]));
            ctx->zone_group[i] = -1;
            ctx->zone_rows[i] = 0;
        }

        static int diag_frame = 0;
        unsigned char *row_written = calloc((size_t)display_height, 1);
        if (!row_written) {
            jpeg_abort_decompress(&ctx->ci);
            return -1;
        }
        /* ensure full frame zeroed before filling */
        memset(frame, 0, (size_t)display_width * (size_t)display_height * 3);
        for (int y = 0; y < G.caph; y++) {
            JSAMPROW row = ctx->rowbuf;
            if (jpeg_read_scanlines(&ctx->ci, &row, 1) != 1) {
                fprintf(stderr, "joydiv_analyzer: jpeg_read_scanlines failed at y=%d\n", y);
                jpeg_abort_decompress(&ctx->ci);
                return -1;
            }

            int first_display_row = (int)((int64_t)y * display_height / G.caph);
            int end_display_row = (int)((int64_t)(y + 1) * display_height / G.caph);
            if (first_display_row >= display_height) continue;
            if (end_display_row > display_height) end_display_row = display_height;
            if (end_display_row <= first_display_row) end_display_row = first_display_row + 1;
            if (first_display_row < 0) first_display_row = 0;
            /* ensure no gaps by extending previous if needed - not necessary, but track */
            if (first_display_row < end_display_row) {
                for (int x = 0; x < display_width; x++) {
                    int source_x = ctx->xmap[x];
                    const uint8_t *pixel = ctx->rowbuf + (size_t)source_x * 3;
                    ctx->disprow[(size_t)x * 3] = pixel[0];
                    ctx->disprow[(size_t)x * 3 + 1] = pixel[1];
                    ctx->disprow[(size_t)x * 3 + 2] = pixel[2];
                }
                for (int display_y = first_display_row; display_y < end_display_row;
                     display_y++) {
                    memcpy(frame + (size_t)display_y * (size_t)display_width * 3,
                           ctx->disprow, (size_t)display_width * 3);
                    if (display_y >= 0 && display_y < display_height)
                        row_written[display_y] = 1;
                }
                if (diag_frame < 3 && y == 0) {
                    fprintf(stderr, "joydiv_diag: cap %dx%d -> disp %dx%d first=%d end=%d\n",
                            G.capw, G.caph, display_width, display_height, first_display_row, end_display_row);
                }
            } else if (diag_frame < 5) {
                fprintf(stderr, "joydiv_diag: y=%d bad map first=%d end=%d\n", y, first_display_row, end_display_row);
            }

        for (int zone_index = 0; zone_index < layout.zone_count; zone_index++) {
            const joydiv_zone_result_t *zone = &layout_zones[zone_index];
            if (y < zone->y || y >= zone->y + zone->h) continue;

            int group = (y - zone->y) / G.joydiv_grouprows;
            if (ctx->zone_group[zone_index] != group) {
                finalize_group(ctx, zone_index, out);
                ctx->zone_group[zone_index] = group;
            }
            ctx->zone_rows[zone_index]++;

            const uint8_t *pixel = ctx->rowbuf + (size_t)zone->x * 3;
            uint64_t *sums = ctx->zone_sums[zone_index];
            if (zone->channel_count == 1) {
                for (int x = 0; x < zone->w; x++, pixel += 3) {
                    uint32_t luminosity =
                        (77u * pixel[0] + 150u * pixel[1] + 29u * pixel[2]) >> 8;
                    sums[x] += luminosity;
                }
            } else {
                for (int x = 0; x < zone->w; x++, pixel += 3) {
                    sums[(size_t)x * 3] += pixel[0];
                    sums[(size_t)x * 3 + 1] += pixel[1];
                    sums[(size_t)x * 3 + 2] += pixel[2];
                }
            }
        }
    }

    for (int i = 0; i < layout.zone_count; i++)
        finalize_group(ctx, i, out);
    jpeg_abort_decompress(&ctx->ci);
    int filled = 0;
    for (int i = 0; i < display_height; i++) if (row_written[i]) filled++;
    diag_frame++;
    if (diag_frame % 30 == 0 || filled != display_height) {
        fprintf(stderr, "joydiv_analyzer_diag: frame=%d filled=%d/%d display rows\n",
                diag_frame, filled, display_height);
    }
    free(row_written);
    return 0;
}

static int analyze(void *context, const uint8_t *data, size_t len, void *out) {
    return process(context, data, len, out);
}

frame_analyzer_t joydiv_analyzer = {
    .name = "joydiv",
    .result_cell_size = 1,
    .create_context = create_context,
    .destroy_context = destroy_context,
    .analyze = analyze,
};
