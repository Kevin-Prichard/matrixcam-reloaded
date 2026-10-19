/* ---------- CLI argument parsing ---------- */
#define _GNU_SOURCE
#include "config.h"
#include "camera_controls.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

config_t G = {
    .vdev = "/dev/video1", .ddev = "/dev/dri/card0",
    .capw = 640, .caph = 480, .cw = 8, .ch = 8,
    .outw = 1280, .outh = 720, .nworkers = 3, .hflip = 1, .focus_value = -1
};

static void parse_wh(const char *s, int *w, int *h) { sscanf(s, "%dx%d", w, h); }

/* Parses "RRGGBB:RRGGBB" (foreground:background) hex color pairs for
 * bichrome mode, returning 0 on success or -1 if the text does not match. */
static int parse_bichrome_colors(const char *s, uint32_t *fg, uint32_t *bg) {
    unsigned fg_r, fg_g, fg_b, bg_r, bg_g, bg_b;
    if (strlen(s) != 13 ||
        sscanf(s, "%2x%2x%2x:%2x%2x%2x", &fg_r, &fg_g, &fg_b, &bg_r, &bg_g, &bg_b) != 6)
        return -1;
    *fg = (uint32_t)fg_r << 16 | (uint32_t)fg_g << 8 | fg_b;
    *bg = (uint32_t)bg_r << 16 | (uint32_t)bg_g << 8 | bg_b;
    return 0;
}

/* Parses one endpoint of a "uHH", "uHHHH", "uHHHHHH", or "U+HH" style
 * codepoint, returning 0 on success. */
static int parse_codepoint_endpoint(const char *text, uint32_t *value) {
    if ((text[0] == 'u' || text[0] == 'U') && text[1] == '+') text += 2;
    else if (text[0] == 'u' || text[0] == 'U') text += 1;
    if (!*text) return -1;
    char *end;
    unsigned long parsed = strtoul(text, &end, 16);
    if (*end || parsed > 0x10FFFFul) return -1;
    *value = (uint32_t)parsed;
    return 0;
}

/* Parses one "LOW-HIGH" codepoint range and appends it to G.codepoint_ranges. */
static int add_codepoint_range(char *spec) {
    char *dash = strchr(spec, '-');
    if (!dash) return -1;
    *dash = '\0';
    uint32_t low, high;
    if (parse_codepoint_endpoint(spec, &low) || parse_codepoint_endpoint(dash + 1, &high))
        return -1;
    if (low > high) { uint32_t swap = low; low = high; high = swap; }
    codepoint_range_t *grown = realloc(G.codepoint_ranges,
        (G.codepoint_range_count + 1) * sizeof(*G.codepoint_ranges));
    if (!grown) { perror("codepoint ranges"); exit(1); }
    G.codepoint_ranges = grown;
    G.codepoint_ranges[G.codepoint_range_count].low = low;
    G.codepoint_ranges[G.codepoint_range_count].high = high;
    G.codepoint_range_count++;
    return 0;
}

/* Accepts one -r argument, which may hold several comma-separated ranges. */
static void parse_codepoint_ranges_arg(const char *arg) {
    char *copy = strdup(arg);
    if (!copy) { perror("codepoint ranges"); exit(1); }
    char *saveptr = NULL;
    char *token = strtok_r(copy, ",", &saveptr);
    while (token) {
        if (add_codepoint_range(token) < 0) {
            fprintf(stderr, "invalid character range: %s (expected uHH-uHH, uHHHH-uHHHH, "
                    "uHHHHHH-uHHHHHH, or U+HH-U+HH notation)\n", token);
            exit(1);
        }
        token = strtok_r(NULL, ",", &saveptr);
    }
    free(copy);
}

static void add_camera_param(const char *name, const char *value_text) {
    const char *canonical = camera_control_name(name);
    if (!canonical) {
        fprintf(stderr, "unknown camera parameter: %s\n", name);
        exit(1);
    }
    errno = 0;
    char *end;
    long value = strtol(value_text, &end, 10);
    if (!*value_text || *end || errno == ERANGE || value < INT_MIN || value > INT_MAX) {
        fprintf(stderr, "invalid value for camera parameter %s: %s (expected an integer)\n",
                canonical, value_text);
        exit(1);
    }
    camera_param_t *grown = realloc(G.camera_params,
        (G.camera_param_count + 1) * sizeof(*G.camera_params));
    if (!grown) { perror("camera parameters"); exit(1); }
    G.camera_params = grown;
    G.camera_params[G.camera_param_count].name = canonical;
    G.camera_params[G.camera_param_count].value = (int)value;
    G.camera_param_count++;
}

static void parse_camera_param_set(char *arg) {
    char *token = arg;
    for (;;) {
        char *comma = strchr(token, ',');
        if (comma) *comma = '\0';
        char *equals = strchr(token, '=');
        if (!equals || equals == token || !equals[1]) {
            fprintf(stderr, "invalid camera parameter assignment: %s (expected NAME=INTEGER)\n",
                    token);
            exit(1);
        }
        *equals = '\0';
        add_camera_param(token, equals + 1);
        if (!comma) break;
        token = comma + 1;
        if (!*token) {
            fprintf(stderr, "invalid empty camera parameter assignment\n");
            exit(1);
        }
    }
}

