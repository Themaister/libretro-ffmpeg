#include "fft.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <new>

#define GLM_SWIZZLE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/constants.hpp>
using namespace glm;

#define GL_DEBUG 0
#if GL_DEBUG
#define GL_CHECK_ERROR() do { \
   if (glGetError() != GL_NO_ERROR) \
   { \
      fprintf(stderr, "GL error at line: %d\n", __LINE__); \
      abort(); \
   } \
} while(0)
#else
#define GL_CHECK_ERROR()
#endif

static const char vertex_program_heightmap[] =
   "#version 300 es\n"
   "layout(location = 0) in vec2 aVertex;\n"
   "uniform sampler2D sHeight;\n"
   "uniform mat4 uMVP;\n"
   "uniform ivec2 uOffset;\n"
   "uniform vec4 uHeightmapParams;\n"
   "uniform float uAngleScale;\n"
   "out vec3 vWorldPos;\n"
   "out vec3 vHeight;\n"

   "#define PI 3.141592653\n"

   "void main() {\n"
   "  vec2 tex_coord = vec2(aVertex.x + float(uOffset.x) + 0.5, -aVertex.y + float(uOffset.y) + 0.5) / vec2(textureSize(sHeight, 0));\n"

   "  vec3 world_pos = vec3(aVertex.x, 0.0, aVertex.y);\n"
   "  world_pos.xz += uHeightmapParams.xy;\n"

   "  float angle = world_pos.x * uAngleScale;\n"
   "  world_pos.xz *= uHeightmapParams.zw;\n"

   "  float lod = log2(world_pos.z + 1.0) - 6.0;\n"
   "  vec4 heights = textureLod(sHeight, tex_coord, lod);\n"
   
   "  float cangle = cos(angle);\n"
   "  float sangle = sin(angle);\n"

   "  int c = int(-sign(world_pos.x) + 1.0);\n"
   "  float height = mix(heights[c], heights[1], abs(angle) / PI);\n"
   "  height = height * 80.0 - 40.0;\n"

   "  vec3 up = vec3(-sangle, cangle, 0.0);\n"

   "  float base_y = 80.0 - 80.0 * cangle;\n"
   "  float base_x = 80.0 * sangle;\n"
   "  world_pos.xy = vec2(base_x, base_y);\n"
   "  world_pos += up * height;\n"

   "  vWorldPos = world_pos;\n"
   "  vHeight = vec3(height, heights.yw * 80.0 - 40.0);\n"
   "  gl_Position = uMVP * vec4(world_pos, 1.0);\n"
   "}";

static const char fragment_program_heightmap[] =
   "#version 300 es\n"
   "precision mediump float;\n"
   "out vec4 FragColor;\n"
   "in vec3 vWorldPos;\n"
   "in vec3 vHeight;\n"

   "vec3 colormap(vec3 height) {\n"
   "   return 1.0 / (1.0 + exp(-0.08 * height));\n"
   "}"

   "void main() {\n"
   "   vec3 color = mix(vec3(1.0, 0.7, 0.7) * colormap(vHeight), vec3(0.1, 0.15, 0.1), clamp(vWorldPos.z / 400.0, 0.0, 1.0));\n"
   "   color = mix(color, vec3(0.1, 0.15, 0.1), clamp(1.0 - vWorldPos.z / 2.0, 0.0, 1.0));\n"
   "   FragColor = vec4(color, 1.0);\n"
   "}";

static const char vertex_program[] =
   "#version 300 es\n"
   "layout(location = 0) in vec2 aVertex;\n"
   "layout(location = 1) in vec2 aTexCoord;\n"
   "uniform vec4 uOffsetScale;\n"
   "out vec2 vTex;\n"
   "void main() {\n"
   "   vTex = uOffsetScale.xy + aTexCoord * uOffsetScale.zw;\n"
   "   gl_Position = vec4(aVertex, 0.0, 1.0);\n"
   "}";

