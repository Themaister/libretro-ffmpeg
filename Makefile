TARGET := libretro-ffmpeg.so

SOURCE := $(wildcard *.c)
OBJECTS := $(SOURCE:.c=.o)

LIBS := $(shell pkg-config libavcodec libavformat libavutil libswscale libswresample glew --libs) -fPIC -shared -Wl,--no-undefined -pthread -lGL
CFLAGS += $(shell pkg-config libavcodec libavformat libavutil libswscale libswresample glew --cflags) -pthread
CFLAGS += -Wall -std=gnu99 -pedantic -fPIC

ifeq ($(DEBUG), 1)
   CFLAGS += -O0 -g
else
   CFLAGS += -O2
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

