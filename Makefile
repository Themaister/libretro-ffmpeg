TARGET := ffmpeg_libretro.so

SOURCE := $(wildcard *.c)
OBJECTS := $(SOURCE:.c=.o)

LIBS = $(shell pkg-config libavcodec libavformat libavutil libswscale libswresample libass --libs) -fPIC -shared -Wl,--no-undefined -pthread
CFLAGS += $(shell pkg-config libavcodec libavformat libavutil libswscale libswresample libass --cflags) -pthread
CFLAGS += -Wall -std=gnu99 -pedantic -fPIC

ifeq ($(DEBUG), 1)
   CFLAGS += -O0 -g
else
   CFLAGS += -O2
endif

ifeq ($(GLES), 1)
   LIBS += -lGLESv2
   CFLAGS += -DGLES
else
   LIBS += -lGL
endif

all: $(TARGET)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $^ $(LIBS)

clean:
	rm -f $(OBJECTS)
	rm -f $(TARGET)

.PHONY: clean

