#
# FFmpeg 9.0 PlayStation 3 PSL1GHT v2 Build Makefile
#

C ?= $(CURDIR)
BUILDDIR ?= $(C)/build

ifeq ($(wildcard $(C)/ps3dev),)
PS3DEV ?= /usr/local/ps3dev
else
PS3DEV ?= $(C)/ps3dev
endif

PPU_PREFIX := $(PS3DEV)/ppu/bin/ppu-
CC := $(PS3DEV)/ppu/bin/ppu-gcc
FFMPEG_BUILD_DIR = $(BUILDDIR)/ffmpeg/build

# Software Audio Decoders supported by Movian on Cell PPE
AUDIO_DECODERS = aac,aac_latm,mp3,mp3float,mp2,mp1,ac3,eac3,dca,truehd,mlp,flac,alac,wavpack,ape,tak,tta,shorten,vorbis,opus,wmav1,wmav2,wmapro,wmalossless,wmavoice,atrac1,atrac3,atrac3p,cook,ra_144,ra_288,pcm_s16le,pcm_s16be,pcm_u16le,pcm_u16be,pcm_s24le,pcm_s24be,pcm_u24le,pcm_u24be,pcm_s32le,pcm_s32be,pcm_u32le,pcm_u32be,pcm_f32le,pcm_f32be,pcm_s8,pcm_u8,pcm_alaw,pcm_mulaw,pcm_bluray,pcm_dvd,adpcm_ms,adpcm_ima_wav,adpcm_ima_qt,adpcm_adx

# Software Video & Image Decoders (including modern HEVC, AV1, VP8, and VP9)
VIDEO_DECODERS = h264,mpeg2video,mpeg1video,mpeg4,vp8,vp9,mjpeg,png,flv,dvvideo,wmv1,wmv2,wmv3,vc1,gif,webp,tiff,bmp,dds,hevc,av1

# Container Demuxers
DEMUXERS = matroska,mov,avi,asf,mpegps,mpegts,mpegvideo,flv,ogg,wav,aac,mp3,ac3,eac3,dts,flac,ape,wv,tta,tak,shorten,rm,image2,image2pipe,gif,rawvideo,h264,hevc,m4v,vc1,srt,ass,webvtt,subviewer,av1,mlp,truehd,ivf,dv

# Stream Parsers
PARSERS = aac,aac_latm,ac3,dca,flac,h264,hevc,mjpeg,mpegaudio,mpegvideo,mpeg4video,opus,vorbis,vp8,vp9,av1

# SPDIF passthrough and capture muxers
MUXERS = spdif,matroska

# Thumbnail and Passthrough encoders
ENCODERS = ac3,eac3,pcm_s16le,ffvhuff,mjpeg,png

build: $(FFMPEG_BUILD_DIR)/config.h
	@echo "Building FFmpeg 9.0 static libraries for PlayStation 3..."
	${MAKE} -C $(FFMPEG_BUILD_DIR) -j12
	@echo "Installing FFmpeg 9.0 headers and static libraries into $(BUILDDIR)/inst..."
	${MAKE} -C $(FFMPEG_BUILD_DIR) install

$(FFMPEG_BUILD_DIR)/config.h:
	@mkdir -p $(FFMPEG_BUILD_DIR) $(BUILDDIR)/ffmpeg $(BUILDDIR)/inst
	@echo "Compiling TLSF custom allocator malloc stub for FFmpeg configure..."
	$(CC) -c -o $(BUILDDIR)/ffmpeg/mallocstub.o $(C)/support/mallocstub.c
	@echo "Configuring FFmpeg 9.0 for Cell Broadband Engine PPE..."
	cd $(FFMPEG_BUILD_DIR) && PSL1GHT=$(PS3DEV) $(C)/ext/ffmpeg/configure \
		--cross-prefix=$(PPU_PREFIX) \
		--enable-cross-compile \
		--arch=powerpc64 \
		--cpu=cell \
		--target-os=none \
		--malloc-prefix=my \
		--disable-shared \
		--enable-static \
		--disable-programs \
		--disable-doc \
		--disable-network \
		--disable-avdevice \
		--disable-avfilter \
		--disable-devices \
		--disable-filters \
		--disable-everything \
		--disable-vsx \
		--disable-power8 \
		--enable-avcodec \
		--enable-avformat \
		--enable-avutil \
		--enable-swscale \
		--enable-swresample \
		--enable-decoder=$(AUDIO_DECODERS),$(VIDEO_DECODERS) \
		--enable-demuxer=$(DEMUXERS) \
		--enable-parser=$(PARSERS) \
		--enable-protocol=file \
		--enable-muxer=$(MUXERS) \
		--enable-encoder=$(ENCODERS) \
		--prefix=$(BUILDDIR)/inst \
		--extra-cflags="-mminimal-toc -I$(PS3DEV)/ppu/include -I$(PS3DEV)/portlibs/ppu/include -include $(C)/support/nostrictansi.h -mcpu=cell -std=c11 -Dstatic_assert=_Static_assert" \
		--extra-ldflags="$(BUILDDIR)/ffmpeg/mallocstub.o -L$(PS3DEV)/ppu/lib -L$(PS3DEV)/portlibs/ppu/lib" \
		--cc=$(CC)

.PHONY: build
