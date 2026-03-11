# dvgrab

**dvgrab** is a command-line utility for capturing DV and HDV (MPEG2-TS) video and audio data from digital camcorders via FireWire (IEEE 1394). It also supports USB Video Class (UVC) devices and can read raw DV/HDV streams from pipes.

## Features

- Capture DV and HDV video from FireWire camcorders
- USB UVC device support via V4L2
- Multiple output formats: raw DV, AVI (Type 1 & 2), QuickTime, JPEG sequences, MPEG2-TS
- Interactive mode for camcorder VTR control (play, pause, rewind, fast-forward)
- Automatic file splitting by size, frame count, duration, or new recordings
- Collection-based splitting for DVD archival
- Timecode, timestamp, and system-time-based filenames
- SRT subtitle generation with recording date/time
- Lockstep mode for redundant multi-machine capture
- Record-start mode for camera-operator-controlled capture
- Pipe integration with other tools (stdin/stdout)
- Multi-device capture via GUID selection

## Quick Start

```bash
# Capture from default FireWire device
dvgrab myfilm-

# Capture with autosplit on new recordings, timestamp filenames
dvgrab -autosplit -timestamp myfilm-

# Capture HDV
dvgrab -format hdv -autosplit myfilm-

# Capture from USB UVC device
dvgrab -v4l2 myfilm-
```

## Wiki Contents

- [Kernel Setup](Kernel-Setup) -- Building a custom kernel with FireWire support
- [Installation](Installation) -- Dependencies, building, and installing
- [Usage Guide](Usage-Guide) -- Command-line options and examples
- [Output Formats](Output-Formats) -- Supported file formats in detail
- [File Splitting & Naming](File-Splitting-and-Naming) -- Splitting strategies and filename conventions
- [Interactive Mode](Interactive-Mode) -- Camera VTR control keys
- [Advanced Features](Advanced-Features) -- HDV, USB, lockstep, pipes, and more
- [Troubleshooting](Troubleshooting) -- Common problems and solutions

## License

dvgrab is released under the GNU General Public License v2 (GPLv2).

## Links

- [GitHub Repository](https://github.com/rpster/dvgrab)
- [Issue Tracker](https://github.com/rpster/dvgrab/issues)
