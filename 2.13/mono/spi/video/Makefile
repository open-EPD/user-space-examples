# Local build for the 2.13" mono video player.

CC     ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra \
          -Wno-error=incompatible-pointer-types \
          -Werror=implicit-function-declaration
LDLIBS ?= -lgpiod

COMMON := video_app.h
RPI5   := pixpaper-213-m-test-rpi5-video

.PHONY: all clean
all: $(RPI5)

$(RPI5): $(RPI5).c $(COMMON)
	@grep -qs gpiod_chip_request_lines /usr/include/gpiod.h || { \
	    echo "this player needs libgpiod v2 headers; /usr/include/gpiod.h is $(if $(wildcard /usr/include/gpiod.h),v1,missing) -- apt install libgpiod-dev" >&2; \
	    exit 1; }
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -I. $< -o $@ $(LDLIBS)

clean:
	$(RM) $(RPI5)
