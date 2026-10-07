/*
 *  Copyright (C) 2007-2015 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */
#include <assert.h>
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>


#include "main.h"
#include "fileaccess.h"
#include "fa_imageloader.h"
#if ENABLE_LIBAV
#include "fa_libav.h"
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#endif
#include "misc/callout.h"
#include "misc/minmax.h"
#include "image/pixmap.h"
#include "image/jpeg.h"
#include "backend/backend.h"
#include "blobcache.h"

static const uint8_t pngsig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
static const uint8_t gif89sig[6] = {'G', 'I', 'F', '8', '9', 'a'};
static const uint8_t gif87sig[6] = {'G', 'I', 'F', '8', '7', 'a'};

static const uint8_t svgsig1[5] = {'<', '?', 'x', 'm', 'l'};
static const uint8_t svgsig2[4] = {'<', 's', 'v', 'g'};

static const uint8_t webpsig[4] = {'W', 'E', 'B', 'P'};
static const uint8_t ddssig[4] = {'D', 'D', 'S', ' '};
static const uint8_t tiffsig_le[4] = {'I', 'I', 42, 0};
static const uint8_t tiffsig_be[4] = {'M', 'M', 0, 42};

#if ENABLE_LIBAV
static hts_mutex_t image_from_video_mutex[2];
static AVCodecContext *thumbctx;
static const AVCodec *thumbcodec;
static callout_t thumb_flush_callout;

static image_t *fa_image_from_video(const char *url, const image_meta_t *im,
                                    char *errbuf, size_t errlen,
                                    int *cache_control, cancellable_t *c);
#endif

/**
 *
 */
void
fa_imageloader_init(void)
{
#if ENABLE_LIBAV
  hts_mutex_init(&image_from_video_mutex[0]);
  hts_mutex_init(&image_from_video_mutex[1]);
  thumbcodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
#endif
}


static image_t *
fa_imageloader_buf(buf_t *buf, char *errbuf, size_t errlen)
{
  jpeg_meminfo_t mi;
  image_coded_type_t fmt;
  int width = -1, height = -1, orientation = 0, progressive = 0, planes = 0;

  const uint8_t *p = buf_c8(buf);
  mi.data = p;
  mi.size = buf->b_size;

  if(buf->b_size < 16)
    goto bad;

  /* Probe format */

  if(p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff) {

    jpeginfo_t ji;

    if(jpeg_info(&ji, jpeginfo_mem_reader, &mi,
		 JPEG_INFO_DIMENSIONS | JPEG_INFO_ORIENTATION,
		 p, buf->b_size, errbuf, errlen)) {
      return NULL;
    }

    fmt = IMAGE_JPEG;

    width       = ji.ji_width;
    height      = ji.ji_height;
    orientation = ji.ji_orientation;
    progressive = ji.ji_progressive;
    planes      = ji.ji_components;
    jpeg_info_clear(&ji);

  } else if(!memcmp(pngsig, p, 8)) {
    fmt = IMAGE_PNG;
  } else if(!memcmp(gif87sig, p, sizeof(gif87sig)) ||
	    !memcmp(gif89sig, p, sizeof(gif89sig))) {
    fmt = IMAGE_GIF;
  } else if(p[0] == 'B' && p[1] == 'M') {
    fmt = IMAGE_BMP;
  } else if(!memcmp(svgsig1, p, sizeof(svgsig1)) ||
	    !memcmp(svgsig2, p, sizeof(svgsig2))) {
    fmt = IMAGE_SVG;
  } else if(buf->b_size >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, webpsig, sizeof(webpsig))) {
    /* Google WebP container (RIFF....WEBP) */
    fmt = IMAGE_WEBP;
  } else if(buf->b_size >= 4 && !memcmp(p, ddssig, sizeof(ddssig))) {
    /* Microsoft DirectDraw Surface container ('DDS ') */
    fmt = IMAGE_DDS;
  } else if(buf->b_size >= 4 && (!memcmp(p, tiffsig_le, sizeof(tiffsig_le)) ||
                                 !memcmp(p, tiffsig_be, sizeof(tiffsig_be)))) {
    /* Tagged Image File Format (TIFF - II42 Little-Endian or MM042 Big-Endian) */
    fmt = IMAGE_TIFF;
  } else if(buf->b_size >= 18 && (p[2] == 1 || p[2] == 2 || p[2] == 3 ||
                                  p[2] == 9 || p[2] == 10 || p[2] == 11) &&
            (p[1] == 0 || p[1] == 1)) {
    /* Truevision Targa (TGA - Uncompressed & RLE True-color/Grayscale/Color-mapped) */
    uint16_t tga_w = p[12] | (p[13] << 8);
    uint16_t tga_h = p[14] | (p[15] << 8);
    uint8_t tga_bpp = p[16];
    if(tga_w > 0 && tga_h > 0 && (tga_bpp == 8 || tga_bpp == 15 || tga_bpp == 16 || tga_bpp == 24 || tga_bpp == 32)) {
      fmt = IMAGE_TGA;
      width = tga_w;
      height = tga_h;
    } else {
      goto bad;
    }
  } else {
  bad:
    snprintf(errbuf, errlen, "Unknown format");
    return NULL;
  }

  image_t *img = image_coded_create_from_buf(buf, fmt);
  if(img != NULL) {
    img->im_width = width;
    img->im_height = height;
    img->im_orientation = orientation;
    img->im_color_planes = planes;
    if(progressive)
      img->im_flags |= IMAGE_PROGRESSIVE;
  } else {
    snprintf(errbuf, errlen, "Out of memory");
  }
  return img;
}

