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

ifeq ($(platform), unix)
   TARGET := $(TARGET_NAME)_libretro.so
   fpic := -fPIC
   SHARED := -shared -Wl,--version-script=link.T -Wl,--no-undefined -fPIC
   GL_LIB := -lGL
   HAVE_SSA := 1

   LIBS = $(shell pkg-config libavcodec libavformat libavutil libavdevice libswscale libswresample libass --libs) -pthread
   CFLAGS += $(shell pkg-config libavcodec libavformat libavutil libavdevice libswscale libswresample libass --cflags) -pthread
   CFLAGS += -DHAVE_GL
   HAVE_GL := 1
else ifeq ($(platform), unix-sw)
   TARGET := $(TARGET_NAME)_sw_libretro.so
   fpic := -fPIC
   SHARED := -shared -Wl,--version-script=link.T -Wl,--no-undefined -fPIC
   HAVE_SSA := 1

   LIBS = $(shell pkg-config libavcodec libavformat libavutil libavdevice libswscale libswresample libass --libs) -pthread
   CFLAGS += $(shell pkg-config libavcodec libavformat libavutil libavdevice libswscale libswresample libass --cflags) -pthread
else ifeq ($(platform), osx)
   TARGET := $(TARGET_NAME)_libretro.dylib
   fpic := -fPIC
   SHARED := -dynamiclib
   GL_LIB := -framework OpenGL
   HAVE_SSA := 1
   HAVE_GL := 1

   LIBS = $(shell pkg-config libavcodec libavformat libavutil libavdevice libswscale libswresample libass --libs) -pthread
   CFLAGS += $(shell pkg-config libavcodec libavformat libavutil libavdevice libswscale libswresample libass --cflags) -pthread
   CFLAGS += -DHAVE_GL
else ifeq ($(platform), osx-sw)
   TARGET := $(TARGET_NAME)_sw_libretro.dylib
   fpic := -fPIC
   SHARED := -dynamiclib
   HAVE_SSA := 1

   LIBS = $(shell pkg-config libavcodec libavformat libavutil libavdevice libswscale libswresample libass --libs) -pthread
   CFLAGS += $(shell pkg-config libavcodec libavformat libavutil libavdevice libswscale libswresample libass --cflags) -pthread
else ifeq ($(platform), win-sw)
   CC = gcc
   TARGET := $(TARGET_NAME)_sw_libretro.dll
   SHARED := -shared -static-libgcc -static-libstdc++ -s -Wl,--version-script=link.T -Wl,--no-undefined
   CFLAGS += -Iffmpeg
   LIBS += -L. -Lffmpeg -lavcodec -lavformat -lavutil -lavdevice -lswscale -lswresample
else
   CC = gcc
   TARGET := $(TARGET_NAME)_libretro.dll
   SHARED := -shared -static-libgcc -static-libstdc++ -s -Wl,--version-script=link.T -Wl,--no-undefined
   GL_LIB := -lopengl32
   CFLAGS += -Iffmpeg
   LIBS += -L. -Lffmpeg -lavcodec -lavformat -lavutil -lavdevice -lswscale -lswresample
   CFLAGS += -DHAVE_GL
   HAVE_GL := 1
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