static const char fragment_program_resolve[] =
   "#version 300 es\n"
   "precision mediump float;\n"
   "precision highp int;\n"
   "precision highp usampler2D;\n"
   "precision highp isampler2D;\n"
   "in vec2 vTex;\n"
   "out vec4 FragColor;\n"
   "uniform usampler2D sFFT;\n"

   "vec4 get_heights(highp uvec2 h) {\n"
   "  vec2 l = unpackHalf2x16(h.x);\n"
   "  vec2 r = unpackHalf2x16(h.y);\n"
   "  vec2 channels[4] = vec2[4](\n"
   "     l, 0.5 * (l + r), r, 0.5 * (l - r));\n"
   "  vec4 amps;\n"
   "  for (int i = 0; i < 4; i++)\n"
   "     amps[i] = dot(channels[i], channels[i]);\n"

   "  return 9.0 * log(amps + 0.0001) - 22.0;\n"  
   "}\n"

   "void main() {\n"
   "   uvec2 h = textureLod(sFFT, vTex, 0.0).rg;\n"
   "   vec4 height = get_heights(h);\n"
   "   height = (height + 40.0) / 80.0;\n"
   "   FragColor = height;\n"
   "}";

static const char fragment_program_blur[] =
   "#version 300 es\n"
   "precision mediump float;\n"
   "precision highp int;\n"
   "precision highp usampler2D;\n"
   "precision highp isampler2D;\n"
   "in vec2 vTex;\n"
   "out vec4 FragColor;\n"
   "uniform sampler2D sHeight;\n"
   "void main() {\n"
   "   float k = 0.0;\n"
   "   float t;\n"
   "   vec4 res = vec4(0.0);\n"
   "   #define kernel(x, y) t = exp(-0.35 * float((x) * (x) + (y) * (y))); k += t; res += t * textureLodOffset(sHeight, vTex, 0.0, ivec2(x, y))\n"
   "   kernel(-1, -2);\n"
   "   kernel(-1, -1);\n"
   "   kernel(-1,  0);\n"
   "   kernel( 0, -2);\n"
   "   kernel( 0, -1);\n"
   "   kernel( 0,  0);\n"
   "   kernel( 1, -2);\n"
   "   kernel( 1, -1);\n"
   "   kernel( 1,  0);\n"
   "   FragColor = res / k;\n"
   "}";

static const char fragment_program_real[] =
   "#version 300 es\n"
   "precision mediump float;\n"
   "precision highp int;\n"
   "precision highp usampler2D;\n"
   "precision highp isampler2D;\n"

   "in vec2 vTex;\n"
   "uniform isampler2D sTexture;\n"
   "uniform usampler2D sParameterTexture;\n"
   "uniform usampler2D sWindow;\n"
   "uniform int uViewportOffset;\n"
   "out uvec2 FragColor;\n"

   "vec2 compMul(vec2 a, vec2 b) { return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }\n"

   "void main() {\n"
   "   uvec2 params = texture(sParameterTexture, vec2(vTex.x, 0.5)).rg;\n"
   "   uvec2 coord  = uvec2((params.x >> 16u) & 0xffffu, params.x & 0xffffu);\n"
   "   int ycoord   = int(gl_FragCoord.y) - uViewportOffset;\n"
   "   vec2 twiddle = unpackHalf2x16(params.y);\n"

   "   float window_a = float(texelFetch(sWindow, ivec2(coord.x, 0), 0).r) / float(0x10000);\n"
   "   float window_b = float(texelFetch(sWindow, ivec2(coord.y, 0), 0).r) / float(0x10000);\n"

   "   vec2 a = window_a * vec2(texelFetch(sTexture, ivec2(int(coord.x), ycoord), 0).rg) / vec2(0x8000);\n"
   "   vec2 a_l = vec2(a.x, 0.0);\n"
   "   vec2 a_r = vec2(a.y, 0.0);\n"
   "   vec2 b = window_b * vec2(texelFetch(sTexture, ivec2(int(coord.y), ycoord), 0).rg) / vec2(0x8000);\n"
   "   vec2 b_l = vec2(b.x, 0.0);\n"
   "   vec2 b_r = vec2(b.y, 0.0);\n"
   "   b_l = compMul(b_l, twiddle);\n"
   "   b_r = compMul(b_r, twiddle);\n"

   "   vec2 res_l = a_l + b_l;\n"
   "   vec2 res_r = a_r + b_r;\n"
   "   FragColor = uvec2(packHalf2x16(res_l), packHalf2x16(res_r));\n"
   "}";

