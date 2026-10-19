#ifndef CELLAVG_CAPTURE_H
#define CELLAVG_CAPTURE_H

/* Opens and streams the configured V4L2 device (G.vdev/G.capw/G.caph/
 * G.focus_value), adjusting G.capw/G.caph to the format the driver actually
 * granted. Exits the process on failure. */
void capture_init(void);

/* Producer thread entry point: polls the V4L2 device, forwards each
 * successfully captured MJPEG frame to pipeline_submit_frame(), and
 * transparently reinitializes the device if frames stall or a hard device
 * error (e.g. a disconnected USB cable) occurs. Runs until `running` is
 * cleared. */
void *capture_thread_main(void *arg);

/* Stops streaming and releases V4L2 resources. */
void capture_shutdown(void);

#endif
