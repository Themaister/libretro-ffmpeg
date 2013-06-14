#include "libretro.h"
#include "thread.h"
#include "fifo_buffer.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavdevice/avdevice.h>
#include <libswresample/swresample.h>
#include <ass/ass.h>

#ifdef HAVE_GL
#include "glsym.h"
#endif

#define LOG_ERR_GOTO(msg, label) do { \
   fprintf(stderr, "[FFmpeg]: " msg "\n"); goto label; \
} while(0)

// FFmpeg context data.
static AVFormatContext *fctx;
static AVCodecContext *vctx;
static AVCodecContext *actx;
static AVCodecContext *sctx;
static int video_stream;
static int audio_stream;
static int subtitle_stream;

// AAS/SSA subtitles.
static ASS_Library *ass;
static ASS_Renderer *ass_render;
static ASS_Track *ass_track;
static uint8_t *ass_extra_data;
static size_t ass_extra_data_size;
struct attachment
{
   uint8_t *data;
   size_t size;
};
static struct attachment *attachments;
static size_t attachments_size;

// A/V timing.
static uint64_t frame_cnt;
static uint64_t audio_frames;
static double pts_bias;

// Threaded FIFOs.
static volatile bool decode_thread_dead;
static fifo_buffer_t *video_decode_fifo;
static fifo_buffer_t *audio_decode_fifo;
static scond_t *fifo_cond;
static scond_t *fifo_decode_cond;
static slock_t *fifo_lock;
static sthread_t *decode_thread_handle;
static double decode_last_video_time;
static double decode_last_audio_time;

static uint32_t *video_frame_temp_buffer;

static bool main_sleeping;
static bool temporal_interpolation;

// Seeking.
static bool do_seek;
static double seek_time;

// GL stuff
#if defined(HAVE_GL)
static struct retro_hw_render_callback hw_render;
#endif

struct frame
{
#if defined(HAVE_GL)
   GLuint tex;
#if !defined(GLES)
   GLuint pbo;
#endif
#endif
   double pts;
};

static struct frame frames[2];

#ifdef HAVE_GL
static GLuint prog;
static GLuint vbo;
static GLint vertex_loc;
static GLint tex_loc;
static GLint mix_loc;
#endif

////

static struct
{
   unsigned width;
   unsigned height;

   double fps;
   double interpolate_fps;
   unsigned sample_rate;

   float aspect;
} media;

static void ass_msg_cb(int level, const char *fmt, va_list args, void *data)
{
   (void)data;
   if (level < 6)
      vfprintf(stderr, fmt, args);
}

static void append_attachment(const uint8_t *data, size_t size)
{
   attachments = av_realloc(attachments, (attachments_size + 1) * sizeof(*attachments));

   attachments[attachments_size].data = av_malloc(size);
   attachments[attachments_size].size = size;
   memcpy(attachments[attachments_size].data, data, size);

   attachments_size++;
}

void retro_init(void)
{
   av_register_all();
   avdevice_register_all();
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
   info->valid_extensions = "mkv|avi|f4v|f4f|3gp|ogm|flv|mp4|mp3|flac|ogg|m4a";
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

#ifdef HAVE_GL
   static const struct retro_variable vars[] = {
      { "ffmpeg_temporal_interp", "Temporal Interpolation; enabled|disabled" },
      { NULL, NULL },
   };

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
#endif
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

static void check_variables(void)
{
#ifdef HAVE_GL
   struct retro_variable var = {
      .key = "ffmpeg_temporal_interp"
   };

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         temporal_interpolation = true;
      else if (!strcmp(var.value, "disabled"))
         temporal_interpolation = false;
   }
#endif
}

