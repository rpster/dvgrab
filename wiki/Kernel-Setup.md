# Kernel Setup

## Why a Custom Kernel Is Required

Standard Raspberry Pi OS images do not include FireWire (IEEE 1394) kernel support. The FireWire subsystem modules (`firewire-core` and `firewire-ohci`) are not built, so `modprobe` will fail on a stock kernel. You must compile a custom kernel with FireWire support enabled before the system can communicate with the PCIe FireWire card and DV cameras.

## Required Kernel Config Options

Enable the following options in your kernel configuration (via `menuconfig` or by editing `.config` directly):

| Option | Menu Path | Value |
|--------|-----------|-------|
| `CONFIG_FIREWIRE` | Device Drivers → IEEE 1394 (FireWire) support | `m` or `y` |
| `CONFIG_FIREWIRE_OHCI` | Device Drivers → IEEE 1394 (FireWire) support → OHCI-1394 controllers | `m` or `y` |

Using `=m` (module) is recommended so the modules load on demand via `modprobe`, which is what the install script expects. Using `=y` (built-in) also works but the modules will always be loaded regardless of whether FireWire hardware is present.

## Building the Kernel

**As of 2026-03-10: Please use the official Pi Lite image from 2025-05-13 (Bookworm 6.12.25) for the best compatibility**

Follow the official Raspberry Pi kernel building guide:

**https://www.raspberrypi.com/documentation/computers/linux_kernel.html**

The general workflow is:

1. Clone the Raspberry Pi kernel source
2. Configure with `make menuconfig` and enable the FireWire options above
3. Build the kernel and modules
4. Install the new kernel and modules to the Pi

After installing the custom kernel and rebooting, verify that FireWire modules are available:

```bash
modprobe firewire-core
modprobe firewire-ohci
lsmod | grep firewire
```

You should see both `firewire_core` and `firewire_ohci` in the output. The [install script](Installation) will load these modules automatically and persist them in `/etc/modules`.
