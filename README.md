### Build
The source lives in `src/`, split into one module per pipeline stage
(capture, analysis, visualization, display) plus shared config/pipeline
headers. Build with:
```
$ make
```
which is equivalent to compiling and linking every `src/*.c` file with:
```
$ gcc -O3 -march=native -pthread -Wall -Wextra -Werror \
      $(pkg-config --cflags libdrm freetype2) -c src/<each>.c -o src/<each>.o
$ gcc -O3 -march=native -pthread -Wall -Wextra -Werror \
      $(pkg-config --cflags libdrm freetype2) -o cellavg src/*.o \
      $(pkg-config --libs libdrm freetype2) -ljpeg
```

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
- `display.{h,c}` - DRM/KMS scanout + page flipping
- `main.c` - wires the modules together

New analysis or visualization styles can be added by implementing the
`frame_analyzer_t` / `visualizer_t` interfaces in a new module pair without
touching capture, display, or each other.

### CLI options
```
$ ./cellavg -h
usage: ./cellavg [-v /dev/videoN] [-d /dev/dri/cardN] [-s CAPWxCAPH] [-c CELLWxCELLH] [-o OUTWxOUTH] [-t workers] [-f FONT] [-r RANGE[,RANGE...]] [-F focusvalue (manual focus_absolute, disables autofocus; omit for autofocus)] [-g [FGHEX:BGHEX] (luminosity, or bichrome e.g. -g 11ee11:1111ee)] [-m (disable mirror, default mirrored)] [-p [PARAM_NAME|@PARAM_NAME|PARAM_NAME=value[,PARAM_NAME=value...]]]
RANGE: uHH-uHH, uHHHH-uHHHH, uHHHHHH-uHHHHHH, or U+HH-U+HH notation
```

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
