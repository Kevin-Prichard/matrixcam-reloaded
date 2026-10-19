/* ---------- JPEG decode + per-cell color averaging ---------- */
#include "cell_analyzer.h"
#include "config.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>

enum {
    SUM_FOREGROUND_R, SUM_FOREGROUND_G, SUM_FOREGROUND_B, SUM_FOREGROUND_COUNT,
    SUM_BACKGROUND_R, SUM_BACKGROUND_G, SUM_BACKGROUND_B, SUM_BACKGROUND_COUNT,
    SUM_LUMINOSITY, SUM_FIELDS
};

struct err_mgr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void on_error(j_common_ptr c) { longjmp(((struct err_mgr *)c->err)->jb, 1); }
static void on_msg(j_common_ptr c, int l) { (void)c; (void)l; }

static int process(struct jpeg_decompress_struct *ci, struct err_mgr *je,
                   const uint8_t *data, size_t len, cell_t *out,
                   uint8_t *rowbuf, uint32_t *sum) {
    if (setjmp(je->jb)) { jpeg_abort_decompress(ci); return -1; }
    jpeg_mem_src(ci, data, len);
    jpeg_read_header(ci, TRUE);
    ci->scale_num = 1; ci->scale_denom = G.denom;
    ci->out_color_space = JCS_RGB;
    ci->dct_method = JDCT_IFAST;
    ci->do_fancy_upsampling = FALSE;
    jpeg_start_decompress(ci);
    if ((int)ci->output_width < G.gw * G.scw || (int)ci->output_height < G.gh * G.sch ||
        (int)ci->output_width > G.capw) { jpeg_abort_decompress(ci); return -1; }

    uint32_t pixels_per_cell = G.scw * G.sch;
    for (int cy = 0; cy < G.gh; cy++) {
        memset(sum, 0, G.gw * SUM_FIELDS * sizeof(uint32_t));
        for (int r = 0; r < G.sch; r++) {
            JSAMPROW rp = rowbuf;
            jpeg_read_scanlines(ci, &rp, 1);
            for (int cx = 0; cx < G.gw; cx++) {
                uint32_t *cell_sum = &sum[cx * SUM_FIELDS];
                const uint8_t *pixel = rowbuf + (size_t)cx * G.scw * 3;
                for (int i = 0; i < G.scw; i++, pixel += 3) {
                    uint32_t red = pixel[0], green = pixel[1], blue = pixel[2];
                    uint32_t luminosity = (77 * red + 150 * green + 29 * blue) >> 8;
                    if (luminosity >= 0x7f) {
                        cell_sum[SUM_FOREGROUND_R] += red;
                        cell_sum[SUM_FOREGROUND_G] += green;
                        cell_sum[SUM_FOREGROUND_B] += blue;
                        cell_sum[SUM_FOREGROUND_COUNT]++;
                    } else {
                        cell_sum[SUM_BACKGROUND_R] += red;
                        cell_sum[SUM_BACKGROUND_G] += green;
                        cell_sum[SUM_BACKGROUND_B] += blue;
                        cell_sum[SUM_BACKGROUND_COUNT]++;
                    }
                    cell_sum[SUM_LUMINOSITY] += luminosity;
                }
            }
        }
        for (int cx = 0; cx < G.gw; cx++) {
            int out_cx = G.hflip ? G.gw - 1 - cx : cx;
            cell_t *c = &out[cy * G.gw + out_cx];
            uint32_t *cell_sum = &sum[cx * SUM_FIELDS];
            uint32_t foreground_count = cell_sum[SUM_FOREGROUND_COUNT];
            uint32_t background_count = cell_sum[SUM_BACKGROUND_COUNT];
            /* A cell with only one kind of pixel falls back to black/white for the
             * other, so the glyph stays visible instead of blending into one flat
             * color. */
            if (foreground_count) {
                c->foreground_r = cell_sum[SUM_FOREGROUND_R] / foreground_count;
                c->foreground_g = cell_sum[SUM_FOREGROUND_G] / foreground_count;
                c->foreground_b = cell_sum[SUM_FOREGROUND_B] / foreground_count;
            } else {
                c->foreground_r = c->foreground_g = c->foreground_b = 255;
            }
            if (background_count) {
                c->background_r = cell_sum[SUM_BACKGROUND_R] / background_count;
                c->background_g = cell_sum[SUM_BACKGROUND_G] / background_count;
                c->background_b = cell_sum[SUM_BACKGROUND_B] / background_count;
            } else {
                c->background_r = c->background_g = c->background_b = 0;
            }
            c->luminosity = cell_sum[SUM_LUMINOSITY] / pixels_per_cell;
        }
    }
    jpeg_abort_decompress(ci);   /* skip unread scanlines, keep object reusable */
    return 0;
}

typedef struct {
    struct jpeg_decompress_struct ci;
    struct err_mgr je;
    uint32_t *sum;
    uint8_t *rowbuf;
} cell_ctx_t;

static void *create_context(void) {
    cell_ctx_t *ctx = calloc(1, sizeof *ctx);
    ctx->ci.err = jpeg_std_error(&ctx->je.pub);
    ctx->je.pub.error_exit = on_error; ctx->je.pub.emit_message = on_msg;
    jpeg_create_decompress(&ctx->ci);
    ctx->sum = malloc(G.gw * SUM_FIELDS * sizeof(uint32_t));
    ctx->rowbuf = malloc(G.capw * 3 + 64);
    return ctx;
}

static void destroy_context(void *context) {
    cell_ctx_t *ctx = context;
    jpeg_destroy_decompress(&ctx->ci);
    free(ctx->sum); free(ctx->rowbuf); free(ctx);
}

static int analyze(void *context, const uint8_t *data, size_t len, void *out) {
    cell_ctx_t *ctx = context;
    return process(&ctx->ci, &ctx->je, data, len, (cell_t *)out, ctx->rowbuf, ctx->sum);
}

const frame_analyzer_t cell_analyzer = {
    .name = "cell_averaging",
    .result_cell_size = sizeof(cell_t),
    .create_context = create_context,
    .destroy_context = destroy_context,
    .analyze = analyze,
};