static const char fragment_program_complex[] =
   "#version 300 es\n"
   "precision mediump float;\n"
   "precision highp int;\n"
   "precision highp usampler2D;\n"
   "precision highp isampler2D;\n"

   "in vec2 vTex;\n"
   "uniform usampler2D sTexture;\n"
   "uniform usampler2D sParameterTexture;\n"
   "uniform int uViewportOffset;\n"
   "out uvec2 FragColor;\n"

   "vec2 compMul(vec2 a, vec2 b) { return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }\n"

   "void main() {\n"
   "   uvec2 params = texture(sParameterTexture, vec2(vTex.x, 0.5)).rg;\n"
   "   uvec2 coord  = uvec2((params.x >> 16u) & 0xffffu, params.x & 0xffffu);\n"
   "   int ycoord   = int(gl_FragCoord.y) - uViewportOffset;\n"
   "   vec2 twiddle = unpackHalf2x16(params.y);\n"

   "   uvec2 x = texelFetch(sTexture, ivec2(int(coord.x), ycoord), 0).rg;\n"
   "   uvec2 y = texelFetch(sTexture, ivec2(int(coord.y), ycoord), 0).rg;\n"
   "   vec4 a = vec4(unpackHalf2x16(x.x), unpackHalf2x16(x.y));\n"
   "   vec4 b = vec4(unpackHalf2x16(y.x), unpackHalf2x16(y.y));\n"
   "   b.xy = compMul(b.xy, twiddle);\n"
   "   b.zw = compMul(b.zw, twiddle);\n"

   "   vec4 res = a + b;\n"
   "   FragColor = uvec2(packHalf2x16(res.xy), packHalf2x16(res.zw));\n"
   "}";

GLuint GLFFT::compile_shader(GLenum type, const char *source)
{
   GLuint shader = glCreateShader(type);
   glShaderSource(shader, 1, (const GLchar**)&source, NULL);
   glCompileShader(shader);
   
   GLint status = 0;
   glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

   if (!status)
   {
      fprintf(stderr, "Failed to compile.\n");
      char log_info[8 * 1024];
      GLsizei log_len;
      glGetShaderInfoLog(shader, sizeof(log_info), &log_len, log_info);
      fprintf(stderr, "ERROR: %s\n", log_info);
      return 0;
   }

   return shader;
}

GLuint GLFFT::compile_program(const char *vertex_source, const char *fragment_source)
{
   GLuint prog = glCreateProgram();
   GLuint vert = compile_shader(GL_VERTEX_SHADER, vertex_source);
   GLuint frag = compile_shader(GL_FRAGMENT_SHADER, fragment_source);

   glAttachShader(prog, vert);
   glAttachShader(prog, frag);
   glLinkProgram(prog);

   GLint status = 0;
   glGetProgramiv(prog, GL_LINK_STATUS, &status);
   if (!status)
   {
      fprintf(stderr, "Failed to link.\n");
      char log_info[8 * 1024];
      GLsizei log_len;
      glGetProgramInfoLog(prog, sizeof(log_info), &log_len, log_info);
      fprintf(stderr, "ERROR: %s\n", log_info);
   }

   glDeleteShader(vert);
   glDeleteShader(frag);
   return prog;
}

