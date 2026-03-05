# Interactive Mode

Interactive mode provides direct keyboard control over the camcorder's VTR (Video Tape Recorder) transport and capture operations.

## Entering Interactive Mode

```bash
dvgrab -interactive myfilm-
# or
dvgrab -i myfilm-
```

In non-interactive mode (default), dvgrab immediately starts capturing and stops on Ctrl+C or when a limit is reached. In interactive mode, you control when capture starts and stops, and can operate the camcorder's tape transport.

## Key Reference

### Transport Control

| Key | Action |
|-----|--------|
| `p` | Play |
| `Space` | Toggle play/pause |
| `k` | Pause |
| `h` | Reverse play |

### Tape Navigation

| Key | Action |
|-----|--------|
| `a` | Rewind (stop + rewind) |
| `z` | Fast forward (stop + fast forward) |
| `j` | Backward scan (pause + rewind) |
| `l` | Forward scan (pause + fast forward) |

### Shuttle / Trickplay

| Key | Speed |
|-----|-------|
| `1` | -14 (fastest reverse) |
| `2` | -11 |
| `3` | -8 |
| `4` | -4 |
| `5` | -1 (slow reverse) |
| `6` | +1 (slow forward) |
| `7` | +4 |
| `8` | +8 |
| `9` | +11 |
| `0` | +14 (fastest forward) |

### Capture Control

| Key | Action |
|-----|--------|
| `c` | Start capture |
| `s` or `Esc` | Stop capture (or stop VTR if not capturing) |
| `q` | Quit dvgrab |
| `?` | Show help |

## Simultaneous Stdout Output

In interactive mode, dvgrab can pipe raw DV to stdout while simultaneously capturing to a file. This happens automatically when stdout is piped or redirected:

```bash
# Preview in xine while capturing
dvgrab -i | xine -D stdin://#demux:rawdv

# Preview in mplayer while capturing
dvgrab -i | mplayer -
```

## Multiple Capture Sessions

In interactive mode, each press of `c` (start) and `s` (stop) constitutes a capture session. The `-duration` limit applies per session, so you can have multiple timed sessions in one interactive run.

## Limitations

- Interactive mode requires AV/C support. It is almost useless with USB UVC devices (`-v4l2`) since they don't support AV/C VTR control.
- The `-rewind` option does not apply in interactive mode (rewind manually with `a`).
