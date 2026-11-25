# glshim-test — Kodi splash through the GLES subset shim over GNM.

TITLE=Kodi GLShim Splash Test
VERSION=0.10
TITLE_ID=FGNM00011
CONTENT_ID=IV0000-FGNM00011_00-GLSHIMTEST000000

TOOLCHAIN=$(OO_PS4_TOOLCHAIN)
SHARED_PATH=../shared
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
OBJS=$(SRCS:%.c=%.o) shim_glshim.o
EXE=glshim_test

BASE_CFLAGS=-std=c11 -Wall -Wextra -Wpedantic -D_BSD_SOURCE -I.. -I$(SHARED_PATH)/src
BASE_LDFLAGS=-lc -lkernel --no-as-needed \
	-lSceSysmodule -lSceSystemService -lSceShellCoreUtil -lSceRtc -lSceNet \
	-lSceGnmDriver -lSceVideoOut --as-needed

OPENGNM_LIB=$(OPENGNM_PATH)/libopengnm.a
CFLAGS=$(BASE_CFLAGS) -DUSE_OPENGNM -I$(OPENGNM_PATH)/include -I$(FREEGNM_PATH)
LDFLAGS=$(BASE_LDFLAGS) $(OPENGNM_LIB)

ASSETS=../assets/misc/quad.vert.sb ../assets/misc/quad.frag.sb \
	../assets/misc/clear.frag.sb ../assets/images/splash.png \
	assets/misc/kodi_vert.bin assets/misc/kodi_frag.bin \
	assets/misc/push.vert.sb
RUNTIME_MODULE_DIR?=$(TOOLCHAIN)/samples/graphics/sce_module
RUNTIME_MODULES=sce_module/libc.prx sce_module/libSceFios2.prx
PKG_FILES=eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png \
	$(RUNTIME_MODULES) assets/misc/quad.vert.sb assets/misc/quad.frag.sb \
	assets/misc/clear.frag.sb assets/images/splash.png \
	assets/misc/kodi_vert.bin assets/misc/kodi_frag.bin \
	assets/misc/push.vert.sb

all: $(CONTENT_ID).pkg

%.o: %.c
	$(CC) --target=x86_64-ps4-elf -fPIC -c -isysroot $(TOOLCHAIN) \
		-isystem $(TOOLCHAIN)/include $(CFLAGS) -o $@ $<

shim_glshim.o: ../glshim/glshim.c
	$(CC) --target=x86_64-ps4-elf -fPIC -c -isysroot $(TOOLCHAIN) \
		-isystem $(TOOLCHAIN)/include $(CFLAGS) -o $@ $<

$(EXE): $(OBJS) $(OPENGNM_LIB)
	$(LD) -o $@ $(OBJS) -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x \
		--eh-frame-hdr -L$(TOOLCHAIN)/lib $(LDFLAGS) $(TOOLCHAIN)/lib/crt1.o \
		$(TOOLCHAIN)/lib/crti.o $(TOOLCHAIN)/lib/crtn.o

$(OPENGNM_LIB):
	cp $(OPENGNM_PATH)/config.orbis.mak $(OPENGNM_PATH)/config.mak
	$(MAKE) -C $(OPENGNM_PATH) CC=clang lib

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
assets/misc/quad.vert.sb assets/misc/quad.frag.sb assets/misc/clear.frag.sb:
	mkdir -p assets/misc
	cp ../assets/misc/$(notdir $@) $@
assets/images/splash.png:
	mkdir -p assets/images
	cp ../assets/images/splash.png $@
assets/misc/kodi_vert.bin assets/misc/kodi_frag.bin:
	mkdir -p assets/misc
	cp assets/misc/$(notdir $@) $@ 2>/dev/null || true
assets/misc/push.vert.sb:
	mkdir -p assets/misc
	cp ../assets/misc/push.vert.sb $@

staging: $(PKG_FILES)
	mkdir -p assets/misc assets/images sce_sys/about sce_module
	cp ../assets/misc/quad.vert.sb ../assets/misc/quad.frag.sb assets/misc/
	cp ../assets/misc/clear.frag.sb assets/misc/
	cp ../assets/misc/push.vert.sb assets/misc/
	cp ../assets/images/splash.png assets/images/
	cp $(TOOLCHAIN)/samples/graphics/sce_sys/icon0.png sce_sys/icon0.png
	cp $(TOOLCHAIN)/samples/graphics/sce_sys/about/right.sprx sce_sys/about/
	cp $(RUNTIME_MODULE_DIR)/libc.prx $(RUNTIME_MODULE_DIR)/libSceFios2.prx sce_module/

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

pkg.gp4: staging $(PKG_FILES)
	$(MKGP4) -out=$@ -content-id=$(CONTENT_ID) -files "$(PKG_FILES)"

$(CONTENT_ID).pkg: pkg.gp4
	$(PKGTOOL) pkg_build $< .

clean:
	rm -f $(OBJS) $(EXE) eboot.bin $(EXE).oelf pkg.gp4 *.pkg
	rm -rf assets sce_sys sce_module
