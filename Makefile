TARGET := libretro-ffmpeg.so

SOURCE := $(wildcard *.c)
OBJECTS := $(SOURCE:.c=.o)

LIBS := $(shell pkg-config libavcodec libavformat libavutil libswscale --libs) -fPIC -shared -Wl,--no-undefined
CFLAGS += $(shell pkg-config libavcodec libavformat libavutil libswscale --cflags)
CFLAGS += -O3 -g -Wall -std=gnu99 -pedantic -fPIC

all: $(TARGET)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $^ $(LIBS)

clean:
	rm -f $(OBJECTS)
	rm -f $(TARGET)

.PHONY: clean

