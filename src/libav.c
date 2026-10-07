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
#include <ctype.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>
#include <libavutil/channel_layout.h>

#include "main.h"
#include "media/media.h"
#include "libav.h"
#include "fileaccess/fa_libav.h"
#include "video/video_decoder.h"
#include "video/video_settings.h"

#if ENABLE_VDPAU
#include "video/vdpau.h"
#endif




static const int libav_colorspace_tbl[] = {
  [AVCOL_SPC_BT709]     = COLOR_SPACE_BT_709,
  [AVCOL_SPC_BT470BG]   = COLOR_SPACE_BT_601,
  [AVCOL_SPC_SMPTE170M] = COLOR_SPACE_BT_601,
  [AVCOL_SPC_SMPTE240M] = COLOR_SPACE_SMPTE_240M,
};


#define vd_valid_duration(t) ((t) > 10000ULL && (t) < 1000000ULL)


/**
 *
 */
static void
libav_deliver_frame(video_decoder_t *vd,
                    media_pipe_t *mp, media_queue_t *mq,
                    AVCodecContext *ctx, AVFrame *frame,
                    const media_buf_meta_t *mbm, int decode_time,
                    const media_codec_t *mc)
{
  frame_info_t fi;

  /* Compute aspect ratio */
  switch(mbm->mbm_aspect_override) {
  case 0:
  default:

    fi.fi_dar_num = frame->width;
    fi.fi_dar_den = frame->height;

    if(frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0) {
      fi.fi_dar_num *= frame->sample_aspect_ratio.num;
      fi.fi_dar_den *= frame->sample_aspect_ratio.den;
    } else if(ctx->sample_aspect_ratio.num > 0 && ctx->sample_aspect_ratio.den > 0) {
      fi.fi_dar_num *= ctx->sample_aspect_ratio.num;
      fi.fi_dar_den *= ctx->sample_aspect_ratio.den;
    } else if(mc->sar_num > 0 && mc->sar_den > 0) {
      fi.fi_dar_num *= mc->sar_num;
      fi.fi_dar_den *= mc->sar_den;
    }

    break;
  case 1:
    fi.fi_dar_num = 4;
    fi.fi_dar_den = 3;
    break;
  case 2:
    fi.fi_dar_num = 16;
    fi.fi_dar_den = 9;
    break;
  }

  if(fi.fi_dar_num <= 0 || fi.fi_dar_den <= 0) {
    fi.fi_dar_num = frame->width ? frame->width : 16;
    fi.fi_dar_den = frame->height ? frame->height : 9;
  }

  int64_t pts = video_decoder_infer_pts(mbm, vd,
					frame->pict_type == AV_PICTURE_TYPE_B);

  int duration = mbm->mbm_duration;

  if(!vd_valid_duration(duration)) {
    /* duration is zero or very invalid, use duration from last output */
    duration = vd->vd_estimated_duration;
  }

  if(pts == AV_NOPTS_VALUE && vd->vd_nextpts != AV_NOPTS_VALUE)
    pts = vd->vd_nextpts; /* no pts set, use estimated pts */

  if(pts != AV_NOPTS_VALUE && vd->vd_prevpts != AV_NOPTS_VALUE) {
    /* we know PTS of a prior frame */
    int64_t t = (pts - vd->vd_prevpts) / vd->vd_prevpts_cnt;

    if(vd_valid_duration(t)) {
      /* inter frame duration seems valid, store it */
      vd->vd_estimated_duration = t;
      if(duration == 0)
	duration = t;

    }
  }
  
  duration += frame->repeat_pict * duration / 2;
 
  if(pts != AV_NOPTS_VALUE) {
    vd->vd_prevpts = pts;
    vd->vd_prevpts_cnt = 0;
  }
  vd->vd_prevpts_cnt++;

  if(duration == 0) {
    TRACE(TRACE_DEBUG, "Video", "Dropping frame with duration = 0");
    return;
  }

  prop_set_int(mq->mq_prop_too_slow, decode_time > duration);

  if(pts != AV_NOPTS_VALUE) {
    vd->vd_nextpts = pts + duration;
  } else {
    vd->vd_nextpts = AV_NOPTS_VALUE;
  }
#if 0
  static int64_t lastpts = AV_NOPTS_VALUE;
  if(lastpts != AV_NOPTS_VALUE) {
    printf(" VDEC: %20"PRId64" : %-20"PRId64" %d %"PRId64" %6d %d epoch=%d\n", pts, pts - lastpts, mbm->mbm_drive_clock,
           mbm->mbm_user_time, duration, mbm->mbm_sequence, mbm->mbm_epoch);
#if 0
    if(pts - lastpts > 1000000) {
      abort();
    }
    #endif
  }
  lastpts = pts;
#endif


  media_discontinuity_debug(&vd->vd_debug_discont_out,
                            mbm->mbm_dts,
                            mbm->mbm_pts,
                            mbm->mbm_epoch,
                            mbm->mbm_skip,
                            "VOUT");

  /* In modern FFmpeg, interlacing and field parity flags are encapsulated within frame->flags */
  vd->vd_interlaced |=
    ((frame->flags & AV_FRAME_FLAG_INTERLACED) != 0) && !mbm->mbm_disable_deinterlacer;

  fi.fi_width = frame->width;
  fi.fi_height = frame->height;
  fi.fi_pts = pts;
  fi.fi_epoch = mbm->mbm_epoch;
  fi.fi_user_time = mbm->mbm_user_time;
  fi.fi_duration = duration;
  fi.fi_drive_clock = mbm->mbm_drive_clock;

  fi.fi_interlaced = !!vd->vd_interlaced;
  fi.fi_tff = !!(frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST);
  fi.fi_prescaled = 0;

  fi.fi_color_space = 
    ctx->colorspace < ARRAYSIZE(libav_colorspace_tbl) ? 
    libav_colorspace_tbl[ctx->colorspace] : 0;

  fi.fi_type = 'LAVC';

  // Check if we should skip directly to convert code
  if(vd->vd_convert_width  != frame->width ||
     vd->vd_convert_height != frame->height ||
     vd->vd_convert_pixfmt != frame->format) {

    // Nope, go ahead and deliver frame as-is

    fi.fi_data[0] = frame->data[0];
    fi.fi_data[1] = frame->data[1];
    fi.fi_data[2] = frame->data[2];

    fi.fi_pitch[0] = frame->linesize[0];
    fi.fi_pitch[1] = frame->linesize[1];
    fi.fi_pitch[2] = frame->linesize[2];

    fi.fi_pix_fmt = frame->format;
    fi.fi_avframe = frame;

    int r = video_deliver_frame(vd, &fi);

    /* return value
     * 0  = OK
     * 1  = Need convert to YUV420P
     * -1 = Fail
     */

    if(r != 1)
      return;
  }

  // Need to convert frame

  vd->vd_sws =
    sws_getCachedContext(vd->vd_sws,
                         frame->width, frame->height, frame->format,
                         frame->width, frame->height, AV_PIX_FMT_YUV420P,
                         0, NULL, NULL, NULL);

  if(vd->vd_sws == NULL) {
    TRACE(TRACE_ERROR, "Video", "Unable to convert from %s to %s",
	  av_get_pix_fmt_name(frame->format),
	  av_get_pix_fmt_name(AV_PIX_FMT_YUV420P));
    return;
  }

  if(vd->vd_convert_width  != frame->width  ||
     vd->vd_convert_height != frame->height ||
     vd->vd_convert_pixfmt != frame->format ||
     vd->vd_convert_frame == NULL) {
    if(vd->vd_convert_frame != NULL) {
      av_frame_free(&vd->vd_convert_frame);
    }

    vd->vd_convert_width  = frame->width;
    vd->vd_convert_height = frame->height;
    vd->vd_convert_pixfmt = frame->format;

    vd->vd_convert_frame = av_frame_alloc();
    vd->vd_convert_frame->format = AV_PIX_FMT_YUV420P;
    vd->vd_convert_frame->width  = frame->width;
    vd->vd_convert_frame->height = frame->height;
    av_frame_get_buffer(vd->vd_convert_frame, 32);

    TRACE(TRACE_DEBUG, "Video", "Converting from %s to %s",
	  av_get_pix_fmt_name(frame->format),
	  av_get_pix_fmt_name(AV_PIX_FMT_YUV420P));
  }

  sws_scale(vd->vd_sws, (void *)frame->data, frame->linesize, 0,
            frame->height, vd->vd_convert_frame->data, vd->vd_convert_frame->linesize);

  fi.fi_data[0] = vd->vd_convert_frame->data[0];
  fi.fi_data[1] = vd->vd_convert_frame->data[1];
  fi.fi_data[2] = vd->vd_convert_frame->data[2];

  fi.fi_pitch[0] = vd->vd_convert_frame->linesize[0];
  fi.fi_pitch[1] = vd->vd_convert_frame->linesize[1];
  fi.fi_pitch[2] = vd->vd_convert_frame->linesize[2];

  fi.fi_type = 'LAVC';
  fi.fi_pix_fmt = AV_PIX_FMT_YUV420P;
  fi.fi_avframe = NULL;
  video_deliver_frame(vd, &fi);
}



