#include "libretro.h"
#include "thread.h"
#include "fifo_buffer.h"

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
static int video_stream;
static int audio_stream;

static uint64_t frame_cnt;
static uint64_t audio_frames;

static volatile bool decode_thread_dead;
static fifo_buffer_t *video_decode_fifo;
static fifo_buffer_t *audio_decode_fifo;
static scond_t *fifo_cond;
static scond_t *fifo_decode_cond;
static slock_t *fifo_lock;
static sthread_t *decode_thread_handle;

static uint32_t *video_read_buffer;

static double packet_pts;
static double pts_bias;
static double last_audio_pts;

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

void retro_run(void)
{
   input_poll_cb();

   if (decode_thread_dead)
   {
      environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
      return;
   }

   frame_cnt++;

   double min_pts = frame_cnt / media.interpolate_fps + pts_bias;
   if (video_stream >= 0)
   {
      // Video
      bool dupe = true;
      while (!decode_thread_dead && min_pts >= packet_pts)
      {
         slock_lock(fifo_lock);
         size_t to_read_frame_bytes = media.width * media.height * sizeof(uint32_t) + sizeof(int64_t);
         while (!decode_thread_dead && fifo_read_avail(video_decode_fifo) < to_read_frame_bytes)
            scond_wait(fifo_cond, fifo_lock);

         int64_t pts = 0;
         if (!decode_thread_dead)
         {
            fifo_read(video_decode_fifo, &pts, sizeof(int64_t));
            fifo_read(video_decode_fifo, video_read_buffer, media.width * media.height * sizeof(uint32_t));
            dupe = false;
         }

         scond_signal(fifo_decode_cond);
         slock_unlock(fifo_lock);

         if (pts != AV_NOPTS_VALUE)
            packet_pts = av_q2d(fctx->streams[video_stream]->time_base) * pts;
         else
            packet_pts += 1.0 / media.fps;

         //fprintf(stderr, "PTS: %.2f s\n", packet_pts);
      }

      video_cb(dupe ? NULL : video_read_buffer, media.width, media.height, media.width * sizeof(uint32_t));
   }
   else
   {
      uint32_t black = 0;
      video_cb(&black, 1, 1, sizeof(black));
   }

   if (audio_stream >= 0)
   {
      // Audio
      uint64_t expected_audio_frames = frame_cnt * media.sample_rate / media.interpolate_fps;
      size_t to_read_frames = expected_audio_frames - audio_frames;
      size_t to_read_bytes = to_read_frames * sizeof(int16_t) * 2;

      slock_lock(fifo_lock);
      while (!decode_thread_dead && fifo_read_avail(audio_decode_fifo) < to_read_bytes)
         scond_wait(fifo_cond, fifo_lock);

      int16_t audio_buffer[2048];
      if (!decode_thread_dead)
         fifo_read(audio_decode_fifo, audio_buffer, to_read_bytes);
      scond_signal(fifo_decode_cond);

      slock_unlock(fifo_lock);

      if (!decode_thread_dead)
         audio_batch_cb(audio_buffer, to_read_frames);

      audio_frames += to_read_frames;

      pts_bias = +0.25; // Hack
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
      media.sample_rate = actx->sample_rate;

   media.interpolate_fps = 60.0;
   if (vctx)
   {
      media.fps = 1.0 / (vctx->ticks_per_frame * av_q2d(vctx->time_base));
      fprintf(stderr, "FPS: %.3f\n", media.fps);
      media.width  = vctx->width;
      media.height = vctx->height;
      media.aspect = (float)vctx->width * av_q2d(vctx->sample_aspect_ratio) / vctx->height;
   }

   return true;
}

static bool decode_video(AVPacket *pkt, AVFrame *frame, AVFrame *conv, struct SwsContext *sws)
{
   unsigned retry_cnt = 0;
   int got_ptr = 0;

   while (!got_ptr)
   {
      if (avcodec_decode_video2(vctx, frame, &got_ptr, pkt) <= 0)
      {
         if (retry_cnt++ < 4)
            continue;

         return false;
      }
   }

   sws_scale(sws, (const uint8_t * const*)frame->data, frame->linesize, 0, media.height,
         conv->data, conv->linesize);
   return true;
}

static int16_t *decode_audio(AVPacket *pkt, AVFrame *frame, int16_t *buffer, size_t *buffer_cap,
      size_t *written_bytes,
      SwrContext *swr)
{
   unsigned retry_cnt = 0;
   int got_ptr = 0;

   while (!got_ptr)
   {
      if (avcodec_decode_audio4(actx, frame, &got_ptr, pkt) < 0)
      {
         if (retry_cnt++ < 4)
            continue;

         return buffer;
      }
   }

   size_t required_buffer = frame->nb_samples * sizeof(int16_t) * 2;
   if (required_buffer > *buffer_cap)
   {
      buffer = av_realloc(buffer, required_buffer);
      *buffer_cap = required_buffer;
   }

   swr_convert(swr,
         (uint8_t*[]) { (uint8_t*)buffer },
         frame->nb_samples,
         (const uint8_t**)frame->data,
         frame->nb_samples);

   *written_bytes = required_buffer;

   return buffer;
}

static void decode_thread(void *data)
{
   (void)data;
   struct SwsContext *sws = NULL;
   
   if (video_stream >= 0)
   {
      sws = sws_getCachedContext(NULL,
            media.width, media.height, vctx->pix_fmt,
            media.width, media.height, PIX_FMT_RGB32,
            SWS_POINT, NULL, NULL, NULL);
   }

   SwrContext *swr = swr_alloc();

   if (audio_stream >= 0)
   {
      av_opt_set_int(swr, "in_channel_layout", actx->channel_layout, 0);
      av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
      av_opt_set_int(swr, "in_sample_rate", media.sample_rate, 0);
      av_opt_set_int(swr, "out_sample_rate", media.sample_rate, 0);
      av_opt_set_int(swr, "in_sample_fmt", actx->sample_fmt, 0);
      av_opt_set_int(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
      swr_init(swr);
   }

   AVFrame *aud_frame = avcodec_alloc_frame();
   AVFrame *vid_frame = avcodec_alloc_frame();

   AVFrame *conv_frame = NULL;
   void *conv_frame_buf = NULL;
   size_t frame_size = 0;

   if (video_stream >= 0)
   {
      frame_size = avpicture_get_size(PIX_FMT_RGB32, media.width, media.height);
      conv_frame = avcodec_alloc_frame();
      conv_frame_buf = av_malloc(frame_size);
      avpicture_fill((AVPicture*)conv_frame, conv_frame_buf,
            PIX_FMT_RGB32, media.width, media.height);
   }

   int16_t *audio_buffer = NULL;
   size_t audio_buffer_cap = 0;

   while (!decode_thread_dead)
   {
      AVPacket pkt;
      if (av_read_frame(fctx, &pkt) < 0)
         break;

      if (pkt.stream_index == video_stream)
      {
         if (decode_video(&pkt, vid_frame, conv_frame, sws))
         {
            int64_t pts = vid_frame->pts;

            size_t decoded_size = frame_size + sizeof(pts);
            slock_lock(fifo_lock);
            while (!decode_thread_dead && fifo_write_avail(video_decode_fifo) < decoded_size)
               scond_wait(fifo_decode_cond, fifo_lock);

            if (!decode_thread_dead)
            {
               fifo_write(video_decode_fifo, &pts, sizeof(pts));
               const uint8_t *src = conv_frame->data[0];
               int stride = conv_frame->linesize[0];
               for (unsigned y = 0; y < media.height; y++, src += stride)
                  fifo_write(video_decode_fifo, src, media.width * sizeof(uint32_t));
            }
            scond_signal(fifo_cond);
            slock_unlock(fifo_lock);
         }
      }
      else if (pkt.stream_index == audio_stream)
      {
         size_t decoded_size = 0;
         audio_buffer = decode_audio(&pkt, aud_frame,
               audio_buffer, &audio_buffer_cap,
               &decoded_size,
               swr);

         slock_lock(fifo_lock);
         while (!decode_thread_dead && fifo_write_avail(audio_decode_fifo) < decoded_size)
            scond_wait(fifo_decode_cond, fifo_lock);

         if (!decode_thread_dead)
            fifo_write(audio_decode_fifo, audio_buffer, decoded_size);

         last_audio_pts = aud_frame->pts;
         scond_signal(fifo_cond);
         slock_unlock(fifo_lock);
      }

      av_free_packet(&pkt);
   }

   if (sws)
      sws_freeContext(sws);
   swr_free(&swr);
   av_freep(&aud_frame);
   av_freep(&vid_frame);
   av_freep(&conv_frame);
   av_freep(&conv_frame_buf);
   av_freep(&audio_buffer);

   slock_lock(fifo_lock);
   decode_thread_dead = true;
   scond_signal(fifo_cond);
   slock_unlock(fifo_lock);
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

   decode_thread_dead = false;

   if (video_stream >= 0)
      video_decode_fifo = fifo_new(media.width * media.height * sizeof(uint32_t) * 32);
   if (audio_stream >= 0)
      audio_decode_fifo = fifo_new(media.sample_rate * 2 * sizeof(int16_t) * 2);

   fifo_cond = scond_new();
   fifo_decode_cond = scond_new();
   fifo_lock = slock_new();

   video_read_buffer = av_malloc(media.width * media.height * sizeof(uint32_t));

   decode_thread_handle = sthread_create(decode_thread, NULL);

   return true;

error:
   retro_unload_game();
   return false;
}

void retro_unload_game(void)
{
   slock_lock(fifo_lock);
   decode_thread_dead = true;
   scond_signal(fifo_decode_cond);
   slock_unlock(fifo_lock);
   sthread_join(decode_thread_handle);
   decode_thread_handle = NULL;

   scond_free(fifo_cond);
   scond_free(fifo_decode_cond);
   slock_free(fifo_lock);

   if (video_decode_fifo)
      fifo_free(video_decode_fifo);
   if (audio_decode_fifo)
      fifo_free(audio_decode_fifo);

   fifo_cond = NULL;
   fifo_decode_cond = NULL;
   fifo_lock = NULL;
   video_decode_fifo = NULL;
   audio_decode_fifo = NULL;

   packet_pts = 0.0;
   pts_bias = 0.0;
   last_audio_pts = 0.0;
   frame_cnt = 0;
   audio_frames = 0;

   av_freep(&video_read_buffer);

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

   if (fctx)
   {
      avformat_close_input(&fctx);
      fctx = NULL;
   }
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

