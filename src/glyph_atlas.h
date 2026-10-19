#ifndef CELLAVG_GLYPH_ATLAS_H
#define CELLAVG_GLYPH_ATLAS_H
#include <stdint.h>
#include <stddef.h>

/* Loads G.font_path (or a default system font) at G.cw x G.ch pixels and
 * builds a 256-level luminosity-to-glyph-bitmap atlas honoring any -r
 * codepoint ranges in G.codepoint_ranges. Exits the process on failure. */
void glyph_atlas_init(void);

/* Returns the G.cw * G.ch byte alpha-coverage glyph bitmap (row-major, one
 * byte per pixel) for the glyph level closest to the given 0-255
 * luminosity. */
const uint8_t *glyph_atlas_bitmap_for_luminosity(uint8_t luminosity);

/* Bytes per glyph bitmap (G.cw * G.ch). */
size_t glyph_atlas_cell_size(void);

void glyph_atlas_shutdown(void);

#endif
