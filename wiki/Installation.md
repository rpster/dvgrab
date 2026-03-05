# Installation

## Dependencies

### Required

| Library | Minimum Version | Purpose |
|---------|----------------|---------|
| libraw1394 | 1.1.0 | IEEE 1394 (FireWire) communication |
| libavc1394 | 0.5.1 | AV/C camcorder VTR control |
| libiec61883 | 1.0.0 | IEC 61883 DV/MPEG2-TS streaming protocol |
| pthread | -- | POSIX threads |

### Optional

| Library | Minimum Version | Purpose |
|---------|----------------|---------|
| libdv | 0.103 | DV frame decoding, improved dv2 AVI audio, JPEG output |
| libquicktime | 0.9.5 | QuickTime (.mov) format support |
| libjpeg | -- | JPEG still image output (also requires libdv) |
| Linux V4L2 headers | -- | USB UVC device support (`linux/videodev2.h`) |

## Installing Dependencies

### Debian / Ubuntu

```bash
sudo apt-get install \
    libraw1394-dev libiec61883-dev libavc1394-dev \
    libdv4-dev libjpeg-dev libquicktime-dev \
    autoconf automake gcc g++ make
```

### Fedora / RHEL

```bash
sudo dnf install \
    libraw1394-devel libiec61883-devel libavc1394-devel \
    libdv-devel libjpeg-devel \
    autoconf automake gcc gcc-c++ make
```

## Building from Source

```bash
git clone https://github.com/rpster/dvgrab.git
cd dvgrab
autoreconf -fi
./configure
make
sudo make install
```

### Configure Options

- `--with-libdv` / `--without-libdv` -- Enable/disable libdv support (default: auto-detect)
- `--with-libquicktime` / `--without-libquicktime` -- Enable/disable QuickTime support (default: auto-detect)
- `--with-libjpeg` / `--without-libjpeg` -- Enable/disable JPEG support (default: auto-detect)
- `--prefix=PATH` -- Installation prefix (default: `/usr/local`)
- `--with-efence` -- Enable ElectricFence debugging

## Docker Cross-Compilation (Intel x86_64 on Fedora)

Create a `Dockerfile`:

```dockerfile
FROM --platform=linux/amd64 fedora:43

RUN dnf install -y \
    gcc gcc-c++ make autoconf automake \
    libraw1394-devel libiec61883-devel libavc1394-devel \
    libdv-devel libjpeg-devel \
    file \
    && dnf clean all

WORKDIR /build
COPY . .

RUN autoreconf -fi
RUN ./configure \
    --host=x86_64-linux-gnu \
    CFLAGS="-g -O2 -march=x86-64 -mtune=generic" \
    CXXFLAGS="-g -O2 -std=c++11 -march=x86-64 -mtune=generic"
RUN make -j$(nproc)
RUN file dvgrab

CMD ["cat","/dvgrab"]
```

Build and extract the binary:

```bash
docker buildx build --platform linux/amd64 -t dvgrab-builder .
docker create --name extract_container dvgrab-builder
docker cp extract_container:/build/dvgrab dvgrab
docker rm extract_container
```

The target machine may need `libdv` installed separately:

```bash
sudo dnf install libdv.x86_64
```