/**
 *
 */
/**
 * Flushes internal decoder buffers and queues.
 *
 * In modern FFmpeg (FFmpeg 3.1+ / 9.0), avcodec_flush_buffers() cleanly resets
 * the internal codec state, dropping all pending B-frames and picture queues.
 *
 * @param mc Pointer to the media codec wrapper.
 * @param vd Pointer to the video decoder state machine.
 */
static void
libav_video_flush(media_codec_t *mc, video_decoder_t *vd)
{
  AVCodecContext *ctx = mc->ctx;
  if(ctx != NULL) {
    avcodec_flush_buffers(ctx);
  }
}


/**
 * Drains all delayed/buffered frames from the decoder at end-of-stream or discontinuity.
 *
 * In modern FFmpeg, passing a NULL packet (or sending a NULL packet via avcodec_send_packet)
 * enters the draining state. We then repeatedly invoke avcodec_receive_frame() until
 * AVERROR_EOF is returned, ensuring all cached B-frames and delayed reference frames
 * are presented and displayed.
 *
 * @param mc Pointer to the media codec wrapper.
 * @param vd Pointer to the video decoder state machine.
 * @param mq Pointer to the media queue receiving metadata updates.
 */
static void
libav_video_eof(media_codec_t *mc, video_decoder_t *vd,
                struct media_queue *mq)
{
  media_pipe_t *mp = vd->vd_mp;
  AVCodecContext *ctx = mc->ctx;
  AVFrame *frame = vd->vd_frame;
  int t;

