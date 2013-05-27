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

   LIBS = $(shell pkg-config libavcodec libavformat libavutil libswscale libswresample libass --libs) -pthread
   CFLAGS += $(shell pkg-config libavcodec libavformat libavutil libswscale libswresample libass --cflags) -pthread
else ifeq ($(platform), osx)
   TARGET := $(TARGET_NAME)_libretro.dylib
   fpic := -fPIC
   SHARED := -dynamiclib
   GL_LIB := -framework OpenGL

   LIBS = $(shell pkg-config libavcodec libavformat libavutil libswscale libswresample libass --libs) -pthread
   CFLAGS += $(shell pkg-config libavcodec libavformat libavutil libswscale libswresample libass --cflags) -pthread
else
   CC = gcc
   TARGET := $(TARGET_NAME)_retro.dll
   SHARED := -shared -static-libgcc -static-libstdc++ -s -Wl,--version-script=link.T -Wl,--no-undefined
   GL_LIB := -lopengl32
   CFLAGS += -Iffmpeg
   LIBS += -L. -lavcodec -lavformat -lavutil -lswscale -lswresample -lass
endif

SOURCE := $(wildcard *.c)
OBJECTS := $(SOURCE:.c=.o)

CFLAGS += -Wall -std=gnu99 -pedantic $(fpic)

ifeq ($(DEBUG), 1)
   CFLAGS += -O0 -g
else
   CFLAGS += -O3
endif

ifeq ($(GLES), 1)
   LIBS += -lGLESv2
   CFLAGS += -DGLES
else
   LIBS += $(GL_LIB)
endif

all: $(TARGET)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $^ $(LIBS) $(SHARED)

clean:
	rm -f $(OBJECTS)
	rm -f $(TARGET)

.PHONY: clean

