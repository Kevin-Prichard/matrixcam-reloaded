/* ---------- CLI argument parsing ---------- */
#define _GNU_SOURCE
#include "config.h"
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

void config_parse_args(int argc, char **argv) {
    int o;
    while ((o = getopt(argc, argv, "v:d:s:c:o:t:f:r:F:gm")) != -1) switch (o) {
        case 'v': G.vdev = optarg; break;
        case 'd': G.ddev = optarg; break;
        case 's': parse_wh(optarg, &G.capw, &G.caph); break;
        case 'c': parse_wh(optarg, &G.cw, &G.ch); break;
        case 'o': parse_wh(optarg, &G.outw, &G.outh); break;
        case 't': G.nworkers = atoi(optarg); break;
        case 'f': G.font_path = optarg; break;
        case 'r': parse_codepoint_ranges_arg(optarg); break;
        case 'F': G.focus_value = atoi(optarg); break;
        case 'g':
            if (optind < argc &&
                parse_bichrome_colors(argv[optind], &G.bichrome_fg, &G.bichrome_bg) == 0) {
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
                    "e.g. -g 11ee11:1111ee)] [-m (disable mirror, default mirrored)]\n"
                    "  RANGE: uHH-uHH, uHHHH-uHHHH, uHHHHHH-uHHHHHH, or U+HH-U+HH notation\n",
                    argv[0]);
            exit(1);
    }
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
