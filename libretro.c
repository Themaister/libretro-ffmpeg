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
#include <libswresample/swresample.h>

#include <GL/glew.h>

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
static double decode_last_video_time;
static double decode_last_audio_time;

static uint32_t *video_read_buffer;

static double pts_bias;
static double last_audio_pts;

static bool main_sleeping;

// Seeking.
static bool do_seek;
static double seek_time;

// GL stuff
static struct retro_hw_render_callback hw_render;
struct frame
{
   GLuint tex;
   GLuint pbo;
   double pts;
};

static struct frame frames[2];

static GLuint prog;
static GLuint vbo;
static GLint vertex_loc;
static GLint tex_loc;
static GLint mix_loc;

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

   int seek_frames = 0;
   static bool last_left;
   static bool last_right;
   bool left = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
         RETRO_DEVICE_ID_JOYPAD_LEFT);
   bool right = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
         RETRO_DEVICE_ID_JOYPAD_RIGHT);

   if (left && !last_left)
      seek_frames -= 600;
   if (right && !last_right)
      seek_frames += 600;

   last_left = left;
   last_right = right;

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

      frames[0].pts = 0.0;
      frames[1].pts = 0.0;
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

   double min_pts = frame_cnt / media.interpolate_fps + pts_bias;
   if (video_stream >= 0)
   {
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

            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, frames[1].pbo);
            uint32_t *data = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER,
                  0, media.width * media.height, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);

            fifo_read(video_decode_fifo, data, media.width * media.height * sizeof(uint32_t));

            fprintf(stderr, "Read frame, frames: #%zu\n", fifo_read_avail(video_decode_fifo) /
                  to_read_frame_bytes);

            glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
            glBindTexture(GL_TEXTURE_2D, frames[1].tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                  media.width, media.height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
         }

         scond_signal(fifo_decode_cond);
         slock_unlock(fifo_lock);

         if (pts != AV_NOPTS_VALUE)
            frames[1].pts = av_q2d(fctx->streams[video_stream]->time_base) * pts;
         else if (frames[1].pts < frames[0].pts)
            frames[1].pts = frames[0].pts + 1.0 / media.fps;
         else
            frames[1].pts += 1.0 / media.fps;

         //fprintf(stderr, "OLD: %.2f s\n", frames[0].pts);
         //fprintf(stderr, "PTS: %.2f s\n", frames[1].pts);
      }

      //fprintf(stderr, "Main: Found suitable frame.\n");

      float mix_factor = (min_pts - frames[0].pts) / (frames[1].pts - frames[0].pts);

      //fprintf(stderr, "Mix factor: %f, diff: %f\n", mix_factor, frames[1].pts - frames[0].pts);

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
   }
   else
      video_cb(NULL, 1, 1, sizeof(uint32_t));

   if (audio_stream >= 0)
   {
      // Audio
      uint64_t expected_audio_frames = frame_cnt * media.sample_rate / media.interpolate_fps;
      size_t to_read_frames = expected_audio_frames - audio_frames;
      size_t to_read_bytes = to_read_frames * sizeof(int16_t) * 2;

      slock_lock(fifo_lock);
      while (!decode_thread_dead && fifo_read_avail(audio_decode_fifo) < to_read_bytes)
      {
         fprintf(stderr, "Main: Audio fifo is empty.\n");
         main_sleeping = true;
         scond_signal(fifo_decode_cond);
         scond_wait(fifo_cond, fifo_lock);
         main_sleeping = false;
      }

      int16_t audio_buffer[2048];
      if (!decode_thread_dead)
         fifo_read(audio_decode_fifo, audio_buffer, to_read_bytes);
      scond_signal(fifo_decode_cond);

      slock_unlock(fifo_lock);

      if (!decode_thread_dead)
         audio_batch_cb(audio_buffer, to_read_frames);

      audio_frames += to_read_frames;
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
      int ret = avcodec_decode_video2(vctx, frame, &got_ptr, pkt);
      if (ret <= 0)
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

static void decode_thread_seek(double time)
{
   int flags = 0;
   double seek_to = 0.0;
   int stream = -1;

   if (video_stream >= 0)
   {
      stream = video_stream;
      double tb = 1.0 / av_q2d(fctx->streams[video_stream]->time_base);
      seek_to = time * tb;
      if (time < decode_last_video_time)
         flags = AVSEEK_FLAG_BACKWARD;

   }
   else if (audio_stream >= 0)
   {
      stream = audio_stream;
      double tb = 1.0 / av_q2d(fctx->streams[audio_stream]->time_base);
      seek_to = time * tb;
      if (time < decode_last_audio_time)
         flags = AVSEEK_FLAG_BACKWARD;
   }

   if (seek_to < 0.0)
      seek_to = 0.0;

   decode_last_video_time = time;
   decode_last_audio_time = time;

   int ret = av_seek_frame(fctx, stream, seek_to, flags);
   if (ret < 0)
      fprintf(stderr, "av_seek_frame() failed.\n");
   else
      fprintf(stderr, "Seeking successful to %.2f!\n", time);

   if (actx)
      avcodec_flush_buffers(actx);
   if (vctx)
      avcodec_flush_buffers(vctx);
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

      if (pkt.stream_index == video_stream)
      {
         if (decode_video(&pkt, vid_frame, conv_frame, sws))
         {
            int64_t pts = av_frame_get_best_effort_timestamp(vid_frame);
            decode_last_video_time = pts * av_q2d(fctx->streams[video_stream]->time_base);
            //fprintf(stderr, "Got video frame PTS: %.2f.\n", decode_last_video_time);

            size_t decoded_size = frame_size + sizeof(pts);
            slock_lock(fifo_lock);
            while (!decode_thread_dead && fifo_write_avail(video_decode_fifo) < decoded_size)
            {
               //fprintf(stderr, "Thread: Video fifo is full ...\n");
               if (!main_sleeping)
                  scond_wait(fifo_decode_cond, fifo_lock);
               else
               {
                  fprintf(stderr, "Thread: Video deadlock detected ...\n");
                  fifo_clear(video_decode_fifo);
                  break;
               }
            }

            if (!decode_thread_dead)
            {
               fifo_write(video_decode_fifo, &pts, sizeof(pts));
               const uint8_t *src = conv_frame->data[0];
               int stride = conv_frame->linesize[0];
               for (unsigned y = 0; y < media.height; y++, src += stride)
                  fifo_write(video_decode_fifo, src, media.width * sizeof(uint32_t));

               fprintf(stderr, "Wrote frame, frames: #%zu\n", fifo_read_avail(video_decode_fifo) /
                     decoded_size);
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

         int64_t pts = av_frame_get_best_effort_timestamp(aud_frame);
         decode_last_audio_time = pts * av_q2d(fctx->streams[audio_stream]->time_base);
         //fprintf(stderr, "Got audio frame PTS: %.2f.\n", decode_last_audio_time);

         slock_lock(fifo_lock);
         while (!decode_thread_dead && fifo_write_avail(audio_decode_fifo) < decoded_size)
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

static void context_reset(void)
{
   glewInit();
   prog = glCreateProgram();
   GLuint vert = glCreateShader(GL_VERTEX_SHADER);
   GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);

   static const char *vertex_source =
      "attribute vec2 aVertex;\n"
      "attribute vec2 aTexCoord;\n"
      "varying vec2 vTex;\n"
      "void main() { gl_Position = vec4(aVertex, 0.0, 1.0); vTex = aTexCoord; }\n";

   static const char *fragment_source =
      "varying vec2 vTex;\n"
      "uniform sampler2D sTex0;\n"
      "uniform sampler2D sTex1;\n"
      "uniform float uMix;\n"
      "void main() { gl_FragColor = vec4(mix(texture2D(sTex0, vTex).rgb, texture2D(sTex1, vTex).rgb, uMix), 1.0); }\n";

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
      glGenBuffers(1, &frames[i].pbo);

      glBindTexture(GL_TEXTURE_2D, frames[i].tex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, frames[i].pbo);
      glBufferData(GL_PIXEL_UNPACK_BUFFER, media.width * media.height * sizeof(uint32_t), NULL, GL_STREAM_DRAW);
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
   glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
   glBindTexture(GL_TEXTURE_2D, 0);
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
   {
      video_decode_fifo = fifo_new(media.width * media.height * sizeof(uint32_t) * 64);

      hw_render.context_reset = context_reset;
      hw_render.context_type = RETRO_HW_CONTEXT_OPENGL;
      if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
         LOG_ERR_GOTO("Cannot initialize HW render.", error);
   }
   if (audio_stream >= 0)
      audio_decode_fifo = fifo_new(2 * media.sample_rate * 2 * sizeof(int16_t) * 2);

   fifo_cond = scond_new();
   fifo_decode_cond = scond_new();
   fifo_lock = slock_new();

   video_read_buffer = av_malloc(media.width * media.height * sizeof(uint32_t));

   decode_thread_handle = sthread_create(decode_thread, NULL);

   pts_bias = 0.0; // Hack

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

   decode_last_video_time = 0.0;
   decode_last_audio_time = 0.0;

   frames[0].pts = frames[1].pts = 0.0;
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

