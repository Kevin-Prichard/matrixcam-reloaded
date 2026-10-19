#ifndef CELLAVG_DISPLAY_H
#define CELLAVG_DISPLAY_H
#include <stdint.h>
#include <stddef.h>
#include <xf86drmMode.h>

typedef struct { uint32_t handle, fb, pitch; size_t size; uint8_t *map; } display_buffer_t;

/* Opens G.ddev, picks a connected connector/mode near G.outw x G.outh (or
 * the preferred mode if that exact size isn't offered), and allocates two
 * dumb-buffer scanout framebuffers for double-buffered presentation. Exits
 * the process on failure. */
void display_init(void);

/* Returns the active display mode (resolution, refresh, etc.) chosen by
 * display_init(). */
const drmModeModeInfo *display_mode(void);

/* Returns one of the two framebuffers (index 0 or 1) for the caller to
 * render into before calling display_present(). */
display_buffer_t *display_buffer(int index);

/* Flips to the given buffer index and blocks (polling the DRM fd) until the
 * page flip completes or the process is shutting down. Returns 0 on
 * success, -1 if the page flip could not be scheduled. */
int display_present(int index);

/* Restores the original CRTC mode and releases DRM resources. */
void display_shutdown(void);

#endif
