/* ---------- CLI argument parsing ---------- */
#define _GNU_SOURCE
#include "config.h"
#include "camera_controls.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

config_t G = {
    .vdev = "/dev/video1", .ddev = "/dev/dri/card0",
    .visualizer = "glyph",
    .capw = 640, .caph = 480, .cw = 8, .ch = 8,
    .outw = 1280, .outh = 720, .nworkers = 3, .hflip = 1, .focus_value = -1,
    .delaunay_point_count = 600, .delaunay_edge_weight = 1.0, .delaunay_seed = 1,
    .slic_cluster_count = 400, .slic_iterations = 5, .slic_sample_stride = 3,
    .slic_compactness = 10, .slic_downscale = 2,
    .joydiv_grouprows = 8, .joydiv_alpha = 255,
    .joydiv_background = JOYDIV_BACKGROUND_COLOR,
    .joydiv_background_color = 0x000000u,
    .joydiv_default_yvalue = JOYDIV_YVALUE_LUM,
    .joydiv_default_lines = JOYDIV_LINES_STRAIGHT,
    .joydiv_default_b = 0,
    .joydiv_default_bez_tension = 0.5,
    .joydiv_default_bez_smooth = 8.0
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

/* Parses Delaunay settings as POINTS[,EDGE_WEIGHT[,SEED]]. */
static void parse_delaunay_options(const char *arg) {
    char *copy = strdup(arg);
    if (!copy) { perror("Delaunay options"); exit(1); }
    char *parts[3] = {0};
    char *cursor = copy;
    for (int i = 0; i < 3; i++) {
        parts[i] = strsep(&cursor, ",");
        if (!parts[i]) break;
    }
    if (cursor || !parts[0] || !*parts[0] ||
        (parts[1] && !*parts[1]) || (parts[2] && !*parts[2])) {
        fprintf(stderr, "invalid Delaunay options: %s (expected POINTS[,EDGE_WEIGHT[,SEED]])\n",
                arg);
        free(copy);
        exit(1);
    }

    errno = 0;
    char *end;
    long point_count = strtol(parts[0], &end, 10);
    if (!*parts[0] || *end || errno == ERANGE || point_count < 16 || point_count > 3000) {
        fprintf(stderr, "invalid Delaunay point count: %s (expected 16..3000)\n", parts[0]);
        free(copy);
        exit(1);
    }
    G.delaunay_point_count = (int)point_count;

    if (parts[1]) {
        errno = 0;
        double edge_weight = strtod(parts[1], &end);
        if (!*parts[1] || *end || errno == ERANGE || !isfinite(edge_weight) ||
            edge_weight < 0.0 || edge_weight > 100.0) {
            fprintf(stderr, "invalid Delaunay edge weight: %s (expected 0..100)\n", parts[1]);
            free(copy);
            exit(1);
        }
        G.delaunay_edge_weight = edge_weight;
    }
    if (parts[2]) {
        errno = 0;
        unsigned long seed = strtoul(parts[2], &end, 10);
        if (!*parts[2] || *end || errno == ERANGE || seed > UINT32_MAX) {
            fprintf(stderr, "invalid Delaunay random seed: %s (expected 0..4294967295)\n",
                    parts[2]);
            free(copy);
            exit(1);
        }
        G.delaunay_seed = (uint32_t)seed;
    }
    free(copy);
}

typedef struct {
    const char *name;
    int *value;
    int minimum, maximum;
} slic_param_t;

static slic_param_t slic_parameters[] = {
    { "clusters", &G.slic_cluster_count, 16, 65534 },
    { "iterations", &G.slic_iterations, 1, 100 },
    { "sample_stride", &G.slic_sample_stride, 1, 16 },
    { "compactness", &G.slic_compactness, 1, 100 },
    { "downscale", &G.slic_downscale, 1, 8 }
};

static slic_param_t *find_slic_parameter(const char *name) {
    for (size_t i = 0; i < sizeof(slic_parameters) / sizeof(slic_parameters[0]); i++)
        if (!strcmp(slic_parameters[i].name, name)) return &slic_parameters[i];
    return NULL;
}

static void parse_slic_options(const char *arg) {
    if (!*arg) {
        if (G.slic_params_action != SLIC_PARAMS_NONE || G.slic_params_set) {
            fprintf(stderr, "SLIC parameter reporting cannot be combined with other -a options\n");
            exit(1);
        }
        G.slic_params_action = SLIC_PARAMS_LIST;
        return;
    }

    char *copy = strdup(arg);
    if (!copy) { perror("SLIC parameters"); exit(1); }
    if (copy[0] == '@' || !strchr(copy, '=')) {
        if (G.slic_params_action != SLIC_PARAMS_NONE || G.slic_params_set) {
            fprintf(stderr, "SLIC parameter reporting cannot be combined with other -a options\n");
            exit(1);
        }
        int range = copy[0] == '@';
        const char *name = range ? copy + 1 : copy;
        slic_param_t *parameter = find_slic_parameter(name);
        if (!parameter) {
            fprintf(stderr, "unknown SLIC parameter: %s\n", name);
            free(copy);
            exit(1);
        }
        G.slic_params_action = range ? SLIC_PARAMS_RANGE : SLIC_PARAMS_GET;
        G.slic_param_query = parameter->name;
        free(copy);
        return;
    }
    if (G.slic_params_action != SLIC_PARAMS_NONE) {
        fprintf(stderr, "SLIC parameter assignments cannot be combined with reporting\n");
        free(copy);
        exit(1);
    }

    char *token = copy;
    for (;;) {
        char *comma = strchr(token, ',');
        if (comma) *comma = '\0';
        char *equals = strchr(token, '=');
        if (!equals || equals == token || !equals[1]) {
            fprintf(stderr, "invalid SLIC parameter assignment: %s (expected NAME=INTEGER)\n",
                    token);
            free(copy);
            exit(1);
        }
        *equals = '\0';
        slic_param_t *parameter = find_slic_parameter(token);
        if (!parameter) {
            fprintf(stderr, "unknown SLIC parameter: %s\n", token);
            free(copy);
            exit(1);
        }
        errno = 0;
        char *end;
        long value = strtol(equals + 1, &end, 10);
        if (!*(equals + 1) || *end || errno == ERANGE ||
            value < parameter->minimum || value > parameter->maximum) {
            fprintf(stderr, "invalid SLIC parameter %s: %s (expected %d..%d)\n",
                    parameter->name, equals + 1, parameter->minimum, parameter->maximum);
            free(copy);
            exit(1);
        }
        *parameter->value = (int)value;
        G.slic_params_set = 1;
        if (!comma) break;
        token = comma + 1;
        if (!*token) {
            fprintf(stderr, "invalid empty SLIC parameter assignment\n");
            free(copy);
            exit(1);
        }
    }
    free(copy);
}

static void joydiv_add_zone(int x, int y, int w, int h) {
    joydiv_zone_t *grown = realloc(G.joydiv_zones,
        (G.joydiv_zone_count + 1) * sizeof(*G.joydiv_zones));
    if (!grown) { perror("joydiv zones"); exit(1); }
    G.joydiv_zones = grown;
    joydiv_zone_t *zone = &G.joydiv_zones[G.joydiv_zone_count++];
    zone->x = x; zone->y = y; zone->w = w; zone->h = h;
    zone->yvalue = G.joydiv_default_yvalue;
    zone->lines = G.joydiv_default_lines;
    zone->b = G.joydiv_default_b;
    zone->bez_tension = G.joydiv_default_bez_tension;
    zone->bez_smooth = G.joydiv_default_bez_smooth;
}

static int parse_hex_color(const char *text, uint32_t *color) {
    unsigned value;
    char tail;
    if (strlen(text) != 6 || sscanf(text, "%6x%c", &value, &tail) != 1) return -1;
    *color = (uint32_t)value;
    return 0;
}

static int parse_joydiv_double(const char *text, double minimum, double maximum,
                               double *value) {
    errno = 0;
    char *end;
    double parsed = strtod(text, &end);
    if (!*text || *end || errno == ERANGE || !isfinite(parsed) ||
        parsed < minimum || parsed > maximum)
        return -1;
    *value = parsed;
    return 0;
}

static void parse_joydiv_options(const char *arg) {
    if (!*arg) {
        fprintf(stderr, "joydiv requires -a key=value options\n");
        exit(1);
    }
    char *copy = strdup(arg);
    if (!copy) { perror("joydiv options"); exit(1); }
    char *equals = strchr(copy, '=');
    if (!equals || equals == copy || !equals[1]) {
        fprintf(stderr, "invalid joydiv option: %s (expected NAME=VALUE)\n", arg);
        free(copy);
        exit(1);
    }
    *equals = '\0';
    const char *key = copy;
    const char *text = equals + 1;

    if (!strcmp(key, "zone")) {
        int x, y, w, h, used = 0;
        if (sscanf(text, "%d.%d+%d.%d%n", &x, &y, &w, &h, &used) != 4 ||
            text[used] != '\0' || x < 0 || y < 0 || w < 1 || h < 1 ||
            (int64_t)x + w > INT_MAX || (int64_t)y + h > INT_MAX) {
            fprintf(stderr, "invalid joydiv zone: %s (expected STARTX.STARTY+WIDTH.HEIGHT)\n",
                    text);
            free(copy);
            exit(1);
        }
        joydiv_add_zone(x, y, w, h);
    } else if (!strcmp(key, "grouprows")) {
        errno = 0;
        char *end;
        long value = strtol(text, &end, 10);
        if (!*text || *end || errno == ERANGE || value < 1 || value > INT_MAX) {
            fprintf(stderr, "invalid joydiv grouprows: %s (expected a positive integer)\n",
                    text);
            free(copy);
            exit(1);
        }
        G.joydiv_grouprows = (int)value;
    } else if (!strcmp(key, "yvalue")) {
        joydiv_yvalue_t value;
        if (!strcmp(text, "lum")) value = JOYDIV_YVALUE_LUM;
        else if (!strcmp(text, "color")) value = JOYDIV_YVALUE_COLOR;
        else {
            fprintf(stderr, "invalid joydiv yvalue: %s (expected lum or color)\n", text);
            free(copy);
            exit(1);
        }
        G.joydiv_default_yvalue = value;
        for (int i = 0; i < G.joydiv_zone_count; i++)
            G.joydiv_zones[i].yvalue = value;
    } else if (!strcmp(key, "lines")) {
        joydiv_lines_t value;
        if (!strcmp(text, "straight")) value = JOYDIV_LINES_STRAIGHT;
        else if (!strcmp(text, "bezier")) value = JOYDIV_LINES_BEZIER;
        else {
            fprintf(stderr, "invalid joydiv lines: %s (expected straight or bezier)\n", text);
            free(copy);
            exit(1);
        }
        G.joydiv_default_lines = value;
        for (int i = 0; i < G.joydiv_zone_count; i++)
            G.joydiv_zones[i].lines = value;
    } else if (!strcmp(key, "b")) {
        errno = 0;
        char *end;
        long value = strtol(text, &end, 10);
        if (!*text || *end || errno == ERANGE || value < 0 || value > INT_MAX) {
            fprintf(stderr, "invalid joydiv b: %s (expected a non-negative integer)\n", text);
            free(copy);
            exit(1);
        }
        G.joydiv_default_b = (int)value;
        for (int i = 0; i < G.joydiv_zone_count; i++)
            G.joydiv_zones[i].b = (int)value;
    } else if (!strcmp(key, "bez-tension")) {
        double value;
        if (parse_joydiv_double(text, 0.0, 1.0, &value) < 0) {
            fprintf(stderr, "invalid joydiv bez-tension: %s (expected 0..1)\n", text);
            free(copy);
            exit(1);
        }
        G.joydiv_default_bez_tension = value;
        for (int i = 0; i < G.joydiv_zone_count; i++)
            G.joydiv_zones[i].bez_tension = value;
    } else if (!strcmp(key, "bez-smooth")) {
        double value;
        if (parse_joydiv_double(text, 1.0, 64.0, &value) < 0) {
            fprintf(stderr, "invalid joydiv bez-smooth: %s (expected 1..64)\n", text);
            free(copy);
            exit(1);
        }
        G.joydiv_default_bez_smooth = value;
        for (int i = 0; i < G.joydiv_zone_count; i++)
            G.joydiv_zones[i].bez_smooth = value;
    } else if (!strcmp(key, "background")) {
        if (!strcmp(text, "video")) {
            G.joydiv_background = JOYDIV_BACKGROUND_VIDEO;
        } else if (!strcmp(text, "color")) {
            G.joydiv_background = JOYDIV_BACKGROUND_COLOR;
        } else if (!strcmp(text, "black")) {
            G.joydiv_background = JOYDIV_BACKGROUND_COLOR;
            G.joydiv_background_color = 0x000000u;
        } else {
            fprintf(stderr, "invalid joydiv background: %s (expected black, color, or video)\n",
                    text);
            free(copy);
            exit(1);
        }
    } else if (!strcmp(key, "color")) {
        if (parse_hex_color(text, &G.joydiv_background_color) < 0) {
            fprintf(stderr, "invalid joydiv color: %s (expected RRGGBB)\n", text);
            free(copy);
            exit(1);
        }
    } else if (!strcmp(key, "alpha")) {
        errno = 0;
        char *end;
        long value = strtol(text, &end, 10);
        if (!*text || *end || errno == ERANGE || value < 0 || value > 255) {
            fprintf(stderr, "invalid joydiv alpha: %s (expected 0..255)\n", text);
            free(copy);
            exit(1);
        }
        G.joydiv_alpha = (int)value;
    } else {
        fprintf(stderr, "unknown joydiv option: %s\n", key);
        free(copy);
        exit(1);
    }
    free(copy);
}

void config_print_slic_params(void) {
    if (G.slic_params_action == SLIC_PARAMS_LIST) {
        for (size_t i = 0; i < sizeof(slic_parameters) / sizeof(slic_parameters[0]); i++)
            printf("%s=%d [%d..%d]\n", slic_parameters[i].name, *slic_parameters[i].value,
                   slic_parameters[i].minimum, slic_parameters[i].maximum);
        return;
    }
    slic_param_t *parameter = find_slic_parameter(G.slic_param_query);
    if (G.slic_params_action == SLIC_PARAMS_RANGE)
        printf("%d..%d\n", parameter->minimum, parameter->maximum);
    else
        printf("%d\n", *parameter->value);
}

void config_parse_args(int argc, char **argv) {
    /* Determine the selected visualizer first so its ignored options stay
     * ignored regardless of their position relative to -V. */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-V") && i + 1 < argc)
            G.visualizer = argv[i + 1];
        else if (!strncmp(argv[i], "-V", 2) && argv[i][2])
            G.visualizer = argv[i] + 2;
    }

    /* GNU getopt supports required arguments only when attached; normalize -p
     * and -a so both separated and bare reporting forms are accepted. */
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
        if (strcmp(argv[i], "-a") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-')
                normalized[normalized_argc++] = argv[++i];
            else
                normalized[normalized_argc++] = "";
        }
    }
    normalized[normalized_argc] = NULL;
    optind = 1;
    char **visualizer_options = calloc((size_t)argc, sizeof(*visualizer_options));
    if (!visualizer_options) { perror("visualizer options"); exit(1); }
    int visualizer_option_count = 0;
    int o;
    while ((o = getopt(normalized_argc, normalized, "v:d:s:c:o:t:f:r:F:gp:mV:a:")) != -1) switch (o) {
        case 'v': G.vdev = optarg; break;
        case 'd': G.ddev = optarg; break;
        case 's': parse_wh(optarg, &G.capw, &G.caph); break;
        case 'c':
            if (!strcmp(G.visualizer, "joydiv")) {
                fprintf(stderr, "-c is not supported with -V joydiv\n");
                exit(1);
            }
            if (strcmp(G.visualizer, "slic")) parse_wh(optarg, &G.cw, &G.ch);
            break;
        case 'o': parse_wh(optarg, &G.outw, &G.outh); break;
        case 't': G.nworkers = atoi(optarg); break;
        case 'f':
            if (!strcmp(G.visualizer, "joydiv")) {
                fprintf(stderr, "-f is not supported with -V joydiv\n");
                exit(1);
            }
            if (strcmp(G.visualizer, "slic")) G.font_path = optarg;
            break;
        case 'r':
            if (!strcmp(G.visualizer, "joydiv")) {
                fprintf(stderr, "-r is not supported with -V joydiv\n");
                exit(1);
            }
            if (strcmp(G.visualizer, "slic")) parse_codepoint_ranges_arg(optarg);
            break;
        case 'F': G.focus_value = atoi(optarg); break;
        case 'p': parse_camera_params_arg(optarg); break;
        case 'g':
            if (!strcmp(G.visualizer, "joydiv")) {
                fprintf(stderr, "-g is not supported with -V joydiv\n");
                exit(1);
            }
            if (!strcmp(G.visualizer, "slic")) {
                uint32_t fg, bg;
                if (optind < normalized_argc &&
                    parse_bichrome_colors(normalized[optind], &fg, &bg) == 0)
                    optind++;
            } else {
                if (optind < normalized_argc &&
                    parse_bichrome_colors(normalized[optind], &G.bichrome_fg, &G.bichrome_bg) == 0) {
                    G.bichrome = 1;
                    optind++;
                } else {
                    G.gray = 1;
                }
            }
            break;
        case 'm': G.hflip = 0; break;
        case 'V':
            if (strcmp(optarg, "glyph") && strcmp(optarg, "delaunay") &&
                strcmp(optarg, "slic") && strcmp(optarg, "joydiv")) {
                fprintf(stderr, "unknown visualizer: %s (expected glyph, delaunay, slic, or joydiv)\n",
                        optarg);
                exit(1);
            }
            G.visualizer = optarg;
            break;
        case 'a': visualizer_options[visualizer_option_count++] = optarg; break;
        default:
            fprintf(stderr, "usage: %s [-v /dev/videoN] [-d /dev/dri/cardN] [-s CAPWxCAPH] "
                    "[-c CELLWxCELLH] [-o OUTWxOUTH] [-t workers] [-f FONT] "
                    "[-r RANGE[,RANGE...]] [-F focusvalue (manual focus_absolute, disables "
                    "autofocus; omit for autofocus)] [-g [FGHEX:BGHEX] (luminosity, or bichrome "
                    "e.g. -g 11ee11:1111ee)] [-m (disable mirror, default mirrored)] "
                    "[-p [PARAM_NAME|@PARAM_NAME|PARAM_NAME=value[,PARAM_NAME=value...]]] "
                    "[-V glyph|delaunay|slic|joydiv] "
                    "[-a POINTS[,EDGE_WEIGHT[,SEED]]|SLIC_PARAMETER[=VALUE]|JOYDIV_KEY=VALUE]\n"
                    "  RANGE: uHH-uHH, uHHHH-uHHHH, uHHHHHH-uHHHHHH, or U+HH-U+HH notation\n"
                    "  JOYDIV: zone=X.Y+W.H, grouprows=N, yvalue=lum|color, "
                    "lines=straight|bezier, b=N,\n"
                    "          bez-tension=F, bez-smooth=F, background=black|color|video, "
                    "color=RRGGBB, alpha=N\n",
                    argv[0]);
            exit(1);
    }
    free(normalized);
    for (int i = 0; i < visualizer_option_count; i++) {
        if (!strcmp(G.visualizer, "slic"))
            parse_slic_options(visualizer_options[i]);
        else if (!strcmp(G.visualizer, "joydiv"))
            parse_joydiv_options(visualizer_options[i]);
        else
            parse_delaunay_options(visualizer_options[i]);
    }
    free(visualizer_options);
    if ((G.slic_params_action != SLIC_PARAMS_NONE &&
         (G.camera_params_action != CAMERA_PARAMS_NONE || G.camera_param_count)) ||
        (G.camera_params_action != CAMERA_PARAMS_NONE && G.slic_params_set)) {
        fprintf(stderr, "camera and SLIC parameter reporting/actions cannot be combined\n");
        exit(1);
    }
    if (!strcmp(G.visualizer, "delaunay")) {
        /* These options belong to glyph rendering and do not control the
         * Delaunay analyzer's fixed, camera-relative sampling grid. */
        G.cw = 8;
        G.ch = 8;
    }
    if (G.cw < 1 || G.ch < 1 || G.nworkers < 1) {
        fprintf(stderr, "cell dimensions and worker count must be positive\n");
        exit(1);
    }
}

