#ifndef GLSYM_H__
#define GLSYM_H__

#ifdef HAVE_GL
#include "libretro.h"

#define GL_GLEXT_PROTOTYPES
#if defined(GLES)
#ifdef IOS
#include <OpenGLES/ES2/gl.h>
#else
#include <GLES2/gl2.h>
#endif
#elif defined(__APPLE__)
#define GL3_PROTOTYPES
#include <OpenGL/gl3.h>
#include <OpenGL/glext.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#if !defined(GLES) && !defined(__APPLE__)
#define glCreateProgram pglCreateProgram
#define glCreateShader pglCreateShader
#define glCompileShader pglCompileShader
#define glUseProgram pglUseProgram
#define glShaderSource pglShaderSource
#define glAttachShader pglAttachShader
#define glLinkProgram pglLinkProgram
#define glBindFramebuffer pglBindFramebuffer
#define glGetUniformLocation pglGetUniformLocation
#define glUniformMatrix4fv pglUniformMatrix4fv
#define glUniform1i pglUniform1i
#define glUniform1f pglUniform1f
#define glGetAttribLocation pglGetAttribLocation
#define glVertexAttribPointer pglVertexAttribPointer
#define glEnableVertexAttribArray pglEnableVertexAttribArray
#define glDisableVertexAttribArray pglDisableVertexAttribArray
#define glGenBuffers pglGenBuffers
#define glBufferData pglBufferData
#define glBindBuffer pglBindBuffer
#define glMapBufferRange pglMapBufferRange
#define glActiveTexture pglActiveTexture
#define glUnmapBuffer pglUnmapBuffer

extern PFNGLCREATEPROGRAMPROC pglCreateProgram;
extern PFNGLCREATESHADERPROC pglCreateShader;
extern PFNGLCREATESHADERPROC pglCompileShader;
extern PFNGLCREATESHADERPROC pglUseProgram;
extern PFNGLSHADERSOURCEPROC pglShaderSource;
extern PFNGLATTACHSHADERPROC pglAttachShader;
extern PFNGLLINKPROGRAMPROC pglLinkProgram;
extern PFNGLBINDFRAMEBUFFERPROC pglBindFramebuffer;
extern PFNGLGETUNIFORMLOCATIONPROC pglGetUniformLocation;
extern PFNGLUNIFORMMATRIX4FVPROC pglUniformMatrix4fv;
extern PFNGLUNIFORM1IPROC pglUniform1i;
extern PFNGLUNIFORM1FPROC pglUniform1f;
extern PFNGLGETATTRIBLOCATIONPROC pglGetAttribLocation;
extern PFNGLVERTEXATTRIBPOINTERPROC pglVertexAttribPointer;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC pglEnableVertexAttribArray;
extern PFNGLDISABLEVERTEXATTRIBARRAYPROC pglDisableVertexAttribArray;
extern PFNGLGENBUFFERSPROC pglGenBuffers;
extern PFNGLBUFFERDATAPROC pglBufferData;
extern PFNGLBINDBUFFERPROC pglBindBuffer;
extern PFNGLMAPBUFFERRANGEPROC pglMapBufferRange;
extern PFNGLACTIVETEXTUREPROC pglActiveTexture;
extern PFNGLUNMAPBUFFERPROC pglUnmapBuffer;
#endif

void glsym_init_procs(retro_hw_get_proc_address_t cb);

#endif

#endif