  if(ctx == NULL)
    return;

  /* Enter draining mode by sending a NULL packet */
  avcodec_send_packet(ctx, NULL);

  while(1) {
    avgtime_start(&vd->vd_decode_time);

    /* Fetch next available reconstructed frame from the draining queue */
    int ret = avcodec_receive_frame(ctx, frame);

    t = avgtime_stop(&vd->vd_decode_time, mq->mq_prop_decode_avg,
                     mq->mq_prop_decode_peak);

    /* Non-zero return indicates either AVERROR_EOF or no more frames available */
    if(ret != 0)
      break;

    /* In modern FFmpeg, pkt->opaque is copied to frame->opaque under AV_CODEC_FLAG_COPY_OPAQUE */
    uintptr_t reorder_idx = ((uintptr_t)frame->opaque) & VIDEO_DECODER_REORDER_MASK;
    const media_buf_meta_t *mbm = &vd->vd_reorder[reorder_idx];
    if(!mbm->mbm_skip)
      libav_deliver_frame(vd, mp, mq, ctx, frame, mbm, t, mc);

    av_frame_unref(frame);
  }

  /* Reset codec state after complete draining */
  avcodec_flush_buffers(ctx);
}

#include "misc/minmax.h"

/**
 * Decodes compressed video packets into raw reconstructed frames using modern FFmpeg send/receive API.
 *
 * Modern FFmpeg completely eliminated avcodec_decode_video2() in favor of the decoupled
 * avcodec_send_packet() and avcodec_receive_frame() state machine.
 * A single input packet may yield 0, 1, or multiple video frames (or none if the packet
 * is consumed purely as a reference frame).
 *
 * @param mc Pointer to the media codec wrapper.
 * @param vd Pointer to the video decoder state machine.
 * @param mq Pointer to the media queue for timing telemetry.
 * @param mb Pointer to the incoming media buffer holding compressed bitstream data.
 * @param reqsize Unused requested buffer size parameter.
 */
