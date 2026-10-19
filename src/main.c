/* cellavg - MJPEG webcam -> per-cell averages -> DRM/KMS (demo)
 *
 * This file wires together the capture / analysis / visualization / display
 * pipeline modules. See each module header for its responsibilities:
 *   config.h           CLI parsing and shared tunables (G)
 *   capture.h           V4L2 webcam frame source
 *   pipeline.h          generic capture -> analyzer worker pool + stats
 *   cell_analyzer.h     concrete frame_analyzer_t: JPEG decode + cell averaging
 *   glyph_atlas.h       FreeType glyph bitmap atlas
 *   visualizer.h        abstract analysis-grid -> framebuffer interface
 *   glyph_visualizer.h  concrete visualizer_t: colored glyph rendering
 *   display.h           DRM/KMS scanout + page flipping
 */
#define _GNU_SOURCE
#include "common.h"
#include "config.h"
#include "capture.h"
#include "pipeline.h"
#include "cell_analyzer.h"
#include "glyph_atlas.h"
#include "glyph_visualizer.h"
#include "display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

int main(int argc, char **argv) {
    config_parse_args(argc, argv);
    install_signal_handlers();

    capture_init();
    config_derive_grid();
    printf("capture %dx%d MJPEG, workers %d\n", G.capw, G.caph, G.nworkers);

    glyph_atlas_init();
    display_init();
    const drmModeModeInfo *mode = display_mode();
    const visualizer_t *vis = &glyph_visualizer;
    if (vis->init(G.gw, G.gh, mode->hdisplay, mode->vdisplay) < 0) {
        display_shutdown();
        glyph_atlas_shutdown();
        return 1;
    }
    printf("grid %dx%d cells (%dx%d px each, jpeg scale 1/%d)\n",
           G.gw, G.gh, G.cw, G.ch, G.denom);

    if (pipeline_start(&cell_analyzer, G.nworkers, G.nworkers + 2, G.gw, G.gh) < 0) {
        vis->shutdown();
        display_shutdown();
        glyph_atlas_shutdown();
        return 1;
    }

    pthread_t cap_thread;
    pthread_create(&cap_thread, NULL, capture_thread_main, NULL);

    cell_t *local = malloc((size_t)G.gw * G.gh * sizeof(cell_t));
    uint64_t shown_seq = 0, t_last = now_us();
    int front = 0;

    while (running) {
        uint64_t seq = pipeline_wait_latest(local, shown_seq);
        int fresh = seq != shown_seq;
        if (fresh) shown_seq = seq;

        if (fresh) {
            int back = 1 - front;
            display_buffer_t *buf = display_buffer(back);
            vis->render(local, buf->map, buf->pitch);
            if (display_present(back) == 0) front = back;
            pipeline_note_shown();
        }

        uint64_t now = now_us();
        if (now - t_last >= 5000000) {
            pipeline_stats_t s = pipeline_take_stats();
            double secs = (now - t_last) / 1e6;
            double ms = s.proc ? s.proc_us / (double)s.proc / 1000.0 : 0;
            time_t wall_now = time(NULL);
            struct tm tm_now;
            localtime_r(&wall_now, &tm_now);
            char timestamp[32];
            strftime(timestamp, sizeof timestamp, "%Y-%m-%d %H:%M:%S", &tm_now);
            printf("%s [%.1fs] camera %.1f fps (dropped %llu, bad %llu) | decode+avg %.2f ms/frame "
                   "(~%.0f fps capacity) | mjpeg %.1f KB/frame | display %.1f fps\n",
                   timestamp, secs, s.cap / secs, (unsigned long long)s.drop,
                   (unsigned long long)s.bad, ms,
                   ms > 0 ? G.nworkers * 1000.0 / ms : 0.0,
                   s.cap ? s.bytes / (double)s.cap / 1024.0 : 0.0, s.shown / secs);
            fflush(stdout);
            t_last = now;
        }
    }

    pthread_join(cap_thread, NULL);
    pipeline_stop();
    vis->shutdown();
    capture_shutdown();
    display_shutdown();
    glyph_atlas_shutdown();
    free(local);
    return 0;
}
