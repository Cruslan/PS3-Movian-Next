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
#include <unistd.h>
#include <assert.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mem.h>

#include "misc/queue.h"
#include "main.h"
#include "image/pixmap.h"
#include "glw_rec.h"
#include "misc/minmax.h"
#include "audio2/audio.h"

LIST_HEAD(glw_rec_list, glw_rec);
LIST_HEAD(audio_source_list, audio_source);
TAILQ_HEAD(audio_buf_queue, audio_buf);
TAILQ_HEAD(video_frame_queue, video_frame);

static HTS_MUTEX_DECL(glw_rec_mutex);

static struct glw_rec_list glw_recs;



typedef struct audio_buf {
  TAILQ_ENTRY(audio_buf) ab_link;
  int16_t *ab_buf;
  int ab_samples;
  int ab_used;
  int64_t ab_ts;
} audio_buf_t;


typedef struct audio_source {
  LIST_ENTRY(audio_source) as_link;
  int as_id;
  int as_format;
  int64_t as_channel_layout;
  int as_sample_rate;
  /** Modern FFmpeg audio resampler context */
  SwrContext *as_avr;
  struct audio_buf_queue as_queue;


  int as_start_drop;
  int as_samples_queued;

  int as_samples_consumed;

  int64_t as_last_ts;
  int as_last_ts_sample;

} audio_source_t;



typedef struct video_frame {
  TAILQ_ENTRY(video_frame) vf_link;
  pixmap_t *vf_pm;
} video_frame_t;



struct glw_rec {

  LIST_ENTRY(glw_rec) global_link;
  char *filename;
  const AVOutputFormat *fmt;
  AVFormatContext *oc;

  AVCodecContext *v_ctx;
  AVStream *v_st;

  AVCodecContext *a_ctx;
  AVStream *a_st;

  int width;
  int height;
  int fps;
  int framenum;

  int64_t video_pts;
  int64_t audio_pts;

  int samples_written;
  struct audio_source_list asources;

  hts_cond_t gr_cond;
  struct video_frame_queue gr_vframes;
  int gr_vqlen;
  int gr_stop;
};



/**
 *
 */
/**
 * @brief Emits buffered audio samples into the recording stream.
 *
 * Pulls accumulated PCM audio samples from registered audio sources, mixes them
 * into an interleaved 16-bit stereo buffer, packages them into an AVFrame,
 * and submits them to the modern FFmpeg audio encoder using avcodec_send_frame()
 * and avcodec_receive_packet().
 *
 * @param gr Recording context.
 * @param pts Presentation timestamp boundary up to which audio should be processed.
 */
