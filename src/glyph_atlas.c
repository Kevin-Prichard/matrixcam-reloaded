/* ---------- FreeType glyph atlas ---------- */
#include "glyph_atlas.h"
#include "config.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ft2build.h>
#include FT_FREETYPE_H

static FT_Library font_library;
static FT_Face font_face;
static uint8_t *font_atlas;
static uint8_t font_level_present[256], font_level_for_luminosity[256];
static size_t font_cell_size;

static const char *find_default_font(void) {
    static const char *font_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        NULL
    };
    for (int i = 0; font_paths[i]; i++)
        if (access(font_paths[i], R_OK) == 0) return font_paths[i];
    return NULL;
}

static int font_bitmap_pixel(const FT_Bitmap *bitmap, int x, int y) {
    const uint8_t *row = bitmap->pitch >= 0
        ? bitmap->buffer + (size_t)y * bitmap->pitch
        : bitmap->buffer + (size_t)(bitmap->rows - 1 - y) * -bitmap->pitch;
    if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY) {
        unsigned gray_levels = bitmap->num_grays > 1 ? bitmap->num_grays - 1 : 255;
        return row[x] * 255 / gray_levels;
    }
    if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO)
        return (row[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
    return 0;
}

void glyph_atlas_init(void) {
    const char *font_path = G.font_path ? G.font_path : find_default_font();
    if (!font_path) {
        fprintf(stderr, "no default system font found; specify one with -f FONT\n");
        exit(1);
    }
    if (FT_Init_FreeType(&font_library)) {
        fprintf(stderr, "could not initialize FreeType\n");
        exit(1);
    }
    if (FT_New_Face(font_library, font_path, 0, &font_face)) {
        fprintf(stderr, "could not load font: %s\n", font_path);
        exit(1);
    }
    /* Prefer a Unicode cmap explicitly: some fonts only carry a legacy
     * Microsoft Symbol cmap (codepoints offset by 0xF000) or no cmap at all,
     * and FreeType's automatic selection does not always favor Unicode. */
    FT_Select_Charmap(font_face, FT_ENCODING_UNICODE);
    if (!font_face->charmap) FT_Select_Charmap(font_face, FT_ENCODING_MS_SYMBOL);
    int symbol_encoded = font_face->charmap &&
        font_face->charmap->encoding == FT_ENCODING_MS_SYMBOL;
    if (FT_Set_Pixel_Sizes(font_face, G.cw, G.ch)) {
        fprintf(stderr, "could not size font for %dx%d cells\n", G.cw, G.ch);
        exit(1);
    }

    if ((size_t)G.cw > SIZE_MAX / (size_t)G.ch) {
        fprintf(stderr, "cell dimensions too large for font atlas\n");
        exit(1);
    }
    font_cell_size = (size_t)G.cw * G.ch;
    if (font_cell_size > SIZE_MAX / 256) {
        fprintf(stderr, "cell dimensions too large for font atlas\n");
        exit(1);
    }
    font_atlas = calloc(256, font_cell_size);
    if (!font_atlas) { perror("font atlas"); exit(1); }
    uint8_t *candidate = calloc(font_cell_size, 1);
    if (!candidate) { perror("font glyph"); exit(1); }

    /* Rasterizes whatever glyph is currently loaded into font_face->glyph and
     * claims its luminosity level in the atlas unless that level is already
     * taken by an earlier (lower-codepoint, or lower-index) glyph. */
    void process_current_glyph(void) {
        const FT_Bitmap *bitmap = &font_face->glyph->bitmap;
        memset(candidate, 0, font_cell_size);
        int left = (G.cw - (int)bitmap->width) / 2;
        int top = (G.ch - (int)bitmap->rows) / 2;
        for (unsigned int y = 0; y < bitmap->rows; y++) {
            int target_y = top + (int)y;
            if (target_y < 0 || target_y >= G.ch) continue;
            for (unsigned int x = 0; x < bitmap->width; x++) {
                int target_x = left + (int)x;
                if (target_x >= 0 && target_x < G.cw)
                    candidate[(size_t)target_y * G.cw + target_x] =
                        font_bitmap_pixel(bitmap, (int)x, (int)y);
            }
        }
        uint64_t coverage_sum = 0;
        for (size_t i = 0; i < font_cell_size; i++) coverage_sum += candidate[i];
        unsigned int luminosity = (coverage_sum + font_cell_size / 2) / font_cell_size;
        if (!font_level_present[luminosity]) {
            memcpy(font_atlas + (size_t)luminosity * font_cell_size, candidate, font_cell_size);
            font_level_present[luminosity] = 1;
        }
    }

    /* Resolves a Unicode codepoint to a glyph index, retrying with the
     * legacy Microsoft Symbol 0xF000 offset when the active cmap uses it. */
    FT_UInt resolve_glyph_index(FT_ULong character_code) {
        FT_UInt glyph_index = FT_Get_Char_Index(font_face, character_code);
        if (!glyph_index && symbol_encoded && character_code <= 0xFF)
            glyph_index = FT_Get_Char_Index(font_face, 0xF000 | character_code);
        return glyph_index;
    }

    void consider_codepoint(FT_ULong character_code) {
        if (character_code < 0x20 || character_code == 0x7f) return;
        FT_UInt glyph_index = resolve_glyph_index(character_code);
        if (!glyph_index ||
            FT_Load_Glyph(font_face, glyph_index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL))
            return;
        process_current_glyph();
    }

    void consider_glyph_index(FT_UInt glyph_index) {
        if (!glyph_index ||
            FT_Load_Glyph(font_face, glyph_index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL))
            return;
        process_current_glyph();
    }

    if (!font_face->charmap) {
        /* No character map at all (rare, but seen with some malformed or
         * CID-only TTFs): fall back to raw glyph-index enumeration so the
         * atlas can still be populated. Unicode ranges cannot be honored. */
        fprintf(stderr, "font has no character map, ignoring any -r ranges and using glyph "
                "order instead: %s\n", font_path);
        for (FT_Long glyph_index = 1; glyph_index < font_face->num_glyphs; glyph_index++)
            consider_glyph_index((FT_UInt)glyph_index);
    } else if (G.codepoint_range_count > 0) {
        /* Only the user-requested ranges are scanned, skipping codepoints the
         * font itself does not map to a glyph. */
        for (int i = 0; i < G.codepoint_range_count; i++) {
            codepoint_range_t range = G.codepoint_ranges[i];
            for (FT_ULong character_code = range.low; character_code <= range.high;
                 character_code++) {
                consider_codepoint(character_code);
                if (character_code == 0x10FFFF) break; /* avoid overflow at the Unicode ceiling */
            }
        }
    } else {
        FT_UInt glyph_index;
        FT_ULong character_code = FT_Get_First_Char(font_face, &glyph_index);
        while (glyph_index) {
            consider_codepoint(character_code);
            character_code = FT_Get_Next_Char(font_face, character_code, &glyph_index);
        }
    }
    free(candidate);

    int have_level = 0;
    for (int luminosity = 0; luminosity < 256; luminosity++)
        if (font_level_present[luminosity]) { have_level = 1; break; }
    if (!have_level) {
        fprintf(stderr, "font contains no renderable characters: %s\n", font_path);
        exit(1);
    }
    for (int wanted = 0; wanted < 256; wanted++) {
        int best_level = 0, best_distance = INT_MAX;
        for (int candidate = 0; candidate < 256; candidate++) {
            if (!font_level_present[candidate]) continue;
            int distance = abs(candidate - wanted);
            if (distance < best_distance) {
                best_distance = distance;
                best_level = candidate;
            }
        }
        font_level_for_luminosity[wanted] = best_level;
    }
    int populated_levels = 0;
    for (int i = 0; i < 256; i++) populated_levels += font_level_present[i];
    printf("font %s, %d glyph luminosity levels\n", font_path, populated_levels);
}

const uint8_t *glyph_atlas_bitmap_for_luminosity(uint8_t luminosity) {
    return font_atlas + (size_t)font_level_for_luminosity[luminosity] * font_cell_size;
}

size_t glyph_atlas_cell_size(void) { return font_cell_size; }

void glyph_atlas_shutdown(void) {
    free(font_atlas);
    FT_Done_Face(font_face);
    FT_Done_FreeType(font_library);
}
