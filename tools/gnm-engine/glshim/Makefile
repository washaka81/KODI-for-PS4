# libglshim.a — GLES2/EGL over sceGnm, linked by the Kodi PS4 port.
TOOLCHAIN?=$(OO_PS4_TOOLCHAIN)
SHARED_PATH=../shared
OPENGNM_PATH?=/tmp/kodi-ps4/opengnm
FREEGNM_PATH?=/tmp/kodi-ps4/freegnm

CC=clang
AR=ar
CFLAGS=-std=c11 -Wall -Wextra -Wno-unused-parameter -D_BSD_SOURCE \
	-I.. -I$(SHARED_PATH)/src -DUSE_OPENGNM -DGLSHIM_KODI -I$(OPENGNM_PATH)/include \
	-I$(FREEGNM_PATH)
SRCS=glshim.c eglshim.c \
	$(SHARED_PATH)/src/u/fs.c \
	$(SHARED_PATH)/src/u/utility.c \
	$(SHARED_PATH)/src/displayctx.c \
	$(SHARED_PATH)/src/memalloc.c \
	$(SHARED_PATH)/src/misc.c
OBJS=$(SRCS:%.c=%.o)
LIB=libglshim.a

all: $(LIB)

%.o: %.c
	$(CC) --target=x86_64-ps4-elf -fPIC -c -isysroot $(TOOLCHAIN) \
		-isystem $(TOOLCHAIN)/include $(CFLAGS) -o $@ $<

$(OPENGNM_PATH)/libopengnm.a:
	cp $(OPENGNM_PATH)/config.orbis.mak $(OPENGNM_PATH)/config.mak
	$(MAKE) -C $(OPENGNM_PATH) CC=clang lib

$(LIB): $(OBJS) $(OPENGNM_PATH)/libopengnm.a
	rm -f $@
	$(AR) rcs $@ $(OBJS)

clean:
	rm -f $(OBJS) $(LIB)

.PHONY: all clean
