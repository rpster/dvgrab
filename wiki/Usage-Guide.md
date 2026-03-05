# Usage Guide

## Synopsis

```
dvgrab [options] [base] [-]
```

- **base** -- Base filename for output files. Files are named `base-NNN.ext` where `NNN` is a sequential number and `ext` is the format extension. Default: `dvgrab-`
- **`-`** (trailing dash) -- Forces raw DV/HDV output to stdout. dvgrab also outputs to stdout automatically when it detects a pipe or redirect.

If `base` includes a file extension, dvgrab infers the output format from it.

## Options Reference

Options longer than a single character can use one or two leading hyphens. Arguments can be separated by space or `=`.

### Capture Control

| Option | Description |
|--------|-------------|
| `-a[n]`, `-autosplit[=n]` | Split on new recording detection. Optional `n` sets gap threshold in seconds (e.g., `-autosplit=3600` for 1-hour gaps) |
| `-buffers NUM` | Number of internal frame buffers (default: 100) |
| `-d`, `-duration TIME` | Maximum total capture duration in SMIL2 time format |
| `-every N` | Capture every Nth frame only (default: 1 = all frames) |
| `-F`, `-frames NUM` | Maximum frames per file before splitting. 0 = unlimited (default: 0) |
| `-s`, `-size NUM` | Maximum file size in MiB. 0 = unlimited (default: 1024) |
| `-showstatus` | Display capture status during capture, updated per frame |

### Device Selection

| Option | Description |
|--------|-------------|
| `-card NUM` | Select FireWire card number (default: first card with a camera) |
| `-channel NUM` | Isochronous channel to listen on (default: 63) |
| `-guid HEX` | Select device by GUID in hex. Use `1` to auto-discover with peer-to-peer connection |
| `-I`, `-input FILE` | Read from file instead of FireWire. Use `-` for stdin |
| `-stdin` | Read DV/HDV stream from stdin pipe |
| `-noavc` | Disable AV/C VTR control (use when camera is in camera mode) |
| `-nostop` | Don't send AV/C stop command on exit |
| `-V`, `-v4l2` | Capture from USB UVC device via V4L2 (default device: `/dev/video`) |

### Output Format

| Option | Description |
|--------|-------------|
| `-f`, `-format TYPE` | Set output format (default: raw). See [Output Formats](Output-Formats) |
| `-opendml` | Enable OpenDML extensions for dv2 AVI files >1GB |
| `-24p` | Set QuickTime frame rate to 24 fps |
| `-24pa` | Remove 2:3:3:2 pulldown for 24p Advanced (QuickTime only) |

### File Naming

| Option | Description |
|--------|-------------|
| `-t`, `-timestamp` | Use recording date/time in filename |
| `-timecode` | Use first frame's timecode in filename |
| `-timesys` | Use system date/time in filename |

### Record Modes

| Option | Description |
|--------|-------------|
| `-i`, `-interactive` | Interactive mode with camera VTR control. See [Interactive Mode](Interactive-Mode) |
| `-r`, `-recordonly` | Only capture when device is actively recording (not paused) |
| `-record-start` | Wait for device to enter record mode before capturing. Re-arms between sessions. Implies `-recordonly` |
| `-rewind` | Rewind tape to beginning before capture |

### File Splitting

| Option | Description |
|--------|-------------|
| `-csize NUM` | Split when collection of files exceeds NUM MiB (for DVD archival) |
| `-cmincutsize NUM` | Minimum file size in MiB for collection split (default: 0) |

See [File Splitting & Naming](File-Splitting-and-Naming) for details.

### JPEG Options

Requires libdv and libjpeg. DV input only (not HDV).

| Option | Description |
|--------|-------------|
| `-jpeg-deinterlace` | Deinterlace by doubling lines of upper field |
| `-jpeg-height NUM` | Scale output height (1-2048) |
| `-jpeg-width NUM` | Scale output width (1-2048) |
| `-jpeg-quality NUM` | JPEG compression level (0-100, default: 75) |
| `-jpeg-overwrite` | Overwrite same file instead of creating sequence |
| `-jpeg-temp NAME` | Use temporary file, rename when done (useful for webcam) |

### HDV Options

| Option | Description |
|--------|-------------|
| `-jvc-p25` | Fix JVC P25 mode streams: remove repeat_first_field, set fps to 25 |
| `-debug TYPE` | Display HDV debug info. Types: `all`, `pat`, `pmt`, `pids`, `pid=N`, `pes`, `packet`, `video`, `sonya1` |

### Lockstep Options

| Option | Description |
|--------|-------------|
| `-lockstep` | Align capture to multiples of `-frames` based on timecode |
| `-lockstep_maxdrops NUM` | Max consecutive dropped frames before closing file. -1 = unlimited (default) |
| `-lockstep_totaldrops NUM` | Max total dropped frames before closing file. -1 = unlimited (default) |

### Miscellaneous

| Option | Description |
|--------|-------------|
| `-srt` | Generate SRT subtitle files with recording date/time |
| `-h`, `-help` | Show help and exit |
| `-v`, `-version` | Show version and exit |

## SMIL2 Time Formats

The `-duration` option accepts SMIL2 MediaClipping Time values:

- `XXX[.Y]h` -- hours
- `XXX[.Y]min` -- minutes
- `XXX[.Y][s]` -- seconds
- `XXXms` -- milliseconds
- `[[HH:]MM:]SS[.ms]` -- clock format
- `smpte=[[[HH:]MM:]SS:]FF` -- SMPTE timecode

## Examples

```bash
# Basic capture
dvgrab foo-

# One second per file (PAL: 25 fps)
dvgrab -frames 25 foo-

# Autosplit with 30-sec chunks and timestamp filenames
dvgrab -autosplit -frames 750 -timestamp foo-

# DVD archival: 2GB file limit, 4.4GB collection limit
dvgrab -autosplit -size 1998 -csize 4400 -cmincutsize 10 foo-

# HDV capture with autosplit
dvgrab -format hdv -autosplit

# Digital TV settop box via GUID
dvgrab -format mpeg2 -guid 1

# Single JPEG frame for webcam
dvgrab -jpeg-over -jpeg-w=320 -jpeg-h=240 -d smpte=1 webcam.jpeg

# USB UVC device capture
dvgrab -v4l2

# USB UVC with specific device
dvgrab -v4l2 -input /dev/video1

# HDV capture, split on 8-hour gaps, with subtitles
dvgrab -format=hdv -autosplit=28800 -srt foo-
```
