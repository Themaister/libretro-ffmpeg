#include "libretro.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

// NOTE: THIS IS A BIG BAD HACK.

#define LOG_ERR_GOTO(msg, label) do { \
   fprintf(stderr, "[FFmpeg]: " msg "\n"); goto label; \
} while(0)

static AVFormatContext *fctx;
static AVCodecContext *vctx;
static AVCodecContext *actx;
static SwrContext *swr;
static AVFrame *aud_frame;
static AVFrame *vid_frame;
static struct SwsContext *sws_ctx;
static int video_stream;
static int audio_stream;

static double temporal_ratio;
static double temporal_index;
static unsigned conv_frame_primary;

static unsigned conv_frame_decoded;
static unsigned conv_frame_used;

#define FRAME_BUFFER 8
static AVFrame *conv_frame[FRAME_BUFFER];
static void *conv_frame_buf[FRAME_BUFFER];
#define PREV_FRAME(index) (((index) - 1) & (FRAME_BUFFER - 1))
#define NEXT_FRAME(index) (((index) + 1) & (FRAME_BUFFER - 1))

static uint32_t *blended_buf;
static unsigned frame_cnt;

static int16_t *audio_buffer;
static size_t audio_buffer_frames;
static size_t audio_buffer_frames_cap;
static size_t audio_write_cnt;

static struct
{
   unsigned width;
   unsigned height;

   double fps;
   double interpolate_fps;
   unsigned sample_rate;

   float aspect;
} media;

void retro_init(void)
{
   av_register_all();
}

void retro_deinit(void)
{}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "FFmpeg";
   info->library_version  = "v0";
   info->need_fullpath    = true;
   info->valid_extensions = "mkv|avi|mp4|mp3|flac|ogg|m4a"; // Anything is fine, we don't care.
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->timing = (struct retro_system_timing) {
      .fps = media.interpolate_fps,
      .sample_rate = actx ? media.sample_rate : 32000.0,
   };

   unsigned width  = vctx ? media.width : 320;
   unsigned height = vctx ? media.height : 240;
   float aspect = vctx ? media.aspect : 0.0;

   info->geometry = (struct retro_game_geometry) {
      .base_width   = width,
      .base_height  = height,
      .max_width    = width,
      .max_height   = height,
      .aspect_ratio = aspect,
   };
}

static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_reset(void)
{}

static void decode_video(AVPacket *pkt)
{
   unsigned retry_cnt = 0;
   int got_ptr = 0;

   while (!got_ptr)
   {
      if (avcodec_decode_video2(vctx, vid_frame, &got_ptr, pkt) <= 0)
      {
         if (retry_cnt++ < 4)
            continue;

         return;
      }
   }

   conv_frame_decoded++;
   unsigned conv_frame_decode_target = conv_frame_decoded & (FRAME_BUFFER - 1);

   sws_scale(sws_ctx, (const uint8_t * const*)vid_frame->data, vid_frame->linesize, 0, media.height,
         conv_frame[conv_frame_decode_target]->data, conv_frame[conv_frame_decode_target]->linesize);
}

static void decode_audio(AVPacket *pkt)
{
   unsigned retry_cnt = 0;
   int got_ptr = 0;

   while (!got_ptr)
   {
      if (avcodec_decode_audio4(actx, aud_frame, &got_ptr, pkt) < 0)
      {
         if (retry_cnt++ < 4)
            continue;

         return;
      }
   }

   size_t required_frames = audio_buffer_frames + aud_frame->nb_samples;
   if (required_frames > audio_buffer_frames_cap)
   {
      while (required_frames > audio_buffer_frames_cap)
         audio_buffer_frames_cap = (audio_buffer_frames_cap + 1) * 2;

      audio_buffer = av_realloc(audio_buffer, audio_buffer_frames_cap * sizeof(int16_t) * 2);
   }

   swr_convert(swr, (uint8_t*[]) { (uint8_t*)audio_buffer + audio_buffer_frames * sizeof(int16_t) * 2 },
         aud_frame->nb_samples,
         (const uint8_t**)aud_frame->data,
         aud_frame->nb_samples);

   audio_buffer_frames += aud_frame->nb_samples;
}