static void
libav_decode_video(struct media_codec *mc, struct video_decoder *vd,
                   struct media_queue *mq, struct media_buf *mb, int reqsize)
{
  media_pipe_t *mp = vd->vd_mp;
  AVCodecContext *ctx = mc->ctx;
  AVFrame *frame = vd->vd_frame;
  int t;

  if(mb->mb_flush)
    libav_video_eof(mc, vd, mq);

  /* Preserve reorder index across the circular reorder queue */
  copy_mbm_from_mb(&vd->vd_reorder[vd->vd_reorder_ptr], mb);
  /* In modern FFmpeg, attach reorder index to packet opaque; propagated to frame->opaque under AV_CODEC_FLAG_COPY_OPAQUE */
  mb->mb_pkt.opaque = (void *)(uintptr_t)vd->vd_reorder_ptr;
  vd->vd_reorder_ptr = (vd->vd_reorder_ptr + 1) & VIDEO_DECODER_REORDER_MASK;

  /*
   * If seeking, instruct the decoder to discard non-reference frames to accelerate keyframe seeking
   */
  ctx->skip_frame = mb->mb_skip == 1 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
  avgtime_start(&vd->vd_decode_time);

  /* Send packet to decoder */
  int ret = avcodec_send_packet(ctx, &mb->mb_pkt);
  if(ret == AVERROR(EAGAIN)) {
    /*
     * Decoder internal input queue is saturated. Drain available decoded frames
     * first to free internal decoder slots, then retry packet submission.
     */
    while(1) {
      ret = avcodec_receive_frame(ctx, frame);
      t = avgtime_stop(&vd->vd_decode_time, mq->mq_prop_decode_avg,
                       mq->mq_prop_decode_peak);
      if(ret != 0)
        break;

      mp_set_mq_meta(mq, ctx->codec, ctx);

      uintptr_t reorder_idx = ((uintptr_t)frame->opaque) & VIDEO_DECODER_REORDER_MASK;
      const media_buf_meta_t *mbm = &vd->vd_reorder[reorder_idx];
      if(!mbm->mbm_skip)
        libav_deliver_frame(vd, mp, mq, ctx, frame, mbm, t, mc);

      av_frame_unref(frame);
      avgtime_start(&vd->vd_decode_time);
    }
    ret = avcodec_send_packet(ctx, &mb->mb_pkt);
  }

  if(ret < 0 && ret != AVERROR_EOF) {
    avgtime_stop(&vd->vd_decode_time, mq->mq_prop_decode_avg, mq->mq_prop_decode_peak);
    return;
  }

