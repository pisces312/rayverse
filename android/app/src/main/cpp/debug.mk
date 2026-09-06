NDK_LOCAL_PATH := $(call my-dir)
GAME_PATH := $(NDK_LOCAL_PATH)/../../../../../..
SDL_PATH := $(GAME_PATH)/3rd/SDL
$(info NDK_LOCAL_PATH = $(NDK_LOCAL_PATH))
$(info GAME_PATH = $(GAME_PATH))
$(info SDL_PATH = $(SDL_PATH))
$(info SDL Android.mk exists = $(wildcard $(SDL_PATH)/Android.mk))

include $(SDL_PATH)/Android.mk
