#include "config.h"
#include "slic_analyzer.h"
#include "slic_visualizer.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <jpeglib.h>

static std::vector<uint8_t> make_test_frame(int width, int height) {
    jpeg_compress_struct compressor = {};
    jpeg_error_mgr error = {};
    compressor.err = jpeg_std_error(&error);
    jpeg_create_compress(&compressor);

    unsigned char *encoded = NULL;
    unsigned long encoded_size = 0;
    jpeg_mem_dest(&compressor, &encoded, &encoded_size);
    compressor.image_width = width;
    compressor.image_height = height;
    compressor.input_components = 3;
    compressor.in_color_space = JCS_RGB;
    jpeg_set_defaults(&compressor);
    jpeg_set_quality(&compressor, 100, TRUE);
    jpeg_start_compress(&compressor, TRUE);

    std::vector<uint8_t> row((size_t)width * 3);
    while (compressor.next_scanline < compressor.image_height) {
        for (int x = 0; x < width; x++) {
            bool red_side = x < width / 2;
            row[(size_t)x * 3] = red_side ? 240 : 0;
            row[(size_t)x * 3 + 1] = 0;
            row[(size_t)x * 3 + 2] = red_side ? 0 : 240;
        }
        JSAMPROW scanline = row.data();
        jpeg_write_scanlines(&compressor, &scanline, 1);
    }
    jpeg_finish_compress(&compressor);
    jpeg_destroy_compress(&compressor);

    std::vector<uint8_t> result(encoded, encoded + encoded_size);
    free(encoded);
    return result;
}

static const slic_pixel_t &pixel_at(const std::vector<slic_pixel_t> &pixels,
                                    int width, int x, int y) {
    return pixels[(size_t)y * width + x];
}

int main(void) {
    G.gw = 64;
    G.gh = 48;
    G.slic_cluster_count = 64;
    G.slic_iterations = 3;
    G.slic_sample_stride = 2;
    G.slic_compactness = 10;
    G.slic_downscale = 1;
    G.hflip = 0;

    std::vector<uint8_t> frame = make_test_frame(G.gw, G.gh);
    std::vector<slic_pixel_t> result((size_t)G.gw * G.gh);
    void *context = slic_analyzer.create_context();
    assert(context);
    assert(slic_analyzer.analyze(context, frame.data(), frame.size(), result.data()) == 0);

    for (const slic_pixel_t &pixel : result)
        assert(pixel.label < G.slic_cluster_count);
    assert(pixel_at(result, G.gw, 8, G.gh / 2).red >
           pixel_at(result, G.gw, 8, G.gh / 2).blue * 2);
    assert(pixel_at(result, G.gw, G.gw - 9, G.gh / 2).blue >
           pixel_at(result, G.gw, G.gw - 9, G.gh / 2).red * 2);

    G.hflip = 1;
    assert(slic_analyzer.analyze(context, frame.data(), frame.size(), result.data()) == 0);
    assert(pixel_at(result, G.gw, 8, G.gh / 2).blue >
           pixel_at(result, G.gw, 8, G.gh / 2).red * 2);
    assert(pixel_at(result, G.gw, G.gw - 9, G.gh / 2).red >
           pixel_at(result, G.gw, G.gw - 9, G.gh / 2).blue * 2);

    slic_analyzer.destroy_context(context);

    slic_pixel_t render_source[4] = {
        { 0, 240, 0, 0 }, { 1, 0, 0, 240 },
        { 0, 240, 0, 0 }, { 1, 0, 0, 240 }
    };
    uint32_t rendered[16] = {};
    assert(slic_visualizer.init(2, 2, 4, 4) == 0);
    slic_visualizer.render(render_source, reinterpret_cast<uint8_t *>(rendered), 4 * 4);
    for (int y = 0; y < 4; y++) {
        assert(rendered[y * 4] == 0x00f00000u);
        assert(rendered[y * 4 + 1] == 0x00f00000u);
        assert(rendered[y * 4 + 2] == 0x00101010u);
        assert(rendered[y * 4 + 3] == 0x00101010u);
    }
    slic_visualizer.shutdown();

    puts("SLIC analyzer and renderer tests passed");
    return 0;
}
