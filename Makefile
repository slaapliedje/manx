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
CFLAGS  := -DUB_SYSV4 $(CPU) -std=gnu99 -O2 -fomit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
# BearSSL: no /dev/urandom, no 64-bit fast paths (the 68030 has none)
BROPT   ?= -O2
BRFLAGS := $(CPU) $(BROPT) -fomit-frame-pointer \
	-DBR_USE_URANDOM=0 -DBR_USE_UNIX_TIME=1 -DBR_64=0 -DBR_INT128=0 -DBR_UMUL128=0
LDLIBS  := -lsocket
else
$(error TARGET must be host or sysv4)
endif

INC := -Ios -Inet -Itls -I$(BEARSSL)/inc

OS_SRC := $(wildcard $(OS)/*.c)
OS_OBJ := $(OS_SRC:%.c=$(B)/%.o)

BR_SRC := $(wildcard $(BEARSSL)/src/*/*.c) $(BEARSSL)/src/settings.c
BR_OBJ := $(BR_SRC:%.c=$(B)/%.o)
BR_LIB := $(B)/libbearssl.a

NET_OBJ := $(patsubst %.c,$(B)/%.o,$(wildcard net/*.c))
TLS_OBJ := $(patsubst %.c,$(B)/%.o,$(wildcard tls/*.c))

SPIKES := $(B)/tlsbench $(B)/ufetch

all: $(SPIKES)

$(B)/tlsbench: $(B)/spikes/tlsbench.o $(OS_OBJ) $(BR_LIB)
	$(LD) -o $@ $^ $(LDLIBS)

$(B)/ufetch: $(B)/spikes/ufetch.o $(NET_OBJ) $(TLS_OBJ) $(OS_OBJ) $(BR_LIB)
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

build/test/%: tests/%.c $(NET_OBJ) $(TLS_OBJ) $(OS_OBJ) $(BR_LIB)
	@mkdir -p build/test
	$(CC) $(CFLAGS) $(INC) -o $@ $^ $(LDLIBS)

build/test/test_snprintf: tests/test_snprintf.c os/sysv4/snprintf.c
	@mkdir -p build/test
	cc -std=c99 -Wall -Dvsnprintf=ub_vsnprintf -Dsnprintf=ub_snprintf \
		-c os/sysv4/snprintf.c -o build/test/snprintf.o
	cc -std=c99 -Wall -Wno-format -Wno-format-truncation \
		tests/test_snprintf.c build/test/snprintf.o -o $@

test: $(TESTS) build/test/test_snprintf $(B)/tlsbench
	@for t in $(TESTS) build/test/test_snprintf; do $$t || exit 1; done
	$(B)/tlsbench 50 kat

clean:
	rm -rf build

.PHONY: all clean test
-include $(shell find build -name '*.d' 2>/dev/null)