void GLFFT::render(GLuint backbuffer, unsigned width, unsigned height)
{
   // Render scene.
   glBindFramebuffer(GL_FRAMEBUFFER, backbuffer);
   glViewport(0, 0, width, height);
   glClearColor(0.1f, 0.15f, 0.1f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

   vec3 eye(0, 80, -60);
   mat4 mvp = perspective(half_pi<float>(), (float)width / height, 1.0f, 500.0f) *
      lookAt(eye, eye + vec3(0.0f, 0.0f, 1.0f), vec3(0.0f, 1.0f, 0.0f));

   glUseProgram(block.prog);
   glUniformMatrix4fv(glGetUniformLocation(block.prog, "uMVP"), 1, GL_FALSE, value_ptr(mvp));
   glUniform2i(glGetUniformLocation(block.prog, "uOffset"), (-int(fft_block_size) + 1) / 2, output_ptr);
   glUniform4f(glGetUniformLocation(block.prog, "uHeightmapParams"), -(fft_block_size - 1.0f) / 2.0f, 0.0f, 3.0f, 2.0f);
   glUniform1f(glGetUniformLocation(block.prog, "uAngleScale"), pi<float>() / ((fft_block_size - 1) / 2));

   glBindVertexArray(block.vao);
   glBindTexture(GL_TEXTURE_2D, blur.tex);
   glDrawElements(GL_TRIANGLE_STRIP, block.elems, GL_UNSIGNED_INT, NULL);
   glBindVertexArray(0);
   glUseProgram(0);
}

void GLFFT::step_fft(const GLshort *audio_buffer, unsigned frames)
{
   glEnable(GL_DEPTH_TEST);
   glEnable(GL_CULL_FACE);
   glBindVertexArray(vao);

   glActiveTexture(GL_TEXTURE2);
   glBindTexture(GL_TEXTURE_2D, window_tex);
   
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, input_tex);
   glUseProgram(prog_real);

   memmove(sliding.data(), sliding.data() + frames * 2, (sliding.size() - 2 * frames) * sizeof(GLshort));
   memcpy(sliding.data() + sliding.size() - frames * 2, audio_buffer, 2 * frames * sizeof(GLshort));

   // Upload audio data to GPU.
   glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
   GLshort *buffer = static_cast<GLshort*>(glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0,
            2 * fft_size * sizeof(GLshort), GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT));
   if (buffer)
   {
      memcpy(buffer, sliding.data(), sliding.size() * sizeof(GLshort));
      glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
   }
   glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fft_size, 1, GL_RG_INTEGER, GL_SHORT, NULL);
   glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

   // Perform FFT of new block.
   glViewport(0, 0, fft_size, 1);
   for (unsigned i = 0; i < fft_steps; i++)
   {
      if (i == fft_steps - 1)
      {
         glBindFramebuffer(GL_FRAMEBUFFER, output.fbo);
         glUniform1i(glGetUniformLocation(i == 0 ? prog_real : prog_complex, "uViewportOffset"), output_ptr);
         glViewport(0, output_ptr, fft_size, 1);
      }
      else
      {
         glUniform1i(glGetUniformLocation(i == 0 ? prog_real : prog_complex, "uViewportOffset"), 0);
         glBindFramebuffer(GL_FRAMEBUFFER, passes[i].target.fbo);
         glClear(GL_COLOR_BUFFER_BIT);
      }

      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, passes[i].parameter_tex);

      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, passes[i].target.tex);

      if (i == 0)
         glUseProgram(prog_complex);
   }
   glActiveTexture(GL_TEXTURE0);

   // Resolve new chunk to heightmap.
   glViewport(0, output_ptr, fft_size, 1);
   glUseProgram(prog_resolve);
   glBindFramebuffer(GL_FRAMEBUFFER, resolve.fbo);
   const GLfloat resolve_offset[] = { 0.0f, float(output_ptr) / fft_depth, 1.0f, 1.0f / fft_depth };
   glUniform4fv(glGetUniformLocation(prog_resolve, "uOffsetScale"), 1, resolve_offset);
   glBindTexture(GL_TEXTURE_2D, output.tex);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

   // Re-blur damaged regions of heightmap.
   glUseProgram(prog_blur);
   glBindTexture(GL_TEXTURE_2D, resolve.tex);
   glBindFramebuffer(GL_FRAMEBUFFER, blur.fbo);
   glUniform4fv(glGetUniformLocation(prog_blur, "uOffsetScale"), 1, resolve_offset);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

   // Mipmap the heightmap.
   glBindTexture(GL_TEXTURE_2D, blur.tex);
   glGenerateMipmap(GL_TEXTURE_2D);
   glBindTexture(GL_TEXTURE_2D, 0);

   output_ptr++;
   output_ptr &= fft_depth - 1;

   glBindVertexArray(0);
   glUseProgram(0);
   GL_CHECK_ERROR();
}

