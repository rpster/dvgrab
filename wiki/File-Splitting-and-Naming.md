# File Splitting and Naming

dvgrab provides flexible control over how captured video is split across files and how those files are named.

## Default Naming

Files are named using the pattern `base-NNN.ext`, where:
- `base` is the base filename argument (default: `dvgrab-`)
- `NNN` is a three-digit sequential number starting at 001
- `ext` is the format extension (e.g., `.dv`, `.avi`, `.m2t`)

## Splitting Methods

Splitting methods can be combined. A new file is created whenever any active split condition is triggered.

### By Frame Count (`-frames`)

```bash
# 25 frames per file (1 second of PAL video)
dvgrab -frames 25 foo-

# 750 frames per file (30 seconds of PAL video)
dvgrab -frames 750 foo-
```

Frame rates: PAL = 25 fps, NTSC ~= 30 fps. Set to 0 for unlimited (default).

### By File Size (`-size`)

```bash
# Max 1GB per file
dvgrab -size 1024 foo-

# Max 2GB per file (stays under ISO9660 limit)
dvgrab -size 1998 foo-
```

Size is in mebibytes (MiB). Default: 1024. Set to 0 for unlimited.

### By Duration (`-duration`)

Limits the total capture duration across all file splits for a single capture session.

```bash
# Capture exactly 1 hour
dvgrab -duration 1h foo-

# Capture 30 minutes
dvgrab -duration 30min foo-

# Capture 1 frame
dvgrab -duration smpte=:::1 foo-
```

See the [Usage Guide](Usage-Guide) for all SMIL2 time format options.

### By Recording Boundaries (`-autosplit`)

Detects when a new recording starts on the tape and creates a new file.

```bash
# Split on new recordings (detected via stream flag or timecode discontinuity)
dvgrab -autosplit foo-

# Split on gaps longer than 1 hour
dvgrab -autosplit=3600 foo-

# Split on gaps longer than 8 hours (one file per day)
dvgrab -autosplit=28800 foo-
```

Without the optional seconds argument, dvgrab splits on either a new-recording flag in the DV stream or a timecode discontinuity (anything backwards or more than one second gap). With the seconds argument, only time gaps exceeding that threshold trigger a split, and the stream flag is ignored.

### Collection-Based Splitting (`-csize`)

Splits files into collections that fit within a size limit, useful for archiving to DVD.

```bash
# Collections of ~4.4GB (DVD size), files up to 2GB, min 10MB per file
dvgrab -autosplit -size 1998 -csize 4400 -cmincutsize 10 foo-
```

- `-csize NUM` -- Start a new collection when files would exceed NUM MiB
- `-cmincutsize NUM` -- If a split would occur within NUM MiB of the collection end, start the new collection early to avoid tiny files

## Filename Schemes

Alternative naming schemes replace the sequential number with time information.

### Recording Timestamp (`-timestamp`)

Embeds the recording date and time from the DV stream into the filename.

```bash
dvgrab -timestamp foo-
# Produces: foo-2024.01.15_14-30-22.avi
```

### Timecode (`-timecode`)

Embeds the first frame's timecode into the filename.

```bash
dvgrab -timecode foo-
# Produces: foo-00:15:30:12.avi
```

### System Time (`-timesys`)

Uses the system clock date/time instead of the recording date. Useful with converter devices that don't update the DV recording timestamp.

```bash
dvgrab -timesys foo-
```

These can be combined: `-timecode -timestamp` uses both in the filename.

## SRT Subtitle Generation (`-srt`)

Generates subtitle files alongside each video file containing the recording date and time.

```bash
dvgrab -srt foo-
```

For each video file (e.g., `foo-001.m2t`), two subtitle files are created:

- `foo-001.srt0` -- Timing based on running time from file start (for transcoded formats like AVI)
- `foo-001.srt1` -- Timing based on the camera's timecode (compatible with mplayer)

## Every Nth Frame (`-every`)

Capture only every Nth frame, discarding the rest.

```bash
# Capture every 5th frame
dvgrab -every 5 foo-
```
