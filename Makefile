# unix-browser
#
#   make                  host build (Linux): tests, fuzzing, development
#   make TARGET=sysv4     static AMIX binary; runs on AMIX and, through
#                         atari-sysv-sp1's amx module, on Atari System V
#
# TARGET=sysv4 needs AMIX_SYSROOT (atari-sysv-sp1 amix/mksysroot.sh), the
# mint GCC and the SVR4 binutils of gcc-cross-amix (see toolchain/sysv4-cc).

TARGET ?= host
B      := build/$(TARGET)

BEARSSL := third_party/bearssl

ifeq ($(TARGET),host)
CC      := cc
LD      := $(CC)
AR      := ar
OS      := os/host
CFLAGS  := -std=c99 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
BRFLAGS := -O2 -g
LDLIBS  :=
TERMLIB := -ltinfo
X11LIB  := -lX11
else ifeq ($(TARGET),sysv4)
AMIX_SYSROOT ?= $(HOME)/dev/OpenUA/data/work/asv/amix/sysroot
export AMIX_SYSROOT
ASV_CROSS ?= $(HOME)/opt/asv-cross
CC      := toolchain/sysv4-cc
LD      := toolchain/sysv4-ld
AR      := $(ASV_CROSS)/bin/m68k-cbm-sysv4-ar
OS      := os/sysv4
# -msoft-float: AMIX's libm returns doubles soft-float style, and nothing
# here needs the FPU
CPU     := -m68030 -msoft-float
OPT     ?= -O2
CFLAGS  := -DMANX_SYSV4 $(CPU) -std=gnu99 $(OPT) -fomit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
# BearSSL: no /dev/urandom, no 64-bit fast paths (the 68030 has none)
BROPT   ?= -O2
BRFLAGS := $(CPU) $(BROPT) -fomit-frame-pointer \
	-DBR_USE_URANDOM=0 -DBR_USE_UNIX_TIME=1 -DBR_64=0 -DBR_INT128=0 -DBR_UMUL128=0
LDLIBS  := -lsocket
TERMLIB := -ltermlib
X11LIB  := -lX11
else
$(error TARGET must be host or sysv4)
endif

INC := -Ios -Inet -Itls -Itext -Ihtml -Istyle -Ilayout -Iimage -Ifrontend -I$(BEARSSL)/inc

OS_SRC := $(wildcard os/*.c) $(wildcard $(OS)/*.c)
OS_OBJ := $(OS_SRC:%.c=$(B)/%.o)

BR_SRC := $(wildcard $(BEARSSL)/src/*/*.c) $(BEARSSL)/src/settings.c
BR_OBJ := $(BR_SRC:%.c=$(B)/%.o)
BR_LIB := $(B)/libbearssl.a

