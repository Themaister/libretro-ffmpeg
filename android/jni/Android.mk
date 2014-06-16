LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := avcodec
LOCAL_SRC_FILES := ../prebuilt/libavcodec.a
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/../includes
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := avformat
LOCAL_SRC_FILES := ../prebuilt/libavformat.a
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/../includes
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := avutil
LOCAL_SRC_FILES := ../prebuilt/libavutil.a
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/../includes
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := swscale
LOCAL_SRC_FILES := ../prebuilt/libswscale.a
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/../includes
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := swresample
LOCAL_SRC_FILES := ../prebuilt/libswresample.a
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/../includes
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := glfft
LOCAL_ARM_MODE := arm
LOCAL_CFLAGS += -Wall -DHAVE_OPENGLES2 -DGLES -DHAVE_OPENGLES3 -fno-strict-aliasing
LOCAL_SRC_FILES := ../../fft/fft.cpp
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := retro_ffmpeg
LOCAL_ARM_MODE := arm
LOCAL_CFLAGS += -std=gnu99 -Wall -DHAVE_OPENGLES2 -DGLES -DHAVE_OPENGLES3 -DHAVE_GL -DHAVE_GL_FFT
LOCAL_LDLIBS := -llog -lz -lGLESv3 -lEGL
LOCAL_SRC_FILES := ../../libretro.c ../../thread.c ../../fifo_buffer.c ../../glsym/glsym_es2.c ../../glsym/rglgen.c
LOCAL_STATIC_LIBRARIES := glfft avformat avcodec avutil swscale swresample
include $(BUILD_SHARED_LIBRARY)

