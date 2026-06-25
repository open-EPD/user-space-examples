# Local build for the Open-EP Pet examples.
#
# Flags mirror ci/build-one.sh; CI (ci/discover-targets.py + GitHub Actions)
# remains the source of truth for the cross-arch board builds.
#
# This x86 dev host can natively build only the libgpiod v1 target (imx93).
# The rpi5 target needs libgpiod v2 headers/lib (build it on an rpi5 or in CI).
#
#   make            build every target
#   make imx93      build the frdm-imx93 (libgpiod v1) target
#   make rpi5       build the rpi5 (libgpiod v2) target
#   make clean      remove built binaries

CC     ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra \
          -Wno-error=incompatible-pointer-types \
          -Werror=implicit-function-declaration
LDLIBS ?= -lgpiod

COMMON := pet_app.h petsprite.h jpfont.h

IMX93  := pixpaper-213-m-test-frdm-imx93-pet
RPI5   := pixpaper-213-m-test-rpi5-pet

.PHONY: all imx93 rpi5 clean
all: $(IMX93) $(RPI5)
imx93: $(IMX93)
rpi5: $(RPI5)

%: %.c $(COMMON)
	$(CC) $(CFLAGS) -I. $< -o $@ $(LDLIBS)

clean:
	$(RM) $(IMX93) $(RPI5)
