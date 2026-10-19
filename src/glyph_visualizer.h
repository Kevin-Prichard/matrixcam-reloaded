#ifndef CELLAVG_GLYPH_VISUALIZER_H
#define CELLAVG_GLYPH_VISUALIZER_H
#include "visualizer.h"

/* Renders a cell_analyzer grid (see cell_analyzer.h) as colored monospace
 * glyphs: each cell is drawn with the font glyph whose coverage best
 * matches its luminosity, in its averaged foreground/background colors (or
 * grayscale/bichrome, per G.gray/G.bichrome). Uses a thread pool sized to
 * the available CPU cores to split work by scanline range. Requires
 * glyph_atlas_init() to have been called first. */
extern const visualizer_t glyph_visualizer;

#endif
