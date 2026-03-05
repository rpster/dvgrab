# Output Formats

dvgrab supports several output formats, selected with `-format TYPE` or inferred from the base filename extension.

## Format Comparison

| Format Flag | Extension | Description | Requires |
|-------------|-----------|-------------|----------|
| `raw` | `.dv` | Raw DV stream (default) | -- |
| `dif` | `.dif` | Raw DV stream (DIF naming) | -- |
| `dv1` | `.avi` | AVI Type 1 (integrated DV track) | -- |
| `dv2` / `avi` | `.avi` | AVI Type 2 (separate audio track) | -- |
| `qt` / `mov` | `.mov` | QuickTime DV movie | libquicktime |
| `jpeg` / `jpg` | `.jpeg` | Sequence of JPEG images | libdv + libjpeg |
| `mpeg2` / `hdv` | `.m2t` | MPEG-2 Transport Stream (HDV) | -- |

## Format Details

### Raw DV (`raw`)

Stores the DV data stream unmodified with a `.dv` extension. Compatible with Apple QuickTime, many GNU/Linux tools, and video editors that accept raw DV files. This is the default format.

### DIF (`dif`)

Identical to raw DV but uses a `.dif` extension. This naming convention is used by MainConcept MainActor5 and similar applications.

### AVI Type 1 (`dv1`)

Stores a single, integrated DV track in an AVI container. The DV format natively interleaves audio and video, so Type 1 files are smaller than Type 2. However, some applications cannot read this format. Type 1 AVI inherently supports files larger than 1GB.

### AVI Type 2 (`dv2` / `avi`)

Stores separate DV video and audio tracks in an AVI container. More widely compatible with video editing applications. When libdv is available at compile time, it produces higher quality audio with error handling (avoiding audio "bleeps" from tape errors).

By default, Type 2 AVI files are limited to 1GB. Use `-opendml` to enable OpenDML extensions for files larger than 1GB. dvgrab automatically enables OpenDML when `-frames` or `-size` would produce files exceeding 1GB.

### QuickTime (`qt` / `mov`)

Requires libquicktime at compile time. Creates a QuickTime movie with a separate audio track in twos complement format. Supports the `-24p` and `-24pa` options for 24 fps modes.

### JPEG (`jpeg` / `jpg`)

Requires both libdv and libjpeg at compile time. Creates a sequence of JPEG still images from DV input. **Does not work with HDV (MPEG2-TS) input.**

File naming uses two number parts: the first increments on scene changes, the second increments with each frame. Use `-jpeg-overwrite` to write to the same file each frame (useful for webcam applications).

JPEG-specific options:

- `-jpeg-quality NUM` -- Compression quality 0-100 (default: 75)
- `-jpeg-deinterlace` -- Cheap deinterlace by doubling upper field lines (50% resolution loss)
- `-jpeg-width NUM` / `-jpeg-height NUM` -- Scale output (1-2048). Both dimensions must be either smaller or larger than the native frame size.
- `-jpeg-overwrite` -- Continuously update same file
- `-jpeg-temp NAME` -- Write to temp file and rename when done

Square pixel sizes for scaling:
- **NTSC** (720x480): 800x600, 640x480, 320x240
- **PAL** (720x576): 768x540, 384x270

### MPEG-2 Transport Stream (`mpeg2` / `hdv`)

For HDV camcorders and digital TV settop boxes. Stores the MPEG-2 Transport Stream data with a `.m2t` extension. See [Advanced Features](Advanced-Features) for HDV-specific options.

## Auto-Detection from Filename

If the base filename includes an extension, dvgrab infers the format:

| Extension | Format |
|-----------|--------|
| `.avi` | dv2 (AVI Type 2) |
| `.dv` | raw |
| `.dif` | dif |
| `.mov` | QuickTime |
| `.jpg` / `.jpeg` | JPEG |
| `.m2t` | MPEG-2 TS |
