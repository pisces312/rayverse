NDK_LOCAL_PATH := $(call my-dir)

# Compute paths before any include changes $(call my-dir)
# From android/app/src/main/cpp/ up 5 levels = rayverse/
GAME_PATH := $(NDK_LOCAL_PATH)/../../../../..
SDL_PATH := $(GAME_PATH)/3rd/SDL

# Tell ndk-build where to find SDL2 as a module
$(call import-add-path,$(GAME_PATH)/3rd)

# Build SDL2
include $(SDL_PATH)/Android.mk

# Now restore our path (my-dir got clobbered by SDL's Android.mk)
LOCAL_PATH := $(NDK_LOCAL_PATH)

include $(CLEAR_VARS)
LOCAL_MODULE := main

# rayverse is a unity build: rayverse.c #includes all other .c files
GAME_SRC := $(GAME_PATH)/src
LOCAL_C_INCLUDES := $(LOCAL_PATH) $(GAME_SRC) $(SDL_PATH)/include
LOCAL_SRC_FILES := $(GAME_SRC)/rayverse.c $(LOCAL_PATH)/android_jni.c $(LOCAL_PATH)/android_fileio.c

LOCAL_CFLAGS += -std=gnu99 -D_GNU_SOURCE=1 -DANDROID=1 -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable -Wno-unused-function -Wno-missing-declarations -Wno-implicit-function-declaration
LOCAL_SHARED_LIBRARIES := SDL2
LOCAL_LDLIBS := -lGLESv1_CM -lGLESv2 -lOpenSLES -llog -landroid -ldl

include $(BUILD_SHARED_LIBRARY)
