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
#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <altivec.h>

/**
 * PSL1GHT v2 Audio and Event Queue Subsystems:
 * Standardizes on 48 kHz float interleaved PCM with hardware notify event queue pacing.
 */
#include <audio/audio.h>
#include <sys/event_queue.h>

#include <libavutil/avutil.h>

#include "main.h"
#include "media/media.h"
#include "audio2/audio.h"

/**
 * @brief PS3 Audio Decoder Context.
 *
 * Encapsulates the active audio port state, ring-buffer configuration, and synchronization
 * event queue between the PPU audio feeder thread and GameOS audio mixer hardware.
 */
typedef struct decoder {
  audio_decoder_t ad;
  u32 port_num;
  sys_event_queue_t snd_queue;
  audioPortConfig config;
  u64 snd_queue_key;
  int channels;
  int write_ptr;
  int audio_blocks;
} decoder_t;

#define INVALID_PORT 0xffffffff



/**
 *
 */
int64_t
arch_get_avtime(void)
{
  return arch_get_ts();
}


/**
 *
 */
static void
ps3_audio_destroy_port(decoder_t *d)
{
  if(d->port_num != INVALID_PORT) {
    audioPortStop(d->port_num);
    audioRemoveNotifyEventQueue(d->snd_queue_key);
    audioPortClose(d->port_num);
    /* Destroy the audio event queue utilizing modern PSL1GHT v2 sysEventQueueDestroy */
    sysEventQueueDestroy(d->snd_queue, 0);
    d->port_num = INVALID_PORT;
  }

}


/**
 *
 */
static int
ps3_audio_init(audio_decoder_t *ad)
{
  decoder_t *d = (decoder_t *)ad;
  d->audio_blocks = 16;
  d->port_num = INVALID_PORT;
  ad->ad_tile_size = AUDIO_BLOCK_SAMPLES;
  return 0;
}


/**
 *
 */
static void
ps3_audio_fini(audio_decoder_t *ad)
{
  decoder_t *d = (decoder_t *)ad;
  ps3_audio_destroy_port(d);
}


/**
 *
 */
/**
 * @brief Reconfigures the active PS3 audio output port according to input channel layout.
 *
 * Tears down any previously open port and allocates a fresh PSL1GHT v2 audio port
 * configured for 48000 Hz 32-bit floating point PCM with 2 or 8 channels.
 *
 * @param ad Pointer to base audio decoder instance.
 * @return int Returns 0 on success, or non-zero error code.
 *
 * @complexity Time: O(1) device open/configure latency. Space: O(1).
 */
static int
ps3_audio_reconfig(audio_decoder_t *ad)
{
  decoder_t *d = (decoder_t *)ad;

  /* Destroy existing active port before reallocating */
  ps3_audio_destroy_port(d);

  /* The Cell audio service fixed hardware rate and format: 48 kHz Float32 */
  ad->ad_out_sample_rate = 48000;
  ad->ad_out_sample_format = AV_SAMPLE_FMT_FLT;

  /* Determine channel layout: 2 channels for stereo/mono, 8 channels for surround */
  if(ad->ad_in_channel_layout == AV_CH_LAYOUT_MONO ||
     ad->ad_in_channel_layout == AV_CH_LAYOUT_STEREO) {
    ad->ad_out_channel_layout = AV_CH_LAYOUT_STEREO;
    d->channels = 2;
  } else {
    d->channels = 8;
    ad->ad_out_channel_layout = AV_CH_LAYOUT_7POINT1;
  }

  /* Configure PSL1GHT v2 audio port parameters */
  audioPortParam params;
  memset(&params, 0, sizeof(params));
  params.numChannels = d->channels;
  params.numBlocks = d->audio_blocks;
  params.attrib = 0;
  params.level = 1.0f;

  /*
   * Open hardware audio port via PSL1GHT v2 libaudio.
   * If requesting an 8-channel surround port fails (e.g. on stereo-only HDMI/AV
   * hardware configurations or stereo emulator backends), gracefully fall back
   * to 2-channel stereo. The upstream libavresample instance will automatically
   * perform multi-channel downmixing (5.1/7.1 to stereo) using AltiVec vector SIMD.
   */
  int r = audioPortOpen(&params, &d->port_num);
  if(r != 0 && d->channels == 8) {
    TRACE(TRACE_INFO, "AUDIO",
          "Failed to open 8-channel PS3 audio port (0x%x), attempting fallback to stereo (2ch)", r);
    d->channels = 2;
    ad->ad_out_channel_layout = AV_CH_LAYOUT_STEREO;
    params.numChannels = 2;
    r = audioPortOpen(&params, &d->port_num);
  }

  if(r != 0) {
    TRACE(TRACE_ERROR, "AUDIO", "Failed to open PS3 audio port: 0x%x", r);
    d->port_num = INVALID_PORT;
    return -1;
  }

  TRACE(TRACE_DEBUG, "AUDIO",
        "PS3 audio port %d opened (%d channels, %d blocks)",
        d->port_num, d->channels, d->audio_blocks);

  /* Query mapped ring-buffer layout and register notify event queue */
  audioGetPortConfig(d->port_num, &d->config);
  audioCreateNotifyEventQueue(&d->snd_queue, &d->snd_queue_key);
  audioSetNotifyEventQueue(d->snd_queue_key);
  sysEventQueueDrain(d->snd_queue);
  audioPortStart(d->port_num);

  return 0;
}


