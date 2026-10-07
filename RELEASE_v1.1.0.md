# Movian Next v1.1.0 (PlayStation 3 Modern Edition)

**Release Version:** v1.1.0  
**Title ID:** `HTSS00004` (Content-ID: `UP0001-HTSS00004_00-0000000000000000`)  
**Target Platform:** Sony PlayStation 3 (Custom Firmware & PS3HEN) / RPCS3  
**Target Architecture:** Cell Broadband Engine (PPE + 6 User SPUs) & RSX GPU (NV47/G70)  
**SDK & Toolchain:** PSL1GHT v2 & GCC Toolchain  

---

## Overview

**Movian Next v1.1.0** represents a major architectural milestone in the modernization of the celebrated Movian (formerly Showtime) media player for PlayStation 3. This release completely retires the legacy Libav 11 multimedia core, transitioning entirely to modern **FFmpeg 9.0 ("Lei")** compiled natively for the Cell Broadband Engine architecture. Coupled with whole-program Link-Time Optimization (`-flto`), next-generation codec handling with interactive confirmation dialogs, IVF container demuxing, and modern audio resampling (`libswresample`), v1.1.0 delivers the most resilient and performant homebrew media experience available on PS3 hardware.

---

## Package Deliverables & Verified Checksums

All packages have been built, signed, and validated with zero compiler warnings/errors:

| File Name | Format / Target | Size | SHA-256 Checksum |
| :--- | :--- | :--- | :--- |
| **`movian-next.pkg`** | Retail / PS3HEN Standard Package | 7.7 MB | `441bfa91cd0e042d42082cbc25dcb280171cc499d88ea47ae7ff6b9d8d043101` |
| **`movian-next_geohot.pkg`** | CFW Finalized (Evilnat / Rebug / Cobra) | 7.7 MB | `9ffff27afef72785c70bf263df5f6a77447f67a279c6437566d6d13e6abac690` |
| **`EBOOT.BIN`** | Standalone Executable (RPCS3 / USB) | 7.7 MB | `0cf337d6dd2e446d2cbc6a55053f3e6bf51c3255478233fd378a42b55d59268d` |

---

## Key Highlights & Changelog in v1.1.0

### 1. Modern FFmpeg 9.0 ("Lei") Multimedia Core
* **Full Libav 11 Retirement:** Migrated the entire core decoding, demuxing, parsing, and resampling infrastructure to modern FFmpeg 9.0 static libraries (`libavcodec.a`, `libavformat.a`, `libavutil.a`, `libswscale.a`, `libswresample.a`).
* **Decoupled Asynchronous Decoding:** Converted legacy blocking video/audio decode loops to standard non-blocking `avcodec_send_packet()` and `avcodec_receive_frame()` state machines handling `AVERROR(EAGAIN)` and EOF flushes cleanly.
* **Modern Immutable Stream Parameters:** Migrated all container probing and demuxer handoffs from deprecated `st->codec` to immutable `st->codecpar` structures.
* **Probing Delay Bypass for Cell PPU:** Configured stream analysis (`ext/ffmpeg/libavformat/demux.c`) to bypass CPU-intensive software frame delay guessing on the in-order PowerPC core, eliminating stalls when opening H.264/MPEG streams.

### 2. Audio Engine Modernization (`libswresample`)
* **Modern Resampling Subsystem:** Replaced retired `libavresample` with modern `libswresample` across all audio sinks and playback pipes.
* **Modern Channel Layouts:** Integrated `AVChannelLayout` descriptors and real-time delay tracking via `swr_get_delay()` and `swr_get_out_samples()`.
* **AltiVec SIMD Vector Mixing:** Maintained 48 kHz 32-bit floating-point PCM audio output with hardware AltiVec vector swizzling (`vec_perm`) and volume scaling (`vec_madd`).

