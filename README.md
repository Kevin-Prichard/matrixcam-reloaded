### Build
The source lives in `src/`, split into one module per pipeline stage
(capture, analysis, visualization, display) plus shared config/pipeline
headers. SLIC additionally requires the fast-slic checkout at
`candidate-dependencies/fast-slic`. Build with:
```
$ make
```
The Makefile compiles the C application and the fast-slic C++ sources,
enabling the matching AVX2 or Neon implementation when available in the
compiler target.

### Source layout
- `config.{h,c}` - CLI parsing and shared tunables (`G`)
- `capture.{h,c}` - V4L2 webcam frame source
- `pipeline.{h,c}` - generic capture -> analyzer worker pool + stats, built
  around the abstract `frame_analyzer_t` interface
- `cell_analyzer.{h,c}` - concrete analyzer: JPEG decode + per-cell color
  averaging
- `glyph_atlas.{h,c}` - FreeType glyph bitmap atlas
- `visualizer.h` - abstract analysis-grid -> framebuffer interface
- `glyph_visualizer.{h,c}` - concrete visualizer: colored glyph rendering
- `delaunay.{h,c}` - reusable bounded-workspace Delaunay triangulator
- `delaunay_visualizer.{h,c}` - image-colored, edge-biased triangle mosaic
- `slic_analyzer.{h,cpp}` - fast-slic JPEG decode and superpixel assignment
- `slic_visualizer.{h,c}` - threaded SLIC superpixel rendering
- `display.{h,c}` - DRM/KMS scanout + page flipping
- `main.c` - wires the modules together

New analysis or visualization styles can be added by implementing the
`frame_analyzer_t` / `visualizer_t` interfaces in a new module pair without
touching capture, display, or each other.

### CLI options
```
$ ./cellavg -h
usage: ./cellavg [-v /dev/videoN] [-d /dev/dri/cardN] [-s CAPWxCAPH] [-c CELLWxCELLH] [-o OUTWxOUTH] [-t workers] [-f FONT] [-r RANGE[,RANGE...]] [-F focusvalue] [-g [FGHEX:BGHEX]] [-m] [-p [camera parameters]] [-V glyph|delaunay|slic] [-a POINTS[,EDGE_WEIGHT[,SEED]]|SLIC_PARAMETER[=VALUE]]
RANGE: uHH-uHH, uHHHH-uHHHH, uHHHHHH-uHHHHHH, or U+HH-U+HH notation
```

The glyph visualizer remains the default. Select the Delaunay low-poly
visualizer with `-V delaunay`; its `-a` tuple sets the point budget,
edge-sampling bias, and deterministic seed (defaults: `600,1.0,1`):
```
$ sudo ./cellavg -V delaunay -a 900,1.5,42 -s 1280x720 -o 1280x720
```
In Delaunay mode, `-c`, `-f`, `-r`, and `-g` are ignored. The analyzer uses
its fixed 8x8-pixel sampling cells; `-d`, `-s`, and `-o` retain their normal
camera, capture-resolution, and output-resolution meanings.

Select the SLIC superpixel visualizer with `-V slic`. It uses the native
fast-slic C++ implementation from `candidate-dependencies/fast-slic`; each
pipeline worker segments frames while separate render workers scale and draw
the superpixels. SLIC ignores `-c`, `-f`, `-r`, and `-g`, and accepts these
`-a` parameters (defaults: `clusters=400`, `iterations=5`,
`sample_stride=3`, `compactness=10`, `downscale=2`):
```
$ ./cellavg -V slic -a clusters=700,iterations=4,sample_stride=2 -s 640x480 -o 1280x720
$ ./cellavg -V slic -a
$ ./cellavg -V slic -a clusters
$ ./cellavg -V slic -a @clusters
```
Parameter bounds are `clusters=16..65534`, `iterations=1..100`,
`sample_stride=1..16`, `compactness=1..100`, and `downscale` of 1, 2, 4, or 8 (the JPEG is decoded
at 1/N size and segmented at that resolution, which is much faster on small
boards). The requested cluster count
must also fit within the actual captured frame. `-d`, `-F`, `-p`, `-s`, and
`-o` retain their usual device, focus, camera-control, capture-size, and
output-size meanings.

Run the Delaunay geometry and SLIC analyzer/render tests with `make test`.

### Camera controls
V4L2 camera controls can be inspected before startup or set at startup with
`-p`. Reporting opens the configured camera, prints to stdout, and exits;
assignments are applied before frame capture begins. Repeat `-p` to set
multiple controls, and use comma-separated assignments to set several at
once:
```
$ ./cellavg -p
$ ./cellavg -p brightness
$ ./cellavg -p @brightness
$ ./cellavg -p brightness=20 -p contrast=40
$ ./cellavg -p brightness=20,contrast=40,wb_auto=1
```
Supported parameter names are `autoexp`, `backlight`, `brightness`,
`contrast`, `exp_dyn`, `exp_abs`, `gain`, `gamma`, `hue`, `power`, `sat`,
`sharp`, `wb_auto`, and `wb_temp`. Only controls exposed by the camera appear
in the no-argument listing. Values are integers; setting a value outside the
driver-reported range or menu choices is an error. The app also exposes
`capture_camera_control_{query,get,set}` and reporting helpers in `capture.h`
for runtime control access.

### ## Run
```
# JP (one code page, try others and document)
$ sudo ./cellavg -s 1920x1080 -c 16x16 -o 1920x1080 -F 127 -f fonts/NotoSansJP-Regular.ttf -r u2F00-u2FFF -g 119911:112211

# Emojis (one code page, try others and document)
$ sudo ./cellavg -s 1920x1080 -c 32x32 -o 1920x1080 -F 127 -f fonts/NotoEmoji-Regular.ttf -r u1000-u2fff
```