/**
 *
 */
static int
ps3_audio_deliver(audio_decoder_t *ad, int samples, int64_t pts, int epoch)
{
  int i;
  decoder_t *d = (decoder_t *)ad;

  assert(samples >= AUDIO_BLOCK_SAMPLES);

  sys_event_t event;
  int ret = sysEventQueueReceive(d->snd_queue, &event, 20 * 1000);
  if(ret)
    TRACE(TRACE_ERROR, "PS3AUDIO", "Audio queue timeout");

  float *buf = (float *)(intptr_t)d->config.audioDataStart;
  int current_block = *(uint64_t *)(intptr_t)d->config.readIndex;

  int bi;

  if(d->write_ptr != -1)
    bi = d->write_ptr;
  else
    bi = (current_block + 1) & 7;

  while(bi != current_block &&
	avresample_available(ad->ad_avr) >= AUDIO_BLOCK_SAMPLES) {

    float *dst = buf + d->channels * AUDIO_BLOCK_SAMPLES * bi;
    uint8_t *planes[8] = {0};

    float s = audio_master_mute ? 0 : audio_master_volume * ad->ad_vol_scale;

    vector float m = vec_splats(s);
    vector float z = vec_splats(0.0f);

    switch(ad->ad_out_channel_layout) {
    case AV_CH_LAYOUT_STEREO:
      planes[0] = (uint8_t *)dst;
      avresample_read(ad->ad_avr, planes, AUDIO_BLOCK_SAMPLES);

      for(i = 0; i < AUDIO_BLOCK_SAMPLES / 2; i++) {
	vec_st(vec_madd(vec_ld(0, dst), m, z), 0, dst);
	dst += 4;
      }
      break;

    case AV_CH_LAYOUT_7POINT1:
      planes[0] = (uint8_t *)dst;
      avresample_read(ad->ad_avr, planes, AUDIO_BLOCK_SAMPLES);

      // Swap Side-channels with Rear-channels as the channel
      // order differs between PS3 and libav

      for(i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {

	vector float v1 = vec_ld(0,  dst);
	vector float v2 = vec_ld(16, dst);
	
	v2 = vec_perm(v2, v2, (const vector unsigned char) {
	    0x8,0x9,0xa,0xb,
	      0xc,0xd,0xe,0xf,
	      0x0,0x1,0x2,0x3,
	      0x4,0x5,0x6,0x7});
			  
	v1 = vec_madd(v1, m, z);
	v2 = vec_madd(v2, m, z);

	vec_st(v1, 0, dst);
	vec_st(v2, 16, dst);
	dst += 8;
      }
      break;

    default:
      break;
    }

    bi = (bi + 1) & (d->audio_blocks - 1);

    if(pts != AV_NOPTS_VALUE) {

      pts -= 1000000LL * (AUDIO_BLOCK_SAMPLES * (d->audio_blocks - 1)) / 48000;

      media_pipe_t *mp = ad->ad_mp;

      hts_mutex_lock(&mp->mp_clock_mutex);
      mp->mp_audio_clock = pts;
      mp->mp_audio_clock_avtime = arch_get_avtime();
      mp->mp_audio_clock_epoch = epoch;
      hts_mutex_unlock(&mp->mp_clock_mutex);
      pts = AV_NOPTS_VALUE;
    }
  }
  d->write_ptr = bi;
  return 0;
}


/**
 *
 */
static audio_class_t ps3_audio_class = {
  .ac_alloc_size = sizeof(decoder_t),
  .ac_init = ps3_audio_init,
  .ac_fini = ps3_audio_fini,
  .ac_reconfig = ps3_audio_reconfig,
  .ac_deliver_unlocked = ps3_audio_deliver,
};


/**
 * @brief Global PlayStation 3 audio driver subsystem initialization.
 *
 * Initializes the PSL1GHT v2 audio service and returns the driver class descriptor.
 *
 * @param asettings Audio driver properties structure.
 * @return audio_class_t* Pointer to the PS3 audio decoder class.
 *
 * @complexity Time: O(1). Space: O(1).
 */
audio_class_t *
audio_driver_init(struct prop *asettings)
{
  /* Initialize PSL1GHT audio subsystem */
  audioInit();

  return &ps3_audio_class;
}