void retro_run(void)
{
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables();

   input_poll_cb();

   int seek_frames = 0;
   static bool last_left;
   static bool last_right;
   static bool last_up;
   static bool last_down;
   bool left = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
         RETRO_DEVICE_ID_JOYPAD_LEFT);
   bool right = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
         RETRO_DEVICE_ID_JOYPAD_RIGHT);
   bool up = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
         RETRO_DEVICE_ID_JOYPAD_UP);
   bool down = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
         RETRO_DEVICE_ID_JOYPAD_DOWN);

   if (left && !last_left)
      seek_frames -= 10 * media.interpolate_fps;
   if (right && !last_right)
      seek_frames += 10 * media.interpolate_fps;
   if (up && !last_up)
      seek_frames += 60 * media.interpolate_fps;
   if (down && !last_down)
      seek_frames -= 60 * media.interpolate_fps;

   last_left = left;
   last_right = right;
   last_up = up;
   last_down = down;

   // Push seek request to thread,
   // wait for seek to complete.
   if (seek_frames)
   {
      if (seek_frames < 0 && (unsigned)-seek_frames > frame_cnt)
         frame_cnt = 0;
      else
         frame_cnt += seek_frames;

      slock_lock(fifo_lock);

      do_seek = true;
      seek_time = frame_cnt / media.interpolate_fps;

      if (seek_frames < 0)
      {
         fprintf(stderr, "Resetting PTS.\n");
         frames[0].pts = 0.0;
         frames[1].pts = 0.0;
      }
      audio_frames = frame_cnt * media.sample_rate / media.interpolate_fps;

      if (video_decode_fifo)
         fifo_clear(video_decode_fifo);
      if (audio_decode_fifo)
         fifo_clear(audio_decode_fifo);
      scond_signal(fifo_decode_cond);

      while (!decode_thread_dead && do_seek)
         scond_wait(fifo_cond, fifo_lock);
      slock_unlock(fifo_lock);
   }

   if (decode_thread_dead)
   {
      environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
      return;
   }

   frame_cnt++;

   int16_t audio_buffer[2048];
   size_t to_read_frames = 0;

   // Have to decode audio before video incase there are PTS fuckups due
   // to seeking.
   if (audio_stream >= 0)
   {
      // Audio
      uint64_t expected_audio_frames = frame_cnt * media.sample_rate / media.interpolate_fps;
      to_read_frames = expected_audio_frames - audio_frames;
      size_t to_read_bytes = to_read_frames * sizeof(int16_t) * 2;

      slock_lock(fifo_lock);
      while (!decode_thread_dead && fifo_read_avail(audio_decode_fifo) < to_read_bytes)
      {
         //fprintf(stderr, "Main: Audio fifo is empty.\n");
         main_sleeping = true;
         scond_signal(fifo_decode_cond);
         scond_wait(fifo_cond, fifo_lock);
         main_sleeping = false;
      }

      double reading_pts = decode_last_audio_time -
         (double)fifo_read_avail(audio_decode_fifo) / (media.sample_rate * sizeof(int16_t) * 2);

      double expected_pts = (double)audio_frames / media.sample_rate;

      double old_pts_bias = pts_bias;
      pts_bias = reading_pts - expected_pts;
      if (pts_bias < old_pts_bias - 1.0)
      {
         fprintf(stderr, "Resetting PTS (bias).\n");
         frames[0].pts = 0.0;
         frames[1].pts = 0.0;
         //fprintf(stderr, "Expect delay: %.2f s.\n", old_pts_bias - pts_bias);
      }

      if (!decode_thread_dead)
         fifo_read(audio_decode_fifo, audio_buffer, to_read_bytes);
      scond_signal(fifo_decode_cond);

      slock_unlock(fifo_lock);
      audio_frames += to_read_frames;
   }

   double min_pts = frame_cnt / media.interpolate_fps + pts_bias;
   if (video_stream >= 0)
   {
#ifndef HAVE_GL
      bool dupe = true;
#endif
      // Video
      if (min_pts > frames[1].pts)
      {
         struct frame tmp = frames[1];
         frames[1] = frames[0];
         frames[0] = tmp;
      }

      while (!decode_thread_dead && min_pts > frames[1].pts)
      {
         slock_lock(fifo_lock);
         size_t to_read_frame_bytes = media.width * media.height * sizeof(uint32_t) + sizeof(int64_t);
         while (!decode_thread_dead && fifo_read_avail(video_decode_fifo) < to_read_frame_bytes)
         {
            //fprintf(stderr, "Main: Video fifo is empty.\n");
            main_sleeping = true;
            scond_signal(fifo_decode_cond);
            scond_wait(fifo_cond, fifo_lock);
            main_sleeping = false;
         }

         //fprintf(stderr, "Main: Reading video frame.\n");

         int64_t pts = 0;
         if (!decode_thread_dead)
         {
            fifo_read(video_decode_fifo, &pts, sizeof(int64_t));
#if defined(HAVE_GL)
#if defined(GLES)
            fifo_read(video_decode_fifo, video_frame_temp_buffer, media.width * media.height * sizeof(uint32_t));
            glBindTexture(GL_TEXTURE_2D, frames[1].tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                  media.width, media.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, video_frame_temp_buffer);
            glBindTexture(GL_TEXTURE_2D, 0);
#else
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, frames[1].pbo);
            uint32_t *data = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER,
                  0, media.width * media.height, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);

            fifo_read(video_decode_fifo, data, media.width * media.height * sizeof(uint32_t));

            glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
            glBindTexture(GL_TEXTURE_2D, frames[1].tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                  media.width, media.height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
#endif
#else
            fifo_read(video_decode_fifo, video_frame_temp_buffer, media.width * media.height * sizeof(uint32_t));
            dupe = false;
#endif
         }

         scond_signal(fifo_decode_cond);
         slock_unlock(fifo_lock);

         frames[1].pts = av_q2d(fctx->streams[video_stream]->time_base) * pts;
      }

      //fprintf(stderr, "Main: Found suitable frame.\n");

      float mix_factor = (min_pts - frames[0].pts) / (frames[1].pts - frames[0].pts);
      if (!temporal_interpolation)
         mix_factor = 1.0f;

      //fprintf(stderr, "Mix factor: %f, diff: %f\n", mix_factor, frames[1].pts - frames[0].pts);