NET_OBJ := $(patsubst %.c,$(B)/%.o,$(wildcard net/*.c))
TLS_OBJ := $(patsubst %.c,$(B)/%.o,$(wildcard tls/*.c))
HTML_OBJ := $(patsubst %.c,$(B)/%.o,$(wildcard text/*.c html/*.c style/*.c layout/*.c))
IMG_OBJ := $(patsubst %.c,$(B)/%.o,$(wildcard image/*.c))

SPIKES := $(B)/tlsbench
TOOLS  := $(B)/manx $(B)/ufetch $(B)/manxtrust $(B)/uparse
FRONT_OBJ := $(patsubst %.c,$(B)/%.o,$(wildcard frontend/*.c))
# xmanx: the same browser in an X11 window (X11=0 to leave it out)
X11 ?= 1
X11_OBJ := $(patsubst %.c,$(B)/%.o,$(wildcard frontend/x11/*.c))
ifeq ($(X11),1)
TOOLS  += $(B)/xmanx
endif
BENCH  := $(B)/bench_parse $(B)/bench_micro $(B)/bench_loops $(B)/bench_mem \
	$(B)/bench_inflate $(B)/bench_image $(B)/imgconv

all: $(SPIKES) $(TOOLS) $(BENCH)

$(B)/tlsbench: $(B)/spikes/tlsbench.o $(OS_OBJ) $(BR_LIB)
	$(LD) -o $@ $^ $(LDLIBS)

$(B)/manx: $(B)/src/manx.o $(FRONT_OBJ) $(HTML_OBJ) $(NET_OBJ) $(TLS_OBJ) $(OS_OBJ) $(BR_LIB)
	$(LD) -o $@ $^ $(TERMLIB) $(LDLIBS)

$(B)/xmanx: $(B)/src/manx.o $(X11_OBJ) $(HTML_OBJ) $(NET_OBJ) $(TLS_OBJ) $(OS_OBJ) $(BR_LIB)
	$(LD) -o $@ $^ $(X11LIB) $(LDLIBS)

$(B)/ufetch: $(B)/src/ufetch.o $(NET_OBJ) $(TLS_OBJ) $(OS_OBJ) $(BR_LIB)
	$(LD) -o $@ $^ $(LDLIBS)

$(B)/uparse: $(B)/src/uparse.o $(HTML_OBJ) $(NET_OBJ) $(TLS_OBJ) $(OS_OBJ) $(BR_LIB)
	$(LD) -o $@ $^ $(LDLIBS)

$(B)/bench_parse: $(B)/tests/bench_parse.o $(HTML_OBJ) $(B)/net/url.o $(OS_OBJ)
	$(LD) -o $@ $^ $(LDLIBS)

$(B)/bench_micro: $(B)/tests/bench_micro.o $(B)/html/tags.o $(OS_OBJ)
	$(LD) -o $@ $^ $(LDLIBS)

$(B)/bench_loops: $(B)/tests/bench_loops.o $(OS_OBJ)
	$(LD) -o $@ $^ $(LDLIBS)

$(B)/bench_inflate: $(B)/tests/bench_inflate.o $(B)/net/inflate.o $(OS_OBJ)
	$(LD) -o $@ $^ $(LDLIBS)

$(B)/bench_image: $(B)/tests/bench_image.o $(IMG_OBJ) $(B)/net/inflate.o $(OS_OBJ)
	$(LD) -o $@ $^ $(LDLIBS)

$(B)/imgconv: $(B)/tests/imgconv.o $(IMG_OBJ) $(B)/net/inflate.o $(OS_OBJ)
	$(LD) -o $@ $^ $(LDLIBS)

$(B)/bench_mem: $(B)/tests/bench_mem.o $(OS_OBJ)
	$(LD) -o $@ $^ $(LDLIBS)

$(B)/manxtrust: $(B)/src/manxtrust.o $(NET_OBJ) $(TLS_OBJ) $(OS_OBJ) $(BR_LIB)
	$(LD) -o $@ $^ $(LDLIBS)

$(BR_LIB): $(BR_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

$(B)/$(BEARSSL)/%.o: $(BEARSSL)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BRFLAGS) -I$(BEARSSL)/inc -I$(BEARSSL)/src -c $< -o $@

$(B)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -MMD -c $< -o $@

# host-only unit tests: each tests/test_NAME.c links with the modules
# (test_snprintf is special: it tests the SVR4 snprintf against glibc's)
TESTS := $(filter-out build/test/test_snprintf,\
	$(patsubst tests/%.c,build/test/%,$(wildcard tests/test_*.c)))

build/test/%: tests/%.c $(HTML_OBJ) $(IMG_OBJ) $(NET_OBJ) $(TLS_OBJ) $(OS_OBJ) $(BR_LIB)
	@mkdir -p build/test
	$(CC) $(CFLAGS) $(INC) -o $@ $^ $(LDLIBS)

build/test/test_snprintf: tests/test_snprintf.c os/sysv4/snprintf.c
	@mkdir -p build/test
	cc -std=c99 -Wall -Dvsnprintf=ub_vsnprintf -Dsnprintf=ub_snprintf \
		-c os/sysv4/snprintf.c -o build/test/snprintf.o
	cc -std=c99 -Wall -Wno-format -Wno-format-truncation \
		tests/test_snprintf.c build/test/snprintf.o -o $@

# test_inflate decodes gzip/zlib made by Python from corpus pages (and
# stored/empty streams) when the corpus is there
INFLATE_PAGES = $(wordlist 1,6,$(wildcard build/corpus/*.html))

test: $(TESTS) build/test/test_snprintf $(B)/tlsbench
	@for t in $(filter-out build/test/test_inflate build/test/test_image,$(TESTS)) build/test/test_snprintf; do $$t || exit 1; done
	build/test/test_inflate $$(python3 tests/gen_deflate.py build/test/deflate $(INFLATE_PAGES))
	@mkdir -p build/test/img
	python3 tests/gen_images.py build/test/img > build/test/img/manifest
	build/test/test_image build/test/img
	$(B)/tlsbench 50 kat

# the HTML engine under AddressSanitizer + UBSan, fed mutated corpus pages
# (tests/fetch_corpus.sh first). FUZZ_ITERS, FUZZ_SEED to taste.
FUZZ_ITERS ?= 20000
FUZZ_SEED ?= 1
build/fuzz/fuzz_html: tests/fuzz_html.c $(wildcard text/*.c html/*.c style/*.c layout/*.c) os/mem.c os/host/os_time.c net/url.c
	@mkdir -p build/fuzz
	cc -std=c99 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
		-Ios -Itext -Ihtml -Istyle -Ilayout -Inet -o $@ $^

build/fuzz/fuzz_image: tests/fuzz_image.c $(wildcard image/*.c) net/inflate.c os/mem.c
	@mkdir -p build/fuzz
	cc -std=c99 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
		-Ios -Inet -Iimage -o $@ $^

fuzz: build/fuzz/fuzz_html build/fuzz/fuzz_image
	build/fuzz/fuzz_html $(FUZZ_ITERS) $(FUZZ_SEED) build/corpus/*.html
	@mkdir -p build/test/img
	python3 tests/gen_images.py build/test/img > build/test/img/manifest
	build/fuzz/fuzz_image $(FUZZ_ITERS) $(FUZZ_SEED) build/test/img

clean:
	rm -rf build

.PHONY: all clean test fuzz
-include $(shell find build -name '*.d' 2>/dev/null)
