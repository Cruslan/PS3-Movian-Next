# Movian Next (PlayStation 3 Modern Edition)

[![Platform](https://img.shields.io/badge/Platform-PlayStation%203-blue.svg)](https://en.wikipedia.org/wiki/PlayStation_3)
[![SDK](https://img.shields.io/badge/SDK-PSL1GHT%20v2-green.svg)](https://github.com/ps3dev/ps3dev)
[![Toolchain](https://img.shields.io/badge/Toolchain-PPU%20GCC%2013.2.0%20%2F%207.2.0-orange.svg)](https://github.com/ps3dev/ps3toolchain)
[![Architecture](https://img.shields.io/badge/Architecture-Cell%20B.E.%20%2B%20RSX-lightgrey.svg)](https://en.wikipedia.org/wiki/Cell_(processor))
[![Optimization](https://img.shields.io/badge/Optimization--mcpu%3Dcell%20--O2%20--flto-brightgreen.svg)](#compiler-optimization--performance-profile)
[![Multimedia](https://img.shields.io/badge/Core-FFmpeg%209.0%20(Lei)-blueviolet.svg)](https://ffmpeg.org/)
[![License](https://img.shields.io/badge/License-GPLv3-red.svg)](LICENSE)
[![Title ID](https://img.shields.io/badge/Title%20ID-HTSS00004-purple.svg)](https://rpcs3.net/)

**Movian Next** is an authoritative, high-performance standalone modernization of the celebrated **Movian** (formerly *Showtime*) media center, engineered exclusively for the **Sony PlayStation 3** homebrew ecosystem using the modern open-source **PSL1GHT v2** SDK and modern GCC toolchains.

Originally designed and architected by **Andreas Öman (Lonelycoder AB)**, Movian has long stood as one of the most capable and versatile multimedia entertainment hubs ever brought to homebrew platforms. This modernized edition strips away legacy multi-platform abstraction layers, purges all non-PS3 architectures, migrates the multimedia core from obsolete Libav 11 to **FFmpeg 9.0 ("Lei")**, and unifies the entire build and runtime pipeline into a hermetic, single-stage monolithic executable tailored specifically for the PlayStation 3 Cell Broadband Engine and RSX GPU.

---

## Key Highlights & Architectural Innovations

### 1. Monolithic Single-Stage Build Pipeline & Hermetic SDK
* **Monolithic Build Architecture:** Inspired by modern PS3 projects such as `PS3-Moonlight`, the multi-stage build system has been unified into a single standalone [Makefile](Makefile). Legacy configure scripts (`configure`, `configure.ps3`, `support/configure.inc`, `config.default`), external `.zip` archives, and trampoline binaries (`eboot.c`, `ziptail.c`) have been completely eliminated.
* **Hermetic Local SDK (`make prepare`):** The build system automatically detects and links against a local `./ps3dev` SDK or system toolchains. Developers can bootstrap the official nightly PSL1GHT v2 SDK and PPU GCC compiler suite with a single `make prepare` command, requiring zero root privileges.
* **Embedded In-Memory Bundles:** Shaders, flat skins, FreeType fonts, vector SVG icons, ECMAScript runtimes, and internationalization catalogs are compiled directly into the binary `.rodata` section via `support/mkbundle` and `bundle.o`. Resource queries resolve instantly via the `bundle://` virtual filesystem with zero disk lookup latency and zero ZIP overhead.
* **Streamlined Binary Footprint:** Monolithic executable linking coupled with symbol stripping, Link-Time Optimization (`-flto`), and NPDRM encryption packages the entire application into a lean ~7.6 MB standalone distribution.

### 2. Modern FFmpeg 9.0 ("Lei") Core Subsystem Migration
* **Direct FFmpeg 9.0 Architecture:** Replaced retired Libav 11 with upstream modern FFmpeg 9.0 compiled natively for the Cell PPE architecture (`ext/ffmpeg/` orchestrated via `ext/ffmpeg.mk`).
* **Modern Resampling Engine (`libswresample`):** Replaced legacy `libavresample` with modern `libswresample`, utilizing `AVChannelLayout` descriptors and real-time delay tracking.
* **Asynchronous Decoupled Decode Pipeline:** Implemented non-blocking `avcodec_send_packet()` and `avcodec_receive_frame()` loops across video, audio, image, and streaming demuxers.
* **Immutable Codec Parameters (`AVCodecParameters`):** Migrated container parsing and stream probing from deprecated `st->codec` to modern immutable `st->codecpar` structures.
* **Cell PPU Demux Optimization:** Patched demux probe routines (`ext/ffmpeg/libavformat/demux.c`) to bypass CPU-intensive software frame delay guessing on the in-order PPU, enabling instant opening of complex H.264/MPEG streams.

### 3. RSX (NV47/G70) 60 FPS Engine & Hardware FIFO Stabilization
* **Canonical Per-Frame Reset Model (`resetCommandBuffer`):** Solves historical UI jitter, font sliding, and texture distortion bugs caused by unmanaged GCM ring-buffer accumulation. Movian synchronizes with hardware VSYNC, flushes pending primitives via `rsxFinish`, and issues a hardware NV40 JUMP instruction to reset the command buffer back to base every frame.
* **Host Memory Overwrite Shield:** PSL1GHT's context callback returns `-1` on unexpected buffer boundaries, completely preventing RSX command packets from overflowing into adjacent host I/O memory where font glyph caches and atlas textures reside.
* **Zero RSX FIFO Desync:** Eliminates RSX command processor parsing errors (such as interpreting float vertex data `0xbf800000` as method headers), guaranteeing a rock-solid, tear-free 60 FPS navigation experience.
* **Defensive Primitive Filtering:** RSX rendering jobs with empty index queues (`num_indices <= 0`) are discarded before dispatch, and uniform constant boundaries are strictly validated.

### 4. Dual-Tier Video Architecture: Hardware SPU Offloading & Planar RSX Rendering
* **Hardware SPU Acceleration (`cellVdec`):** Full hardware-assisted video decoding leveraging Sony's GameOS `cellVdec` microkernel firmware (`libvdec.a`). Real-time decoding of H.264 / AVC (Baseline, Main, and High Profile up to Level 4.2 @ 1080p60) and MPEG-2 streams is offloaded entirely to the Synergistic Processing Elements (SPEs), freeing the PowerPC PPE core for system tasks.
* **Universal Software Decoding Engine:** Software playback for standard web and legacy video codecs—including MPEG-4 Part 2 (DivX / XviD), VP8, Sorenson Spark (FLV1), MJPEG, MPEG-1, DV, and WMV—is routed through FFmpeg and transferred directly to mapped RSX GDDR3 VRAM buffers. Hardware fragment shaders convert planar YUV420 to RGB at full 60 FPS.
* **PPU Single-Thread Safety Shield:** Unconditionally forces `thread_count = 1` for software video decoding on PS3, preventing PSL1GHT `libpthread.a` condition variable deadlocks (`sys_cond_wait`) and thread starvation. Software decoding runs deterministically on Movian's dedicated video thread with a 128 KB native stack.
* **Interactive Next-Gen Codec Dialog:** Playing compute-intensive next-generation streams (H.265/HEVC, AV1, VP9, VP8) triggers an interactive pre-flight confirmation modal warning the user of potential in-order PPE CPU load and dropped frames before playback initializes.
* **RSX VRAM Surface Pool Optimization:** The planar YUV allocation pool has been optimized down to 4 active surfaces, slashing VRAM consumption from ~31 MB to ~12.4 MB at 1080p and preserving contiguous graphics memory for high-resolution UI textures.

### 5. Audio Subsystem: AltiVec SIMD Acceleration & Resilient Routing
* **Cell PPE AltiVec SIMD Engine:** Audio output operates in native 48 kHz 32-bit floating-point PCM (`AV_SAMPLE_FMT_FLT`) via PSL1GHT v2 `libaudio.a`. Real-time channel layout swizzling (`vec_perm`) and volume scaling (`vec_madd`) are computed entirely in hardware vector registers on the Cell PPE.
* **DTS Surround Sound Parsing:** Audio stream probe buffering captures required DTS synchronization markers, preventing false identification as MP3 streams. Standalone `.dts` and `.dtshd` files map directly to native decoders.
* **Graceful Downmix Fallback:** When connecting to standard television setups where 8-channel surround ports fail to initialize, the audio driver automatically drops to 2-channel stereo (`AV_CH_LAYOUT_STEREO`) and applies real-time AltiVec downmixing via `libswresample`.

### 6. Image Subsystem Modernization & 4K Memory Protection
* **Universal Format Support:** Comprehensive image viewer supporting PNG, JPEG, GIF (both legacy GIF87a and animated GIF89a), WebP, DirectDraw Surface (DDS), TIFF, Truevision TGA, BMP, and vector SVG.
* **Big-Endian Palette Unpacking:** Native `AV_PIX_FMT_PAL8` decoder extracts color-indexed GIF and bitmap data directly into 32-bit `PIXMAP_RGBA`, preserving complete per-pixel alpha transparency and eliminating Big-Endian byte truncation.
* **4K/8K TLSF Heap Shield & Multi-Tier Scaler:** Protects the PS3's 96 MB application heap against out-of-memory crashes when browsing ultra-high-resolution photography. Clamps image allocation boundaries to 1080p and provides a tiered fallback ladder (`SWS_LANCZOS` &rarr; `SWS_BILINEAR` &rarr; `SWS_FAST_BILINEAR`) coupled with an emergency deterministic point-sampling downsampler.

### 7. Outward Network Hardening & Privacy by Default
* **Default Network Isolation:** All network-facing listeners and broadcasting services default to **DISABLED (OFF)**:
  * Remote Control (STPP TCP port 42000 and UDP multicast beacons) is closed until explicitly enabled in Settings.
  * UPnP / DLNA AVTransport and SSDP multicast discovery beacons (`239.255.255.250:1900`) are silenced.
  * BitTorrent peer-to-peer downloading engine defaults to inactive.
  * Metadata scrapers (TheMovieDB, TheTVDB, Last.fm) are disabled on initial run to avoid background queries during local disk navigation.
* **Defunct Service Stubbing:** Hardcoded endpoints pointing to defunct `movian.tv` infrastructure (auto-upgrader, telemetry reporting, site news popups) have been converted into zero-cost dormant stubs, preserving the code for future community services while preventing network errors.

### 8. Isolated Application Identity (Movian Next - HTSS00004)
* **Title ID `HTSS00004`:** Configured with Content-ID `UP0001-HTSS00004_00-0000000000000000`, completely isolating Movian Next from legacy Movian or M7 installations (`HTSS00003`). User settings, database files, and caches reside safely in `/dev_hdd0/game/HTSS00004/USRDIR/settings/` without configuration collisions.

---

## Compiler Optimization & Performance Profile

Movian Next is built with an aggressive, architecture-specific compiler configuration tuned specifically for the Cell Broadband Engine:

```makefile
OPTFLAGS    ?= -mcpu=cell -O2 -flto
OPTFLAGS_O3 ?= -mcpu=cell -O3 -flto
CFLAGS_cfg  += -mminimal-toc -fno-strict-aliasing
```

* **`-mcpu=cell`:** Enables the Cell PPE microarchitectural scheduling model, unlocking dual-issue instruction pairing and native 128-bit AltiVec/VMX vector instructions.
* **`-O2 -flto`:** Enables whole-program Link-Time Optimization across translation units, eliminating cross-module function call overhead and pruning unused dead code.
* **Selective `-O3` Libraries:** High-throughput computational components (PolarSSL cryptography, image scaling, FreeType glyph rasterization, and FFmpeg codecs) are assigned `-O3` with targeted SIMD loop unrolling.
* **`-mminimal-toc`:** Enforces a compact Table of Contents, preventing linker symbol overflows across the monolithic ELF.
* **`-fno-strict-aliasing`:** Preserves pointer safety across C struct reinterpreters in the GLW widget rendering tree.

---

## Supported Media Formats & Codecs

Movian Next provides exhaustive out-of-the-box support for multimedia formats across local USB drives, internal hard drives, and network shares:

### Video Acceleration & Playback

| Format / Codec | Profile / Specification | Execution Pipeline | Status |
| :--- | :--- | :--- | :--- |
| **H.264 / AVC** | Baseline, Main, High Profile up to Level 4.2 | Hardware Offload (`cellVdec` / SPU) | **Full 1080p @ 60 FPS** |
| **MPEG-2 Video** | Main Profile @ High Level (MP@HL) | Hardware Offload (`cellVdec` / SPU) | **Full 1080p @ 60 FPS** |
| **MPEG-4 Part 2** | DivX (3/4/5/6), XviD, Simple & Advanced Simple | Software PPU + RSX Planar YUV | **Full 720p / 1080p** |
| **VP8** | WebM Video Specification | Software PPU + RSX Planar YUV | **Supported (Confirmation Modal)** |
| **VP9** | Google VP9 Profile 0/2 | Software PPU + RSX Planar YUV | **Supported (Confirmation Modal)** |
| **H.265 / HEVC** | Main / Main 10 Profile | Software PPU + RSX Planar YUV | **Supported (Confirmation Modal)** |
| **AV1** | AOMedia Video 1 | Software PPU + RSX Planar YUV | **Supported (Confirmation Modal)** |
| **MJPEG** | Motion JPEG (Camera & Web streams) | Software PPU + RSX Planar YUV | **Supported** |
| **Sorenson Spark** | FLV1 (Flash Video) | Software PPU + RSX Planar YUV | **Supported** |
| **MPEG-1 Video** | VCD / MPEG-1 Systems | Software PPU + RSX Planar YUV | **Supported** |
| **DV Video** | Standard Digital Video | Software PPU + RSX Planar YUV | **Supported** |
| **WMV / VC-1** | WMV1, WMV2, WMV3 / VC-1 Simple/Main | Software PPU + RSX Planar YUV | **Supported** |

### Audio Codecs (Cell PPE AltiVec Accelerated)

| Category | Supported Audio Codecs & Formats |
| :--- | :--- |
| **Cinema & Surround** | Dolby Digital (AC-3), Dolby Digital Plus (E-AC-3), DTS / DCA, DTS-HD Master Audio, Dolby TrueHD, MLP |
| **Lossless Audiophile** | FLAC, Apple Lossless (ALAC), WavPack (.wv), Monkey's Audio (APE), True Audio (TTA), TAK, Shorten |
| **Web & Streaming** | AAC (AAC-LC, HE-AAC v1/v2, AAC-LD), MP3, MP2, MP1, Ogg Vorbis, Opus |
| **Uncompressed & Games** | Linear PCM (16/24/32-bit Float/Integer, Little/Big Endian, DVD LPCM, Blu-ray LPCM), ADPCM |
| **Legacy & Proprietary** | WMA (v1, v2, Pro, Lossless, Voice), Sony ATRAC (ATRAC1, ATRAC3, ATRAC3+), RealAudio (Cook) |

### Image Formats (4K Heap-Protected & Downscaled)

| Format | Extensions | Decoder Engine | Color / Alpha Features |
| :--- | :--- | :--- | :--- |
| **PNG** | `.png` | FFmpeg 9.0 + Rescale Shield | 24-bit RGB, 32-bit RGBA, Interlaced |
| **JPEG** | `.jpg`, `.jpeg` | FFmpeg 9.0 + Rescale Shield | Baseline, Progressive, YUV420/422/444 |
| **GIF** | `.gif` | FFmpeg 9.0 + Direct PAL8 Unpack | GIF87a, GIF89a, Animated, Full Alpha |
| **WebP** | `.webp` | FFmpeg 9.0 + Rescale Shield | Lossy, Lossless, WebP Extended VP8X |
| **DDS** | `.dds` | FFmpeg 9.0 + Native Remap | DirectDraw Surface, BGRA, DXT textures |
| **TIFF** | `.tif`, `.tiff` | FFmpeg 9.0 + Rescale Shield | Big/Little Endian TIFF, Deflate, LZW |
| **TGA** | `.tga` | FFmpeg 9.0 + Rescale Shield | Truevision TGA (16/24/32-bit, RLE) |
| **BMP** | `.bmp` | FFmpeg 9.0 + Rescale Shield | Standard Windows & OS/2 DIB formats |
| **SVG** | `.svg` | In-Tree Vector Rasterizer | Dynamic XML vector UI and icon assets |

### Supported Containers & Subtitles

* **Containers:** Matroska (`.mkv`, `.mka`), MP4 (`.mp4`, `.m4v`, `.m4a`), AVI, MPEG-TS (`.ts`, `.m2ts`), Ogg (`.ogg`, `.ogv`), FLV, WebM, WAV, AIFF, VOB, ISO (Optical Disc Image), IVF (`.ivf`).
* **Subtitles:** SubRip (`.srt`), Advanced SubStation Alpha (`.ass`, `.ssa` via libass), VobSub (`.sub`/`.idx`), MicroDVD, SAMI, embedded Matroska text/bitmap subtitles.

---

## Build Prerequisites

To compile Movian Next, you need a standard 64-bit Linux distribution (Gentoo, Ubuntu, Debian, Fedora, Arch Linux, openSUSE) or macOS with the following host utilities installed:

```bash
# Ubuntu / Debian
sudo apt-get install --no-install-recommends build-essential curl tar python3 gio-bin

# Arch Linux
sudo pacman -S base-devel curl tar python glib2

# Fedora
sudo dnf install @development-tools curl tar python3 glib2

# openSUSE
sudo zypper install --type pattern devel_basis && sudo zypper install curl tar python3 glib2
```

---

## Quick Start & Compilation

Movian Next features a single-command SDK bootstrap and compilation workflow:

```bash
# 1. Clone the repository
git clone https://github.com/Cruslan/PS3-Movian-Next.git
cd PS3-Movian-Next

# 2. Bootstrap the hermetic PSL1GHT v2 SDK (installs into ./ps3dev if no system SDK is found)
make prepare

# 3. Build the complete application and PS3 packages (using parallel threads)
make -j$(nproc)
```

### Generated Build Deliverables

Upon successful completion, all build artifacts are generated inside `build/`:

| Output File | Description | Target Deployment |
| :--- | :--- | :--- |
| `build/pkg/USRDIR/EBOOT.BIN` | Standalone stripped, relocated & NPDRM signed executable | RPCS3 `dev_hdd0` / USB testing |
| `build/movian-next.pkg` | Standard Retail / HEN installable package | PS3 Package Manager (Retail / HEN) |
| `build/movian-next_geohot.pkg` | Geohot-signed Custom Firmware package | PS3 Custom Firmware (Evilnat, Rebug, Cobra) |
| `build/PARAM.SFO` | Generated application parameter file (`HTSS00004`) | Package metadata descriptor |

---

## Makefile Target Reference

The monolithic build system provides intuitive multi-tier maintenance and clean targets:

| Target | Description |
| :--- | :--- |
| `make pkg` | **Default target (`all`).** Compiles C/ASM sources, FFmpeg static core, bundles assets, links executable, signs `EBOOT.BIN`, and builds `.pkg` packages. |
| `make eboot` | Compiles application code and generates the standalone signed `EBOOT.BIN`. |
| `make self` | Compiles and generates standard unencrypted `movian.self`. |
| `make elf` | Preserves and builds relocated `movian.bundle` ELF binary. |
| `make shaders` | Compiles GLSL shaders into RSX vertex/fragment programs (`.vp` / `.fp`) using `cgc` and `cgcomp`. |
| `make clean` | Safely moves Movian application objects (`src/`, `bundles/`, `pkg/`, `movian*`, `*.o`, `*.d`) to trash while preserving expensive third-party libraries (`ext/`, `ffmpeg`). |
| `make clean-ext` | Cleans compiled third-party dependencies inside `build/ext/` and `build/ffmpeg/`. |
| `make clean-all` | Combines `clean` and `clean-ext` for a complete application rebuild. |
| `make distclean` | Safely purges the entire `build/` directory tree for a 100% clean slate. |
| `make prepare` | Downloads and unpacks the official nightly PSL1GHT SDK into `./ps3dev`. |

---

## Testing & Deployment

### 1. RPCS3 Emulator
Deploy the generated executable directly to the emulator's virtual hard drive:

```bash
mkdir -p ~/.config/rpcs3/dev_hdd0/game/HTSS00004/USRDIR
cp build/pkg/USRDIR/EBOOT.BIN ~/.config/rpcs3/dev_hdd0/game/HTSS00004/USRDIR/
```

Alternatively, drag and drop `build/movian-next.pkg` directly into the RPCS3 main window.

### 2. Physical PlayStation 3 Hardware (CFW & PS3HEN)
1. Copy `movian-next.pkg` (or `movian-next_geohot.pkg` for CFW systems) to the root directory of a FAT32 or exFAT USB flash drive.
2. Insert the USB drive into your PS3 (the port closest to the Blu-ray drive is recommended).
3. On the PS3 XMB menu, navigate to **Game > Package Manager > Install Package Files > Standard**.
4. Select `Movian Next` to install. Once installation finishes, the player will appear directly under the **Game** column.

---

## Architecture & Codebase Layout

```
movian/
├── Makefile                     # Monolithic single-stage build orchestrator
├── AGENTS.md                    # Modernization tracking and architecture blueprints
├── README.md                    # Project documentation
├── res/                         # Raw resources and shaders
│   └── shaders/                 # Vertex and fragment programs (RSX / GLSL)
├── glwskins/                    # Graphical widget skin definitions
│   └── flat/                    # Modern flat UI layout and views
├── ext/                         # Third-party dependencies
│   ├── ffmpeg/                  # Modern FFmpeg 9.0 ('Lei') core multimedia source tree
│   ├── ffmpeg.mk               # PSL1GHT v2 static compilation orchestrator for FFmpeg
│   ├── sqlite/                  # Embedded metadata SQL database engine
│   ├── duktape/                 # Lightweight ECMAScript runtime for plugins
│   ├── dvd/                     # DVD navigation and playback support
│   ├── libass/                  # Advanced SubStation Alpha subtitle renderer
│   ├── libntfs_ext/             # Direct USB BOT / Gekko SCSI NTFS filesystem driver
│   ├── gumbo-parser/            # HTML5 parsing library
│   ├── rtmpdump/                # RTMP stream capture library
│   ├── polarssl-1.3/            # TLS/SSL and cryptographic primitives
│   └── tlsf/                    # Two-Level Segregated Fit memory allocator
├── src/                         # Core Movian application source
│   ├── arch/ps3/                # PlayStation 3 hardware integration
│   │   ├── ps3_audio.c          # PSL1GHT libaudio driver & AltiVec SIMD mixer
│   │   └── ps3_vdec.c           # SPU cellVdec H.264/MPEG-2 hardware decoder
│   ├── ui/glw/                  # OpenGL Widget (GLW) UI framework
│   │   ├── glw_ps3.c            # RSX display management & per-frame command reset
│   │   ├── glw_rsx.c            # RSX drawing pipeline, shaders & vertex buffers
│   │   ├── glw_video_rsx.c      # RSX planar YUV video rendering engine
│   │   └── glw_texture_rsx.c    # RSX GDDR3 VRAM texture upload and swizzlers
│   ├── fileaccess/              # Media probing, container demuxing & I/O
│   │   ├── fa_probe.c           # Format detection and signature probes
│   │   ├── fa_imageloader.c     # Multi-format image loader dispatches
│   │   └── fa_bundle.c          # In-memory bundle:// virtual filesystem
│   ├── image/                   # Image decoding and color space management
│   │   └── image_decoder_libav.c# FFmpeg 9.0 image decoder with 4K rescale shields
│   └── backend/bittorrent/      # BitTorrent networking backend
└── support/                     # Build tools
    ├── mkbundle.c               # In-memory asset packager
    └── sfo.xml                  # PS3 PARAM.SFO metadata definition
```

---

## Credits & Acknowledgements

* **Original Creator & Architect:** [Andreas Öman](https://github.com/andoma) ([Lonelycoder AB](https://movian.tv)) - Creator of Showtime / Movian and primary architect of the media player, GLW GUI engine, and core playback pipeline.
* **PlayStation 3 Modernization & PSL1GHT v2 Port:** [Cruslan](https://github.com/Cruslan) - Port to modern PSL1GHT v2, monolithic single-stage build system, FFmpeg 9.0 migration, RSX command buffer stabilization, multi-format media engine expansion, memory safety hardening, and Cell PPE/SPE pipeline tuning.
* **PSL1GHT & PS3 Toolchain Community:** The developers and contributors of the open-source [PSL1GHT SDK](https://github.com/ps3dev/ps3dev) and [ps3toolchain](https://github.com/ps3dev/ps3toolchain) projects.
* **FFmpeg Contributors:** For the industry-standard multimedia demuxing, decoding, and filtering libraries.

---

## License

Movian Next is distributed under the terms of the **GNU General Public License Version 3 (GPLv3)**. For full licensing terms and conditions, please consult the [LICENSE](LICENSE) file.
