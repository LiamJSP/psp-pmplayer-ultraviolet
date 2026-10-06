TARGET = cooleyesBridge
OBJS = main.o sceAudio_driver3xx.o sceMeCore_driver3xx.o

INCDIR = ..
CFLAGS = -O2 -G0 -Wall -Wextra -Wno-unused-parameter
CXXFLAGS = -fno-exceptions -fno-rtti
ASFLAGS =

BUILD_PRX = 1
PRX_EXPORTS = cooleyesBridge.exp
USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1
PSP_FW_VERSION = 660
LDFLAGS = -nostartfiles
LIBS = -lpspdisplay_driver -lpspkernel

PSPSDK := $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