#ifdef HAVE_GL
      glBindFramebuffer(GL_FRAMEBUFFER, hw_render.get_current_framebuffer());
      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glViewport(0, 0, media.width, media.height);
      glUseProgram(prog);

      glUniform1f(mix_loc, mix_factor);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, frames[1].tex);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, frames[0].tex);


      glBindBuffer(GL_ARRAY_BUFFER, vbo);
      glVertexAttribPointer(vertex_loc, 2, GL_FLOAT, GL_FALSE,
            4 * sizeof(GLfloat), (const GLvoid*)(0 * sizeof(GLfloat)));
      glVertexAttribPointer(tex_loc, 2, GL_FLOAT, GL_FALSE,
            4 * sizeof(GLfloat), (const GLvoid*)(2 * sizeof(GLfloat)));
      glEnableVertexAttribArray(vertex_loc);
      glEnableVertexAttribArray(tex_loc);
      glBindBuffer(GL_ARRAY_BUFFER, 0);

      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      glDisableVertexAttribArray(vertex_loc);
      glDisableVertexAttribArray(tex_loc);

      glUseProgram(0);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, 0);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, 0);

      video_cb(RETRO_HW_FRAME_BUFFER_VALID, media.width, media.height, media.width * sizeof(uint32_t));
#else
      video_cb(dupe ? NULL : video_frame_temp_buffer, media.width, media.height, media.width * sizeof(uint32_t));
#endif
   }
   else
      video_cb(NULL, 1, 1, sizeof(uint32_t));

   if (to_read_frames)
      audio_batch_cb(audio_buffer, to_read_frames);
}

static bool open_codec(AVCodecContext **ctx, unsigned index)
{
   AVCodec *codec = avcodec_find_decoder(fctx->streams[index]->codec->codec_id);
   if (!codec)
   {
      fprintf(stderr, "Couldn't find suitable decoder, exiting...\n");
      return false;
   }

   *ctx = fctx->streams[index]->codec;
   if (avcodec_open2(*ctx, codec, NULL) < 0)
      return false;

   return true;
}

static bool open_codecs(void)
{
   video_stream = -1;
   audio_stream = -1;
   subtitle_stream = -1;

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

         case AVMEDIA_TYPE_SUBTITLE:
            if (!sctx && fctx->streams[i]->codec->codec_id == CODEC_ID_SSA)
            {
               if (!open_codec(&sctx, i))
                  return false;
               subtitle_stream = i;

               ass_extra_data_size = sctx->extradata ? sctx->extradata_size : 0;

               if (ass_extra_data_size)
               {
                  ass_extra_data = av_malloc(ass_extra_data_size);
                  memcpy(ass_extra_data, sctx->extradata,
                        ass_extra_data_size);
               }
            }
            break;

         case AVMEDIA_TYPE_ATTACHMENT:
         {
            AVCodecContext *ctx = fctx->streams[i]->codec;
            if (ctx->codec_id == CODEC_ID_TTF)
               append_attachment(ctx->extradata, ctx->extradata_size);
            break;
         }

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

   if (sctx)
   {
      ass = ass_library_init();
      ass_set_message_cb(ass, ass_msg_cb, NULL);
      for (size_t i = 0; i < attachments_size; i++)
         ass_add_font(ass, (char*)"", (char*)attachments[i].data, attachments[i].size);

      ass_render = ass_renderer_init(ass);
      ass_set_frame_size(ass_render, media.width, media.height);
      ass_set_extract_fonts(ass, true);
      ass_set_fonts(ass_render, NULL, NULL, 1, NULL, 1);
      ass_set_hinting(ass_render, ASS_HINTING_LIGHT);

      ass_track = ass_new_track(ass);
      ass_process_codec_private(ass_track, (char*)ass_extra_data,
            ass_extra_data_size);
   }

   return true;
}