static void
emit_audio(glw_rec_t *gr, int64_t pts)
{
#define SAMPLES_PER_FRAME 1024
  int16_t data[SAMPLES_PER_FRAME * 2];

  while(1) {
    memset(data, 0, sizeof(data));

    hts_mutex_lock(&glw_rec_mutex);
    audio_source_t *as;

    LIST_FOREACH(as, &gr->asources, as_link) {
      if(as->as_samples_queued < 1024) {
        hts_mutex_unlock(&glw_rec_mutex);
        return;
      }
    }

    LIST_FOREACH(as, &gr->asources, as_link) {
      int offset = 0;

      while(as->as_samples_queued > 0 && offset < SAMPLES_PER_FRAME) {
        audio_buf_t *ab = TAILQ_FIRST(&as->as_queue);

        if(ab->ab_ts != AV_NOPTS_VALUE) {
          as->as_last_ts = ab->ab_ts;
          as->as_last_ts_sample = as->as_samples_consumed;
        }

        int consume = MIN(ab->ab_samples - ab->ab_used,
                          SAMPLES_PER_FRAME - offset);
        assert(consume > 0);
        for(int i = 0; i < consume; i++) {
          data[(offset + i) * 2 + 0] += ab->ab_buf[(i + ab->ab_used) * 2 + 0];
          data[(offset + i) * 2 + 1] += ab->ab_buf[(i + ab->ab_used) * 2 + 1];
        }
        offset += consume;
        ab->ab_used += consume;
        as->as_samples_consumed += consume;
        assert(ab->ab_used <= ab->ab_samples);
        as->as_samples_queued -= consume;
        if(ab->ab_used == ab->ab_samples) {
          free(ab->ab_buf);
          TAILQ_REMOVE(&as->as_queue, ab, ab_link);
          free(ab);
        }
      }
    }

    hts_mutex_unlock(&glw_rec_mutex);

    int64_t ts = av_rescale_q(gr->samples_written,
                              (AVRational){1, 48000},
                              gr->a_st->time_base);

    if(ts >= 0) {
      AVFrame *frame = av_frame_alloc();
      AVPacket *pkt = av_packet_alloc();

      if(frame != NULL && pkt != NULL) {
        frame->nb_samples = SAMPLES_PER_FRAME;
        frame->format = gr->a_ctx->sample_fmt;
        av_channel_layout_copy(&frame->ch_layout, &gr->a_ctx->ch_layout);
        frame->sample_rate = gr->a_ctx->sample_rate;
        frame->pts = ts;

        if(av_frame_get_buffer(frame, 0) >= 0) {
          memcpy(frame->data[0], data, SAMPLES_PER_FRAME * 2 * sizeof(int16_t));

          /* Submit PCM audio frame to encoder */
          if(avcodec_send_frame(gr->a_ctx, frame) >= 0) {
            while(avcodec_receive_packet(gr->a_ctx, pkt) == 0) {
              pkt->stream_index = gr->a_st->index;
              av_packet_rescale_ts(pkt, gr->a_ctx->time_base, gr->a_st->time_base);
              av_interleaved_write_frame(gr->oc, pkt);
              av_packet_unref(pkt);
            }
          }
        }
      }

      if(frame != NULL)
        av_frame_free(&frame);
      if(pkt != NULL)
        av_packet_free(&pkt);
    }

    gr->samples_written += SAMPLES_PER_FRAME;

    int64_t t = av_rescale_q(gr->samples_written,
                             (AVRational){1, 48000},
                             AV_TIME_BASE_Q);
    if(t >= pts)
      break;
  }
}


/**
 * @brief Encodes a raw video pixmap frame into the recording container.
 *
 * Allocates a modern AVFrame, attaches the pixmap scanline buffers, timestamps
 * the frame, and feeds it into the video encoder pipeline via avcodec_send_frame().
 * Encoded bitstream packets are collected via avcodec_receive_packet() and
 * written to the muxer container.
 *
 * @param gr Recording context.
 * @param pm Raw UI pixmap containing the captured frame.
 */
static void
encode_vframe(glw_rec_t *gr, struct pixmap *pm)
{
  AVFrame *frame = av_frame_alloc();
  AVPacket *pkt = av_packet_alloc();

  if(frame == NULL || pkt == NULL) {
    if(frame != NULL) av_frame_free(&frame);
    if(pkt != NULL) av_packet_free(&pkt);
    return;
  }

  frame->format = gr->v_ctx->pix_fmt;
  frame->width = gr->width;
  frame->height = gr->height;
  frame->data[0] = pm->pm_data + pm->pm_linesize * (gr->height - 1);
  frame->linesize[0] = -pm->pm_linesize;
  frame->pts = 1000000LL * gr->framenum / gr->fps;
  gr->framenum++;

  int64_t pts = frame->pts;

  /* Dispatch raw video frame into modern FFmpeg encoder */
  if(avcodec_send_frame(gr->v_ctx, frame) >= 0) {
    while(avcodec_receive_packet(gr->v_ctx, pkt) == 0) {
      pkt->stream_index = gr->v_st->index;
      av_packet_rescale_ts(pkt, gr->v_ctx->time_base, gr->v_st->time_base);
      av_interleaved_write_frame(gr->oc, pkt);
      av_packet_unref(pkt);
    }
  }

  av_frame_free(&frame);
  av_packet_free(&pkt);

  emit_audio(gr, pts);
}


/**
 * @brief Background recording thread function.
 *
 * Allocates modern FFmpeg format and codec contexts, initializes video and audio
 * encoders (FFVHUFF and PCM S16LE), writes container headers, processes queued UI
 * frames in a loop, and cleanly flushes/finalizes the recording output on termination.
 *
 * @param aux Pointer to glw_rec_t recording structure.
 * @return NULL on thread exit.
 */