/**
 * Load entire image into memory using fileaccess load method.
 * Faster than open+read+close.
 */
static image_t *
fa_imageloader2(const char *url, char *errbuf, size_t errlen,
                int *cache_control, cancellable_t *c)
{
  buf_t *buf;

  buf = fa_load(url,
                FA_LOAD_ERRBUF(errbuf, errlen),
                FA_LOAD_CACHE_CONTROL(cache_control),
                FA_LOAD_CANCELLABLE(c),
                FA_LOAD_FLAGS(FA_NON_INTERACTIVE | FA_CONTENT_ON_ERROR),
                FA_LOAD_NO_FALLBACK(),
                NULL);
  if(buf == NULL || buf == NOT_MODIFIED || buf == NO_LOAD_METHOD)
    return (image_t *)buf;

  image_t *img = fa_imageloader_buf(buf, errbuf, errlen);
  buf_release(buf);
  return img;
}


/**
 *
 */
static int
jpeginfo_reader(void *handle, void *buf, int64_t offset, size_t size)
{
  if(fa_seek(handle, offset, SEEK_SET) != offset)
    return -1;
  return fa_read(handle, buf, size);
}


/**
 *
 */
image_t *
fa_imageloader(const char *url, const struct image_meta *im,
	       char *errbuf, size_t errlen,
	       int *cache_control, cancellable_t *c,
               backend_t *be)
{
  uint8_t p[32];
  int r;
  int width = -1, height = -1, orientation = 0;
  fa_handle_t *fh;
  image_t *img;
  image_coded_type_t fmt;

#if ENABLE_LIBAV
  if(strchr(url, '#'))
    return fa_image_from_video(url, im, errbuf, errlen, cache_control, c);
#endif

  if(!im->im_want_thumb) {
    image_t *img = fa_imageloader2(url, errbuf, errlen, cache_control, c);
    if(img != NO_LOAD_METHOD)
      return img;
  }

  fa_open_extra_t foe = {
    .foe_cancellable = c
  };

  if(ONLY_CACHED(cache_control)) {
    snprintf(errbuf, errlen, "Not cached");
    return NULL;
  }

  if((fh = fa_open_resolver(url, errbuf, errlen,
                            FA_BUFFERED_SMALL | FA_NON_INTERACTIVE,
                            &foe)) == NULL)
    return NULL;

  r = fa_read(fh, p, sizeof(p));
  if(r < 16) {
    snprintf(errbuf, errlen, "File too short");
    fa_close(fh);
    return NULL;
  }

  /* Probe format */

  if(p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff) {
      
    jpeginfo_t ji;
    
    if(jpeg_info(&ji, jpeginfo_reader, fh,
		 JPEG_INFO_DIMENSIONS |
		 JPEG_INFO_ORIENTATION |
		 (im->im_want_thumb ? JPEG_INFO_THUMBNAIL : 0),
		 p, sizeof(p), errbuf, errlen)) {
      fa_close(fh);
      return NULL;
    }

    if(im->im_want_thumb && ji.ji_thumbnail) {
      image_t *im = image_retain(ji.ji_thumbnail);
      fa_close(fh);
      jpeg_info_clear(&ji);
      im->im_flags |= IMAGE_ADAPTED;
      return im;
    }

#if ENABLE_LIBJPEG
    if(!im->im_no_decoding) {
      pixmap_t *pm = libjpeg_decode(fh, im, errbuf, errlen);
      if(pm != NULL) {
        image_t *im = image_create_from_pixmap(pm);
        im->im_origin_coded_type = IMAGE_JPEG;
        im->im_origin_coded_type = ji.ji_orientation;
        pixmap_release(pm);
        return im;
      } else {
        return NULL;
      }
    }
#endif

    fmt = IMAGE_JPEG;

    width = ji.ji_width;
    height = ji.ji_height;
    orientation = ji.ji_orientation;

    jpeg_info_clear(&ji);

  } else if(!memcmp(pngsig, p, 8)) {
    fmt = IMAGE_PNG;
  } else if(!memcmp(gif87sig, p, sizeof(gif87sig)) ||
	    !memcmp(gif89sig, p, sizeof(gif89sig))) {
    fmt = IMAGE_GIF;
  } else if(p[0] == 'B' && p[1] == 'M') {
    fmt = IMAGE_BMP;
  } else if(!memcmp(svgsig1, p, sizeof(svgsig1)) ||
	    !memcmp(svgsig2, p, sizeof(svgsig2))) {
    fmt = IMAGE_SVG;
  } else if(!memcmp(p, "RIFF", 4) && !memcmp(p + 8, webpsig, sizeof(webpsig))) {
    /* Google WebP container (RIFF....WEBP) */
    fmt = IMAGE_WEBP;
  } else if(!memcmp(p, ddssig, sizeof(ddssig))) {
    /* Microsoft DirectDraw Surface container ('DDS ') */
    fmt = IMAGE_DDS;
  } else if(!memcmp(p, tiffsig_le, sizeof(tiffsig_le)) ||
            !memcmp(p, tiffsig_be, sizeof(tiffsig_be))) {
    /* Tagged Image File Format (TIFF - II42 Little-Endian or MM042 Big-Endian) */
    fmt = IMAGE_TIFF;
  } else if(r >= 18 && (p[2] == 1 || p[2] == 2 || p[2] == 3 ||
                        p[2] == 9 || p[2] == 10 || p[2] == 11) &&
            (p[1] == 0 || p[1] == 1)) {
    /* Truevision Targa (TGA - Uncompressed & RLE True-color/Grayscale/Color-mapped) */
    uint16_t tga_w = p[12] | (p[13] << 8);
    uint16_t tga_h = p[14] | (p[15] << 8);
    uint8_t tga_bpp = p[16];
    if(tga_w > 0 && tga_h > 0 && (tga_bpp == 8 || tga_bpp == 15 || tga_bpp == 16 || tga_bpp == 24 || tga_bpp == 32)) {
      fmt = IMAGE_TGA;
      width = tga_w;
      height = tga_h;
    } else {
      snprintf(errbuf, errlen, "Unknown format");
      fa_close(fh);
      return NULL;
    }
  } else {
    snprintf(errbuf, errlen, "Unknown format");
    fa_close(fh);
    return NULL;
  }

  int64_t s = fa_fsize(fh);
  if(s < 0) {
    snprintf(errbuf, errlen, "Can't read from non-seekable file");
    fa_close(fh);
    return NULL;
  }

  void *ptr;
  img = image_coded_alloc(&ptr, s, fmt);

  if(img == NULL) {
    snprintf(errbuf, errlen, "Out of memory");
    fa_close(fh);
    return NULL;
  }

  img->im_width = width;
  img->im_height = height;
  img->im_orientation = orientation;
  fa_seek(fh, SEEK_SET, 0);
  r = fa_read(fh, ptr, s);
  fa_close(fh);

  if(r != s) {
    image_release(img);
    snprintf(errbuf, errlen, "Read error");
    return NULL;
  }
  return img;
}

