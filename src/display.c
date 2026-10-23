/* ---------- DRM/KMS display output ---------- */
#include "display.h"
#include "common.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static int dfd; static uint32_t crtc_id, conn_id; static drmModeModeInfo mode;
static drmModeCrtc *saved_crtc;
static display_buffer_t bufs[2];

static void on_flip(int fd, unsigned a, unsigned b, unsigned c, void *d) {
    (void)fd; (void)a; (void)b; (void)c; *(int *)d = 0;
}

void display_init(void) {
    dfd = open(G.ddev, O_RDWR | O_CLOEXEC);
    if (dfd < 0) { perror(G.ddev); exit(1); }
    drmModeRes *res = drmModeGetResources(dfd);
    drmModeConnector *conn = NULL;
    for (int i = 0; res && i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(dfd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) { conn = c; break; }
        drmModeFreeConnector(c);
    }
    if (!conn) { fprintf(stderr, "no connected display on %s\n", G.ddev); if (res) drmModeFreeResources(res); close(dfd); exit(1); }
    conn_id = conn->connector_id;

    int pick = -1;
    for (int i = 0; i < conn->count_modes; i++)
        if (conn->modes[i].hdisplay == G.outw && conn->modes[i].vdisplay == G.outh) { pick = i; break; }
    if (pick < 0) {
        fprintf(stderr, "requested %dx%d not offered on %s\n", G.outw, G.outh, G.ddev);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close(dfd);
        exit(1);
    }
    mode = conn->modes[pick];

    drmModeEncoder *enc = conn->encoder_id ? drmModeGetEncoder(dfd, conn->encoder_id) : NULL;
    if (enc && enc->crtc_id) crtc_id = enc->crtc_id;
    else for (int i = 0; i < conn->count_encoders && !crtc_id; i++) {
        drmModeEncoder *e = drmModeGetEncoder(dfd, conn->encoders[i]);
        for (int c = 0; e && c < res->count_crtcs; c++)
            if (e->possible_crtcs & (1u << c)) { crtc_id = res->crtcs[c]; break; }
    }
    if (!crtc_id) { fprintf(stderr, "no CRTC\n"); exit(1); }
    saved_crtc = drmModeGetCrtc(dfd, crtc_id);

    for (int i = 0; i < 2; i++) {
        struct drm_mode_create_dumb cr = { .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32 };
        if (drmIoctl(dfd, DRM_IOCTL_MODE_CREATE_DUMB, &cr)) { perror("create dumb"); exit(1); }
        bufs[i].handle = cr.handle; bufs[i].pitch = cr.pitch; bufs[i].size = cr.size;
        if (drmModeAddFB(dfd, mode.hdisplay, mode.vdisplay, 24, 32, cr.pitch, cr.handle, &bufs[i].fb)) {
            perror("addfb"); exit(1);
        }
        struct drm_mode_map_dumb mr = { .handle = cr.handle };
        if (drmIoctl(dfd, DRM_IOCTL_MODE_MAP_DUMB, &mr)) { perror("map dumb"); exit(1); }
        bufs[i].map = mmap(NULL, cr.size, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, mr.offset);
        if (bufs[i].map == MAP_FAILED) { perror("mmap fb"); exit(1); }
        memset(bufs[i].map, 0, cr.size);
    }
    if (drmModeSetCrtc(dfd, crtc_id, bufs[0].fb, 0, 0, &conn_id, 1, &mode)) {
        perror("SetCrtc (are you DRM master? run from a text console)"); exit(1);
    }
    printf("display %ux%u pitch=%u size=%zu\n", mode.hdisplay, mode.vdisplay, bufs[0].pitch, bufs[0].size);
}

const drmModeModeInfo *display_mode(void) { return &mode; }

display_buffer_t *display_buffer(int index) { return &bufs[index]; }

int display_present(int index) {
    int pending = 1;
    drmEventContext ev = { .version = 2, .page_flip_handler = on_flip };
    /* ensure dirty */
    /* skip dirtyfb */
    if (drmModePageFlip(dfd, crtc_id, bufs[index].fb, DRM_MODE_PAGE_FLIP_EVENT, &pending) != 0) {
        fprintf(stderr, "disp_flip: drmModePageFlip failed index=%d: %s\n", index, strerror(errno));
        return -1;
    }
    while (pending && running) {
        struct pollfd p = { dfd, POLLIN, 0 };
        int pr = poll(&p, 1, 1000);
        if (pr > 0) {
            if (drmHandleEvent(dfd, &ev) < 0) {
                fprintf(stderr, "disp_flip: drmHandleEvent failed index=%d\n", index);
                break;
            }
        } else if (pr == 0) {
            fprintf(stderr, "disp_flip: timeout index=%d\n", index);
            break;
        } else {
            fprintf(stderr, "disp_flip: poll error index=%d: %s\n", index, strerror(errno));
            break;
        }
    }
    if (pending) {
        fprintf(stderr, "disp_flip: not completed index=%d\n", index);
        return -1;
    }
    return 0;
}

void display_shutdown(void) {
    if (saved_crtc)
        drmModeSetCrtc(dfd, saved_crtc->crtc_id, saved_crtc->buffer_id, saved_crtc->x,
                       saved_crtc->y, &conn_id, 1, &saved_crtc->mode);
}
