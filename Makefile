# rtltcp, an rtl_tcp input module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later
#
#   make                        build/rtltcp, the native build
#   make static [ARCH=...]      build/static-ARCH/rtltcp, fully static
#   make package [ARCH=...]     dist/rtltcp-VERSION-linux-ARCH.fernmod
#   make test                   unit + fake rtl_tcp integration + cli/package checks
#
# ARCH is x86_64, aarch64 or armhf and defaults to the machine's own.

VERSION := 0.2.0

HOST_ARCH := $(shell uname -m | sed -e 's/^armv7.*/armhf/' -e 's/^arm64$$/aarch64/')
ARCH ?= $(HOST_ARCH)
ifeq ($(filter $(ARCH),x86_64 aarch64 armhf),)
$(error ARCH must be x86_64, aarch64 or armhf, not "$(ARCH)")
endif

CROSS_x86_64 := x86_64-linux-gnu-
CROSS_aarch64 := aarch64-linux-gnu-
CROSS_armhf := arm-linux-gnueabihf-
ifeq ($(ARCH),$(HOST_ARCH))
CROSS ?=
else
CROSS ?= $(CROSS_$(ARCH))
endif
ARCH_FLAGS_armhf := -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard
ARCH_FLAGS := $(ARCH_FLAGS_$(ARCH))

OPT ?= -O2
CXXFLAGS := -std=c++17 $(OPT) -g -pthread -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
	-DFERN_RTLTCP_VERSION='"$(VERSION)"'

SRCS := src/main.cpp
HDRS := src/json.h

# A package needs the --describe output of a build that runs on this machine,
# whatever ARCH is being packaged.
DESCRIBE_BIN := build/static-$(HOST_ARCH)/rtltcp

.PHONY: all static package test clean

all: build/rtltcp

build/rtltcp: $(SRCS) $(HDRS)
	@mkdir -p build
	$(CROSS)$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

static: build/static-$(ARCH)/rtltcp

build/static-$(ARCH)/rtltcp: $(SRCS) $(HDRS)
	@mkdir -p build/static-$(ARCH)
	$(CROSS)$(CXX) $(CXXFLAGS) $(ARCH_FLAGS) -static -s -Wl,--gc-sections -o $@ $(SRCS)

PACKAGE := dist/rtltcp-$(VERSION)-linux-$(ARCH).fernmod

package: $(PACKAGE)

$(PACKAGE): build/static-$(ARCH)/rtltcp $(DESCRIBE_BIN) tools/mkfernmod.py tools/check_fernmod.py
	@mkdir -p dist
	python3 tools/mkfernmod.py --executable build/static-$(ARCH)/rtltcp \
		--describe-with $(DESCRIBE_BIN) --platform linux-$(ARCH) --version $(VERSION) \
		--license GPL-2.0-or-later \
		--source https://github.com/vlig-alt/Fern-RTLTCP \
		--description "An rtl_tcp network source, including read-only OpenWebRX rtltcp_compat feeds" \
		--output $@
	python3 tools/check_fernmod.py $@

test: build/rtltcp
	python3 tests/fake_rtltcp_test.py build/rtltcp

clean:
	rm -rf build dist
