#include "slic_analyzer.h"
#include "config.h"
#include "context.h"
#include "fast-slic-common.h"
#include "parallel.h"

#include <stdio.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <exception>
#include <memory>
#include <vector>

struct color_sum {
    uint64_t red, green, blue, count;
};

struct slic_jpeg_error {
    jpeg_error_mgr manager;
    jmp_buf jump;
};

static double now_ms(void) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

struct slic_context {
    double t_decode = 0, t_init = 0, t_iter = 0, t_out = 0;
    int frames = 0;
    bool timing = false;
    std::vector<slic_pixel_t> averages;
    jpeg_decompress_struct decoder;
    slic_jpeg_error error;
    bool decoder_created;
    std::vector<uint8_t> rgb;
    std::vector<Cluster> clusters;
    std::vector<uint16_t> assignments;
    std::vector<color_sum> palette;
    std::unique_ptr<fslic::Context> slic;
};

static void jpeg_failure(j_common_ptr common) {
    slic_jpeg_error *error = reinterpret_cast<slic_jpeg_error *>(common->err);
    longjmp(error->jump, 1);
}

static void jpeg_message(j_common_ptr common, int level) {
    (void)common;
    (void)level;
}

static void destroy_context(void *opaque);

static void *create_context(void) {
    slic_context *context = new (std::nothrow) slic_context{};
    if (!context) {
        perror("SLIC JPEG context");
        return NULL;
    }
    context->decoder.err = jpeg_std_error(&context->error.manager);
    context->error.manager.error_exit = jpeg_failure;
    context->error.manager.emit_message = jpeg_message;
    jpeg_create_decompress(&context->decoder);
    context->decoder_created = true;
    try {
        const size_t pixel_count = (size_t)G.gw * G.gh;
        context->rgb.resize(pixel_count * 3);
        context->clusters.resize((size_t)G.slic_cluster_count);
        context->assignments.resize(pixel_count);
        context->palette.resize((size_t)G.slic_cluster_count);

        context->timing = getenv("SLIC_TIMING") != NULL;
        context->averages.resize((size_t)G.slic_cluster_count);
        fslic::ContextBuilder builder;
#if defined(USE_AVX2)
        builder.set_arch("x64/avx2");
#elif defined(USE_NEON)
        builder.set_arch("arm/neon");
#endif
        if (!builder.is_supported_arch()) {
            fprintf(stderr, "fast-slic was built without the selected SIMD implementation\n");
            destroy_context(context);
            return NULL;
        }
        context->slic.reset(builder.build(G.gh, G.gw, G.slic_cluster_count,
                                          context->rgb.data(), context->clusters.data()));
        if (!context->slic) {
            fprintf(stderr, "fast-slic could not create a processing context\n");
            destroy_context(context);
            return NULL;
        }
        context->slic->num_threads = 4;
        if (context->timing)
            fprintf(stderr, "slic arch=%s\n",
#if defined(USE_AVX2)
                    "x64/avx2"
#elif defined(USE_NEON)
                    "arm/neon"
#else
                    "scalar"
#endif
            );
        context->slic->compactness = (float)G.slic_compactness;
        context->slic->min_size_factor = 0.1f;
        context->slic->subsample_stride_config = (int16_t)G.slic_sample_stride;
    } catch (const std::exception &error) {
        fprintf(stderr, "could not initialize fast-slic: %s\n", error.what());
        destroy_context(context);
        return NULL;
    }
    return context;
}

static void destroy_context(void *opaque) {
    slic_context *context = static_cast<slic_context *>(opaque);
    if (!context) return;
    if (context->decoder_created) jpeg_destroy_decompress(&context->decoder);
    delete context;
}