void config_derive_grid(void) {
    if (!strcmp(G.visualizer, "joydiv")) {
        if (G.joydiv_grouprows < 1) {
            fprintf(stderr, "joydiv grouprows must be positive\n");
            exit(1);
        }
        if (G.joydiv_zone_count == 0)
            joydiv_add_zone(0, 0, G.capw, G.caph);
        for (int i = 0; i < G.joydiv_zone_count; i++) {
            const joydiv_zone_t *zone = &G.joydiv_zones[i];
            if (zone->x < 0 || zone->y < 0 || zone->w < 1 || zone->h < 1 ||
                (int64_t)zone->x + zone->w > G.capw ||
                (int64_t)zone->y + zone->h > G.caph) {
                fprintf(stderr, "joydiv zone %d.%d+%d.%d is outside the %dx%d capture frame\n",
                        zone->x, zone->y, zone->w, zone->h, G.capw, G.caph);
                exit(1);
            }
        }
        G.denom = 1;
        G.scw = G.sch = 1;
        G.gw = G.gh = 1;
        return;
    }
    if (!strcmp(G.visualizer, "slic")) {
        if (G.slic_downscale != 1 && G.slic_downscale != 2 &&
            G.slic_downscale != 4 && G.slic_downscale != 8) {
            fprintf(stderr, "SLIC downscale must be 1, 2, 4, or 8\n");
            exit(1);
        }
        G.denom = G.slic_downscale;
        G.scw = G.sch = 1;
        G.gw = (G.capw + G.denom - 1) / G.denom;
        G.gh = (G.caph + G.denom - 1) / G.denom;
        if (G.gw < 1 || G.gh < 1 ||
            (int64_t)G.gw * G.gh > INT_MAX ||
            G.slic_cluster_count > (int64_t)G.gw * G.gh) {
            fprintf(stderr, "SLIC image dimensions or cluster count are out of range\n");
            exit(1);
        }
        return;
    }
    G.denom = 1;
    for (int d = 8; d >= 1; d /= 2) if (G.cw % d == 0 && G.ch % d == 0) { G.denom = d; break; }
    G.scw = G.cw / G.denom; G.sch = G.ch / G.denom;
    G.gw = G.capw / G.cw; G.gh = G.caph / G.ch;
    if (G.gw < 1 || G.gh < 1) { fprintf(stderr, "cell larger than frame\n"); exit(1); }
}
