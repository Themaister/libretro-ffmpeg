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
#ifdef HAVE_SSA
#include <ass/ass.h>
#endif

#ifdef HAVE_GL
#include "glsym.h"
#endif

retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

#define LOG_ERR_GOTO(msg, label) do { \
   if (log_cb) \
      log_cb(RETRO_LOG_ERROR, "[FFmpeg]: " msg "\n"); \
   goto label; \
} while(0)

// FFmpeg context data.
static AVFormatContext *fctx;
static AVCodecContext *vctx;
static int video_stream;

static enum AVColorSpace colorspace;

#define MAX_STREAMS 8
static AVCodecContext *actx[MAX_STREAMS];
static AVCodecContext *sctx[MAX_STREAMS];
static int audio_streams[MAX_STREAMS];
static int audio_streams_num;
static int audio_streams_ptr;
static int subtitle_streams[MAX_STREAMS];
static int subtitle_streams_num;
static int subtitle_streams_ptr;

// AAS/SSA subtitles.
#ifdef HAVE_SSA
static ASS_Library *ass;
static ASS_Renderer *ass_render;
static ASS_Track *ass_track[MAX_STREAMS];
static uint8_t *ass_extra_data[MAX_STREAMS];
static size_t ass_extra_data_size[MAX_STREAMS];
#endif

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
static slock_t *decode_thread_lock;
static sthread_t *decode_thread_handle;
static double decode_last_video_time;
static double decode_last_audio_time;

static uint32_t *video_frame_temp_buffer;

static bool main_sleeping;

// Seeking.
static bool do_seek;
static double seek_time;

// GL stuff
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
static bool temporal_interpolation;
static struct retro_hw_render_callback hw_render;
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

   double interpolate_fps;
   unsigned sample_rate;

   float aspect;
} media;

