ifeq ($(platform),)
platform = unix
ifeq ($(shell uname -a),)
   platform = win
else ifneq ($(findstring MINGW,$(shell uname -a)),)
   platform = win
else ifneq ($(findstring Darwin,$(shell uname -a)),)
   platform = osx
else ifneq ($(findstring win,$(shell uname -a)),)
   platform = win
endif
endif

TARGET_NAME := ffmpeg

ifneq (,$(findstring unix,$(platform)))
   TARGET := $(TARGET_NAME)_libretro.so
   fpic := -fPIC
   SHARED := -shared -Wl,--version-script=link.T -Wl,--no-undefined -fPIC
ifneq (,$(findstring opengl,$(platform)))
   GL_LIB := -lGL
   CFLAGS += -DHAVE_GL
   HAVE_GL := 1
endif
   HAVE_SSA := 1

   LIBS = $(shell pkg-config libavcodec libavformat libavutil libavdevice libswscale libswresample libass --libs) -pthread
   CFLAGS += $(shell pkg-config libavcodec libavformat libavutil libavdevice libswscale libswresample libass --cflags) -pthread
else ifneq (,$(findstring osx,$(platform)))
   TARGET := $(TARGET_NAME)_libretro.dylib
   fpic := -fPIC
   SHARED := -dynamiclib
ifneq (,$(findstring opengl,$(platform)))
   GL_LIB := -framework OpenGL
   HAVE_GL := 1
   CFLAGS += -DHAVE_GL
endif
   HAVE_SSA := 1
OSXVER = `sw_vers -productVersion | cut -c 4`
ifneq ($(OSXVER),9)
   fpic += -mmacosx-version-min=10.5
endif

   LIBS = $(shell pkg-config libavcodec libavformat libavutil libavdevice libswscale libswresample libass --libs) -pthread
   CFLAGS += $(shell pkg-config libavcodec libavformat libavutil libavdevice libswscale libswresample libass --cflags) -pthread
else ifneq (,$(findstring win,$(platform)))
   CC = gcc
   TARGET := $(TARGET_NAME)_libretro.dll
   SHARED := -shared -static-libgcc -static-libstdc++ -s -Wl,--version-script=link.T -Wl,--no-undefined
   CFLAGS += -Iffmpeg
ifneq (,$(findstring opengl,$(platform)))
   GL_LIB := -lopengl32
   CFLAGS += -DHAVE_GL
   HAVE_GL := 1
endif
   LIBS += -L. -Lffmpeg -lavcodec -lavformat -lavutil -lavdevice -lswscale -lswresample
endif

ifeq ($(HAVE_SSA), 1)
   LIBS += $(shell pkg-config libass --libs)
   CFLAGS += -DHAVE_SSA
endif

SOURCE := libretro.c fifo_buffer.c thread.c

CFLAGS += -Wall -std=gnu99 -pedantic $(fpic)

ifeq ($(DEBUG), 1)
   CFLAGS += -O0 -g
else
   CFLAGS += -O3
endif

ifeq ($(HAVE_GL), 1)
   ifeq ($(GLES), 1)
      LIBS += -lGLESv2
      CFLAGS += -DGLES
   else
      LIBS += $(GL_LIB)
   endif
   SOURCE += glsym.c
endif

OBJECTS := $(SOURCE:.c=.o)

all: $(TARGET)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $^ $(LIBS) $(SHARED)

clean:
	rm -f $(OBJECTS)
	rm -f $(TARGET)

.PHONY: clean