#if defined(__SSE2__) && 1
#include <emmintrin.h>
static void blend_temporal(uint32_t *buffer,
      const AVFrame *primary, const AVFrame *secondary, double index,
      unsigned width, unsigned height)
{
   const uint32_t *src_p = (const uint32_t*)primary->data[0];
   const uint32_t *src_s = (const uint32_t*)secondary->data[0];
   uint32_t *dst = buffer;

   __m128i mul_factor_p = _mm_set1_epi16((int16_t)(index * 128.0));
   __m128i mul_factor_s = _mm_set1_epi16((int16_t)((1.0 - index) * 128.0));
   __m128i zero = _mm_setzero_si128();

   for (unsigned y = 0; y < height; y++,
         dst += width, src_p += (primary->linesize[0] >> 2), src_s += (secondary->linesize[0] >> 2))
   {
      for (unsigned x = 0; x < width; x += 4)
      {
         __m128i p_src = _mm_load_si128((const __m128i*)(src_p + x));
         __m128i s_src = _mm_load_si128((const __m128i*)(src_s + x));
         __m128i p_lo = _mm_unpacklo_epi8(p_src, zero);
         __m128i p_hi = _mm_unpackhi_epi8(p_src, zero);
         __m128i s_lo = _mm_unpacklo_epi8(s_src, zero);
         __m128i s_hi = _mm_unpackhi_epi8(s_src, zero);

         p_lo = _mm_mullo_epi16(p_lo, mul_factor_p);
         p_hi = _mm_mullo_epi16(p_hi, mul_factor_p);
         s_lo = _mm_mullo_epi16(s_lo, mul_factor_s);
         s_hi = _mm_mullo_epi16(s_hi, mul_factor_s);

         __m128i res_lo = _mm_srli_epi16(_mm_adds_epi16(p_lo, s_lo), 7);
         __m128i res_hi = _mm_srli_epi16(_mm_adds_epi16(p_hi, s_hi), 7);
         _mm_store_si128((__m128i*)(dst + x), _mm_packus_epi16(res_lo, res_hi));
      }
   }
}
#else
static void blend_temporal(uint32_t *buffer,
      const AVFrame *primary, const AVFrame *secondary, double index,
      unsigned width, unsigned height)
{
   const uint32_t *src_p = (const uint32_t*)primary->data[0];
   const uint32_t *src_s = (const uint32_t*)secondary->data[0];
   uint32_t *dst = buffer;

   uint32_t mul_factor_p = index * 256;
   uint32_t mul_factor_s = 256 - mul_factor_p;

   for (unsigned y = 0; y < height; y++,
         dst += width, src_p += (primary->linesize[0] >> 2), src_s += (secondary->linesize[0] >> 2))
   {
      for (unsigned x = 0; x < width; x++)
      {
         uint32_t src_p_col = src_p[x];
         uint32_t src_s_col = src_s[x];
         uint32_t src_p_r = (src_p_col & 0xff0000) * mul_factor_p;
         uint32_t src_p_g = (src_p_col & 0x00ff00) * mul_factor_p;
         uint32_t src_p_b = (src_p_col & 0x0000ff) * mul_factor_p;
         uint32_t src_s_r = (src_s_col & 0xff0000) * mul_factor_s;
         uint32_t src_s_g = (src_s_col & 0x00ff00) * mul_factor_s;
         uint32_t src_s_b = (src_s_col & 0x0000ff) * mul_factor_s;
         uint32_t res_r = (src_p_r + src_s_r + 0x80) >> 8;
         uint32_t res_g = (src_p_g + src_s_g + 0x80) >> 8;
         uint32_t res_b = (src_p_b + src_s_b + 0x80) >> 8;
         dst[x] = (res_r & 0xff0000) | (res_g & 0x00ff00) | (res_b & 0x0000ff);
      }
   }
}
#endif

void retro_run(void)
{
   input_poll_cb();

   size_t expected_audio_frames = frame_cnt * media.sample_rate / media.interpolate_fps;
   size_t should_play_frames = expected_audio_frames - audio_write_cnt;

   // Check if we have already decoded images.
   if (vctx)
   {
      while (temporal_index >= 1.0 && conv_frame_used < conv_frame_decoded)
      {
         conv_frame_primary = NEXT_FRAME(conv_frame_primary);
         temporal_index -= 1.0;
         conv_frame_used++;
      }
   }

   // We need a fresh frame before continuing.
   while (temporal_index >= 1.0 || (actx && audio_buffer_frames < should_play_frames))
   {
      AVPacket pkt;
      if (av_read_frame(fctx, &pkt) < 0)
      {
         // Movie is probably done playing.
         environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
         return;
      }

      if (pkt.stream_index == video_stream)
      {
         decode_video(&pkt);
         av_free_packet(&pkt);

         if (temporal_index >= 1.0)
         {
            temporal_index -= 1.0;
            conv_frame_primary = NEXT_FRAME(conv_frame_primary);
            conv_frame_used++;
         }
      }
      else if (pkt.stream_index == audio_stream)
      {
         decode_audio(&pkt);
         av_free_packet(&pkt);
      }
      else
         av_free_packet(&pkt);
   }

   if (vctx)
   {
      blend_temporal(blended_buf,
            conv_frame[conv_frame_primary], conv_frame[PREV_FRAME(conv_frame_primary)],
            temporal_index, media.width, media.height);

      temporal_index += temporal_ratio;
      video_cb(blended_buf, media.width, media.height, media.width * sizeof(uint32_t));
      frame_cnt++;
   }

   if (actx)
   {
      if (should_play_frames > audio_buffer_frames)
      {
         fprintf(stderr, "[FFmpeg]: Audio underrun! Expected to play %zu frames, can only play %zu frames.\n",
               should_play_frames, audio_buffer_frames);
         should_play_frames = audio_buffer_frames;
      }

      size_t written_frames = 0;
      while (written_frames < should_play_frames)
         written_frames += audio_batch_cb(audio_buffer + written_frames * 2,
               should_play_frames - written_frames);

      // Ye, we should use ring buffers, but whatever ;)
      memmove(audio_buffer, audio_buffer + should_play_frames * 2,
            (audio_buffer_frames - should_play_frames) * sizeof(int16_t) * 2);
      audio_buffer_frames -= should_play_frames;
      audio_write_cnt += should_play_frames;
   }
}

