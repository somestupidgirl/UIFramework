# Copyright (C) 2026, LibreDarwin
# SPDX-License-Identifier: BSD-3-Clause
# Clean-room reimplementation of Apple's UIFoundation.
#
#   make                 # build UIFoundation.framework for CONFIG (default release)
#   make CONFIG=debug    # debug build
#   make doc-parity      # byte-compare the .doc writer against /usr/bin/textutil
#   make clean
#
# Everything lands under build/.  The Makefile is portable to both GNU make and
# BSD make (bmake): no pattern rules, no ifeq/ifdef/.if conditionals and no
# $(if)/$(shell) functions.  Per-config flags come from make/<CONFIG>.mk.

CONFIG ?= release
SDK    ?= /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
CC     := /Users/sunneva/xnuports-root/devel/xcode-tools/build/release/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang

-include make/$(CONFIG).mk

BUILD_DIR := build/$(CONFIG)
OBJDIR    := $(BUILD_DIR)/obj

CFLAGS := $(OPT) -std=c11 -D_DARWIN_C_SOURCE -isysroot "$(SDK)" -Wall -Wextra

FRAMEWORK := $(BUILD_DIR)/UIFoundation.framework

DOCGEN_BIN  := $(BUILD_DIR)/docgen
DOCGEN_OBJS := $(OBJDIR)/docgen.o

OBJS := $(OBJDIR)/NSDocFormatWriter.o

all: $(FRAMEWORK) $(DOCGEN_BIN)

$(FRAMEWORK): $(OBJS)
	@mkdir -p $(FRAMEWORK)/Headers $(FRAMEWORK)/Modules $(FRAMEWORK)/Resources
	$(CC) $(CFLAGS) -dynamiclib \
	    -install_name @rpath/UIFoundation.framework/UIFoundation \
	    -compatibility_version 1 -current_version 1 -o $(FRAMEWORK)/UIFoundation $(OBJS)
	cp include/UIFoundation/NSDocFormatWriter.h $(FRAMEWORK)/Headers/NSDocFormatWriter.h
	cp src/module.modulemap $(FRAMEWORK)/Modules/module.modulemap
	cp src/Info.plist $(FRAMEWORK)/Resources/Info.plist

$(OBJDIR)/NSDocFormatWriter.o: src/NSDocFormatWriter.c \
	src/doc_head_0600.h src/doc_char_fkp_0200.h src/doc_para_fkp_0200.h \
	src/doc_desc_0200.h src/table1_nonempty_4096.h src/table1_empty_4096.h \
	src/si_4096.h src/dsi_4096.h src/doc_hdr_0050.h src/doc_dir_0280.h \
	include/UIFoundation/NSDocFormatWriter.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -Iinclude -Isrc -c -o $@ src/NSDocFormatWriter.c

$(DOCGEN_BIN): $(DOCGEN_OBJS) $(FRAMEWORK)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(DOCGEN_OBJS) -F$(BUILD_DIR) \
	    -framework UIFoundation -Wl,-rpath,@executable_path

$(OBJDIR)/docgen.o: tests/docgen.c include/UIFoundation/NSDocFormatWriter.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -Iinclude -c -o $@ tests/docgen.c

# Byte-parity check against Apple's /usr/bin/textutil.  Same MY/oracle
# convention as the other LibreDarwin tool harnesses; run with bash explicitly
# because the script is not POSIX-sh clean.
doc-parity: all
	MY="build/$(CONFIG)/docgen" ORACLE="/usr/bin/textutil" bash tests/doc-parity.sh

test: doc-parity

clean:
	rm -rf build

.PHONY: all doc-parity test clean
