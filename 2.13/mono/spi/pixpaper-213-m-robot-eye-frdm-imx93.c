/*
 * Robot eye demo for GDEY0213B74 (SSD1680) 2.13" mono EPD, FRDM-iMX93.
 *
 * The panel is behind a bezel with a 25.2 mm diameter round opening
 * centered on the active area. Pixel pitch is ~0.194 mm, so the visible
 * region is a circle of radius ~65 px centered at (x=125, y=61).
 *
 * Draws a single robot eye (big pupil + glint) inside that circle.
 * Gaze moves with eased interpolation, blinks use a lens-shaped
 * (elliptic) eyelid aperture. Animation runs on partial updates
 * (0x22 = 0xFF) with the fast waveform init; a full refresh runs
 * periodically to clear ghosting.
 *
 * Build (target): gcc -o robot-eye pixpaper-213-m-robot-eye-frdm-imx93.c -lgpiod -lm
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <fcntl.h>
#include <gpiod.h>
#include <linux/spi/spidev.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define EPD_SPI_DEVICE "/dev/spidev0.0"
#define EPD_GPIO_CHIP "gpiochip0"

#define EPD_DC_PIN 0
#define EPD_RST_PIN 5
#define EPD_BUSY_PIN 26

#define SPI_SPEED 5000000

#define EPD_W 250 /* long axis (gate lines) */
#define EPD_H 122 /* short axis (source, 16 bytes/line) */

/* bezel opening: 25.2 mm / 0.194 mm/px / 2 ~= 65 px */
#define EYE_CX 125
#define EYE_CY 61
#define R_FIELD 62  /* white eye field */
#define R_PUPIL 32
#define R_GLINT 9
#define GAZE_MAX_X 26 /* R_PUPIL + gaze <= R_FIELD keeps a white rim */
#define GAZE_MAX_Y 14 /* eyes mostly move horizontally */

#define FULL_REFRESH_EVERY 60 /* partial frames between ghost-clearing full refreshes */

int spi_fd;
struct gpiod_chip *chip;
struct gpiod_line *epd_dc_line, *epd_rst_line, *epd_busy_line;

static uint8_t fb[EPD_H * EPD_W]; /* 0xFF = white, 0x00 = black */
static uint8_t packed[EPD_W * 16];
static int frames;

void sleep_ms(unsigned int milliseconds) {
	struct timespec ts;
	ts.tv_sec = milliseconds / 1000;
	ts.tv_nsec = (milliseconds % 1000) * 1000000;
	nanosleep(&ts, NULL);
}

void sleep_us(unsigned int microseconds) {
	struct timespec ts;
	ts.tv_sec = microseconds / 1000000;
	ts.tv_nsec = (microseconds % 1000000) * 1000;
	nanosleep(&ts, NULL);
}

void spi_write(uint8_t *data, int len) {
	struct spi_ioc_transfer tr = {
	    .tx_buf = (unsigned long)data,
	    .len = len,
	    .speed_hz = SPI_SPEED,
	    .bits_per_word = 8,
	};
	ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr);
}

void epd_writeCommand(uint8_t command) {
	gpiod_line_set_value(epd_dc_line, 0);
	sleep_us(1);
	spi_write(&command, 1);
}

void epd_writeData(uint8_t data) {
	gpiod_line_set_value(epd_dc_line, 1);
	sleep_us(1);
	spi_write(&data, 1);
}

/* bulk data write: one DC toggle, chunked SPI transfers */
void epd_writeDataBuf(const uint8_t *data, int len) {
	gpiod_line_set_value(epd_dc_line, 1);
	sleep_us(1);
	while (len > 0) {
		int n = len > 2048 ? 2048 : len;
		spi_write((uint8_t *)data, n);
		data += n;
		len -= n;
	}
}

void epd_waitUntilIdle() {
	sleep_ms(2);
	while (1) {
		if (gpiod_line_get_value(epd_busy_line) == 0)
			break;
	}
}

void epd_HWreset() {
	sleep_ms(50);
	gpiod_line_set_value(epd_rst_line, 0);
	sleep_ms(50);
	gpiod_line_set_value(epd_rst_line, 1);
	sleep_ms(50);
}

