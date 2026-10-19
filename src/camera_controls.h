#ifndef CELLAVG_CAMERA_CONTROLS_H
#define CELLAVG_CAMERA_CONTROLS_H

#include <stdio.h>
#include <linux/videodev2.h>

typedef struct {
    struct v4l2_queryctrl query;
} camera_control_info_t;

/* Returns the canonical parameter name, or NULL for an unknown name. */
const char *camera_control_name(const char *name);

/* Query, read, write, and report standard V4L2 camera controls on fd.
 * These functions return 0 on success and -1 after reporting errors to stderr. */
int camera_controls_query(int fd, const char *name, camera_control_info_t *info);
int camera_controls_get(int fd, const char *name, int *value);
int camera_controls_validate(int fd, const char *name, int value);
int camera_controls_set(int fd, const char *name, int value);
int camera_controls_print_value(int fd, const char *name, FILE *out);
int camera_controls_print_range(int fd, const char *name, FILE *out);
int camera_controls_list(int fd, FILE *out);

#endif