static void *
rec_thread(void *aux)
{
  glw_rec_t *gr = aux;
  video_frame_t *vf;

  gr->fmt = av_guess_format(NULL, gr->filename, NULL);
  if(gr->fmt == NULL) {
    TRACE(TRACE_ERROR, "REC",
	  "Unable to record to %s -- Unknown file format",
	  gr->filename);
    return NULL;
  }

  if(avformat_alloc_output_context2(&gr->oc, gr->fmt, NULL, gr->filename) < 0 || gr->oc == NULL) {
    TRACE(TRACE_ERROR, "REC",
	  "Unable to allocate output context for %s",
	  gr->filename);
    return NULL;
  }

  /* Initialize video stream and encoder */
  const AVCodec *v_codec = avcodec_find_encoder(AV_CODEC_ID_FFVHUFF);
  if(v_codec == NULL) {
    TRACE(TRACE_ERROR, "REC", "Unable to find FFVHUFF encoder");
    return NULL;
  }

  gr->v_st = avformat_new_stream(gr->oc, NULL);
  if(gr->v_st == NULL)
    return NULL;

  gr->v_st->avg_frame_rate.num = gr->fps;
  gr->v_st->avg_frame_rate.den = 1;

  gr->v_ctx = avcodec_alloc_context3(v_codec);
  if(gr->v_ctx == NULL)
    return NULL;

  gr->v_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
  gr->v_ctx->codec_id = AV_CODEC_ID_FFVHUFF;
  gr->v_ctx->width = gr->width;
  gr->v_ctx->height = gr->height;
  gr->v_ctx->time_base.den = gr->fps;
  gr->v_ctx->time_base.num = 1;
  gr->v_ctx->pix_fmt = AV_PIX_FMT_RGB32;
  gr->v_ctx->thread_count = gconf.concurrency;

  if(gr->oc->oformat->flags & AVFMT_GLOBALHEADER)
    gr->v_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  if(avcodec_open2(gr->v_ctx, v_codec, NULL) < 0) {
    TRACE(TRACE_ERROR, "REC",
	  "Unable to record to %s -- Unable to open video codec",
	  gr->filename);
    return NULL;
  }

  avcodec_parameters_from_context(gr->v_st->codecpar, gr->v_ctx);
  gr->v_st->time_base = gr->v_ctx->time_base;

  /* Initialize audio stream and encoder */
  const AVCodec *a_codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
  if(a_codec == NULL) {
    TRACE(TRACE_ERROR, "REC", "Unable to find PCM_S16LE encoder");
    return NULL;
  }

  gr->a_st = avformat_new_stream(gr->oc, NULL);
  if(gr->a_st == NULL)
    return NULL;

  gr->a_ctx = avcodec_alloc_context3(a_codec);
  if(gr->a_ctx == NULL)
    return NULL;

  gr->a_ctx->codec_type = AVMEDIA_TYPE_AUDIO;
  gr->a_ctx->codec_id = AV_CODEC_ID_PCM_S16LE;
  gr->a_ctx->sample_rate = 48000;
  gr->a_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
  av_channel_layout_default(&gr->a_ctx->ch_layout, 2);
  gr->a_ctx->time_base.den = 48000;
  gr->a_ctx->time_base.num = 1;

  if(gr->oc->oformat->flags & AVFMT_GLOBALHEADER)
    gr->a_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  gr->a_ctx->thread_count = 1;
  if(avcodec_open2(gr->a_ctx, a_codec, NULL) < 0) {
    TRACE(TRACE_ERROR, "REC",
	  "Unable to record to %s -- Unable to open audio codec",
	  gr->filename);
    return NULL;
  }

  avcodec_parameters_from_context(gr->a_st->codecpar, gr->a_ctx);
  gr->a_st->time_base = gr->a_ctx->time_base;

  av_dump_format(gr->oc, 0, gr->filename, 1);

  if(!(gr->oc->oformat->flags & AVFMT_NOFILE)) {
    if(avio_open(&gr->oc->pb, gr->filename, AVIO_FLAG_WRITE) < 0) {
      TRACE(TRACE_ERROR, "REC",
	    "Unable to record to %s -- Unable to open file for writing",
	    gr->filename);
      return NULL;
    }
  }

  /* Write the stream header */
  if(avformat_write_header(gr->oc, NULL) < 0) {
    TRACE(TRACE_ERROR, "REC",
          "Unable to write container header for %s",
          gr->filename);
    if(gr->v_ctx != NULL)
      avcodec_free_context(&gr->v_ctx);
    if(gr->a_ctx != NULL)
      avcodec_free_context(&gr->a_ctx);
    if(gr->oc != NULL && !(gr->oc->oformat->flags & AVFMT_NOFILE))
      avio_closep(&gr->oc->pb);
    if(gr->oc != NULL)
      avformat_free_context(gr->oc);
    free(gr->filename);
    free(gr);
    return NULL;
  }

  hts_mutex_lock(&glw_rec_mutex);

  while(!gr->gr_stop) {
    vf = TAILQ_FIRST(&gr->gr_vframes);
    if(vf == NULL) {
      hts_cond_wait(&gr->gr_cond, &glw_rec_mutex);
      continue;
    }
    TAILQ_REMOVE(&gr->gr_vframes, vf, vf_link);
    gr->gr_vqlen--;
    hts_mutex_unlock(&glw_rec_mutex);
    encode_vframe(gr, vf->vf_pm);
    pixmap_release(vf->vf_pm);
    free(vf);
    hts_mutex_lock(&glw_rec_mutex);
  }

  hts_mutex_unlock(&glw_rec_mutex);

  av_write_trailer(gr->oc);

  /* Clean up modern contexts */
  if(gr->v_ctx != NULL)
    avcodec_free_context(&gr->v_ctx);
  if(gr->a_ctx != NULL)
    avcodec_free_context(&gr->a_ctx);

  if(gr->oc != NULL && !(gr->oc->oformat->flags & AVFMT_NOFILE))
    avio_closep(&gr->oc->pb);

  if(gr->oc != NULL)
    avformat_free_context(gr->oc);

  free(gr->filename);
  free(gr);
  return NULL;
}


