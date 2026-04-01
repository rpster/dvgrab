# Troubleshooting

## Permissions

### FireWire Access

Accessing FireWire devices requires appropriate permissions. Either run as root or ensure your user is in the correct group:

```bash
# Run as root
sudo dvgrab myfilm-

# Or add your user to the video group (distribution-dependent)
sudo usermod -aG video $USER
# Log out and back in for group changes to take effect
```

### Real-Time Scheduling

Running as root enables real-time scheduling and memory locking, which helps prevent dropped frames. If you see frame drops running as a regular user, try running with `sudo`.

## Kernel Modules

Ensure the required kernel modules are loaded:

```bash
# Newer kernels (firewire stack)
sudo modprobe firewire-core
sudo modprobe firewire-ohci

# Older kernels (legacy stack)
sudo modprobe raw1394
sudo modprobe ohci1394
```

For USB UVC devices:

```bash
sudo modprobe uvcvideo
```

Check that your device is detected:

```bash
# FireWire devices
ls /sys/bus/ieee1394/devices/

# USB UVC devices
ls /dev/video*
```

## Dropped Frames

If you experience dropped frames:

1. **Increase buffer count**: Use `-buffers NUM` (default: 100). More buffers help absorb I/O delays.
   ```bash
   dvgrab -buffers 200 myfilm-
   ```

2. **Run as root**: Enables real-time scheduling and memory locking.

3. **Reduce disk I/O**: Write to a fast, dedicated disk. Avoid capturing to the system drive.

4. **Close other applications**: Reduce CPU and I/O contention.

5. **Use raw format**: Raw DV (`-format raw`) has the least processing overhead.

## Camera Mode vs VTR Mode

When your camera is in **camera mode** (live viewfinder, not playing tape), use `-noavc` to prevent dvgrab from sending AV/C commands. Without `-noavc`, a play command could tell the camera to **start recording** over your tape.

```bash
# Safe for camera mode (live capture)
dvgrab -noavc myfilm-
```

In **VTR mode** (playing back tape), AV/C is safe and useful -- dvgrab will send play/stop commands automatically.

## Large File Issues

### AVI Files Over 1GB

AVI Type 2 (`-format dv2`) files are limited to 1GB by default. Enable OpenDML for larger files:

```bash
dvgrab -format dv2 -opendml -size 4000 myfilm-
```

AVI Type 1 (`-format dv1`) and raw DV inherently support files larger than 1GB.

### DVD Archival (ISO9660 Limit)

The Linux ISO9660 filesystem driver has a 2GB file size limit. For DVD archival, keep files under 2GB:

```bash
dvgrab -autosplit -size 1998 -csize 4400 -cmincutsize 10 myfilm-
```

## Device Not Found

If dvgrab cannot find your camera:

1. Check physical connections (FireWire cable, power)
2. Verify kernel modules are loaded (see above)
3. Try specifying the card manually: `dvgrab -card 0 myfilm-`
4. For multiple devices, use `-guid` to select by GUID
5. Ensure the camera is powered on and in VTR or camera mode

## DVCPRO Notes

### DVCPRO50 / DVCPRO HD Not Detected

DVCPRO50 and DVCPRO HD devices use raw isochronous mode rather than standard AV/C isochronous. If your DVCPRO device is not detected:

1. Ensure the device is powered on and connected via FireWire before starting dvgrab
2. Try specifying the device with `-guid` if auto-detection fails
3. DVCPRO format is detected automatically from the stream -- no `-format` flag is needed

### JPEG Output Not Available with DVCPRO HD

The JPEG output format requires libdv for frame decoding, and libdv does not support DVCPRO HD frames. Use raw, AVI, or QuickTime output formats instead.

### DVCPRO HD Files Use .mxf Extension

DVCPRO HD recordings automatically use a `.mxf` file extension instead of `.dv` when the raw output format is selected. This is expected behavior for proper container identification.

## Known Fixes in Recent Versions

### Year 2025 Date Bug

Versions prior to 3.5.1 incorrectly displayed recordings from 2025 onwards as 1925. Fixed in v3.5.1.

### Ctrl+C Handling

Ctrl+C during device discovery could leave the process in an unresponsive state. Fixed to properly terminate the process.

### Device Disconnect

On device disconnect (cable unplugged, camera powered off), dvgrab now exits cleanly rather than hanging.

### Corrupt DV Data

Frames with corrupt DV data are now discarded instead of causing crashes, improving reliability with old or damaged tapes.
