/* ---------- V4L2 capture ---------- */
#define _GNU_SOURCE
#include "capture.h"
#include "common.h"
#include "config.h"
#include "pipeline.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

static int vfd;
static void *vmap[4]; static size_t vmaplen[4]; static unsigned vmapcount;
#define CAMERA_STALL_TIMEOUT_US 3000000
static void v4l2_reinit(void);

/* Applies the -F focus setting: disables autofocus and locks to a clamped
 * manual focus_absolute value, or (when -F was not given) re-enables
 * autofocus. Warns but does not fail if the camera lacks focus controls,
 * since not all UVC cameras expose them. */
static void apply_focus_control(int fd) {
    if (G.focus_value >= 0) {
        struct v4l2_queryctrl q; memset(&q, 0, sizeof q);
        q.id = V4L2_CID_FOCUS_ABSOLUTE;
        int has_manual_focus = ioctl(fd, VIDIOC_QUERYCTRL, &q) == 0;
        int value = G.focus_value;
        if (has_manual_focus) {
            if (value < q.minimum) value = q.minimum;
            if (value > q.maximum) value = q.maximum;
        }
        struct v4l2_control c = { .id = V4L2_CID_FOCUS_AUTO, .value = 0 };
        ioctl(fd, VIDIOC_S_CTRL, &c); /* best effort: disable autofocus */
        if (!has_manual_focus) {
            fprintf(stderr, "warning: camera has no focus_absolute control; -F ignored\n");
            return;
        }
        c.id = V4L2_CID_FOCUS_ABSOLUTE; c.value = value;
        if (ioctl(fd, VIDIOC_S_CTRL, &c) < 0)
            fprintf(stderr, "warning: could not set focus_absolute=%d: %s\n", value, strerror(errno));
        else if (value != G.focus_value)
            fprintf(stderr, "focus_absolute %d out of range [%d,%d]; clamped to %d\n",
                    G.focus_value, q.minimum, q.maximum, value);
    } else {
        struct v4l2_control c = { .id = V4L2_CID_FOCUS_AUTO, .value = 1 };
        ioctl(fd, VIDIOC_S_CTRL, &c); /* best effort: restore autofocus */
    }
}

/* Opens and streams on the capture device without exiting the process;
 * returns 0 on success or -1 on failure so callers can retry. */
static int v4l2_configure(void) {
    vfd = open(G.vdev, O_RDWR | O_NONBLOCK);
    if (vfd < 0) { fprintf(stderr, "%s: %s\n", G.vdev, strerror(errno)); return -1; }
    struct v4l2_format f; memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = G.capw; f.fmt.pix.height = G.caph;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG; f.fmt.pix.field = V4L2_FIELD_ANY;
    if (ioctl(vfd, VIDIOC_S_FMT, &f) < 0 || f.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        fprintf(stderr, "MJPEG not available at %dx%d\n", G.capw, G.caph);
        close(vfd); vfd = -1;
        return -1;
    }
    G.capw = f.fmt.pix.width; G.caph = f.fmt.pix.height;
    apply_focus_control(vfd);

    struct v4l2_requestbuffers rb; memset(&rb, 0, sizeof rb);
    rb.count = 4; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(vfd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
        fprintf(stderr, "REQBUFS: %s\n", strerror(errno));
        close(vfd); vfd = -1;
        return -1;
    }
    vmapcount = 0;
    for (unsigned i = 0; i < rb.count && i < 4; i++) {
        struct v4l2_buffer b; memset(&b, 0, sizeof b);
        b.type = rb.type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        ioctl(vfd, VIDIOC_QUERYBUF, &b);
        vmaplen[i] = b.length;
        vmap[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, b.m.offset);
        if (vmap[i] == MAP_FAILED) {
            fprintf(stderr, "mmap: %s\n", strerror(errno));
            for (unsigned j = 0; j < i; j++) munmap(vmap[j], vmaplen[j]);
            close(vfd); vfd = -1;
            return -1;
        }
        vmapcount = i + 1;
        ioctl(vfd, VIDIOC_QBUF, &b);
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(vfd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "STREAMON: %s\n", strerror(errno));
        for (unsigned j = 0; j < vmapcount; j++) munmap(vmap[j], vmaplen[j]);
        close(vfd); vfd = -1;
        return -1;
    }
    return 0;
}

/* Stops streaming and releases the device so it can be reopened. */
static void v4l2_teardown(void) {
    if (vfd < 0) return;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(vfd, VIDIOC_STREAMOFF, &type);
    for (unsigned i = 0; i < vmapcount; i++) munmap(vmap[i], vmaplen[i]);
    vmapcount = 0;
    close(vfd);
    vfd = -1;
}

/* Tears down and retries v4l2_configure() until it succeeds or shutdown is
 * requested, used to recover from a disconnected/stalled camera. */
static void v4l2_reinit(void) {
    v4l2_teardown();
    while (running) {
        if (v4l2_configure() == 0) {
            fprintf(stderr, "camera %s reconnected\n", G.vdev);
            return;
        }
        for (int slept = 0; slept < 10 && running; slept++) usleep(100000);
    }
}

void capture_init(void) {
    if (v4l2_configure() < 0) exit(1);
}

void *capture_thread_main(void *arg) {
    (void)arg;
    uint64_t last_frame_us = now_us();
    while (running) {
        struct pollfd pfd = { vfd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0) {
            if (now_us() - last_frame_us >= CAMERA_STALL_TIMEOUT_US) {
                fprintf(stderr, "camera stalled (no frames for %.1fs); reinitializing %s\n",
                        CAMERA_STALL_TIMEOUT_US / 1e6, G.vdev);
                v4l2_reinit();
                last_frame_us = now_us();
            }
            continue;
        }
        struct v4l2_buffer b; memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
        if (ioctl(vfd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) continue;
            fprintf(stderr, "camera error (%s); reinitializing %s\n", strerror(errno), G.vdev);
            v4l2_reinit();
            last_frame_us = now_us();
            continue;
        }
        if (b.bytesused && !(b.flags & V4L2_BUF_FLAG_ERROR)) {
            last_frame_us = now_us();
            pipeline_submit_frame(vmap[b.index], b.bytesused);
        }
        ioctl(vfd, VIDIOC_QBUF, &b);
    }
    return NULL;
}

void capture_shutdown(void) {
    v4l2_teardown();
}