  /* Receive all decoded frames resulting from this packet */
  while(1) {
    ret = avcodec_receive_frame(ctx, frame);
    t = avgtime_stop(&vd->vd_decode_time, mq->mq_prop_decode_avg,
		     mq->mq_prop_decode_peak);

    if(ret != 0)
      break;

    mp_set_mq_meta(mq, ctx->codec, ctx);

    uintptr_t reorder_idx = ((uintptr_t)frame->opaque) & VIDEO_DECODER_REORDER_MASK;
    const media_buf_meta_t *mbm = &vd->vd_reorder[reorder_idx];
    if(!mbm->mbm_skip)
      libav_deliver_frame(vd, mp, mq, ctx, frame, mbm, t, mc);

    av_frame_unref(frame);

    /* Restart timer for next potential frame in this packet */
    avgtime_start(&vd->vd_decode_time);
  }
}


/**
 *
 */
static enum AVPixelFormat
libav_get_format(struct AVCodecContext *ctx, const enum AVPixelFormat *fmt)
{
  media_codec_t *mc = ctx->opaque;
  if(mc->close != NULL) {
    mc->close(mc);
    mc->close = NULL;
  }

#if ENABLE_VDPAU
  if(!vdpau_init_libav_decode(mc, ctx)) {
    return AV_PIX_FMT_VDPAU;
  }
#endif
  mc->get_buffer2 = &avcodec_default_get_buffer2;
  return avcodec_default_get_format(ctx, fmt);
}


/**
 *
 */
static int
get_buffer2_wrapper(struct AVCodecContext *s, AVFrame *frame, int flags)
{
  media_codec_t *mc = s->opaque;
  return mc->get_buffer2(s, frame, flags);
}

/**
 *
 */
