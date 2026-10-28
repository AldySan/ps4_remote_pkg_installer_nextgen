# Remote PKG Installer NextGen
# OpenOrbis PS4 Toolchain build
#
# Requirements:
#   - OpenOrbis PS4 Toolchain (OO_PS4_TOOLCHAIN env var)
#   - clang + lld on PATH
#   - python3 (for create-fself helper scripts)
#
# See README.md and .github/workflows/build.yml for a working CI recipe.

# Package metadata.
TITLE       := Remote PKG Installer NextGen
VERSION     := 0.0.1
TITLE_ID    := RPIN00001
CONTENT_ID  := IV0000-RPIN00001_00-RPINEXTGEN000000

# Libraries linked into the ELF.
LIBS        := -lc -lkernel -lc++ \
               -lSceSysmodule -lSceSystemService -lSceUserService \
               -lSceNetCtl -lSceNet -lSceSsl -lSceHttp -lSceNpCommon \
               -lSceAppInstUtil -lSceBgft \
               -lSceVideoOut -lSceAudioOut -lScePad -lSceFreeType -lSDL2

# Additional compile flags.
EXTRAFLAGS  := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O2 -g \
               -D__PS4__ -D_GNU_SOURCE \
               -DAPP_VERSION=\"$(VERSION)\"

# Third-party sources built into the ELF.
TPDIR       := thirdparty
TPSRCS      := $(TPDIR)/cJSON/cJSON.c $(TPDIR)/dbglogger/dbglogger.c

# Asset and module directories.
ASSETS      :=
LIBMODULES  :=

# You likely won't need to touch anything below this point.

# Root vars
TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
PROJDIR     := src
INTDIR      := build/obj
OUTDIR      := build

# Define objects to build (third-party objects are flattened: tp_<name>.o)
CFILES      := $(wildcard $(PROJDIR)/*.c)
OBJS        := $(patsubst $(PROJDIR)/%.c, $(INTDIR)/%.o, $(CFILES)) \
               $(foreach s,$(TPSRCS),$(INTDIR)/tp_$(notdir $(basename $(s))).o)

# Define final C flags
CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c $(EXTRAFLAGS) \
               -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include \
               -isystem $(TPDIR)/cJSON -isystem $(TPDIR)/dbglogger
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr \
               -Lthirdparty/stubs -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

# Create the intermediate directory in case it doesn't already exist.
_unused     := $(shell mkdir -p $(INTDIR))

# Check for linux vs macOS and account for clang/ld path
UNAME_S     := $(shell uname -s)

ifeq ($(UNAME_S),Linux)
	CC       := clang
	LD       := ld.lld
	CDIR     := linux
endif
ifeq ($(UNAME_S),Darwin)
	CC       := clang
	LD       := ld.lld
	CDIR     := macos
endif

.PHONY: all clean test

all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: pkg.gp4
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core pkg_build $< .

pkg.gp4: eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_sys/pic0.png sce_sys/pic1.png $(LIBMODULES) $(ASSETS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"

# SDK lib shim for create-fself (it searches $OO_PS4_TOOLCHAIN/lib before
# -library-path, so the custom stub must shadow the stock one there).
STUBSDK      := build/sdkstub

$(STUBSDK)/lib/libSceBgft.so: thirdparty/stubs/libSceBgft.so
	mkdir -p $(STUBSDK)/lib
	for f in $(TOOLCHAIN)/lib/*.so; do ln -sf $$f $(STUBSDK)/lib/; done
	ln -sf $(CURDIR)/thirdparty/stubs/libSceBgft.so $(STUBSDK)/lib/libSceBgft.so

# Custom libSceBgft stub (app-facing sceBgftDownload* symbols missing
# from the stock toolchain stub; see thirdparty/stubs/bgft_stub.c).
thirdparty/stubs/libSceBgft.so: $(TPDIR)/stubs/bgft_stub.c
	$(CC) --target=x86_64-pc-freebsd12-elf -fPIC -shared -nostdlib -o $@ $<

sce_sys/about/right.sprx:
	mkdir -p sce_sys/about
	printf 'SPRX' > $@

sce_sys/param.sfo: Makefile
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_new $@
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 32
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gde'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ PARENTAL_LEVEL --type Integer --maxsize 4 --value 1
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

eboot.bin: $(INTDIR) $(OBJS) thirdparty/stubs/libSceBgft.so $(STUBSDK)/lib/libSceBgft.so
	$(LD) $(INTDIR)/*.o -o $(OUTDIR)/$(notdir $(CURDIR)).elf $(LDFLAGS)
	OO_PS4_TOOLCHAIN=$(CURDIR)/$(STUBSDK) $(TOOLCHAIN)/bin/$(CDIR)/create-fself -in=$(OUTDIR)/$(notdir $(CURDIR)).elf -out=$(OUTDIR)/eboot.oelf --eboot "eboot.bin" -library-path thirdparty/stubs --paid 0x3800000000000011 --authinfo 000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000

$(INTDIR)/%.o: $(PROJDIR)/%.c
	$(CC) $(CFLAGS) -o $@ $<

define TP_RULE
$(INTDIR)/tp_$(notdir $(basename $(1))).o: $(1)
	$$(CC) $$(CFLAGS) -o $$@ $$<
endef
$(foreach s,$(TPSRCS),$(eval $(call TP_RULE,$(s))))

# Host unit tests for the portable core (util/sfo/pkg).
# HOSTCC can be overridden, e.g. make test HOSTCC=clang
HOSTCC ?= cc
TEST_SRCS := src/util.c src/sfo.c src/pkg.c tools/host_tests.c

.PHONY: test
test:
	$(HOSTCC) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O2 -Isrc \
		-o build/host_tests $(TEST_SRCS)
	./build/host_tests tests/data

clean:
	rm -f $(CONTENT_ID).pkg pkg.gp4 sce_sys/param.sfo eboot.bin \
		thirdparty/stubs/libSceBgft.so \
                $(OUTDIR)/*.elf $(OUTDIR)/*.oelf $(OBJS)
	rm -rf sce_sys/about $(INTDIR) $(STUBSDK)