### 3. Next-Gen Codec Pre-Flight Confirmation Dialog
* **Interactive Playback Modal:** Attempting to play computationally heavy modern streams (H.265/HEVC, AV1, VP8, VP9) launches an on-screen dialog informing the user that playback relies on software decoding which may cause high CPU load or dropped frames on the PS3's in-order PPE.
* **User Control:** Users can confirm to attempt playback or cancel cleanly back to menu navigation without thread freezes.
* **VP9 & IVF Container Support:** Added native IVF demuxing (`.ivf`) and VP9 decoding in `ext/ffmpeg.mk` and `metadata.c`.

### 4. Compiler & System Tuning
* **Whole-Program Link-Time Optimization (`-flto`):** Enabled `-flto` across the monolithic build pipeline (`-mcpu=cell -O2 -flto`), pruning dead code and optimizing cross-unit function inlining.
* **Selective `-O3` Libraries:** Assigned `-O3` to compute-heavy libraries (PolarSSL cryptography, image scaler, FreeType rasterizer, and FFmpeg core) for maximum throughput.
* **Single-Stage Monolithic Build:** Monolithic single-stage build system (`make all`) bundling in-memory assets (`bundle.o`), eliminating external ZIP archives, configure scripts, and trampoline loaders.
* **Build Directory Consolidation:** Canonical `build/` directory across all compilation stages.

### 5. Proven Hardware Acceleration & Stability (Preserved)
* **Hardware SPU Video Acceleration (`cellVdec`):** Full hardware decoding of H.264 (up to High Profile Level 4.2 @ 1080p60) and MPEG-2 offloaded entirely to Synergistic Processing Elements (SPEs).
* **Rock-Solid RSX 60 FPS Engine:** Per-frame command buffer reset model (`resetCommandBuffer`) with hardware NV40 JUMP instructions, completely preventing font distortion, UI sliding, and RSX FIFO desync (`0xbf800000`).
* **Memory Safety & 4K Heap Guard:** Dynamic clamping for high-resolution images, protecting the 96 MB application heap against OOM crashes.
* **Default Network Hardening:** All remote control ports, UPnP multicast beacons, and background scrapers default to disabled for privacy and stability.

---

## Installation & Deployment Guide

### Option A: Installation on PlayStation 3 (CFW / PS3HEN)
1. Copy **`movian-next.pkg`** (or **`movian-next_geohot.pkg`** for Custom Firmware) to the root directory of a FAT32 or exFAT formatted USB flash drive.
2. Insert the USB drive into your PS3 (port closest to the Blu-ray drive is recommended).
3. On the PS3 XMB menu, navigate to:
   * **CFW:** `Game` > `Package Manager` > `Install Package Files` > `Standard`
   * **PS3HEN:** Enable HEN > `Game` > `Package Manager` > `Install Package Files` > `Standard`
4. Select `Movian Next` to install. Once completed, launch the application from the **Game** column.

### Option B: RPCS3 Emulator
* **Drag-and-Drop:** Drag `movian-next.pkg` directly into the RPCS3 main window to install.
* **Direct EBOOT.BIN:** Copy `build/pkg/USRDIR/EBOOT.BIN` to:
  ```
  ~/.config/rpcs3/dev_hdd0/game/HTSS00004/USRDIR/EBOOT.BIN
  ```

---

## Credits & Acknowledgements

* **Original Creator & Architect:** [Andreas Öman](https://github.com/andoma) ([Lonelycoder AB](https://movian.tv)) - Creator of Showtime / Movian and lead designer of the GLW UI and multimedia engine.
* **PlayStation 3 Modernization & PSL1GHT v2 Port:** [Cruslan](https://github.com/Cruslan) - PSL1GHT v2 migration, FFmpeg 9.0 integration, RSX 60 FPS stabilization, SPU video offloading, and build modernizations.
* **Open-Source Toolchains:** The [PSL1GHT](https://github.com/ps3dev/ps3dev) and [ps3toolchain](https://github.com/ps3dev/ps3toolchain) maintainers, and the [FFmpeg](https://ffmpeg.org/) project.