/**
 *
 */
glw_rec_t *
glw_rec_init(const char *filename, int width, int height, int fps)
{
  struct glw_rec *gr = calloc(1, sizeof(glw_rec_t));

  TAILQ_INIT(&gr->gr_vframes);
  //  gr->samples_written = -40000;
  gr->width = width;
  gr->height = height;
  gr->fps = fps;
  gr->filename = strdup(filename);
  hts_cond_init(&gr->gr_cond, &glw_rec_mutex);

  hts_mutex_lock(&glw_rec_mutex);
  LIST_INSERT_HEAD(&glw_recs, gr, global_link);
  hts_mutex_unlock(&glw_rec_mutex);

  hts_thread_create_detached("rec", rec_thread, gr, THREAD_PRIO_BGTASK);
  return gr;
}


/**
 *
 */
void
glw_rec_stop(glw_rec_t *gr)
{
  hts_mutex_lock(&glw_rec_mutex);
  gr->gr_stop = 1;
  hts_cond_signal(&gr->gr_cond);
  LIST_REMOVE(gr, global_link);
  hts_mutex_unlock(&glw_rec_mutex);
}


/**
 * @brief Receives decoded audio frames and stages them for the UI recorder.
 *
 * Configures modern libswresample conversion from source audio sample format,
 * rate, and channel mask into 48 kHz signed 16-bit interleaved stereo PCM.
 *
 * @param ad Decoder instance emitting audio frames.
 * @param frame Decoded AVFrame containing audio samples, or NULL when stream terminates.
 * @param pts Frame presentation timestamp.
 */