static inline unsigned log2i(unsigned x)
{
   unsigned res;
   for (res = 0; x; x >>= 1)
      res++;
   return res - 1;
}

static inline unsigned bitinverse(unsigned x, unsigned size)
{
   unsigned size_log2 = log2i(size);
   unsigned ret = 0;
   for (unsigned i = 0; i < size_log2; i++)
      ret |= ((x >> i) & 0x1) << (size_log2 - 1 - i);
   return ret;
}

void GLFFT::build_fft_params(GLuint *buffer, unsigned step, unsigned size)
{
   unsigned step_size = 1 << step;
   for (unsigned i = 0; i < size; i += step_size << 1)
   {
      for (unsigned j = i; j < i + step_size; j++)
      {
         int s = j - i;
         float phase = -1.0f * (float)s / step_size;

         float twiddle_real = cos(M_PI * phase);
         float twiddle_imag = sin(M_PI * phase);

         unsigned a = j;
         unsigned b = j + step_size;

         unsigned read_a = (step == 0) ? bitinverse(a, size) : a;
         unsigned read_b = (step == 0) ? bitinverse(b, size) : b;

         buffer[2 * a + 0] = (read_a << 16) | read_b;
         buffer[2 * a + 1] = packHalf2x16(vec2(twiddle_real, twiddle_imag));
         buffer[2 * b + 0] = (read_a << 16) | read_b;
         buffer[2 * b + 1] = packHalf2x16(-vec2(twiddle_real, twiddle_imag));
      }
   }
}

void GLFFT::init_quad_vao(void)
{
   static const GLbyte quad_buffer[] = {
      -1, -1, 1, -1, -1, 1, 1, 1,
       0,  0, 1,  0,  0, 1, 1, 1,
   };
   glGenBuffers(1, quad.addr());
   glBindBuffer(GL_ARRAY_BUFFER, quad);
   glBufferData(GL_ARRAY_BUFFER, sizeof(quad_buffer), quad_buffer, GL_STATIC_DRAW);
   glBindBuffer(GL_ARRAY_BUFFER, 0);

   glGenVertexArrays(1, vao.addr());
   glBindVertexArray(vao);
   glBindBuffer(GL_ARRAY_BUFFER, quad);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glVertexAttribPointer(0, 2, GL_BYTE, GL_FALSE, 0, 0);
   glVertexAttribPointer(1, 2, GL_BYTE, GL_FALSE, 0, reinterpret_cast<const GLvoid*>(uintptr_t(8)));
   glBindVertexArray(0);
   glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// Modified Bessel function of first order.
// Check Wiki for mathematical definition ...
static inline double kaiser_besseli0(double x)
{
   unsigned i;
   double sum = 0.0;

   double factorial = 1.0;
   double factorial_mult = 0.0;
   double x_pow = 1.0;
   double two_div_pow = 1.0;
   double x_sqr = x * x;

   // Approximate. This is an infinite sum.
   // Luckily, it converges rather fast.
   for (i = 0; i < 18; i++)
   {
      sum += x_pow * two_div_pow / (factorial * factorial);

      factorial_mult += 1.0;
      x_pow *= x_sqr;
      two_div_pow *= 0.25;
      factorial *= factorial_mult;
   }

   return sum;
}

static inline double kaiser_window(double index, double beta)
{
   return kaiser_besseli0(beta * glm::sqrt(1 - index * index));
}

void GLFFT::init_texture(GLuint &tex, GLenum format, unsigned width, unsigned height, unsigned levels, GLenum mag, GLenum min)
{
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexStorage2D(GL_TEXTURE_2D, levels, format, width, height);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min);
   glBindTexture(GL_TEXTURE_2D, 0);
}