static bool open_codec(AVCodecContext **ctx, unsigned index)
{
   AVCodec *codec = avcodec_find_decoder(fctx->streams[index]->codec->codec_id);
   if (!codec)
      return false;

   *ctx = fctx->streams[index]->codec;
   if (avcodec_open2(*ctx, codec, NULL) < 0)
      return false;

   return true;
}

static bool open_codecs(void)
{
   video_stream = -1;
   audio_stream = -1;

   for (unsigned i = 0; i < fctx->nb_streams; i++)
   {
      switch (fctx->streams[i]->codec->codec_type)
      {
         case AVMEDIA_TYPE_AUDIO:
            if (!actx)
            {
               if (!open_codec(&actx, i))
                  return false;
               audio_stream = i;
            }
            break;

         case AVMEDIA_TYPE_VIDEO:
            if (!vctx)
            {
               if (!open_codec(&vctx, i))
                  return false;
               video_stream = i;
            }
            break;

         default:
            break;
      }
   }

   return actx || vctx;
}

static bool init_media_info(void)
{
   if (actx)
   {
      media.sample_rate = actx->sample_rate;
      aud_frame = avcodec_alloc_frame();
      swr = swr_alloc();
      av_opt_set_int(swr, "in_channel_layout", actx->channel_layout, 0);
      av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
      av_opt_set_int(swr, "in_sample_rate", media.sample_rate, 0);
      av_opt_set_int(swr, "out_sample_rate", media.sample_rate, 0);
      av_opt_set_int(swr, "in_sample_fmt", actx->sample_fmt, 0);
      av_opt_set_int(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
      swr_init(swr);
   }

   if (vctx)
   {
      media.fps    = 1.0 / (vctx->ticks_per_frame * av_q2d(vctx->time_base));
      fprintf(stderr, "FPS: %.3f\n", media.fps);
      media.interpolate_fps =  60.0;
      media.width  = vctx->width;
      media.height = vctx->height;
      media.aspect = (float)vctx->width * av_q2d(vctx->sample_aspect_ratio) / vctx->height;

      vid_frame = avcodec_alloc_frame();

      sws_ctx = sws_getCachedContext(sws_ctx,
            media.width, media.height, vctx->pix_fmt,
            media.width, media.height, PIX_FMT_RGB32,
            SWS_POINT, NULL, NULL, NULL);

      size_t size = avpicture_get_size(PIX_FMT_RGB32, media.width, media.height);

      for (unsigned i = 0; i < FRAME_BUFFER; i++)
      {
         conv_frame_buf[i] = av_malloc(size);
         if (!conv_frame_buf[i])
            LOG_ERR_GOTO("Failed to allocate frame.", error);

         conv_frame[i] = avcodec_alloc_frame();
         avpicture_fill((AVPicture*)conv_frame[i], conv_frame_buf[i],
               PIX_FMT_RGB32, media.width, media.height);
      }

      blended_buf = av_malloc(media.width * media.height * sizeof(uint32_t));
      temporal_ratio = media.fps / media.interpolate_fps;
   }

   return true;

error:
   return false;
}

bool retro_load_game(const struct retro_game_info *info)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      LOG_ERR_GOTO("Cannot set pixel format.", error);

   if (avformat_open_input(&fctx, info->path, NULL, NULL) < 0)
      LOG_ERR_GOTO("Failed to open input.", error);

   if (avformat_find_stream_info(fctx, NULL) < 0)
      LOG_ERR_GOTO("Failed to find stream info.", error);

   if (!open_codecs())
      LOG_ERR_GOTO("Failed to find codec.", error);

   if (!init_media_info())
      LOG_ERR_GOTO("Failed to init media info.", error);

   return true;

error:
   retro_unload_game();
   return false;
}

void retro_unload_game(void)
{
   if (actx)
   {
      avcodec_close(actx);
      actx = NULL;
   }

   if (vctx)
   {
      avcodec_close(vctx);
      vctx = NULL;
   }

   if (aud_frame)
      av_freep(&aud_frame);
   if (vid_frame)
      av_freep(&vid_frame);

   if (fctx)
   {
      avformat_close_input(&fctx);
      fctx = NULL;
   }

   for (unsigned i = 0; i < FRAME_BUFFER; i++)
   {
      if (conv_frame[i])
         av_freep(&conv_frame[i]);
      if (conv_frame_buf[i])
         av_freep(&conv_frame_buf[i]);
   }

   if (blended_buf)
      av_freep(&blended_buf);

   if (audio_buffer)
      av_freep(&audio_buffer);
   audio_buffer_frames = 0;
   audio_buffer_frames_cap = 0;
   audio_write_cnt = 0;

   if (sws_ctx)
   {
      sws_freeContext(sws_ctx);
      sws_ctx = NULL;
   }

   if (swr)
      swr_free(&swr);

   temporal_index = 0.0;
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

bool retro_unserialize(const void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