void
glw_rec_audio_send(struct audio_decoder *ad, AVFrame *frame, int64_t pts)
{
  glw_rec_t *gr;

  if(LIST_FIRST(&glw_recs) == NULL)
    return;

  hts_mutex_lock(&glw_rec_mutex);
  LIST_FOREACH(gr, &glw_recs, global_link) {

    audio_source_t *as;

    LIST_FOREACH(as, &gr->asources, as_link) {
      if(as->as_id == ad->ad_id)
        break;
    }

    if(frame == NULL) {
      if(as == NULL)
        continue;
      LIST_REMOVE(as, as_link);
      if(as->as_avr != NULL)
        swr_free(&as->as_avr);
      audio_buf_t *ab;
      while((ab = TAILQ_FIRST(&as->as_queue)) != NULL) {
        TAILQ_REMOVE(&as->as_queue, ab, ab_link);
        free(ab->ab_buf);
        free(ab);
      }
      free(as);
      printf("Stream %d stopped\n", ad->ad_id);
      continue;
    }

    if(as == NULL) {
      printf("Delay = %d\n", ad->ad_delay);

      as = calloc(1, sizeof(audio_source_t));
      as->as_last_ts = AV_NOPTS_VALUE;
      TAILQ_INIT(&as->as_queue);
      LIST_INSERT_HEAD(&gr->asources, as, as_link);
      as->as_id = ad->ad_id;
      as->as_start_drop = 24000;
    }

    if(as->as_format != ad->ad_in_sample_format ||
       as->as_channel_layout != ad->ad_in_channel_layout ||
       as->as_sample_rate != ad->ad_in_sample_rate) {

      if(as->as_avr != NULL)
        swr_free(&as->as_avr);

      as->as_format = ad->ad_in_sample_format;
      as->as_channel_layout = ad->ad_in_channel_layout;
      as->as_sample_rate = ad->ad_in_sample_rate;

      AVChannelLayout in_layout = {0};
      AVChannelLayout out_layout = {0};

      av_channel_layout_from_mask(&in_layout, as->as_channel_layout);
      av_channel_layout_default(&out_layout, 2);

      swr_alloc_set_opts2(&as->as_avr,
                          &out_layout, AV_SAMPLE_FMT_S16, 48000,
                          &in_layout, as->as_format, as->as_sample_rate,
                          0, NULL);

      char buf1[128];
      av_channel_layout_describe(&in_layout, buf1, sizeof(buf1));

      av_channel_layout_uninit(&in_layout);
      av_channel_layout_uninit(&out_layout);

      TRACE(TRACE_DEBUG, "REC",
            "Converting from [%s %dHz %s]",
            buf1, as->as_sample_rate,
            av_get_sample_fmt_name(as->as_format));

      if(as->as_avr == NULL || swr_init(as->as_avr) < 0) {
        TRACE(TRACE_ERROR, "REC", "Unable to open resampler");
        if(as->as_avr != NULL)
          swr_free(&as->as_avr);
      }
    }

    if(as->as_avr == NULL)
      continue;

    int max_out = swr_get_out_samples(as->as_avr, frame->nb_samples);
    if(max_out <= 0)
      continue;

    int bytes = max_out * sizeof(int16_t) * 2;
    int16_t *buf = malloc(bytes);
    if(buf == NULL)
      continue;

    uint8_t *out_planes[1] = { (uint8_t *)buf };
    int converted = swr_convert(as->as_avr, out_planes, max_out,
                                (const uint8_t * const *)frame->data,
                                frame->nb_samples);

    if(converted > 0) {
      if(as->as_start_drop > 0) {
        int drop = MIN(converted, as->as_start_drop);
        as->as_start_drop -= drop;

        if(converted > drop) {
          int rem = converted - drop;
          memmove(buf, buf + drop * 2, rem * sizeof(int16_t) * 2);
          audio_buf_t *ab = calloc(1, sizeof(audio_buf_t));
          ab->ab_buf = buf;
          ab->ab_samples = rem;
          for(int i = 0; i < rem * 2; i++) {
            ab->ab_buf[i] *= ad->ad_vol_scale;
          }
          TAILQ_INSERT_TAIL(&as->as_queue, ab, ab_link);
          as->as_samples_queued += rem;
        } else {
          free(buf);
        }
      } else {
        audio_buf_t *ab = calloc(1, sizeof(audio_buf_t));
        ab->ab_buf = buf;
        ab->ab_samples = converted;
        for(int i = 0; i < converted * 2; i++) {
          ab->ab_buf[i] *= ad->ad_vol_scale;
        }
        TAILQ_INSERT_TAIL(&as->as_queue, ab, ab_link);
        as->as_samples_queued += converted;
      }
    } else {
      free(buf);
    }
  }
  hts_mutex_unlock(&glw_rec_mutex);
}


void
glw_rec_deliver_vframe(glw_rec_t *gr, struct pixmap *pm)
{
  video_frame_t *vf = calloc(1, sizeof(video_frame_t));
  vf->vf_pm = pixmap_dup(pm);

  hts_mutex_lock(&glw_rec_mutex);
  hts_cond_signal(&gr->gr_cond);
  TAILQ_INSERT_TAIL(&gr->gr_vframes, vf, vf_link);
  gr->gr_vqlen++;
  if(gr->gr_vqlen > 10)
    TRACE(TRACE_ERROR, "REC", "Warning video queue length is %d", gr->gr_vqlen);
  hts_mutex_unlock(&glw_rec_mutex);
}