void GLFFT::init_target(Target &target, GLenum format, unsigned width, unsigned height, unsigned levels, GLenum mag, GLenum min)
{
   init_texture(target.tex, format, width, height, levels, mag, min);
   glGenFramebuffers(1, target.fbo.addr());
   glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);

   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
      target.tex, 0);

   glClearColor(0, 0, 0, 0);
   glClear(GL_COLOR_BUFFER_BIT);
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GLFFT::init_fft()
{
   prog_real = compile_program(vertex_program, fragment_program_real);
   prog_complex = compile_program(vertex_program, fragment_program_complex);
   prog_resolve = compile_program(vertex_program, fragment_program_resolve);
   prog_blur = compile_program(vertex_program, fragment_program_blur);
   GL_CHECK_ERROR();

   static const GLfloat unity[] = { 0.0f, 0.0f, 1.0f, 1.0f };

   glUseProgram(prog_real);
   glUniform1i(glGetUniformLocation(prog_real, "sTexture"), 0);
   glUniform1i(glGetUniformLocation(prog_real, "sParameterTexture"), 1);
   glUniform1i(glGetUniformLocation(prog_real, "sWindow"), 2);
   glUniform4fv(glGetUniformLocation(prog_real, "uOffsetScale"), 1, unity);

   glUseProgram(prog_complex);
   glUniform1i(glGetUniformLocation(prog_complex, "sTexture"), 0);
   glUniform1i(glGetUniformLocation(prog_complex, "sParameterTexture"), 1);
   glUniform4fv(glGetUniformLocation(prog_complex, "uOffsetScale"), 1, unity);
   
   glUseProgram(prog_resolve);
   glUniform1i(glGetUniformLocation(prog_resolve, "sFFT"), 0);
   glUniform4fv(glGetUniformLocation(prog_resolve, "uOffsetScale"), 1, unity);

   glUseProgram(prog_blur);
   glUniform1i(glGetUniformLocation(prog_blur, "sHeight"), 0);
   glUniform4fv(glGetUniformLocation(prog_blur, "uOffsetScale"), 1, unity);

   GL_CHECK_ERROR();

   init_texture(window_tex, GL_R16UI, fft_size, 1);
   GL_CHECK_ERROR();
#define KAISER_BETA 12.0
   std::vector<GLushort> window(fft_size);
   double window_mod = 1.0 / kaiser_window(0.0, KAISER_BETA);
   for (int i = 0; i < int(fft_size); i++)
   {
      double phase = (double)(i - int(fft_size) / 2) / (int(fft_size) / 2);
      double w = kaiser_window(phase, KAISER_BETA);
      window[i] = round(0xffff * w * window_mod);
   }
   glBindTexture(GL_TEXTURE_2D, window_tex);
   glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fft_size, 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT, window.data());
   glBindTexture(GL_TEXTURE_2D, 0);

   GL_CHECK_ERROR();
   init_texture(input_tex, GL_RG16I, fft_size, 1);
   init_target(output, GL_RG32UI, fft_size, fft_depth, 1);
   init_target(resolve, GL_RGBA8, fft_size, fft_depth, 1);
   init_target(blur, GL_RGBA8, fft_size, fft_depth, log2i(max(fft_size, fft_depth)) + 1, GL_NEAREST, GL_LINEAR_MIPMAP_LINEAR);

   GL_CHECK_ERROR();
   for (unsigned i = 0; i < fft_steps; i++)
   {
      init_target(passes[i].target, GL_RG32UI, fft_size, 1, 1);
      init_texture(passes[i].parameter_tex, GL_RG32UI, fft_size, 1);

      std::vector<GLuint> param_buffer(2 * fft_size);
      build_fft_params(param_buffer.data(), i, fft_size);

      glBindTexture(GL_TEXTURE_2D, passes[i].parameter_tex);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
            fft_size, 1, GL_RG_INTEGER, GL_UNSIGNED_INT, param_buffer.data());
      glBindTexture(GL_TEXTURE_2D, 0);
   }

   GL_CHECK_ERROR();
   glGenBuffers(1, pbo.addr());
   glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
   glBufferData(GL_PIXEL_UNPACK_BUFFER, fft_size * 2 * sizeof(GLshort), 0, GL_DYNAMIC_DRAW);
   glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