#ifdef HAVE_SSA
static void ass_msg_cb(int level, const char *fmt, va_list args, void *data)
{
   (void)data;
   if (level < 6 && log_cb)
      log_cb(RETRO_LOG_INFO, fmt);
}
#endif

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
   //avdevice_register_all(); // FIXME: Occasionally crashes inside libavdevice for some odd reason on reentrancy. Likely a libavdevice bug.
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
   info->library_version  = "v1";
   info->need_fullpath    = true;
   info->valid_extensions = "mkv|avi|f4v|f4f|3gp|ogm|flv|mp4|mp3|flac|ogg|m4a";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->timing = (struct retro_system_timing) {
      .fps = media.interpolate_fps,
      .sample_rate = actx[0] ? media.sample_rate : 32000.0,
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

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   static const struct retro_variable vars[] = {
#ifdef HAVE_GL
      { "ffmpeg_temporal_interp", "Temporal Interpolation; enabled|disabled" },
#endif
      { "ffmpeg_color_space", "Colorspace; auto|BT.709|BT.601|FCC|SMPTE240M" },
      { NULL, NULL },
   };

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
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

   struct retro_variable color_var = {
      .key = "ffmpeg_color_space",
   };

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &color_var) && color_var.value)
   {
      slock_lock(decode_thread_lock);
      if (!strcmp(color_var.value, "BT.709"))
         colorspace = AVCOL_SPC_BT709;
      else if (!strcmp(color_var.value, "BT.601"))
         colorspace = AVCOL_SPC_BT470BG;
      else if (!strcmp(color_var.value, "FCC"))
         colorspace = AVCOL_SPC_FCC;
      else if (!strcmp(color_var.value, "SMPTE240M"))
         colorspace = AVCOL_SPC_SMPTE240M;
      else
         colorspace = AVCOL_SPC_UNSPECIFIED;
      slock_unlock(decode_thread_lock);
   }
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
   static bool last_l;
   static bool last_r;
   bool left = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
         RETRO_DEVICE_ID_JOYPAD_LEFT);
   bool right = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
         RETRO_DEVICE_ID_JOYPAD_RIGHT);
   bool up = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
         RETRO_DEVICE_ID_JOYPAD_UP);
   bool down = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
         RETRO_DEVICE_ID_JOYPAD_DOWN);
   bool l = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
         RETRO_DEVICE_ID_JOYPAD_L);
   bool r = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
         RETRO_DEVICE_ID_JOYPAD_R);

   if (left && !last_left)
      seek_frames -= 10 * media.interpolate_fps;
   if (right && !last_right)
      seek_frames += 10 * media.interpolate_fps;
   if (up && !last_up)
      seek_frames += 60 * media.interpolate_fps;
   if (down && !last_down)
      seek_frames -= 60 * media.interpolate_fps;

   if (l && !last_l && audio_streams_num > 0)
   {
      slock_lock(decode_thread_lock);
      audio_streams_ptr = (audio_streams_ptr + 1) % audio_streams_num;
      slock_unlock(decode_thread_lock);

      char msg[256];
      snprintf(msg, sizeof(msg), "Audio Track #%d.", audio_streams_ptr);
      environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &(struct retro_message) { .msg = msg, .frames = 180 });
   }
   else if (r && !last_r && subtitle_streams_num > 0)
   {
      slock_lock(decode_thread_lock);
      subtitle_streams_ptr = (subtitle_streams_ptr + 1) % subtitle_streams_num;
      slock_unlock(decode_thread_lock);

      char msg[256];
      snprintf(msg, sizeof(msg), "Subtitle Track #%d.", subtitle_streams_ptr);
      environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &(struct retro_message) { .msg = msg, .frames = 180 });
   }

   last_left = left;
   last_right = right;
   last_up = up;
   last_down = down;
   last_l = l;
   last_r = r;

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

      char msg[256];
      snprintf(msg, sizeof(msg), "Seek: %u s.", (unsigned)seek_time);
      environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &(struct retro_message) { .msg = msg, .frames = 180 });

      if (seek_frames < 0)
      {
         if (log_cb)
            log_cb(RETRO_LOG_INFO, "Resetting PTS.\n");
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
   if (audio_streams_num > 0)
   {
      // Audio
      uint64_t expected_audio_frames = frame_cnt * media.sample_rate / media.interpolate_fps;
      to_read_frames = expected_audio_frames - audio_frames;
      size_t to_read_bytes = to_read_frames * sizeof(int16_t) * 2;

      slock_lock(fifo_lock);
      while (!decode_thread_dead && fifo_read_avail(audio_decode_fifo) < to_read_bytes)
      {
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
         if (log_cb)
            log_cb(RETRO_LOG_INFO, "Resetting PTS (bias).\n");
         frames[0].pts = 0.0;
         frames[1].pts = 0.0;
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
            main_sleeping = true;
            scond_signal(fifo_decode_cond);
            scond_wait(fifo_cond, fifo_lock);
            main_sleeping = false;
         }

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

#ifdef HAVE_GL
      float mix_factor = (min_pts - frames[0].pts) / (frames[1].pts - frames[0].pts);
      if (!temporal_interpolation)
         mix_factor = 1.0f;

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
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "Couldn't find suitable decoder, exiting ... \n");
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
   memset(audio_streams, 0, sizeof(audio_streams));
   memset(subtitle_streams, 0, sizeof(subtitle_streams));
   audio_streams_num = 0;
   audio_streams_ptr = 0;
   subtitle_streams_num = 0;
   subtitle_streams_ptr = 0;

   for (unsigned i = 0; i < fctx->nb_streams; i++)
   {
      switch (fctx->streams[i]->codec->codec_type)
      {
         case AVMEDIA_TYPE_AUDIO:
            if (audio_streams_num < MAX_STREAMS)
            {
               if (!open_codec(&actx[audio_streams_num], i))
                  return false;
               audio_streams[audio_streams_num] = i;
               audio_streams_num++;
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
#ifdef HAVE_SSA
            if (subtitle_streams_num < MAX_STREAMS && fctx->streams[i]->codec->codec_id == CODEC_ID_SSA)
            {
               AVCodecContext **s = &sctx[subtitle_streams_num];
               subtitle_streams[subtitle_streams_num] = i;
               if (!open_codec(s, i))
                  return false;

               int size = (*s)->extradata ? (*s)->extradata_size : 0;
               ass_extra_data_size[subtitle_streams_num] = size;

               if (size)
               {
                  ass_extra_data[subtitle_streams_num] = av_malloc(size);
                  memcpy(ass_extra_data[subtitle_streams_num], (*s)->extradata, size);
               }

               subtitle_streams_num++;
            }
#endif
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

   return actx[0] || vctx;
}

static bool init_media_info(void)
{
   if (actx[0])
      media.sample_rate = actx[0]->sample_rate;

   media.interpolate_fps = 60.0;
   if (vctx)
   {
      media.width  = vctx->width;
      media.height = vctx->height;
      media.aspect = (float)vctx->width * av_q2d(vctx->sample_aspect_ratio) / vctx->height;
   }

#ifdef HAVE_SSA
   if (sctx[0])
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

      for (int i = 0; i < subtitle_streams_num; i++)
      {
         ass_track[i] = ass_new_track(ass);
         ass_process_codec_private(ass_track[i], (char*)ass_extra_data[i],
               ass_extra_data_size[i]);
      }
   }
#endif

   return true;
}

static void set_colorspace(struct SwsContext *sws,
      unsigned width, unsigned height, enum AVColorSpace default_color, int in_range)
{
   const int *coeffs = NULL;
   if (colorspace == AVCOL_SPC_UNSPECIFIED)
   {
      if (default_color != AVCOL_SPC_UNSPECIFIED)
         coeffs = sws_getCoefficients(default_color);
      else if (width >= 1280 || height > 576)
         coeffs = sws_getCoefficients(AVCOL_SPC_BT709);
      else
         coeffs = sws_getCoefficients(AVCOL_SPC_BT470BG);
   }
   else
      coeffs = sws_getCoefficients(colorspace);

   if (coeffs)
   {
      int in_full, out_full, brightness, contrast, saturation;
      const int *inv_table, *table;
      sws_getColorspaceDetails(sws, (int**)&inv_table, &in_full,
            (int**)&table, &out_full,
            &brightness, &contrast, &saturation);

      if (in_range != AVCOL_RANGE_UNSPECIFIED)
         in_full = in_range == AVCOL_RANGE_JPEG;

      inv_table = coeffs;
      sws_setColorspaceDetails(sws, inv_table, in_full,
            table, out_full,
            brightness, contrast, saturation);
   }
}

static bool decode_video(AVPacket *pkt, AVFrame *frame, AVFrame *conv, struct SwsContext *sws)
{
   int got_ptr = 0;
   int ret = avcodec_decode_video2(vctx, frame, &got_ptr, pkt);
   if (ret < 0)
      return false;

   if (got_ptr)
   {
      set_colorspace(sws, media.width, media.height,
            av_frame_get_colorspace(frame), av_frame_get_color_range(frame));
      sws_scale(sws, (const uint8_t * const*)frame->data, frame->linesize, 0, media.height,
            conv->data, conv->linesize);
      return true;
   }
   else
      return false;
}

static int16_t *decode_audio(AVCodecContext *ctx, AVPacket *pkt, AVFrame *frame, int16_t *buffer, size_t *buffer_cap,
      SwrContext *swr)
{
   AVPacket pkt_tmp = *pkt;

   int got_ptr = 0;

   for (;;)
   {
      int ret = 0;
      ret = avcodec_decode_audio4(ctx, frame, &got_ptr, &pkt_tmp);
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
         if (!main_sleeping)
            scond_wait(fifo_decode_cond, fifo_lock);
         else
         {
            if (log_cb)
               log_cb(RETRO_LOG_ERROR, "Thread: Audio deadlock detected ...\n");
            fifo_clear(audio_decode_fifo);
            break;
         }
      }

      decode_last_audio_time = pts * av_q2d(fctx->streams[audio_streams[audio_streams_ptr]]->time_base);
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
   if (ret < 0 && log_cb)
      log_cb(RETRO_LOG_ERROR, "av_seek_frame() failed.\n");

   if (actx[audio_streams_ptr])
      avcodec_flush_buffers(actx[audio_streams_ptr]);
   if (vctx)
      avcodec_flush_buffers(vctx);
   if (sctx[subtitle_streams_ptr])
      avcodec_flush_buffers(sctx[subtitle_streams_ptr]);
#ifdef HAVE_SSA
   if (ass_track[subtitle_streams_ptr])
      ass_flush_events(ass_track[subtitle_streams_ptr]);
#endif
}

#ifdef HAVE_SSA
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
#endif

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

   SwrContext *swr[audio_streams_num];
   for (int i = 0; i < audio_streams_num; i++)
   {
      swr[i] = swr_alloc();

      av_opt_set_int(swr[i], "in_channel_layout", actx[i]->channel_layout, 0);
      av_opt_set_int(swr[i], "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
      av_opt_set_int(swr[i], "in_sample_rate", actx[i]->sample_rate, 0);
      av_opt_set_int(swr[i], "out_sample_rate", media.sample_rate, 0);
      av_opt_set_int(swr[i], "in_sample_fmt", actx[i]->sample_fmt, 0);
      av_opt_set_int(swr[i], "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
      swr_init(swr[i]);
   }

   AVFrame *aud_frame = av_frame_alloc();
   AVFrame *vid_frame = av_frame_alloc();

   AVFrame *conv_frame = NULL;
   void *conv_frame_buf = NULL;
   size_t frame_size = 0;

   if (video_stream >= 0)
   {
      frame_size = avpicture_get_size(PIX_FMT_RGB32, media.width, media.height);
      conv_frame = av_frame_alloc();
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

      slock_lock(decode_thread_lock);
      int audio_stream = audio_streams[audio_streams_ptr];
      int audio_stream_ptr = audio_streams_ptr;
      int subtitle_stream = subtitle_streams[subtitle_streams_ptr];
      AVCodecContext *actx_active = actx[audio_streams_ptr];
      AVCodecContext *sctx_active = sctx[subtitle_streams_ptr];
#ifdef HAVE_SSA
      ASS_Track *ass_track_active = ass_track[subtitle_streams_ptr];
#endif
      slock_unlock(decode_thread_lock);

      if (pkt.stream_index == video_stream)
      {
         if (decode_video(&pkt, vid_frame, conv_frame, sws))
         {
            int64_t pts = av_frame_get_best_effort_timestamp(vid_frame);

            double video_time = pts * av_q2d(fctx->streams[video_stream]->time_base);
#ifdef HAVE_SSA
            if (ass_render)
            {
               int change = 0;
               ASS_Image *img = ass_render_frame(ass_render, ass_track_active,
                     1000 * video_time, &change);

               // Do it on CPU for now.
               // We're in a thread anyways, so shouldn't really matter.
               render_ass_img(conv_frame, img);
            }
#endif

            size_t decoded_size = frame_size + sizeof(pts);
            slock_lock(fifo_lock);
            while (!decode_thread_dead && fifo_write_avail(video_decode_fifo) < decoded_size)
            {
               if (!main_sleeping)
                  scond_wait(fifo_decode_cond, fifo_lock);
               else
               {
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
            }
            scond_signal(fifo_cond);
            slock_unlock(fifo_lock);
         }
      }
      else if (pkt.stream_index == audio_stream)
      {
         audio_buffer = decode_audio(actx_active, &pkt, aud_frame,
               audio_buffer, &audio_buffer_cap,
               swr[audio_stream_ptr]);
      }
      else if (pkt.stream_index == subtitle_stream)
      {
         AVSubtitle sub;
         memset(&sub, 0, sizeof(sub));

         int finished = 0;
         while (!finished)
         {
            if (avcodec_decode_subtitle2(sctx_active, &sub, &finished, &pkt) < 0 && log_cb)
            {
               log_cb(RETRO_LOG_ERROR, "Decode subtitles failed.\n");
               break;
            }
         }

#ifdef HAVE_SSA
         for (int i = 0; i < sub.num_rects; i++)
         {
            if (sub.rects[i]->ass)
               ass_process_data(ass_track_active, sub.rects[i]->ass, strlen(sub.rects[i]->ass));
         }
#endif

         avsubtitle_free(&sub);
      }

      av_free_packet(&pkt);
   }

   if (sws)
      sws_freeContext(sws);
   sws = NULL;

   for (int i = 0; i < audio_streams_num; i++)
      swr_free(&swr[i]);

   av_frame_free(&aud_frame);
   av_frame_free(&vid_frame);
   av_frame_free(&conv_frame);
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

   av_dump_format(fctx, 0, info->path, 0);

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
   if (audio_streams_num > 0)
      audio_decode_fifo = fifo_new(20 * media.sample_rate * sizeof(int16_t) * 2);

   fifo_cond = scond_new();
   fifo_decode_cond = scond_new();
   fifo_lock = slock_new();
   decode_thread_lock = slock_new();

   check_variables();

   decode_thread_handle = sthread_create(decode_thread, NULL);

   video_frame_temp_buffer = av_malloc(media.width * media.height * sizeof(uint32_t));

   pts_bias = 0.0;

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
   if (decode_thread_lock)
      slock_free(decode_thread_lock);

   if (video_decode_fifo)
      fifo_free(video_decode_fifo);
   if (audio_decode_fifo)
      fifo_free(audio_decode_fifo);

   fifo_cond = NULL;
   fifo_decode_cond = NULL;
   fifo_lock = NULL;
   decode_thread_lock = NULL;
   video_decode_fifo = NULL;
   audio_decode_fifo = NULL;

   decode_last_video_time = 0.0;
   decode_last_audio_time = 0.0;

   frames[0].pts = frames[1].pts = 0.0;
   pts_bias = 0.0;
   frame_cnt = 0;
   audio_frames = 0;

   for (unsigned i = 0; i < MAX_STREAMS; i++)
   {
      if (sctx[i])
         avcodec_close(sctx[i]);
      if (actx[i])
         avcodec_close(actx[i]);
      sctx[i] = NULL;
      actx[i] = NULL;
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

   for (size_t i = 0; i < attachments_size; i++)
      av_freep(&attachments[i].data);
   av_freep(&attachments);
   attachments_size = 0;

#ifdef HAVE_SSA
   for (unsigned i = 0; i < MAX_STREAMS; i++)
   {
      if (ass_track[i])
         ass_free_track(ass_track[i]);
      ass_track[i] = NULL;

      av_freep(&ass_extra_data[i]);
      ass_extra_data_size[i] = 0;
   }
   if (ass_render)
      ass_renderer_done(ass_render);
   if (ass)
      ass_library_done(ass);

   ass_render = NULL;
   ass = NULL;
#endif

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