#if ENABLE_LIBAV

static char *ifv_url;
static AVFormatContext *ifv_fctx;
static AVCodecContext *ifv_ctx;
int ifv_stream;

static void
ifv_close(void)
{
  free(ifv_url);
  ifv_url = NULL;

  if(ifv_ctx != NULL) {
    avcodec_free_context(&ifv_ctx);
  }

  if(ifv_fctx != NULL) {
    fa_libav_close_format(ifv_fctx, 0);
    ifv_fctx = NULL;
  }
}

/**
 *
 */
static void
ifv_autoclose(callout_t *c, void *aux)
{
  if(hts_mutex_trylock(&image_from_video_mutex[1])) {
    callout_arm(&thumb_flush_callout, ifv_autoclose, NULL, 5);
  } else {
    TRACE(TRACE_DEBUG, "Thumb", "Closing movie for thumb sources"); 
    ifv_close();
    hts_mutex_unlock(&image_from_video_mutex[1]);
  }
}

/**
 *
 */
static void
write_thumb(const AVCodecContext *src, const AVFrame *sframe, 
            int width, int height, const char *cacheid, time_t mtime)
{
  if(thumbcodec == NULL || sframe == NULL)
    return;

  int src_w = (sframe && sframe->width > 0) ? sframe->width : (src ? src->width : 0);
  int src_h = (sframe && sframe->height > 0) ? sframe->height : (src ? src->height : 0);
  int src_fmt = (sframe && sframe->format != AV_PIX_FMT_NONE) ? sframe->format : (src ? src->pix_fmt : AV_PIX_FMT_NONE);

  if(src_w <= 0 || src_h <= 0 || src_fmt == AV_PIX_FMT_NONE)
    return;

  AVCodecContext *ctx = thumbctx;

  if(ctx == NULL || ctx->width  != width || ctx->height != height) {
    
    if(ctx != NULL) {
      /* In modern FFmpeg, avcodec_free_context cleanly resets internals and deallocates memory */
      avcodec_free_context(&ctx);
    }

    ctx = avcodec_alloc_context3(thumbcodec);
    if(ctx == NULL)
      return;

    ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    ctx->time_base.den = 1;
    ctx->time_base.num = 1;
    ctx->sample_aspect_ratio.num = 1;
    ctx->sample_aspect_ratio.den = 1;
    ctx->width  = width;
    ctx->height = height;

    ctx->thread_count = 1;
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;

    if(avcodec_open2(ctx, thumbcodec, NULL) < 0) {
      TRACE(TRACE_ERROR, "THUMB", "Unable to open thumb encoder");
      avcodec_free_context(&ctx);
      thumbctx = NULL;
      return;
    }
    thumbctx = ctx;
  }

  AVFrame *oframe = av_frame_alloc();
  if(oframe == NULL)
    return;

  oframe->format = ctx->pix_fmt;
  oframe->width  = width;
  oframe->height = height;
  if(av_frame_get_buffer(oframe, 32) < 0) {
    av_frame_free(&oframe);
    return;
  }
      
  struct SwsContext *sws;
  sws = sws_getContext(src_w, src_h, src_fmt,
                       width, height, ctx->pix_fmt,
                       SWS_FAST_BILINEAR | SWS_ACCURATE_RND,
                       NULL, NULL, NULL);
  if(sws == NULL) {
    sws = sws_getContext(src_w, src_h, src_fmt,
                         width, height, ctx->pix_fmt, SWS_BILINEAR,
                         NULL, NULL, NULL);
  }
  if(sws == NULL) {
    av_frame_free(&oframe);
    return;
  }

  sws_scale(sws, (const uint8_t **)sframe->data, sframe->linesize,
            0, src_h, oframe->data, oframe->linesize);
  sws_freeContext(sws);

  oframe->pts = 1;
  AVPacket *out = av_packet_alloc();
  if(out == NULL) {
    av_frame_free(&oframe);
    return;
  }

  int r = avcodec_send_frame(ctx, oframe);
  if(r >= 0) {
    r = avcodec_receive_packet(ctx, out);
    if(r >= 0 && out->size > 0) {
      buf_t *b = buf_create_and_copy(out->size, out->data);
      if(b != NULL) {
        blobcache_put(cacheid, "videothumb", b, INT32_MAX, NULL, mtime, 0);
        buf_release(b);
      }
    }
  }

  av_packet_free(&out);
  av_frame_free(&oframe);
}


