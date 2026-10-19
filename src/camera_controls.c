#define _GNU_SOURCE
#include "camera_controls.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>

typedef struct {
    const char *name;
    const char *label;
    uint32_t id;
} camera_control_def_t;

static const camera_control_def_t controls[] = {
    { "autoexp", "Auto Exposure", V4L2_CID_EXPOSURE_AUTO },
    { "backlight", "Backlight Compensation", V4L2_CID_BACKLIGHT_COMPENSATION },
    { "brightness", "Brightness", V4L2_CID_BRIGHTNESS },
    { "contrast", "Contrast", V4L2_CID_CONTRAST },
    { "exp_dyn", "Exposure, Dynamic Framerate", V4L2_CID_EXPOSURE_AUTO_PRIORITY },
    { "exp_abs", "Exposure Time, Absolute", V4L2_CID_EXPOSURE_ABSOLUTE },
    { "gain", "Gain", V4L2_CID_GAIN },
    { "gamma", "Gamma", V4L2_CID_GAMMA },
    { "hue", "Hue", V4L2_CID_HUE },
    { "power", "Power Line Frequency", V4L2_CID_POWER_LINE_FREQUENCY },
    { "sat", "Saturation", V4L2_CID_SATURATION },
    { "sharp", "Sharpness", V4L2_CID_SHARPNESS },
    { "wb_auto", "White Balance, Automatic", V4L2_CID_AUTO_WHITE_BALANCE },
    { "wb_temp", "White Balance Temperature", V4L2_CID_WHITE_BALANCE_TEMPERATURE }
};

static const camera_control_def_t *find_control(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof controls / sizeof controls[0]; i++)
        if (strcmp(name, controls[i].name) == 0) return &controls[i];
    return NULL;
}

const char *camera_control_name(const char *name) {
    const camera_control_def_t *def = find_control(name);
    return def ? def->name : NULL;
}

static int query_menu(int fd, const struct v4l2_queryctrl *q, int index,
                      struct v4l2_querymenu *menu) {
    memset(menu, 0, sizeof *menu);
    menu->id = q->id;
    menu->index = (uint32_t)index;
    return ioctl(fd, VIDIOC_QUERYMENU, menu);
}

int camera_controls_query(int fd, const char *name, camera_control_info_t *info) {
    const camera_control_def_t *def = find_control(name);
    if (!def) {
        fprintf(stderr, "unknown camera parameter: %s\n", name ? name : "(null)");
        return -1;
    }
    memset(info, 0, sizeof *info);
    info->query.id = def->id;
    if (ioctl(fd, VIDIOC_QUERYCTRL, &info->query) < 0) {
        fprintf(stderr, "camera parameter %s is unavailable: %s\n", name, strerror(errno));
        return -1;
    }
    if (info->query.flags & V4L2_CTRL_FLAG_DISABLED) {
        fprintf(stderr, "camera parameter %s is disabled\n", name);
        return -1;
    }
    return 0;
}

int camera_controls_get(int fd, const char *name, int *value) {
    camera_control_info_t info;
    if (camera_controls_query(fd, name, &info) < 0) return -1;
    struct v4l2_control control = { .id = info.query.id };
    if (ioctl(fd, VIDIOC_G_CTRL, &control) < 0) {
        fprintf(stderr, "cannot read camera parameter %s: %s\n", name, strerror(errno));
        return -1;
    }
    *value = control.value;
    return 0;
}

static int validate_value(int fd, const char *name, const struct v4l2_queryctrl *q, int value) {
    if (value < q->minimum || value > q->maximum ||
        (q->step > 0 && ((int64_t)value - q->minimum) % q->step != 0)) {
        fprintf(stderr, "camera parameter %s=%d is outside its supported range [%d..%d] step %d\n",
                name, value, q->minimum, q->maximum, q->step);
        return -1;
    }
    if (q->type == V4L2_CTRL_TYPE_MENU || q->type == V4L2_CTRL_TYPE_INTEGER_MENU) {
        struct v4l2_querymenu menu;
        if (query_menu(fd, q, value, &menu) < 0) {
            fprintf(stderr, "camera parameter %s=%d is not an allowed menu value\n", name, value);
            return -1;
        }
    }
    return 0;
}

int camera_controls_validate(int fd, const char *name, int value) {
    camera_control_info_t info;
    if (camera_controls_query(fd, name, &info) < 0) return -1;
    return validate_value(fd, name, &info.query, value);
}

int camera_controls_set(int fd, const char *name, int value) {
    camera_control_info_t info;
    if (camera_controls_query(fd, name, &info) < 0) return -1;
    if (validate_value(fd, name, &info.query, value) < 0) return -1;
    struct v4l2_control control = { .id = info.query.id, .value = value };
    if (ioctl(fd, VIDIOC_S_CTRL, &control) < 0) {
        fprintf(stderr, "cannot set camera parameter %s=%d: %s\n", name, value, strerror(errno));
        return -1;
    }
    return 0;
}

static int print_range(int fd, const camera_control_info_t *info, FILE *out) {
    const struct v4l2_queryctrl *q = &info->query;
    fprintf(out, "[%d..%d] step %d", q->minimum, q->maximum, q->step);
    if (q->type == V4L2_CTRL_TYPE_MENU || q->type == V4L2_CTRL_TYPE_INTEGER_MENU) {
        int first = 1;
        fprintf(out, "; choices {");
        for (int64_t index = q->minimum; index <= q->maximum; index++) {
            struct v4l2_querymenu menu;
            if (query_menu(fd, q, (int)index, &menu) < 0) continue;
            fprintf(out, "%s%lld=", first ? "" : ", ", (long long)index);
            if (q->type == V4L2_CTRL_TYPE_MENU)
                fprintf(out, "'%s'", (const char *)menu.name);
            else
                fprintf(out, "%lld", (long long)menu.value);
            first = 0;
        }
        fprintf(out, "}");
        if (first) {
            fprintf(stderr, "camera control %s has no readable menu values\n", q->name);
            return -1;
        }
    }
    return 0;
}

int camera_controls_print_value(int fd, const char *name, FILE *out) {
    int value;
    if (camera_controls_get(fd, name, &value) < 0) return -1;
    fprintf(out, "%s=%d\n", name, value);
    return 0;
}

int camera_controls_print_range(int fd, const char *name, FILE *out) {
    camera_control_info_t info;
    if (camera_controls_query(fd, name, &info) < 0) return -1;
    fprintf(out, "%s: ", name);
    if (print_range(fd, &info, out) < 0) return -1;
    fputc('\n', out);
    return 0;
}

int camera_controls_list(int fd, FILE *out) {
    int count = 0;
    for (size_t i = 0; i < sizeof controls / sizeof controls[0]; i++) {
        camera_control_info_t info;
        memset(&info, 0, sizeof info);
        info.query.id = controls[i].id;
        if (ioctl(fd, VIDIOC_QUERYCTRL, &info.query) < 0) {
            if (errno != EINVAL && errno != ENOTTY) {
                fprintf(stderr, "cannot query camera parameter %s: %s\n",
                        controls[i].name, strerror(errno));
                return -1;
            }
            continue;
        }
        if (info.query.flags & V4L2_CTRL_FLAG_DISABLED) continue;
        int value;
        if (camera_controls_get(fd, controls[i].name, &value) < 0) return -1;
        fprintf(out, "%s (%s): current=%d; ", controls[i].name, controls[i].label, value);
        if (print_range(fd, &info, out) < 0) return -1;
        fputc('\n', out);
        count++;
    }
    if (!count) {
        fprintf(stderr, "no supported camera parameters found\n");
        return -1;
    }
    return 0;
}