void epd_init(void) {
	epd_HWreset();
	sleep_ms(100);
	epd_waitUntilIdle();

	epd_writeCommand(0x12); /* SW reset */
	epd_waitUntilIdle();

	epd_writeCommand(0x01); /* driver output control */
	epd_writeData(0xF9);
	epd_writeData(0x00);
	epd_writeData(0x00);

	epd_writeCommand(0x11); /* data entry: Y dec, X inc */
	epd_writeData(0x01);

	epd_writeCommand(0x44); /* X window 0..15 */
	epd_writeData(0x00);
	epd_writeData(0x0F);

	epd_writeCommand(0x45); /* Y window 249..0 */
	epd_writeData(0xF9);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData(0x00);

	epd_writeCommand(0x3C); /* border */
	epd_writeData(0x05);

	epd_writeCommand(0x21);
	epd_writeData(0x00);
	epd_writeData(0x80);

	epd_writeCommand(0x18); /* internal temp sensor */
	epd_writeData(0x80);

	/* fast waveform: fake a high temperature so the OTP LUT picked is
	 * the short one — shortens full and partial updates alike */
	epd_writeCommand(0x22);
	epd_writeData(0xB1);
	epd_writeCommand(0x20);
	epd_waitUntilIdle();

	epd_writeCommand(0x1A);
	epd_writeData(0x64);
	epd_writeData(0x00);

	epd_writeCommand(0x22);
	epd_writeData(0x91);
	epd_writeCommand(0x20);
	epd_waitUntilIdle();
}

void epd_set_cursor(void) {
	epd_writeCommand(0x4E);
	epd_writeData(0x00);
	epd_writeCommand(0x4F);
	epd_writeData(0xF9);
	epd_writeData(0x00);
}

/* pack fb (byte per pixel) into controller layout: 250 lines x 16 bytes */
void fb_pack(void) {
	int i = 0;
	for (int x = 0; x < EPD_W; x++) {
		for (int y_group = 0; y_group < 16; y_group++) {
			uint8_t data = 0;
			for (int bit = 0; bit < 8; bit++) {
				int y = y_group * 8 + bit;
				uint8_t pixel = 1;
				if (y < EPD_H)
					pixel = fb[y * EPD_W + x] >= 128 ? 1 : 0;
				data |= (pixel << (7 - bit));
			}
			packed[i++] = data;
		}
	}
}

void epd_display_full(void) {
	fb_pack();
	epd_set_cursor();
	epd_writeCommand(0x24);
	epd_writeDataBuf(packed, sizeof(packed));
	epd_set_cursor();
	epd_writeCommand(0x26);
	epd_writeDataBuf(packed, sizeof(packed));

	epd_writeCommand(0x22);
	epd_writeData(0xC7);
	epd_writeCommand(0x20);
	epd_waitUntilIdle();

	/* border must float for clean partials afterwards */
	epd_writeCommand(0x3C);
	epd_writeData(0x80);

	frames = 0; /* full refresh cleans all accumulated ghosting */
}

void epd_display_partial(void) {
	fb_pack();
	epd_set_cursor();
	epd_writeCommand(0x24);
	epd_writeDataBuf(packed, sizeof(packed));

	epd_writeCommand(0x22);
	epd_writeData(0xFF);
	epd_writeCommand(0x20);
	epd_waitUntilIdle();

	/* sync old-data RAM so the next partial diffs against what is
	 * actually on glass, otherwise reverted pixels ghost */
	epd_set_cursor();
	epd_writeCommand(0x26);
	epd_writeDataBuf(packed, sizeof(packed));

	frames++;
}

void fb_disc(int cx, int cy, int r, uint8_t v) {
	for (int y = cy - r; y <= cy + r; y++) {
		if (y < 0 || y >= EPD_H)
			continue;
		for (int x = cx - r; x <= cx + r; x++) {
			if (x < 0 || x >= EPD_W)
				continue;
			int dx = x - cx, dy = y - cy;
			if (dx * dx + dy * dy <= r * r)
				fb[y * EPD_W + x] = v;
		}
	}
}

#define LID_EDGE 3 /* eyelid contour line thickness */

/*
 * gx/gy: pupil offset from center (gx along the 250 axis).
 * aperture: eyelid opening half-height in px; R_FIELD = fully open,
 * 0 = closed.
 *
 * The lids are black with a white contour line along the lens-shaped
 * opening (ellipse: horiz semi-axis R_FIELD, vert semi-axis aperture),
 * so the pupil is clipped behind the lid line instead of merging with
 * the black lid.
 */