/**
 *
 */
attribute_unused static image_t *
thumb_from_buf(buf_t *buf, char *errbuf, size_t errlen,
               const char *cacheid, time_t mtime)
{
  image_t *img = fa_imageloader_buf(buf, errbuf, errlen);

  if(img != NULL)
    blobcache_put(cacheid, "videothumb", buf, INT32_MAX, NULL, mtime, 0);

  buf_release(buf);
  return img;
}


/**
 *
 */
attribute_unused static image_t *
thumb_from_attachment(const char *url, int64_t offset, int size,
                      char *errbuf, size_t errlen, const char *cacheid,
                      time_t mtime)
{
  fa_handle_t *fh = fa_open_ex(url, errbuf, errlen, FA_NON_INTERACTIVE, NULL);
  if(fh == NULL)
    return NULL;

  fh = fa_slice_open(fh, offset, size);
  buf_t *buf = fa_load_and_close(fh);
  if(buf == NULL) {
    snprintf(errbuf, errlen, "Load error");
    return NULL;
  }
  return thumb_from_buf(buf, errbuf, errlen, cacheid, mtime);
}


/**
 *
 */
static image_t *
fa_image_from_video2(const char *url, const image_meta_t *im,
		     const char *cacheid, char *errbuf, size_t errlen,
		     int sec, time_t mtime, cancellable_t *c)
{
  image_t *img = NULL;

  if(ifv_url == NULL || strcmp(url, ifv_url)) {
    // Need to open
    int i;
    AVFormatContext *fctx;
    fa_handle_t *fh = fa_open_ex(url, errbuf, errlen,
                                 FA_BUFFERED_BIG | FA_NON_INTERACTIVE,
                                 NULL);

    if(fh == NULL)
      return NULL;

    int strategy = FA_LIBAV_OPEN_STRATEGY_THUMBNAIL;

    AVIOContext *avio = fa_libav_reopen(fh, 0);

    if((fctx = fa_libav_open_format(avio, url, NULL, 0, NULL,
                                    strategy)) == NULL) {
      fa_libav_close(avio);
      snprintf(errbuf, errlen, "Unable to open format");
      return NULL;
    }

    if(!strcmp(fctx->iformat->name, "avi"))
      fctx->flags |= AVFMT_FLAG_GENPTS;

    AVCodecContext *ctx = NULL;
    AVCodecParameters *vpar = NULL;
    int vstream = 0;
    for(i = 0; i < fctx->nb_streams; i++) {
      AVStream *st = fctx->streams[i];
      AVCodecParameters *c = st->codecpar;
      AVDictionaryEntry *mt;

      if(c == NULL)
        continue;

      switch(c->codec_type) {
      case AVMEDIA_TYPE_VIDEO:
        if(st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
          if(st->attached_pic.size > 0 && st->attached_pic.data != NULL) {
            buf_t *b = buf_create_and_copy(st->attached_pic.size,
                                           st->attached_pic.data);
            fa_libav_close_format(fctx, 0);
            return thumb_from_buf(b, errbuf, errlen, cacheid, mtime);
          }
        }
        if(vpar == NULL) {
          vstream = i;
          vpar = st->codecpar;
        }
        break;

      case AVMEDIA_TYPE_ATTACHMENT:
        mt = av_dict_get(st->metadata, "mimetype", NULL, AV_DICT_IGNORE_SUFFIX);
        AVDictionaryEntry *fn = av_dict_get(st->metadata, "filename", NULL, AV_DICT_IGNORE_SUFFIX);
        int is_cover_img = (mt != NULL && (!strcmp(mt->value, "image/jpeg") || !strcmp(mt->value, "image/png"))) ||
                           (fn != NULL && (strstr(fn->value, ".jpg") || strstr(fn->value, ".jpeg") || strstr(fn->value, ".png")));
        if(is_cover_img) {
          if(st->codecpar->extradata_size > 0 && st->codecpar->extradata != NULL) {
            buf_t *b = buf_create_and_copy(st->codecpar->extradata_size,
                                           st->codecpar->extradata);
            fa_libav_close_format(fctx, 0);
            return thumb_from_buf(b, errbuf, errlen, cacheid, mtime);
          }
        }
        break;

      default:
        break;
      }
    }
    if(vpar == NULL) {
      fa_libav_close_format(fctx, 0);
      return NULL;
    }

    /* Reject next-gen codecs for thumbnail frame decode to prevent stalling the Cell PPE */
    if(vpar->codec_id == AV_CODEC_ID_HEVC ||
       vpar->codec_id == AV_CODEC_ID_VP9 ||
       vpar->codec_id == AV_CODEC_ID_AV1 ||
       vpar->codec_id == AV_CODEC_ID_VP8) {
      fa_libav_close_format(fctx, 0);
      snprintf(errbuf, errlen, "Next-gen codec thumbnail decode bypassed on PS3");
      return NULL;
    }

    const AVCodec *codec = avcodec_find_decoder(vpar->codec_id);
    if(codec == NULL) {
      fa_libav_close_format(fctx, 0);
      snprintf(errbuf, errlen, "Unable to find codec");
      return NULL;
    }

    ctx = avcodec_alloc_context3(codec);
    if(ctx == NULL || avcodec_parameters_to_context(ctx, vpar) < 0) {
      if(ctx != NULL) avcodec_free_context(&ctx);
      fa_libav_close_format(fctx, 0);
      snprintf(errbuf, errlen, "Unable to allocate decoder context");
      return NULL;
    }

    /* Optimized thumbnail decode configuration for Cell PPE:
     * Discard non-reference frames, skip deblocking in-loop filter, and decode at lower resolution */
    ctx->thread_count = 1;
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->skip_loop_filter = AVDISCARD_ALL;
    ctx->skip_frame = AVDISCARD_NONREF;
    if(codec->max_lowres > 0) {
      ctx->lowres = MIN(codec->max_lowres, 2);
    }

    if(avcodec_open2(ctx, codec, NULL) < 0) {
      avcodec_free_context(&ctx);
      fa_libav_close_format(fctx, 0);
      snprintf(errbuf, errlen, "Unable to open codec");
      return NULL;
    }

    ifv_close();

    ifv_stream = vstream;
    ifv_url = strdup(url);
    ifv_fctx = fctx;
    ifv_ctx = ctx;
  }

  AVPacket pkt;
  AVFrame *frame = av_frame_alloc();
  int got_pic = 0;

  AVStream *st = ifv_fctx->streams[ifv_stream];

  if(sec == -1) {
    /* Fast responsive UI thumbnailing: extract the initial keyframe at position 0.
     * Avoids deep backward seeks that incur heavy disk/USB I/O latency. */
    sec = 0;
  }

  int64_t ts = av_rescale(sec, st->time_base.den, st->time_base.num);
  int delayed_seek = 0;

  if(ifv_ctx->codec_id == AV_CODEC_ID_RV40 ||
     ifv_ctx->codec_id == AV_CODEC_ID_RV30) {
    delayed_seek = 1;
  } else {
    if(sec > 0) {
      if(av_seek_frame(ifv_fctx, ifv_stream, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        av_seek_frame(ifv_fctx, ifv_stream, 0, AVSEEK_FLAG_BACKWARD);
      }
    } else {
      av_seek_frame(ifv_fctx, ifv_stream, 0, AVSEEK_FLAG_BACKWARD);
    }
  }

  avcodec_flush_buffers(ifv_ctx);
  ifv_ctx->skip_frame = AVDISCARD_NONREF;

  int i = 0;
  int video_packets = 0;
  while(1) {
    int r;

    i++;
    if(i >= 50) {
      break;
    }

    r = av_read_frame(ifv_fctx, &pkt);

    if(r == AVERROR(EAGAIN))
      continue;

    if(r == AVERROR_EOF)
      break;

    if(cancellable_is_cancelled(c)) {
      snprintf(errbuf, errlen, "Cancelled");
      av_packet_unref(&pkt);
      break;
    }

    if(r != 0) {
      ifv_close();
      break;
    }

    if(pkt.stream_index != ifv_stream) {
      av_packet_unref(&pkt);
      continue;
    }

    video_packets++;

    got_pic = 0;
    if(avcodec_send_packet(ifv_ctx, &pkt) >= 0) {
      if(avcodec_receive_frame(ifv_ctx, frame) == 0) {
        got_pic = 1;
      }
    }
    av_packet_unref(&pkt);

    if(delayed_seek) {
      delayed_seek = 0;
      if(av_seek_frame(ifv_fctx, ifv_stream, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        av_seek_frame(ifv_fctx, ifv_stream, 0, AVSEEK_FLAG_BACKWARD);
      }
      continue;
    }

    if(got_pic) {
      break;
    }

    /* Fast UI guard: limit scanned video packets to 25 to preserve 60 FPS responsiveness */
    if(video_packets >= 25) {
      break;
    }
  }

  /* Drain decoder in case keyframe was buffered pending reorder */
  if(got_pic == 0 && ifv_ctx != NULL) {
    if(avcodec_send_packet(ifv_ctx, NULL) >= 0) {
      if(avcodec_receive_frame(ifv_ctx, frame) == 0) {
        got_pic = 1;
      }
    }
    avcodec_flush_buffers(ifv_ctx);
  }

  int vid_w = (frame && frame->width > 0) ? frame->width : (ifv_ctx ? ifv_ctx->width : 0);
  int vid_h = (frame && frame->height > 0) ? frame->height : (ifv_ctx ? ifv_ctx->height : 0);
  int vid_fmt = (frame && frame->format != AV_PIX_FMT_NONE) ? frame->format : (ifv_ctx ? ifv_ctx->pix_fmt : AV_PIX_FMT_NONE);

  if(got_pic && vid_w > 0 && vid_h > 0 && vid_fmt != AV_PIX_FMT_NONE) {
    int w, h;

    if(im->im_req_width != -1 && im->im_req_height != -1) {
      w = im->im_req_width;
      h = im->im_req_height;
    } else if(im->im_req_width != -1) {
      w = im->im_req_width;
      h = im->im_req_width * vid_h / vid_w;
    } else if(im->im_req_height != -1) {
      w = im->im_req_height * vid_w / vid_h;
      h = im->im_req_height;
    } else {
      w = im->im_req_width;
      h = im->im_req_height;
    }

    pixmap_t *pm = pixmap_create(w, h, PIXMAP_BGR32, 0);

    if(pm == NULL) {
      ifv_close();
      snprintf(errbuf, errlen, "Out of memory");
      av_frame_free(&frame);
      return NULL;
    }

    struct SwsContext *sws;
    sws = sws_getContext(vid_w, vid_h, vid_fmt,
                         w, h, AV_PIX_FMT_BGR32,
                         SWS_FAST_BILINEAR | SWS_ACCURATE_RND,
                         NULL, NULL, NULL);
    if(sws == NULL) {
      sws = sws_getContext(vid_w, vid_h, vid_fmt,
                           w, h, AV_PIX_FMT_BGR32, SWS_BILINEAR,
                           NULL, NULL, NULL);
    }
    if(sws == NULL) {
      ifv_close();
      snprintf(errbuf, errlen, "Scaling failed");
      pixmap_release(pm);
      av_frame_free(&frame);
      return NULL;
    }

    uint8_t *ptr[4] = {0,0,0,0};
    int strides[4] = {0,0,0,0};

    ptr[0] = pm->pm_data;
    strides[0] = pm->pm_linesize;

    sws_scale(sws, (const uint8_t **)frame->data, frame->linesize,
              0, vid_h, ptr, strides);

    sws_freeContext(sws);

    write_thumb(ifv_ctx, frame, w, h, cacheid, mtime);

    img = image_create_from_pixmap(pm);
    pixmap_release(pm);
  }

  av_frame_free(&frame);
  if(img == NULL)
    snprintf(errbuf, errlen, "Frame not found for %s", url);

  if(ifv_ctx != NULL) {
    avcodec_flush_buffers(ifv_ctx);
    callout_arm(&thumb_flush_callout, ifv_autoclose, NULL, 5);
  }
  return img;
}


/**
 *
 */
static image_t *
fa_image_from_video(const char *url0, const image_meta_t *im,
		    char *errbuf, size_t errlen, int *cache_control,
		    cancellable_t *c)
{
  static char *stated_url;
  static fa_stat_t fs;
  time_t stattime = 0;
  time_t mtime = 0;
  image_t *img = NULL;
  char cacheid[512];
  char *url = mystrdupa(url0);
  char *tim = strchr(url, '#');
  const char *siz;
  *tim++ = 0;
  int secs;

  if(!strcmp(tim, "cover"))
    secs = -1;
  else
    secs = atoi(tim);

  hts_mutex_lock(&image_from_video_mutex[0]);

  if(strcmp(url, stated_url ?: "")) {
    free(stated_url);
    stated_url = NULL;
    if(fa_stat_ex(url, &fs, errbuf, errlen, FA_NON_INTERACTIVE)) {
      hts_mutex_unlock(&image_from_video_mutex[0]);
      return NULL;
    }
    stated_url = strdup(url);
  }
  stattime = fs.fs_mtime;
  hts_mutex_unlock(&image_from_video_mutex[0]);

  if(im->im_req_width < 100 && im->im_req_height < 100) {
    siz = "min";
  } else if(im->im_req_width < 200 && im->im_req_height < 200) {
    siz = "mid";
  } else {
    siz = "max";
  }

  snprintf(cacheid, sizeof(cacheid), "%s-%s", url0, siz);
  buf_t *b = blobcache_get(cacheid, "videothumb", 0, 0, NULL, &mtime);
  if(b != NULL && mtime == stattime) {
    if(b->b_size > 0) {
      img = image_coded_create_from_buf(b, IMAGE_JPEG);
      buf_release(b);
      return img;
    }
    /* Stale 0-byte tombstone from previous broken decoder: discard and retry decoding */
    buf_release(b);
  } else {
    buf_release(b);
  }

  if(ONLY_CACHED(cache_control)) {
    snprintf(errbuf, errlen, "Not cached");
    return NULL;
  }

  hts_mutex_lock(&image_from_video_mutex[1]);
  img = fa_image_from_video2(url, im, cacheid, errbuf, errlen,
                             secs, stattime, c);
  if(img == NULL) {
    /* Cache a transient 0-byte tombstone (60s TTL) so repeated immediate draws don't re-trigger disk I/O,
     * but without permanently bricking thumbnails across sessions */
    buf_t *tombstone = buf_create(0);
    if(tombstone != NULL) {
      blobcache_put(cacheid, "videothumb", tombstone, 60, NULL, stattime, 0);
      buf_release(tombstone);
    }
  }
  hts_mutex_unlock(&image_from_video_mutex[1]);
  if(img != NULL)
    img->im_flags |= IMAGE_ADAPTED;
  return img;
}
#endif
