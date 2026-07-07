# Nevertrustrobots Wabot — VCV Rack Plugin
# Set RACK_DIR to the path of your Rack SDK (local) or pass via env (CI)
RACK_DIR ?= $(HOME)/Documents/VCV_Dev/Rack-SDK

SLUG = NTRWabot

SOURCES += src/NTR_Plugin.cpp
SOURCES += src/NTR_Wabot.cpp

FLAGS += -fvisibility=hidden -fvisibility-inlines-hidden
FLAGS += -DNDEBUG
FLAGS := $(filter-out -g,$(FLAGS))

# macOS: detect SDK path dynamically (works locally and on GitHub Actions)
ifeq ($(shell uname -s),Darwin)
  SDK_PATH := $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)
  ifneq ($(SDK_PATH),)
    FLAGS += -isysroot $(SDK_PATH)
    FLAGS += -I$(SDK_PATH)/usr/include/c++/v1
  endif
endif

CFLAGS   +=
CXXFLAGS +=
LDFLAGS  +=

include $(RACK_DIR)/arch.mk

# httplib needs Winsock on Windows
ifdef ARCH_WIN
  LDFLAGS += -lws2_32
endif

include $(RACK_DIR)/plugin.mk