void render_eye(int gx, int gy, int aperture) {
	/* black outside the opening so the bezel edge disappears */
	memset(fb, 0x00, sizeof(fb));

	fb_disc(EYE_CX, EYE_CY, R_FIELD, 0xFF);
	fb_disc(EYE_CX + gx, EYE_CY + gy, R_PUPIL, 0x00);
	fb_disc(EYE_CX + gx - 11, EYE_CY + gy - 11, R_GLINT, 0xFF);

	if (aperture >= R_FIELD)
		return;
	for (int x = EYE_CX - R_FIELD; x <= EYE_CX + R_FIELD; x++) {
		int dx = x - EYE_CX;
		float k = 1.0f - (float)(dx * dx) / (float)(R_FIELD * R_FIELD);
		int yf, ye;

		if (x < 0 || x >= EPD_W || k <= 0.0f)
			continue;
		yf = (int)(R_FIELD * sqrtf(k)); /* eyeball extent this column */
		ye = aperture > 0 ? (int)(aperture * sqrtf(k)) : -1; /* lid edge */

		for (int y = EYE_CY - yf; y <= EYE_CY + yf; y++) {
			int ady = abs(y - EYE_CY);

			if (y < 0 || y >= EPD_H || ady <= ye)
				continue; /* open eye area, keep pupil */
			fb[y * EPD_W + x] =
			    ady <= ye + LID_EDGE ? 0xFF : 0x00;
		}
	}
}

static int gx, gy; /* current gaze */

#define GAZE_STEP 14 /* px per frame: constant speed, no sudden hops */

/* glide to a new gaze point at constant speed */
void move_gaze(int tx, int ty, int aperture) {
	while (gx != tx || gy != ty) {
		int dx = tx - gx, dy = ty - gy;
		float d = sqrtf((float)(dx * dx + dy * dy));
		if (d <= GAZE_STEP) {
			gx = tx;
			gy = ty;
		} else {
			gx += (int)roundf(dx * GAZE_STEP / d);
			gy += (int)roundf(dy * GAZE_STEP / d);
		}
		render_eye(gx, gy, aperture);
		epd_display_partial();
	}
}

/* half blink only: never fully cover the pupil, and let the periodic
 * full refresh handle whatever ghost the lid flip leaves behind */
void blink(void) {
	render_eye(gx, gy, 26);
	epd_display_partial();
	render_eye(gx, gy, R_FIELD);
	epd_display_partial();
}

void pick_gaze(int *tx, int *ty) {
	do {
		*tx = rand() % (2 * GAZE_MAX_X + 1) - GAZE_MAX_X;
		*ty = rand() % (2 * GAZE_MAX_Y + 1) - GAZE_MAX_Y;
	} while (*tx * *tx + *ty * *ty > GAZE_MAX_X * GAZE_MAX_X);
}

int main(void) {
	spi_fd = open(EPD_SPI_DEVICE, O_RDWR);
	if (spi_fd < 0) {
		perror("Error opening SPI device");
		exit(1);
	}

	uint8_t spi_mode = SPI_MODE_0;
	ioctl(spi_fd, SPI_IOC_WR_MODE, &spi_mode);
	uint32_t speed = SPI_SPEED;
	ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

	chip = gpiod_chip_open_by_name(EPD_GPIO_CHIP);
	if (!chip) {
		perror("Error opening GPIO chip");
		exit(1);
	}

	epd_dc_line = gpiod_chip_get_line(chip, EPD_DC_PIN);
	epd_rst_line = gpiod_chip_get_line(chip, EPD_RST_PIN);
	epd_busy_line = gpiod_chip_get_line(chip, EPD_BUSY_PIN);
	if (!epd_dc_line || !epd_rst_line || !epd_busy_line) {
		perror("Error getting GPIO lines");
		gpiod_chip_close(chip);
		exit(1);
	}
	gpiod_line_request_output(epd_dc_line, "epd_dc", 0);
	gpiod_line_request_output(epd_rst_line, "epd_rst", 0);
	gpiod_line_request_input(epd_busy_line, "epd_busy");

	srand(time(NULL));
	epd_init();

	render_eye(0, 0, R_FIELD);
	epd_display_full();
	printf("base frame done, entering animation loop\n");

	while (1) {
		if (frames >= FULL_REFRESH_EVERY) {
			render_eye(gx, gy, R_FIELD);
			epd_display_full();
		}

		if (rand() % 4 == 0) {
			blink();
		} else {
			int tx, ty;
			pick_gaze(&tx, &ty);
			move_gaze(tx, ty, R_FIELD);
			sleep_ms(300 + rand() % 900);
		}
	}

	close(spi_fd);
	gpiod_chip_close(chip);
	return 0;
}
