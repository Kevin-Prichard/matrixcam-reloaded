#include "joydiv_analyzer.h"
#include "joydiv_visualizer.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>

config_t G;

static unsigned char *make_jpeg(int w, int h, unsigned long *size) {
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr e;
    c.err = jpeg_std_error(&e);
    jpeg_create_compress(&c);
    unsigned char *buf = NULL;
    jpeg_mem_dest(&c, &buf, size);
    c.image_width = w; c.image_height = h; c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_start_compress(&c, TRUE);
    unsigned char *row = malloc((size_t)w * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            row[x*3+0] = (unsigned char)(x * 255 / (w - 1));
            row[x*3+1] = (unsigned char)(y * 255 / (h - 1));
            row[x*3+2] = (unsigned char)(128);
        }
        JSAMPROW rp = row;
        jpeg_write_scanlines(&c, &rp, 1);
    }
    free(row);
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    return buf;
}

int main(void) {
    memset(&G, 0, sizeof G);
    G.capw = 16; G.caph = 8;
    G.hflip = 0;
    G.joydiv_grouprows = 2;
    G.joydiv_alpha = 255;
    G.joydiv_background = JOYDIV_BACKGROUND_COLOR;
    G.joydiv_background_color = 0;
    G.joydiv_zone_count = 1;
    G.joydiv_zones = calloc(1, sizeof(*G.joydiv_zones));
    G.joydiv_zones[0] = (joydiv_zone_t){0,0,16,8,JOYDIV_YVALUE_LUM,JOYDIV_LINES_STRAIGHT,0,0.5,8.0};
    if (joydiv_analyzer_configure(32, 16) < 0) return 1;
    unsigned long jpeg_size = 0;
    unsigned char *jpeg = make_jpeg(16, 8, &jpeg_size);
    if (!jpeg) return 1;
    void *ctx = joydiv_analyzer.create_context();
    if (!ctx) return 1;
    unsigned char *result = calloc(1, joydiv_analyzer.result_cell_size);
    if (!result) return 1;
    if (joydiv_analyzer.analyze(ctx, jpeg, jpeg_size, result) < 0) return 1;
    joydiv_result_header_t *hdr = (void *)result;
    if (hdr->magic != JOYDIV_RESULT_MAGIC || hdr->zone_count != 1 || hdr->grouprows != 2) return 1;
    joydiv_zone_result_t *z = (void *)(result + hdr->zone_offset);
    if (z->group_count != 4 || z->channel_count != 1) return 1;
    const unsigned char *data = result + z->data_offset;
    int nonzero = 0;
    for (int i = 0; i < z->group_count * z->w; i++) if (data[i]) nonzero++;
    if (!nonzero) return 1;
    if (joydiv_visualizer.init(1, 1, 32, 16) < 0) return 1;
    unsigned char *fb = calloc(32 * 16, 4);
    if (!fb) return 1;
    joydiv_visualizer.render(result, fb, 32 * 4);
    int lit = 0;
    for (int i = 0; i < 32 * 16; i++) if (((uint32_t *)fb)[i] != 0) lit++;
    if (!lit) return 1;
    joydiv_visualizer.shutdown();
    joydiv_analyzer.destroy_context(ctx);
    free(fb); free(result); free(jpeg); free(G.joydiv_zones);
    puts("joydiv analyzer/visualizer smoke test passed");
    return 0;
}