static int decode_rgb(slic_context *context, const uint8_t *data, size_t length,
                      uint8_t *rgb) {
    jpeg_decompress_struct *decoder = &context->decoder;
    if (setjmp(context->error.jump)) {
        jpeg_abort_decompress(decoder);
        return -1;
    }

    jpeg_mem_src(decoder, data, length);
    jpeg_read_header(decoder, TRUE);
    decoder->scale_num = 1;
    decoder->scale_denom = 1;
    decoder->out_color_space = JCS_RGB;
    decoder->dct_method = JDCT_IFAST;
    decoder->do_fancy_upsampling = FALSE;
    jpeg_start_decompress(decoder);
    if (decoder->output_components != 3 ||
        decoder->output_width != (JDIMENSION)G.gw ||
        decoder->output_height != (JDIMENSION)G.gh) {
        jpeg_abort_decompress(decoder);
        return -1;
    }

    JSAMPARRAY row = (*decoder->mem->alloc_sarray)(
        reinterpret_cast<j_common_ptr>(decoder), JPOOL_IMAGE,
        decoder->output_width * decoder->output_components, 1);
    while (decoder->output_scanline < decoder->output_height) {
        JDIMENSION source_y = decoder->output_scanline;
        if (jpeg_read_scanlines(decoder, row, 1) != 1) {
            jpeg_abort_decompress(decoder);
            return -1;
        }
        uint8_t *destination = rgb + (size_t)source_y * G.gw * 3;
        if (G.hflip) {
            for (int x = 0; x < G.gw; x++)
                memcpy(destination + (size_t)(G.gw - 1 - x) * 3,
                       row[0] + (size_t)x * 3, 3);
        } else {
            memcpy(destination, row[0], (size_t)G.gw * 3);
        }
    }
    jpeg_abort_decompress(decoder);
    return 0;
}

static int analyze(void *opaque, const uint8_t *data, size_t length, void *out) {
    slic_context *context = static_cast<slic_context *>(opaque);
    const int width = G.gw, height = G.gh, clusters_count = G.slic_cluster_count;
    const size_t pixel_count = (size_t)width * height;
    try {
        double t0 = now_ms();
        if (decode_rgb(context, data, length, context->rgb.data()) < 0) return -1;
        double t1 = now_ms();
        context->slic->initialize_clusters();
        double t2 = now_ms();
        context->slic->iterate(context->assignments.data(), G.slic_iterations);
        double t3 = now_ms();
        std::fill(context->palette.begin(), context->palette.end(), color_sum{0, 0, 0, 0});
        for (size_t i = 0; i < pixel_count; i++) {
            uint16_t label = context->assignments[i];
            if (label >= clusters_count) {
                fprintf(stderr, "fast-slic produced an invalid cluster label\n");
                return -1;
            }
            color_sum &sum = context->palette[label];
            sum.red += context->rgb[i * 3];
            sum.green += context->rgb[i * 3 + 1];
            sum.blue += context->rgb[i * 3 + 2];
            sum.count++;
        }
        for (int c = 0; c < clusters_count; c++) {
            const color_sum &sum = context->palette[c];
            uint64_t n = sum.count ? sum.count : 1;
            context->averages[c] = {(uint16_t)c, (uint8_t)(sum.red / n),
                                    (uint8_t)(sum.green / n), (uint8_t)(sum.blue / n)};
        }
        slic_pixel_t *pixels = static_cast<slic_pixel_t *>(out);
        for (size_t i = 0; i < pixel_count; i++)
            pixels[i] = context->averages[context->assignments[i]];
        if (context->timing) {
            double t4 = now_ms();
            context->t_decode += t1 - t0;
            context->t_init += t2 - t1;
            context->t_iter += t3 - t2;
            context->t_out += t4 - t3;
            if (++context->frames == 20) {
                fprintf(stderr, "slic ms/frame: decode %.1f init %.1f iterate %.1f out %.1f\n",
                        context->t_decode / 20, context->t_init / 20,
                        context->t_iter / 20, context->t_out / 20);
                context->t_decode = context->t_init = context->t_iter = context->t_out = 0;
                context->frames = 0;
            }
        }
    } catch (const std::exception &error) {
        fprintf(stderr, "fast-slic failed: %s\n", error.what());
        return -1;
    }
    return 0;
}

extern "C" const frame_analyzer_t slic_analyzer = {
    "slic",
    sizeof(slic_pixel_t),
    create_context,
    destroy_context,
    analyze
};