static bool decode_video(AVPacket *pkt, AVFrame *frame, AVFrame *conv, struct SwsContext *sws)
{
   int got_ptr = 0;
   avcodec_get_frame_defaults(frame);
   int ret = avcodec_decode_video2(vctx, frame, &got_ptr, pkt);
   if (ret < 0)
      return false;

   if (got_ptr)
   {
      sws_scale(sws, (const uint8_t * const*)frame->data, frame->linesize, 0, media.height,
            conv->data, conv->linesize);
      return true;
   }
   else
      return false;
}

static int16_t *decode_audio(AVPacket *pkt, AVFrame *frame, int16_t *buffer, size_t *buffer_cap,
      SwrContext *swr)
{
   AVPacket pkt_tmp = *pkt;

   int got_ptr = 0;

   for (;;)
   {
      int ret = 0;
      avcodec_get_frame_defaults(frame);
      ret = avcodec_decode_audio4(actx, frame, &got_ptr, &pkt_tmp);
      if (ret < 0)
         return buffer;

      pkt_tmp.data += ret;
      pkt_tmp.size -= ret;

      if (!got_ptr)
         break;

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

      int64_t pts = av_frame_get_best_effort_timestamp(frame);

      slock_lock(fifo_lock);
      while (!decode_thread_dead && fifo_write_avail(audio_decode_fifo) < required_buffer)
      {
         //fprintf(stderr, "Thread: Audio fifo is full ...\n");
         if (!main_sleeping)
            scond_wait(fifo_decode_cond, fifo_lock);
         else
         {
            fprintf(stderr, "Thread: Audio deadlock detected ...\n");
            fifo_clear(audio_decode_fifo);
            break;
         }
      }

      decode_last_audio_time = pts * av_q2d(fctx->streams[audio_stream]->time_base);
      if (!decode_thread_dead)
         fifo_write(audio_decode_fifo, buffer, required_buffer);

      scond_signal(fifo_cond);
      slock_unlock(fifo_lock);
   }

   return buffer;
}

static void decode_thread_seek(double time)
{
   int64_t seek_to = time * AV_TIME_BASE;
   if (seek_to < 0)
      seek_to = 0;

   decode_last_video_time = time;
   decode_last_audio_time = time;

   int ret = avformat_seek_file(fctx, -1, INT64_MIN, seek_to, INT64_MAX, 0);
   if (ret < 0)
      fprintf(stderr, "av_seek_frame() failed.\n");

   if (actx)
      avcodec_flush_buffers(actx);
   if (vctx)
      avcodec_flush_buffers(vctx);
   if (sctx)
      avcodec_flush_buffers(sctx);
   if (ass_track)
      ass_flush_events(ass_track);
}

