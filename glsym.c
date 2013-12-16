#include "glsym.h"
#include <string.h>
#include <stdio.h>
#include "libretro.h"

extern retro_log_printf_t log_cb;

#if !defined(GLES) && !defined(__APPLE__)
PFNGLCREATEPROGRAMPROC pglCreateProgram;
PFNGLCREATESHADERPROC pglCreateShader;
PFNGLCREATESHADERPROC pglCompileShader;
PFNGLCREATESHADERPROC pglUseProgram;
PFNGLSHADERSOURCEPROC pglShaderSource;
PFNGLATTACHSHADERPROC pglAttachShader;
PFNGLLINKPROGRAMPROC pglLinkProgram;
PFNGLBINDFRAMEBUFFERPROC pglBindFramebuffer;
PFNGLGETUNIFORMLOCATIONPROC pglGetUniformLocation;
PFNGLUNIFORMMATRIX4FVPROC pglUniformMatrix4fv;
PFNGLUNIFORM1IPROC pglUniform1i;
PFNGLUNIFORM1FPROC pglUniform1f;
PFNGLGETATTRIBLOCATIONPROC pglGetAttribLocation;
PFNGLVERTEXATTRIBPOINTERPROC pglVertexAttribPointer;
PFNGLENABLEVERTEXATTRIBARRAYPROC pglEnableVertexAttribArray;
PFNGLDISABLEVERTEXATTRIBARRAYPROC pglDisableVertexAttribArray;
PFNGLGENBUFFERSPROC pglGenBuffers;
PFNGLBUFFERDATAPROC pglBufferData;
PFNGLBINDBUFFERPROC pglBindBuffer;
PFNGLMAPBUFFERRANGEPROC pglMapBufferRange;
PFNGLACTIVETEXTUREPROC pglActiveTexture;
PFNGLUNMAPBUFFERPROC pglUnmapBuffer;

struct gl_proc_map
{
   void *proc;
   const char *sym;
};

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PROC_BIND(name) { &(pgl##name), "gl" #name }
static const struct gl_proc_map proc_map[] = {
   PROC_BIND(CreateProgram),
   PROC_BIND(CreateShader),
   PROC_BIND(CompileShader),
   PROC_BIND(UseProgram),
   PROC_BIND(ShaderSource),
   PROC_BIND(AttachShader),
   PROC_BIND(LinkProgram),
   PROC_BIND(BindFramebuffer),
   PROC_BIND(GetUniformLocation),
   PROC_BIND(GetAttribLocation),
   PROC_BIND(UniformMatrix4fv),
   PROC_BIND(Uniform1i),
   PROC_BIND(Uniform1f),
   PROC_BIND(VertexAttribPointer),
   PROC_BIND(EnableVertexAttribArray),
   PROC_BIND(DisableVertexAttribArray),
   PROC_BIND(GenBuffers),
   PROC_BIND(BufferData),
   PROC_BIND(BindBuffer),
   PROC_BIND(MapBufferRange),
   PROC_BIND(ActiveTexture),
   PROC_BIND(UnmapBuffer),
};

void glsym_init_procs(retro_hw_get_proc_address_t cb)
{
   for (unsigned i = 0; i < ARRAY_SIZE(proc_map); i++)
   {
      retro_proc_address_t proc = cb(proc_map[i].sym);
      if (!proc && log_cb)
         log_cb(RETRO_LOG_ERROR, "Symbol %s not found!\n", proc_map[i].sym);
      memcpy(proc_map[i].proc, &proc, sizeof(proc));
   }
}
#else
void glsym_init_procs(retro_hw_get_proc_address_t cb)
{
   (void)cb;
}
#endif

