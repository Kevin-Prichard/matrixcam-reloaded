#ifndef CELLAVG_CAPTURE_H
#define CELLAVG_CAPTURE_H
#include "camera_controls.h"
#include <stdio.h>

/* Opens and streams the configured V4L2 device (G.vdev/G.capw/G.caph/
 * G.focus_value), adjusting G.capw/G.caph to the format the driver actually
 * granted. Exits the process on failure. */
void capture_init(void);

/* Reporting mode opens the camera controls without starting frame capture.
 * Returns 0 after printing to stdout, or -1 after reporting an error. */
int capture_report_camera_params(void);

/* Runtime camera-control interface; use after capture_init(). */
int capture_camera_control_query(const char *name, camera_control_info_t *info);
int capture_camera_control_get(const char *name, int *value);
int capture_camera_control_set(const char *name, int value);
int capture_camera_controls_list(FILE *out);
int capture_camera_control_print_range(const char *name, FILE *out);

/* Producer thread entry point: polls the V4L2 device, forwards each
 * successfully captured MJPEG frame to pipeline_submit_frame(), and
 * transparently reinitializes the device if frames stall or a hard device
 * error (e.g. a disconnected USB cable) occurs. Runs until `running` is
 * cleared. */
void *capture_thread_main(void *arg);

/* Stops streaming and releases V4L2 resources. */
void capture_shutdown(void);

#endif