// Straight CPU alpha blending.
// Should probably do in GL.
static void render_ass_img(AVFrame *conv_frame, ASS_Image *img)
{
   uint32_t *frame = (uint32_t*)conv_frame->data[0];
   int stride = conv_frame->linesize[0] / sizeof(uint32_t);

   for (; img; img = img->next)
   {
      if (img->w == 0 && img->h == 0)
         continue;

      const uint8_t *bitmap = img->bitmap;
      uint32_t *dst = frame + img->dst_x + img->dst_y * stride;

      unsigned r = (img->color >> 24) & 0xff;
      unsigned g = (img->color >> 16) & 0xff;
      unsigned b = (img->color >>  8) & 0xff;
      unsigned a = 255 - (img->color & 0xff);

      for (int y = 0; y < img->h; y++,
            bitmap += img->stride, dst += stride)
      {
         for (int x = 0; x < img->w; x++)
         {
            unsigned src_alpha = ((bitmap[x] * (a + 1)) >> 8) + 1;
            unsigned dst_alpha = 256 - src_alpha;

            uint32_t dst_color = dst[x];
            unsigned dst_r = (dst_color >> 16) & 0xff;
            unsigned dst_g = (dst_color >>  8) & 0xff;
            unsigned dst_b = (dst_color >>  0) & 0xff;

            dst_r = (r * src_alpha + dst_r * dst_alpha) >> 8;
            dst_g = (g * src_alpha + dst_g * dst_alpha) >> 8;
            dst_b = (b * src_alpha + dst_b * dst_alpha) >> 8;

            dst[x] = (0xffu << 24) | (dst_r << 16) | (dst_g << 8) | (dst_b << 0);
         }
      }
   }
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
      slock_lock(fifo_lock);
      bool seek = do_seek;
      double seek_time_thread = seek_time;
      slock_unlock(fifo_lock);

      if (seek)
      {
         decode_thread_seek(seek_time_thread);

         slock_lock(fifo_lock);
         do_seek = false;
         seek_time = 0.0;

         if (video_decode_fifo)
            fifo_clear(video_decode_fifo);
         if (audio_decode_fifo)
            fifo_clear(audio_decode_fifo);

         scond_signal(fifo_cond);
         slock_unlock(fifo_lock);
      }

      AVPacket pkt;
      memset(&pkt, 0, sizeof(pkt));
      if (av_read_frame(fctx, &pkt) < 0)
         break;

      if (pkt.stream_index == video_stream)
      {
         if (decode_video(&pkt, vid_frame, conv_frame, sws))
         {
            int64_t pts = av_frame_get_best_effort_timestamp(vid_frame);
            //fprintf(stderr, "Got video frame PTS: %.2f.\n", decode_last_video_time);

            double video_time = pts * av_q2d(fctx->streams[video_stream]->time_base);
            if (ass_render)
            {
               int change = 0;
               ASS_Image *img = ass_render_frame(ass_render, ass_track,
                     1000 * video_time, &change);

               //fprintf(stderr, "Rendering ASS image: %p.\n", (void*)img);
               // Do it on CPU for now.
               // We're in a thread anyways, so shouldn't really matter.
               render_ass_img(conv_frame, img);
            }

            size_t decoded_size = frame_size + sizeof(pts);
            slock_lock(fifo_lock);
            while (!decode_thread_dead && fifo_write_avail(video_decode_fifo) < decoded_size)
            {
               //fprintf(stderr, "Thread: Video fifo is full ...\n");
               if (!main_sleeping)
                  scond_wait(fifo_decode_cond, fifo_lock);
               else
               {
                  //fprintf(stderr, "Thread: Video deadlock detected ...\n");
                  fifo_clear(video_decode_fifo);
                  break;
               }
            }

            decode_last_video_time = video_time;
            if (!decode_thread_dead)
            {
               fifo_write(video_decode_fifo, &pts, sizeof(pts));
               const uint8_t *src = conv_frame->data[0];
               int stride = conv_frame->linesize[0];
               for (unsigned y = 0; y < media.height; y++, src += stride)
                  fifo_write(video_decode_fifo, src, media.width * sizeof(uint32_t));

               //fprintf(stderr, "Wrote frame, frames: #%zu\n", fifo_read_avail(video_decode_fifo) /
               //      decoded_size);
            }
            scond_signal(fifo_cond);
            slock_unlock(fifo_lock);
         }
      }
      else if (pkt.stream_index == audio_stream)
      {
         audio_buffer = decode_audio(&pkt, aud_frame,
               audio_buffer, &audio_buffer_cap,
               swr);
      }
      else if (pkt.stream_index == subtitle_stream)
      {
         AVSubtitle sub;
         memset(&sub, 0, sizeof(sub));

         int finished = 0;
         while (!finished)
         {
            if (avcodec_decode_subtitle2(sctx, &sub, &finished, &pkt) < 0)
            {
               fprintf(stderr, "Decode subtitles failed.\n");
               break;
            }
         }

         for (int i = 0; i < sub.num_rects; i++)
         {
            if (sub.rects[i]->ass)
               ass_process_data(ass_track, sub.rects[i]->ass, strlen(sub.rects[i]->ass));
         }

         avsubtitle_free(&sub);
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

#ifdef HAVE_GL
static void context_reset(void)
{
   glsym_init_procs(hw_render.get_proc_address);

   prog = glCreateProgram();
   GLuint vert = glCreateShader(GL_VERTEX_SHADER);
   GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);

   static const char *vertex_source =
      "attribute vec2 aVertex;\n"
      "attribute vec2 aTexCoord;\n"
      "varying vec2 vTex;\n"
      "void main() { gl_Position = vec4(aVertex, 0.0, 1.0); vTex = aTexCoord; }\n";

   static const char *fragment_source =
      "#ifdef GL_ES\n"
      "precision mediump float;\n"
      "#endif\n"
      "varying vec2 vTex;\n"
      "uniform sampler2D sTex0;\n"
      "uniform sampler2D sTex1;\n"
      "uniform float uMix;\n"
#ifdef GLES
      "void main() { gl_FragColor = vec4(mix(texture2D(sTex0, vTex).bgr, texture2D(sTex1, vTex).bgr, uMix), 1.0); }\n";
      // Get format as GL_RGBA/GL_UNSIGNED_BYTE. Assume little endian, so we get ARGB -> BGRA byte order, and we have to swizzle to .BGR.
#else
      "void main() { gl_FragColor = vec4(mix(texture2D(sTex0, vTex).rgb, texture2D(sTex1, vTex).rgb, uMix), 1.0); }\n";
#endif

   glShaderSource(vert, 1, &vertex_source, NULL);
   glShaderSource(frag, 1, &fragment_source, NULL);
   glCompileShader(vert);
   glCompileShader(frag);
   glAttachShader(prog, vert);
   glAttachShader(prog, frag);
   glLinkProgram(prog);

   glUseProgram(prog);

   glUniform1i(glGetUniformLocation(prog, "sTex0"), 0);
   glUniform1i(glGetUniformLocation(prog, "sTex1"), 1);
   vertex_loc = glGetAttribLocation(prog, "aVertex");
   tex_loc = glGetAttribLocation(prog, "aTexCoord");
   mix_loc = glGetUniformLocation(prog, "uMix");

   glUseProgram(0);

   for (unsigned i = 0; i < 2; i++)
   {
      glGenTextures(1, &frames[i].tex);

      glBindTexture(GL_TEXTURE_2D, frames[i].tex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

#ifndef GLES
      glGenBuffers(1, &frames[i].pbo);
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, frames[i].pbo);
      glBufferData(GL_PIXEL_UNPACK_BUFFER, media.width * media.height * sizeof(uint32_t), NULL, GL_STREAM_DRAW);
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
#endif
   }

   static const GLfloat vertex_data[] = {
      -1, -1, 0, 0,
       1, -1, 1, 0,
      -1,  1, 0, 1,
       1,  1, 1, 1,
   };

   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), vertex_data, GL_STATIC_DRAW);

   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glBindTexture(GL_TEXTURE_2D, 0);
}
#endif

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
   {
      video_decode_fifo = fifo_new(media.width * media.height * sizeof(uint32_t) * 32);

#ifdef HAVE_GL
      hw_render.context_reset = context_reset;
#ifdef GLES
      hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES2;
#else
      hw_render.context_type = RETRO_HW_CONTEXT_OPENGL;
#endif
      if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
         LOG_ERR_GOTO("Cannot initialize HW render.", error);
#endif
   }
   if (audio_stream >= 0)
      audio_decode_fifo = fifo_new(20 * media.sample_rate * sizeof(int16_t) * 2);

   fifo_cond = scond_new();
   fifo_decode_cond = scond_new();
   fifo_lock = slock_new();

   decode_thread_handle = sthread_create(decode_thread, NULL);

   video_frame_temp_buffer = av_malloc(media.width * media.height * sizeof(uint32_t));

   pts_bias = 0.0;
   check_variables();

   return true;

