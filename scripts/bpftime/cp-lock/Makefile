# Standalone build for the cp_config_lock bpftime instrumentation.
# Not part of the repo's meson/make build: this is an opt-in diagnostic,
# built and run by hand.
#
# libbpf is picked up via pkg-config by default (the usual libbpf-dev
# package). On a system without it, point LIBBPF_DIR at a prefix that
# has include/bpf/libbpf.h and a static archive under lib/ or lib64/,
# e.g. a libbpf built from source: `make LIBBPF_DIR=/path/to/libbpf/install`.

# Plain `?=` would not win here: make's builtin rules already give CC a
# default of "cc", so `CC ?= clang` would never take effect. `make
# CC=gcc` from the command line still overrides this.
CC := clang
# Separate from CC: a BPF compile always needs clang's own -target bpf
# support, regardless of whatever CC a caller points at the ordinary C
# compiles below (e.g. `make CC=gcc`).
BPF_CLANG ?= clang
PKG_CONFIG ?= pkg-config

LIBBPF_DIR ?=
ifeq ($(LIBBPF_DIR),)
LIBBPF_CFLAGS := $(shell $(PKG_CONFIG) --cflags libbpf 2>/dev/null)
LIBBPF_LIBDIR := $(shell $(PKG_CONFIG) --variable=libdir libbpf 2>/dev/null)
LIBBPF_STATIC := $(LIBBPF_LIBDIR)/libbpf.a
else
LIBBPF_CFLAGS := -I$(LIBBPF_DIR)/include
# Some libbpf installs (e.g. built via CMake/autotools with a 64-bit
# libdir convention) use lib64/ instead of lib/.
LIBBPF_STATIC := $(firstword $(wildcard $(LIBBPF_DIR)/lib/libbpf.a $(LIBBPF_DIR)/lib64/libbpf.a))
endif

# Static linking avoids a dynamic libbpf.so dragging in a newer glibc
# symbol version than the process bpftime loads it into provides.
ifeq ($(wildcard $(LIBBPF_STATIC)),)
LIBBPF_LIBS := -lbpf -lelf -lz
else
LIBBPF_LIBS := $(LIBBPF_STATIC) -lelf -lz
endif

CFLAGS ?= -O2 -g -Wall -Wextra
BPF_CFLAGS := -O2 -g -target bpf -D__TARGET_ARCH_x86 -Wall -Wextra $(LIBBPF_CFLAGS)

# clang's -target bpf does not pick up the host's own multiarch header
# path the way a native compile does; added only when that directory
# actually exists, since not every distribution uses the multiarch
# layout.
MULTIARCH_INCLUDE_DIR := /usr/include/$(shell uname -m)-linux-gnu
ifneq ($(wildcard $(MULTIARCH_INCLUDE_DIR)),)
BPF_CFLAGS += -I$(MULTIARCH_INCLUDE_DIR)
endif

# Site-table capacity for the selftest's table-full leg only; the real
# default (cp_lock.h) is unaffected.
CP_LOCK_TEST_MAX_SITES ?= 2

.PHONY: all selftest clean

all: cp_lock.bpf.o collector

cp_lock.bpf.o: cp_lock.bpf.c cp_lock.h
	$(BPF_CLANG) $(BPF_CFLAGS) -c $< -o $@

selftest/cp_lock_overflow_test.bpf.o: cp_lock.bpf.c cp_lock.h
	$(BPF_CLANG) $(BPF_CFLAGS) -DCP_LOCK_MAX_SITES=$(CP_LOCK_TEST_MAX_SITES) -c $< -o $@

collector: collector.c elf_util.c elf_util.h cp_lock.h
	# -Wno-visibility: some libbpf header snapshots declare a function
	# taking a struct they never define in that header set, which is
	# a defect in that header, not in this code.
	$(CC) $(CFLAGS) -Wno-visibility $(LIBBPF_CFLAGS) collector.c elf_util.c -o $@ $(LIBBPF_LIBS)

selftest/workload: selftest/workload.c
	# -fno-unroll-loops: the try_lock attempt loop must stay a single
	# call site, or the compiler can unroll it into several distinct
	# return addresses and the selftest sees several rows where it
	# expects one.
	$(CC) $(CFLAGS) -fno-unroll-loops -pthread $< -o $@

selftest: all selftest/workload selftest/cp_lock_overflow_test.bpf.o
	./selftest/run.sh

clean:
	rm -f cp_lock.bpf.o collector selftest/workload
	rm -f selftest/cp_lock_overflow_test.bpf.o
