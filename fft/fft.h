#ifndef FFT_H__
#define FFT_H__

#include "../glsym/glsym.h"

#ifdef __cplusplus
#include "fft.hpp"
extern "C" {
#endif

typedef struct GLFFT glfft_t;

glfft_t *glfft_new(unsigned fft_steps, rglgen_proc_address_t proc);
void glfft_free(glfft_t *fft);

void glfft_step_fft(glfft_t *fft, const GLshort *buffer, unsigned frames);
void glfft_render(glfft_t *fft, GLuint backbuffer, unsigned width, unsigned height);

#ifdef __cplusplus
}
#endif

#endif