error:
   retro_unload_game();
   return false;
}

void retro_unload_game(void)
{
   if (decode_thread_handle)
   {
      slock_lock(fifo_lock);
      decode_thread_dead = true;
      scond_signal(fifo_decode_cond);
      slock_unlock(fifo_lock);
      sthread_join(decode_thread_handle);
   }
   decode_thread_handle = NULL;

   if (fifo_cond)
      scond_free(fifo_cond);
   if (fifo_decode_cond)
      scond_free(fifo_decode_cond);
   if (fifo_lock)
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

   decode_last_video_time = 0.0;
   decode_last_audio_time = 0.0;

   frames[0].pts = frames[1].pts = 0.0;
   pts_bias = 0.0;
   frame_cnt = 0;
   audio_frames = 0;

   if (sctx)
   {
      avcodec_close(sctx);
      sctx = NULL;
   }

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

   av_freep(&ass_extra_data);
   ass_extra_data_size = 0;

   for (size_t i = 0; i < attachments_size; i++)
      av_freep(&attachments[i].data);
   av_freep(&attachments);
   attachments_size = 0;

   if (ass_track)
      ass_free_track(ass_track);
   if (ass_render)
      ass_renderer_done(ass_render);
   if (ass)
      ass_library_done(ass);

   ass_track = NULL;
   ass_render = NULL;
   ass = NULL;

   av_freep(&video_frame_temp_buffer);
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