static void parse_camera_params_arg(const char *arg) {
    if (!*arg) {
        if (G.camera_params_action != CAMERA_PARAMS_NONE || G.camera_param_count) {
            fprintf(stderr, "camera parameter reporting cannot be combined with other -p options\n");
            exit(1);
        }
        G.camera_params_action = CAMERA_PARAMS_LIST;
        return;
    }
    char *copy = strdup(arg);
    if (!copy) { perror("camera parameter"); exit(1); }
    if (copy[0] == '@') {
        if (G.camera_params_action != CAMERA_PARAMS_NONE || G.camera_param_count) {
            fprintf(stderr, "camera parameter reporting cannot be combined with other -p options\n");
            exit(1);
        }
        const char *canonical = camera_control_name(copy + 1);
        if (!canonical || !copy[1]) {
            fprintf(stderr, "unknown camera parameter: %s\n", copy + 1);
            exit(1);
        }
        G.camera_params_action = CAMERA_PARAMS_RANGE;
        G.camera_param_query = canonical;
        free(copy);
        return;
    }
    if (!strchr(copy, '=')) {
        if (G.camera_params_action != CAMERA_PARAMS_NONE || G.camera_param_count) {
            fprintf(stderr, "camera parameter reporting cannot be combined with other -p options\n");
            exit(1);
        }
        const char *canonical = camera_control_name(copy);
        if (!canonical) {
            fprintf(stderr, "unknown camera parameter: %s\n", copy);
            exit(1);
        }
        G.camera_params_action = CAMERA_PARAMS_GET;
        G.camera_param_query = canonical;
        free(copy);
        return;
    }
    if (G.camera_params_action != CAMERA_PARAMS_NONE) {
        fprintf(stderr, "camera parameter assignments cannot be combined with reporting\n");
        exit(1);
    }
    G.camera_params_action = CAMERA_PARAMS_NONE;
    parse_camera_param_set(copy);
    free(copy);
}

void config_parse_args(int argc, char **argv) {
    /* GNU getopt supports optional arguments only when attached; normalize -p
     * so the documented "-p NAME" form and bare "-p" are both accepted. */
    char **normalized = calloc(2 * (size_t)argc + 1, sizeof(*normalized));
    if (!normalized) { perror("arguments"); exit(1); }
    int normalized_argc = 1;
    normalized[0] = argv[0];
    for (int i = 1; i < argc; i++) {
        normalized[normalized_argc++] = argv[i];
        if (strcmp(argv[i], "-p") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-')
                normalized[normalized_argc++] = argv[++i];
            else
                normalized[normalized_argc++] = "";
        }
    }
    normalized[normalized_argc] = NULL;
    optind = 1;
    int o;
    while ((o = getopt(normalized_argc, normalized, "v:d:s:c:o:t:f:r:F:gp:m")) != -1) switch (o) {
        case 'v': G.vdev = optarg; break;
        case 'd': G.ddev = optarg; break;
        case 's': parse_wh(optarg, &G.capw, &G.caph); break;
        case 'c': parse_wh(optarg, &G.cw, &G.ch); break;
        case 'o': parse_wh(optarg, &G.outw, &G.outh); break;
        case 't': G.nworkers = atoi(optarg); break;
        case 'f': G.font_path = optarg; break;
        case 'r': parse_codepoint_ranges_arg(optarg); break;
        case 'F': G.focus_value = atoi(optarg); break;
        case 'p': parse_camera_params_arg(optarg); break;
        case 'g':
            if (optind < normalized_argc &&
                parse_bichrome_colors(normalized[optind], &G.bichrome_fg, &G.bichrome_bg) == 0) {
                G.bichrome = 1;
                optind++;
            } else {
                G.gray = 1;
            }
            break;
        case 'm': G.hflip = 0; break;
        default:
            fprintf(stderr, "usage: %s [-v /dev/videoN] [-d /dev/dri/cardN] [-s CAPWxCAPH] "
                    "[-c CELLWxCELLH] [-o OUTWxOUTH] [-t workers] [-f FONT] "
                    "[-r RANGE[,RANGE...]] [-F focusvalue (manual focus_absolute, disables "
                    "autofocus; omit for autofocus)] [-g [FGHEX:BGHEX] (luminosity, or bichrome "
                    "e.g. -g 11ee11:1111ee)] [-m (disable mirror, default mirrored)] "
                    "[-p [PARAM_NAME|@PARAM_NAME|PARAM_NAME=value[,PARAM_NAME=value...]]]\n"
                    "  RANGE: uHH-uHH, uHHHH-uHHHH, uHHHHHH-uHHHHHH, or U+HH-U+HH notation\n",
                    argv[0]);
            exit(1);
    }
    free(normalized);
    if (G.cw < 1 || G.ch < 1 || G.nworkers < 1) {
        fprintf(stderr, "cell dimensions and worker count must be positive\n");
        exit(1);
    }
}

void config_derive_grid(void) {
    G.denom = 1;
    for (int d = 8; d >= 1; d /= 2) if (G.cw % d == 0 && G.ch % d == 0) { G.denom = d; break; }
    G.scw = G.cw / G.denom; G.sch = G.ch / G.denom;
    G.gw = G.capw / G.cw; G.gh = G.caph / G.ch;
    if (G.gw < 1 || G.gh < 1) { fprintf(stderr, "cell larger than frame\n"); exit(1); }
}