void GLFFT::init_block()
{
   block.prog = compile_program(vertex_program_heightmap, fragment_program_heightmap);
   glUseProgram(block.prog);
   glUniform1i(glGetUniformLocation(block.prog, "sHeight"), 0);

   std::vector<GLushort> block_vertices(2 * fft_block_size * fft_depth);
   for (unsigned y = 0; y < fft_depth; y++)
   {
      for (unsigned x = 0; x < fft_block_size; x++)
      {
         block_vertices[2 * (y * fft_block_size + x) + 0] = x;
         block_vertices[2 * (y * fft_block_size + x) + 1] = y;
      }
   }
   glGenBuffers(1, &block.vbo);
   glBindBuffer(GL_ARRAY_BUFFER, block.vbo);
   glBufferData(GL_ARRAY_BUFFER, block_vertices.size() * sizeof(GLushort), block_vertices.data(), GL_STATIC_DRAW);

   block.elems = (2 * fft_block_size - 1) * (fft_depth - 1) + 1;
   std::vector<GLuint> block_indices(block.elems);
   GLuint *bp = block_indices.data();

   int pos = 0;
   for (int y = 0; y < int(fft_depth) - 1; y++)
   {
      int step_odd = -int(fft_block_size) + ((y & 1) ? -1 : 1);
      int step_even = fft_block_size;
      for (int x = 0; x < 2 * int(fft_block_size) - 1; x++)
      {
         *bp++ = pos;
         pos += (x & 1) ? step_odd : step_even;
      }
   }
   *bp++ = pos;

   glGenVertexArrays(1, &block.vao);
   glBindVertexArray(block.vao);

   glGenBuffers(1, &block.ibo);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, block.ibo);
   glBufferData(GL_ELEMENT_ARRAY_BUFFER, block_indices.size() * sizeof(GLuint), block_indices.data(), GL_STATIC_DRAW);

   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_UNSIGNED_SHORT, GL_FALSE, 0, 0);
   glBindVertexArray(0);

   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

GLFFT::GLFFT()
   : output_ptr(0), fft_steps(0), fft_size(0), fft_block_size(0), fft_depth(0)
{}

void GLFFT::context_reset(unsigned fft_steps, rglgen_proc_address_t proc, unsigned fft_depth)
{
   rglgen_resolve_symbols(proc);

   this->fft_steps = fft_steps;
   this->fft_depth = fft_depth;
   fft_size = 1 << fft_steps;
   fft_block_size = fft_size / 4 + 1;

   passes.resize(fft_steps);
   sliding.resize(2 * fft_size);

   GL_CHECK_ERROR();
   init_quad_vao();
   GL_CHECK_ERROR();
   init_fft();
   GL_CHECK_ERROR();
   init_block();
   GL_CHECK_ERROR();
}

void GLFFT::context_destroy()
{
   passes.clear();
   sliding.clear();
}

// GLFFT requires either GLES3 or desktop GL with ES3_compat (supported by MESA on Linux) extension.
glfft_t *glfft_new(unsigned fft_steps, rglgen_proc_address_t proc)
{
#ifdef HAVE_OPENGLES3
   const char *ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
   if (ver)
   {
      unsigned major, minor;
      if (sscanf(ver, "OpenGL ES %u.%u", &major, &minor) != 2 || major < 3)
         return NULL;
   }
   else
      return NULL;
#else
   const char *exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
   if (!exts || !strstr(exts, "ARB_ES3_compatibility"))
      return NULL;
#endif

   glfft_t *fft = new(std::nothrow) GLFFT;
   if (!fft)
      return NULL;
   fft->context_reset(fft_steps, proc);
   return fft;
}

void glfft_free(glfft_t *fft)
{
   fft->context_destroy();
   delete fft;
}

void glfft_step_fft(glfft_t *fft, GLshort *buffer, unsigned frames)
{
   fft->step_fft(buffer, frames);
}

void glfft_render(glfft_t *fft, GLuint backbuffer, unsigned width, unsigned height)
{
   fft->render(backbuffer, width, height);
}