static int
media_codec_create_lavc(media_codec_t *cw, const media_codec_params_t *mcp,
                        media_pipe_t *mp)
{
  const AVCodec *codec = avcodec_find_decoder(cw->codec_id);

  if(codec == NULL)
    return -1;

  cw->ctx = avcodec_alloc_context3(codec);
  if(cw->fmt_ctx != NULL) {
    /*
     * Copy codec parameters from the format context to the decoder context.
     * Replaces deprecated/removed avcodec_copy_context() via an intermediate AVCodecParameters.
     */
    AVCodecParameters *par = avcodec_parameters_alloc();
    if(par != NULL) {
      avcodec_parameters_from_context(par, cw->fmt_ctx);
      avcodec_parameters_to_context(cw->ctx, par);
      avcodec_parameters_free(&par);
    }
  }

  // cw->ctx->debug = FF_DEBUG_PICT_INFO | FF_DEBUG_BUGS;

  if(mcp != NULL && mcp->extradata != NULL && !cw->ctx->extradata) {
    cw->ctx->extradata = calloc(1, mcp->extradata_size +
				AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(cw->ctx->extradata, mcp->extradata, mcp->extradata_size);
    cw->ctx->extradata_size = mcp->extradata_size;
  }

  if(mcp && mcp->cheat_for_speed)
    cw->ctx->flags2 |= AV_CODEC_FLAG2_FAST;

  if(codec->type == AVMEDIA_TYPE_VIDEO) {

    /* Enable automatic copying of pkt->opaque into decoded frame->opaque in modern FFmpeg */
    cw->ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;

    cw->get_buffer2 = &avcodec_default_get_buffer2;

    // If we run with vdpau and h264 libav will crash when going
    // back and forth between accelerated and non-accelerated mode
#if defined(PLATFORM_PS3) || defined(__PPU__) || defined(PS3)
    /**
     * PSL1GHT libpthread threading within libavcodec induces PPU thread deadlocks
     * and condition variable aborts (sys_cond_wait timeout) due to 64KB stack limits.
     * Force single-threaded decoding on PS3 so that decoding executes synchronously
     * and safely on Movian's dedicated 'video decoder' thread without spawning pthreads.
     */
    cw->ctx->thread_count = 1;
#else
    if(!(video_settings.vdpau && cw->codec_id == AV_CODEC_ID_H264))
      cw->ctx->thread_count = gconf.concurrency;
#endif

    cw->ctx->opaque = cw;
    cw->ctx->get_format = &libav_get_format;
    cw->ctx->get_buffer2 = &get_buffer2_wrapper;

    cw->decode = &libav_decode_video;
    cw->flush  = &libav_video_flush;
  }

  if(avcodec_open2(cw->ctx, codec, NULL) < 0) {
    TRACE(TRACE_INFO, "libav", "Unable to open codec %s",
	  codec ? codec->name : "<noname>");

    avcodec_free_context(&cw->ctx);

    return -1;
  }

  return 0;
}


REGISTER_CODEC(NULL, media_codec_create_lavc, 1000);

/**
 *
 */
media_format_t *
media_format_create(AVFormatContext *fctx)
{
  media_format_t *fw = malloc(sizeof(media_format_t));
  atomic_set(&fw->refcount, 1);
  fw->fctx = fctx;
  return fw;
}


/**
 *
 */
void
media_format_deref(media_format_t *fw)
{
  if(atomic_dec(&fw->refcount))
    return;
  fa_libav_close_format(fw->fctx, 0);
  free(fw);
}


/**
 *
 */
void
metadata_from_libav(char *dst, size_t dstlen,
		    const AVCodec *codec, const AVCodecContext *avctx)
{
  const char *name = codec->name;
  const char *profile = av_get_profile_name(codec, avctx->profile);

  if(codec->id == AV_CODEC_ID_DTS && profile != NULL)
    name = NULL;

  int off = 0;

  if(name) {
    off = snprintf(dst, dstlen, "%s", codec->name);
    char *n = dst;
    while(*n) {
      *n = toupper((int)*n);
      n++;
    }
  }

  if(profile != NULL)
    off += snprintf(dst + off, dstlen - off,
                    "%s%s", off ? " " : "", profile);

  if(codec->id == AV_CODEC_ID_H264 && avctx->level != AV_LEVEL_UNKNOWN)
    off += snprintf(dst + off, dstlen - off,
                    " (Level %d.%d)",
                    avctx->level / 10, avctx->level % 10);

  if(avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
    char buf[64] = {0};

    /* Modern FFmpeg channel layout description */
    av_channel_layout_describe(&avctx->ch_layout, buf, sizeof(buf));

    off += snprintf(dst + off, dstlen - off, ", %d Hz, %s",
		    avctx->sample_rate, buf);
  }

  if(avctx->width)
    off += snprintf(dst + off, dstlen - off,
		    ", %dx%d", avctx->width, avctx->height);

  if(avctx->hwaccel != NULL)
    off += snprintf(dst + off, dstlen - off, " (%s)",
                    avctx->hwaccel->name);
}

/**
 * Updates media queue stream metadata properties for UI display.
 *
 * @param mq Media queue whose metadata properties are being refreshed.
 * @param codec Active AVCodec decoder definition.
 * @param avctx Active AVCodecContext instance containing stream parameters.
 */
void
mp_set_mq_meta(media_queue_t *mq, const AVCodec *codec,
	       const AVCodecContext *avctx)
{
  uint64_t layout_mask = (avctx->ch_layout.order == AV_CHANNEL_ORDER_NATIVE) ?
                         avctx->ch_layout.u.mask : 0;
  int channels = avctx->ch_layout.nb_channels;

  if(mq->mq_meta_codec_id       == codec->id &&
     mq->mq_meta_profile        == avctx->profile &&
     mq->mq_meta_channels       == channels &&
     mq->mq_meta_channel_layout == layout_mask &&
     mq->mq_meta_width          == avctx->width &&
     mq->mq_meta_height         == avctx->height)
    return;

  mq->mq_meta_codec_id       = codec->id;
  mq->mq_meta_profile        = avctx->profile;
  mq->mq_meta_channels       = channels;
  mq->mq_meta_channel_layout = layout_mask;
  mq->mq_meta_width          = avctx->width;
  mq->mq_meta_height         = avctx->height;

  char buf[128];
  metadata_from_libav(buf, sizeof(buf), codec, avctx);
  prop_set_string(mq->mq_prop_codec, buf);
}


