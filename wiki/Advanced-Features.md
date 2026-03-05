# Advanced Features

## HDV Capture

dvgrab automatically detects whether a FireWire device is sending DV or HDV (MPEG2 Transport Stream) data, unless `-noavc`, `-input`, or `-stdin` is used.

```bash
# Automatic HDV detection
dvgrab -autosplit myfilm-

# Explicit HDV format
dvgrab -format hdv -autosplit myfilm-

# Digital TV settop box (requires connection management)
dvgrab -format mpeg2 -guid 1
```

### JVC P25 Mode

JVC cameras recording in P25 mode produce streams that need post-processing for interoperability:

```bash
dvgrab -jvc-p25 -format hdv myfilm-
```

This removes the `repeat_first_field` flag and sets the frame rate to 25 fps.

### HDV Debug Info

```bash
dvgrab -debug all -format hdv myfilm-
```

Debug types: `all`, `pat`, `pmt`, `pids`, `pid=N`, `pes`, `packet`, `video`, `sonya1`

## USB UVC Device Capture

dvgrab supports USB Video Class (UVC) DV devices via the `uvcvideo` kernel module (V4L2).

```bash
# Default device (/dev/video)
dvgrab -v4l2 myfilm-

# Specific device
dvgrab -v4l2 -input /dev/video1 myfilm-
```

**Limitations:** USB UVC mode has no AV/C VTR control, so interactive mode transport commands (play, pause, rewind, etc.) will not work. Capture control (`c`/`s` keys) still functions.

## Lockstep Redundant Capture

Lockstep mode enables multiple machines to capture from the same FireWire device simultaneously, producing identically named files with the same content.

```bash
# On each machine, use the same base name and options:
dvgrab -lockstep -frames 750 -timecode myfilm-
```

- Capture is aligned to multiples of `-frames` based on timecode
- Use `-timecode` so each machine generates the same filenames
- `-lockstep_maxdrops NUM` -- Close file and skip to next interval if NUM consecutive frames are dropped (-1 = unlimited, default)
- `-lockstep_totaldrops NUM` -- Close file and skip to next interval if NUM total frames are dropped (-1 = unlimited, default)

## Record Modes

### Record-Only (`-recordonly`)

Only captures frames when the camcorder is actively recording (not paused in record mode). The camera operator controls when capture occurs.

```bash
dvgrab -recordonly -autosplit myfilm-
```

### Record-Start (`-record-start`)

Waits for the device to enter record transport state before beginning capture. Does not send a play command. When the device stops recording, dvgrab waits for the next recording session, saving each session to a new file. Ctrl+C to exit.

```bash
dvgrab -record-start -autosplit myfilm-
```

This implies `-recordonly` and requires AV/C (incompatible with `-noavc`).

## Pipe Integration

### Reading from stdin

dvgrab can process raw DV or HDV data from other programs:

```bash
# From another DV producer
ffmpeg -i input.mp4 -f rawvideo - | dvgrab -stdin -format dv2 output-

# Using -input with dash
dvgrab -input - -format dv2 output-
```

### Writing to stdout

A trailing `-` forces raw output to stdout. dvgrab also writes to stdout automatically when it detects a pipe:

```bash
# Pipe to a media player
dvgrab - | mplayer -

# Simultaneous capture and preview
dvgrab -i myfilm- | xine -D stdin://#demux:rawdv
```

## Multi-Device Capture

Select a specific device when multiple DV devices are on the bus:

```bash
# Select by GUID (see /sys/bus/ieee1394/devices/)
dvgrab -guid 0x00a0b1c2d3e4f5 myfilm-

# Auto-discover with peer-to-peer connection
dvgrab -guid 1 myfilm-
```

Most devices run at 100 Mbps, limiting the bus to two simultaneous DV streams. Devices supporting 200 or 400 Mbps allow more concurrent streams. Multiple host adapters are transparently supported with `-guid`.

## Webcam Mode

Capture a single JPEG frame continuously for webcam use:

```bash
dvgrab -jpeg-overwrite -jpeg-width 320 -jpeg-height 240 -duration smpte=1 webcam.jpeg
```

Use `-jpeg-temp` to write to a temporary file and atomically rename, avoiding partial reads by the web server:

```bash
dvgrab -jpeg-overwrite -jpeg-temp /tmp/webcam.tmp -jpeg-width 320 -jpeg-height 240 webcam.jpeg
```

## Real-Time Scheduling

When run with superuser permissions, dvgrab:

- Enables real-time scheduling policy for reduced frame drops
- Locks all memory resident to prevent paging

```bash
sudo dvgrab -autosplit myfilm-
```

This is recommended for high-reliability capture scenarios.
