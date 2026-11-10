# gnm-engine — Kodi PS4 GPU render engine probe (sceGnm + sceVideoOut).
#
# Builds a standalone package that draws a fullscreen textured quad with
# the Kodi splash image, entirely on the GPU, without Piglet/OpenGL.
#
# Requirements (host):
#   - OpenOrbis toolchain (OO_PS4_TOOLCHAIN)
#   - opengnm + its psbc (setup-psbc.sh creates them under /tmp/kodi-ps4)

TITLE=Kodi GNM Splash Engine
VERSION=0.10
TITLE_ID=FGNM00010
CONTENT_ID=IV0000-FGNM00010_00-GNMENGINE0000000

TOOLCHAIN=$(OO_PS4_TOOLCHAIN)
SHARED_PATH=shared
OPENGNM_PATH?=/tmp/kodi-ps4/opengnm
FREEGNM_PATH?=/tmp/kodi-ps4/freegnm

CC=clang
LD=ld.lld
GLSLC=glslc
PSBC=psbc
MKFSELF?=$(TOOLCHAIN)/bin/linux/create-fself
MKGP4?=$(TOOLCHAIN)/bin/linux/create-gp4
PKGTOOL?=$(TOOLCHAIN)/bin/linux/PkgTool.Core

SRCS=\
	$(SHARED_PATH)/src/u/fs.c \
	$(SHARED_PATH)/src/u/utility.c \
	$(SHARED_PATH)/src/displayctx.c \
	$(SHARED_PATH)/src/memalloc.c \
	$(SHARED_PATH)/src/misc.c \
	src/main.c
OBJS=$(SRCS:%.c=%.o)
EXE=gnm_engine

VERT_SRCS=assets/misc/quad.vert.glsl
FRAG_SRCS=assets/misc/quad.frag.glsl \
	assets/misc/clear.frag.glsl
VERT_SPVS=$(VERT_SRCS:%.glsl=%.spv)
VERT_PSBS=$(VERT_SPVS:%.spv=%.sb)
FRAG_SPVS=$(FRAG_SRCS:%.glsl=%.spv)
FRAG_PSBS=$(FRAG_SPVS:%.spv=%.sb)

BASE_CFLAGS=-std=c11 -Wall -Wextra -Wpedantic -D_BSD_SOURCE \
	-I$(SHARED_PATH)/src
BASE_LDFLAGS=-lc -lkernel -lSceGnmDriver -lSceVideoOut

OPENGNM_LIB=$(OPENGNM_PATH)/libopengnm.a
CFLAGS=$(BASE_CFLAGS) -DUSE_OPENGNM -I$(OPENGNM_PATH)/include -I$(FREEGNM_PATH)
LDFLAGS=$(BASE_LDFLAGS) $(OPENGNM_LIB)

ASSETS=$(VERT_PSBS) $(FRAG_PSBS) assets/images/splash.png
RUNTIME_MODULE_DIR?=$(TOOLCHAIN)/samples/graphics/sce_module
RUNTIME_MODULES=sce_module/libc.prx sce_module/libSceFios2.prx
PKG_FILES=eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png \
	$(RUNTIME_MODULES) $(ASSETS)

all: $(CONTENT_ID).pkg

%.o: %.c
	$(CC) --target=x86_64-ps4-elf -fPIC -c -isysroot $(TOOLCHAIN) \
		-isystem $(TOOLCHAIN)/include $(CFLAGS) -o $@ $<

$(OPENGNM_LIB):
	cp $(OPENGNM_PATH)/config.orbis.mak $(OPENGNM_PATH)/config.mak
	$(MAKE) -C $(OPENGNM_PATH) CC=clang lib

$(EXE): $(OBJS) $(OPENGNM_LIB)
	$(LD) -o $@ $(OBJS) -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x \
		--eh-frame-hdr -L$(TOOLCHAIN)/lib $(LDFLAGS) $(TOOLCHAIN)/lib/crt1.o \
		$(TOOLCHAIN)/lib/crti.o $(TOOLCHAIN)/lib/crtn.o

%.vert.spv: %.vert.glsl
	$(GLSLC) -fshader-stage=vertex $< -o $@
%.frag.spv: %.frag.glsl
	$(GLSLC) -fshader-stage=fragment $< -o $@
%.vert.sb: %.vert.spv
	$(PSBC) -s vertex -f $< -o $@
%.frag.sb: %.frag.spv
	$(PSBC) -s fragment -f $< -o $@

eboot.bin: $(EXE)
	$(MKFSELF) -in=$(EXE) -out=$(EXE).oelf -eboot=eboot.bin --paid 0x3800000000000011

sce_sys/icon0.png:
	mkdir -p sce_sys/about
	cp $(TOOLCHAIN)/samples/graphics/sce_sys/icon0.png $@
sce_sys/about/right.sprx:
	mkdir -p sce_sys/about
	cp $(TOOLCHAIN)/samples/graphics/sce_sys/about/right.sprx $@
sce_module/libc.prx:
	mkdir -p sce_module
	cp $(RUNTIME_MODULE_DIR)/libc.prx $@
sce_module/libSceFios2.prx:
	mkdir -p sce_module
	cp $(RUNTIME_MODULE_DIR)/libSceFios2.prx $@

sce_sys/param.sfo: Makefile
	mkdir -p sce_sys/about
	$(PKGTOOL) sfo_new $@
	$(PKGTOOL) sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(PKGTOOL) sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(PKGTOOL) sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(PKGTOOL) sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(PKGTOOL) sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(PKGTOOL) sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(PKGTOOL) sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

pkg.gp4: $(PKG_FILES)
	$(MKGP4) -out=$@ -content-id=$(CONTENT_ID) -files "$(PKG_FILES)"

$(CONTENT_ID).pkg: pkg.gp4
	$(PKGTOOL) pkg_build $< .

clean:
	rm -f $(OBJS) $(EXE) eboot.bin $(EXE).oelf $(VERT_SPVS) $(FRAG_SPVS) \
		pkg.gp4 *.pkg
